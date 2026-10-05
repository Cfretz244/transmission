// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cstddef> // size_t
#include <cstdint> // int64_t
#include <cerrno>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <iterator> // std::inserter
#include <set>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h> // mkfifo()
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <libtransmission/quark.h>
#include <libtransmission/transmission.h>
#include <libtransmission/file.h>
#include <libtransmission/local-data.h>
#include <libtransmission/rpcimpl.h>
#include <libtransmission/session.h>
#include <libtransmission/torrent.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/string-utils.h> // tr_strerror()
#include <libtransmission/variant.h>

#include "test-fixtures.h"

struct tr_session;

using namespace std::literals;

namespace tr::test
{

using RpcTest = SessionTest;

namespace
{
[[nodiscard]] std::string makeRequest(tr_session* session, std::string_view const jsonreq)
{
    auto serde = tr_variant_serde::json().inplace();

    auto request = serde.parse(jsonreq);
    if (!request)
    {
        return {};
    }

    auto response = tr_variant{};
    tr_rpc_request_exec(session, *request, [&response](tr_variant&& resp) { response = std::move(resp); });

    return serde.to_string(response);
}
} // namespace

TEST_F(RpcTest, EmptyRequest)
{
    static auto constexpr Request = ""sv;

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, Request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_EQ(result, nullptr);
    auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32700); // don't use constants here in case they are wrong
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Parse error"sv);
    auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id);
}

TEST_F(RpcTest, NotArrayOrObject)
{
    auto requests = std::vector<tr_variant>{};
    requests.emplace_back(12345);
    requests.emplace_back(0.5);
    requests.emplace_back("12345"sv);
    requests.emplace_back(nullptr);
    requests.emplace_back(true);

    for (auto& req : requests)
    {
        auto response = tr_variant{};
        tr_rpc_request_exec(session_, req, [&response](tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find(TR_KEY_result);
        EXPECT_EQ(result, std::end(*response_map));
        auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
        ASSERT_NE(error, nullptr);
        auto const error_code = error->value_if<int64_t>(TR_KEY_code);
        ASSERT_TRUE(error_code);
        EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
        auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
        ASSERT_TRUE(error_message);
        EXPECT_EQ(*error_message, "Invalid Request"sv);
        auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
        ASSERT_NE(error_data, nullptr);
        auto const error_string = error_data->value_if<std::string_view>(TR_KEY_error_string);
        ASSERT_TRUE(error_string);
        EXPECT_EQ(*error_string, "request must be an Array or Object"sv);
        auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
        EXPECT_TRUE(id);
    }
}

TEST_F(RpcTest, JsonRpcWrongVersion)
{
    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, "1.0");
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    request_map.try_emplace(TR_KEY_id, 12345);
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->find(TR_KEY_result);
    EXPECT_EQ(result, std::end(*response_map));
    auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    auto const error_string = error_data->value_if<std::string_view>(TR_KEY_error_string);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "JSON-RPC version is not 2.0"sv);
    auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id);
}

TEST_F(RpcTest, idSync)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(12345);
    ids.emplace_back(0.5);
    ids.emplace_back("12345"sv);
    ids.emplace_back(nullptr);

    for (auto const& request_id : ids)
    {
        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
        request_map[TR_KEY_id].merge(request_id); // copy
        auto request = tr_variant{ std::move(request_map) };

        auto response = tr_variant{};
        tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
        EXPECT_NE(result, nullptr);
        auto const error = response_map->find(TR_KEY_error);
        EXPECT_EQ(error, std::end(*response_map));
        switch (request_id.index())
        {
        case tr_variant::IntIndex:
            EXPECT_EQ(request_id.value_if<int64_t>(), response_map->value_if<int64_t>(TR_KEY_id));
            break;
        case tr_variant::DoubleIndex:
            EXPECT_EQ(request_id.value_if<double>(), response_map->value_if<double>(TR_KEY_id));
            break;
        case tr_variant::StringIndex:
        case tr_variant::StringViewIndex:
            EXPECT_EQ(request_id.value_if<std::string_view>(), response_map->value_if<std::string_view>(TR_KEY_id));
            break;
        case tr_variant::NullIndex:
            EXPECT_EQ(request_id.value_if<std::nullptr_t>(), response_map->value_if<std::nullptr_t>(TR_KEY_id));
            break;
        default:
            break;
        }
    }
}

