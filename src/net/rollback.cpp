#include "net/rollback.h"

#include <climits>

namespace ss {

void Rollback::Init(const Track* t, u32 seed, int players, int local, int laps, int delay, const i32* characters) {
    PlayerInput* keepHistory = history;
    i32          keepCap     = historyCapacity;
    memset((void*)this, 0, sizeof(*this));
    history         = keepHistory;
    historyCapacity = keepCap;

    track       = t;
    numPlayers  = players;
    localPlayer = local;
    inputDelay  = delay;
    rollbackTo  = INT32_MAX;
    SimInit(state, *t, seed, players, laps, characters);
    // The first `delay` ticks have neutral input for everyone, so every peer
    // agrees on them without any network traffic.
    for (int p = 0; p < kMaxPlayers; ++p) confirmed[p] = delay - 1;
}

i32 Rollback::MinConfirmed() const {
    i32 m = INT32_MAX;
    for (int p = 0; p < numPlayers; ++p) m = Min(m, confirmed[p]);
    return m;
}

void Rollback::AddLocalInput(PlayerInput in) {
    i32 t = confirmed[localPlayer] + 1;
    if (t > CurrentTick() + inputDelay) return;  // already scheduled this far ahead
    inputs[t % kInputRing][localPlayer] = in;
    confirmed[localPlayer]              = t;
    if (history && t < historyCapacity) history[t * kMaxPlayers + localPlayer] = in;
}

bool Rollback::AddRemoteInput(int player, i32 tick, PlayerInput in) {
    if (player < 0 || player >= numPlayers || player == localPlayer) return false;
    if (tick <= confirmed[player]) return true;  // duplicate from a redundant resend
    if (tick != confirmed[player] + 1) return false;
    if (tick >= CurrentTick() + kInputRing / 2) return false;  // would overrun the ring

    inputs[tick % kInputRing][player] = in;
    confirmed[player]                 = tick;
    if (history && tick < historyCapacity) history[tick * kMaxPlayers + player] = in;

    // Already simulated this tick with a guess? If the guess was wrong, the
    // present is invalid from here on.
    if (tick < CurrentTick() && used[tick % kInputRing][player] != in) {
        rollbackTo = Min(rollbackTo, tick);
        ++stats.mispredictions;
    }
    return true;
}

PlayerInput Rollback::InputFor(int player, i32 tick) const {
    i32 known = Min(tick, confirmed[player]);
    if (known < 0) return {};
    return inputs[known % kInputRing][player];
}

namespace {
void Step(Rollback& r) {
    i32 t                          = r.state.tick;
    r.snapshots[t % kSnapshotRing] = r.state;
    PlayerInput in[kMaxPlayers]    = {};
    for (int p = 0; p < r.numPlayers; ++p) in[p] = r.used[t % kInputRing][p] = r.InputFor(p, t);
    SimTick(r.state, *r.track, in);
}
}  // namespace

void Rollback::Settle() {
    if (rollbackTo < CurrentTick()) {
        i32 target = CurrentTick();
        i32 depth  = target - rollbackTo;
        SS_ASSERT(depth < kSnapshotRing, "rollback deeper than the snapshot ring");
        state = snapshots[rollbackTo % kSnapshotRing];
        SS_ASSERT(state.tick == rollbackTo, "snapshot ring returned the wrong tick");
        while (state.tick < target) Step(*this);
        ++stats.rollbacks;
        stats.resimulatedTicks += (u32)depth;
        stats.lastDepth = (u32)depth;
        stats.maxDepth  = Max(stats.maxDepth, (u32)depth);
    }
    rollbackTo = INT32_MAX;
}

void Rollback::Advance() {
    Settle();
    Step(*this);
}

bool Rollback::ConfirmedHash(i32 tick, u64* out) const {
    if (tick > CurrentTick() || tick <= CurrentTick() - kSnapshotRing) return false;
    if (tick - 1 > MinConfirmed() || rollbackTo < tick) return false;
    const SimState& s = tick == CurrentTick() ? state : snapshots[tick % kSnapshotRing];
    if (s.tick != tick) return false;
    *out = SimHash(s);
    return true;
}

}  // namespace ss
