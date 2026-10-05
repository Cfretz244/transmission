// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::min
#include <array>
#include <cstdint> // uint8_t, uint64_t
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/open-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/types.h"
#include "libtransmission/utils.h" // _()

namespace
{

[[nodiscard]] auto is_open(tr_sys_file_t fd) noexcept
{
    return fd != TR_BAD_SYS_FILE;
}

bool preallocate_file_sparse(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, TR_SYS_FILE_PREALLOC_SPARSE, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Fast preallocation failed: {} ({})", local_error.message(), local_error.code()));

    if (!tr_error_is_enospc(local_error.code()))
    {
        static char constexpr Zero = '\0';

        local_error = {};

        /* fallback: the old-style seek-and-write */
        if (tr_sys_file_write_at(fd, &Zero, 1, length - 1, nullptr, &local_error) &&
            tr_sys_file_truncate(fd, length, &local_error))
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Fast prellocation fallback failed: {} ({})", local_error.message(), local_error.code()));
    }

    if (error != nullptr)
    {
        *error = std::move(local_error);
    }

    return false;
}

bool preallocate_file_full(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, 0, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Full preallocation failed: {} ({})", local_error.message(), local_error.code()));

    if (!tr_error_is_enospc(local_error.code()))
    {
        auto buf = std::array<uint8_t, 4096>{};
        bool success = true;

        local_error = {};

        /* fallback: the old-fashioned way */
        while (success && length > 0)
        {
            uint64_t const this_pass = std::min(length, uint64_t{ std::size(buf) });
            uint64_t bytes_written = 0;
            auto const bytes = std::as_bytes(std::span{ buf }.first(static_cast<size_t>(this_pass)));
            success = tr_sys_file_write(fd, bytes.data(), bytes.size_bytes(), &bytes_written, &local_error);
            length -= bytes_written;
        }

        if (success)
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Full preallocation fallback failed: {} ({})", local_error.message(), local_error.code()));
    }

    if (error != nullptr)
    {
        *error = std::move(local_error);
    }

    return false;
}

} // unnamed namespace

// ---

// Shared by the pool's slot and any leases; the last owner closes the fd.
struct tr_open_files::File
{
    File(tr_sys_file_t fd_in, bool writable_in) noexcept
        : fd{ fd_in }
        , writable{ writable_in }
    {
    }

    File(File const&) = delete;
    File& operator=(File const&) = delete;
    File(File&&) = delete;
    File& operator=(File&&) = delete;

    ~File()
    {
        tr_sys_file_close(fd);
    }

    tr_sys_file_t const fd;
    bool const writable;
};

tr_sys_file_t tr_open_files::Lease::fd() const noexcept
{
    return file_ ? file_->fd : TR_BAD_SYS_FILE;
}

tr_open_files::tr_open_files(size_t max_open_files)
    : slots_(max_open_files)
{
}

tr_open_files::Slot* tr_open_files::find(Key const& key) noexcept
{
    auto const iter = std::ranges::find_if(slots_, [&key](Slot const& slot) { return slot.file && slot.key == key; });
    return iter != std::end(slots_) ? &*iter : nullptr;
}

// Caches `file` under `key` and returns the file it displaced, if any,
// so the caller can close that file after releasing mutex_.
// A file is leased iff use_count() > 1. Leases are only created under
// mutex_, so the count seen here can be stale-high but never stale-low:
// a leased file is never evicted. If every slot is leased, `file` is
// left uncached and closes when its lease is released.
std::shared_ptr<tr_open_files::File> tr_open_files::add(Key const& key, std::shared_ptr<File> file)
{
    auto* slot = find(key);

    if (slot == nullptr)
    {
        for (auto& candidate : slots_)
        {
            if (!candidate.file)
            {
                slot = &candidate;
                break;
            }

            if (candidate.file.use_count() == 1 && (slot == nullptr || candidate.last_used < slot->last_used))
            {
                slot = &candidate;
            }
        }
    }

    if (slot == nullptr)
    {
        return {};
    }

    slot->key = key;
    slot->last_used = ++use_counter_;
    std::swap(slot->file, file);
    return file;
}

tr_open_files::Lease tr_open_files::get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable)
{
    auto const lock = std::lock_guard{ mutex_ };

    if (auto* const slot = find(make_key(tor_id, file_num)); slot != nullptr && (!writable || slot->file->writable))
    {
        slot->last_used = ++use_counter_;
        return Lease{ slot->file };
    }

    return {};
}