TEST_F(RpcTest, idWrongType)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(tr_variant::Map{});
    ids.emplace_back(tr_variant::Vector{});
    ids.emplace_back(true);

    for (auto const& request_id : ids)
    {
        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
        request_map[TR_KEY_id].merge(request_id); // copy
        auto request = tr_variant{ std::move(request_map) };

        auto response = tr_variant{};
        tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find(TR_KEY_result);
        EXPECT_EQ(result, std::end(*response_map));
        auto const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
        ASSERT_NE(error, nullptr);
        auto const error_code = error->value_if<int64_t>(TR_KEY_code);
        ASSERT_TRUE(error_code);
        EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
        auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
        ASSERT_TRUE(error_message);
        EXPECT_EQ(*error_message, "Invalid Request"sv);
        auto const error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
        ASSERT_NE(error_data, nullptr);
        auto const error_string = error_data->value_if<std::string_view>(TR_KEY_error_string);
        ASSERT_TRUE(error_string);
        EXPECT_EQ(*error_string, "id type must be String, Number, or Null"sv);
        auto const id = response_map->value_if<std::nullptr_t>(TR_KEY_id);
        EXPECT_TRUE(id);
    }
}

TEST_F(RpcTest, tagSyncLegacy)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    request_map.try_emplace(TR_KEY_tag, 12345);
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "success"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);
}

#ifndef _WIN32
namespace
{
int connectLoopback(uint16_t port)
{
    auto const sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1)
    {
        return -1;
    }
    auto addr = sockaddr_in{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0)
    {
        close(sock);
        return -1;
    }
    auto tv = timeval{ 10, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

// Reads until EOF or the receive timeout.
std::string readAll(int sock)
{
    auto out = std::string{};
    auto buf = std::array<char, 4096>{};
    for (;;)
    {
        auto const n = read(sock, std::data(buf), std::size(buf));
        if (n <= 0)
        {
            return out;
        }
        out.append(std::data(buf), static_cast<size_t>(n));
    }
}

void sendHttpPost(int sock, uint16_t port, std::string_view session_id, std::string_view body)
{
    auto const req = fmt::format(
        "POST /transmission/rpc HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nX-Transmission-Session-Id: {}\r\n"
        "Content-Type: application/json\r\nContent-Length: {}\r\n\r\n{}",
        port,
        session_id,
        std::size(body),
        body);
    ASSERT_EQ(static_cast<ssize_t>(std::size(req)), write(sock, std::data(req), std::size(req)));
}
} // namespace

// An async RPC (here rename-path, held up behind a stalled disk queue) can
// finish after the session has started closing. The reply must not touch the
// HTTP request, which the RPC listener freed when it went down: without the
// liveness check this is a use-after-free that ASan catches.
TEST_F(RpcTest, asyncHttpReplyLandingDuringCloseIsDropped)
{
    // an RPC listener on a free loopback port
    auto const port = []()
    {
        auto const sock = socket(AF_INET, SOCK_STREAM, 0);
        auto addr = sockaddr_in{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr));
        auto len = socklen_t{ sizeof(addr) };
        getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len);
        close(sock);
        return ntohs(addr.sin_port);
    }();
    tr_sessionSetRPCPort(session_, port);
    tr_sessionSetRPCEnabled(session_, true);
    ASSERT_TRUE(waitFor(
        [port]()
        {
            auto const sock = connectLoopback(port);
            if (sock == -1)
            {
                return false;
            }
            close(sock);
            return true;
        },
        5000));

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);

    // stall the torrent's disk queue: a read whose "file" is a FIFO with no
    // writer blocks in open() until the test opens the other end
    auto const fifo_name = "stall.fifo"sv;
    auto const fifo_path = tr_pathbuf{ tr_sessionGetDownloadDir(session_), '/', fifo_name };
    ASSERT_EQ(0, mkfifo(fifo_path.c_str(), 0600)) << tr_strerror(errno);
    auto stall_plan = makeIoPlan(tor);
    ASSERT_EQ(1U, std::size(stall_plan.files));
    stall_plan.files[0].index = tor->file_count() + 1U;
    stall_plan.files[0].subpath = fifo_name;
    session_->local_data.read(
        std::move(stall_plan),
        [](tr_torrent_id_t, tr_byte_span_t, tr_error const&, std::unique_ptr<tr::LocalData::BlockData>) {});

    // get a session id
    auto sock = connectLoopback(port);
    ASSERT_NE(-1, sock);
    sendHttpPost(sock, port, "", R"({"method":"session-get"})");
    auto const first = readAll(sock);
    close(sock);
    auto const key = "X-Transmission-Session-Id: "sv;
    auto const key_pos = first.find(key);
    ASSERT_NE(std::string::npos, key_pos) << first;
    auto const id_begin = key_pos + std::size(key);
    auto const session_id = first.substr(id_begin, first.find("\r\n", id_begin) - id_begin);

    // the rename queues behind the stall; no reply can come yet
    sock = connectLoopback(port);
    ASSERT_NE(-1, sock);
    sendHttpPost(
        sock,
        port,
        session_id,
        fmt::format(
            R"({{"method":"torrent-rename-path","arguments":{{"ids":[{}],"path":"files-filled-with-zeroes/512","name":"renamed"}}}})",
            tor->id()));
    std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });

    // close the session while it is pending, then release the stall so the
    // rename (or its cancellation) delivers its reply during the close
    auto closer = std::thread{ [this]()
                               {
                                   closeSession();
                               } };
    std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
    auto const writer = open(fifo_path.c_str(), O_WRONLY);
    ASSERT_NE(-1, writer) << tr_strerror(errno);
    close(writer);
    closer.join();

    // the connection went down with the listener: no reply, no crash
    auto const rest = readAll(sock);
    close(sock);
    EXPECT_EQ(std::string::npos, rest.find("HTTP/1.1 200")) << rest;
}
#endif

