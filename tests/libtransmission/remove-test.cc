// This file Copyright (C) 2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint> // uint64_t
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <fcntl.h> // open()
#include <sys/stat.h> // mkfifo()
#include <unistd.h> // close()
#include <utime.h>
#endif

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/file.h>
#include <libtransmission/local-data.h>
#include <libtransmission/session.h>
#include <libtransmission/string-utils.h> // tr_strerror()
#include <libtransmission/torrent-files.h>
#include <libtransmission/torrent.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

using namespace std::literals;
using SubpathAndSize = std::pair<std::string_view, uint64_t>;
using tr::test::waitFor;

class RemoveTest : public tr::test::SandboxedTest
{
protected:
    static constexpr std::string_view Content = "Hello, World!"sv;
    static constexpr std::string_view JunkBasename = ".DS_Store"sv;
    static constexpr std::string_view NonJunkBasename = "passwords.txt"sv;

    static auto aliceFiles()
    {
        static constexpr std::array<SubpathAndSize, 106> AliceFiles = { {
            { "alice_in_wonderland_librivox/AliceInWonderland_librivox.m4b", 87525736ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland.jpg", 81464ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland.pdf", 185367ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_abbyy.gz", 24582ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_chocr.html.gz", 22527ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_djvu.txt", 2039ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_djvu.xml", 28144ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_hocr.html", 56942ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_hocr_pageindex.json.gz", 40ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_hocr_searchtext.txt.gz", 943ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_jp2.zip", 1499986ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_page_numbers.json", 136ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_scandata.xml", 538ULL },
            { "alice_in_wonderland_librivox/Alice_in_Wonderland_thumb.jpg", 26987ULL },
            { "alice_in_wonderland_librivox/__ia_thumb.jpg", 16557ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox.json", 13740ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox.storj-store.trigger", 0ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox_128kb.m3u", 984ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox_64kb.m3u", 1044ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox_meta.sqlite", 20480ULL },
            { "alice_in_wonderland_librivox/alice_in_wonderland_librivox_meta.xml", 2805ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01.mp3", 10249859ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01.ogg", 7509828ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01.png", 10779ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01_64kb.mp3", 5124992ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01_esshigh.json.gz", 1977ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01_esslow.json.gz", 29258ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_01_spectrogram.png", 234022ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02.mp3", 11772312ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02.ogg", 5148365ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02.png", 10962ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02_64kb.mp3", 5886455ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02_esshigh.json.gz", 1980ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02_esslow.json.gz", 30287ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_02_spectrogram.png", 326161ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03.mp3", 17024560ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03.ogg", 12046177ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03.png", 8725ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03_64kb.mp3", 8512448ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03_esshigh.json.gz", 1966ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03_esslow.json.gz", 34110ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_03_spectrogram.png", 218264ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04.mp3", 19087768ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04.ogg", 9880920ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04.png", 6055ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04_64kb.mp3", 9544016ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04_esshigh.json.gz", 1967ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04_esslow.json.gz", 36154ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_04_spectrogram.png", 282145ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05.mp3", 12946949ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05.ogg", 7470734ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05.png", 12061ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05_64kb.mp3", 6473687ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05_esshigh.json.gz", 1963ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05_esslow.json.gz", 31048ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_05_spectrogram.png", 304150ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06.mp3", 12413304ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06.ogg", 7154040ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06.png", 11383ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06_64kb.mp3", 6206820ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06_esshigh.json.gz", 1942ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06_esslow.json.gz", 30295ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_06_spectrogram.png", 288601ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07.mp3", 16742808ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07.ogg", 10513847ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07.png", 8180ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07_64kb.mp3", 8371136ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07_esshigh.json.gz", 1963ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07_esslow.json.gz", 33992ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_07_spectrogram.png", 233725ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08.mp3", 12784781ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08.ogg", 9306961ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08.png", 11470ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08_64kb.mp3", 6392576ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08_esshigh.json.gz", 1973ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08_esslow.json.gz", 31964ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_08_spectrogram.png", 233626ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09.mp3", 14528920ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09.ogg", 8062952ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09.png", 9439ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09_64kb.mp3", 7264466ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09_esshigh.json.gz", 1946ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09_esslow.json.gz", 32295ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_09_spectrogram.png", 282898ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10.mp3", 21894203ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10.ogg", 15226220ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10.png", 8796ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10_64kb.mp3", 10947200ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10_esshigh.json.gz", 1970ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10_esslow.json.gz", 37062ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_10_spectrogram.png", 221277ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11.mp3", 9919894ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11.ogg", 7676067ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11.png", 7140ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11_64kb.mp3", 4959296ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11_esshigh.json.gz", 1959ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11_esslow.json.gz", 28694ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_11_spectrogram.png", 229471ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12.mp3", 12359368ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12.ogg", 8065179ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12.png", 11074ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12_64kb.mp3", 6179840ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12_esshigh.json.gz", 1981ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12_esslow.json.gz", 30975ULL },
            { "alice_in_wonderland_librivox/wonderland_ch_12_spectrogram.png", 235568ULL },
            { "alice_in_wonderland_librivox/history/files/alice_in_wonderland_librivox.storj-store.trigger.~1~", 0ULL },
        } };

        auto files = tr_torrent_files{};

        for (auto const& file : AliceFiles)
        {
            auto const& [filename, size] = file;
            files.add(filename, size);
        }

        return files;
    }

    static auto ubuntuFiles()
    {
        static auto constexpr Files = std::array<SubpathAndSize, 1>{ { { "ubuntu-20.04.4-desktop-amd64.iso"sv,
                                                                         3379068928ULL } } };

        auto files = tr_torrent_files{};

        for (auto const& file : Files)
        {
            auto const& [filename, size] = file;
            files.add(filename, size);
        }

        return files;
    }

    static auto createFiles(tr_torrent_files const& files, char const* parent)
    {
        auto paths = std::set<std::string>{};

        for (tr_file_index_t i = 0, n = files.file_count(); i < n; ++i)
        {
            auto const filename = tr_pathbuf{ parent, '/', files.path(i) };
            createFileWithContents(filename, std::data(Content), std::size(Content));
            paths.emplace(filename);

            auto walk = filename.sv();
            while (!tr_sys_path_is_same(parent, walk))
            {
                walk = tr_sys_path_dirname(walk);
                paths.emplace(walk);
            }
        }

        return paths;
    }

    static auto getSubtreeContents(std::string_view parent_dir)
    {
        auto filenames = std::set<std::string>{};

        auto file_func = [&filenames](char const* filename)
        {
            filenames.emplace(filename);
        };

        tr::test::depthFirstWalk(tr_pathbuf{ parent_dir }, file_func);

        return filenames;
    }
};

TEST_F(RemoveTest, RemovesSingleFile)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = ubuntuFiles();
    expected_tree = createFiles(files, parent.c_str());
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    files.remove(parent, "tmpdir_prefix"sv, tr_sys_path_remove);
    expected_tree = { parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

TEST_F(RemoveTest, RemovesSubtree)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = aliceFiles();
    expected_tree = createFiles(files, parent.c_str());
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    files.remove(parent, "tmpdir_prefix"sv, tr_sys_path_remove);
    expected_tree = { parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

TEST_F(RemoveTest, RemovesLeftoverJunk)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = aliceFiles();
    expected_tree = createFiles(files, parent.c_str());
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    // add a junk file *inside of* the torrent's top directory.
    auto const junk_file = tr_pathbuf{ parent, "/alice_in_wonderland_librivox/"sv, JunkBasename };
    createFileWithContents(junk_file, std::data(Content), std::size(Content));
    expected_tree.emplace(junk_file);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    files.remove(parent, "tmpdir_prefix"sv, tr_sys_path_remove);
    expected_tree = { parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

TEST_F(RemoveTest, LeavesSiblingsAlone)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = aliceFiles();
    expected_tree = createFiles(files, parent.c_str());
    EXPECT_GT(std::size(expected_tree), 100U);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    // add a junk file *as a sibling of* the torrent's top directory.
    auto const junk_file = tr_pathbuf{ parent, '/', JunkBasename };
    createFileWithContents(junk_file, std::data(Content), std::size(Content));
    expected_tree.emplace(junk_file);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    // add a non-junk file *as a sibling of* the torrent's top directory.
    auto const non_junk_file = tr_pathbuf{ parent, '/', NonJunkBasename };
    createFileWithContents(non_junk_file, std::data(Content), std::size(Content));
    expected_tree.emplace(non_junk_file);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    files.remove(parent, "tmpdir_prefix"sv, tr_sys_path_remove);
    expected_tree = { parent, junk_file.c_str(), non_junk_file.c_str() };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

TEST_F(RemoveTest, LeavesNonJunkAlone)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = aliceFiles();
    expected_tree = createFiles(files, parent.c_str());
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    // add a non-junk file.
    auto const nonjunk_file = tr_pathbuf{ parent, "/alice_in_wonderland_librivox/"sv, NonJunkBasename };
    createFileWithContents(nonjunk_file, std::data(Content), std::size(Content));
    expected_tree.emplace(nonjunk_file);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    files.remove(parent, "tmpdir_prefix"sv, tr_sys_path_remove);
    expected_tree = { parent, std::string{ tr_sys_path_dirname(nonjunk_file) }, nonjunk_file.c_str() };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

TEST_F(RemoveTest, PreservesDirectoryHierarchyIfPossible)
{
    auto const parent = sandboxDir();
    auto expected_tree = std::set<std::string>{ parent };
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    // add a recycle bin
    auto const recycle_bin = tr_pathbuf{ parent, "/Trash"sv };
    tr_sys_dir_create(recycle_bin, TR_SYS_DIR_CREATE_PARENTS, 0777);
    expected_tree.emplace(recycle_bin);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const files = aliceFiles();
    expected_tree = createFiles(files, parent.c_str());
    expected_tree.emplace(recycle_bin);
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));

    auto const recycle_func = [&recycle_bin](std::string_view const old_name, tr_error* error) -> bool
    {
        auto const new_name = tr_pathbuf{ recycle_bin, '/', tr_sys_path_basename(old_name) };
        return tr_sys_path_rename(old_name, new_name, error);
    };
    files.remove(parent, "tmpdir_prefix"sv, recycle_func);

    // after remove, the subtree should be:
    expected_tree = { parent, recycle_bin.c_str() };
    for (tr_file_index_t i = 0, n = files.file_count(); i < n; ++i)
    {
        expected_tree.emplace(tr_pathbuf{ recycle_bin, '/', files.path(i) });
    }
    expected_tree.emplace(tr_pathbuf{ recycle_bin, "/alice_in_wonderland_librivox"sv });
    expected_tree.emplace(tr_pathbuf{ recycle_bin, "/alice_in_wonderland_librivox/history"sv });
    expected_tree.emplace(tr_pathbuf{ recycle_bin, "/alice_in_wonderland_librivox/history/files"sv });
    EXPECT_EQ(expected_tree, getSubtreeContents(parent));
}

// ---

class RemoveTorrentTest : public tr::test::SessionTest
{
protected:
    static auto constexpr MaxWaitMsec = 3000;

#ifndef _WIN32
    // A complete torrent removed with its data while its disk queue is
    // stalled, then added again before the delete has run. The re-added
    // torrent is in deferred init: it must not look at, move, rename or
    // delete the files under its path until the stall is released and the
    // delete finishes. See tr_torrent::DeferredInit.
    struct DeferredReadd
    {
        tr_torrent* readded = nullptr;
        std::string download_dir;
        std::string first_file; // a file of the removed torrent, still on disk
        std::string fifo_path;
        bool verified = false; // the re-added torrent has taken its first look at its files
        bool stall_answered = false;
    };

    void makeDeferredReadd(std::shared_ptr<DeferredReadd>& setme)
    {
        auto state = std::make_shared<DeferredReadd>();

        auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
        blockingTorrentVerify(tor);
        ASSERT_TRUE(tor->has_all());
        auto const info_hash = tor->info_hash();

        // Verification queued a path check per file on the torrent's disk
        // queue. Let those drain behind a sentinel read, so that the
        // is_active() wait below can only be satisfied by the stall.
        auto const drained = std::make_shared<std::atomic<bool>>(false);
        session_->local_data.read(
            makeIoPlan(tor),
            [drained](tr_torrent_id_t, tr_byte_span_t, tr_error const&, std::unique_ptr<tr::LocalData::BlockData>)
            { *drained = true; });
        ASSERT_TRUE(waitFor([drained]() { return drained->load(); }, MaxWaitMsec));
        state->download_dir = tr_sessionGetDownloadDir(session_);
        state->first_file = tr_pathbuf{ state->download_dir, '/', tr_torrentFile(tor, 0).name };
        ASSERT_TRUE(tr_sys_path_exists(state->first_file));

        // Stall the torrent's disk queue, which runs one op at a time: a read
        // whose "file" is a FIFO with no writer blocks in open() until the test
        // opens the other end. Its file index is one the torrent doesn't have,
        // so the fd pool never hands the FIFO out for a real block.
        auto const fifo_name = "stall.fifo"sv;
        state->fifo_path = tr_pathbuf{ state->download_dir, '/', fifo_name };
        ASSERT_EQ(0, mkfifo(state->fifo_path.c_str(), 0600)) << tr_strerror(errno);
        auto stall_plan = makeIoPlan(tor);
        ASSERT_EQ(1U, std::size(stall_plan.files));
        stall_plan.files[0].index = tor->file_count() + 1U;
        stall_plan.files[0].subpath = fifo_name;
        session_->local_data.read(
            std::move(stall_plan),
            [state](tr_torrent_id_t, tr_byte_span_t, tr_error const&, std::unique_ptr<tr::LocalData::BlockData>)
            { state->stall_answered = true; });

        // Wait for a worker to pick the read up: tr_torrentRemove() forgets
        // the torrent's still-queued reads, and a cancelled stall is no stall.
        ASSERT_TRUE(waitFor([this, id = tor->id()]() { return session_->local_data.is_active(id); }, MaxWaitMsec));

        // remove the torrent and its files: the delete queues behind the stall
        tr_torrentRemove(tor, true);
        ASSERT_TRUE(waitFor([this, &info_hash]() { return session_->torrents().get(info_hash) == nullptr; }, MaxWaitMsec));
        ASSERT_FALSE(state->stall_answered);
        ASSERT_TRUE(tr_sys_path_exists(state->first_file));

        // Backdate the files so the re-added torrent takes the "this new torrent
        // is already a seed" shortcut if it looks at them at all.
        auto* const ctor = zeroTorrentCtor();
        auto const* const metainfo = tr_ctorGetMetainfo(ctor);
        auto const old_times = utimbuf{ 1000000000, 1000000000 };
        for (tr_file_index_t i = 0, n = metainfo->file_count(); i < n; ++i)
        {
            ASSERT_EQ(0, utime(tr_pathbuf{ state->download_dir, '/', metainfo->file_subpath(i) }, &old_times))
                << tr_strerror(errno);
        }

        // add it again while the delete is still pending
        ctor->set_verify_done_callback([state](tr_torrent*) { state->verified = true; });
        state->readded = tr_torrentNew(ctor, nullptr);
        tr_ctorFree(ctor);
        ASSERT_NE(nullptr, state->readded);

        // it must not claim the old files
        ASSERT_TRUE(state->readded->deferred_init_.has_value());
        ASSERT_FALSE(state->readded->has_all());
        ASSERT_TRUE(state->readded->has_none());
        ASSERT_EQ(TR_STATUS_STOPPED, state->readded->activity());
        ASSERT_FALSE(state->stall_answered);
        ASSERT_TRUE(tr_sys_path_exists(state->first_file));

        setme = std::move(state);
    }

    // Let the stalled read return, so the queued delete runs.
    static void releaseStall(DeferredReadd const& state)
    {
        auto const writer = open(state.fifo_path.c_str(), O_WRONLY);
        ASSERT_NE(-1, writer) << tr_strerror(errno);
        EXPECT_EQ(0, close(writer));
    }
#endif
};

// "Remove with data, then add it again" is how users force a fresh
// download. The delete runs later on a LocalData worker, so the re-added
// torrent must not look at its files until the delete has finished: it
// would find the old files, call itself complete and announce `left=0`
// for data that is about to vanish.
TEST_F(RemoveTorrentTest, readdedTorrentWaitsForPendingDelete)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    auto state = std::shared_ptr<DeferredReadd>{};
    ASSERT_NO_FATAL_FAILURE(makeDeferredReadd(state));
    auto* const readded = state->readded;

    // release the stall: the delete runs, then the re-added torrent checks its files
    ASSERT_NO_FATAL_FAILURE(releaseStall(*state));
    EXPECT_TRUE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([&state]() { return state->verified; }, MaxWaitMsec));
    EXPECT_TRUE(state->stall_answered);

    EXPECT_FALSE(readded->deferred_init_.has_value());
    EXPECT_FALSE(readded->has_all());
    EXPECT_TRUE(readded->has_none());
    EXPECT_FALSE(readded->is_running());
    EXPECT_FALSE(tr_sys_path_exists(state->first_file));
#endif
}

// A deferred torrent has nothing of its own on disk, so "move" has nothing
// to move. Moving the files under its path would carry the removed
// torrent's data out from under the delete and make this torrent complete.
TEST_F(RemoveTorrentTest, movingADeferredTorrentDoesNotMoveTheFilesBeingDeleted)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    auto state = std::shared_ptr<DeferredReadd>{};
    ASSERT_NO_FATAL_FAILURE(makeDeferredReadd(state));
    auto* const readded = state->readded;

