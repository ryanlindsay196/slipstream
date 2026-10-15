#include "net/session.h"
#include "game/characters.h"
#include "game/tracks.h"

#include <climits>

namespace ss {

namespace {

constexpr u16 kMagic          = 0x5353;  // "SS"
constexpr u8  kProtocol       = 2;  // v2: characters and track selection
constexpr u32 kTimeoutMs      = 5000;
constexpr u32 kConnectTimeout = 8000;
constexpr u32 kResendMs       = 200;
constexpr int kMaxBlockInputs = 40;
constexpr int kMaxPacket      = 600;

enum MsgType : u8 { Msg_Hello = 1, Msg_Welcome, Msg_Lobby, Msg_Start, Msg_StartAck, Msg_Input, Msg_Checksum, Msg_Leave };

// Explicit little-endian wire format: no struct dumps, no host byte order.
struct Writer {
    u8   buf[kMaxPacket];
    int  pos = 0;
    bool ok  = true;
    void U8(u8 v) {
        if (pos >= kMaxPacket) { ok = false; return; }
        buf[pos++] = v;
    }
    void U16(u16 v) { U8((u8)v); U8((u8)(v >> 8)); }
    void U32(u32 v) { U16((u16)v); U16((u16)(v >> 16)); }
    void I32(i32 v) { U32((u32)v); }
    void U64(u64 v) { U32((u32)v); U32((u32)(v >> 32)); }
    void Name(const char* s) { for (int i = 0; i < kNameLen; ++i) U8(i < (int)strlen(s) ? (u8)s[i] : 0); }
};

struct Reader {
    const u8* p;
    int       size;
    int       pos = 0;
    bool      ok  = true;
    u8 U8() {
        if (pos >= size) { ok = false; return 0; }
        return p[pos++];
    }
    u16 U16() { u16 lo = U8(); return (u16)(lo | (U8() << 8)); }
    u32 U32() { u32 lo = U16(); return lo | ((u32)U16() << 16); }
    i32 I32() { return (i32)U32(); }
    u64 U64() { u64 lo = U32(); return lo | ((u64)U32() << 32); }
    void Name(char* out) {
        for (int i = 0; i < kNameLen; ++i) {
            u8 c   = U8();
            out[i] = (c >= 32 && c < 127) ? (char)c : 0;  // untrusted: printable ASCII only
        }
        out[kNameLen - 1] = 0;
    }
};

void Begin(Writer& w, MsgType t) {
    w.U16(kMagic);
    w.U8(kProtocol);
    w.U8(t);
}

u32 NextRand(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

void Send(NetSession& n, plat::NetAddress to, const Writer& w, u32 nowMs) {
    if (!w.ok) return;
    n.bytesSent += (u32)w.pos;
    ++n.packetsSent;
    const NetConditions& c = n.conditions;
    if (!c.latencyMs && !c.jitterMs && !c.lossPercent) {
        plat::UdpSend(n.socket, to, w.buf, w.pos);
        return;
    }
    if (NextRand(n.simRng) % 100 < c.lossPercent) {
        ++n.packetsDropped;
        return;
    }
    if (n.delayCount == (int)SS_COUNTOF(n.delayQueue)) {
        plat::UdpSend(n.socket, to, w.buf, w.pos);
        return;
    }
    DelayedPacket& d = n.delayQueue[n.delayCount++];
    d.sendAt         = nowMs + c.latencyMs + (c.jitterMs ? NextRand(n.simRng) % (c.jitterMs + 1) : 0);
    d.to             = to;
    d.size           = (u16)w.pos;
    memcpy(d.data, w.buf, (size_t)w.pos);
}

void FlushDelayed(NetSession& n, u32 nowMs) {
    for (int i = 0; i < n.delayCount;) {
        DelayedPacket& d = n.delayQueue[i];
        if ((i32)(nowMs - d.sendAt) >= 0) {
            plat::UdpSend(n.socket, d.to, d.data, d.size);
            d = n.delayQueue[--n.delayCount];  // swap-remove; jitter may reorder, like the internet
        } else {
            ++i;
        }
    }
}

NetPeer* FindPeer(NetSession& n, plat::NetAddress a) {
    for (NetPeer& p : n.peers)
        if (p.used && p.addr == a) return &p;
    return nullptr;
}

void SendLobby(NetSession& n, u32 nowMs) {
    Writer w;
    Begin(w, Msg_Lobby);
    int count = NetConnectedCount(n);
    w.U8((u8)count);
    w.Name(n.localName);
    w.U8((u8)n.localCharacter);
    for (const NetPeer& p : n.peers)
        if (p.used) {
            w.Name(p.name);
            w.U8((u8)p.character);
        }
    w.U8((u8)n.trackId);
    for (const NetPeer& p : n.peers)
        if (p.used) Send(n, p.addr, w, nowMs);
}

void SendStart(NetSession& n, NetPeer& p, u32 nowMs) {
    Writer w;
    Begin(w, Msg_Start);
    w.U32(n.seed);
    w.U8((u8)n.numPlayers);
    w.U8((u8)p.slot);
    w.U8((u8)n.laps);
    w.U8((u8)n.trackId);
    for (int i = 0; i < n.numPlayers; ++i) {
        w.Name(n.playerNames[i]);
        w.U8((u8)n.playerCharacters[i]);
    }
    Send(n, p.addr, w, nowMs);
}

void SendInputs(NetSession& n, NetPeer& peer, u32 nowMs) {
    Rollback& rb = *n.rb;
    Writer    w;
    Begin(w, Msg_Input);
    w.I32(rb.CurrentTick());
    w.U32(nowMs);
    w.U32(peer.echoMs);
    w.U32(peer.echoMs ? nowMs - peer.echoRecvAt : 0);

    int blockCountPos = w.pos;
    w.U8(0);
    u8 blocks = 0;
    for (int p = 0; p < rb.numPlayers; ++p) {
        // Host forwards everyone but the receiver; a client sends only itself.
        if (n.isHost ? p == peer.slot : p != n.localSlot) continue;
        i32 from = Max(peer.ackedBy[p] + 1, rb.confirmed[p] - (kInputRing / 2));
        i32 to   = Min(rb.confirmed[p], from + kMaxBlockInputs - 1);
        if (to < from) continue;
        w.U8((u8)p);
        w.I32(from);
        w.U8((u8)(to - from + 1));
        for (i32 t = from; t <= to; ++t) {
            PlayerInput in = rb.inputs[t % kInputRing][p];
            w.U8((u8)in.steer);
            w.U8(in.buttons);
        }
        ++blocks;
    }
    w.buf[blockCountPos] = blocks;

    // Acks: tell the receiver how far we've confirmed the inputs it sends us.
    int ackCountPos = w.pos;
    w.U8(0);
    u8 acks = 0;
    for (int p = 0; p < rb.numPlayers; ++p) {
        if (n.isHost ? p != peer.slot : p == n.localSlot) continue;
        w.U8((u8)p);
        w.I32(rb.confirmed[p]);
        ++acks;
    }
    w.buf[ackCountPos] = acks;
    Send(n, peer.addr, w, nowMs);
}

void HandleInput(NetSession& n, NetPeer& peer, Reader& r, u32 nowMs) {
    i32 senderTick = r.I32();
    u32 sendMs     = r.U32();
    u32 echoMs     = r.U32();
    u32 echoDelay  = r.U32();
    if (!r.ok) return;
    peer.remoteTick     = senderTick;
    peer.remoteTickAtMs = nowMs;
    peer.echoMs         = sendMs;
    peer.echoRecvAt     = nowMs;
    if (echoMs) {
        u32 rtt = nowMs - echoMs - echoDelay;
        if (rtt < 5000) peer.rttMs = peer.rttMs ? (peer.rttMs * 7 + rtt) / 8 : rtt;
    }

    Rollback* rb     = n.rb;
    int       blocks = r.U8();
    for (int b = 0; b < blocks && r.ok; ++b) {
        int player = r.U8();
        i32 first  = r.I32();
        int count  = r.U8();
        bool allowed = player < kMaxPlayers && (!n.isHost || player == peer.slot);  // clients can't spoof others
        for (int k = 0; k < count && r.ok; ++k) {
            PlayerInput in{(i8)r.U8(), r.U8()};
            if (r.ok && allowed && rb) rb->AddRemoteInput(player, first + k, in);
        }
    }
    int acks = r.U8();
    for (int a = 0; a < acks && r.ok; ++a) {
        int player = r.U8();
        i32 tick   = r.I32();
        if (r.ok && player < kMaxPlayers) peer.ackedBy[player] = Max(peer.ackedBy[player], tick);
    }
}

void CompareChecksum(NetSession& n, i32 tick, u64 remote) {
    int slot = (tick / kChecksumInterval) % 16;
    if (n.ownChecksumTick[slot] == tick) {
        u64 local = n.ownChecksumHash[slot];
        if (local == remote) ++n.checksumsMatched;
        else if (!n.desynced) {
            n.desynced   = true;
            n.desyncTick = tick;
        }
        return;
    }
    // Ours isn't computed yet: park it until it is.
    n.remoteChecksumTick[slot]  = tick;
    n.remoteChecksumHash[slot]  = remote;
}

void HandlePacket(NetSession& n, plat::NetAddress from, const u8* data, int size, u32 nowMs) {
    Reader r{data, size};
    if (r.U16() != kMagic || r.U8() != kProtocol || !r.ok) return;
    u8       type = r.U8();
    NetPeer* peer = FindPeer(n, from);
    if (peer) peer->lastRecvMs = nowMs;

    if (n.isHost) {
        if (type == Msg_Hello && !peer && n.phase == NetPhase::Lobby) {
            for (NetPeer& p : n.peers) {
                if (p.used || NetConnectedCount(n) >= kMaxPlayers) continue;
                p            = {};
                p.used       = true;
                p.addr       = from;
                p.lastRecvMs = nowMs;
                r.Name(p.name);
                if (!p.name[0]) strcpy(p.name, "PLAYER");
                p.character = Min((int)r.U8(), Char_Count - 1);
                peer = &p;
                break;
            }
        }
        if (!peer) return;
        switch (type) {
            case Msg_Hello: {
                if (n.phase == NetPhase::Lobby) {  // keep-alives also carry the current pick
                    char ignored[kNameLen];
                    Reader again{data, size};
                    again.pos = 4;
                    again.Name(ignored);
                    u8 c = again.U8();
                    if (again.ok) peer->character = Min((int)c, Char_Count - 1);
                }
                Writer w;
                Begin(w, Msg_Welcome);
                Send(n, from, w, nowMs);
                break;
            }
            case Msg_StartAck: peer->startAcked = true; break;
            case Msg_Input:    HandleInput(n, *peer, r, nowMs); break;
            case Msg_Checksum: {
                i32 tick = r.I32();
                u64 hash = r.U64();
                if (r.ok) CompareChecksum(n, tick, hash);
                break;
            }
            case Msg_Leave:
                if (n.phase == NetPhase::Lobby) peer->used = false;
                else { n.phase = NetPhase::Disconnected; n.error = "A PLAYER LEFT THE RACE"; }
                break;
        }
        return;
    }

    // Client: only the host may talk to us.
    if (!peer) return;
    switch (type) {
        case Msg_Welcome:
            if (n.phase == NetPhase::Connecting) n.phase = NetPhase::Lobby;
            break;
        case Msg_Lobby: {
            int count = r.U8();
            for (int i = 0; i < count && i < kMaxPlayers; ++i) {
                r.Name(n.playerNames[i]);
                n.playerCharacters[i] = Min((int)r.U8(), Char_Count - 1);
            }
            u8 track = r.U8();
            if (r.ok) {
                n.numPlayers = Min(count, kMaxPlayers);
                n.trackId    = Min((int)track, kTrackCount - 1);
            }
            if (n.phase == NetPhase::Connecting) n.phase = NetPhase::Lobby;
            break;
        }
        case Msg_Start: {
            u32 seed = r.U32();
            int players = r.U8(), slot = r.U8(), laps = r.U8(), track = r.U8();
            char names[kMaxPlayers][kNameLen] = {};
            i32  chars[kMaxPlayers]           = {};
            for (int i = 0; i < players && i < kMaxPlayers; ++i) {
                r.Name(names[i]);
                chars[i] = Min((int)r.U8(), Char_Count - 1);
            }
            if (!r.ok || players < 1 || players > kMaxPlayers || slot >= players || laps < 1 || laps > 9 ||
                track >= kTrackCount)
                return;
            if (n.phase == NetPhase::Connecting || n.phase == NetPhase::Lobby) {
                n.seed       = seed;
                n.numPlayers = players;
                n.localSlot  = slot;
                n.laps       = laps;
                n.trackId    = track;
                memcpy(n.playerNames, names, sizeof(names));
                memcpy(n.playerCharacters, chars, sizeof(chars));
                n.phase = NetPhase::InGame;
            }
            Writer w;  // ack every copy: the host keeps resending until it hears one
            Begin(w, Msg_StartAck);
            w.U8((u8)slot);
            Send(n, from, w, nowMs);
            break;
        }
        case Msg_Input:
            if (n.phase == NetPhase::InGame) HandleInput(n, *peer, r, nowMs);
            break;
        case Msg_Checksum: {
            i32 tick = r.I32();
            u64 hash = r.U64();
            if (r.ok) CompareChecksum(n, tick, hash);
            break;
        }
        case Msg_Leave:
            n.phase = NetPhase::Disconnected;
            n.error = "HOST CLOSED THE SESSION";
            break;
    }
}

}  // namespace

bool NetHost(NetSession& n, u16 port, const char* name, i32 character, i32 trackId) {
    NetConditions keep = n.conditions;
    bool          loop = n.loopbackOnly;
    memset((void*)&n, 0, sizeof(n));
    n.conditions       = keep;
    n.loopbackOnly     = loop;
    n.simRng           = 0x1234567u ^ plat::NowMs();
    n.socket           = plat::UdpOpen(port, loop);
    if (n.socket == plat::kInvalidSocket) {
        n.error = "COULD NOT OPEN PORT";
        return false;
    }
    n.isHost         = true;
    n.localCharacter = character;
    n.trackId        = trackId;
    n.phase  = NetPhase::Lobby;
    strncpy(n.localName, name, kNameLen - 1);
    n.numPlayers = 1;
    return true;
}

bool NetJoin(NetSession& n, plat::NetAddress host, const char* name, i32 character) {
    NetConditions keep = n.conditions;
    bool          loop = n.loopbackOnly;
    memset((void*)&n, 0, sizeof(n));
    n.conditions       = keep;
    n.loopbackOnly     = loop;
    n.simRng           = 0x7654321u ^ plat::NowMs();
    n.socket           = plat::UdpOpen(0, loop);
    if (n.socket == plat::kInvalidSocket) {
        n.error = "COULD NOT OPEN SOCKET";
        return false;
    }
    n.isHost          = false;
    n.localCharacter  = character;
    n.phase           = NetPhase::Connecting;
    n.phaseStartMs    = 0;  // stamped on the first poll, in the caller's clock
    n.peers[0].used   = true;
    n.peers[0].addr   = host;
    n.peers[0].slot   = 0;
    strncpy(n.localName, name, kNameLen - 1);
    return true;
}

void NetClose(NetSession& n) {
    if (n.socket == plat::kInvalidSocket || !n.socket) return;
    Writer w;
    Begin(w, Msg_Leave);
    for (const NetPeer& p : n.peers)
        if (p.used) plat::UdpSend(n.socket, p.addr, w.buf, w.pos);
    plat::UdpClose(n.socket);
    n.socket = plat::kInvalidSocket;
    n.phase  = NetPhase::Idle;
}

void NetPoll(NetSession& n, u32 nowMs) {
    if (n.phase == NetPhase::Idle) return;
    FlushDelayed(n, nowMs);
    u8               buf[2048];
    plat::NetAddress from;
    for (int guard = 0; guard < 512; ++guard) {
        int size = plat::UdpRecv(n.socket, &from, buf, sizeof(buf));
        if (size < 0) break;
        n.bytesReceived += (u32)size;
        ++n.packetsReceived;
        HandlePacket(n, from, buf, size, nowMs);
    }

    // Timeouts.
    if (n.phase == NetPhase::Connecting && !n.phaseStartMs) n.phaseStartMs = nowMs;
    if (n.phase == NetPhase::Connecting && nowMs - n.phaseStartMs > kConnectTimeout) {
        n.phase = NetPhase::Disconnected;
        n.error = "NO RESPONSE FROM HOST";
    }
    for (NetPeer& p : n.peers) {
        if (!p.used || n.phase == NetPhase::Connecting || nowMs - p.lastRecvMs < kTimeoutMs) continue;
        if (n.isHost && n.phase == NetPhase::Lobby) {
            p.used = false;
        } else if (n.phase != NetPhase::Disconnected) {
            n.phase = NetPhase::Disconnected;
            n.error = n.isHost ? "A PLAYER TIMED OUT" : "LOST CONNECTION TO HOST";
        }
    }
}

void NetSendFrame(NetSession& n, u32 nowMs) {
    switch (n.phase) {
        case NetPhase::Connecting:
            if (nowMs - n.lastLobbySendMs >= kResendMs) {
                n.lastLobbySendMs = nowMs;
                Writer w;
                Begin(w, Msg_Hello);
                w.Name(n.localName);
                w.U8((u8)n.localCharacter);
                Send(n, n.peers[0].addr, w, nowMs);
            }
            break;
        case NetPhase::Lobby:
            if (nowMs - n.lastLobbySendMs >= kResendMs) {
                n.lastLobbySendMs = nowMs;
                if (n.isHost) SendLobby(n, nowMs);
                else {  // keep-alive so the host doesn't drop us
                    Writer w;
                    Begin(w, Msg_Hello);
                    w.Name(n.localName);
                    w.U8((u8)n.localCharacter);
                    Send(n, n.peers[0].addr, w, nowMs);
                }
            }
            break;
        case NetPhase::Starting: {
            bool all = true;
            for (NetPeer& p : n.peers) {
                if (!p.used || p.startAcked) continue;
                all = false;
                SendStart(n, p, nowMs);
            }
            if (all) n.phase = NetPhase::InGame;
            else if (nowMs - n.phaseStartMs > kTimeoutMs) {
                n.phase = NetPhase::Disconnected;
                n.error = "A PLAYER FAILED TO START";
            }
            break;
        }
        case NetPhase::InGame: {
            if (!n.rb) break;
            for (NetPeer& p : n.peers)
                if (p.used) SendInputs(n, p, nowMs);

            // Exchange hashes of fully-confirmed states.
            while (n.nextChecksumTick <= n.rb->CurrentTick()) {
                i32 tick = n.nextChecksumTick;
                u64 hash;
                if (tick <= n.rb->CurrentTick() - kSnapshotRing) {
                    n.nextChecksumTick += kChecksumInterval;  // scrolled out; skip
                    continue;
                }
                if (!n.rb->ConfirmedHash(tick, &hash)) break;
                Writer w;
                Begin(w, Msg_Checksum);
                w.I32(tick);
                w.U64(hash);
                for (NetPeer& p : n.peers)
                    if (p.used) Send(n, p.addr, w, nowMs);
                int slot                 = (tick / kChecksumInterval) % 16;
                n.ownChecksumTick[slot]  = tick;
                n.ownChecksumHash[slot]  = hash;
                if (n.remoteChecksumTick[slot] == tick) CompareChecksum(n, tick, n.remoteChecksumHash[slot]);
                n.nextChecksumTick += kChecksumInterval;
            }
            break;
        }
        default: break;
    }
    FlushDelayed(n, nowMs);
}

void NetHostBeginRace(NetSession& n, u32 seed, int laps, u32 nowMs) {
    n.seed       = seed;
    n.laps       = laps;
    n.numPlayers = 1;
    n.localSlot  = 0;
    strncpy(n.playerNames[0], n.localName, kNameLen - 1);
    n.playerCharacters[0] = n.localCharacter;
    for (NetPeer& p : n.peers) {
        if (!p.used) continue;
        p.slot       = n.numPlayers++;
        p.startAcked = false;
        strncpy(n.playerNames[p.slot], p.name, kNameLen - 1);
        n.playerCharacters[p.slot] = p.character;
    }
    n.phase        = NetPhase::Starting;
    n.phaseStartMs = nowMs;
}

void NetAttachRollback(NetSession& n, Rollback* rb) {
    n.rb               = rb;
    n.nextChecksumTick = kChecksumInterval;
    for (NetPeer& p : n.peers)
        for (int i = 0; i < kMaxPlayers; ++i) p.ackedBy[i] = rb->inputDelay - 1;
}

float NetFrameAdvantage(const NetSession& n, u32 nowMs) {
    if (!n.rb) return 0;
    float worst = -1e9f;
    for (const NetPeer& p : n.peers) {
        if (!p.used || !p.remoteTickAtMs) continue;
        float remoteNow = (float)p.remoteTick + (float)(nowMs - p.remoteTickAtMs + p.rttMs / 2) * (kTickRate / 1000.0f);
        worst           = Max(worst, (float)n.rb->CurrentTick() - remoteNow);
    }
    return worst < -1e8f ? 0 : worst;
}

u32 NetWorstRtt(const NetSession& n) {
    u32 worst = 0;
    for (const NetPeer& p : n.peers)
        if (p.used) worst = Max(worst, p.rttMs);
    return worst;
}

int NetConnectedCount(const NetSession& n) {
    int count = 1;
    for (const NetPeer& p : n.peers)
        if (p.used) ++count;
    return n.isHost ? count : Max(count, n.numPlayers);
}

}  // namespace ss
