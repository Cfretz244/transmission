// This file copyright Transmission authors and contributors.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef> // size_t
#include <cstdint> // uint64_t
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h> // fcntl()

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/error.h>
#include <libtransmission/file.h>
#include <libtransmission/open-files.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

using namespace std::literals;

using OpenFilesTest = tr::test::SessionTest;

static auto constexpr PreallocateFull = tr_file_preallocation::Full;

TEST_F(OpenFilesTest, getCachedFailsIfNotCached)
{
    auto const lease = session_->openFiles().get(0, 0, false);
    EXPECT_FALSE(lease);
}

TEST_F(OpenFilesTest, getOpensIfNotCached)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    // confirm that it's not pre-cached
    EXPECT_FALSE(session_->openFiles().get(0, 0, false));

    // confirm that we can cache the file
    auto const lease = session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents));
    ASSERT_TRUE(lease);
    EXPECT_NE(TR_BAD_SYS_FILE, lease.fd());

    // test the file contents to confirm that fd points to the right file
    auto buf = std::array<char, std::size(Contents) + 1>{};
    auto bytes_read = uint64_t{};
    EXPECT_TRUE(tr_sys_file_read_at(lease.fd(), std::data(buf), std::size(Contents), 0, &bytes_read));
    auto const contents = std::string_view{ std::data(buf), static_cast<size_t>(bytes_read) };
    EXPECT_EQ(Contents, contents);
}

TEST_F(OpenFilesTest, getCacheSucceedsIfCached)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    EXPECT_FALSE(session_->openFiles().get(0, 0, false));
    EXPECT_TRUE(session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents)));
    EXPECT_TRUE(session_->openFiles().get(0, 0, false));
}

TEST_F(OpenFilesTest, getCachedReturnsTheSameFd)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    EXPECT_FALSE(session_->openFiles().get(0, 0, false));
    auto const lease1 = session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents));
    auto const lease2 = session_->openFiles().get(0, 0, false);
    ASSERT_TRUE(lease1);
    ASSERT_TRUE(lease2);
    EXPECT_EQ(lease1.fd(), lease2.fd());
}

TEST_F(OpenFilesTest, getCachedFailsIfWrongPermissions)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    // cache it in ro mode
    EXPECT_FALSE(session_->openFiles().get(0, 0, false));
    EXPECT_TRUE(session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents)));

    // now try to get it in r/w mode
    EXPECT_TRUE(session_->openFiles().get(0, 0, false));
    EXPECT_FALSE(session_->openFiles().get(0, 0, true));
}

TEST_F(OpenFilesTest, opensInReadOnlyUnlessWritableIsRequested)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    // cache a file read-only mode
    auto const lease = session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents));
    ASSERT_TRUE(lease);

    // confirm that writing to it fails
    auto error = tr_error{};
    EXPECT_FALSE(tr_sys_file_write(lease.fd(), std::data(Contents), std::size(Contents), nullptr, &error));
    EXPECT_TRUE(error);
}

TEST_F(OpenFilesTest, createsMissingFileIfWriteRequested)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    EXPECT_FALSE(tr_sys_path_exists(filename));

    auto lease = session_->openFiles().get(0, 0, false);
    EXPECT_FALSE(lease);
    EXPECT_FALSE(tr_sys_path_exists(filename));

    lease = session_->openFiles().get(0, 0, true, filename, PreallocateFull, std::size(Contents));
    ASSERT_TRUE(lease);
    EXPECT_NE(TR_BAD_SYS_FILE, lease.fd());
    EXPECT_TRUE(tr_sys_path_exists(filename));
}

TEST_F(OpenFilesTest, closeFileClosesTheFile)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    // cache a file read-only mode
    EXPECT_TRUE(session_->openFiles().get(0, 0, false, filename, PreallocateFull, std::size(Contents)));
    EXPECT_TRUE(session_->openFiles().get(0, 0, false));

    // close the file
    session_->openFiles().close_file(0, 0);

    // confirm that its fd is no longer cached
    EXPECT_FALSE(session_->openFiles().get(0, 0, false));
}

TEST_F(OpenFilesTest, closeTorrentClosesTheTorrentFiles)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    static auto constexpr TorId = tr_torrent_id_t{ 0 };

    auto filename = tr_pathbuf{ sandboxDir(), "/a.txt" };
    createFileWithContents(filename, Contents);
    EXPECT_TRUE(session_->openFiles().get(TorId, 1, false, filename, PreallocateFull, std::size(Contents)));

    filename.assign(sandboxDir(), "/b.txt");
    createFileWithContents(filename, Contents);
    EXPECT_TRUE(session_->openFiles().get(TorId, 3, false, filename, PreallocateFull, std::size(Contents)));

    // confirm that closing a different torrent does not affect these files
    session_->openFiles().close_torrent(TorId + 1);
    EXPECT_TRUE(session_->openFiles().get(TorId, 1, false));
    EXPECT_TRUE(session_->openFiles().get(TorId, 3, false));

    // confirm that closing this torrent closes and uncaches the files
    session_->openFiles().close_torrent(TorId);
    EXPECT_FALSE(session_->openFiles().get(TorId, 1, false));
    EXPECT_FALSE(session_->openFiles().get(TorId, 3, false));
}