TEST_F(RpcTest, idAsync)
{
    auto ids = std::vector<tr_variant>{};
    ids.emplace_back(12345);
    ids.emplace_back(0.5);
    ids.emplace_back("12345"sv);
    ids.emplace_back(nullptr);

    for (auto const& request_id : ids)
    {
        auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
        EXPECT_NE(nullptr, tor);

        auto request_map = tr_variant::Map{ 3U };
        request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
        request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_torrent_rename_path));
        request_map[TR_KEY_id].merge(request_id); // copy

        auto params_map = tr_variant::Map{ 2U };
        params_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
        params_map.try_emplace(TR_KEY_name, "512_test");
        request_map.try_emplace(TR_KEY_params, std::move(params_map));

        auto request = tr_variant{ std::move(request_map) };
        auto promise = std::promise<tr_variant>{};
        auto future = promise.get_future();
        tr_rpc_request_exec(session_, request, [&promise](tr_variant&& resp) { promise.set_value(std::move(resp)); });
        auto const response = future.get();

        auto const* const response_map = response.get_if<tr_variant::Map>();
        ASSERT_NE(response_map, nullptr);
        auto const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
        EXPECT_NE(result, nullptr);
        auto const error = response_map->find(TR_KEY_error);
        EXPECT_EQ(error, std::end(*response_map));
        switch (request_id.index())
        {
        case tr_variant::IntIndex:
            EXPECT_EQ(request_id.value_if<int64_t>(), response_map->value_if<int64_t>(TR_KEY_id));
            break;
        case tr_variant::DoubleIndex:
            EXPECT_EQ(request_id.value_if<double>(), response_map->value_if<double>(TR_KEY_id));
            break;
        case tr_variant::StringIndex:
        case tr_variant::StringViewIndex:
            EXPECT_EQ(request_id.value_if<std::string_view>(), response_map->value_if<std::string_view>(TR_KEY_id));
            break;
        case tr_variant::NullIndex:
            EXPECT_EQ(request_id.value_if<std::nullptr_t>(), response_map->value_if<std::nullptr_t>(TR_KEY_id));
            break;
        default:
            break;
        }

        // cleanup
        tr_torrentRemove(tor, false);
        EXPECT_TRUE(waitFor([this] { return std::empty(session_->torrents()); }, 5s));
    }
}

