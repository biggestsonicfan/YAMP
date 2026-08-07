// yampnet.dll - the netplay plugin.
//
// Implements the YampNet ABI on top of the lockstep engine (Lockstep.cpp), the pad codec
// (PadCodec.cpp) and a datagram transport (Transport.cpp).
//
// Modelled on the Sonic the Fighters PS3 netcode: delay-based lockstep, inputs keyed by absolute
// frame into per-player rings, every packet re-carrying the last N frames so loss repairs itself,
// a round barrier that resets per-round state, and a single shared match seed so the emulated
// ROM's `rand` produces identical values on both machines.
//
// TRANSPORT: RPCN (RpcnTransport). connect() logs in and runs discovery, create_room()/join_room()
// place us in a room, and the guest resolves the host through the signaling helper before sending
// first - the host learns the guest's address from that first datagram. Transport.h's UdpTransport
// remains as a dependency-free fallback for LAN testing; swapping it back changes only the member
// type in yampnet_session, because both satisfy ITransport.

#include "YampNet.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "Lockstep.h"
#include "PadCodec.h"
#include "RpcnTransport.h"
#include "Transport.h"
#include "m2ftg/m2ftg.h"

namespace
{
    yampnet_layout BuiltLayout()
    {
        using m2ftg::m2ftg_execute_info_t;
        using m2ftg::m2ftg_pad_t;

        yampnet_layout l = {};
        l.execute_info_size = static_cast<uint32_t>(sizeof(m2ftg_execute_info_t));
        l.pad_size = static_cast<uint32_t>(sizeof(m2ftg_pad_t));
        l.pad0_offset = static_cast<uint32_t>(offsetof(m2ftg_execute_info_t, pad));
        l.pad1_offset = static_cast<uint32_t>(offsetof(m2ftg_execute_info_t, pad[1]));
        l.pad_buttons_offset = static_cast<uint32_t>(offsetof(m2ftg_pad_t, m_buttons));
        l.pad_port_offset = static_cast<uint32_t>(offsetof(m2ftg_pad_t, m_port));
        return l;
    }

    // `host` is what YAMP declared; `built` is what this plugin was compiled against.
    //
    // The four PAD fields are compared exactly - the plugin dereferences through every one of
    // them, so a difference is memory corruption waiting to happen. execute_info_size is NOT,
    // and deliberately: since ABI 7 the host declares the SMALLEST execute_info it may pass
    // (YAMP now hosts two arcade emulators whose blocks are 0x1760 and 0x1780 and whose pads
    // coincide), so the only thing this plugin needs from it is that the pads fit. Testing it
    // for equality would refuse the larger family for a field the plugin never reads.
    bool LayoutMatches(const yampnet_layout& host, const yampnet_layout& built)
    {
        if (host.pad_size != built.pad_size
            || host.pad0_offset != built.pad0_offset
            || host.pad1_offset != built.pad1_offset
            || host.pad_buttons_offset != built.pad_buttons_offset
            || host.pad_port_offset != built.pad_port_offset)
        {
            return false;
        }
        // Every byte this plugin writes lies inside pad[1], so this is the whole of its reach.
        return host.execute_info_size >= host.pad1_offset + host.pad_size;
    }
}

struct yampnet_session
{
    yampnet_host host = {};
    yampnet_state state = YAMPNET_STATE_IDLE;
    char error[256] = {};

    yampnet::RpcnTransport transport;
    yampnet::Lockstep lockstep;
    yampnet::PadHistory pads[yampnet::kMaxPlayers];

    int32_t local_player = -1;
    uint32_t match_seed = 0;
    uint32_t generation = 0;

    // --- State-check rings (desync detection) ---------------------------------------------
    // Two small ring buffers keyed by frame: what WE computed, and what the peer sent. They are
    // separate because either side can arrive first - the peer may still be a frame or two behind
    // when its check for frame N shows up, or ahead of us when ours does.
    struct CheckRing
    {
        static constexpr uint32_t kSize = 256;   // frames of history; peers are never this far apart
        uint32_t frames[kSize] = {};
        uint32_t values[kSize] = {};
        bool     valid[kSize] = {};