    auto const new_dir = tr_pathbuf{ sandboxDir(), "/moved"sv };
    ASSERT_TRUE(tr_sys_dir_create(new_dir, 0, 0700));
    auto const moved_file = tr_pathbuf{ new_dir, '/', tr_torrentFile(readded, 0).name };

    int volatile move_state = -1;
    tr_torrentSetLocation(readded, new_dir.c_str(), true, &move_state);
    EXPECT_TRUE(waitFor([&move_state]() { return move_state != TR_LOC_MOVING; }, MaxWaitMsec));
    EXPECT_EQ(TR_LOC_DONE, move_state);

    // the files stayed where the delete will find them...
    EXPECT_TRUE(tr_sys_path_exists(state->first_file));
    EXPECT_FALSE(tr_sys_path_exists(moved_file));
    EXPECT_EQ(new_dir.sv(), readded->download_dir().sv());
    EXPECT_EQ(new_dir.sv(), readded->current_dir().sv());

    // ...and the torrent, pointing away from the delete now, took its first
    // look at its (absent) files without waiting for it
    EXPECT_TRUE(waitFor([&state]() { return state->verified; }, MaxWaitMsec));
    EXPECT_FALSE(readded->deferred_init_.has_value());
    EXPECT_TRUE(readded->has_none());
    EXPECT_FALSE(state->stall_answered);

