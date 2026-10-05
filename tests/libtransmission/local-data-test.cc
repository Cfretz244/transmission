// This file Copyright (C) 2026 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/error.h>
#include <libtransmission/inout.h>
#include <libtransmission/local-data.h>
#include <libtransmission/torrent-files.h>

using namespace std::literals;

namespace
{

auto constexpr WaitTimeout = 10s;

[[nodiscard]] tr_io_plan make_plan(tr_torrent_id_t const tor_id, tr_byte_span_t const byte_span)
{
    auto plan = tr_io_plan{};
    plan.tor_id = tor_id;
    plan.byte_span = byte_span;
    return plan;
}

[[nodiscard]] tr_io_result make_result(tr_error_code_t const err)
{
    auto result = tr_io_result{};
    if (err != 0)
    {
        result.error.set(err, "stub error");
    }
    return result;
}

// A backend that records every call, and can block calls on a gate so a
// test can control when each operation finishes.
class StubBackend final : public tr::LocalData::Backend
{
public:
    [[nodiscard]] tr_io_result read(tr_io_plan const& plan, tr::LocalData::BlockData& setme) override
    {
        auto const guard = Running{ *this, plan.tor_id, "read" };
        read_span = plan.byte_span;
        setme.assign({ uint8_t{ 1U }, uint8_t{ 2U }, uint8_t{ 3U } });
        return make_result(read_err);
    }

    [[nodiscard]] tr_io_result test_piece(tr_io_plan const& plan, tr_sha1_digest_t& setme_hash) override
    {
        auto const guard = Running{ *this, plan.tor_id, "test" };
        tested_span = plan.byte_span;
        setme_hash = hash;
        return make_result(test_err);
    }

    [[nodiscard]] tr_io_result write(tr_io_plan const& plan, tr::LocalData::BlockData const& data) override
    {
        auto const guard = Running{ *this, plan.tor_id, "write" };
        write_span = plan.byte_span;
        last_write.assign(std::begin(data), std::end(data));
        auto result = make_result(write_err);
        result.created_file = write_creates_file;
        return result;
    }

    [[nodiscard]] tr_error_code_t move(
        tr_torrent_id_t tor_id,
        [[maybe_unused]] tr_torrent_files const& files,
        std::string_view old_parent,
        std::string_view parent,
        std::string_view parent_name) override
    {
        auto const guard = Running{ *this, tor_id, "move" };
        moved_from = std::string{ old_parent };
        moved_to = std::string{ parent };
        moved_name = std::string{ parent_name };
        return move_err;
    }

    [[nodiscard]] tr_error remove(
        tr_torrent_id_t tor_id,
        [[maybe_unused]] tr_torrent_files const& files,
        std::string_view parent,
        std::string_view name,
        [[maybe_unused]] tr_torrent_remove_func const& remove_func) override
    {
        auto const guard = Running{ *this, tor_id, "remove" };
        removed_parent = std::string{ parent };
        removed_name = std::string{ name };
        remove_called = true;
        return make_result(remove_err).error;
    }

    [[nodiscard]] tr_error_code_t rename(
        tr_torrent_id_t tor_id,
        std::string_view base,
        std::string_view oldpath,
        std::string_view newname) override
    {
        auto const guard = Running{ *this, tor_id, "rename" };
        renamed_base = std::string{ base };
        renamed_from = std::string{ oldpath };
        renamed_to = std::string{ newname };
        return rename_err;
    }

    void close_all() override
    {
        close_all_called = true;
    }

    void close_torrent(tr_torrent_id_t tor_id) override
    {
        auto const guard = Running{ *this, tor_id, "close_torrent" };
        closed_torrent = tor_id;
    }

    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num) override
    {
        auto const guard = Running{ *this, tor_id, "close_file" };
        closed_file = std::pair{ tor_id, file_num };
    }

    // --- gate control

    // Block every call for `tor_id` until release() is called.
    void hold(tr_torrent_id_t tor_id)
    {
        auto const lock = std::lock_guard{ mutex_ };
        held_.insert(tor_id);
    }

    void release(tr_torrent_id_t tor_id)
    {
        {
            auto const lock = std::lock_guard{ mutex_ };
            held_.erase(tor_id);
        }
        cv_.notify_all();
    }