        void Clear() { for (uint32_t i = 0; i < kSize; ++i) valid[i] = false; }
        void Put(uint32_t frame, uint32_t value)
        {
            const uint32_t i = frame % kSize;
            frames[i] = frame; values[i] = value; valid[i] = true;
        }
        bool Get(uint32_t frame, uint32_t* out) const
        {
            const uint32_t i = frame % kSize;
            if (!valid[i] || frames[i] != frame)
                return false;
            *out = values[i];
            return true;
        }
    };
    CheckRing local_checks;
    CheckRing remote_checks;
    uint32_t last_check_frame = yampnet::kNoCheck;   // most recent local check, sent on every packet
    uint32_t last_check_value = 0;
    // Baseline difference between the two counters, learned on the first comparable frame.
    uint32_t check_baseline = 0;
    bool check_baseline_set = false;
    // Latched first disagreement. desync_frame == kNoCheck means "still in agreement".
    uint32_t desync_frame = yampnet::kNoCheck;
    uint32_t desync_local = 0;
    uint32_t desync_remote = 0;
    yampnet_match_config match = {};
    uint64_t wait_since_ms = 0;      // when the current stall began; 0 = not stalling
    uint64_t last_wait_report_ms = 0;
    uint64_t last_seed_send_ms = 0;  // host's IN_ROOM seed heartbeat
    bool seeded_delay_frames = false;
    bool room_logged = false;

    // ---- Linked-cabinet channel (kPacketLink) --------------------------------------------
    //
    // ONE slot, newest wins, and that is the correct policy rather than a simplification: the
    // payload is a complete snapshot of the peer cabinet every frame, so an older datagram says
    // nothing a newer one does not, and queueing them would feed the emulated hardware stale
    // state one frame at a time. Deliberately independent of round state, generation and the
    // input rings - two cabinets link during their boot-time network check and stay linked
    // between matches, which is long before and long after any round exists.
    uint8_t link_in[yampnet::kLinkPayloadMax] = {};
    uint32_t link_in_len = 0;
    bool link_have = false;
    uint32_t link_rx = 0;    // counters, for the log only
    uint32_t link_tx = 0;

    void Log(yampnet_log_level level, const char* fmt, ...)
    {
        if (!host.log)
            return;
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        host.log(host.log_ctx, level, buf);
    }

    void Fail(const char* msg)
    {
        strncpy_s(error, msg, _TRUNCATE);
        state = YAMPNET_STATE_FAILED;
        Log(YAMPNET_LOG_ERROR, "%s", msg);
    }

    bool IsHost() const { return local_player == 0; }

    // Low 32 bits of the room id, stamped on every datagram so traffic from another room (or a
    // leftover process still playing a previous one) is identifiable and droppable.
    uint32_t SessionId() const { return static_cast<uint32_t>(transport.RoomId()); }

    void SendAnnounce()
    {
        yampnet::AnnouncePacket pkt = {};
        pkt.header.type = yampnet::kPacketAnnounce;
        pkt.header.player = static_cast<uint8_t>(local_player < 0 ? 0 : local_player);
        pkt.header.generation = static_cast<uint8_t>(generation & 0x1F);
        pkt.header.session = SessionId();
        pkt.seed = IsHost() ? match_seed : 0;   // only the host's seed is authoritative
        transport.Send(0, &pkt, sizeof(pkt));
    }

    // Publish the match seed without touching the barrier. Host only - a guest has nothing
    // authoritative to say - and sent from IN_ROOM onwards so a guest holds the seed BEFORE it
    // starts preparing a round, whichever player presses Start first.
    //
    // Same payload as an announce, different type, and that distinction is the whole point: the
    // receiver feeds announces to Lockstep::OnPeerAnnounce, and a heartbeat that did the same
    // could release a barrier for a round this peer has not prepared. Generation is stamped only
    // so the packet is self-describing; nothing reads it.
    void SendSeed()
    {
        if (!IsHost())
            return;
        yampnet::AnnouncePacket pkt = {};
        pkt.header.type = yampnet::kPacketSeed;
        pkt.header.player = static_cast<uint8_t>(local_player < 0 ? 0 : local_player);
        pkt.header.generation = static_cast<uint8_t>(generation & 0x1F);
        pkt.header.session = SessionId();
        pkt.seed = match_seed;
        transport.Send(0, &pkt, sizeof(pkt));
    }
};

namespace
{
    using namespace yampnet;

    // How often the barrier reports why it is still waiting. Often enough to be useful while a
    // player stares at the waiting screen, rare enough not to bury the log.
    constexpr uint64_t kWaitReportIntervalMs = 5000;

    // How often the host republishes the match seed while idling in a room. Often enough that a
    // guest holds it long before anyone reaches for Start, rare enough to be invisible.
    constexpr uint64_t kSeedPublishIntervalMs = 250;

    // Pads live at validated offsets inside YAMP's execute_info - the layout handshake at create()
    // is what makes this pointer arithmetic safe.
    pxd::lj_pad_t* PadAt(const yampnet_session* s, void* execute_info, uint32_t player)
    {
        const uint32_t off = (player == 0) ? s->host.layout.pad0_offset : s->host.layout.pad1_offset;
        return reinterpret_cast<pxd::lj_pad_t*>(static_cast<uint8_t*>(execute_info) + off);
    }

