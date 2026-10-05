// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::max
#include <array>
#include <cerrno>
#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint32_t
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h> // open()
#include <sys/socket.h>
#include <sys/stat.h> // mkfifo()
#include <unistd.h> // read(), write(), close()
#endif

#include <event2/util.h>

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/inout.h>
#include <libtransmission/local-data.h>
#include <libtransmission/net.h>
#include <libtransmission/peer-common.h>
#include <libtransmission/peer-io.h>
#include <libtransmission/peer-mgr.h>
#include <libtransmission/peer-msgs.h>
#include <libtransmission/peer-socket-tcp.h>
#include <libtransmission/session.h> // tr_peerIdInit()
#include <libtransmission/string-utils.h> // tr_strerror()
#include <libtransmission/torrent.h>
#include <libtransmission/tr-macros.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

#define LOCAL_SOCKETPAIR_AF TR_IF_WIN32(AF_INET, AF_UNIX)

using namespace std::literals;

namespace tr::test
{

// Drives a tr_peerMsgs over one end of a socketpair; the test plays the
// remote peer on the other end with raw BitTorrent wire messages.
class PeerMsgsTest : public SessionTest
{
protected:
    static auto constexpr MaxWaitMsec = 5000;

    static auto constexpr MsgInterested = uint8_t{ 2 };
    static auto constexpr MsgRequest = uint8_t{ 6 };
    static auto constexpr MsgPiece = uint8_t{ 7 };

    struct Block
    {
        uint32_t index = 0;
        uint32_t offset = 0;
        uint32_t length = 0;

        [[nodiscard]] constexpr bool operator==(Block const& that) const noexcept
        {
            return index == that.index && offset == that.offset && length == that.length;
        }
    };

    struct RemotePeer
    {
        evutil_socket_t fd = -1;
        std::vector<uint8_t> inbuf;
        std::vector<Block> pieces_received;

        [[nodiscard]] static constexpr uint32_t be32(uint8_t const* p) noexcept
        {
            return (uint32_t{ p[0] } << 24U) | (uint32_t{ p[1] } << 16U) | (uint32_t{ p[2] } << 8U) | uint32_t{ p[3] };
        }

        static void put32(std::vector<uint8_t>& out, uint32_t val)
        {
            out.push_back(static_cast<uint8_t>(val >> 24U));
            out.push_back(static_cast<uint8_t>(val >> 16U));
            out.push_back(static_cast<uint8_t>(val >> 8U));
            out.push_back(static_cast<uint8_t>(val));
        }

        void send(std::vector<uint8_t> const& bytes) const
        {
            auto const* walk = std::data(bytes);
            auto n_left = std::size(bytes);
            while (n_left > 0U)
            {
                auto const n = ::write(fd, walk, n_left);
                ASSERT_GT(n, 0) << tr_strerror(errno);
                walk += n;
                n_left -= static_cast<size_t>(n);
            }
        }

        void send_interested() const
        {
            auto out = std::vector<uint8_t>{};
            put32(out, 1U);
            out.push_back(MsgInterested);
            send(out);
        }

        void send_requests(std::vector<Block> const& blocks) const
        {
            auto out = std::vector<uint8_t>{};
            for (auto const& block : blocks)
            {
                put32(out, 13U);
                out.push_back(MsgRequest);
                put32(out, block.index);
                put32(out, block.offset);
                put32(out, block.length);
            }
            send(out);
        }

        // Reads whatever the client has sent so far (nonblocking) and
        // records every PIECE message. Other messages are skipped.
        void drain()
        {
            for (;;)
            {
                auto buf = std::array<uint8_t, 65536>{};
                auto const n = ::read(fd, std::data(buf), std::size(buf));
                if (n <= 0)
                {
                    break;
                }
                inbuf.insert(std::end(inbuf), std::begin(buf), std::begin(buf) + n);
            }

            auto pos = size_t{};
            while (std::size(inbuf) - pos >= 4U)
            {
                auto const len = be32(std::data(inbuf) + pos);
                if (std::size(inbuf) - pos < 4U + len)
                {
                    break;
                }

                if (len >= 9U && inbuf[pos + 4U] == MsgPiece)
                {
                    auto const* payload = std::data(inbuf) + pos + 5U;
                    pieces_received.push_back({ .index = be32(payload), .offset = be32(payload + 4), .length = len - 9U });
                }

                pos += 4U + len;
            }
            inbuf.erase(std::begin(inbuf), std::begin(inbuf) + pos);
        }
    };

    // Runs `func` on the session thread and waits for it to finish.
    void runOnSessionThread(std::function<void()> func)
    {
        auto done = false;
        session_->run_in_session_thread(
            [&done, func = std::move(func)]()
            {
                func();
                done = true;
            });
        ASSERT_TRUE(waitFor([&done]() { return done; }, MaxWaitMsec));
    }