TEST_F(RpcTest, tagAsyncLegacy)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_torrent_rename_path));
    request_map.try_emplace(TR_KEY_tag, 12345);

    auto arguments_map = tr_variant::Map{ 2U };
    arguments_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
    arguments_map.try_emplace(TR_KEY_name, "512_test");
    request_map.try_emplace(TR_KEY_arguments, std::move(arguments_map));

    auto request = tr_variant{ std::move(request_map) };
    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(session_, request, [&promise](tr_variant&& resp) { promise.set_value(std::move(resp)); });
    auto const response = future.get();

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "success"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);

    // cleanup
    tr_torrentRemove(tor, false);
}

TEST_F(RpcTest, NotificationSync)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    EXPECT_FALSE(response.has_value());
}

TEST_F(RpcTest, NotificationAsync)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_torrent_rename_path));

    auto params_map = tr_variant::Map{ 2U };
    params_map.try_emplace(TR_KEY_path, "files-filled-with-zeroes/512");
    params_map.try_emplace(TR_KEY_name, "512_test");
    request_map.try_emplace(TR_KEY_params, std::move(params_map));

    auto request = tr_variant{ std::move(request_map) };
    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(session_, request, [&promise](tr_variant&& resp) { promise.set_value(std::move(resp)); });
    auto const response = future.get();

    EXPECT_FALSE(response.has_value());

    // cleanup
    tr_torrentRemove(tor, false);
}

TEST_F(RpcTest, tagNoHandler)
{
    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "sdgdhsgg");
    request_map.try_emplace(TR_KEY_id, 12345);
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const jsonrpc = response_map->value_if<std::string_view>(TR_KEY_jsonrpc);
    ASSERT_TRUE(jsonrpc);
    EXPECT_EQ(*jsonrpc, JsonRpc::Version);
    auto const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_EQ(result, nullptr);
    auto const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, JsonRpc::Error::METHOD_NOT_FOUND);
    auto const error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Method not found"sv);
    auto const id = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id);
    EXPECT_EQ(*id, 12345);
}

TEST_F(RpcTest, tagNoHandlerLegacy)
{
    auto request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, "sdgdhsgg");
    request_map.try_emplace(TR_KEY_tag, 12345);
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const result = response_map->value_if<std::string_view>(TR_KEY_result);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, "no method name"sv);
    auto const tag = response_map->value_if<int64_t>(TR_KEY_tag);
    ASSERT_TRUE(tag);
    EXPECT_EQ(*tag, 12345);
}