TEST_F(OpenFilesTest, closesLeastRecentlyUsedFile)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    static auto constexpr TorId = tr_torrent_id_t{ 0 };
    static auto constexpr LargerThanCacheLimit = 100;

    // Walk through a number of files. Confirm that they all succeed
    // even when the number exhausts the cache size, and newer files
    // supplant older ones.
    for (int i = 0; i < LargerThanCacheLimit; ++i)
    {
        auto filename = tr_pathbuf{ sandboxDir(), fmt::format("/file-{:d}.txt"sv, i) };
        EXPECT_TRUE(session_->openFiles().get(TorId, i, true, filename, PreallocateFull, std::size(Contents)));
    }

    // Do a lookup-only for the files again *in the same order*. By following the
    // order, the first files we check will be the oldest from the last pass and
    // should have aged out. So we should have a nonzero number of failures; but
    // once we get a success, all the remaining should also succeed.
    auto results = std::array<bool, LargerThanCacheLimit>{};
    auto sorted = std::array<bool, LargerThanCacheLimit>{};
    for (int i = 0; i < LargerThanCacheLimit; ++i)
    {
        auto filename = tr_pathbuf{ sandboxDir(), fmt::format("/file-{:d}.txt"sv, i) };
        results[i] = static_cast<bool>(session_->openFiles().get(TorId, i, false));
    }
    sorted = results;
    std::ranges::sort(sorted);
    EXPECT_EQ(sorted, results);
    EXPECT_GT(std::ranges::count(results, true), 0);
}

namespace
{

[[nodiscard]] bool is_fd_open(tr_sys_file_t fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

} // namespace

TEST_F(OpenFilesTest, closeClosesUnleasedFileImmediately)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    auto const filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    auto& open_files = session_->openFiles();
    auto const fd = open_files.get(0, 0, false, filename, PreallocateFull, std::size(Contents)).fd();
    ASSERT_NE(TR_BAD_SYS_FILE, fd);
    EXPECT_TRUE(is_fd_open(fd)); // cached

    open_files.close_file(0, 0);
    EXPECT_FALSE(is_fd_open(fd));
    EXPECT_EQ(EBADF, errno);
}

TEST_F(OpenFilesTest, leasedFileStaysOpenUntilReleased)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    static auto constexpr TorId = tr_torrent_id_t{ 1 };
    auto const filename = tr_pathbuf{ sandboxDir(), "/test-file.txt" };
    createFileWithContents(filename, Contents);

    auto& open_files = session_->openFiles();
    auto const closers = std::array<std::function<void()>, 3>{
        [&open_files]() { open_files.close_file(TorId, 0); },
        [&open_files]() { open_files.close_torrent(TorId); },
        [&open_files]() { open_files.close_all(); },
    };

    for (auto const& close : closers)
    {
        auto lease = open_files.get(TorId, 0, false, filename, PreallocateFull, std::size(Contents));
        ASSERT_TRUE(lease);
        auto const fd = lease.fd();

        close();

        // the file is uncached, but the leased fd still reads the file
        EXPECT_FALSE(open_files.get(TorId, 0, false));
        EXPECT_TRUE(is_fd_open(fd));
        auto buf = std::array<char, std::size(Contents)>{};
        auto bytes_read = uint64_t{};
        EXPECT_TRUE(tr_sys_file_read_at(fd, std::data(buf), std::size(buf), 0, &bytes_read));
        EXPECT_EQ(Contents, std::string_view(std::data(buf), static_cast<size_t>(bytes_read)));

        // releasing the lease closes the fd
        lease = {};
        EXPECT_FALSE(is_fd_open(fd));
        EXPECT_EQ(EBADF, errno);
    }
}

TEST_F(OpenFilesTest, getSucceedsWhenEveryCachedFileIsLeased)
{
    static auto constexpr Contents = "Hello, World!\n"sv;
    static auto constexpr TorId = tr_torrent_id_t{ 1 };
    auto open_files = tr_open_files{ 2U };

    auto filenames = std::array<tr_pathbuf, 3>{};
    for (size_t i = 0; i < std::size(filenames); ++i)
    {
        filenames[i].assign(sandboxDir(), fmt::format("/file-{:d}.txt"sv, i));
        createFileWithContents(filenames[i], Contents);
    }

    // fill the cache with leased files
    auto const lease0 = open_files.get(TorId, 0, false, filenames[0], PreallocateFull, std::size(Contents));
    auto const lease1 = open_files.get(TorId, 1, false, filenames[1], PreallocateFull, std::size(Contents));
    ASSERT_TRUE(lease0);
    ASSERT_TRUE(lease1);

    // a third file is served without evicting either leased file
    auto lease2 = open_files.get(TorId, 2, false, filenames[2], PreallocateFull, std::size(Contents));
    ASSERT_TRUE(lease2);
    auto const fd2 = lease2.fd();
    EXPECT_TRUE(is_fd_open(lease0.fd()));
    EXPECT_TRUE(is_fd_open(lease1.fd()));
    EXPECT_TRUE(is_fd_open(fd2));
    EXPECT_TRUE(open_files.get(TorId, 0, false));
    EXPECT_TRUE(open_files.get(TorId, 1, false));

    // the third file was not cached, so releasing its lease closes it
    EXPECT_FALSE(open_files.get(TorId, 2, false));
    lease2 = {};
    EXPECT_FALSE(is_fd_open(fd2));
}

