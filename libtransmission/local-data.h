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
#include "libtransmission/types.h"

class tr_open_files;
class tr_torrents;

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
 */
class LocalData
{
public:
    using BlockData = small::max_size_vector<uint8_t, TrBlockSize>;

    using OnRead = std::function<
        void(tr_torrent_id_t, tr_byte_span_t byte_span, tr_error const& error, std::unique_ptr<BlockData> data)>;

    using OnTest = std::function<
        void(tr_torrent_id_t, tr_piece_index_t piece, tr_error const& error, std::optional<tr_sha1_digest_t> hash)>;

    using OnWrite = std::function<void(tr_torrent_id_t, tr_byte_span_t byte_span, tr_error const& error)>;

    using OnMove = std::function<void(tr_torrent_id_t, tr_error const& error)>;

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

        [[nodiscard]] virtual tr_error_code_t read(tr_torrent_id_t tor_id, tr_byte_span_t byte_span, BlockData& setme) = 0;
        [[nodiscard]] virtual tr_error_code_t test_piece(
            tr_torrent_id_t tor_id,
            tr_piece_index_t piece,
            tr_sha1_digest_t& setme_hash) = 0;
        [[nodiscard]] virtual tr_error_code_t write(
            tr_torrent_id_t tor_id,
            tr_byte_span_t byte_span,
            BlockData const& data) = 0;
        [[nodiscard]] virtual tr_error_code_t move(
            tr_torrent_id_t id,
            std::string_view old_parent,
            std::string_view parent,
            std::string_view parent_name) = 0;
        [[nodiscard]] virtual tr_error_code_t remove(tr_torrent_id_t id, tr_torrent_remove_func remove_func) = 0;
        [[nodiscard]] virtual tr_error_code_t rename(tr_torrent_id_t id, std::string_view oldpath, std::string_view newname) = 0;
        virtual void close_all() = 0;
        virtual void close_torrent(tr_torrent_id_t tor_id) = 0;
        virtual void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num) = 0;
    };

    // `worker_count == 0` picks a default based on hardware concurrency.
    LocalData(tr_torrents const& torrents, tr_open_files& open_files, Dispatcher dispatcher, size_t worker_count = {});
    LocalData(std::unique_ptr<Backend> backend, Dispatcher dispatcher, size_t worker_count = {});

    LocalData(LocalData const&) = delete;
    LocalData(LocalData&&) = delete;
    LocalData& operator=(LocalData const&) = delete;
    LocalData& operator=(LocalData&&) = delete;

    ~LocalData();

    // Read a block.
    void read(tr_torrent_id_t id, tr_byte_span_t byte_span, OnRead on_read);

    // Read a piece and report its SHA1 checksum.
    void test_piece(tr_torrent_id_t id, tr_piece_index_t piece, OnTest on_test);

    // Write a block.
    void write(tr_torrent_id_t id, tr_byte_span_t byte_span, std::unique_ptr<BlockData> data, OnWrite on_write);

    // Close a torrent's files, e.g. so a finished download reopens them read-only.
    void close_torrent(tr_torrent_id_t tor_id);

    // Close one of a torrent's files.
    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num);

    // Blocks until every queued operation has run, then closes all open files.
    void close_all();

    // See tr_torrent_files::move()
    void move(
        tr_torrent_id_t id,
        std::string_view old_parent,
        std::string_view parent,
        std::string_view parent_name,
        OnMove on_move);

    // See tr_torrent_files::remove(). Discards the torrent's queued reads,
    // writes, moves and renames first; their callbacks get ECANCELED.
    void remove(tr_torrent_id_t id, tr_torrent_remove_func remove_func);

    // See tr_torrentRenamePath()
    void rename(tr_torrent_id_t id, std::string_view oldpath, std::string_view newname, tr_torrent_rename_done_func callback);

    // Stops accepting reads and tests, cancels the queued ones, and blocks
    // until every queued write, move, rename and remove has run.
    void shutdown();

    // Bytes in queued writes that have not reached the backend yet.
    [[nodiscard]] uint64_t enqueued_write_bytes() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tr
