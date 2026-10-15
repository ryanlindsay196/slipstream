// rollback.h - rollback (GGPO-style) input prediction and resimulation.
//
// Every peer runs the full simulation. Remote inputs we don't have yet are
// predicted (repeat the last known input). When the real input arrives and
// differs from the prediction, we restore the snapshot from that tick and
// resimulate to the present. Local inputs are scheduled `inputDelay` ticks
// ahead, which hides small latencies without any rollback at all.
//
// This only works because SimTick is bit-exact deterministic and SimState is a
// flat blob: a snapshot is a memcpy and a desync check is a hash compare.
#pragma once

#include "game/sim.h"

namespace ss {

constexpr int kDefaultInputDelay = 2;
constexpr int kMaxPrediction     = 10;   // ticks we may run ahead of the slowest confirmed input
constexpr int kSnapshotRing      = 32;   // must exceed kMaxPrediction + inputDelay
constexpr int kInputRing         = 128;

struct RollbackStats {
    u32 rollbacks;
    u32 resimulatedTicks;
    u32 maxDepth;
    u32 lastDepth;
    u32 mispredictions;
    u32 stalls;
};

struct Rollback {
    const Track* track;
    SimState     state;  // present (possibly predicted)
    SimState     snapshots[kSnapshotRing];
    PlayerInput  inputs[kInputRing][kMaxPlayers];
    PlayerInput  used[kInputRing][kMaxPlayers];  // what the sim consumed for each tick
    i32          confirmed[kMaxPlayers];         // last tick with a known input, per player
    i32          numPlayers;
    i32          localPlayer;
    i32          inputDelay;
    i32          rollbackTo;  // earliest tick needing resimulation, or INT32_MAX
    RollbackStats stats;

    // Optional full input log (for offline verification and replays).
    PlayerInput* history;
    i32          historyCapacity;

    void Init(const Track* t, u32 seed, int players, int local, int laps, int delay, const i32* characters = nullptr);
    i32  CurrentTick() const { return state.tick; }
    i32  MinConfirmed() const;
    bool CanAdvance() const { return CurrentTick() - MinConfirmed() < kMaxPrediction; }

    void AddLocalInput(PlayerInput in);  // schedules for CurrentTick() + inputDelay
    // Accepts only the next contiguous tick for `player`; returns false on a gap
    // (the sender re-sends redundantly, so the gap fills on a later packet).
    bool AddRemoteInput(int player, i32 tick, PlayerInput in);
    PlayerInput InputFor(int player, i32 tick) const;  // confirmed or predicted

    void Advance();  // resimulate if a misprediction was found, then step one tick
    void Settle();   // apply any pending correction without stepping forward

    // Hash of the state at the *start* of `tick`, available once every input
    // before it is confirmed. Used for cross-peer desync detection.
    bool ConfirmedHash(i32 tick, u64* out) const;
};

}  // namespace ss