    // Compares one frame's checks once both sides are known. Latches the FIRST disagreement:
    // after divergence every later frame differs too, and the first one is the only one that
    // points at a cause.
    void CompareCheck(yampnet_session* s, uint32_t frame)
    {
        if (s->desync_frame != kNoCheck)
            return;

        uint32_t mine = 0, theirs = 0;
        if (!s->local_checks.Get(frame, &mine) || !s->remote_checks.Get(frame, &theirs))
            return;

        // A counter still sitting at 0 means that peer's ROM has not started running frames yet,
        // so any baseline taken from it is meaningless - that is exactly how the first version of
        // this reported a desync at frame 44 while both boards were merely still booting. The
        // round-start anchor should make this unreachable; it stays as the cheap safety net.
        if (mine == 0 || theirs == 0)
            return;

        // EXACT: the values are a hash, so the only agreement that means anything is equality.
        // Baselining a difference between two hashes is not a weaker test, it is a meaningless
        // one - see yampnet_match_config::state_check_exact.
        if (s->match.state_check_exact)
        {
            if (mine == theirs)
                return;
            s->desync_frame = frame;
            s->desync_local = mine;
            s->desync_remote = theirs;
            s->Log(YAMPNET_LOG_ERROR,
                   "DESYNC at frame %u: local state 0x%08X, peer 0x%08X - the simulations parted "
                   "company here",
                   frame, mine, theirs);
            return;
        }

        // COUNTER: compare the RELATIONSHIP between the two counters, not their absolute values.
        // The board reset that precedes a round does not guarantee both machines re-enter the
        // round with the ROM's frame_counter at the same absolute value, and a constant offset is
        // harmless - both simulations still advance one frame per frame. What is never harmless
        // is that offset CHANGING, which is exactly "one side executed game code the other did
        // not". Baselining on the first comparable frame catches that with no false alarms.
        const uint32_t delta = mine - theirs;   // unsigned wrap is intentional
        if (!s->check_baseline_set)
        {
            s->check_baseline = delta;
            s->check_baseline_set = true;
            if (delta != 0)
            {
                s->Log(YAMPNET_LOG_WARN,
                       "state check baseline offset %d at frame %u (the boards entered the round "
                       "with different frame_counter values; tracking the difference instead)",
                       static_cast<int32_t>(delta), frame);
            }
            return;
        }
        if (delta == s->check_baseline)
            return;

        s->desync_frame = frame;
        s->desync_local = mine;
        s->desync_remote = theirs;
        s->Log(YAMPNET_LOG_ERROR,
               "DESYNC at frame %u: local frame_counter %u, peer %u (offset %d, expected %d) - "
               "the simulations parted company here",
               frame, mine, theirs, static_cast<int32_t>(delta),
               static_cast<int32_t>(s->check_baseline));
    }

