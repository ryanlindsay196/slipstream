// Slipstream - a from-scratch 3D kart racer: software rasterizer, fixed-point
// deterministic sim, rollback netcode, rebindable controls.
//
//   slipstream                          play (menus: single race, host, join, controls)
//   slipstream --race                   jump straight into a single race
//   slipstream --gallery                model viewer: every procedurally generated model
//   slipstream --host [--port 7777]     host an online race
//   slipstream --join 1.2.3.4:7777      join one
//   slipstream --name ALEX              player name shown to others
//   slipstream --lag 60 --jitter 20 --loss 5   simulate a bad network (ms, ms, %)
//   slipstream --autopilot              let the AI drive your kart
//   slipstream --host --autostart 2     host starts as soon as 2 players are in
//   slipstream --overlay                start with the F1 debug overlay on
//
// Headless tools (no window):
//   slipstream --selftest               math, determinism and rollback unit checks
//   slipstream --nettest [ticks]        two peers over real UDP loopback, verify convergence
//   slipstream --botrace [seed]         full 8-AI race: lap times, off-road %, sim cost
//   slipstream --bench [frames] [--shot out.bmp] [--screen title|race]

#include "app/app.h"
#include "game/characters.h"
#include "game/tracks.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ss {

[[noreturn]] void AssertFail(const char* expr, const char* msg, const char* file, int line) {
    plat::Log("ASSERT FAILED: %s\n  %s\n  at %s:%d\n", msg, expr, file, line);
    abort();
}

namespace {

constexpr size_t kMB               = 1024 * 1024;
constexpr size_t kPersistentBudget = 32 * kMB;
constexpr size_t kLevelBudget      = 16 * kMB;  // one track's scene; reset on track change
constexpr size_t kScratchBudget    = 8 * kMB;

struct Args {
    AppOptions  app{.showcaseDegrees = -1.0f};
    bool        selftest  = false;
    bool        nettest   = false;
    bool        bench     = false;
    bool        botrace   = false;
    u32         count     = 0;
    const char* shot      = nullptr;
    const char* screen    = "race";
    i32         desyncAt  = -1;
    i32         trackId   = 0;
};

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const char* s    = argv[i];
        auto        next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        auto        optNum = [&](u32* out) {
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') *out = (u32)strtoul(argv[++i], nullptr, 10);
        };
        if (!strcmp(s, "--race")) a.app.autoRace = true;
        else if (!strcmp(s, "--gallery")) a.app.gallery = true;
        else if (!strcmp(s, "--autopilot")) a.app.autopilot = true;
        else if (!strcmp(s, "--host")) a.app.autoHost = true;
        else if (!strcmp(s, "--overlay")) a.app.overlay = true;
        else if (!strcmp(s, "--autostart")) { const char* v = next(); if (!v) return false; a.app.autoStartPlayers = atoi(v); }
        else if (!strcmp(s, "--join")) { if (!(a.app.joinAddress = next())) return false; }
        else if (!strcmp(s, "--port")) { const char* v = next(); if (!v) return false; a.app.port = (u16)atoi(v); }
        else if (!strcmp(s, "--name")) {
            const char* v = next();
            if (!v) return false;
            snprintf(a.app.name, sizeof(a.app.name), "%s", v);
            for (char* c = a.app.name; *c; ++c)
                if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        }
        else if (!strcmp(s, "--lag")) { const char* v = next(); if (!v) return false; a.app.conditions.latencyMs = (u32)atoi(v); }
        else if (!strcmp(s, "--jitter")) { const char* v = next(); if (!v) return false; a.app.conditions.jitterMs = (u32)atoi(v); }
        else if (!strcmp(s, "--loss")) { const char* v = next(); if (!v) return false; a.app.conditions.lossPercent = (u32)atoi(v); }
        else if (!strcmp(s, "--selftest")) a.selftest = true;
        else if (!strcmp(s, "--nettest")) { a.nettest = true; optNum(&a.count); }
        else if (!strcmp(s, "--bench")) { a.bench = true; optNum(&a.count); }
        else if (!strcmp(s, "--botrace")) { a.botrace = true; optNum(&a.count); }
        else if (!strcmp(s, "--shot")) { if (!(a.shot = next())) return false; }
        else if (!strcmp(s, "--screen")) { if (!(a.screen = next())) return false; }
        else if (!strcmp(s, "--showcase")) { const char* v = next(); if (!v) return false; a.app.showcaseDegrees = (float)atof(v); }
        else if (!strcmp(s, "--desync-at")) { const char* v = next(); if (!v) return false; a.desyncAt = atoi(v); }
        else if (!strcmp(s, "--track")) {
            const char* v = next();
            if (!v) return false;
            a.trackId = a.app.trackId = Clamp(atoi(v), 0, kTrackCount - 1);
        }
        else if (!strcmp(s, "--character")) {
            const char* v = next();
            if (!v) return false;
            a.app.character = Clamp(atoi(v), 0, Char_Count - 1);
        }
        else { plat::Log("unknown argument '%s'\n", s); return false; }
    }
    return true;
}

