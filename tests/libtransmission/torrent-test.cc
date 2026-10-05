// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstddef>
#include <ctime>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <utime.h>

#include <libtransmission/file-utils.h>
#include <libtransmission/file.h>
#include <libtransmission/quark.h>
#include <libtransmission/session.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/torrent.h>
#include <libtransmission/torrent-ctor.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/variant.h>

#include "test-fixtures.h"

using TorrentTest = tr::test::SessionTest;
using tr::test::waitFor;

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
// itself to the tracker as a complete seed. The stats and the hash run on
// the verify thread, so the result arrives after tr_torrentNew() returns.
TEST_F(TorrentTest, addedTorrentWithMatchingFilesIsASeedWithoutVerify)
{
    auto* const ctor = zeroTorrentCtor();
    writeFilesAs(*tr_ctorGetMetainfo(ctor), tr_sessionGetDownloadDir(session_), '\0');

    auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, tor);

    EXPECT_TRUE(tor->is_piece_checked(0U));
    EXPECT_FALSE(tor->is_piece_checked(1U)); // held, but only the first piece was hashed
    EXPECT_TRUE(tor->has_all());
    EXPECT_TRUE(tor->is_done());
    EXPECT_NE(TR_STATUS_CHECK, tor->activity());
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

// Resume files are written on the session's state-writer thread, not the
// session thread; flush() is how a reader waits for one.
TEST_F(TorrentTest, resumeFileIsSavedOffTheSessionThread)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    auto const resume_file = std::string{ tor->resume_file() };
    EXPECT_FALSE(tr_sys_path_exists(resume_file));

    // pausing marks the torrent dirty and saves it, as the save timer would
    tr_torrentStop(tor);
    EXPECT_TRUE(waitFor([&resume_file]() { return tr_sys_path_exists(resume_file); }, 3000));
    session_->state_writer.flush(resume_file);
    EXPECT_TRUE(tr_sys_path_exists(resume_file));
}

// Removing a torrent deletes its resume file behind any save still queued
// for it, so the delete wins and a later re-add starts from nothing.
TEST_F(TorrentTest, removedTorrentsResumeFileIsDeletedBehindQueuedSaves)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    auto const resume_file = std::string{ tor->resume_file() };
    auto const info_hash = tor->info_hash();

    for (int i = 0; i < 20; ++i)
    {
        tr_torrentStop(tor); // each one queues a save
    }
    tr_torrentRemove(tor, false);
    EXPECT_TRUE(waitFor([this, &info_hash]() { return session_->torrents().get(info_hash) == nullptr; }, 3000));

    session_->state_writer.flush(resume_file);
    EXPECT_FALSE(tr_sys_path_exists(resume_file));
}

// State files written by old versions as `name.hash16.suffix` are renamed
// to `hash.suffix` at add time and their contents used. The legacy names
// are found by listing each state dir once per session instead of a stat()
// per add, so a session whose dirs hold no legacy files never looks for them.
TEST_F(TorrentTest, legacyStateFilesAreMigratedAtAdd)
{
    auto* const ctor = zeroTorrentCtor();
    auto const& metainfo = *tr_ctorGetMetainfo(ctor);
    auto const hash16 = metainfo.info_hash_string().sv().substr(0, 16);
    auto const legacy_resume = tr_pathbuf{ session_->resumeDir(), '/', metainfo.name(), '.', hash16, ".resume"sv };
    auto const legacy_store = tr_pathbuf{ session_->torrentDir(), '/', metainfo.name(), '.', hash16, ".torrent"sv };

    // a legacy resume file carrying a download dir this test can recognise
    auto const download_dir = tr_pathbuf{ sandboxDir(), "/legacy-destination"sv };
    auto resume = tr_variant::Map{};
    resume.try_emplace(TR_KEY_destination, download_dir.sv());
    ASSERT_TRUE(tr_file_save(legacy_resume, tr_variant_serde::benc().to_string(tr_variant{ std::move(resume) })));
    ASSERT_TRUE(tr_file_save(legacy_store, ctor->contents()));

    auto* const tor = tr_torrentNew(ctor, nullptr);
    ASSERT_NE(nullptr, tor);
    tr_ctorFree(ctor);

    EXPECT_EQ(download_dir.sv(), tor->download_dir().sv());
    EXPECT_FALSE(tr_sys_path_exists(legacy_resume));
    EXPECT_TRUE(tr_sys_path_exists(tor->resume_file()));
    session_->state_writer.flush(tor->store_file());
    EXPECT_FALSE(tr_sys_path_exists(legacy_store));
    EXPECT_TRUE(tr_sys_path_exists(tor->store_file()));
}

// The .torrent file saved at add time is written on the state-writer
// thread too, and a tracker edit rewrites it there.
TEST_F(TorrentTest, storeFileIsWrittenAndRewrittenOffTheSessionThread)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    auto const store_file = std::string{ tor->store_file() };

    session_->state_writer.flush(store_file);
    ASSERT_TRUE(tr_sys_path_exists(store_file));
    auto on_disk = tr_torrent_metainfo{};
    ASSERT_TRUE(on_disk.parse_torrent_file(store_file));
    static auto constexpr Tracker = "http://127.0.0.1:1/announce"sv;
    EXPECT_NE(Tracker, on_disk.announce_list().at(0).announce.sv());

    // a tracker edit on a paused torrent: nothing is announced, but the
    // .torrent on disk must carry the new list
    EXPECT_TRUE(tr_torrentSetTrackerList(tor, Tracker));
    EXPECT_FALSE(tor->is_running());
    session_->state_writer.flush(store_file);
    ASSERT_TRUE(on_disk.parse_torrent_file(store_file));
    ASSERT_EQ(1U, std::size(on_disk.announce_list()));
    EXPECT_EQ(Tracker, on_disk.announce_list().at(0).announce.sv());
}

TEST_F(TorrentTest, removedTorrentsStoreFileIsDeletedBehindQueuedSaves)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    auto const store_file = std::string{ tor->store_file() };
    auto const info_hash = tor->info_hash();
    session_->state_writer.flush(store_file);
    ASSERT_TRUE(tr_sys_path_exists(store_file));

    EXPECT_TRUE(tr_torrentSetTrackerList(tor, "http://127.0.0.1:1/announce"sv)); // queues a rewrite
    tr_torrentRemove(tor, false);
    EXPECT_TRUE(waitFor([this, &info_hash]() { return session_->torrents().get(info_hash) == nullptr; }, 3000));

    session_->state_writer.flush(store_file);
    EXPECT_FALSE(tr_sys_path_exists(store_file));
}