    // Connects a tr_peerMsgs for `tor` to one end of a socketpair and
    // returns the other end, with the remote peer unchoked and interested.
    std::shared_ptr<tr_peerMsgs> connectPeer(tr_torrent* tor, RemotePeer& remote)
    {
        auto sockpair = std::array<evutil_socket_t, 2>{ -1, -1 };
        EXPECT_EQ(0, evutil_socketpair(LOCAL_SOCKETPAIR_AF, SOCK_STREAM, 0, std::data(sockpair))) << tr_strerror(errno);
        EXPECT_EQ(0, evutil_make_socket_nonblocking(sockpair[0]));
        EXPECT_EQ(0, evutil_make_socket_nonblocking(sockpair[1]));
        remote.fd = sockpair[1];

        static auto const SockAddr = tr_socket_address{ *tr_address::from_string("127.0.0.1"sv), tr_port::from_host(51413) };

        auto msgs = std::shared_ptr<tr_peerMsgs>{};
        runOnSessionThread(
            [this, tor, &sockpair, &msgs]()
            {
                auto io = tr_peerIo::new_incoming(
                    session_,
                    &session_->top_bandwidth_,
                    tr_peer_socket_tcp::create(*session_, SockAddr, static_cast<tr_socket_t>(sockpair[0])));
                auto peer_info = std::make_shared<tr_peer_info>(
                    SockAddr.address(),
                    uint8_t{},
                    TR_PEER_FROM_INCOMING,
                    []() { return tr_port{}; });
                msgs = tr_peerMsgs::create(
                    *tor,
                    std::move(peer_info),
                    std::move(io),
                    tr_peerIdInit(),
                    [](tr_peerMsgs*, tr_peer_event const&, void*) {},
                    nullptr);
                msgs->set_choke(false);
            });

        remote.send_interested();
        return msgs;
    }

    // Every block of the torrent, in order, as the remote peer would request it.
    [[nodiscard]] static std::vector<Block> allBlocks(tr_torrent const* tor)
    {
        auto blocks = std::vector<Block>{};
        for (tr_block_index_t block = 0, n = tor->block_count(); block < n; ++block)
        {
            auto const loc = tor->block_loc(block);
            blocks.push_back({ .index = loc.piece, .offset = loc.piece_offset, .length = tor->block_size(block) });
        }
        return blocks;
    }
};

// A peer may pipeline up to `reqq` requests. The client must not read every
// requested block the moment the request arrives: it reads a bounded
// lookahead, keeps the rest as plain queued requests, and still answers
// all of them, in order.
TEST_F(PeerMsgsTest, uploadReadsAreABoundedLookahead)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    static auto constexpr MaxOutstandingReads = size_t{ 16U }; // MaxOutstandingUploadReads in peer-msgs.cc

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    ASSERT_TRUE(tor->has_all());

    // Stall the torrent's disk queue, which runs one op at a time: a read
    // whose "file" is a FIFO with no writer blocks in open() until the test
    // opens the other end. Its file index is one the torrent doesn't have,
    // so the fd pool never hands the FIFO out for a real block.
    auto const fifo_name = "stall.fifo"sv;
    auto const fifo_path = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', fifo_name };
    ASSERT_EQ(0, mkfifo(fifo_path.c_str(), 0600)) << tr_strerror(errno);
    auto stall_plan = makeIoPlan(tor);
    ASSERT_EQ(1U, std::size(stall_plan.files));
    stall_plan.files[0].index = tor->file_count() + 1U;
    stall_plan.files[0].subpath = fifo_name;
    auto stall_answered = false;
    session_->local_data.read(
        std::move(stall_plan),
        [&stall_answered](tr_torrent_id_t, tr_byte_span_t, tr_error const&, std::unique_ptr<tr::LocalData::BlockData>)
        { stall_answered = true; });

    auto remote = RemotePeer{};
    auto msgs = connectPeer(tor, remote);
    ASSERT_NE(nullptr, msgs);

    // ask for every block twice: well over the lookahead, well under reqq
    auto const blocks = allBlocks(tor);
    auto requests = blocks;
    requests.insert(std::end(requests), std::begin(blocks), std::end(blocks));
    auto const n_requests = std::size(requests);
    ASSERT_GT(n_requests, MaxOutstandingReads * 4U);
    remote.send_requests(requests);

    auto n_queued = size_t{};
    auto n_outstanding = size_t{};
    auto const sample = [this, &msgs, &n_queued, &n_outstanding]()
    {
        runOnSessionThread(
            [&msgs, &n_queued, &n_outstanding]()
            {
                n_queued = msgs->active_req_count(tr_direction::PeerToClient);
                n_outstanding = msgs->upload_reads_outstanding();
                msgs->pulse(); // refills the output buffer, as the peer manager does
            });
    };

    // every request is queued once parsed, and none can be answered yet
    EXPECT_TRUE(waitFor(
        [&]()
        {
            sample();
            return n_queued == n_requests;
        },
        MaxWaitMsec));
    EXPECT_EQ(n_requests, n_queued);
    EXPECT_EQ(MaxOutstandingReads, n_outstanding) << "reads should be issued for the lookahead only";
    EXPECT_FALSE(stall_answered);

    // release the stall: the FIFO read fails (ESPIPE) and the block reads behind it run
    auto const writer = open(fifo_path.c_str(), O_WRONLY);
    ASSERT_NE(-1, writer) << tr_strerror(errno);
    EXPECT_EQ(0, close(writer));

    auto max_outstanding = n_outstanding;
    EXPECT_TRUE(waitFor(
        [&]()
        {
            sample();
            max_outstanding = std::max(max_outstanding, n_outstanding);
            remote.drain();
            return std::size(remote.pieces_received) >= n_requests;
        },
        MaxWaitMsec));
    sample();

    EXPECT_TRUE(stall_answered);
    EXPECT_LE(max_outstanding, MaxOutstandingReads);
    EXPECT_EQ(0U, n_queued);
    EXPECT_EQ(0U, n_outstanding);
    ASSERT_EQ(n_requests, std::size(remote.pieces_received));
    EXPECT_EQ(requests, remote.pieces_received) << "blocks must be sent in the order they were requested";

    // the peer must go before the torrent it references
    runOnSessionThread([&msgs]() { msgs.reset(); });
    evutil_closesocket(remote.fd);
    tr_torrentRemove(tor, true);
#endif
}

} // namespace tr::test