TEST_F(RpcTest, batch)
{
    auto request_vec = tr_variant::Vector{};
    request_vec.reserve(8U);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    request_map.try_emplace(TR_KEY_id, 12345);
    request_vec.emplace_back(std::move(request_map));

    request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_set));
    request_vec.emplace_back(std::move(request_map));

    request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    request_map.try_emplace(TR_KEY_id, "12345"sv);
    request_vec.emplace_back(std::move(request_map));

    request_map = tr_variant::Map{ 1U };
    request_map.try_emplace(tr_quark_new("foo"sv), "boo"sv);
    request_vec.emplace_back(std::move(request_map));

    request_vec.emplace_back(1);

    request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "dnfsojnsdkjf");
    request_map.try_emplace(TR_KEY_id, 12345);
    request_vec.emplace_back(std::move(request_map));

    request_map = tr_variant::Map{ 1U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, "dnfsojnsdkjf");
    request_vec.emplace_back(std::move(request_map));

    request_map = tr_variant::Map{ 2U };
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_stats));
    request_map.try_emplace(TR_KEY_tag, 12345);
    request_vec.emplace_back(std::move(request_map));

    auto request = tr_variant{ std::move(request_vec) };
    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto* const response_vec_ptr = response.get_if<tr_variant::Vector>();
    ASSERT_NE(response_vec_ptr, nullptr);
    auto const& response_vec = *response_vec_ptr;

    ASSERT_EQ(std::size(response_vec), 6U);

    auto const* response_map = response_vec[0].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto const* result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_NE(result, nullptr);
    auto error_it = response_map->find(TR_KEY_error);
    EXPECT_EQ(error_it, std::end(*response_map));
    auto id_int = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id_int);
    EXPECT_EQ(*id_int, 12345);

    response_map = response_vec[1].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    EXPECT_NE(result, nullptr);
    error_it = response_map->find(TR_KEY_error);
    EXPECT_EQ(error_it, std::end(*response_map));
    auto id_str = response_map->value_if<std::string_view>(TR_KEY_id);
    ASSERT_TRUE(id_str);
    EXPECT_EQ(*id_str, "12345"sv);

    response_map = response_vec[2].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    auto error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    auto error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);

    response_map = response_vec[3].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    auto error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    auto error_string = error_data->value_if<std::string_view>(TR_KEY_error_string);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "request must be an Object"sv);
    id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);

    response_map = response_vec[4].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32601); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Method not found"sv);
    id_int = response_map->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(id_int);
    EXPECT_EQ(*id_int, 12345);

    response_map = response_vec[5].get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    result_it = response_map->find(TR_KEY_result);
    EXPECT_EQ(result_it, std::end(*response_map));
    error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    error_code = error->value_if<int64_t>(TR_KEY_code);
    ASSERT_TRUE(error_code);
    EXPECT_EQ(*error_code, -32600); // don't use constants here in case they are wrong
    error_message = error->value_if<std::string_view>(TR_KEY_message);
    ASSERT_TRUE(error_message);
    EXPECT_EQ(*error_message, "Invalid Request"sv);
    error_data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(error_data, nullptr);
    error_string = error_data->value_if<std::string_view>(TR_KEY_error_string);
    ASSERT_TRUE(error_string);
    EXPECT_EQ(*error_string, "JSON-RPC version is not 2.0"sv);
    id_null = response_map->value_if<std::nullptr_t>(TR_KEY_id);
    EXPECT_TRUE(id_null);
}

/***
****
***/