bool SaveBmp(const char* path, const u32* px, int w, int h, int scale, Arena& scratch) {
    ScratchScope scope(scratch);
    int    ow = w * scale, oh = h * scale;
    size_t row = ((size_t)ow * 3 + 3) & ~(size_t)3, size = 54 + row * (size_t)oh;
    u8*    out = (u8*)scratch.Alloc(size, 4);
    memset(out, 0, size);
    auto put32 = [&](size_t at, u32 v) { for (int i = 0; i < 4; ++i) out[at + i] = (u8)(v >> (8 * i)); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, (u32)size);
    put32(10, 54);
    put32(14, 40);
    put32(18, (u32)ow);
    put32(22, (u32)oh);
    out[26] = 1;
    out[28] = 24;
    put32(34, (u32)(row * (size_t)oh));
    for (int y = 0; y < oh; ++y) {
        u8* dst = out + 54 + row * (size_t)(oh - 1 - y);
        for (int x = 0; x < ow; ++x) {
            u32 c          = px[(y / scale) * w + x / scale];
            dst[x * 3 + 0] = (u8)c;
            dst[x * 3 + 1] = (u8)(c >> 8);
            dst[x * 3 + 2] = (u8)(c >> 16);
        }
    }
    return plat::SaveFile(path, out, size);
}

