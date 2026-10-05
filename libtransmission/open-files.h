// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // for size_t
#include <cstdint> // for uintX_t
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "libtransmission/file.h" // tr_sys_file_t
#include "libtransmission/types.h"

// A pool of open files that are cached while reading / writing torrents' data.
// Thread-safe: LocalData's worker threads share one pool.
class tr_open_files
{
    struct File;

public:
    // Keeps one file descriptor open while its holder reads or writes it.
    // The pool never evicts a leased file, and close_*() on a leased file
    // only uncaches it: the fd is closed when the last lease is released.
    // This guarantees the fd number cannot be reused for another file
    // while a pread() / pwrite() on it is in flight.
    class Lease
    {
    public:
        Lease() noexcept = default;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&&) noexcept = default;
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        ~Lease() = default;

        // TR_BAD_SYS_FILE if the lease is empty
        [[nodiscard]] tr_sys_file_t fd() const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return file_ != nullptr;
        }

    private:
        friend class tr_open_files;

        explicit Lease(std::shared_ptr<File> file) noexcept
            : file_{ std::move(file) }
        {
        }

        std::shared_ptr<File> file_;
    };

    static constexpr size_t DefaultMaxOpenFiles = 32U;

    explicit tr_open_files(size_t max_open_files = DefaultMaxOpenFiles);

    // Returns an empty lease if the file is not cached.
    [[nodiscard]] Lease get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable);

    // Returns an empty lease if the file cannot be opened.
    [[nodiscard]] Lease get(
        tr_torrent_id_t tor_id,
        tr_file_index_t file_num,
        bool writable,
        std::string_view filename,
        tr_file_preallocation allocation,
        uint64_t file_size);

    void close_all();
    void close_torrent(tr_torrent_id_t tor_id);
    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num);

private:
    using Key = std::pair<tr_torrent_id_t, tr_file_index_t>;

    [[nodiscard]] static Key make_key(tr_torrent_id_t tor_id, tr_file_index_t file_num) noexcept
    {
        return std::make_pair(tor_id, file_num);
    }

    struct Slot
    {
        Key key = {};
        std::shared_ptr<File> file; // nullptr if the slot is empty
        uint64_t last_used = 0U;
    };

    [[nodiscard]] Slot* find(Key const& key) noexcept;
    [[nodiscard]] std::shared_ptr<File> add(Key const& key, std::shared_ptr<File> file);

    std::mutex mutex_;
    std::vector<Slot> slots_; // guarded by mutex_
    uint64_t use_counter_ = 0U; // guarded by mutex_
};