    // Wait until a call for `tor_id` has started (and is possibly blocked).
    bool wait_until_running(tr_torrent_id_t tor_id)
    {
        auto lock = std::unique_lock{ mutex_ };
        return cv_.wait_for(lock, WaitTimeout, [this, tor_id]() { return running_.contains(tor_id); });
    }

    [[nodiscard]] std::vector<std::string> log() const
    {
        auto const lock = std::lock_guard{ mutex_ };
        return log_;
    }

    [[nodiscard]] size_t max_concurrent_per_torrent() const
    {
        auto const lock = std::lock_guard{ mutex_ };
        return max_concurrent_per_torrent_;
    }

    [[nodiscard]] size_t max_concurrent_total() const
    {
        auto const lock = std::lock_guard{ mutex_ };
        return max_concurrent_total_;
    }

    tr_error_code_t read_err = 0;
    tr_error_code_t test_err = 0;
    tr_error_code_t write_err = 0;
    tr_error_code_t move_err = 0;
    tr_error_code_t remove_err = 0;
    tr_error_code_t rename_err = 0;
    std::atomic<bool> remove_called = false;
    std::atomic<bool> close_all_called = false;
    tr_byte_span_t read_span{};
    tr_byte_span_t write_span{};
    tr_byte_span_t tested_span{};
    bool write_creates_file = false;
    tr_sha1_digest_t hash = tr_sha1::digest("local-data-test"sv);
    std::vector<uint8_t> last_write;
    std::string moved_from;
    std::string moved_to;
    std::string moved_name;
    std::string removed_parent;
    std::string removed_name;
    std::string renamed_base;
    std::string renamed_from;
    std::string renamed_to;
    std::atomic<tr_torrent_id_t> closed_torrent = -1;
    std::optional<std::pair<tr_torrent_id_t, tr_file_index_t>> closed_file;

private:
    struct Running
    {
        Running(StubBackend& backend, tr_torrent_id_t tor_id, std::string_view op)
            : backend_{ backend }
            , tor_id_{ tor_id }
        {
            auto lock = std::unique_lock{ backend_.mutex_ };
            backend_.log_.emplace_back(std::to_string(tor_id) + ':' + std::string{ op });
            auto const n = ++backend_.running_[tor_id];
            backend_.max_concurrent_per_torrent_ = std::max(backend_.max_concurrent_per_torrent_, n);
            ++backend_.n_running_;
            backend_.max_concurrent_total_ = std::max(backend_.max_concurrent_total_, backend_.n_running_);
            backend_.cv_.notify_all();
            backend_.cv_.wait_for(lock, WaitTimeout, [this]() { return !backend_.held_.contains(tor_id_); });
        }

        ~Running()
        {
            {
                auto const lock = std::lock_guard{ backend_.mutex_ };
                if (--backend_.running_[tor_id_] == 0U)
                {
                    backend_.running_.erase(tor_id_);
                }
                --backend_.n_running_;
            }
            backend_.cv_.notify_all();
        }

        StubBackend& backend_;
        tr_torrent_id_t tor_id_;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::set<tr_torrent_id_t> held_;
    std::map<tr_torrent_id_t, size_t> running_;
    std::vector<std::string> log_;
    size_t n_running_ = 0;
    size_t max_concurrent_per_torrent_ = 0;
    size_t max_concurrent_total_ = 0;
};

// Counts how many callbacks went through the dispatcher.
struct CountingDispatcher
{
    std::shared_ptr<std::atomic<size_t>> count = std::make_shared<std::atomic<size_t>>(0U);

    void operator()(std::function<void()> func) const
    {
        ++*count;
        func();
    }
};

template<typename T>
[[nodiscard]] bool wait_for(std::future<T>& future)
{
    return future.wait_for(WaitTimeout) == std::future_status::ready;
}

} // namespace

TEST(LocalData, ReadCompletesThroughDispatcher)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto dispatcher = CountingDispatcher{};
    auto local_data = tr::LocalData{ std::move(backend), dispatcher, 1U };

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    local_data.read(
        make_plan(7, { .begin = 10U, .end = 13U }),
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_byte_span_t byte_span, tr_error const& error, auto data)
        {
            EXPECT_EQ(7, tor_id);
            EXPECT_EQ(raw_backend->read_span.begin, byte_span.begin);
            EXPECT_EQ(raw_backend->read_span.end, byte_span.end);
            EXPECT_FALSE(error);
            ASSERT_NE(nullptr, data);
            EXPECT_EQ((std::vector<uint8_t>{ 1U, 2U, 3U }), std::vector<uint8_t>(std::begin(*data), std::end(*data)));
            done.set_value();
        });

    ASSERT_TRUE(wait_for(done_future));
    EXPECT_EQ(1U, *dispatcher.count);
}