    void DrainSocket(yampnet_session* s)
    {
        // Big enough for the LARGEST packet on this wire, which is now the linked-cabinet payload
        // and not an input record. A short buffer is not a truncated read here - recvfrom drops
        // the remainder and reports WSAEMSGSIZE - so an undersized one would make link packets
        // vanish while every lockstep packet kept working, which is a nasty way to find out.
        static_assert(sizeof(LinkPacket) >= sizeof(InputPacket), "size the buffer to the biggest");
        alignas(4) uint8_t buf[sizeof(LinkPacket) + 64];
        uint32_t peer = 0;
        for (;;)
        {
            const int got = s->transport.Recv(buf, sizeof(buf), &peer);
            if (got <= 0)
                break;
            if (got < static_cast<int>(sizeof(PacketHeader)))
                continue;

            const auto* hdr = reinterpret_cast<const PacketHeader*>(buf);

            // Not our room: a stale process from an earlier test, or another session that happens
            // to share this address pair. Dropping it here is what stops it releasing our barrier
            // or feeding our rings. Only filter once we know our own room (0 = not yet).
            const uint32_t mine = s->SessionId();
            if (mine != 0 && hdr->session != 0 && hdr->session != mine)
                continue;

            if (hdr->type == kPacketInput && got >= static_cast<int>(sizeof(InputPacket)))
            {
                const auto* pkt = reinterpret_cast<const InputPacket*>(buf);
                s->lockstep.OnRecord(pkt->record);
                if (pkt->check_frame != kNoCheck)
                {
                    s->remote_checks.Put(pkt->check_frame, pkt->check_value);
                    CompareCheck(s, pkt->check_frame);
                }
            }
            else if (hdr->type == kPacketAnnounce && got >= static_cast<int>(sizeof(AnnouncePacket)))
            {
                const auto* ann = reinterpret_cast<const AnnouncePacket*>(buf);

                // THE HOST OWNS THE ROUND NUMBER, exactly as it owns the seed. Both peers must
                // compute the same generation or the barrier never releases, and neither knows how
                // many rounds the other has played - which is why this used to be pinned at 1, and
                // why a second round in the same room could then be poisoned by stale packets from
                // the first. A guest now simply adopts whatever round the host announces.
                if (!s->IsHost() && hdr->player == 0
                    && s->state == YAMPNET_STATE_SYNCING
                    && hdr->generation != static_cast<uint8_t>(s->generation & 0x1F))
                {
                    s->generation = hdr->generation;
                    s->lockstep.BeginRound(hdr->generation);
                    for (auto& h : s->pads)
                        h.Clear();
                    s->local_checks.Clear();
                    s->remote_checks.Clear();
                    s->last_check_frame = kNoCheck;
                    s->check_baseline_set = false;
                    s->desync_frame = kNoCheck;
                    s->Log(YAMPNET_LOG_INFO, "adopted the host's round %u", hdr->generation);
                    s->SendAnnounce();
                }

                s->lockstep.OnPeerAnnounce(hdr->player, hdr->generation);
                // Adopt the host's seed. A guest never invents one, so both machines re-seed the
                // emulator RNG identically.
                if (!s->IsHost() && hdr->player == 0)
                    s->match_seed = ann->seed;
            }
            else if (hdr->type == kPacketLink
                     && got >= static_cast<int>(sizeof(PacketHeader) + sizeof(uint32_t)))
            {
                // Opaque cargo. Newest wins, no ordering, no round, no generation check - see
                // kPacketLink. The only validation is that the declared length fits, because
                // everything past this point is a memcpy into emulated hardware.
                const auto* link = reinterpret_cast<const LinkPacket*>(buf);
                const uint32_t len = link->len;
                if (len != 0 && len <= kLinkPayloadMax
                    && got >= static_cast<int>(sizeof(PacketHeader) + sizeof(uint32_t) + len))
                {
                    memcpy(s->link_in, link->payload, len);
                    s->link_in_len = len;
                    s->link_have = true;
                    ++s->link_rx;
                }
            }
            else if (hdr->type == kPacketSeed && got >= static_cast<int>(sizeof(AnnouncePacket)))
            {
                // Seed only. Pointedly does NOT call OnPeerAnnounce: this arrives while the host
                // is merely sitting in the room, and treating it as an announcement would let the
                // host release a barrier for a round the guest has not begun.
                const auto* ann = reinterpret_cast<const AnnouncePacket*>(buf);
                if (!s->IsHost() && hdr->player == 0 && ann->seed != 0
                    && s->match_seed != ann->seed)
                {
                    s->match_seed = ann->seed;
                    s->Log(YAMPNET_LOG_INFO, "adopted the host's match seed 0x%08X", ann->seed);
                }
            }
        }
    }

    // Frames [0, frame_delay) have no local input yet - the delay means input sampled now applies
    // `delay` frames later. Pre-fill them as neutral on both machines so the round can start.
    void SeedDelayFrames(yampnet_session* s)
    {
        const uint32_t delay = s->lockstep.FrameDelay();
        for (uint32_t f = 0; f < delay; ++f)
        {
            InputPacket pkt = {};
            pkt.header.type = kPacketInput;
            pkt.header.player = static_cast<uint8_t>(s->local_player);
            pkt.header.generation = static_cast<uint8_t>(s->generation & 0x1F);
            pkt.header.session = s->SessionId();
            pkt.check_frame = kNoCheck;   // no frame has been executed yet
            s->lockstep.SubmitLocal(f, 0, &pkt.record);
            s->transport.Send(0, &pkt, sizeof(pkt));
        }
        s->seeded_delay_frames = true;
    }

    // -------------------------------------------------------------------------------------
    // ABI
    // -------------------------------------------------------------------------------------

    yampnet_session* ApiCreate(const yampnet_host* host, yampnet_result* out_err)
    {
        auto fail = [out_err](yampnet_result e) -> yampnet_session* {
            if (out_err) *out_err = e;
            return nullptr;
        };

        if (!host || host->abi_version != YAMPNET_ABI_VERSION)
            return fail(YAMPNET_ERR_ABI);
        if (!LayoutMatches(host->layout, BuiltLayout()))
            return fail(YAMPNET_ERR_LAYOUT);

        auto* s = new (std::nothrow) yampnet_session();
        if (!s)
            return fail(YAMPNET_ERR_INTERNAL);

        s->host = *host;
        s->state = YAMPNET_STATE_IDLE;
        s->Log(YAMPNET_LOG_INFO, "yampnet session created (RPCN transport)");

        if (out_err) *out_err = YAMPNET_OK;
        return s;
    }

    void ApiDestroy(yampnet_session* s) { delete s; }

