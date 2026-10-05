// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include "libtransmission/constants.h" // TrBlockSize
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h"
#include "libtransmission/open-files.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h" // tr_pathbuf
#include "libtransmission/types.h"
#include "libtransmission/utils.h"

using namespace std::literals;

namespace
{

bool read_entire_buf(tr_sys_file_t const fd, uint64_t file_offset, std::span<uint8_t> buf, tr_error& error)
{
    while (!std::empty(buf))
    {
        auto n_read = uint64_t{};

        if (!tr_sys_file_read_at(fd, std::data(buf), std::size(buf), file_offset, &n_read, &error))
        {
            return false;
        }

        buf = buf.subspan(n_read);
        file_offset += n_read;
    }

    return true;
}

bool write_entire_buf(tr_sys_file_t const fd, uint64_t file_offset, std::span<uint8_t const> buf, tr_error& error)
{
    while (!std::empty(buf))
    {
        auto n_written = uint64_t{};

        if (!tr_sys_file_write_at(fd, std::data(buf), std::size(buf), file_offset, &n_written, &error))
        {
            return false;
        }

        buf = buf.subspan(n_written);
        file_offset += n_written;
    }

    return true;
}

// The returned lease keeps the fd open; hold it until the I/O on that fd is done.
[[nodiscard]] tr_open_files::Lease get_fd(
    tr_open_files& open_files,
    tr_io_plan const& plan,
    tr_io_plan::File const& file,
    bool const writable,
    tr_io_result& result)
{
    // is the file already open in the fd pool?
    if (auto lease = open_files.get(plan.tor_id, file.index, writable); lease)
    {
        return lease;
    }

    // does the file exist?
    auto const prealloc = writable && file.wanted ? plan.prealloc : tr_file_preallocation::None;
    auto paths = std::array<std::string_view, 2>{};
    auto n_paths = size_t{};
    for (auto const& dir : { std::string_view{ plan.download_dir }, std::string_view{ plan.incomplete_dir } })
    {
        if (!std::empty(dir))
        {
            paths[n_paths++] = dir;
        }
    }
    if (auto const found = tr_torrent_files::find(file.subpath, std::data(paths), n_paths); found)
    {
        return open_files.get(plan.tor_id, file.index, writable, found->filename(), prealloc, file.size);
    }

    // do we want to create it?
    auto err = ENOENT;
    if (writable)
    {
        auto const suffix = plan.incomplete_file_naming ? tr_torrent_files::PartialFileSuffix : ""sv;
        auto const filename = tr_pathbuf{ plan.current_dir, '/', file.subpath, suffix };
        if (auto lease = open_files.get(plan.tor_id, file.index, writable, filename, prealloc, file.size); lease)
        {
            result.created_file = true;
            return lease;
        }

        err = errno;
    }

    result.error.set(
        err,
        fmt::format(
            fmt::runtime(_("Couldn't get '{path}': {error} ({error_code})")),
            fmt::arg("path", file.subpath),
            fmt::arg("error", tr_strerror(err)),
            fmt::arg("error_code", err)));
    return {};
}

void read_bytes(
    tr_open_files& open_files,
    tr_io_plan const& plan,
    tr_io_plan::File const& file,
    uint64_t const file_offset,
    std::span<uint8_t> buf,
    tr_io_result& result)
{
    TR_ASSERT(file_offset + std::size(buf) <= file.size);

    auto const lease = get_fd(open_files, plan, file, false, result);
    if (!lease || result.error)
    {
        return;
    }

    if (auto& error = result.error; !read_entire_buf(lease.fd(), file_offset, buf, error))
    {
        result.log_message = fmt::format(
            fmt::runtime(_("Couldn't read '{path}': {error} ({error_code})")),
            fmt::arg("path", file.subpath),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code()));
    }
}

void write_bytes(
    tr_open_files& open_files,
    tr_io_plan const& plan,
    tr_io_plan::File const& file,
    uint64_t const file_offset,
    std::span<uint8_t const> buf,
    tr_io_result& result)
{
    TR_ASSERT(file_offset + std::size(buf) <= file.size);

    auto const lease = get_fd(open_files, plan, file, true, result);
    if (!lease || result.error)
    {
        return;
    }

    if (auto& error = result.error; !write_entire_buf(lease.fd(), file_offset, buf, error))
    {
        result.log_message = fmt::format(
            fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
            fmt::arg("path", file.subpath),
            fmt::arg("error", error.message()),
            fmt::arg("error_code", error.code()));
    }
}

// Whether [offset, offset + n_bytes) is a non-empty part of a plan whose files cover its whole span.
[[nodiscard]] bool is_valid_request(tr_io_plan const& plan, uint64_t const offset, uint64_t const n_bytes)
{
    auto planned = uint64_t{};
    for (auto const& file : plan.files)
    {
        planned += file.length;
    }

    auto const span_size = plan.byte_span.is_valid() ? plan.byte_span.size() : 0U;
    return n_bytes != 0U && planned == span_size && offset <= span_size && n_bytes <= span_size - offset;
}

[[nodiscard]] tr_io_result make_einval()
{
    auto result = tr_io_result{};
    result.error.set_from_errno(EINVAL);
    return result;
}
} // namespace

tr_io_result tr_ioRead(tr_io_plan const& plan, tr_open_files& open_files, uint64_t offset, std::span<uint8_t> const setme)
{
    if (!is_valid_request(plan, offset, std::size(setme)))
    {
        return make_einval();
    }

    auto result = tr_io_result{};
    auto buf = setme;
    for (auto const& file : plan.files)
    {
        if (std::empty(buf) || result.error)
        {
            break;
        }

        if (offset >= file.length)
        {
            offset -= file.length;
            continue;
        }

        auto const bytes_this_pass = std::min<uint64_t>(std::size(buf), file.length - offset);
        read_bytes(open_files, plan, file, file.offset + offset, buf.first(bytes_this_pass), result);
        buf = buf.subspan(bytes_this_pass);
        offset = 0U;
    }

    return result;
}

tr_io_result tr_ioWrite(tr_io_plan const& plan, tr_open_files& open_files, std::span<uint8_t const> const writeme)
{
    if (!is_valid_request(plan, 0U, std::size(writeme)) || std::size(writeme) != plan.byte_span.size())
    {
        return make_einval();
    }

    auto result = tr_io_result{};
    auto buf = writeme;
    for (auto const& file : plan.files)
    {
        if (result.error)
        {
            break;
        }

        write_bytes(open_files, plan, file, file.offset, buf.first(file.length), result);
        buf = buf.subspan(file.length);
    }

    return result;
}

tr_io_result tr_ioHashSpan(tr_io_plan const& plan, tr_open_files& open_files, tr_sha1_digest_t& setme_hash)
{
    auto const n_bytes = plan.byte_span.is_valid() ? plan.byte_span.size() : 0U;
    if (n_bytes == 0U)
    {
        auto result = tr_io_result{};
        result.error.set_from_errno(EINVAL);
        return result;
    }

    auto sha = tr_sha1{};
    auto buffer = std::array<uint8_t, TrBlockSize>{};
    for (auto offset = uint64_t{}; offset < n_bytes;)
    {
        auto const len = static_cast<size_t>(std::min<uint64_t>(n_bytes - offset, std::size(buffer)));
        if (auto result = tr_ioRead(plan, open_files, offset, std::span{ std::data(buffer), len }); result.error)
        {
            return result;
        }

        sha.add(std::data(buffer), len);
        offset += len;
    }

    setme_hash = sha.finish();
    return {};
}