TEST(LocalData, ReadErrorYieldsNoData)
{
    auto backend = std::make_unique<StubBackend>();
    backend->read_err = EIO;
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    local_data.read(
        make_plan(7, { .begin = 10U, .end = 13U }),
        [&done](tr_torrent_id_t, tr_byte_span_t, tr_error const& error, auto data)
        {
            EXPECT_TRUE(error);
            EXPECT_EQ(EIO, error.code());
            EXPECT_EQ(nullptr, data);
            done.set_value();
        });

    ASSERT_TRUE(wait_for(done_future));
}

TEST(LocalData, TestPieceReportsHash)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    local_data.test_piece(
        make_plan(9, { .begin = 30U, .end = 40U }),
        3,
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_piece_index_t piece, tr_error const& error, auto hash)
        {
            EXPECT_EQ(9, tor_id);
            EXPECT_EQ(3U, piece);
            EXPECT_EQ(30U, raw_backend->tested_span.begin);
            EXPECT_EQ(40U, raw_backend->tested_span.end);
            EXPECT_FALSE(error);
            ASSERT_TRUE(hash.has_value());
            EXPECT_EQ(raw_backend->hash, *hash);
            done.set_value();
        });

    ASSERT_TRUE(wait_for(done_future));
}

TEST(LocalData, WriteDeliversDataAndAccountsBytes)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(11);
    backend->write_creates_file = true;
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 4U }, uint8_t{ 5U }, uint8_t{ 6U } });

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    local_data.write(
        make_plan(11, { .begin = 20U, .end = 23U }),
        std::move(data),
        [&done, raw_backend](tr_torrent_id_t tor_id, tr_byte_span_t byte_span, tr_error const& error, bool created_file)
        {
            EXPECT_EQ(11, tor_id);
            EXPECT_EQ(raw_backend->write_span.begin, byte_span.begin);
            EXPECT_EQ(raw_backend->write_span.end, byte_span.end);
            EXPECT_FALSE(error);
            EXPECT_TRUE(created_file);
            EXPECT_EQ((std::vector<uint8_t>{ 4U, 5U, 6U }), raw_backend->last_write);
            done.set_value();
        });

    // the write is queued or blocked in the backend, so its bytes are still pending
    ASSERT_TRUE(raw_backend->wait_until_running(11));
    EXPECT_EQ(3U, local_data.enqueued_write_bytes());
    EXPECT_EQ(3U, local_data.enqueued_write_bytes(11));
    EXPECT_EQ(0U, local_data.enqueued_write_bytes(12));

    raw_backend->release(11);
    ASSERT_TRUE(wait_for(done_future));
    local_data.close_all(); // waits for the worker to finish bookkeeping
    EXPECT_EQ(0U, local_data.enqueued_write_bytes());
    EXPECT_EQ(0U, local_data.enqueued_write_bytes(11));
}

TEST(LocalData, InvalidWriteFailsWithoutTouchingBackend)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    local_data.write(
        make_plan(11, { .begin = 20U, .end = 23U }),
        nullptr,
        [&done](tr_torrent_id_t, tr_byte_span_t, tr_error const& error, bool /*created_file*/)
        {
            EXPECT_EQ(EINVAL, error.code());
            done.set_value();
        });

    ASSERT_TRUE(wait_for(done_future));
    local_data.close_all();
    EXPECT_TRUE(raw_backend->log().empty());
}

TEST(LocalData, AdminOperationsDelegate)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto move_done = std::promise<void>{};
    auto move_future = move_done.get_future();
    local_data.move(
        5,
        {},
        "/old",
        "/new",
        "name",
        [&move_done](tr_torrent_id_t tor_id, tr_error const& error)
        {
            EXPECT_EQ(5, tor_id);
            EXPECT_FALSE(error);
            move_done.set_value();
        });
    ASSERT_TRUE(wait_for(move_future));
    EXPECT_EQ("/old", raw_backend->moved_from);
    EXPECT_EQ("/new", raw_backend->moved_to);
    EXPECT_EQ("name", raw_backend->moved_name);

    auto rename_done = std::promise<void>{};
    auto rename_future = rename_done.get_future();
    local_data.rename(
        8,
        "/base",
        "old",
        "new",
        [&rename_done](tr_torrent_id_t tor_id, std::string_view oldpath, std::string_view newname, tr_error const& error)
        {
            EXPECT_EQ(8, tor_id);
            EXPECT_EQ("old", oldpath);
            EXPECT_EQ("new", newname);
            EXPECT_FALSE(error);
            rename_done.set_value();
        });
    ASSERT_TRUE(wait_for(rename_future));
    EXPECT_EQ("/base", raw_backend->renamed_base);
    EXPECT_EQ("old", raw_backend->renamed_from);
    EXPECT_EQ("new", raw_backend->renamed_to);

    auto remove_done = std::promise<void>{};
    auto remove_future = remove_done.get_future();
    local_data.remove(
        12,
        {},
        "/parent",
        "name",
        {},
        [&remove_done](tr_torrent_id_t tor_id, tr_error const& error)
        {
            EXPECT_EQ(12, tor_id);
            EXPECT_FALSE(error);
            remove_done.set_value();
        });
    ASSERT_TRUE(wait_for(remove_future));
    EXPECT_EQ("/parent", raw_backend->removed_parent);
    EXPECT_EQ("name", raw_backend->removed_name);
    local_data.close_file(13, 2);
    local_data.close_torrent(14);
    local_data.close_all();

    EXPECT_TRUE(raw_backend->remove_called);
    ASSERT_TRUE(raw_backend->closed_file.has_value());
    EXPECT_EQ(13, raw_backend->closed_file->first);
    EXPECT_EQ(2U, raw_backend->closed_file->second);
    EXPECT_EQ(14, raw_backend->closed_torrent);
    EXPECT_TRUE(raw_backend->close_all_called);

    // a move or rename closes the torrent's files before touching them
    auto const log = raw_backend->log();
    auto const expected = std::vector<std::string>{ "5:close_torrent", "5:move",  "8:close_torrent", "8:rename",
                                                    "12:close_torrent", "12:remove", "13:close_file",    "14:close_torrent" };
    EXPECT_EQ(expected, log);

    local_data.shutdown();
}

TEST(LocalData, SameTorrentRunsInOrderOneAtATime)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    auto local_data = tr::LocalData{ std::move(backend), {}, 4U };

    auto completions = std::vector<std::string>{};
    auto completions_mutex = std::mutex{};
    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    auto n_remaining = std::atomic<int>{ 4 };
    auto const on_done = [&](std::string name)
    {
        {
            auto const lock = std::lock_guard{ completions_mutex };
            completions.emplace_back(std::move(name));
        }
        if (--n_remaining == 0)
        {
            done.set_value();
        }
    };

    local_data.read(make_plan(1, { .begin = 0U, .end = 3U }), [&](auto, auto, auto&, auto) { on_done("read1"); });
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 1U } });
    local_data.write(
        make_plan(1, { .begin = 0U, .end = 1U }),
        std::move(data),
        [&](auto, auto, auto&, auto) { on_done("write"); });
    local_data.move(1, {}, "/a", "/b", "n", [&](auto, auto&) { on_done("move"); });
    local_data.read(make_plan(1, { .begin = 3U, .end = 6U }), [&](auto, auto, auto&, auto) { on_done("read2"); });

    // the first read is blocked in the backend; nothing else may start
    ASSERT_TRUE(raw_backend->wait_until_running(1));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ((std::vector<std::string>{ "1:read" }), raw_backend->log());

    raw_backend->release(1);
    ASSERT_TRUE(wait_for(done_future));
    local_data.close_all();

    EXPECT_EQ((std::vector<std::string>{ "read1", "write", "move", "read2" }), completions);
    EXPECT_EQ((std::vector<std::string>{ "1:read", "1:write", "1:close_torrent", "1:move", "1:read" }), raw_backend->log());
    EXPECT_EQ(1U, raw_backend->max_concurrent_per_torrent());
}

TEST(LocalData, DifferentTorrentsRunConcurrently)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    backend->hold(2);
    auto local_data = tr::LocalData{ std::move(backend), {}, 2U };

    auto done = std::promise<void>{};
    auto done_future = done.get_future();
    auto n_remaining = std::atomic<int>{ 2 };
    auto const on_done = [&](auto, auto, auto&, auto)
    {
        if (--n_remaining == 0)
        {
            done.set_value();
        }
    };

    local_data.read(make_plan(1, { .begin = 0U, .end = 3U }), on_done);
    local_data.read(make_plan(2, { .begin = 0U, .end = 3U }), on_done);

    // both reads are blocked in the backend at the same time
    ASSERT_TRUE(raw_backend->wait_until_running(1));
    ASSERT_TRUE(raw_backend->wait_until_running(2));
    EXPECT_EQ(2U, raw_backend->max_concurrent_total());

    raw_backend->release(1);
    raw_backend->release(2);
    ASSERT_TRUE(wait_for(done_future));
}

TEST(LocalData, RemoveDiscardsQueuedWorkForThatTorrent)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto first_read = std::promise<tr_error_code_t>{};
    auto first_read_future = first_read.get_future();
    local_data.read(
        make_plan(1, { .begin = 0U, .end = 3U }),
        [&](auto, auto, tr_error const& error, auto) { first_read.set_value(error.code()); });
    ASSERT_TRUE(raw_backend->wait_until_running(1));

    auto write_result = std::promise<tr_error_code_t>{};
    auto write_future = write_result.get_future();
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 1U } });
    local_data.write(
        make_plan(1, { .begin = 0U, .end = 1U }),
        std::move(data),
        [&](auto, auto, tr_error const& error, auto) { write_result.set_value(error.code()); });

    auto move_result = std::promise<tr_error_code_t>{};
    auto move_future = move_result.get_future();
    local_data.move(1, {}, "/a", "/b", "n", [&](auto, tr_error const& error) { move_result.set_value(error.code()); });

    auto other_read = std::promise<tr_error_code_t>{};
    auto other_read_future = other_read.get_future();
    local_data.read(
        make_plan(2, { .begin = 0U, .end = 3U }),
        [&](auto, auto, tr_error const& error, auto) { other_read.set_value(error.code()); });

    local_data.remove(1, {}, "/parent", "name", {}, {});

    // the queued write and move were discarded; the other torrent is untouched
    ASSERT_TRUE(wait_for(write_future));
    EXPECT_EQ(ECANCELED, write_future.get());
    ASSERT_TRUE(wait_for(move_future));
    EXPECT_EQ(ECANCELED, move_future.get());
    EXPECT_EQ(0U, local_data.enqueued_write_bytes());

    raw_backend->release(1);
    ASSERT_TRUE(wait_for(first_read_future));
    EXPECT_EQ(0, first_read_future.get());
    ASSERT_TRUE(wait_for(other_read_future));
    EXPECT_EQ(0, other_read_future.get());
    local_data.close_all();

    EXPECT_TRUE(raw_backend->remove_called);
    EXPECT_EQ((std::vector<std::string>{ "1:read", "1:close_torrent", "1:remove", "2:read" }), raw_backend->log());
}

