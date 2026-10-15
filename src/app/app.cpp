#include "app/app.h"
#include "game/characters.h"
#include "game/tracks.h"

#include <cstdio>
#include <initializer_list>

namespace ss {

namespace {

constexpr const char* kControlsFile = "slipstream_controls.cfg";
constexpr double      kTickSeconds  = 1.0 / kTickRate;

constexpr u32 kWhite  = 0xFFFFFF;
constexpr u32 kDim    = 0xA9B4C2;
constexpr u32 kAccent = 0xFFD447;
constexpr u32 kBad    = 0xFF5A5A;
constexpr u32 kGood   = 0x6BE38A;
constexpr u32 kPanel  = 0x0E1420;

const char* kItemNames[Item_Count] = {"", "BOOST", "BANANA", "GREEN SHELL", "RED SHELL", "STAR"};

void ShowMessage(App& a, const char* text, u32 ms = 2500) {
    snprintf(a.message, sizeof(a.message), "%s", text);
    a.messageUntilMs = a.nowMs + ms;
}

const char* Ordinal(int place) {
    static const char* k[] = {"?", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH"};
    return k[Clamp(place, 0, 8)];
}

u32 PlaceColor(int place) {
    switch (place) {
        case 1:  return 0xFFD447;
        case 2:  return 0xD9E2EC;
        case 3:  return 0xE09A5A;
        default: return kWhite;
    }
}

void KartName(const App& a, int i, char* out, int cap) {
    const SimState& s = a.rb->state;
    if (i < s.numHumans) {
        if (a.mode == RaceMode::Online) snprintf(out, (size_t)cap, "%s", a.net->playerNames[i]);
        else snprintf(out, (size_t)cap, "%s", a.opt.name);
    } else {
        snprintf(out, (size_t)cap, "%s", kCharacters[s.karts[i].character].name);
    }
}

bool KeyPressed(const App& a, plat::Key k) { return a.in.keys[k] && !a.prev.keys[k]; }

bool ActionPressed(const App& a, Action act) {
    return ActionValue(a.bindings, act, a.in) >= 0.5f && ActionValue(a.bindings, act, a.prev) < 0.5f;
}

float Smooth(float prev, float sample) { return prev * 0.9f + sample * 0.1f; }

// ---------------------------------------------------------------------------
// Screen transitions
// ---------------------------------------------------------------------------
// Builds the scene for a track (if it isn't already loaded) in the level
// arena, and restarts the attract-mode race on it.
void LoadTrack(App& a, int id) {
    id = Clamp(id, 0, kTrackCount - 1);
    if (a.loadedTrack == id && a.scene->chunkCount) return;
    a.mem->level.Reset();
    a.track = &a.tracks[id];
    BuildScene(*a.scene, *a.track, a.mem->level, a.mem->scratch);
    SimInit(*a.demo, *a.track, 7, 0, 99);
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;  // fit the minimap
    for (int i = 0; i < a.track->count; ++i) {
        float x = a.track->s[i].pos.x.ToFloat(), z = a.track->s[i].pos.z.ToFloat();
        minX = Min(minX, x); maxX = Max(maxX, x); minZ = Min(minZ, z); maxZ = Max(maxZ, z);
    }
    a.mapScale = 96.0f / Max(maxX - minX, maxZ - minZ);
    a.mapMinX  = minX;
    a.mapMinZ  = minZ;
    a.demoVisuals.valid = false;
    a.camValid          = false;
    a.loadedTrack       = id;
}

void GoToTitle(App& a) {
    a.screen    = Screen::Title;
    a.menuIndex = 0;
}

void LeaveRace(App& a) {
    if (a.mode == RaceMode::Online) NetClose(*a.net);
    a.raceActive = false;
    a.paused     = false;
    GoToTitle(a);
}

void StartOnlineRace(App& a) {
    NetSession& n = *a.net;
    LoadTrack(a, n.trackId);
    a.rb->Init(a.track, n.seed, n.numPlayers, n.localSlot, n.laps, kDefaultInputDelay, n.playerCharacters);
    NetAttachRollback(n, a.rb);
    a.mode       = RaceMode::Online;
    a.screen     = Screen::Race;
    a.raceActive = true;
    a.paused     = false;
    a.camValid   = false;
    a.raceVisuals.valid = false;
}

void EnterHostLobby(App& a) {
    if (!NetHost(*a.net, a.opt.port, a.opt.name, a.selectedCharacter, a.selectedTrack)) {
        ShowMessage(a, a.net->error);
        return;
    }
    a.screen = Screen::HostLobby;
}

void BeginJoin(App& a) {
    plat::NetAddress addr;
    if (!plat::ParseAddress(a.joinText, &addr)) {
        ShowMessage(a, "USE THE FORMAT 192.168.1.20:7777");
        return;
    }
    if (!NetJoin(*a.net, addr, a.opt.name, a.selectedCharacter)) {
        ShowMessage(a, a.net->error);
        return;
    }
    a.screen = Screen::Connecting;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
void UpdateTitle(App& a, const MenuInput& m) {
    constexpr int kItems = 5;
    if (m.up) a.menuIndex = (a.menuIndex + kItems - 1) % kItems;
    if (m.down) a.menuIndex = (a.menuIndex + 1) % kItems;
    if (!m.confirm) return;
    switch (a.menuIndex) {
        case 0: a.pending = Pending::Offline; a.screen = Screen::CharacterSelect; break;
        case 1: a.pending = Pending::Host; a.screen = Screen::CharacterSelect; break;
        case 2: a.pending = Pending::Join; a.screen = Screen::CharacterSelect; break;
        case 3:
            a.screen = Screen::Controls;
            a.ctlRow = a.ctlCol = 0;
            break;
        case 4: a.quit = true; break;
    }
}

void UpdateCharacterSelect(App& a, const MenuInput& m) {
    int& c = a.selectedCharacter;  // 4 x 2 grid
    if (m.left) c = (c + Char_Count - 1) % Char_Count;
    if (m.right) c = (c + 1) % Char_Count;
    if (m.up || m.down) c = (c + 4) % Char_Count;
    if (m.back) GoToTitle(a);
    if (!m.confirm) return;
    a.screen = a.pending == Pending::Join ? Screen::JoinEntry : Screen::TrackSelect;
}

void UpdateTrackSelect(App& a, const MenuInput& m) {
    if (m.left) a.selectedTrack = (a.selectedTrack + kTrackCount - 1) % kTrackCount;
    if (m.right) a.selectedTrack = (a.selectedTrack + 1) % kTrackCount;
    LoadTrack(a, a.selectedTrack);  // the menu shows the track itself, live
    if (m.back) a.screen = Screen::CharacterSelect;
    if (!m.confirm) return;
    if (a.pending == Pending::Host) EnterHostLobby(a);
    else AppStartOfflineRace(a);
}

void UpdateControls(App& a, const MenuInput& m) {
    const int rows = Act_Count + 2;
    if (a.capturing) {
        if (a.waitRelease) {  // don't capture the very press that opened the prompt
            if (!AnyInputHeld(a.in)) a.waitRelease = false;
            return;
        }
        bool padBack = false;
        for (int p = 0; p < plat::kMaxPads; ++p) {
            u32 bit = 1u << plat::Pad_Back;
            padBack |= (a.in.pads[p].buttons & bit) && !(a.prev.pads[p].buttons & bit);
        }
        if (KeyPressed(a, plat::Key_Escape) || padBack) {
            a.capturing = false;
            return;
        }
        Binding b;
        if (CaptureBinding((BindSlot)a.ctlCol, a.in, a.prev, &b)) {
            AssignBinding(a.bindings, (Action)a.ctlRow, (BindSlot)a.ctlCol, b);
            SaveBindings(a.bindings, kControlsFile, a.mem->scratch);
            a.capturing = false;
            ShowMessage(a, "SAVED", 1200);
        }
        return;
    }

    if (m.up) a.ctlRow = (a.ctlRow + rows - 1) % rows;
    if (m.down) a.ctlRow = (a.ctlRow + 1) % rows;
    if (m.left || m.right) a.ctlCol ^= 1;
    if (a.ctlRow < Act_Count && (KeyPressed(a, plat::Key_Delete) || KeyPressed(a, plat::Key_Backspace))) {
        a.bindings.slots[a.ctlRow][a.ctlCol] = {Bind_None, 0};
        SaveBindings(a.bindings, kControlsFile, a.mem->scratch);
    }
    if (m.back) GoToTitle(a);
    if (!m.confirm) return;
    if (a.ctlRow < Act_Count) {
        a.capturing   = true;
        a.waitRelease = true;
    } else if (a.ctlRow == Act_Count) {
        DefaultBindings(a.bindings);
        SaveBindings(a.bindings, kControlsFile, a.mem->scratch);
        ShowMessage(a, "CONTROLS RESET TO DEFAULTS");
    } else {
        GoToTitle(a);
    }
}

void UpdateLobbies(App& a, const MenuInput& m) {
    NetSession& n = *a.net;
    NetPoll(n, a.nowMs);
    if (n.phase == NetPhase::Disconnected) {
        ShowMessage(a, n.error ? n.error : "DISCONNECTED");
        NetClose(n);
        GoToTitle(a);
        return;
    }
    if (n.phase == NetPhase::InGame) {
        StartOnlineRace(a);
        NetSendFrame(n, a.nowMs);
        return;
    }
    if (a.screen == Screen::Connecting && n.phase == NetPhase::Lobby) a.screen = Screen::ClientLobby;
    bool autoStart = a.opt.autoStartPlayers > 0 && NetConnectedCount(n) >= a.opt.autoStartPlayers;
    if (a.screen == Screen::HostLobby && n.phase == NetPhase::Lobby && (m.confirm || autoStart))
        NetHostBeginRace(n, HashU32(a.nowMs ^ 0xA5A5A5u), kDefaultLaps, a.nowMs);
    if (m.back) {
        NetClose(n);
        GoToTitle(a);
        return;
    }
    NetSendFrame(n, a.nowMs);
}

void UpdateJoinEntry(App& a, const MenuInput& m) {
    size_t len = strlen(a.joinText);
    for (int i = 0; i < a.in.textLen; ++i) {
        char ch = a.in.text[i];
        bool ok = (ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z');
        if (ok && len + 1 < sizeof(a.joinText)) {
            a.joinText[len++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
            a.joinText[len]   = 0;
        }
    }
    if (KeyPressed(a, plat::Key_Backspace) && len > 0) a.joinText[len - 1] = 0;
    if (KeyPressed(a, plat::Key_Enter) || (m.confirm && !a.in.keys[plat::Key_Space])) BeginJoin(a);
    else if (KeyPressed(a, plat::Key_Escape) || (m.back && !a.in.keys[plat::Key_Backspace])) GoToTitle(a);
}

void UpdateRace(App& a, const MenuInput& m, int ticks) {
    u64  t0     = plat::Now();
    bool online = a.mode == RaceMode::Online;
    if (online) {
        NetPoll(*a.net, a.nowMs);
        if (a.net->phase == NetPhase::Disconnected) {
            ShowMessage(a, a.net->error ? a.net->error : "DISCONNECTED", 4000);
            LeaveRace(a);
            return;
        }
    }
    u64 t1 = plat::Now();

    const SimState& s           = a.rb->state;
    bool            showResults = s.phase == Phase_Finished && s.phaseTicks > 150;
    if (ActionPressed(a, Act_Pause) && !showResults) {
        a.paused     = !a.paused;
        a.pauseIndex = 0;
    } else if (a.paused) {
        if (m.up || m.down) a.pauseIndex ^= 1;
        if (m.back) a.paused = false;
        if (m.confirm) {
            if (a.pauseIndex == 0) a.paused = false;
            else {
                LeaveRace(a);
                return;
            }
        }
    } else if (showResults && m.confirm) {
        LeaveRace(a);
        return;
    }

    PlayerInput local{};
    if (!a.paused && !showResults)
        local = a.opt.autopilot ? BotInput(s, *a.track, a.rb->localPlayer) : SampleRaceInput(a.bindings, a.in);

    for (int t = 0; t < ticks; ++t) {
        if (!online && a.paused) break;  // online races can't pause: the others keep driving
        if (!a.rb->CanAdvance()) {
            ++a.rb->stats.stalls;  // too far ahead of the remote inputs; wait for them
            break;
        }
        // Clock sync: if we're running ahead of the other peers, skip every
        // other tick until they catch up. Spreads the correction out.
        if (online && NetFrameAdvantage(*a.net, a.nowMs) > 1.5f && (a.frameCounter & 1)) {
            ++a.rb->stats.stalls;
            continue;
        }
        a.rb->AddLocalInput(local);
        a.rb->Advance();
    }
    u64 t2 = plat::Now();
    if (online) NetSendFrame(*a.net, a.nowMs);
    u64 t3 = plat::Now();
    a.timings.simMs = Smooth(a.timings.simMs, (float)plat::TicksToMs(t2 - t1));
    a.timings.netMs = Smooth(a.timings.netMs, (float)plat::TicksToMs((t1 - t0) + (t3 - t2)));
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
Camera ChaseCamera(App& a, const SimState& s, int kartIndex, bool lookBack) {
    const Kart& k = s.karts[kartIndex];
    Vec3        kp{k.pos.x.ToFloat(), k.y.ToFloat(), k.pos.z.ToFloat()};
    if (a.opt.showcaseDegrees >= 0) {  // close orbit for model screenshots
        float ang = AngleToRadians(k.heading) + a.opt.showcaseDegrees * kPi / 180.0f;
        Vec3  eye = kp + Vec3{std::sin(ang) * 3.4f, 1.5f, std::cos(ang) * 3.4f};
        return {eye, kp + Vec3{0, 0.55f, 0}, 0.9f, 240.0f};
    }
    float       targetYaw = AngleToRadians(k.heading);
    if (!a.camValid) a.camYaw = targetYaw;
    if (k.spinTicks == 0) {  // don't let the camera spin with the kart
        float d = targetYaw - a.camYaw;
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        a.camYaw += d * 0.14f;
    }
    float yaw = a.camYaw + (lookBack ? kPi : 0.0f);
    Vec3  fwd{std::sin(yaw), 0, std::cos(yaw)};
    Vec3  eye    = kp - fwd * 5.6f + Vec3{0, 2.3f, 0};
    Vec3  target = kp + fwd * 4.0f + Vec3{0, 0.8f, 0};
    if (!a.camValid || lookBack) {
        a.camEye    = eye;
        a.camTarget = target;
    } else {
        a.camEye    = Lerp(a.camEye, eye, 0.3f);
        a.camTarget = Lerp(a.camTarget, target, 0.35f);
    }
    a.camValid = !lookBack;
    float fov  = 1.12f + (k.boostTicks > 0 ? 0.12f : 0.0f);
    return {a.camEye, a.camTarget, fov, 240.0f};
}

Camera AttractCamera(App& a) {
    const SimState& s      = *a.demo;
    int             leader = 0;
    for (int i = 0; i < s.numKarts; ++i)
        if (s.karts[i].place == 1) leader = i;
    const Kart& k = s.karts[leader];
    Vec3        kp{k.pos.x.ToFloat(), k.y.ToFloat(), k.pos.z.ToFloat()};
    float       ang = a.timeSeconds * 0.18f;
    Vec3        eye = kp + Vec3{std::sin(ang) * 11.0f, 4.5f, std::cos(ang) * 11.0f};
    a.camEye        = a.camValid ? Lerp(a.camEye, eye, 0.08f) : eye;
    a.camTarget     = a.camValid ? Lerp(a.camTarget, kp, 0.15f) : kp;
    a.camValid      = true;
    return {a.camEye, a.camTarget + Vec3{0, 0.8f, 0}, 1.1f, 240.0f};
}

void Panel(Canvas& c, int x, int y, int w, int h) {
    BlendRect(c, x, y, w, h, kPanel, 190);
    FillRect(c, x, y, w, 1, 0x3A4A66);
    FillRect(c, x, y + h - 1, w, 1, 0x3A4A66);
}

void DrawItemIcon(Canvas& c, int cx, int cy, int item) {
    switch (item) {
        case Item_Boost:  // mushroom
            FillRect(c, cx - 4, cy, 8, 8, 0xF2E3C6);
            FillRect(c, cx - 10, cy - 8, 20, 9, 0xE8433A);
            FillRect(c, cx - 7, cy - 6, 4, 4, kWhite);
            FillRect(c, cx + 3, cy - 6, 4, 4, kWhite);
            break;
        case Item_Banana:
            for (int i = 0; i < 6; ++i) FillRect(c, cx - 9 + i * 3, cy + 4 - (i < 3 ? i : 5 - i) * 3, 5, 5, 0xF5D33A);
            FillRect(c, cx + 8, cy - 4, 3, 3, 0x5A3E1E);
            break;
        case Item_GreenShell:
        case Item_RedShell: {
            u32 col = item == Item_GreenShell ? 0x34C94E : 0xE53B2E;
            FillRect(c, cx - 9, cy - 6, 18, 12, col);
            FillRect(c, cx - 6, cy - 9, 12, 3, col);
            FillRect(c, cx - 10, cy + 4, 20, 3, kWhite);
            FillRect(c, cx - 3, cy - 4, 6, 4, MulColor(col, 1.3f));
            break;
        }
        case Item_Star:
            FillRect(c, cx - 3, cy - 10, 6, 20, 0xFFE14D);
            FillRect(c, cx - 10, cy - 3, 20, 6, 0xFFE14D);
            FillRect(c, cx - 6, cy - 6, 12, 12, 0xFFE14D);
            FillRect(c, cx - 2, cy - 2, 2, 3, 0x202020);
            FillRect(c, cx + 1, cy - 2, 2, 3, 0x202020);
            break;
    }
}

void DrawMinimap(App& a, const SimState& s, int localKart) {
    Canvas& c   = a.canvas;
    int     ox  = kFbWidth - 102, oy = kFbHeight - 102;
    auto    map = [&](V2 p, int* x, int* y) {
        *x = ox + (int)((p.x.ToFloat() - a.mapMinX) * a.mapScale);
        *y = oy + 96 - (int)((p.z.ToFloat() - a.mapMinZ) * a.mapScale);
    };
    BlendRect(c, ox - 4, oy - 4, 104, 104, kPanel, 120);
    const Track& t = *a.track;
    for (int i = 0; i < t.count; i += 2) {
        int x0, y0, x1, y1;
        map(t.s[i].pos, &x0, &y0);
        map(t.s[(i + 2) % t.count].pos, &x1, &y1);
        DrawLine(c, x0, y0 + 1, x1, y1 + 1, 0x101010);
        DrawLine(c, x0, y0, x1, y1, 0xC8D0DA);
    }
    for (int i = s.numKarts - 1; i >= 0; --i) {
        int x, y;
        map(s.karts[i].pos, &x, &y);
        if (i == localKart) {
            FillRect(c, x - 3, y - 3, 7, 7, kWhite);
            FillRect(c, x - 2, y - 2, 5, 5, kCharacters[s.karts[i].character].color);
        } else {
            FillRect(c, x - 1, y - 1, 3, 3, kCharacters[s.karts[i].character].color);
        }
    }
}

void DrawResults(App& a, const SimState& s) {
    Canvas& c = a.canvas;
    int     w = 250, h = 34 + s.numKarts * 13, x = (kFbWidth - w) / 2, y = 40;
    Panel(c, x, y, w, h + 14);
    DrawTextCentered(c, kFbWidth / 2, y + 8, "RESULTS", kAccent, 2);
    for (int p = 1; p <= s.numKarts; ++p) {
        for (int i = 0; i < s.numKarts; ++i) {
            const Kart& k = s.karts[i];
            if (k.place != p) continue;
            char name[24], time[24], line[64];
            KartName(a, i, name, sizeof(name));
            if (k.finishTick) {
                int cs = (k.finishTick - kCountdownTicks) * 100 / kTickRate;
                snprintf(time, sizeof(time), "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
            } else {
                snprintf(time, sizeof(time), "--:--.--");
            }
            snprintf(line, sizeof(line), "%-4s %-12s %s", Ordinal(p), name, time);
            int ly = y + 28 + (p - 1) * 13;
            FillRect(c, x + 12, ly, 6, 6, kCharacters[s.karts[i].character].color);
            DrawText(c, x + 24, ly, line, i == a.rb->localPlayer ? kAccent : kWhite, 1);
        }
    }
    DrawTextCentered(c, kFbWidth / 2, y + h + 2, "PRESS ENTER / A", kDim, 1);
}

void DrawRaceHud(App& a) {
    Canvas&         c  = a.canvas;
    const SimState& s  = a.rb->state;
    int             me = a.rb->localPlayer;
    const Kart&     k  = s.karts[me];
    char            buf[96];

    snprintf(buf, sizeof(buf), "LAP %d/%d", Clamp(k.lap, 1, s.laps), s.laps);
    DrawTextShadow(c, 8, 8, buf, kWhite, 2);

    const char* place = Ordinal(k.place);
    DrawTextShadow(c, kFbWidth - TextWidth(place, 4) - 10, 8, place, PlaceColor(k.place), 4);

    // Item slot.
    int bx = kFbWidth / 2 - 22, by = 6;
    Panel(c, bx, by, 44, 32);
    FillRect(c, bx, by, 1, 32, 0x3A4A66);
    FillRect(c, bx + 43, by, 1, 32, 0x3A4A66);
    int item = k.rouletteTicks > 0 ? 1 + (int)((a.frameCounter / 4) % (Item_Count - 1)) : k.item;
    DrawItemIcon(c, bx + 22, by + 17, item);
    if (k.item != Item_None && k.rouletteTicks == 0) DrawTextCentered(c, kFbWidth / 2, by + 36, kItemNames[k.item], kWhite, 1);

    int kmh = (int)(std::fabs(k.speed.ToFloat()) * kTickRate * 3.6f);
    snprintf(buf, sizeof(buf), "%3d KM/H", kmh);
    DrawTextShadow(c, 8, kFbHeight - 20, buf, k.boostTicks > 0 ? 0xFFB347 : kWhite, 2);
    // Drift charge: bar fills toward the next tier, labelled with the tier reached.
    if (k.driftDir != 0) {
        int         ch    = k.driftCharge;
        u32         col   = ch >= kUltraTurbo ? 0xC060FF : ch >= kSuperTurbo ? 0xFF8A1C : ch >= kMiniTurbo ? 0x3FA9FF : kDim;
        const char* label = ch >= kUltraTurbo ? "ULTRA" : ch >= kSuperTurbo ? "SUPER" : ch >= kMiniTurbo ? "MINI" : "DRIFT";
        FillRect(c, 8, kFbHeight - 28, 80, 4, 0x1A2030);
        FillRect(c, 8, kFbHeight - 28, Min(ch, kUltraTurbo) * 80 / kUltraTurbo, 4, col);
        for (int tier : {kMiniTurbo, kSuperTurbo}) FillRect(c, 8 + tier * 80 / kUltraTurbo, kFbHeight - 29, 1, 6, kWhite);
        DrawTextShadow(c, 92, kFbHeight - 29, label, col, 1);
    }
    // Slipstream charge, and a flash when it fires.
    if (k.draftTicks > 0) {
        FillRect(c, 8, kFbHeight - 36, 80, 3, 0x1A2030);
        FillRect(c, 8, kFbHeight - 36, k.draftTicks * 80 / kDraftChargeTicks, 3, 0x9FE8FF);
        DrawTextShadow(c, 92, kFbHeight - 37, "DRAFT", 0x9FE8FF, 1);
    }
    if (k.draftFlash > 0 && (k.draftFlash / 4) % 2 == 0)
        DrawTextCentered(c, kFbWidth / 2, 62, "SLIPSTREAM!", 0x9FE8FF, 3);
    DrawMinimap(a, s, me);

    // Name tags over other human players (online).
    for (int i = 0; i < s.numHumans; ++i) {
        if (i == me) continue;
        const Kart& o = s.karts[i];
        Vec4        h = Transform(a.rc.viewProj, Vec3{o.pos.x.ToFloat(), o.y.ToFloat() + 2.0f, o.pos.z.ToFloat()});
        if (h.w < 0.5f || h.w > 90.0f) continue;
        int sx = (int)((h.x / h.w * 0.5f + 0.5f) * kFbWidth), sy = (int)((0.5f - h.y / h.w * 0.5f) * kFbHeight);
        char name[24];
        KartName(a, i, name, sizeof(name));
        DrawTextCentered(c, sx, sy, name, kCharacters[s.karts[i].character].color, 1);
    }

    if (s.phase == Phase_Countdown) {
        int n = 3 - s.phaseTicks / kTickRate;
        snprintf(buf, sizeof(buf), "%d", Max(n, 1));
        DrawTextCentered(c, kFbWidth / 2, 90, buf, kAccent, 8);
    } else if (s.phase == Phase_Racing && s.phaseTicks < kTickRate) {
        DrawTextCentered(c, kFbWidth / 2, 90, "GO!", kGood, 8);
    }
    if (k.finishTick && !(s.phase == Phase_Finished && s.phaseTicks > 150)) {
        DrawTextCentered(c, kFbWidth / 2, 80, "FINISH!", kAccent, 6);
    }
    if (s.phase == Phase_Finished && s.phaseTicks > 150) DrawResults(a, s);

    if (a.mode == RaceMode::Online) {
        const NetSession&    n  = *a.net;
        const RollbackStats& st = a.rb->stats;
        snprintf(buf, sizeof(buf), "PING %ums  ROLLBACK %u  STALLS %u", NetWorstRtt(n), st.lastDepth, st.stalls);
        DrawTextShadow(c, 8, kFbHeight - 46, buf, kDim, 1);
        if (n.desynced) {
            snprintf(buf, sizeof(buf), "DESYNC DETECTED AT TICK %d", n.desyncTick);
            DrawTextCentered(c, kFbWidth / 2, 50, buf, kBad, 1);
        }
    }

    if (a.paused) {
        Panel(c, kFbWidth / 2 - 70, 90, 140, 70);
        DrawTextCentered(c, kFbWidth / 2, 98, a.mode == RaceMode::Online ? "MENU" : "PAUSED", kAccent, 2);
        const char* items[2] = {"RESUME", a.mode == RaceMode::Online ? "LEAVE RACE" : "QUIT RACE"};
        for (int i = 0; i < 2; ++i)
            DrawTextCentered(c, kFbWidth / 2, 120 + i * 16, items[i], a.pauseIndex == i ? kAccent : kWhite, 2);
    }
}

void DrawMenuItems(Canvas& c, const char* const* items, int count, int selected, int y) {
    for (int i = 0; i < count; ++i) {
        u32 col = i == selected ? kAccent : kWhite;
        DrawTextCentered(c, kFbWidth / 2, y + i * 18, items[i], col, 2);
        if (i == selected) DrawTextShadow(c, kFbWidth / 2 - TextWidth(items[i], 2) / 2 - 16, y + i * 18, ">", col, 2);
    }
}

void DrawPlayerList(App& a, int y) {
    NetSession& n = *a.net;
    Canvas&     c = a.canvas;
    char        line[48];
    int         count = 0;
    auto        row   = [&](const char* name, bool you, int character) {
        character = Clamp(character, 0, Char_Count - 1);
        snprintf(line, sizeof(line), "%d. %s - %s%s", count + 1, name, kCharacters[character].name, you ? " (YOU)" : "");
        FillRect(c, kFbWidth / 2 - 70, y + count * 14, 7, 7, kCharacters[character].color);
        DrawText(c, kFbWidth / 2 - 58, y + count * 14, line, you ? kAccent : kWhite, 1);
        ++count;
    };
    if (n.isHost) {
        row(n.localName, true, n.localCharacter);
        for (const NetPeer& p : n.peers)
            if (p.used) row(p.name, false, p.character);
    } else {
        for (int i = 0; i < n.numPlayers; ++i)
            row(n.playerNames[i], !strcmp(n.playerNames[i], n.localName), n.playerCharacters[i]);
    }
    snprintf(line, sizeof(line), "+ %d BOTS", kMaxKarts - count);
    DrawText(c, kFbWidth / 2 - 58, y + count * 14 + 4, line, kDim, 1);
}

// A track outline scaled into a square, for the track select screen.
void DrawTrackMap(Canvas& c, const Track& t, int x0, int y0, int size, u32 color) {
    float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
    for (int i = 0; i < t.count; ++i) {
        minX = Min(minX, t.s[i].pos.x.ToFloat()); maxX = Max(maxX, t.s[i].pos.x.ToFloat());
        minZ = Min(minZ, t.s[i].pos.z.ToFloat()); maxZ = Max(maxZ, t.s[i].pos.z.ToFloat());
    }
    float scale = (float)(size - 4) / Max(maxX - minX, maxZ - minZ);
    int   ox = x0 + (int)((float)size - (maxX - minX) * scale) / 2, oy = y0 + size - (int)((float)size - (maxZ - minZ) * scale) / 2;
    auto  map = [&](int i, int* x, int* y) {
        *x = ox + (int)((t.s[i].pos.x.ToFloat() - minX) * scale);
        *y = oy - (int)((t.s[i].pos.z.ToFloat() - minZ) * scale);
    };
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < t.count; i += 2) {
            int ax, ay, bx, by;
            map(i, &ax, &ay);
            map((i + 2) % t.count, &bx, &by);
            for (int d = 0; d < (pass ? 1 : 3); ++d)  // dark outline pass, then the line
                DrawLine(c, ax + d - (pass ? 0 : 1), ay + 1, bx + d - (pass ? 0 : 1), by + 1, pass ? color : 0x101010);
        }
    int sx, sy;
    map(0, &sx, &sy);
    FillRect(c, sx - 2, sy - 2, 5, 5, kWhite);  // start line
}

void DrawStatBar(Canvas& c, int x, int y, const char* label, int value, u32 color) {
    DrawTextShadow(c, x, y, label, kDim, 1);
    for (int i = 0; i < 5; ++i) FillRect(c, x + 46 + i * 13, y, 11, 5, i < value ? color : 0x2A3244);
}

void DrawMenus(App& a) {
    Canvas& c = a.canvas;
    char    buf[96];
    switch (a.screen) {
        case Screen::Title: {
            BlendRect(c, 0, 26, kFbWidth, 60, kPanel, 150);
            DrawTextCentered(c, kFbWidth / 2, 34, "SLIPSTREAM", kAccent, 6);
            DrawTextCentered(c, kFbWidth / 2, 72, "A FROM-SCRATCH KART RACER", kDim, 1);
            static const char* items[] = {"SINGLE RACE", "HOST ONLINE", "JOIN ONLINE", "CONTROLS", "QUIT"};
            BlendRect(c, kFbWidth / 2 - 90, 104, 180, 98, kPanel, 150);
            DrawMenuItems(c, items, 5, a.menuIndex, 112);
            DrawTextCentered(c, kFbWidth / 2, kFbHeight - 14, "ARROWS / D-PAD TO MOVE   ENTER / A TO SELECT", kDim, 1);
            break;
        }
        case Screen::CharacterSelect: {
            const CharacterDef& ch = kCharacters[a.selectedCharacter];
            DrawTextCentered(c, kFbWidth / 2, 8, "CHOOSE YOUR RACER", kAccent, 2);
            Panel(c, 300, 40, 170, 132);
            DrawTextShadow(c, 310, 50, ch.name, ch.color, 2);
            DrawTextShadow(c, 310, 68, ch.tagline, kWhite, 1);
            DrawStatBar(c, 310, 92, "SPEED", ch.barSpeed, ch.color);
            DrawStatBar(c, 310, 106, "ACCEL", ch.barAccel, ch.color);
            DrawStatBar(c, 310, 120, "HANDLING", ch.barHandling, ch.color);
            DrawStatBar(c, 310, 134, "WEIGHT", ch.barWeight, ch.color);
            DrawTextShadow(c, 310, 154, a.pending == Pending::Join ? "NEXT: JOIN A HOST" : "NEXT: PICK A TRACK", kDim, 1);
            for (int i = 0; i < Char_Count; ++i) {  // the roster strip
                int  x = 12 + i * 58, y = kFbHeight - 52;
                bool sel = i == a.selectedCharacter;
                BlendRect(c, x, y, 54, 30, sel ? 0x3A4A66 : kPanel, 210);
                FillRect(c, x + 2, y + 2, 50, 4, kCharacters[i].color);
                DrawTextCentered(c, x + 27, y + 15, kCharacters[i].name, sel ? kAccent : kWhite, 1);
                if (sel) {
                    FillRect(c, x - 1, y - 1, 56, 1, kAccent);
                    FillRect(c, x - 1, y + 30, 56, 1, kAccent);
                }
            }
            DrawTextCentered(c, kFbWidth / 2, kFbHeight - 14, "LEFT/RIGHT: CHOOSE   ENTER: SELECT   ESC: BACK", kDim, 1);
            break;
        }
        case Screen::TrackSelect: {
            const TrackDef& td = kTracks[a.selectedTrack];
            DrawTextCentered(c, kFbWidth / 2, 8, "CHOOSE A TRACK", kAccent, 2);
            Panel(c, 16, 36, 300, 80);
            DrawTextShadow(c, 28, 46, "<", kAccent, 3);
            DrawTextShadow(c, 288, 46, ">", kAccent, 3);
            DrawTextCentered(c, 166, 48, td.name, kWhite, 2);
            DrawTextCentered(c, 166, 70, td.subtitle, kDim, 1);
            snprintf(buf, sizeof(buf), "TRACK %d OF %d   %.1f KM   %d LAPS", a.selectedTrack + 1, kTrackCount,
                     a.tracks[a.selectedTrack].totalLength.ToFloat() / 1000.0f, kDefaultLaps);
            DrawTextCentered(c, 166, 92, buf, kWhite, 1);
            Panel(c, 334, 36, 130, 130);
            DrawTrackMap(c, a.tracks[a.selectedTrack], 337, 39, 124, kAccent);
            DrawTextCentered(c, kFbWidth / 2, kFbHeight - 14,
                             a.pending == Pending::Host ? "LEFT/RIGHT: CHOOSE   ENTER: HOST   ESC: BACK"
                                                        : "LEFT/RIGHT: CHOOSE   ENTER: RACE   ESC: BACK",
                             kDim, 1);
            break;
        }
        case Screen::Controls: {
            Panel(c, 20, 10, kFbWidth - 40, kFbHeight - 20);
            DrawTextCentered(c, kFbWidth / 2, 18, "CONTROLS", kAccent, 2);
            bool pad = false;
            for (const auto& p : a.in.pads) pad |= p.connected;
            DrawTextCentered(c, kFbWidth / 2, 36, pad ? "CONTROLLER CONNECTED" : "NO CONTROLLER CONNECTED",
                             pad ? kGood : kDim, 1);
            const int colX[3] = {40, 210, 330};
            DrawText(c, colX[0], 52, "ACTION", kDim);
            DrawText(c, colX[1], 52, "KEYBOARD", kDim);
            DrawText(c, colX[2], 52, "CONTROLLER", kDim);
            for (int r = 0; r < Act_Count + 2; ++r) {
                int y = 66 + r * 16;
                if (r < Act_Count) {
                    DrawText(c, colX[0], y, kActionNames[r], kWhite);
                    for (int col = 0; col < 2; ++col) {
                        bool sel = r == a.ctlRow && col == a.ctlCol;
                        if (sel) BlendRect(c, colX[col + 1] - 4, y - 3, 100, 11, a.capturing ? 0x8A5A00 : 0x2A4A7A, 220);
                        BindingName(a.bindings.slots[r][col], buf, sizeof(buf));
                        if (sel && a.capturing) snprintf(buf, sizeof(buf), "PRESS...");
                        DrawText(c, colX[col + 1], y, buf, sel ? kAccent : kWhite);
                    }
                } else {
                    const char* label = r == Act_Count ? "RESET DEFAULTS" : "BACK";
                    DrawText(c, colX[0], y + 4, label, r == a.ctlRow ? kAccent : kWhite);
                }
            }
            const char* hint = a.capturing ? (a.ctlCol == Slot_Keyboard ? "PRESS A KEY  (ESC CANCELS)"
                                                                        : "PRESS A BUTTON OR MOVE A STICK  (BACK CANCELS)")
                                           : "ENTER: REBIND   DEL: CLEAR   LEFT/RIGHT: DEVICE   ESC: BACK";
            DrawTextCentered(c, kFbWidth / 2, kFbHeight - 24, hint, a.capturing ? kAccent : kDim, 1);
            break;
        }
        case Screen::HostLobby: {
            Panel(c, 60, 30, kFbWidth - 120, 200);
            DrawTextCentered(c, kFbWidth / 2, 40, "HOSTING ONLINE RACE", kAccent, 2);
            snprintf(buf, sizeof(buf), "%s  -  JOIN WITH YOUR IP, PORT %u", kTracks[a.net->trackId].name, a.opt.port);
            DrawTextCentered(c, kFbWidth / 2, 60, buf, kDim, 1);
            DrawPlayerList(a, 82);
            DrawTextCentered(c, kFbWidth / 2, 210,
                             a.net->phase == NetPhase::Starting ? "STARTING..." : "ENTER: START RACE    ESC: CANCEL",
                             kWhite, 1);
            break;
        }
        case Screen::JoinEntry: {
            Panel(c, 60, 70, kFbWidth - 120, 110);
            DrawTextCentered(c, kFbWidth / 2, 80, "JOIN ONLINE RACE", kAccent, 2);
            DrawTextCentered(c, kFbWidth / 2, 102, "HOST ADDRESS", kDim, 1);
            FillRect(c, 110, 114, kFbWidth - 220, 18, 0x050810);
            snprintf(buf, sizeof(buf), "%s%s", a.joinText, (a.frameCounter / 30) & 1 ? "_" : " ");
            DrawText(c, 116, 118, buf, kWhite, 2);
            DrawTextCentered(c, kFbWidth / 2, 160, "TYPE ADDRESS   ENTER: CONNECT   ESC: BACK", kDim, 1);
            break;
        }
        case Screen::Connecting:
        case Screen::ClientLobby: {
            Panel(c, 60, 30, kFbWidth - 120, 200);
            bool connecting = a.screen == Screen::Connecting;
            DrawTextCentered(c, kFbWidth / 2, 40, connecting ? "CONNECTING..." : "IN LOBBY", kAccent, 2);
            DrawTextCentered(c, kFbWidth / 2, 60, a.joinText, kDim, 1);
            if (!connecting) DrawPlayerList(a, 82);
            DrawTextCentered(c, kFbWidth / 2, 210, connecting ? "ESC: CANCEL" : "WAITING FOR HOST TO START   ESC: LEAVE",
                             kWhite, 1);
            break;
        }
        case Screen::Race: break;
    }
}

void DrawDebugOverlay(App& a) {
    Canvas&            c = a.canvas;
    const RasterStats& r = a.rc.stats;
    char               lines[12][80];
    int                n = 0;
    snprintf(lines[n++], 80, "FRAME %5.2fMS  (%3.0f FPS)", a.timings.frameMs, 1000.0f / Max(a.timings.frameMs, 0.01f));
    snprintf(lines[n++], 80, "SIM %5.2f  RENDER %5.2f  HUD %4.2f  NET %4.2f", a.timings.simMs, a.timings.renderMs,
             a.timings.hudMs, a.timings.netMs);
    snprintf(lines[n++], 80, "MESHES %u  TRIS %u/%u  CLIPPED %u", r.meshes, r.trianglesDrawn, r.trianglesIn,
             r.trianglesClipped);
    snprintf(lines[n++], 80, "HEAP ALLOCS IN GAME: %llu", (unsigned long long)heapguard::AllocsWhileLocked());
    if (a.screen == Screen::Race) {
        const RollbackStats& st = a.rb->stats;
        snprintf(lines[n++], 80, "TICK %d  STATE %08X", a.rb->CurrentTick(), (u32)SimHash(a.rb->state));
        snprintf(lines[n++], 80, "ROLLBACKS %u  RESIM %u  MAX DEPTH %u", st.rollbacks, st.resimulatedTicks, st.maxDepth);
        snprintf(lines[n++], 80, "MISPREDICT %u  STALLS %u", st.mispredictions, st.stalls);
        if (a.mode == RaceMode::Online) {
            const NetSession& ns = *a.net;
            snprintf(lines[n++], 80, "RTT %ums  ADV %+.1f  CHECKSUMS OK %d", NetWorstRtt(ns),
                     NetFrameAdvantage(ns, a.nowMs), ns.checksumsMatched);
            snprintf(lines[n++], 80, "TX %uKB  RX %uKB  SIM-DROPPED %u", ns.bytesSent / 1024, ns.bytesReceived / 1024,
                     ns.packetsDropped);
        }
    }
    BlendRect(c, 4, 44, 236, n * 8 + 6, 0x000000, 170);
    for (int i = 0; i < n; ++i) DrawText(c, 8, 48 + i * 8, lines[i], kGood);
}

void RenderFrame(App& a) {
    u64 t0         = plat::Now();
    a.rc.stats     = {};
    if (a.opt.gallery) {
        DrawGallery(a.rc, *a.scene, a.timeSeconds);
        DrawTextCentered(a.canvas, kFbWidth / 2, 8, "MODEL GALLERY - ALL PROCEDURALLY GENERATED", kWhite, 1);
        a.timings.renderMs = Smooth(a.timings.renderMs, (float)plat::TicksToMs(plat::Now() - t0));
        return;
    }
    bool inRace    = a.screen == Screen::Race && a.raceActive;
    bool lookBack  = inRace && !a.paused && ActionValue(a.bindings, Act_LookBack, a.in) >= 0.5f;
    Camera cam     = inRace ? ChaseCamera(a, a.rb->state, a.rb->localPlayer, lookBack) : AttractCamera(a);
    if (a.screen == Screen::CharacterSelect) DrawCharacterPreview(a.rc, *a.scene, a.selectedCharacter, a.timeSeconds);
    else DrawWorld(a.rc, *a.scene, inRace ? a.rb->state : *a.demo, cam, a.timeSeconds, inRace ? &a.raceVisuals : &a.demoVisuals);
    u64 t1 = plat::Now();
    if (inRace) DrawRaceHud(a);
    else DrawMenus(a);
    if (a.overlay) DrawDebugOverlay(a);
    if (a.nowMs < a.messageUntilMs) {
        int w = TextWidth(a.message, 1) + 16;
        Panel(a.canvas, (kFbWidth - w) / 2, kFbHeight - 48, w, 15);
        DrawTextCentered(a.canvas, kFbWidth / 2, kFbHeight - 43, a.message, kWhite, 1);
    }
    u64 t2           = plat::Now();
    a.timings.renderMs = Smooth(a.timings.renderMs, (float)plat::TicksToMs(t1 - t0));
    a.timings.hudMs    = Smooth(a.timings.hudMs, (float)plat::TicksToMs(t2 - t1));
}

}  // namespace

// ---------------------------------------------------------------------------

void AppInit(App& a, Memory& mem, const AppOptions& opt) {
    a     = {};
    a.mem = &mem;
    a.opt = opt;
    if (!a.opt.name[0]) snprintf(a.opt.name, sizeof(a.opt.name), "PLAYER");
    if (!a.opt.port) a.opt.port = kDefaultPort;

    a.tracks = mem.persistent.Push<Track>(kTrackCount);
    for (int t = 0; t < kTrackCount; ++t) BuildTrack(a.tracks[t], t);
    a.scene             = mem.persistent.Push<Scene>();
    a.loadedTrack       = -1;
    a.selectedCharacter = Clamp(opt.character, 0, Char_Count - 1);
    a.selectedTrack     = Clamp(opt.trackId, 0, kTrackCount - 1);

    a.color              = mem.persistent.Push<u32>((size_t)kFbWidth * kFbHeight);
    a.depth              = mem.persistent.Push<float>((size_t)kFbWidth * kFbHeight);
    a.canvas             = {a.color, kFbWidth, kFbHeight};
    a.rc.target          = {a.color, a.depth, kFbWidth, kFbHeight};
    a.rc.scratch         = &mem.scratch;
    a.rc.fogColor        = 0xBFE0F2;
    a.rc.fogStart        = 70.0f;
    a.rc.fogEnd          = 230.0f;

    DefaultBindings(a.bindings);
    if (!LoadBindings(a.bindings, kControlsFile, mem.scratch)) SaveBindings(a.bindings, kControlsFile, mem.scratch);

    a.demo = mem.persistent.Push<SimState>();
    a.rb   = mem.persistent.Push<Rollback>();
    a.net  = mem.persistent.Push<NetSession>();
    a.net->socket     = plat::kInvalidSocket;
    a.net->conditions = opt.conditions;
    LoadTrack(a, a.selectedTrack);

    snprintf(a.joinText, sizeof(a.joinText), "%s", opt.joinAddress ? opt.joinAddress : "127.0.0.1:7777");
    a.lastFrameTicks = plat::Now();
    a.overlay        = opt.overlay;
    a.screen         = Screen::Title;
    if (opt.autoRace) AppStartOfflineRace(a);
    else if (opt.autoHost) EnterHostLobby(a);
    else if (opt.joinAddress) BeginJoin(a);
}

void AppStartOfflineRace(App& a) {
    LoadTrack(a, a.selectedTrack);
    i32 chars[1] = {a.selectedCharacter};
    a.rb->Init(a.track, HashU32(plat::NowMs() | 1), 1, 0, kDefaultLaps, 0, chars);
    a.mode       = RaceMode::Offline;
    a.screen     = Screen::Race;
    a.raceActive = true;
    a.paused     = false;
    a.camValid   = false;
    a.raceVisuals.valid = false;
}

bool AppFrame(App& a) {
    u64    now = plat::Now();
    double dt  = a.opt.headless ? kTickSeconds : (double)(now - a.lastFrameTicks) / (double)plat::Frequency();
    a.lastFrameTicks = now;
    dt               = Min(dt, 0.25);
    a.nowMs          = a.opt.headless ? 1000u + a.frameCounter * 1000u / kTickRate : plat::NowMs();
    a.timeSeconds += (float)dt;
    a.timings.frameMs = Smooth(a.timings.frameMs, (float)(dt * 1000.0));

    a.prev = a.in;
    plat::PumpEvents(&a.in);
    if (a.in.quitRequested) return false;
    MenuInput m = ReadMenuInput(a.in, a.prev);
    if (KeyPressed(a, plat::Key_F1)) a.overlay = !a.overlay;

    a.accumulator += dt;
    int ticks = 0;
    while (a.accumulator >= kTickSeconds - 1e-9 && ticks < 5) {
        a.accumulator -= kTickSeconds;
        ++ticks;
    }
    if (ticks == 5) a.accumulator = 0;  // fell far behind: drop time instead of spiralling

    switch (a.screen) {
        case Screen::Title:       UpdateTitle(a, m); break;
        case Screen::CharacterSelect: UpdateCharacterSelect(a, m); break;
        case Screen::TrackSelect: UpdateTrackSelect(a, m); break;
        case Screen::Controls:    UpdateControls(a, m); break;
        case Screen::JoinEntry:   UpdateJoinEntry(a, m); break;
        case Screen::HostLobby:
        case Screen::Connecting:
        case Screen::ClientLobby: UpdateLobbies(a, m); break;
        case Screen::Race:        UpdateRace(a, m, ticks); break;
    }
    if (a.screen != Screen::Race)
        for (int t = 0; t < ticks; ++t) SimTick(*a.demo, *a.track, nullptr);

    RenderFrame(a);
    plat::Present(a.color, kFbWidth, kFbHeight);
    ++a.frameCounter;

    if (!a.opt.headless) {
        double frameUsed = (double)(plat::Now() - now) / (double)plat::Frequency();
        if (kTickSeconds - a.accumulator - frameUsed > 0.002) plat::SleepMs(1);
    }
    return !a.quit;
}

void AppShutdown(App& a) {
    if (a.net && a.net->phase != NetPhase::Idle) NetClose(*a.net);
}

}  // namespace ss
