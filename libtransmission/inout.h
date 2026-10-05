// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstdint> // uint8_t, uint64_t
#include <span>
#include <string>
#include <vector>

#include "libtransmission/crypto-utils.h" // tr_sha1_digest_t
#include "libtransmission/error.h"
#include "libtransmission/types.h"

class tr_open_files;

/**
 * Everything needed to read or write one span of a torrent's data,
 * copied from the torrent on the session thread.
 *
 * LocalData's worker threads use a plan instead of the torrent itself
 * because the session thread may rename, move, or free the torrent while
 * the I/O runs. See tr_torrent::make_io_plan().
 */
struct tr_io_plan
{
    // The part of the span that lies in one file.
    struct File
    {
        tr_file_index_t index = {};
        uint64_t offset = {}; // where the span's part starts in this file
        uint64_t length = {}; // how many of the span's bytes are in this file
        uint64_t size = {}; // the whole file's size
        std::string subpath;
        bool wanted = false;
    };

    tr_torrent_id_t tor_id = {};
    tr_byte_span_t byte_span = {};
    std::string name; // the torrent's name, for log messages
    std::string download_dir;
    std::string incomplete_dir;
    std::string current_dir; // where a missing file gets created
    tr_file_preallocation prealloc = tr_file_preallocation::None;
    bool incomplete_file_naming = false;

    // In byte order. Their lengths add up to byte_span.size().
    // Zero-length files are left out: there is nothing to read or write.
    std::vector<File> files;
};

struct tr_io_result
{
    tr_error error;

    // Set when the error should be logged against the torrent.
    std::string log_message;

    // True if a write created a file that did not exist.
    bool created_file = false;
};

/**
 * Reads `setme.size()` bytes, starting `offset` bytes into the plan's span.
 * Touches only the filesystem and `open_files`, so it is safe on any thread.
 */
[[nodiscard]] tr_io_result tr_ioRead(
    tr_io_plan const& plan,
    tr_open_files& open_files,
    uint64_t offset,
    std::span<uint8_t> setme);

/**
 * Writes the plan's whole span. Creates missing files in the plan's current_dir.
 * Touches only the filesystem and `open_files`, so it is safe on any thread.
 */
[[nodiscard]] tr_io_result tr_ioWrite(tr_io_plan const& plan, tr_open_files& open_files, std::span<uint8_t const> writeme);

/**
 * Reads the plan's whole span and stores its SHA1 digest in `setme_hash`.
 * Touches only the filesystem and `open_files`, so it is safe on any thread.
 */
[[nodiscard]] tr_io_result tr_ioHashSpan(tr_io_plan const& plan, tr_open_files& open_files, tr_sha1_digest_t& setme_hash);