    yampnet_result ApiPoll(yampnet_session* s)
    {
        if (!s) return YAMPNET_ERR_ARG;
        if (s->state == YAMPNET_STATE_IDLE || s->state == YAMPNET_STATE_FAILED)
            return YAMPNET_OK;

        s->transport.Update();

        // Mirror the transport's own progress into the ABI state, except while a round is running
        // (SYNCING/IN_MATCH are owned by the barrier below, not by the transport).
        if (s->state != YAMPNET_STATE_SYNCING && s->state != YAMPNET_STATE_IN_MATCH)
        {
            switch (s->transport.GetStage())
            {
            case RpcnTransport::Stage::LoggingIn: s->state = YAMPNET_STATE_CONNECTING; break;
            case RpcnTransport::Stage::Online:    s->state = YAMPNET_STATE_ONLINE; break;
            case RpcnTransport::Stage::Hosting:
            case RpcnTransport::Stage::Joining:
            case RpcnTransport::Stage::Linked:
                if (!s->room_logged)
                {
                    s->room_logged = true;
                    s->Log(YAMPNET_LOG_INFO, "room %llu ready (%s) - peer joins with -net-join %llu",
                           static_cast<unsigned long long>(s->transport.RoomId()),
                           s->transport.IsHost() ? "hosting" : "joined",
                           static_cast<unsigned long long>(s->transport.RoomId()));
                }
                s->state = YAMPNET_STATE_IN_ROOM;
                break;
            case RpcnTransport::Stage::Failed:
                s->Fail(s->transport.LastError());
                return YAMPNET_OK;
            default: break;
            }
        }

        DrainSocket(s);

        // PUBLISH THE SEED FROM THE MOMENT THE ROOM EXISTS, not from the moment a round starts.
        // Until this existed, the seed only ever travelled on an announce, and announces only
        // begin when a peer calls begin_round - so a guest that pressed Start first prepared its
        // round with a seed of 0 while the host used the real one, and the two boards were reset
        // from differently seeded generators before frame 0. Order of pressing decided whether the
        // match could work.
        //
        // Cheap and unconditional rather than clever: 12 bytes a few times a second, only while
        // idling in a room, and repeated because it is a plain datagram that may be lost and
        // because the peer's address is not known the instant the room appears.
        if (s->state == YAMPNET_STATE_IN_ROOM && s->IsHost())
        {
            const uint64_t now = GetTickCount64();
            if (now - s->last_seed_send_ms >= kSeedPublishIntervalMs)
            {
                s->last_seed_send_ms = now;
                s->SendSeed();
            }
        }

        if (s->state == YAMPNET_STATE_SYNCING)
        {
            // Keep announcing until the barrier releases: the announce is a plain datagram and may
            // be lost, and it is also how the host's seed reaches a guest that joined late.
            s->SendAnnounce();

            // A barrier that never releases is almost always a connectivity problem, and "waiting
            // for the peer" says nothing about which half failed. Report which of the two steps we
            // are stuck on: not knowing where the peer is (the server never told us) or knowing
            // and hearing nothing back (a NAT or firewall is eating the punch).
            const uint64_t now = GetTickCount64();
            if (now - s->last_wait_report_ms > kWaitReportIntervalMs)
            {
                s->last_wait_report_ms = now;
                if (!s->transport.PeerKnown())
                {
                    s->Log(YAMPNET_LOG_WARN,
                           "still waiting: the server has not given us the peer's address yet");
                }
                else if (!s->transport.PeerHeard())
                {
                    s->Log(YAMPNET_LOG_WARN,
                           "still waiting: punching %s but nothing has come back - check that UDP "
                           "%u is not blocked by a firewall on either machine",
                           s->transport.PeerAddrText(), yampnet::kRpcnP2PPort);
                }
                else
                {
                    s->Log(YAMPNET_LOG_WARN,
                           "still waiting: peer %s is reachable but has not announced this round",
                           s->transport.PeerAddrText());
                }
            }

            if (s->lockstep.BarrierReleased())
            {
                SeedDelayFrames(s);
                s->state = YAMPNET_STATE_IN_MATCH;
                s->wait_since_ms = 0;
                s->Log(YAMPNET_LOG_INFO, "barrier released; round %u seed 0x%08X",
                       s->generation, s->match_seed);
            }
        }
        return YAMPNET_OK;
    }

    yampnet_state ApiGetState(yampnet_session* s) { return s ? s->state : YAMPNET_STATE_FAILED; }

    const char* ApiGetError(yampnet_session* s) { return (s && s->error[0]) ? s->error : ""; }

    // The transport reports peer discovery and NAT progress through this; it has no idea what a
    // yampnet_session is, and the session is the only thing that knows where logs go.
    void TransportLog(void* ctx, const char* msg)
    {
        auto* s = static_cast<yampnet_session*>(ctx);
        if (s)
            s->Log(YAMPNET_LOG_INFO, "%s", msg);
    }