// Each thread plays one torrent, as LocalData runs one torrent's tasks at a time.
// With a cache smaller than the thread count, threads constantly evict each
// other's files. If a thread's fd could be closed and its number reused for
// another torrent's file mid-I/O, that thread's writes would land in the other
// torrent's file, so each thread writes only its own region of its own files
// and every other region must still be a hole at the end.
TEST_F(OpenFilesTest, concurrentLeasesKeepTheirFiles)
{
    static auto constexpr NumThreads = 4;
    static auto constexpr FilesPerTorrent = 3;
    static auto constexpr BlockSize = size_t{ 4096U };
    static auto constexpr FileSize = uint64_t{ BlockSize * NumThreads };
    static auto constexpr Duration = std::chrono::seconds{ 1 };

    auto open_files = tr_open_files{ 2U };
    auto const dir = sandboxDir();
    auto const filename = [&dir](tr_torrent_id_t tor_id, tr_file_index_t file_num)
    {
        return tr_pathbuf{ dir, fmt::format("/tor-{:d}-file-{:d}"sv, tor_id, file_num) };
    };
    auto const pattern_of = [](tr_torrent_id_t tor_id)
    {
        return static_cast<uint8_t>('A' + tor_id);
    };

    auto stop = std::atomic<bool>{ false };
    auto failures = std::atomic<int>{ 0 };
    auto iterations = std::atomic<uint64_t>{ 0U };

    auto const worker = [&](tr_torrent_id_t const tor_id)
    {
        auto const pattern = std::vector<uint8_t>(BlockSize, pattern_of(tor_id));
        auto const offset = uint64_t{ BlockSize * static_cast<size_t>(tor_id) };
        auto readback = std::vector<uint8_t>(BlockSize);

        for (uint64_t i = 0U; !stop; ++i)
        {
            auto const file_num = static_cast<tr_file_index_t>(i % FilesPerTorrent);
            auto const lease = open_files.get(
                tor_id,
                file_num,
                true,
                filename(tor_id, file_num),
                tr_file_preallocation::None,
                FileSize);
            auto n = uint64_t{};
            auto ok = static_cast<bool>(lease) &&
                tr_sys_file_write_at(lease.fd(), std::data(pattern), std::size(pattern), offset, &n) && n == BlockSize;

            // closing mid-lease must not close the fd under us
            if (i % 5U == 0U)
            {
                open_files.close_torrent(tor_id);
            }

            ok = ok && tr_sys_file_read_at(lease.fd(), std::data(readback), std::size(readback), offset, &n) &&
                n == BlockSize && readback == pattern;

            if (!ok)
            {
                ++failures;
            }

            ++iterations;
        }
    };

    auto threads = std::vector<std::thread>{};
    for (tr_torrent_id_t tor_id = 0; tor_id < NumThreads; ++tor_id)
    {
        threads.emplace_back(worker, tor_id);
    }
    std::this_thread::sleep_for(Duration);
    stop = true;
    for (auto& thread : threads)
    {
        thread.join();
    }
    open_files.close_all();

    EXPECT_EQ(0, failures);
    EXPECT_GT(iterations, 0U);

    // every file holds its own torrent's pattern in its own region, and zeros elsewhere
    for (tr_torrent_id_t tor_id = 0; tor_id < NumThreads; ++tor_id)
    {
        for (tr_file_index_t file_num = 0; file_num < FilesPerTorrent; ++file_num)
        {
            auto const fd = tr_sys_file_open(filename(tor_id, file_num), TR_SYS_FILE_READ, 0);
            ASSERT_NE(TR_BAD_SYS_FILE, fd);
            // without preallocation, the file ends where its last write ended
            auto contents = std::vector<uint8_t>(FileSize);
            EXPECT_TRUE(tr_sys_file_read_at(fd, std::data(contents), std::size(contents), 0, nullptr));
            tr_sys_file_close(fd);

            for (tr_torrent_id_t region = 0; region < NumThreads; ++region)
            {
                auto const expected = region == tor_id ? pattern_of(tor_id) : uint8_t{ 0 };
                auto const begin = std::begin(contents) + static_cast<ptrdiff_t>(BlockSize * static_cast<size_t>(region));
                EXPECT_EQ(BlockSize, static_cast<size_t>(std::count(begin, begin + BlockSize, expected)))
                    << "torrent " << tor_id << " file " << file_num << " region " << region;
            }
        }
    }
}
