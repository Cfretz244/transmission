// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <cstdint> // uintX_t
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <small/vector.hpp>

#include "libtransmission/constants.h"
#include "libtransmission/error-types.h"
#include "libtransmission/inout.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/types.h"

class tr_open_files;

namespace tr
{

/**
 * Async access to the files that hold a torrent's payload.
 *
 * Every operation is queued and executed on a pool of worker threads
 * owned by this object, so the caller never blocks on disk I/O:
 *
 * - Requests for different torrents may run in parallel.
 * - Requests for the same torrent run one at a time, in the order they
 *   were enqueued. A move, rename, or remove therefore never overlaps a
 *   read or write of the same torrent's files.
 *
 * Threading contract for completion callbacks: every callback is handed
 * to the `Dispatcher` given at construction. The session passes a
 * dispatcher that runs the callback on the session thread, so callers
 * inside libtransmission may touch torrent and session state from a
 * callback without taking a lock or re-posting to the session thread.
 * A unit test may pass a dispatcher that runs the callback inline, in
 * which case it runs on the worker thread.
 *
 * Workers never touch a `tr_torrent`, `tr_torrents` or `tr_session`:
 * the session thread may change or free a torrent while its I/O runs.
 * Callers instead pass a snapshot of the torrent state that a task needs
 * (a `tr_io_plan`, or a copy of `tr_torrent_files`), taken when they
 * enqueue it.
 */
class LocalData
{
public:
    using BlockData = small::max_size_vector<uint8_t, TrBlockSize>;

    using OnRead = std::function<
        void(tr_torrent_id_t, tr_byte_span_t byte_span, tr_error const& error, std::unique_ptr<BlockData> data)>;

    using OnTest = std::function<
        void(tr_torrent_id_t, tr_piece_index_t piece, tr_error const& error, std::optional<tr_sha1_digest_t> hash)>;

    // `created_file` is true if the write created a file that did not exist.
    using OnWrite = std::function<
        void(tr_torrent_id_t, tr_byte_span_t byte_span, tr_error const& error, bool created_file)>;

    using OnMove = std::function<void(tr_torrent_id_t, tr_error const& error)>;

    using OnRemove = std::function<void(tr_torrent_id_t, tr_error const& error)>;

    // Runs a completion callback. See the class comment for the contract.
    using Dispatcher = std::function<void(std::function<void()>)>;

    /**
     * Synchronous file operations. Called only from worker threads.
     * Production uses the default backend; tests inject mocks.
     */
    class Backend
    {
    public:
        virtual ~Backend() = default;

        [[nodiscard]] virtual tr_io_result read(tr_io_plan const& plan, BlockData& setme) = 0;
        [[nodiscard]] virtual tr_io_result test_piece(tr_io_plan const& plan, tr_sha1_digest_t& setme_hash) = 0;
        [[nodiscard]] virtual tr_io_result write(tr_io_plan const& plan, BlockData const& data) = 0;
        [[nodiscard]] virtual tr_error_code_t move(
            tr_torrent_id_t id,
            tr_torrent_files const& files,
            std::string_view old_parent,
            std::string_view parent,
            std::string_view parent_name) = 0;
        [[nodiscard]] virtual tr_error remove(
            tr_torrent_id_t id,
            tr_torrent_files const& files,
            std::string_view parent,
            std::string_view name,
            tr_torrent_remove_func const& remove_func) = 0;
        [[nodiscard]] virtual tr_error_code_t rename(
            tr_torrent_id_t id,
            std::string_view base,
            std::string_view oldpath,
            std::string_view newname) = 0;
        virtual void close_all() = 0;
        virtual void close_torrent(tr_torrent_id_t tor_id) = 0;
        virtual void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num) = 0;
    };

    // `worker_count == 0` picks a default based on hardware concurrency.
    LocalData(tr_open_files& open_files, Dispatcher dispatcher, size_t worker_count = {});
    LocalData(std::unique_ptr<Backend> backend, Dispatcher dispatcher, size_t worker_count = {});

    LocalData(LocalData const&) = delete;
    LocalData(LocalData&&) = delete;
    LocalData& operator=(LocalData const&) = delete;
    LocalData& operator=(LocalData&&) = delete;

    ~LocalData();

    // Read a block. `plan` covers the block's span.
    // A failed read's log message is logged from the dispatcher.
    void read(tr_io_plan plan, OnRead on_read);

    // Read a piece and report its SHA1 checksum. `plan` covers the piece's span.
    // A failed read's log message is logged from the dispatcher.
    void test_piece(tr_io_plan plan, tr_piece_index_t piece, OnTest on_test);

    // Write a block. `plan` covers the block's span.
    // A failed write's log message is logged from the dispatcher.
    void write(tr_io_plan plan, std::unique_ptr<BlockData> data, OnWrite on_write);

    // Close a torrent's files, e.g. so a finished download reopens them read-only.
    void close_torrent(tr_torrent_id_t tor_id);

    // Close one of a torrent's files.
    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num);

    // Blocks until every queued operation has run, then closes all open files.
    void close_all();

    // See tr_torrent_files::move()
    void move(
        tr_torrent_id_t id,
        tr_torrent_files files,
        std::string_view old_parent,
        std::string_view parent,
        std::string_view parent_name,
        OnMove on_move);

    // See tr_torrent_files::remove(). Runs after the torrent's in-flight
    // task, so no write can recreate a file after it is deleted.
    // Discards the torrent's queued work first, as forget() does.
    // `on_remove` may be empty.
    void remove(
        tr_torrent_id_t id,
        tr_torrent_files files,
        std::string_view parent,
        std::string_view name,
        tr_torrent_remove_func remove_func,
        OnRemove on_remove);

    // See tr_torrentRenamePath(). `base` is the directory that holds the torrent's files.
    void rename(
        tr_torrent_id_t id,
        std::string_view base,
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback);

    // Discards the torrent's queued reads, tests, writes, moves and renames;
    // their callbacks get ECANCELED. Queued closes and removes still run.
    // Call when the torrent is freed, so its files are not touched after it is gone.
    void forget(tr_torrent_id_t id);

    // Stops accepting reads and tests, cancels the queued ones, and blocks
    // until every queued write, move, rename and remove has run.
    void shutdown();

    // Bytes in queued writes that have not reached the backend yet.
    [[nodiscard]] uint64_t enqueued_write_bytes() const;
    [[nodiscard]] uint64_t enqueued_write_bytes(tr_torrent_id_t id) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr
