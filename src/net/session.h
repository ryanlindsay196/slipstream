// session.h - UDP multiplayer session: lobby, input relay, time sync, desync
// detection, and an optional network condition simulator.
//
// Topology: host-relayed star. Clients send their inputs to the host; the host
// forwards everyone's inputs to everyone. Each packet carries every input the
// receiver hasn't acknowledged yet, so lost packets cost nothing but a little
// latency, with no retransmission timers. Every peer runs the full simulation under
// rollback, so the host has no authority over results; it is only a relay and
// lobby. All peers exchange state hashes to detect desyncs.
#pragma once

#include "net/rollback.h"
#include "platform/platform.h"

namespace ss {

constexpr int kNameLen          = 16;
constexpr u16 kDefaultPort      = 7777;
constexpr int kChecksumInterval = 60;

enum class NetPhase { Idle, Connecting, Lobby, Starting, InGame, Disconnected };

struct NetConditions {  // simulated network, applied to outgoing packets
    u32 latencyMs;  // one-way
    u32 jitterMs;
    u32 lossPercent;
};

struct NetPeer {
    bool            used;
    plat::NetAddress addr;
    char            name[kNameLen];
    i32             character;
    i32             slot;
    u32             lastRecvMs;
    i32             ackedBy[kMaxPlayers];  // per player: last input tick this peer has confirmed
    i32             remoteTick;
    u32             remoteTickAtMs;
    u32             rttMs;
    u32             echoMs, echoRecvAt;  // their timestamp, for RTT
    bool            startAcked;
};

struct DelayedPacket {
    u32             sendAt;
    plat::NetAddress to;
    u16             size;
    u8              data[600];
};

struct NetSession {
    plat::Socket socket;
    bool         isHost;
    NetPhase     phase;
    const char*  error;

    NetPeer peers[kMaxPlayers];  // host: by join order; client: peers[0] is the host
    char    localName[kNameLen];
    char    playerNames[kMaxPlayers][kNameLen];
    i32     playerCharacters[kMaxPlayers];
    i32     localCharacter;
    i32     trackId;  // the host picks; clients learn it from the lobby
    int     numPlayers;
    int     localSlot;
    u32     seed;
    int     laps;
    u32     lastLobbySendMs;
    u32     phaseStartMs;

    Rollback* rb;  // attached once the race starts

    // Desync detection.
    i32  nextChecksumTick;
    i32  ownChecksumTick[16];  // our recent confirmed hashes, kept because a
    u64  ownChecksumHash[16];  // remote one may arrive long after the snapshot is gone
    i32  remoteChecksumTick[16];
    u64  remoteChecksumHash[16];
    bool desynced;
    i32  desyncTick;
    i32  checksumsMatched;

    // Simulated network conditions.
    NetConditions  conditions;
    bool           loopbackOnly;  // bind 127.0.0.1 only (tests: no firewall prompt)
    DelayedPacket  delayQueue[256];
    int            delayCount;
    u32            simRng;

    // Stats.
    u32 bytesSent, bytesReceived, packetsSent, packetsReceived, packetsDropped;
};

bool NetHost(NetSession& n, u16 port, const char* name, i32 character, i32 trackId);
bool NetJoin(NetSession& n, plat::NetAddress host, const char* name, i32 character);
void NetClose(NetSession& n);

// Receive and process everything pending. `nowMs` may be virtual time (tests).
void NetPoll(NetSession& n, u32 nowMs);
// Send this frame's traffic: lobby updates, start handshakes, inputs, checksums.
void NetSendFrame(NetSession& n, u32 nowMs);

// Host: lock the lobby and start the race with everyone currently connected.
void NetHostBeginRace(NetSession& n, u32 seed, int laps, u32 nowMs);
void NetAttachRollback(NetSession& n, Rollback* rb);

// > 0 when we are ahead of the remote(s) in simulated ticks: we should wait.
float NetFrameAdvantage(const NetSession& n, u32 nowMs);
u32   NetWorstRtt(const NetSession& n);
int   NetConnectedCount(const NetSession& n);  // including ourselves

}  // namespace ss