    ASSERT_NO_FATAL_FAILURE(releaseStall(*state));
    EXPECT_TRUE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([&state]() { return state->stall_answered; }, MaxWaitMsec));
    EXPECT_TRUE(readded->has_none());
    EXPECT_FALSE(tr_sys_path_exists(moved_file));
#endif
}

// Likewise rename: renaming the top directory on disk would move the removed
// torrent's files out from under the delete. Only the strings change.
TEST_F(RemoveTorrentTest, renamingADeferredTorrentLeavesTheFilesBeingDeletedAlone)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    auto state = std::shared_ptr<DeferredReadd>{};
    ASSERT_NO_FATAL_FAILURE(makeDeferredReadd(state));
    auto* const readded = state->readded;
    auto const old_name = std::string{ readded->name() };
    auto const renamed_path = tr_pathbuf{ state->download_dir, "/renamed"sv };

    auto rename_error = -1;
    tr_torrentRenamePath(
        readded,
        old_name,
        "renamed",
        [&rename_error](tr_torrent_id_t, std::string_view, std::string_view, tr_error const& error) noexcept
        { rename_error = error ? error.code() : 0; });
    EXPECT_TRUE(waitFor([&rename_error]() { return rename_error != -1; }, MaxWaitMsec));
    EXPECT_EQ(0, rename_error);
    EXPECT_EQ("renamed"sv, readded->name());

    // nothing moved on disk...
    EXPECT_TRUE(tr_sys_path_exists(state->first_file));
    EXPECT_FALSE(tr_sys_path_exists(renamed_path));

    // ...and under its new name the torrent is clear of the delete
    EXPECT_TRUE(waitFor([&state]() { return state->verified; }, MaxWaitMsec));
    EXPECT_FALSE(readded->deferred_init_.has_value());
    EXPECT_TRUE(readded->has_none());
    EXPECT_FALSE(state->stall_answered);

    ASSERT_NO_FATAL_FAILURE(releaseStall(*state));
    EXPECT_TRUE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([&state]() { return state->stall_answered; }, MaxWaitMsec));
    EXPECT_TRUE(readded->has_none());
    EXPECT_FALSE(tr_sys_path_exists(renamed_path));
