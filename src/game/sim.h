// sim.h - the deterministic race simulation.
//
// SimTick(state, track, inputs) is a pure function: fixed-point math only, no
// clocks, no platform calls, no pointers inside SimState. That makes the whole
// race state a flat, padding-free blob that can be snapshotted with memcpy and
// hashed byte-for-byte, which is exactly what rollback netcode needs.
#pragma once

#include "core/base.h"

namespace ss {

constexpr int kTickRate       = 60;
constexpr int kMaxKarts       = 8;
constexpr int kMaxPlayers     = 4;
constexpr int kMaxProjectiles = 24;
constexpr int kMaxHazards     = 24;
constexpr int kMaxItemBoxes   = 16;
constexpr int kDefaultLaps    = 3;
constexpr int kCountdownTicks = 3 * kTickRate;
constexpr int kSpinTicks      = 60;  // spin-out duration when hit

// Drift mini-turbo: charge thresholds (drift ticks) for blue, orange and
// purple sparks. Releasing the drift cashes the highest tier reached.
constexpr int kMiniTurbo  = 90;
constexpr int kSuperTurbo = 200;
constexpr int kUltraTurbo = 320;

// Slipstream: ticks spent drafting close behind another kart before the boost
// fires, and how long the HUD celebrates it.
constexpr int kDraftChargeTicks = 100;
constexpr int kDraftFlashTicks  = 45;

// ---------------------------------------------------------------------------
// Input: the only thing that crosses the network. 2 bytes per player per tick.
// ---------------------------------------------------------------------------
enum InputButton : u8 {
    In_Accel    = 1 << 0,
    In_Brake    = 1 << 1,
    In_Drift    = 1 << 2,
    In_Item     = 1 << 3,
    In_LookBack = 1 << 4,
};

struct PlayerInput {
    i8 steer;    // -127 (left) .. 127 (right)
    u8 buttons;  // InputButton bits
};
inline bool operator==(PlayerInput a, PlayerInput b) { return a.steer == b.steer && a.buttons == b.buttons; }
inline bool operator!=(PlayerInput a, PlayerInput b) { return !(a == b); }

// ---------------------------------------------------------------------------
// Track: static geometry baked once from control points (fixed point, so every
// peer bakes the identical track). Not part of SimState.
// ---------------------------------------------------------------------------
constexpr int kMaxTrackSamples = 512;

struct TrackSample {
    V2 pos;
    Fx y;
    V2 tangent;    // unit, toward the next sample
    V2 right;      // unit, tangent rotated clockwise
    Fx length;     // distance to the next sample
    Fx curvature;  // cross(tangent, tangent 4 samples ahead): + = right-hander
};

struct Track {
    i32         id;     // index into kTracks
    i32         theme;  // TrackTheme
    i32         count;
    TrackSample s[kMaxTrackSamples];
    Fx          roadHalf;  // asphalt half-width
    Fx          wallHalf;  // barrier half-width (grass in between)
    Fx          totalLength;
    i32         itemRows[4];
};

void BuildTrack(Track& t, int trackId);

struct TrackPos {
    i32 seg;
    Fx  along;    // distance past sample `seg` along its tangent
    Fx  lateral;  // signed distance from the centre line (+ = right)
};
TrackPos LocateOnTrack(const Track& t, V2 pos, i32 hintSeg);
Fx       TrackHeight(const Track& t, const TrackPos& tp);
inline i32 WrapSeg(const Track& t, i32 s) { return ((s % t.count) + t.count) % t.count; }

// ---------------------------------------------------------------------------
// Race state. Every field is 4 bytes so the structs have no padding.
// ---------------------------------------------------------------------------
enum Item : i32 { Item_None, Item_Boost, Item_Banana, Item_GreenShell, Item_RedShell, Item_Star, Item_Count };
enum Phase : i32 { Phase_Countdown, Phase_Racing, Phase_Finished };

struct Kart {
    V2    pos;
    Fx    y;
    V2    vel;
    Angle heading;
    Fx    speed;  // signed forward speed, metres per tick
    i32   seg;
    Fx    lateral;
    i32   lap;          // 0 until the start line is first crossed
    i32   checkpoints;  // bitmask of track quarters passed this lap
    i32   progress;     // ranking key within the race
    i32   finishTick;   // 0 = still racing
    i32   place;        // 1-based
    i32   driftDir;     // -1 / 0 / +1
    i32   driftCharge;
    i32   boostTicks;
    i32   spinTicks;
    i32   starTicks;
    i32   hopTicks;
    i32   bumpTicks;
    i32   draftTicks;  // slipstream charge
    i32   draftFlash;  // > 0 right after a slipstream boost fires
    i32   item;
    i32   rouletteTicks;
    i32   prevButtons;
    i32   isHuman;
    i32   skill;      // bots: 0..4
    i32   character;  // CharacterId: drives stats here and the model/paint in the renderer
};

struct Projectile {
    i32 kind;  // Item_GreenShell / Item_RedShell, 0 = free slot
    V2  pos;
    Fx  y;
    V2  vel;
    i32 seg;
    i32 owner;
    i32 target;  // red shells: kart index or -1
    i32 life;
    i32 bounces;
    i32 armTicks;
};

struct Hazard {
    i32 active;
    V2  pos;
    Fx  y;
    i32 owner;
    i32 armTicks;
};

struct ItemBox {
    V2  pos;
    Fx  y;
    i32 respawnTicks;  // 0 = available
};

struct SimState {
    i32        tick;
    i32        phase;
    i32        phaseTicks;
    Rng        rng;
    i32        numKarts;
    i32        numHumans;
    i32        laps;
    i32        finishedCount;
    i32        trackId;
    Kart       karts[kMaxKarts];
    Projectile projectiles[kMaxProjectiles];
    Hazard     hazards[kMaxHazards];
    ItemBox    boxes[kMaxItemBoxes];
    i32        numBoxes;
};

static_assert(std::has_unique_object_representations_v<SimState>,
              "SimState must be padding-free so snapshots and hashes are byte-exact");

// humanCharacters[numHumans] (nullptr = defaults). Bots take the characters
// nobody picked, in roster order.
void SimInit(SimState& s, const Track& t, u32 seed, int numHumans, int laps, const i32* humanCharacters = nullptr);
void SimTick(SimState& s, const Track& t, const PlayerInput* inputs);  // inputs[numHumans]
u64  SimHash(const SimState& s);

// Deterministic driver AI. Pure: reads state, mutates nothing. Used for bots
// inside the sim and as an autopilot for headless network tests.
PlayerInput BotInput(const SimState& s, const Track& t, int kartIndex);

// Binary angle of a direction vector (heading convention: 0 = +z, CW positive).
Angle AngleOf(V2 dir);

}  // namespace ss