    yampnet_result ApiConnect(yampnet_session* s, const yampnet_rpcn_config* cfg)
    {
        if (!s || !cfg) return YAMPNET_ERR_ARG;

        RpcnTransport::Config tc;
        tc.server = cfg->server;
        tc.port = cfg->port ? cfg->port : kRpcnDefaultPort;
        tc.npid = cfg->npid;
        tc.password = cfg->token;          // token field carries the password/auth secret
        tc.com_id = cfg->communication_id;
        tc.fingerprint_hex = cfg->cert_fingerprint;
        tc.log = &TransportLog;
        tc.log_ctx = s;

        if (!s->transport.Start(tc))
        {
            s->Fail(s->transport.LastError());
            return YAMPNET_ERR_NETWORK;
        }
        s->state = YAMPNET_STATE_CONNECTING;
        s->Log(YAMPNET_LOG_INFO, "connecting to %s:%u as %s",
               cfg->server ? cfg->server : "?", tc.port, cfg->npid ? cfg->npid : "?");
        return YAMPNET_OK;
    }

    yampnet_result ApiDisconnect(yampnet_session* s)
    {
        if (!s) return YAMPNET_ERR_ARG;
        s->transport.Stop();
        s->state = YAMPNET_STATE_IDLE;
        s->local_player = -1;
        return YAMPNET_OK;
    }

    yampnet_result ApiCreateRoom(yampnet_session* s, const yampnet_room_config* cfg)
    {
        if (!s || !cfg) return YAMPNET_ERR_ARG;

        if (!s->transport.Host(cfg->max_players ? cfg->max_players : 2, cfg->password,
                               cfg->game_flags))
        {
            s->Fail(s->transport.LastError());
            return YAMPNET_ERR_ROOM;
        }
        s->local_player = 0;

        // The host owns the match seed. A forced value makes a session reproducible, which is the
        // only way to debug a desync twice.
        if (cfg->forced_seed != 0)
            s->match_seed = cfg->forced_seed;
        else
            s->match_seed = static_cast<uint32_t>(GetTickCount64()) ^ 0x9E3779B9u;

        // ZERO IS THE "not known yet" VALUE, so the host must never hold it. A guest reads 0 until
        // the host's seed reaches it and waits on exactly that test; a host that happened to roll 0
        // would publish a seed indistinguishable from silence and hang its guest at Start. One
        // tick in 2^32, and it would look like a network fault.
        if (s->match_seed == 0)
            s->match_seed = 0x9E3779B9u;

        // Do NOT declare IN_ROOM here. The room does not exist until the server replies; poll()'s
        // stage mapping owns the state. Setting it optimistically (a leftover from the UDP backend)
        // made the host open the lockstep barrier in the same frame it merely REQUESTED the room,
        // which then pinned the state at SYNCING and suppressed the stage mapping for good.
        s->Log(YAMPNET_LOG_INFO, "hosting requested; seed 0x%08X flags 0x%08X", s->match_seed,
               cfg->game_flags);
        return YAMPNET_OK;
    }

    yampnet_result ApiSearchRooms(yampnet_session* s)
    {
        if (!s) return YAMPNET_ERR_ARG;
        if (s->state != YAMPNET_STATE_ONLINE)
            return YAMPNET_ERR_STATE;   // discovery needs a logged-in, room-less session

        if (!s->transport.Search())
        {
            s->Log(YAMPNET_LOG_WARN, "room search could not be sent: %s",
                   s->transport.LastError());
            return YAMPNET_ERR_ROOM;
        }
        return YAMPNET_OK;
    }