TEST_F(RpcTest, sessionGet)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_session_get));
    request_map.try_emplace(TR_KEY_id, 12345);
    auto request = tr_variant{ std::move(request_map) };

    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* args_map = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(args_map, nullptr);

    // what we expected
    static auto constexpr ExpectedKeysUnsorted = std::array{
        TR_KEY_alt_speed_down,
        TR_KEY_alt_speed_enabled,
        TR_KEY_alt_speed_time_begin,
        TR_KEY_alt_speed_time_day,
        TR_KEY_alt_speed_time_enabled,
        TR_KEY_alt_speed_time_end,
        TR_KEY_alt_speed_up,
        TR_KEY_anti_brute_force_enabled,
        TR_KEY_anti_brute_force_threshold,
        TR_KEY_blocklist_enabled,
        TR_KEY_blocklist_size,
        TR_KEY_blocklist_url,
        TR_KEY_cache_size_mib,
        TR_KEY_config_dir,
        TR_KEY_default_trackers,
        TR_KEY_dht_enabled,
        TR_KEY_download_dir,
        TR_KEY_download_dir_free_space,
        TR_KEY_download_queue_enabled,
        TR_KEY_download_queue_size,
        TR_KEY_encryption,
        TR_KEY_idle_seeding_limit,
        TR_KEY_idle_seeding_limit_enabled,
        TR_KEY_incomplete_dir,
        TR_KEY_incomplete_dir_enabled,
        TR_KEY_lpd_enabled,
        TR_KEY_peer_limit_global,
        TR_KEY_peer_limit_per_torrent,
        TR_KEY_peer_port,
        TR_KEY_peer_port_random_on_start,
        TR_KEY_pex_enabled,
        TR_KEY_port_forwarding_enabled,
        TR_KEY_preferred_transports,
        TR_KEY_queue_stalled_enabled,
        TR_KEY_queue_stalled_minutes,
        TR_KEY_rename_partial_files,
        TR_KEY_reqq,
        TR_KEY_rpc_version,
        TR_KEY_rpc_version_minimum,
        TR_KEY_rpc_version_semver,
        TR_KEY_script_torrent_added_enabled,
        TR_KEY_script_torrent_added_filename,
        TR_KEY_script_torrent_done_enabled,
        TR_KEY_script_torrent_done_filename,
        TR_KEY_script_torrent_done_seeding_enabled,
        TR_KEY_script_torrent_done_seeding_filename,
        TR_KEY_seed_queue_enabled,
        TR_KEY_seed_queue_size,
        TR_KEY_seed_ratio_limit,
        TR_KEY_seed_ratio_limited,
        TR_KEY_sequential_download,
        TR_KEY_session_id,
        TR_KEY_speed_limit_down,
        TR_KEY_speed_limit_down_enabled,
        TR_KEY_speed_limit_up,
        TR_KEY_speed_limit_up_enabled,
        TR_KEY_start_added_torrents,
        TR_KEY_tcp_enabled,
        TR_KEY_trash_original_torrent_files,
        TR_KEY_units,
        TR_KEY_utp_enabled,
        TR_KEY_version,
    };

    auto const expected_keys = std::set<tr_quark>{ std::begin(ExpectedKeysUnsorted), std::end(ExpectedKeysUnsorted) };

    // what we got
    std::set<tr_quark> actual_keys;
    for (auto const& key : std::views::keys(*args_map))
    {
        actual_keys.insert(key);
    }

    auto missing_keys = std::vector<tr_quark>{};
    std::ranges::set_difference(expected_keys, actual_keys, std::inserter(missing_keys, std::begin(missing_keys)));
    EXPECT_EQ(decltype(missing_keys){}, missing_keys);

    auto unexpected_keys = std::vector<tr_quark>{};
    std::ranges::set_difference(actual_keys, expected_keys, std::inserter(unexpected_keys, std::begin(unexpected_keys)));
    EXPECT_EQ(decltype(unexpected_keys){}, unexpected_keys);

    // cleanup
    tr_torrentRemove(tor, false);
}

TEST_F(RpcTest, torrentGet)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 3U };

    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_torrent_get));
    request_map.try_emplace(TR_KEY_id, 12345);

    auto params = tr_variant::Map{ 1U };
    auto fields = tr_variant::Vector{};
    fields.emplace_back(tr_quark_get_string_view(TR_KEY_id));
    params.try_emplace(TR_KEY_fields, std::move(fields));
    request_map.try_emplace(TR_KEY_params, std::move(params));

    auto request = tr_variant{ std::move(request_map) };
    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(result, nullptr);

    auto* torrents = result->find_if<tr_variant::Vector>(TR_KEY_torrents);
    ASSERT_NE(torrents, nullptr);
    EXPECT_EQ(1UL, std::size(*torrents));

    auto* first_torrent = (*torrents)[0].get_if<tr_variant::Map>();
    ASSERT_NE(first_torrent, nullptr);
    auto first_torrent_id = first_torrent->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(first_torrent_id);
    EXPECT_EQ(1, *first_torrent_id);

    // cleanup
    tr_torrentRemove(tor, false);
}

