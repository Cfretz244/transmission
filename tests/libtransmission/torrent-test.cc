// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef>
#include <ctime>
#include <ranges>
#include <vector>

#include <utime.h>

#include <libtransmission/file.h>
#include <libtransmission/torrent.h>
#include <libtransmission/torrent-ctor.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

using TorrentTest = tr::test::SessionTest;

namespace
{
auto constexpr TorFilenames = std::array{
    "Android-x86 8.1 r6 iso.torrent"sv,
    "debian-11.2.0-amd64-DVD-1.iso.torrent"sv,
    "ubuntu-18.04.6-desktop-amd64.iso.torrent"sv,
    "ubuntu-20.04.4-desktop-amd64.iso.torrent"sv,
};

// Creates every file in `metainfo` under `dir` at its full size, filled
// with `fill`, with an mtime old enough that the add-time seed check does
// not treat the files as modified after the torrent was added.
void writeFilesAs(tr_torrent_metainfo const& metainfo, std::string_view dir, char const fill)
{
    auto const mtime = time(nullptr) - 3600;
    for (tr_file_index_t i = 0, n = metainfo.file_count(); i < n; ++i)
    {
        auto const filename = tr_pathbuf{ dir, '/', metainfo.file_subpath(i) };
        tr_sys_dir_create(tr_sys_path_dirname(filename), TR_SYS_DIR_CREATE_PARENTS, 0700);
        auto const contents = std::vector<char>(metainfo.file_size(i), fill);
        auto fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0600);
        ASSERT_NE(TR_BAD_SYS_FILE, fd);
        ASSERT_TRUE(tr_sys_file_write(fd, std::data(contents), std::size(contents), nullptr));
        tr_sys_file_close(fd);
        auto const times = utimbuf{ mtime, mtime };
        ASSERT_EQ(0, utime(filename.c_str(), &times));
    }
}
}

// A torrent added over files that look complete (right names, sizes and
// mtimes) skips the full verify, but only after hashing its first piece.
// Without that hash, a cross-seeded release with different bytes announces
// itself to the tracker as a complete seed.
TEST_F(TorrentTest, addedTorrentWithMatchingFilesIsASeedWithoutVerify)
{
    auto* const ctor = zeroTorrentCtor();
    writeFilesAs(*tr_ctorGetMetainfo(ctor), tr_sessionGetDownloadDir(session_), '\0');

    auto* const tor = tr_torrentNew(ctor, nullptr);
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, tor);

    EXPECT_TRUE(tor->is_piece_checked(0U));
    EXPECT_TRUE(tor->has_all());
    EXPECT_TRUE(tor->is_done());
}

TEST_F(TorrentTest, addedTorrentWithWrongContentIsNotASeed)
{
    auto* const ctor = zeroTorrentCtor();
    writeFilesAs(*tr_ctorGetMetainfo(ctor), tr_sessionGetDownloadDir(session_), '\1');

    // the first-piece check fails, so the torrent falls back to a full verify
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, tor);

    EXPECT_FALSE(tor->has_all());
    EXPECT_FALSE(tor->is_done());
    EXPECT_EQ(0U, tor->has_total());
}

TEST_F(TorrentTest, queueMoveUp)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 1, 3, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_up(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveDown)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 0, 2, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_down(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveTop)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 3, 1, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_top(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveBottom)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 2, 0, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::ranges::transform(
        TorFilenames,
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrent::queue_move_bottom(move_torrents);

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}