    uint32_t ApiGetRooms(yampnet_session* s, yampnet_room_info* out, uint32_t max_out)
    {
        if (!s || !out || max_out == 0)
            return 0;

        const uint32_t have = s->transport.RoomCount();
        const uint32_t n = have < max_out ? have : max_out;
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto& src = s->transport.Room(i);
            yampnet_room_info& dst = out[i];
            dst = {};
            dst.room_id = src.room_id;
            dst.player_count = src.cur_members;
            dst.max_players = src.max_slots;
            dst.has_password = src.has_password ? 1u : 0u;
            dst.game_flags = src.flag_attr;
            // The owner's npid is the only human-readable handle a room has.
            strncpy_s(dst.name, src.owner[0] ? src.owner : "(unknown)", _TRUNCATE);
        }
        return n;
    }

    yampnet_result ApiJoinRoom(yampnet_session* s, uint64_t room_id, const char* password)
    {
        if (!s) return YAMPNET_ERR_ARG;

        if (!s->transport.Join(room_id, password))
        {
            s->Fail(s->transport.LastError());
            return YAMPNET_ERR_ROOM;
        }
        s->local_player = 1;
        s->match_seed = 0;   // adopted from the host's announce
        // Same as above: the join is only requested here, not complete.
        s->Log(YAMPNET_LOG_INFO, "join requested");
        return YAMPNET_OK;
    }

    yampnet_result ApiLeaveRoom(yampnet_session* s)
    {
        if (!s) return YAMPNET_ERR_ARG;
        s->transport.Stop();
        s->local_player = -1;
        if (s->state != YAMPNET_STATE_FAILED)
            s->state = YAMPNET_STATE_ONLINE;
        return YAMPNET_OK;
    }

    int32_t ApiGetLocalPlayer(yampnet_session* s) { return s ? s->local_player : -1; }

    uint32_t ApiGetMatchSeed(yampnet_session* s) { return s ? s->match_seed : 0; }

    yampnet_result ApiBeginRound(yampnet_session* s, uint32_t generation,
                                 const yampnet_match_config* cfg)
    {
        if (!s || !cfg) return YAMPNET_ERR_ARG;
        if (s->local_player < 0) return YAMPNET_ERR_STATE;

        s->match = *cfg;
        s->generation = generation;
        s->seeded_delay_frames = false;

        const uint32_t delay = cfg->frame_delay ? cfg->frame_delay : 2u;
        s->lockstep.Configure(static_cast<uint32_t>(s->local_player), kMaxPlayers, delay);
        s->lockstep.BeginRound(generation);
        for (auto& h : s->pads)
            h.Clear();

        // A fresh round means a fresh comparison: stale checks from the previous one would
        // otherwise be compared against this round's frame numbers and read as a desync.
        s->local_checks.Clear();
        s->remote_checks.Clear();
        s->last_check_frame = kNoCheck;
        s->check_baseline_set = false;
        s->desync_frame = kNoCheck;

        s->state = YAMPNET_STATE_SYNCING;
        s->wait_since_ms = GetTickCount64();
        s->last_wait_report_ms = s->wait_since_ms;
        s->SendAnnounce();
        return YAMPNET_OK;
    }

    yampnet_result ApiEndRound(yampnet_session* s)
    {
        if (!s) return YAMPNET_ERR_ARG;
        if (s->state == YAMPNET_STATE_IN_MATCH)
            s->state = YAMPNET_STATE_IN_ROOM;
        return YAMPNET_OK;
    }

    yampnet_step ApiStep(yampnet_session* s, uint32_t frame, void* execute_info)
    {
        if (!s || !execute_info)
            return YAMPNET_STEP_DISCONNECTED;
        if (s->state != YAMPNET_STATE_IN_MATCH)
            return YAMPNET_STEP_DISCONNECTED;

        DrainSocket(s);

        // Sample local input for frame + delay. This is the delay in "delay-based": what the
        // player does now is consumed `delay` frames from now, which buys that many frames of
        // network latency before anyone has to stall.
        const uint32_t send_frame = frame + s->lockstep.FrameDelay();
        const uint32_t last_sent = s->lockstep.LastLocalFrame();
        if (last_sent == kInvalidFrame || send_frame > last_sent)
        {
            const pxd::lj_pad_t* local = PadAt(s, execute_info,
                                               static_cast<uint32_t>(s->local_player));
            const uint32_t word = EncodePad(*local);

            InputPacket pkt = {};
            pkt.header.type = kPacketInput;
            pkt.header.player = static_cast<uint8_t>(s->local_player);
            pkt.header.generation = static_cast<uint8_t>(s->generation & 0x1F);
            pkt.header.session = s->SessionId();
            pkt.check_frame = s->last_check_frame;
            pkt.check_value = s->last_check_value;
            s->lockstep.SubmitLocal(send_frame, word, &pkt.record);
            s->transport.Send(0, &pkt, sizeof(pkt));
        }

        if (!s->lockstep.Ready(frame))
        {
            // Stalled: the peer's input for this frame has not arrived. The caller must NOT
            // advance the emulator - it simply calls again next tick.
            const uint64_t now = GetTickCount64();
            if (s->wait_since_ms == 0)
            {
                s->wait_since_ms = now;
                s->lockstep.NoteStall();
            }
            else if (s->match.stall_timeout_ms != 0 &&
                     (now - s->wait_since_ms) > s->match.stall_timeout_ms)
            {
                s->Log(YAMPNET_LOG_WARN, "stalled >%u ms at frame %u",
                       s->match.stall_timeout_ms, frame);
                return YAMPNET_STEP_TIMEOUT;
            }
            return YAMPNET_STEP_WAIT;
        }

        s->wait_since_ms = 0;

        // Write EVERY player's pad from the transmitted words - including our own. Feeding the
        // local module the richer local pad while the peer feeds a reconstruction is the classic
        // way to desync two "identical" simulations. See PadCodec.h.
        for (uint32_t p = 0; p < s->lockstep.PlayerCount(); ++p)
        {
            const uint32_t word = s->lockstep.InputFor(p, frame);
            DecodePad(word, p, s->pads[p], *PadAt(s, execute_info, p));
        }
        return YAMPNET_STEP_READY;
    }

    uint32_t ApiGetPingMs(yampnet_session*) { return 0; }

    uint32_t ApiGetStallCount(yampnet_session* s) { return s ? s->lockstep.StallCount() : 0; }

    // The transport owns the id: it is assigned by the server's CreateRoom/JoinRoom reply, so it
    // is 0 until that reply lands - which is also exactly when the lobby may show it.
    uint64_t ApiGetRoomId(yampnet_session* s) { return s ? s->transport.RoomId() : 0; }

    uint32_t ApiGetRoomFlags(yampnet_session* s) { return s ? s->transport.RoomFlags() : 0; }

    // ---- Linked-cabinet channel ----------------------------------------------------------
    //
    // Deliberately usable from IN_ROOM onwards rather than only in a match: the cabinets link
    // during their boot-time network check, which happens as soon as both are present and long
    // before anyone reaches for Start. Gated on the peer's address being KNOWN, because before
    // that there is nowhere to send and every call would be a silent no-op.
    int32_t ApiLinkSend(yampnet_session* s, const void* data, uint32_t len)
    {
        if (!s || !data || len == 0 || len > kLinkPayloadMax || !s->transport.PeerKnown())
            return 0;
        LinkPacket pkt = {};
        pkt.header.type = kPacketLink;
        pkt.header.player = static_cast<uint8_t>(s->local_player < 0 ? 0 : s->local_player);
        pkt.header.generation = static_cast<uint8_t>(s->generation & 0x1F);
        pkt.header.session = s->SessionId();
        pkt.len = len;
        memcpy(pkt.payload, data, len);
        // Only the live bytes go on the wire - the payload array is a maximum, not a size.
        const uint32_t bytes = sizeof(PacketHeader) + sizeof(uint32_t) + len;
        if (!s->transport.Send(0, &pkt, bytes))
            return 0;
        ++s->link_tx;
        return static_cast<int32_t>(len);
    }

    // Drains first, so a caller running at emulator rate is never a frame behind the socket.
    // Returns 0 when nothing new has arrived, which is how the caller knows to keep the payload
    // it already has rather than hand the hardware a gap.
    int32_t ApiLinkTake(yampnet_session* s, void* out, uint32_t cap)
    {
        if (!s || !out || cap == 0)
            return 0;
        DrainSocket(s);
        if (!s->link_have || s->link_in_len == 0 || s->link_in_len > cap)
            return 0;
        const uint32_t len = s->link_in_len;
        memcpy(out, s->link_in, len);
        s->link_have = false;
        return static_cast<int32_t>(len);
    }

    // "Is there a peer to link with right now?" - the one question the cabinet's firmware model
    // asks, and the honest answer is the peer's address being known, not the round state.
    int32_t ApiLinkReady(yampnet_session* s)
    {
        return (s && s->transport.PeerKnown()) ? 1 : 0;
    }

    void ApiSubmitStateCheck(yampnet_session* s, uint32_t frame, uint32_t value)
    {
        if (!s || frame == kNoCheck)
            return;
        s->local_checks.Put(frame, value);
        // Sent on the NEXT outgoing packet; there is one every frame, so the lag is one frame.
        s->last_check_frame = frame;
        s->last_check_value = value;
        CompareCheck(s, frame);   // the peer's value for this frame may already be here
    }

    int32_t ApiGetDesync(yampnet_session* s, uint32_t* out_frame, uint32_t* out_local,
                         uint32_t* out_remote)
    {
        if (!s || s->desync_frame == kNoCheck)
            return 0;
        if (out_frame)  *out_frame = s->desync_frame;
        if (out_local)  *out_local = s->desync_local;
        if (out_remote) *out_remote = s->desync_remote;
        return 1;
    }

    const yampnet_api kApi = {
        YAMPNET_ABI_VERSION,
        &ApiCreate,
        &ApiDestroy,
        &ApiPoll,
        &ApiGetState,
        &ApiGetError,
        &ApiConnect,
        &ApiDisconnect,
        &ApiCreateRoom,
        &ApiSearchRooms,
        &ApiGetRooms,
        &ApiJoinRoom,
        &ApiLeaveRoom,
        &ApiGetLocalPlayer,
        &ApiGetMatchSeed,
        &ApiBeginRound,
        &ApiEndRound,
        &ApiStep,
        &ApiGetPingMs,
        &ApiGetStallCount,
        &ApiGetRoomId,
        &ApiSubmitStateCheck,
        &ApiGetDesync,
        &ApiGetRoomFlags,
        &ApiLinkReady,
        &ApiLinkSend,
        &ApiLinkTake,
    };
}

extern "C" __declspec(dllexport) const yampnet_api* YampNet_GetApi(uint32_t requested_abi)
{
    return (requested_abi == YAMPNET_ABI_VERSION) ? &kApi : nullptr;
}
