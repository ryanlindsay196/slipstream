#include "game/sim.h"
#include "game/characters.h"

namespace ss {

namespace {

constexpr Fx  kKartRadius   = Fx::Ratio(9, 10);
constexpr Fx  kMaxSpeed     = Fx::Ratio(42, 100);  // m/tick: ~25 m/s
constexpr Fx  kBoostSpeed   = Fx::Ratio(62, 100);
constexpr Fx  kBrake        = Fx::Ratio(15, 1000);
constexpr Fx  kReverseMax   = Fx::Ratio(-12, 100);
constexpr Fx  kCoast        = Fx::Ratio(99, 100);
constexpr Fx  kShellSpeed   = Fx::Ratio(85, 100);
constexpr Fx  kShellRadius  = Fx::Ratio(45, 100);
constexpr i32 kBoxRespawn   = 150;
constexpr i32 kRouletteTicks = 45;
constexpr i32 kLineWindow   = 30;  // samples either side of the start line / checkpoints
constexpr i32 kAllCheckpoints = 0b111;

bool Pressed(const Kart& k, PlayerInput in, u8 button) {
    return (in.buttons & button) && !(k.prevButtons & button);
}

void HitKart(Kart& k) {
    if (k.starTicks > 0) return;
    k.spinTicks   = kSpinTicks;
    k.driftDir    = 0;
    k.driftCharge = 0;
    k.boostTicks  = 0;
    k.speed       = k.speed * Fx::Ratio(35, 100);
}

// Pushes `pos` back inside the barriers. Returns the track position and the
// outward normal speed that was removed (0 if no contact).
struct WallResult {
    TrackPos tp;
    Fx       impact;
    Fx       side;  // +1 / -1 of the wall touched
};
WallResult ConstrainToTrack(const Track& t, V2& pos, V2& vel, i32 hintSeg, Fx radius, Fx restitution) {
    WallResult r{LocateOnTrack(t, pos, hintSeg), Fx{0}, Fx{0}};
    Fx         limit = t.wallHalf - radius;
    if (FxAbs(r.tp.lateral) > limit) {
        const TrackSample& smp  = t.s[r.tp.seg];
        Fx                 side = r.tp.lateral > Fx{0} ? Fx::FromInt(1) : Fx::FromInt(-1);
        pos -= smp.right * ((FxAbs(r.tp.lateral) - limit) * side);
        Fx vn = Dot(vel, smp.right) * side;
        if (vn > Fx{0}) {
            vel -= smp.right * (vn * side * (Fx::FromInt(1) + restitution));
            r.impact = vn;
        }
        r.side       = side;
        r.tp.lateral = limit * side;
    }
    return r;
}

// ---------------------------------------------------------------------------

void UpdateKart(SimState& s, const Track& t, int i, PlayerInput in) {
    Kart& k = s.karts[i];
    if (k.boostTicks > 0) --k.boostTicks;
    if (k.starTicks > 0) --k.starTicks;
    if (k.hopTicks > 0) --k.hopTicks;
    if (k.bumpTicks > 0) --k.bumpTicks;
    if (k.draftFlash > 0) --k.draftFlash;

    bool spinning = k.spinTicks > 0;
    if (spinning) {
        // The spin is purely visual (see KartModel): the heading is untouched,
        // so a hit kart always recovers facing the way it was going.
        --k.spinTicks;
        k.speed   = k.speed * Fx::Ratio(93, 100);
        in        = {};
    }

    // Drift: press while steering to hop into a drift; release to cash in the
    // charge as a mini-turbo.
    if (k.driftDir == 0 && Pressed(k, in, In_Drift)) {
        k.hopTicks = 8;
        if (k.speed > Fx::Ratio(20, 100) && in.steer != 0) {
            k.driftDir    = in.steer > 0 ? 1 : -1;
            k.driftCharge = 0;
        }
    }
    if (k.driftDir != 0 && (!(in.buttons & In_Drift) || k.speed < Fx::Ratio(15, 100))) {
        if (k.driftCharge >= kUltraTurbo) k.boostTicks = Max(k.boostTicks, 130);
        else if (k.driftCharge >= kSuperTurbo) k.boostTicks = Max(k.boostTicks, 85);
        else if (k.driftCharge >= kMiniTurbo) k.boostTicks = Max(k.boostTicks, 45);
        k.driftDir    = 0;
        k.driftCharge = 0;
    }

    // Speed.
    bool offroad = FxAbs(k.lateral) > t.roadHalf;
    const CharacterDef& ch = kCharacters[k.character];
    Fx   cap     = kMaxSpeed * ch.topSpeed;
    if (!k.isHuman) cap = cap * (Fx::Ratio(93, 100) + Fx::Ratio(k.skill, 100));
    if (k.starTicks > 0) cap = cap * Fx::Ratio(112, 100);
    if (offroad && k.boostTicks == 0 && k.starTicks == 0) cap = cap * Fx::Ratio(55, 100);
    if (k.boostTicks > 0) {
        cap     = Max(cap, kBoostSpeed);
        k.speed = Min(k.speed + Fx::Ratio(3, 100), cap);
    }
    if ((in.buttons & In_Accel) && !(in.buttons & In_Brake)) {
        if (k.speed < cap) k.speed = Min(k.speed + ch.accel, cap);
    } else if (in.buttons & In_Brake) {
        k.speed = Max(k.speed - kBrake, kReverseMax);
    } else {
        k.speed = k.speed * kCoast;
    }
    if (k.speed > cap) k.speed = k.speed - (k.speed - cap) * Fx::Ratio(6, 100);

    // Steering.
    if (!spinning) {
        Fx  steer = Fx::Ratio(in.steer, 127);
        i32 turn;
        if (k.driftDir != 0) {
            Fx into = steer * Fx::FromInt(k.driftDir);  // +1 = steering into the drift
            turn    = k.driftDir * (180 + (into * Fx::FromInt(150)).ToInt());  // ~140m..13m radius at top speed
            k.driftCharge += into > Fx::Ratio(3, 10) ? 2 : 1;
        } else {
            Fx speedFactor = Min(FxAbs(k.speed) / Fx::Ratio(15, 100), Fx::FromInt(1));
            turn           = (steer * speedFactor * Fx::FromInt(ch.turnRate)).ToInt();
            if (k.speed < Fx{0}) turn = -turn;
        }
        k.heading = WrapAngle(k.heading + turn);
    }

    // Velocity chases the heading; low grip while drifting gives the slide.
    V2 desired = Forward(k.heading) * k.speed;
    Fx grip    = spinning ? Fx::Ratio(4, 100) : (k.driftDir != 0 ? Fx::Ratio(10, 100) : Fx::Ratio(30, 100));
    k.vel      = k.vel + (desired - k.vel) * grip;
    k.pos += k.vel;

    i32        prevSeg = k.seg;
    WallResult wall    = ConstrainToTrack(t, k.pos, k.vel, k.seg, kKartRadius, Fx::Ratio(5, 10));
    if (wall.impact > Fx::Ratio(6, 100)) {
        k.speed     = k.speed * Fx::Ratio(80, 100);
        k.bumpTicks = 10;
    }
    k.seg     = wall.tp.seg;
    k.lateral = wall.tp.lateral;
    k.y       = TrackHeight(t, wall.tp);

    // Laps: the start line is at sample 0. Quarter checkpoints must be passed
    // in order, so cutting back across the line doesn't count.
    i32 n = t.count;
    for (int q = 1; q <= 3; ++q) {
        i32 qSeg = n * q / 4;
        bool lowerDone = (k.checkpoints & ((1 << (q - 1)) - 1)) == ((1 << (q - 1)) - 1);
        if (k.seg >= qSeg && k.seg < qSeg + kLineWindow && lowerDone) k.checkpoints |= 1 << (q - 1);
    }
    bool crossedForward  = prevSeg >= n - kLineWindow && k.seg < kLineWindow;
    bool crossedBackward = prevSeg < kLineWindow && k.seg >= n - kLineWindow;
    if (crossedForward && k.checkpoints == kAllCheckpoints) {
        ++k.lap;
        k.checkpoints = 0;
        if (k.lap > s.laps && k.finishTick == 0) {
            k.finishTick = s.tick;
            ++s.finishedCount;
        }
    } else if (crossedBackward && k.checkpoints == 0 && k.lap > 0 && k.finishTick == 0) {
        --k.lap;
        k.checkpoints = kAllCheckpoints;
    }
    Fx frac    = Clamp(wall.tp.along / t.s[k.seg].length, Fx{0}, Fx::Ratio(255, 256));
    k.progress = (k.lap * n + k.seg) * 256 + (frac * Fx::FromInt(256)).ToInt();
}

void CollideKarts(SimState& s) {
    const Fx minDist = kKartRadius * Fx::FromInt(2);
    for (int i = 0; i < s.numKarts; ++i) {
        for (int j = i + 1; j < s.numKarts; ++j) {
            Kart& a   = s.karts[i];
            Kart& b   = s.karts[j];
            V2    d   = b.pos - a.pos;
            u64   dsq = LengthSq64(d);
            if (dsq >= RadiusSq64(minDist)) continue;
            Fx dist = Max(Fx{(i32)ISqrt64(dsq)}, Fx::Ratio(1, 100));
            V2 nrm  = dsq ? d * (Fx::FromInt(1) / dist) : V2{Fx::FromInt(1), Fx{0}};
            // Heavier karts shove lighter ones: the overlap and the velocity
            // exchange are split by the other kart's share of the total weight.
            Fx wa = kCharacters[a.character].weight, wb = kCharacters[b.character].weight;
            Fx shareA = wb / (wa + wb), shareB = wa / (wa + wb);  // how much each one gets moved
            V2 push   = nrm * (minDist - dist);
            a.pos -= push * shareA;
            b.pos += push * shareB;
            Fx va = Dot(a.vel, nrm), vb = Dot(b.vel, nrm);
            if (va > vb) {
                Fx ex = (va - vb) * Fx::Ratio(8, 10);
                a.vel -= nrm * (ex * shareA * Fx::FromInt(2));
                b.vel += nrm * (ex * shareB * Fx::FromInt(2));
            }
            if (a.starTicks > 0 && b.starTicks == 0) HitKart(b);
            if (b.starTicks > 0 && a.starTicks == 0) HitKart(a);
        }
    }
}

// Slipstream: sit in the wake of a kart ahead (close behind, nearly in line,
// both moving and pointing the same way) to charge a boost. Reads the other
// karts only, so the result doesn't depend on update order.
void UpdateSlipstream(SimState& s) {
    for (int i = 0; i < s.numKarts; ++i) {
        Kart& k        = s.karts[i];
        bool  drafting = false;
        if (k.spinTicks == 0 && k.boostTicks == 0 && k.speed > Fx::Ratio(25, 100)) {  // no chaining boosts
            V2 fwd = Forward(k.heading);
            for (int j = 0; j < s.numKarts && !drafting; ++j) {
                const Kart& o = s.karts[j];
                if (j == i || o.speed < Fx::Ratio(20, 100)) continue;
                V2 d     = o.pos - k.pos;
                Fx along = Dot(d, fwd);
                if (along < Fx::FromInt(3) || along > Fx::FromInt(12)) continue;
                if (FxAbs(Cross(fwd, d)) > Fx::Ratio(14, 10)) continue;
                drafting = Dot(fwd, Forward(o.heading)) > Fx::Ratio(9, 10);
            }
        }
        if (!drafting) {
            k.draftTicks = Max(0, k.draftTicks - 2);
        } else if (++k.draftTicks >= kDraftChargeTicks) {
            k.boostTicks = Max(k.boostTicks, 70);
            k.draftTicks = 0;
            k.draftFlash = kDraftFlashTicks;
        }
    }
}

i32 RollItem(SimState& s, i32 place, i32 numKarts) {
    // Rubber-banding by position, as in the genre: leaders get defensive items,
    // the back of the pack gets catch-up items. Weights per row sum to 100.
    static constexpr i32 kWeights[4][Item_Count] = {
        // none boost banana green red star
        {0, 15, 45, 40, 0, 0},   // leading
        {0, 20, 25, 30, 25, 0},  // front half
        {0, 35, 0, 20, 35, 10},  // back half
        {0, 40, 0, 0, 30, 30},   // last places
    };
    i32 bucket = place <= 1 ? 0 : (place * 2 <= numKarts ? 1 : (place < numKarts - 1 ? 2 : 3));
    i32 roll   = s.rng.Range(0, 99);
    for (i32 item = 1; item < Item_Count; ++item) {
        roll -= kWeights[bucket][item];
        if (roll < 0) return item;
    }
    return Item_Boost;
}

void SpawnProjectile(SimState& s, i32 kind, V2 pos, Fx y, V2 vel, i32 seg, i32 owner, i32 target) {
    for (Projectile& p : s.projectiles) {
        if (p.kind) continue;
        p          = {};
        p.kind     = kind;
        p.pos      = pos;
        p.y        = y;
        p.vel      = vel;
        p.seg      = seg;
        p.owner    = owner;
        p.target   = target;
        p.life     = 600;
        p.bounces  = 6;
        p.armTicks = 12;
        return;
    }
}

void UseItem(SimState& s, int i, PlayerInput in) {
    Kart& k   = s.karts[i];
    V2    fwd = Forward(k.heading);
    switch (k.item) {
        case Item_Boost: k.boostTicks = Max(k.boostTicks, 75); break;
        case Item_Star:  k.starTicks = 420; break;
        case Item_Banana:
            for (Hazard& h : s.hazards) {
                if (h.active) continue;
                h = {1, k.pos - fwd * Fx::Ratio(18, 10), k.y, i, 20};
                break;
            }
            break;
        case Item_GreenShell:
        case Item_RedShell: {
            bool back   = (in.buttons & In_LookBack) != 0;
            V2   dir    = back ? -fwd : fwd;
            i32  target = -1;
            if (k.item == Item_RedShell && !back)
                for (int j = 0; j < s.numKarts; ++j)
                    if (s.karts[j].place == k.place - 1) target = j;
            SpawnProjectile(s, k.item, k.pos + dir * Fx::Ratio(16, 10), k.y, dir * kShellSpeed, k.seg, i, target);
            break;
        }
    }
    k.item = Item_None;
}

void UpdateItems(SimState& s, const PlayerInput* in) {
    for (int b = 0; b < s.numBoxes; ++b)
        if (s.boxes[b].respawnTicks > 0) --s.boxes[b].respawnTicks;

    for (int i = 0; i < s.numKarts; ++i) {
        Kart& k = s.karts[i];
        for (int b = 0; b < s.numBoxes; ++b) {
            ItemBox& box = s.boxes[b];
            if (box.respawnTicks || LengthSq64(k.pos - box.pos) >= RadiusSq64(Fx::Ratio(16, 10))) continue;
            box.respawnTicks = kBoxRespawn;
            if (k.item == Item_None && k.rouletteTicks == 0) k.rouletteTicks = kRouletteTicks;
        }
        if (k.rouletteTicks > 0 && --k.rouletteTicks == 0) k.item = RollItem(s, k.place, s.numKarts);
        if (k.item != Item_None && k.spinTicks == 0 && Pressed(k, in[i], In_Item)) UseItem(s, i, in[i]);
    }
}

void UpdateProjectiles(SimState& s, const Track& t) {
    for (int pi = 0; pi < kMaxProjectiles; ++pi) {
        Projectile& p = s.projectiles[pi];
        if (!p.kind) continue;
        if (p.armTicks > 0) --p.armTicks;
        if (--p.life <= 0) {
            p.kind = 0;
            continue;
        }

        // Red shells follow the road until close, then home in.
        if (p.kind == Item_RedShell && p.target >= 0) {
            V2 to = s.karts[p.target].pos - p.pos;
            V2 aim;
            if (LengthSq64(to) < RadiusSq64(Fx::FromInt(14))) aim = Normalize(to);
            else aim = Normalize(t.s[WrapSeg(t, p.seg + 5)].pos - p.pos);
            p.vel = Normalize(p.vel + aim * (kShellSpeed * Fx::Ratio(3, 10))) * kShellSpeed;
        }

        p.pos += p.vel;
        bool       green = p.kind == Item_GreenShell;
        WallResult wall  = ConstrainToTrack(t, p.pos, p.vel, p.seg, kShellRadius, green ? Fx::FromInt(1) : Fx{0});
        if (wall.impact > Fx{0}) {
            if (green && --p.bounces < 0) {
                p.kind = 0;
                continue;
            }
            if (!green) p.vel = Normalize(p.vel) * kShellSpeed;  // red shells slide along walls
        }
        p.seg = wall.tp.seg;
        p.y   = TrackHeight(t, wall.tp);

        for (int k = 0; k < s.numKarts && p.kind; ++k) {
            if (k == p.owner && p.armTicks > 0) continue;
            if (LengthSq64(s.karts[k].pos - p.pos) >= RadiusSq64(kKartRadius + kShellRadius)) continue;
            HitKart(s.karts[k]);
            p.kind = 0;
        }
        for (Hazard& h : s.hazards) {
            if (!p.kind || !h.active) continue;
            if (LengthSq64(h.pos - p.pos) < RadiusSq64(Fx::FromInt(1))) h.active = p.kind = 0;
        }
        for (int qi = pi + 1; qi < kMaxProjectiles && p.kind; ++qi) {
            Projectile& q = s.projectiles[qi];
            if (q.kind && LengthSq64(q.pos - p.pos) < RadiusSq64(kShellRadius * Fx::FromInt(2))) q.kind = p.kind = 0;
        }
    }

    for (Hazard& h : s.hazards) {
        if (!h.active) continue;
        if (h.armTicks > 0) --h.armTicks;
        for (int k = 0; k < s.numKarts; ++k) {
            if (k == h.owner && h.armTicks > 0) continue;
            if (LengthSq64(s.karts[k].pos - h.pos) >= RadiusSq64(Fx::Ratio(12, 10))) continue;
            HitKart(s.karts[k]);
            h.active = 0;
            break;
        }
    }
}

void UpdatePlaces(SimState& s) {
    i32 order[kMaxKarts];
    for (int i = 0; i < s.numKarts; ++i) order[i] = i;
    auto ahead = [&](int a, int b) {  // strict "a ranks above b", index as tiebreak
        const Kart& ka = s.karts[a];
        const Kart& kb = s.karts[b];
        if ((ka.finishTick != 0) != (kb.finishTick != 0)) return ka.finishTick != 0;
        if (ka.finishTick != kb.finishTick) return ka.finishTick < kb.finishTick;
        if (ka.progress != kb.progress) return ka.progress > kb.progress;
        return a < b;
    };
    for (int i = 1; i < s.numKarts; ++i)  // insertion sort: tiny n, stable, no allocation
        for (int j = i; j > 0 && ahead(order[j], order[j - 1]); --j) {
            i32 tmp      = order[j];
            order[j]     = order[j - 1];
            order[j - 1] = tmp;
        }
    for (int p = 0; p < s.numKarts; ++p) s.karts[order[p]].place = p + 1;
}

bool KartAheadInCone(const SimState& s, int i, Fx range, Fx minDot) {
    const Kart& k   = s.karts[i];
    V2          fwd = Forward(k.heading);
    for (int j = 0; j < s.numKarts; ++j) {
        if (j == i) continue;
        V2 d = s.karts[j].pos - k.pos;
        if (LengthSq64(d) < RadiusSq64(range) && Dot(fwd, Normalize(d)) > minDot) return true;
    }
    return false;
}

}  // namespace

void SimInit(SimState& s, const Track& t, u32 seed, int numHumans, int laps, const i32* humanCharacters) {
    s           = {};
    s.rng.s     = seed ? seed : 0x9E3779B9u;
    s.numKarts  = kMaxKarts;
    s.numHumans = Clamp(numHumans, 0, kMaxPlayers);
    s.laps      = laps;
    s.trackId   = t.id;
    s.phase     = Phase_Countdown;

    // Humans keep their picks (duplicates allowed); bots take the unpicked
    // characters in roster order. There are always enough: 8 karts, 8 characters.
    bool taken[Char_Count] = {};
    for (int i = 0; i < s.numHumans; ++i) {
        i32 c                   = humanCharacters ? Clamp(humanCharacters[i], 0, Char_Count - 1) : i % Char_Count;
        s.karts[i].character    = c;
        taken[c]                = true;
    }
    for (int i = s.numHumans, next = 0; i < s.numKarts; ++i) {
        while (next < Char_Count - 1 && taken[next]) ++next;
        s.karts[i].character = next;
        taken[next]          = true;
    }

    for (int i = 0; i < s.numKarts; ++i) {
        // Humans start at the back of the grid, as tradition demands.
        int                slot = s.numKarts - 1 - i;
        int                row = slot / 2, col = slot % 2;
        i32                seg = WrapSeg(t, -3 - row * 3);
        const TrackSample& smp = t.s[seg];
        Kart&              k   = s.karts[i];
        k.pos         = smp.pos + smp.right * (col ? Fx::Ratio(25, 10) : Fx::Ratio(-25, 10));
        k.heading     = AngleOf(smp.tangent);
        TrackPos tp   = LocateOnTrack(t, k.pos, seg);
        k.seg         = tp.seg;
        k.lateral     = tp.lateral;
        k.y           = TrackHeight(t, tp);
        k.checkpoints = kAllCheckpoints;  // first crossing of the line starts lap 1
        k.isHuman     = i < s.numHumans;
        k.skill       = (i * 3) % 5;
        k.place       = slot + 1;
    }

    for (int r = 0; r < 4; ++r) {
        for (int b = 0; b < 4; ++b) {
            const TrackSample& smp = t.s[t.itemRows[r]];
            ItemBox&           box = s.boxes[s.numBoxes++];
            box.pos = smp.pos + smp.right * (Fx::FromInt(-9 + 6 * b) * Fx::Ratio(1, 2));
            box.y   = smp.y;
        }
    }
}

void SimTick(SimState& s, const Track& t, const PlayerInput* inputs) {
    PlayerInput in[kMaxKarts] = {};
    for (int i = 0; i < s.numHumans; ++i) in[i] = inputs[i];

    if (s.phase == Phase_Countdown) {
        if (++s.phaseTicks >= kCountdownTicks) {
            s.phase      = Phase_Racing;
            s.phaseTicks = 0;
        }
        for (int i = 0; i < s.numKarts; ++i) s.karts[i].prevButtons = in[i].buttons;
        ++s.tick;
        return;
    }
    ++s.phaseTicks;

    // All AI decisions are taken from the same pre-tick state, so the order
    // karts are updated in can't leak into their choices.
    for (int i = 0; i < s.numKarts; ++i) {
        const Kart& k = s.karts[i];
        if (!k.isHuman || k.finishTick != 0) in[i] = BotInput(s, t, i);
    }

    for (int i = 0; i < s.numKarts; ++i) UpdateKart(s, t, i, in[i]);
    CollideKarts(s);
    UpdateSlipstream(s);
    UpdateItems(s, in);
    UpdateProjectiles(s, t);
    UpdatePlaces(s);
    for (int i = 0; i < s.numKarts; ++i) s.karts[i].prevButtons = in[i].buttons;

    if (s.phase == Phase_Racing) {
        bool done = s.numHumans > 0 ? true : s.finishedCount == s.numKarts;
        for (int i = 0; i < s.numHumans; ++i) done &= s.karts[i].finishTick != 0;
        if (done) {
            s.phase      = Phase_Finished;
            s.phaseTicks = 0;
        }
    }
    ++s.tick;
}

u64 SimHash(const SimState& s) { return HashBytes(&s, sizeof(s)); }

PlayerInput BotInput(const SimState& s, const Track& t, int i) {
    const Kart& k  = s.karts[i];
    PlayerInput in = {0, In_Accel};
    if (s.phase == Phase_Countdown || k.spinTicks > 0) return in;

    // Aim at a point down the road; further ahead when fast. Each bot has a
    // preferred lane and cuts toward the inside of upcoming bends.
    // ~27m ahead at top speed: further and the chord to the target cuts across
    // the grass on the inside of bends.
    i32                la    = 3 + (Max(k.speed, Fx{0}) * Fx::FromInt(14)).ToInt();
    const TrackSample& tgt   = t.s[WrapSeg(t, k.seg + la)];
    Fx                 bend  = t.s[WrapSeg(t, k.seg + la / 2)].curvature;
    Fx                 lane  = Fx::FromInt(((i * 37) % 7) - 3) * Fx::Ratio(6, 10) + bend * Fx::FromInt(4);
    Fx                 limit = t.roadHalf - Fx::Ratio(15, 10);
    // Skilled bots hunt for slipstreams on straights; the rest only draft by chance.
    if (k.skill >= 3 && FxAbs(bend) < Fx::Ratio(8, 100)) {
        V2 fwdK = Forward(k.heading);
        for (int j = 0; j < s.numKarts; ++j) {
            const Kart& o = s.karts[j];
            V2          d = o.pos - k.pos;
            Fx          along = Dot(d, fwdK);
            if (j != i && along > Fx::FromInt(4) && along < Fx::FromInt(14) && FxAbs(Cross(fwdK, d)) < Fx::FromInt(3)) {
                lane = o.lateral;
                break;
            }
        }
    }
    lane      = Clamp(lane, -limit, limit);
    V2 target = tgt.pos + tgt.right * lane;

    V2  fwd   = Forward(k.heading);
    V2  dir   = Normalize(target - k.pos);
    Fx  c     = Cross(fwd, dir);
    i32 steer = (c * Fx::FromInt(127 * 3)).ToInt();
    if (Dot(fwd, dir) < Fx{0}) steer = c < Fx{0} ? -127 : 127;
    in.steer = (i8)Clamp(steer, -127, 127);
    if (Dot(fwd, dir) < Fx::Ratio(5, 10) && k.speed > Fx::Ratio(25, 100)) in.buttons = In_Brake;

    // Drift through bends, holding until the bend straightens out.
    Fx  curve    = t.s[WrapSeg(t, k.seg + 6)].curvature;
    i32 curveDir = curve > Fx{0} ? 1 : -1;
    if (k.driftDir != 0) {
        bool keep = (curveDir == k.driftDir && FxAbs(curve) > Fx::Ratio(8, 100)) ||
                    (k.driftCharge < kMiniTurbo && curveDir == k.driftDir && FxAbs(curve) > Fx::Ratio(5, 100));
        // Bail out (and cash in the boost) before sliding off either edge.
        Fx side = k.lateral * Fx::FromInt(k.driftDir);  // + = toward the inside of the drift
        if (side < -(t.roadHalf - Fx::Ratio(25, 10)) || side > t.roadHalf - Fx::Ratio(15, 10)) keep = false;
        if (keep) in.buttons |= In_Drift;
    } else if (FxAbs(curve) > Fx::Ratio(22, 100) && k.speed > Fx::Ratio(30, 100) && in.steer * curveDir > 30 &&
               !(k.prevButtons & In_Drift)) {
        in.buttons |= In_Drift;
    }

    // Items. "Randomness" is a hash of (tick bucket, kart): pure and repeatable.
    if (k.item != Item_None && !(k.prevButtons & In_Item)) {
        u32  r   = HashU32((u32)(s.tick / 20) * 131u + (u32)i * 7919u);
        bool use = false;
        switch (k.item) {
            case Item_Boost:      use = FxAbs(curve) < Fx::Ratio(5, 100) && r % 3 == 0; break;
            case Item_Banana:     use = r % 4 == 0; break;
            case Item_GreenShell: use = KartAheadInCone(s, i, Fx::FromInt(25), Fx::Ratio(97, 100)); break;
            case Item_RedShell:   use = r % 3 == 0; break;
            case Item_Star:       use = true; break;
        }
        if (use) in.buttons |= In_Item;
    }
    return in;
}

}  // namespace ss