TEST_F(RpcTest, recentlyActiveEmptyOnStartup)
{
    static auto constexpr TorrentFile = LIBTRANSMISSION_TEST_ASSETS_DIR "/debian-11.2.0-amd64-DVD-1.iso.torrent"sv;
    static auto constexpr ResumeFile = LIBTRANSMISSION_TEST_ASSETS_DIR "/debian-11.2.0-amd64-DVD-1.iso.resume"sv;

    if (auto error = tr_error{};
        !tr_sys_path_copy(
            TorrentFile,
            tr_pathbuf{ session_->torrentDir(), "/c9a337562cb0360fd6f5ab40fd2b1b81d5325dbd.torrent"sv },
            &error) ||
        !tr_sys_path_copy(
            ResumeFile,
            tr_pathbuf{ session_->resumeDir(), "/c9a337562cb0360fd6f5ab40fd2b1b81d5325dbd.resume"sv },
            &error))
    {
        GTEST_SKIP() << fmt::format("Failed to setup torrents and resume dir: {} ({})", error.message(), error.code());
    }

    auto* const ctor = tr_ctorNew(session_);
    ctor->set_paused(TR_FORCE, false);
    EXPECT_EQ(tr_sessionLoadTorrents(session_, ctor), 1U);
    tr_ctorFree(ctor);

    //Query recently_active. Should be empty
    auto request_map = tr_variant::Map{ 3U };
    request_map.try_emplace(TR_KEY_jsonrpc, JsonRpc::Version);
    request_map.try_emplace(TR_KEY_method, tr_variant::unmanaged_string(TR_KEY_torrent_get));
    request_map.try_emplace(TR_KEY_id, 12345);

    auto params = tr_variant::Map{ 2U };
    auto fields = tr_variant::Vector{};
    fields.emplace_back(tr_quark_get_string_view(TR_KEY_id));
    params.try_emplace(TR_KEY_fields, std::move(fields));
    params.try_emplace(TR_KEY_ids, tr_quark_get_string_view(TR_KEY_recently_active));
    request_map.try_emplace(TR_KEY_params, std::move(params));

    auto request = tr_variant{ std::move(request_map) };
    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(result, nullptr);

    auto* torrents = result->find_if<tr_variant::Vector>(TR_KEY_torrents);
    ASSERT_NE(torrents, nullptr);
    EXPECT_EQ(0UL, std::size(*torrents));
}

TEST_F(RpcTest, torrentGetLegacy)
{
    auto* tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    EXPECT_NE(nullptr, tor);

    auto request_map = tr_variant::Map{ 1U };

    request_map.try_emplace(TR_KEY_method, tr_quark_get_string_view(TR_KEY_torrent_get_kebab));

    auto args_in = tr_variant::Map{ 1U };
    auto fields = tr_variant::Vector{};
    fields.emplace_back(tr_quark_get_string_view(TR_KEY_id));
    args_in.try_emplace(TR_KEY_fields, std::move(fields));
    request_map.try_emplace(TR_KEY_arguments, std::move(args_in));

    auto request = tr_variant{ std::move(request_map) };
    auto response = tr_variant{};
    tr_rpc_request_exec(session_, request, [&response](tr_variant&& resp) { response = std::move(resp); });

    auto* response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    auto* args_out = response_map->find_if<tr_variant::Map>(TR_KEY_arguments);
    ASSERT_NE(args_out, nullptr);

    auto* torrents = args_out->find_if<tr_variant::Vector>(TR_KEY_torrents);
    ASSERT_NE(torrents, nullptr);
    EXPECT_EQ(1UL, std::size(*torrents));

    auto* first_torrent = (*torrents)[0].get_if<tr_variant::Map>();
    ASSERT_NE(first_torrent, nullptr);
    auto first_torrent_id = first_torrent->value_if<int64_t>(TR_KEY_id);
    ASSERT_TRUE(first_torrent_id);
    EXPECT_EQ(1, *first_torrent_id);

    // cleanup
    tr_torrentRemove(tor, false);
}

namespace free_space_test
{
constexpr std::string_view BadRequest = R"json({
    "id": 39693,
    "jsonrpc": "2.0",
    "method": "free_space",
    "params": {
        "path": "this/path/is/not/absolute"
    }
})json";
constexpr std::string_view BadResponse = R"json({
    "error": {
        "code": 3,
        "data": {
            "error_string": "directory path is not absolute"
        },
        "message": "path is not absolute"
    },
    "id": 39693,
    "jsonrpc": "2.0"
})json";