TEST(LocalData, ForgetDiscardsQueuedWorkButKeepsRemove)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto first_read = std::promise<tr_error_code_t>{};
    auto first_read_future = first_read.get_future();
    local_data.read(
        make_plan(1, { .begin = 0U, .end = 3U }),
        [&](auto, auto, tr_error const& error, auto) { first_read.set_value(error.code()); });
    ASSERT_TRUE(raw_backend->wait_until_running(1));

    auto remove_result = std::promise<tr_error_code_t>{};
    auto remove_future = remove_result.get_future();
    local_data.remove(
        1,
        {},
        "/parent",
        "name",
        {},
        [&](auto, tr_error const& error) { remove_result.set_value(error.code()); });

    auto write_result = std::promise<tr_error_code_t>{};
    auto write_future = write_result.get_future();
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 1U } });
    local_data.write(
        make_plan(1, { .begin = 0U, .end = 1U }),
        std::move(data),
        [&](auto, auto, tr_error const& error, auto) { write_result.set_value(error.code()); });

    auto move_result = std::promise<tr_error_code_t>{};
    auto move_future = move_result.get_future();
    local_data.move(1, {}, "/a", "/b", "n", [&](auto, tr_error const& error) { move_result.set_value(error.code()); });

    auto test_result = std::promise<tr_error_code_t>{};
    auto test_future = test_result.get_future();
    local_data.test_piece(
        make_plan(1, { .begin = 0U, .end = 3U }),
        0U,
        [&](auto, auto, tr_error const& error, auto) { test_result.set_value(error.code()); });

    local_data.forget(1);

    // the queued write, move and test were discarded, but the remove still runs
    ASSERT_TRUE(wait_for(write_future));
    EXPECT_EQ(ECANCELED, write_future.get());
    ASSERT_TRUE(wait_for(move_future));
    EXPECT_EQ(ECANCELED, move_future.get());
    ASSERT_TRUE(wait_for(test_future));
    EXPECT_EQ(ECANCELED, test_future.get());
    EXPECT_EQ(0U, local_data.enqueued_write_bytes());

    raw_backend->release(1);
    ASSERT_TRUE(wait_for(first_read_future));
    EXPECT_EQ(0, first_read_future.get());
    ASSERT_TRUE(wait_for(remove_future));
    EXPECT_EQ(0, remove_future.get());
    local_data.close_all();

    EXPECT_EQ((std::vector<std::string>{ "1:read", "1:close_torrent", "1:remove" }), raw_backend->log());
}

// A piece check queued behind a write must survive shutdown: cancelling
// it would leave a piece on disk that is never verified and never saved.
TEST(LocalData, ShutdownDrainsPieceTests)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto write_result = std::promise<tr_error_code_t>{};
    auto write_future = write_result.get_future();
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 1U } });
    local_data.write(
        make_plan(1, { .begin = 0U, .end = 1U }),
        std::move(data),
        [&](auto, auto, tr_error const& error, auto) { write_result.set_value(error.code()); });
    ASSERT_TRUE(raw_backend->wait_until_running(1));

    auto test_result = std::promise<tr_error_code_t>{};
    auto test_future = test_result.get_future();
    local_data.test_piece(
        make_plan(1, { .begin = 0U, .end = 3U }),
        0U,
        [&](auto, auto, tr_error const& error, auto) { test_result.set_value(error.code()); });

    auto releaser = std::thread(
        [raw_backend]()
        {
            std::this_thread::sleep_for(100ms);
            raw_backend->release(1);
        });
    local_data.shutdown();
    releaser.join();

    ASSERT_TRUE(wait_for(write_future));
    EXPECT_EQ(0, write_future.get());
    ASSERT_TRUE(wait_for(test_future));
    EXPECT_EQ(0, test_future.get());
    EXPECT_EQ((std::vector<std::string>{ "1:write", "1:test" }), raw_backend->log());
}

TEST(LocalData, ShutdownDrainsWritesAndCancelsReads)
{
    auto backend = std::make_unique<StubBackend>();
    auto* raw_backend = backend.get();
    backend->hold(1);
    auto local_data = tr::LocalData{ std::move(backend), {}, 1U };

    auto write_result = std::promise<tr_error_code_t>{};
    auto write_future = write_result.get_future();
    auto data = std::make_unique<tr::LocalData::BlockData>();
    data->assign({ uint8_t{ 1U } });
    local_data.write(
        make_plan(1, { .begin = 0U, .end = 1U }),
        std::move(data),
        [&](auto, auto, tr_error const& error, auto) { write_result.set_value(error.code()); });
    ASSERT_TRUE(raw_backend->wait_until_running(1));

    auto read_result = std::promise<tr_error_code_t>{};
    auto read_future = read_result.get_future();
    local_data.read(
        make_plan(1, { .begin = 0U, .end = 3U }),
        [&](auto, auto, tr_error const& error, auto) { read_result.set_value(error.code()); });

    // shutdown blocks until the held write finishes, so release it from another thread
    auto releaser = std::thread(
        [raw_backend]()
        {
            std::this_thread::sleep_for(100ms);
            raw_backend->release(1);
        });
    local_data.shutdown();
    releaser.join();

    ASSERT_TRUE(wait_for(write_future));
    EXPECT_EQ(0, write_future.get());
    ASSERT_TRUE(wait_for(read_future));
    EXPECT_EQ(ECANCELED, read_future.get());

    // the read never reached the backend
    EXPECT_EQ((std::vector<std::string>{ "1:write" }), raw_backend->log());
}