#endif
}

// Pointing a deferred torrent at another download dir takes it out from
// under the delete: it looks at the new dir right away, not the old files.
TEST_F(RemoveTorrentTest, settingADeferredTorrentsDownloadDirChecksTheNewDir)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    auto state = std::shared_ptr<DeferredReadd>{};
    ASSERT_NO_FATAL_FAILURE(makeDeferredReadd(state));
    auto* const readded = state->readded;

    auto const new_dir = tr_pathbuf{ sandboxDir(), "/elsewhere"sv };
    ASSERT_TRUE(tr_sys_dir_create(new_dir, 0, 0700));
    tr_torrentSetDownloadDir(readded, new_dir);

    EXPECT_TRUE(waitFor([&state]() { return state->verified; }, MaxWaitMsec));
    EXPECT_FALSE(readded->deferred_init_.has_value());
    EXPECT_EQ(new_dir.sv(), readded->download_dir().sv());
    EXPECT_TRUE(readded->has_none());
    EXPECT_TRUE(tr_sys_path_exists(state->first_file));
    EXPECT_FALSE(state->stall_answered);

    ASSERT_NO_FATAL_FAILURE(releaseStall(*state));
    EXPECT_TRUE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, MaxWaitMsec));
    EXPECT_TRUE(readded->has_none());
#endif
}

// Removing a deferred torrent with its data must not queue a second delete
// of the same path: that one would run at once on the new torrent's queue,
// unordered against the first, which still owns those files.
TEST_F(RemoveTorrentTest, removingADeferredTorrentWithDataDeletesNothingItself)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the torrent's disk queue with a FIFO";
#else
    auto state = std::shared_ptr<DeferredReadd>{};
    ASSERT_NO_FATAL_FAILURE(makeDeferredReadd(state));
    auto const info_hash = state->readded->info_hash();

    tr_torrentRemove(state->readded, true);
    EXPECT_TRUE(waitFor([this, &info_hash]() { return session_->torrents().get(info_hash) == nullptr; }, MaxWaitMsec));

    // the first delete still owns the files
    EXPECT_FALSE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, 200));
    EXPECT_EQ(1U, std::size(session_->deleting_paths_));
    EXPECT_FALSE(state->stall_answered);

    ASSERT_NO_FATAL_FAILURE(releaseStall(*state));
    EXPECT_TRUE(waitFor([&state]() { return !tr_sys_path_exists(state->first_file); }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([this]() { return std::empty(session_->deleting_paths_); }, MaxWaitMsec));
    EXPECT_TRUE(state->stall_answered);
#endif
}