// ---------------------------------------------------------------------------
// --selftest
// ---------------------------------------------------------------------------
int RunSelfTest(Memory& mem) {
    int  failures = 0;
    auto check    = [&](bool ok, const char* what) {
        plat::Log("  [%s] %s\n", ok ? " OK " : "FAIL", what);
        if (!ok) ++failures;
    };
    plat::Log("\nSlipstream self-test\n");

    bool sqrtOk = true;
    for (u64 s = 1; s < (1u << 16); ++s) sqrtOk &= ISqrt64(s * s) == s && ISqrt64(s * s - 1) == s - 1;
    check(sqrtOk, "fast ISqrt64 exact at perfect-square boundaries");

    check(Sin(0) == Fx{0} && Sin(kAngleQuarter) == Fx::FromInt(1) && Sin(kAngleQuarter * 2).raw == 0 &&
              Sin(kAngleQuarter * 3) == Fx::FromInt(-1),
          "sine table hits exact values at the quadrants");
    i32 worstAngleErr = 0;
    for (Angle a = 0; a < kAngleTurn; a += 97) {
        Angle back    = AngleOf(Forward(a));
        worstAngleErr = Max(worstAngleErr, (i32)std::abs(AngleDelta(a, back)));
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "CORDIC AngleOf inverts Forward() (worst error %d / 65536 units)", worstAngleErr);
    check(worstAngleErr <= 24, buf);

    Track* track = mem.persistent.Push<Track>();
    BuildTrack(*track, 0);
    bool trackOk = track->count > 100;
    for (int i = 0; i < track->count; ++i) trackOk &= track->s[i].length > Fx::Ratio(1, 2);
    snprintf(buf, sizeof(buf), "track bakes: %d samples, %.0f m per lap", track->count, track->totalLength.ToFloat());
    check(trackOk, buf);

    for (int id = 0; id < kTrackCount; ++id) {
        Track*    t = mem.persistent.Push<Track>();
        SimState* s = mem.persistent.Push<SimState>();
        BuildTrack(*t, id);
        SimInit(*s, *t, 42, 0, 3);
        i32 offroad = 0;
        while (s->phase != Phase_Finished && s->tick < 6 * 60 * kTickRate) {
            SimTick(*s, *t, nullptr);
            for (int i = 0; i < s->numKarts; ++i) offroad += FxAbs(s->karts[i].lateral) > t->roadHalf;
        }
        double secs = (s->karts[0].finishTick ? s->karts[0].finishTick : s->tick) / (double)kTickRate;
        snprintf(buf, sizeof(buf), "%s: %.2f km, all 8 bots finish 3 laps (%.0fs, %.1f%% off-road)", kTracks[id].name,
                 t->totalLength.ToFloat() / 1000.0f, secs, 100.0 * offroad / (s->tick * 8.0));
        check(s->phase == Phase_Finished && s->finishedCount == 8, buf);
    }

    // Determinism and snapshot/restore.
    auto run = [&](SimState& s, u32 seed, int fromTick, int toTick) {
        for (int t = fromTick; t < toTick; ++t) {
            PlayerInput in[kMaxPlayers] = {BotInput(s, *track, 0), BotInput(s, *track, 1)};
            SimTick(s, *track, in);
        }
        (void)seed;
    };
    SimState* a = mem.persistent.Push<SimState>();
    SimState* b = mem.persistent.Push<SimState>();
    SimInit(*a, *track, 99, 2, 3);
    SimInit(*b, *track, 99, 2, 3);
    run(*a, 99, 0, 1800);
    run(*b, 99, 0, 1800);
    check(SimHash(*a) == SimHash(*b), "same seed + inputs => identical state after 1800 ticks");
    SimInit(*a, *track, 99, 2, 3);
    run(*a, 99, 0, 900);
    *b = *a;  // snapshot
    run(*a, 99, 900, 1800);
    run(*b, 99, 900, 1800);
    check(SimHash(*a) == SimHash(*b), "memcpy snapshot resumes bit-identically");
    bool racing = a->phase == Phase_Racing && a->karts[0].lap >= 1;
    int  boxesUsed = 0;
    for (int i = 0; i < a->numBoxes; ++i) boxesUsed += a->boxes[i].respawnTicks > 0;
    snprintf(buf, sizeof(buf), "race progresses (lap %d, place %d, kart 7 lap %d)", a->karts[0].lap, a->karts[0].place,
             a->karts[7].lap);
    check(racing, buf);

    // Slipstream: put kart 0 on the start straight ~8 m behind kart 1, both at
    // speed and driving straight. Kart 0 should get a draft boost; kart 1,
    // with nobody ahead, should not.
    {
        SimState*   s      = mem.persistent.Push<SimState>();
        PlayerInput in[2]  = {{0, In_Accel}, {0, In_Accel}};
        SimInit(*s, *track, 5, 2, 3);
        while (s->phase == Phase_Countdown) SimTick(*s, *track, in);
        for (int i = 0; i < 2; ++i) {
            const TrackSample& smp = track->s[4 + i * 2];  // samples are ~4 m apart here: ~8 m gap
            Kart&              k   = s->karts[i];
            k.pos                  = smp.pos;
            k.heading              = AngleOf(smp.tangent);
            k.speed                = Fx::Ratio(40, 100);
            k.vel                  = Forward(k.heading) * k.speed;
            TrackPos tp            = LocateOnTrack(*track, k.pos, 4 + i * 2);
            k.seg                  = tp.seg;
            k.lateral              = tp.lateral;
        }
        bool drafted[2] = {false, false};
        for (int t = 0; t < 120; ++t) {
            SimTick(*s, *track, in);
            for (int i = 0; i < 2; ++i) drafted[i] |= s->karts[i].draftFlash > 0;
        }
        snprintf(buf, sizeof(buf), "slipstream: follower boosted %d, leader boosted %d (gap %.1fm, draft %d)", drafted[0],
                 drafted[1], Length(s->karts[1].pos - s->karts[0].pos).ToFloat(), s->karts[0].draftTicks);
        check(drafted[0] && !drafted[1], buf);
    }

    // Mini-turbo tiers: hold a drift with each charge level, release, and check
    // the boost length (blue < orange < purple).
    {
        SimState*   s = mem.persistent.Push<SimState>();
        i32         boost[3];
        const i32   charges[3] = {kMiniTurbo, kSuperTurbo, kUltraTurbo};
        for (int tier = 0; tier < 3; ++tier) {
            PlayerInput in[1] = {{0, In_Accel}};
            SimInit(*s, *track, 9, 1, 3);
            while (s->phase == Phase_Countdown) SimTick(*s, *track, in);
            Kart& k       = s->karts[0];
            k.speed       = Fx::Ratio(40, 100);
            k.driftDir    = 1;
            k.driftCharge = charges[tier];
            k.prevButtons = In_Accel | In_Drift;
            SimTick(*s, *track, in);  // drift button released this tick
            boost[tier] = k.boostTicks;
        }
        snprintf(buf, sizeof(buf), "mini-turbo tiers: blue %d, orange %d, purple %d boost ticks", boost[0], boost[1], boost[2]);
        check(boost[0] > 0 && boost[0] < boost[1] && boost[1] < boost[2], buf);
    }

    // Spin-outs must leave karts facing the way they were going.
    {
        SimState* s = mem.persistent.Push<SimState>();
        SimInit(*s, *track, 42, 0, 3);
        Angle startHeading[kMaxKarts] = {};
        int   spins = 0, turned = 0;
        while (s->tick < 6000) {
            i32 before[kMaxKarts];
            for (int i = 0; i < s->numKarts; ++i) before[i] = s->karts[i].spinTicks;
            Angle headingBefore[kMaxKarts];
            for (int i = 0; i < s->numKarts; ++i) headingBefore[i] = s->karts[i].heading;
            SimTick(*s, *track, nullptr);
            for (int i = 0; i < s->numKarts; ++i) {
                const Kart& k = s->karts[i];
                if (before[i] == 0 && k.spinTicks > 0) startHeading[i] = headingBefore[i];
                if (before[i] > 0 && k.spinTicks == 0) {
                    ++spins;
                    turned += std::abs(AngleDelta(startHeading[i], k.heading)) > 182;  // > 1 degree
                }
            }
        }
        snprintf(buf, sizeof(buf), "spin-outs recover facing forward (%d spins, %d changed heading)", spins, turned);
        check(spins > 0 && turned == 0, buf);
    }

    // Rollback vs. ground truth: two peers whose inputs reach each other 6
    // ticks late (forcing constant misprediction) must end bit-identical to a
    // plain simulation fed the final inputs.
    constexpr int kTicks = 1500, kLag = 6;
    Rollback*     rb[2];
    for (int p = 0; p < 2; ++p) {
        rb[p]                  = mem.persistent.Push<Rollback>();
        rb[p]->history         = mem.persistent.Push<PlayerInput>((size_t)(kTicks + 64) * kMaxPlayers);
        rb[p]->historyCapacity = kTicks + 64;
        rb[p]->Init(track, 1234, 2, p, 3, kDefaultInputDelay);
    }
    i32 delivered[2] = {kDefaultInputDelay - 1, kDefaultInputDelay - 1};
    for (int frame = 0; frame < kTicks * 3 && (rb[0]->CurrentTick() < kTicks || rb[1]->CurrentTick() < kTicks); ++frame) {
        for (int p = 0; p < 2; ++p) {
            Rollback& r = *rb[p];
            if (r.CurrentTick() < kTicks && r.CanAdvance()) {
                r.AddLocalInput(BotInput(r.state, *track, p));
                r.Advance();
            }
        }
        for (int p = 0; p < 2; ++p) {  // deliver p's inputs to the other peer, kLag ticks late
            Rollback& from = *rb[p];
            Rollback& to   = *rb[1 - p];
            while (delivered[p] < from.confirmed[p] && delivered[p] + 1 <= from.CurrentTick() - kLag) {
                ++delivered[p];
                to.AddRemoteInput(p, delivered[p], from.inputs[delivered[p] % kInputRing][p]);
            }
            if (from.CurrentTick() >= kTicks)  // flush at the end
                while (delivered[p] < from.confirmed[p]) {
                    ++delivered[p];
                    to.AddRemoteInput(p, delivered[p], from.inputs[delivered[p] % kInputRing][p]);
                }
        }
    }
    for (int p = 0; p < 2; ++p) rb[p]->Settle();  // apply any final correction
    u64 h0 = 0, h1 = 0;
    bool have = rb[0]->ConfirmedHash(kTicks, &h0) && rb[1]->ConfirmedHash(kTicks, &h1);
    SimState* ref = mem.persistent.Push<SimState>();
    SimInit(*ref, *track, 1234, 2, 3);
    for (int t = 0; t < kTicks; ++t) SimTick(*ref, *track, &rb[0]->history[t * kMaxPlayers]);
    snprintf(buf, sizeof(buf), "rollback peers converge to ground truth (%u + %u rollbacks, max depth %u)",
             rb[0]->stats.rollbacks, rb[1]->stats.rollbacks, Max(rb[0]->stats.maxDepth, rb[1]->stats.maxDepth));
    check(have && h0 == h1 && h0 == SimHash(*ref) && rb[0]->stats.rollbacks > 0, buf);

    // Bindings survive a save/load round trip.
    Bindings orig, loaded;
    DefaultBindings(orig);
    AssignBinding(orig, Act_Drift, Slot_Keyboard, {Bind_Key, plat::Key_LShift});
    AssignBinding(orig, Act_UseItem, Slot_Pad, {Bind_PadAxisPos, plat::Axis_RT});
    bool saved = SaveBindings(orig, "slipstream_selftest.cfg", mem.scratch);
    DefaultBindings(loaded);
    bool parsed = LoadBindings(loaded, "slipstream_selftest.cfg", mem.scratch);
    bool same = true;  // field-wise: Binding has a padding byte, so no memcmp
    for (int act = 0; act < Act_Count; ++act)
        for (int slot = 0; slot < Slot_Count; ++slot)
            same &= orig.slots[act][slot].kind == loaded.slots[act][slot].kind &&
                    orig.slots[act][slot].code == loaded.slots[act][slot].code;
    check(saved && parsed && same, "control bindings round-trip through the config file");
    remove("slipstream_selftest.cfg");

    plat::Log("%s\n", failures ? "SELF-TEST FAILED" : "all checks passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// --nettest: a host and a client in one process, talking over real UDP
// sockets on loopback, optionally through the lag/jitter/loss simulator.
// Virtual time keeps it fast and repeatable.
// ---------------------------------------------------------------------------
int RunNetTest(Memory& mem, u32 ticks, NetConditions cond, i32 desyncAt, int trackId) {
    if (!ticks) ticks = 3600;
    Track* track = mem.persistent.Push<Track>();
    BuildTrack(*track, trackId);

    NetSession* peer[2] = {mem.persistent.Push<NetSession>(), mem.persistent.Push<NetSession>()};
    Rollback*   rb[2]   = {mem.persistent.Push<Rollback>(), mem.persistent.Push<Rollback>()};
    for (int p = 0; p < 2; ++p) {
        peer[p]->conditions    = cond;
        peer[p]->loopbackOnly  = true;  // never touches the LAN, so no firewall prompt
        rb[p]->historyCapacity = (i32)ticks + 64;
        rb[p]->history         = mem.persistent.Push<PlayerInput>(((size_t)ticks + 64) * kMaxPlayers);
    }
    const u16 port = 47123;
    if (!NetHost(*peer[0], port, "HOST", Char_Doggo, trackId) || !NetJoin(*peer[1], {0x7F000001, port}, "CLIENT", Char_Banana)) {
        plat::Log("could not open sockets: %s\n", peer[0]->error ? peer[0]->error : peer[1]->error);
        return 2;
    }

    u32  frame = 0;
    auto now   = [&]() { return 1000u + frame * 1000u / kTickRate; };
    auto pump  = [&]() {
        for (int p = 0; p < 2; ++p) NetPoll(*peer[p], now());
    };
    auto send = [&]() {
        for (int p = 0; p < 2; ++p) NetSendFrame(*peer[p], now());
    };

    // Lobby -> start handshake.
    bool started[2] = {false, false};
    for (; frame < 2000 && !(started[0] && started[1]); ++frame) {
        pump();
        if (peer[0]->phase == NetPhase::Lobby && NetConnectedCount(*peer[0]) == 2 && peer[1]->phase == NetPhase::Lobby)
            NetHostBeginRace(*peer[0], 0xC0FFEEu, kDefaultLaps, now());
        for (int p = 0; p < 2; ++p) {
            if (started[p] || peer[p]->phase != NetPhase::InGame) continue;
            rb[p]->Init(track, peer[p]->seed, peer[p]->numPlayers, peer[p]->localSlot, peer[p]->laps, kDefaultInputDelay,
                        peer[p]->playerCharacters);
            NetAttachRollback(*peer[p], rb[p]);
            started[p] = true;
        }
        send();
    }
    if (!(started[0] && started[1])) {
        plat::Log("handshake failed (host %d, client %d)\n", (int)peer[0]->phase, (int)peer[1]->phase);
        return 1;
    }
    u32 handshakeFrames = frame;
    // The lobby must have carried the track and both players' characters.
    bool setupOk = peer[1]->trackId == trackId && peer[1]->playerCharacters[0] == Char_Doggo &&
                   peer[1]->playerCharacters[1] == Char_Banana && peer[0]->playerCharacters[1] == Char_Banana;
    if (!setupOk) {
        plat::Log("FAIL: race setup did not survive the handshake (track %d, chars %d/%d)\n", peer[1]->trackId,
                  peer[1]->playerCharacters[0], peer[1]->playerCharacters[1]);
        return 1;
    }
    for (int p = 0; p < 2; ++p)
        if (rb[p]->state.karts[0].character != Char_Doggo || rb[p]->state.karts[1].character != Char_Banana) {
            plat::Log("FAIL: peer %d seated the wrong characters\n", p);
            return 1;
        }

    u64  t0 = plat::Now();
    bool injected = false;
    for (u32 guard = 0; guard < ticks * 6; ++guard, ++frame) {
        pump();
        for (int p = 0; p < 2; ++p) {
            Rollback& r = *rb[p];
            if (peer[p]->phase == NetPhase::Disconnected) {
                plat::Log("peer %d disconnected: %s\n", p, peer[p]->error);
                return 1;
            }
            if (r.CurrentTick() >= (i32)ticks) {
                r.Settle();  // finished, but late corrections must still apply
                continue;
            }
            if (!r.CanAdvance()) { ++r.stats.stalls; continue; }
            if (NetFrameAdvantage(*peer[p], now()) > 1.5f && (frame & 1)) { ++r.stats.stalls; continue; }
            r.AddLocalInput(BotInput(r.state, *track, r.localPlayer));
            r.Advance();
            if (p == 1 && desyncAt >= 0 && !injected && r.CurrentTick() >= desyncAt) {
                // Simulated memory corruption: flip bit 16 (1 metre) of one
                // coordinate in the live state *and* every snapshot a rollback
                // could restore; otherwise the next rollback would erase it.
                // (A 1-LSB flip is often absorbed by fixed-point rounding at
                // the next wall contact and genuinely heals itself.)
                r.state.karts[5].pos.x.raw ^= 1 << 16;
                for (SimState& snap : r.snapshots)
                    if (snap.tick > r.MinConfirmed()) snap.karts[5].pos.x.raw ^= 1 << 16;
                injected = true;
            }
        }
        send();
        bool done = true;
        for (int p = 0; p < 2; ++p) done &= rb[p]->CurrentTick() >= (i32)ticks && rb[p]->MinConfirmed() >= (i32)ticks - 1;
        if (done && (desyncAt < 0 || peer[0]->desynced || peer[1]->desynced || guard > ticks * 2)) break;
    }
    for (int p = 0; p < 2; ++p) rb[p]->Settle();
    double wallMs = plat::TicksToMs(plat::Now() - t0);

    u64 h[2] = {0, 0};
    bool have = rb[0]->ConfirmedHash((i32)ticks, &h[0]) && rb[1]->ConfirmedHash((i32)ticks, &h[1]);
    SimState* ref = mem.persistent.Push<SimState>();
    const i32 chars[2] = {Char_Doggo, Char_Banana};  // what the two peers picked
    SimInit(*ref, *track, 0xC0FFEEu, 2, kDefaultLaps, chars);
    for (u32 t = 0; t < ticks; ++t) SimTick(*ref, *track, &rb[0]->history[t * kMaxPlayers]);
    u64 truth = SimHash(*ref);

    plat::Log("\nSlipstream net test | %u ticks (%.0fs race) | lag %ums  jitter %ums  loss %u%% | %.0fms wall\n", ticks,
              ticks / (double)kTickRate, cond.latencyMs, cond.jitterMs, cond.lossPercent, wallMs);
    plat::Log("handshake took %u frames | track %s | host DOGGO vs client BANANA (+6 bots)\n\n", handshakeFrames,
              kTracks[trackId].name);
    plat::Log("  %-8s %9s %9s %9s %9s %9s %8s %8s\n", "peer", "rollback", "resim", "maxdepth", "mispred", "stalls",
              "rtt ms", "checksum");
    for (int p = 0; p < 2; ++p) {
        const RollbackStats& s = rb[p]->stats;
        plat::Log("  %-8s %9u %9u %9u %9u %9u %8u %8d\n", p ? "client" : "host", s.rollbacks, s.resimulatedTicks,
                  s.maxDepth, s.mispredictions, s.stalls, NetWorstRtt(*peer[p]), peer[p]->checksumsMatched);
    }
    plat::Log("\n  packets sent %u / %u, simulated drops %u / %u, bytes %u / %u\n", peer[0]->packetsSent,
              peer[1]->packetsSent, peer[0]->packetsDropped, peer[1]->packetsDropped, peer[0]->bytesSent,
              peer[1]->bytesSent);
    plat::Log("  host   state hash at tick %u: %016llX\n", ticks, (unsigned long long)h[0]);
    plat::Log("  client state hash at tick %u: %016llX\n", ticks, (unsigned long long)h[1]);
    plat::Log("  offline ground truth        : %016llX\n\n", (unsigned long long)truth);

    NetClose(*peer[0]);
    NetClose(*peer[1]);

    if (desyncAt >= 0) {
        bool detected = peer[0]->desynced || peer[1]->desynced;
        i32  at       = peer[0]->desynced ? peer[0]->desyncTick : peer[1]->desyncTick;
        if (detected) plat::Log("DESYNC DETECTED as expected: injected at tick %d, flagged at checksum tick %d\n", desyncAt, at);
        else plat::Log("FAIL: injected a desync at tick %d but nobody noticed\n", desyncAt);
        return detected ? 0 : 1;
    }
    bool ok = have && h[0] == h[1] && h[0] == truth && !peer[0]->desynced && !peer[1]->desynced;
    plat::Log(ok ? "CONVERGED: both peers match each other and the offline ground truth\n"
                 : "FAIL: peers diverged\n");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --botrace: a full 8-AI race, for tuning handling/AI and timing the sim.
// ---------------------------------------------------------------------------
int RunBotRace(Memory& mem, u32 seed, int trackId) {
    Track* track = mem.persistent.Push<Track>();
    BuildTrack(*track, trackId);
    SimState* s = mem.persistent.Push<SimState>();
    SimInit(*s, *track, seed ? seed : 42, 0, kDefaultLaps);
    u64 t0 = plat::Now(), worst = 0;
    i32 offroadTicks = 0, spins = 0, itemsUsed = 0, drafts = 0, turbos[3] = {0, 0, 0};
    while (s->phase != Phase_Finished && s->tick < 8 * 60 * kTickRate) {
        i32 heldBefore = 0, driftBefore[kMaxKarts], chargeBefore[kMaxKarts];
        for (int i = 0; i < s->numKarts; ++i) {
            heldBefore += s->karts[i].item != Item_None;
            driftBefore[i]  = s->karts[i].driftDir;
            chargeBefore[i] = s->karts[i].driftCharge;
        }
        u64 a = plat::Now();
        SimTick(*s, *track, nullptr);
        worst = Max(worst, plat::Now() - a);
        for (int i = 0; i < s->numKarts; ++i) {
            const Kart& k = s->karts[i];
            offroadTicks += FxAbs(k.lateral) > track->roadHalf;
            spins += k.spinTicks == kSpinTicks - 1;
            drafts += k.draftFlash == kDraftFlashTicks;
            if (driftBefore[i] && !k.driftDir && k.spinTicks == 0) {  // drift released: which tier?
                int ch = chargeBefore[i];
                if (ch >= kUltraTurbo) ++turbos[2];
                else if (ch >= kSuperTurbo) ++turbos[1];
                else if (ch >= kMiniTurbo) ++turbos[0];
            }
        }
        i32 heldAfter = 0;
        for (int i = 0; i < s->numKarts; ++i) heldAfter += s->karts[i].item != Item_None;
        itemsUsed += Max(0, heldBefore - heldAfter);
    }
    double ms = plat::TicksToMs(plat::Now() - t0);
    plat::Log("\nSlipstream bot race | seed %u | %d ticks simulated in %.1fms (worst tick %.3fms)\n", seed ? seed : 42,
              s->tick, ms, plat::TicksToMs(worst));
    plat::Log("  off-road %.1f%% of kart-ticks, %d spin-outs, ~%d items used\n",
              100.0 * offroadTicks / (s->tick * (double)s->numKarts), spins, itemsUsed);
    plat::Log("  %d slipstream boosts | mini-turbos: %d blue, %d orange, %d purple\n", drafts, turbos[0], turbos[1],
              turbos[2]);
    plat::Log("  final state hash %016llX\n\n", (unsigned long long)SimHash(*s));
    for (int p = 1; p <= s->numKarts; ++p)
        for (int i = 0; i < s->numKarts; ++i) {
            const Kart& k = s->karts[i];
            if (k.place != p) continue;
            if (k.finishTick) {
                double secs = (k.finishTick - kCountdownTicks) / (double)kTickRate;
                plat::Log("  %d. %-12s %d:%05.2f\n", p, kCharacters[k.character].name, (int)secs / 60, secs - 60 * ((int)secs / 60));
            } else {
                plat::Log("  %d. %-12s DNF (lap %d, segment %d)\n", p, kCharacters[k.character].name, k.lap, k.seg);
            }
        }
    return s->phase == Phase_Finished ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --bench: run the real app headless (AI drives) and time every frame.
// ---------------------------------------------------------------------------
int RunBench(Memory& mem, Args& args) {
    u32 frames = args.count ? args.count : 1200;
    args.app.headless  = true;
    args.app.autopilot = true;
    args.app.gallery   = strcmp(args.screen, "gallery") == 0;
    bool menuScreen    = !strcmp(args.screen, "title") || !strcmp(args.screen, "characters") || !strcmp(args.screen, "tracks");
    args.app.autoRace  = !menuScreen && !args.app.gallery;

    App* app = mem.persistent.Push<App>();
    AppInit(*app, mem, args.app);
    if (!strcmp(args.screen, "characters")) app->screen = Screen::CharacterSelect;
    if (!strcmp(args.screen, "tracks")) app->screen = Screen::TrackSelect;
    heapguard::Lock();
    float* samples = mem.persistent.Push<float>(frames);
    float* render  = mem.persistent.Push<float>(frames);
    for (u32 f = 0; f < frames; ++f) {
        u64 t0 = plat::Now();
        AppFrame(*app);
        samples[f] = (float)plat::TicksToMs(plat::Now() - t0);
        render[f]  = app->timings.renderMs;
    }
    heapguard::Unlock();

    double sum = 0, worst = 0;
    for (u32 f = 0; f < frames; ++f) {
        sum += samples[f];
        worst = Max(worst, (double)samples[f]);
    }
    std::sort(samples, samples + frames);
    plat::Log("\nSlipstream bench | %u frames at %dx%d, software rasterized\n", frames, kFbWidth, kFbHeight);
    plat::Log("  frame avg %.2fms  p50 %.2fms  p99 %.2fms  max %.2fms  (budget 16.67ms)\n", sum / frames,
              samples[frames / 2], samples[frames * 99 / 100], worst);
    plat::Log("  last frame: %u meshes, %u/%u triangles drawn, %u clipped\n", app->rc.stats.meshes,
              app->rc.stats.trianglesDrawn, app->rc.stats.trianglesIn, app->rc.stats.trianglesClipped);
    plat::Log("  arena PERSIST high water %.1f MB / %.0f MB, SCRATCH %.1f MB / %.0f MB\n",
              mem.persistent.highWater / (double)kMB, mem.persistent.capacity / (double)kMB,
              mem.scratch.highWater / (double)kMB, mem.scratch.capacity / (double)kMB);
    plat::Log("  heap allocations during frames: %llu\n", (unsigned long long)heapguard::AllocsWhileLocked());
    if (app->raceActive) {
        const Kart& k = app->rb->state.karts[0];
        plat::Log("  race: tick %d, lap %d, place %d\n", app->rb->CurrentTick(), k.lap, k.place);
    }
    if (args.shot) {
        if (SaveBmp(args.shot, app->color, kFbWidth, kFbHeight, 2, mem.scratch)) plat::Log("  saved %s\n", args.shot);
        else plat::Log("  failed to write %s\n", args.shot);
    }
    return heapguard::AllocsWhileLocked() ? 3 : 0;
}

}  // namespace
}  // namespace ss

int main(int argc, char** argv) {
    using namespace ss;
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        plat::Log("see the comment at the top of src/main.cpp for usage\n");
        return 2;
    }

    Memory mem{};
    mem.size  = kPersistentBudget + kLevelBudget + kScratchBudget;
    mem.block = plat::ReserveMemory(mem.size);
    SS_ASSERT(mem.block, "could not reserve the memory budget");
    mem.persistent.Init("PERSIST", mem.block, kPersistentBudget);
    mem.level.Init("LEVEL", (u8*)mem.block + kPersistentBudget, kLevelBudget);
    mem.scratch.Init("SCRATCH", (u8*)mem.block + kPersistentBudget + kLevelBudget, kScratchBudget);

    int rc = 0;
    if (args.selftest || args.nettest || args.bench || args.botrace) {
        if (!plat::Init(nullptr)) return 2;
        if (args.selftest) rc = RunSelfTest(mem);
        else if (args.nettest) rc = RunNetTest(mem, args.count, args.app.conditions, args.desyncAt, args.trackId);
        else if (args.botrace) rc = RunBotRace(mem, args.count, args.trackId);
        else rc = RunBench(mem, args);
        plat::Shutdown();
    } else {
        plat::WindowDesc wd{"Slipstream", kFbWidth, kFbHeight, 3};
        if (!plat::Init(&wd)) {
            plat::Log("could not create a window\n");
            return 2;
        }
        App* app = mem.persistent.Push<App>();
        AppInit(*app, mem, args.app);
        heapguard::Lock();
        while (AppFrame(*app)) {}
        heapguard::Unlock();
        AppShutdown(*app);
        plat::Shutdown();
    }
    plat::ReleaseMemory(mem.block, mem.size);
    return rc;
}