TEST_F(RpcTest, relativeFreeSpaceError)
{
    auto constexpr Input = BadRequest;
    auto constexpr Expected = BadResponse;
    auto const actual = makeRequest(session_, Input);
    EXPECT_EQ(Expected, actual);
}

constexpr std::string_view BadRequestLegacy = R"json({
    "arguments": {
        "path": "this/path/is/not/absolute"
    },
    "method": "free-space",
    "tag": 39693
})json";

constexpr std::string_view BadResponseLegacy = R"json({
    "arguments": {},
    "result": "directory path is not absolute",
    "tag": 39693
})json";

TEST_F(RpcTest, relativeFreeSpaceErrorLegacy)
{
    auto constexpr Input = BadRequestLegacy;
    auto constexpr Expected = BadResponseLegacy;
    auto const actual = makeRequest(session_, Input);
    EXPECT_EQ(Expected, actual);
}

// The well-formed case is answered asynchronously: the statvfs runs on a
// detached thread and the reply is posted back to the session thread.
[[nodiscard]] tr_variant freeSpaceOf(tr_session* session, std::string_view path)
{
    auto params = tr_variant::Map{ 1U };
    params.try_emplace(TR_KEY_path, path);
    auto request_map = tr_variant::Map{ 4U };
    request_map.try_emplace(TR_KEY_jsonrpc, "2.0"sv);
    request_map.try_emplace(TR_KEY_id, 41414);
    request_map.try_emplace(TR_KEY_method, "free_space"sv);
    request_map.try_emplace(TR_KEY_params, std::move(params));
    auto request = tr_variant{ std::move(request_map) };

    auto promise = std::promise<tr_variant>{};
    auto future = promise.get_future();
    tr_rpc_request_exec(session, request, [&promise](tr_variant&& resp) { promise.set_value(std::move(resp)); });
    return future.get();
}

TEST_F(RpcTest, wellFormedFreeSpace)
{
    auto const response = freeSpaceOf(session_, sandboxDir());

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    EXPECT_EQ(41414, response_map->value_if<int64_t>(TR_KEY_id));
    EXPECT_EQ(std::end(*response_map), response_map->find(TR_KEY_error));
    auto const* const result = response_map->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(sandboxDir(), result->value_if<std::string_view>(TR_KEY_path));
    EXPECT_LE(int64_t{ 0 }, result->value_if<int64_t>(TR_KEY_size_bytes).value_or(-1));
    EXPECT_LT(int64_t{ 0 }, result->value_if<int64_t>(TR_KEY_total_size).value_or(-1));
}

TEST_F(RpcTest, freeSpaceOfMissingPathIsAnError)
{
#ifdef _WIN32
    static auto constexpr Path = "C:\\this\\path\\does\\not\\exist"sv;
#else
    static auto constexpr Path = "/this/path/does/not/exist"sv;
#endif

    auto const response = freeSpaceOf(session_, Path);

    auto const* const response_map = response.get_if<tr_variant::Map>();
    ASSERT_NE(response_map, nullptr);
    EXPECT_EQ(41414, response_map->value_if<int64_t>(TR_KEY_id));
    EXPECT_EQ(std::end(*response_map), response_map->find(TR_KEY_result));
    auto const* const error = response_map->find_if<tr_variant::Map>(TR_KEY_error);
    ASSERT_NE(error, nullptr);
    auto const* const data = error->find_if<tr_variant::Map>(TR_KEY_data);
    ASSERT_NE(data, nullptr);
    EXPECT_FALSE(std::empty(data->value_if<std::string_view>(TR_KEY_error_string).value_or(""sv)));
    auto const* const result = data->find_if<tr_variant::Map>(TR_KEY_result);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(Path, result->value_if<std::string_view>(TR_KEY_path));
    EXPECT_EQ(int64_t{ -1 }, result->value_if<int64_t>(TR_KEY_size_bytes));
    EXPECT_EQ(int64_t{ -1 }, result->value_if<int64_t>(TR_KEY_total_size));
}
} // namespace free_space_test

} // namespace tr::test