// Holds mutex_ only for cache lookups and updates.
// Opening, preallocating, and closing files happen outside it.
tr_open_files::Lease tr_open_files::get(
    tr_torrent_id_t tor_id,
    tr_file_index_t file_num,
    bool writable,
    std::string_view const filename,
    tr_file_preallocation allocation,
    uint64_t file_size)
{
    auto const key = make_key(tor_id, file_num);

    // is there already an entry
    auto read_only = std::shared_ptr<File>{};
    {
        auto const lock = std::lock_guard{ mutex_ };

        if (auto* const slot = find(key); slot != nullptr)
        {
            if (!writable || slot->file->writable)
            {
                slot->last_used = ++use_counter_;
                return Lease{ slot->file };
            }

            // uncache so we can re-open as writable
            read_only = std::move(slot->file);
        }
    }
    read_only.reset(); // closes it outside mutex_, unless it is leased

    // create subfolders, if any
    auto error = tr_error{};
    if (writable)
    {
        if (auto const dir = tr_sys_path_dirname(filename); !tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, &error))
        {
            tr_logAddError(
                fmt::format(
                    fmt::runtime(_("Couldn't create '{path}': {error} ({error_code})")),
                    fmt::arg("path", dir),
                    fmt::arg("error", error.message()),
                    fmt::arg("error_code", error.code())));
            return Lease{};
        }
    }

    auto const info = tr_sys_path_get_info(filename);
    bool const already_existed = info && info->isFile();

    // we need write permissions to resize the file
    bool const resize_needed = already_existed && (file_size < info->size);
    writable |= resize_needed;

    // open the file
    int flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
    flags |= TR_SYS_FILE_READ;
    auto const fd = tr_sys_file_open(filename, flags, 0666, &error);
    if (!is_open(fd))
    {
        tr_logAddError(
            fmt::format(
                fmt::runtime(_("Couldn't open '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        return Lease{};
    }

    if (writable && !already_existed && allocation != tr_file_preallocation::None)
    {
        bool success = false;
        char const* type = nullptr;

        if (allocation == tr_file_preallocation::Full)
        {
            success = preallocate_file_full(fd, file_size, &error);
            type = "full";
        }
        else if (allocation == tr_file_preallocation::Sparse)
        {
            success = preallocate_file_sparse(fd, file_size, &error);
            type = "sparse";
        }

        TR_ASSERT(type != nullptr);

        if (!success)
        {
            tr_logAddError(
                fmt::format(
                    fmt::runtime(_("Couldn't preallocate '{path}': {error} ({error_code})")),
                    fmt::arg("path", filename),
                    fmt::arg("error", error.message()),
                    fmt::arg("error_code", error.code())));
            tr_sys_file_close(fd);
            return Lease{};
        }

        tr_logAddDebug(fmt::format("Preallocated file '{}' ({}, size: {})", filename, type, file_size));
    }

    // If the file already exists and it's too large, truncate it.
    // This is a fringe case that happens if a torrent's been updated
    // and one of the updated torrent's files is smaller.
    // https://trac.transmissionbt.com/ticket/2228
    // https://bugs.launchpad.net/ubuntu/+source/transmission/+bug/318249
    if (resize_needed && !tr_sys_file_truncate(fd, file_size, &error))
    {
        tr_logAddWarn(
            fmt::format(
                fmt::runtime(_("Couldn't truncate '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        tr_sys_file_close(fd);
        return Lease{};
    }

    // cache it
    auto file = std::make_shared<File>(fd, writable);
    auto displaced = std::shared_ptr<File>{}; // destroyed after `lock`, so closed outside mutex_
    auto const lock = std::lock_guard{ mutex_ };
    displaced = add(key, file);
    return Lease{ std::move(file) };
}

// The close_*() functions move files out of slots_ under mutex_; the files
// close when `closing` is destroyed after `lock`, or, if leased, when their
// last lease is released.

void tr_open_files::close_all()
{
    auto closing = std::vector<std::shared_ptr<File>>{};
    auto const lock = std::lock_guard{ mutex_ };

    for (auto& slot : slots_)
    {
        if (slot.file)
        {
            closing.emplace_back(std::move(slot.file));
        }
    }
}

void tr_open_files::close_torrent(tr_torrent_id_t tor_id)
{
    auto closing = std::vector<std::shared_ptr<File>>{};
    auto const lock = std::lock_guard{ mutex_ };

    for (auto& slot : slots_)
    {
        if (slot.file && slot.key.first == tor_id)
        {
            closing.emplace_back(std::move(slot.file));
        }
    }
}

void tr_open_files::close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num)
{
    auto closing = std::shared_ptr<File>{};
    auto const lock = std::lock_guard{ mutex_ };

    if (auto* const slot = find(make_key(tor_id, file_num)); slot != nullptr)
    {
        closing = std::move(slot->file);
    }
}
