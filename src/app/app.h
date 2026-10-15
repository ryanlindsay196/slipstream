// app.h - screens, race orchestration, camera and HUD. Owns no OS state:
// everything goes through platform.h, so this runs headless too.
#pragma once

#include "app/input.h"
#include "core/memory.h"
#include "net/session.h"
#include "render/draw2d.h"
#include "render/scene.h"

namespace ss {

constexpr int kFbWidth  = 480;
constexpr int kFbHeight = 270;

struct Memory {
    void*  block;
    size_t size;
    Arena  persistent;
    Arena  level;    // everything for the loaded track; reset to switch tracks
    Arena  scratch;
};

struct AppOptions {
    char          name[kNameLen];
    u16           port;
    const char*   joinAddress;  // auto-join on start
    bool          autoHost;     // auto-host on start
    bool          autoRace;     // jump straight into a single race
    bool          autopilot;    // local kart driven by the bot AI (demos, tests)
    bool          headless;     // fixed one tick per frame, no pacing
    bool          overlay;      // start with the F1 debug overlay visible
    int           autoStartPlayers;  // host: start once this many players are in
    float         showcaseDegrees;   // >= 0: orbit camera around your kart at this angle (screenshots)
    bool          gallery;           // model viewer instead of the game
    i32           character;         // initial character pick
    i32           trackId;           // initial track pick
    NetConditions conditions;
};

enum class Screen { Title, CharacterSelect, TrackSelect, Controls, HostLobby, JoinEntry, Connecting, ClientLobby, Race };
enum class Pending { Offline, Host, Join };  // where character/track select leads
enum class RaceMode { Offline, Online };

struct FrameTimings {
    float simMs, renderMs, hudMs, netMs, frameMs;
};

struct App {
    Memory*    mem;
    AppOptions opt;
    Track*     tracks;  // all circuits, baked once (kTrackCount)
    Track*     track;   // the loaded one
    Scene*     scene;   // built for `track` in the level arena
    i32        loadedTrack;
    i32        selectedCharacter;
    i32        selectedTrack;
    Pending    pending;

    u32*          color;
    float*        depth;
    Canvas        canvas;
    RenderContext rc;

    Bindings         bindings;
    plat::InputFrame in, prev;

    Screen screen;
    int    menuIndex;
    bool   quit;

    // Attract-mode race running behind the menus.
    SimState* demo;

    // Race.
    RaceMode    mode;
    Rollback*   rb;
    NetSession* net;
    bool        raceActive;
    bool        paused;
    int         pauseIndex;
    u32         frameCounter;

    KartVisuals raceVisuals, demoVisuals;  // wheel spin / steering animation

    // Camera (presentation-only smoothing).
    Vec3  camEye, camTarget;
    float camYaw;
    bool  camValid;

    // Controls screen.
    int  ctlRow, ctlCol;
    bool capturing, waitRelease;

    char joinText[48];
    char message[64];
    u32  messageUntilMs;

    // Timing.
    u64          lastFrameTicks;
    double       accumulator;
    bool         overlay;
    FrameTimings timings;
    u32          nowMs;
    float        timeSeconds;

    // Minimap projection.
    float mapMinX, mapMinZ, mapScale;
};

void AppInit(App& app, Memory& mem, const AppOptions& opt);
bool AppFrame(App& app);  // false when the app should exit
void AppShutdown(App& app);

// Starts a race directly (used by menus and command-line shortcuts).
void AppStartOfflineRace(App& app);

}  // namespace ss
