// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <chrono>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h> // open()
#include <sys/stat.h> // mkfifo()
#include <unistd.h> // close(), unlink()
#endif

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/file.h>
#include <libtransmission/string-utils.h> // tr_strerror()
#include <libtransmission/torrent.h>
#include <libtransmission/tr-strbuf.h>

#include "test-fixtures.h"

using namespace std::literals;

namespace tr::test
{

class VerifyTest : public SessionTest
{
};

// Stopping a torrent pulls it off the verify worker. That used to wait for
// the worker thread to finish the read it was in, so a slow or wedged disk
// under a verify stalled every stop and remove on the session thread. Now
// the worker is only flagged: its results hop to the session thread and are
// dropped once the torrent has been pulled, so nothing waits on the disk.
TEST_F(VerifyTest, stopDoesNotWaitForAStalledVerify)
{
#ifdef _WIN32
    GTEST_SKIP() << "stalls the verify thread with a FIFO";
#else
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    ASSERT_TRUE(tor->has_all());

    // Swap the torrent's first file for a FIFO with no writer: the verify
    // thread blocks in open() on it until the test opens the other end.
    auto const found = tor->find_file(0U);
    ASSERT_TRUE(found);
    auto const path = std::string{ found->filename().sv() };
    auto const size = tor->file_size(0U);
    ASSERT_TRUE(tr_sys_path_remove(path));
    ASSERT_EQ(0, mkfifo(path.c_str(), 0600)) << tr_strerror(errno);

    tr_torrentVerify(tor);
    ASSERT_TRUE(waitFor([tor]() { return tor->activity() == TR_STATUS_CHECK; }, 5s));
    // the thread has posted "started"; its next step is the open() above
    std::this_thread::sleep_for(50ms);

    // Stop. Before, this blocked until the FIFO got a writer.
    tr_torrentStop(tor);
    EXPECT_TRUE(waitFor([tor]() { return tor->activity() == TR_STATUS_STOPPED; }, 2s));
    EXPECT_TRUE(tor->has_all());

    // Release the verify thread. It reads nothing from the FIFO, fails the
    // piece, notices it was stopped, and reports -- to nobody: it was pulled.
    auto writer = -1;
    EXPECT_TRUE(waitFor(
        [&writer, &path]()
        {
            writer = open(path.c_str(), O_WRONLY | O_NONBLOCK);
            return writer != -1 || errno != ENXIO;
        },
        5s));
    ASSERT_NE(-1, writer) << tr_strerror(errno);
    close(writer);

    // Verifying another torrent is a barrier: the worker runs verifies in
    // order, so by the time this one is done the stalled one has finished
    // reporting, and its stale "piece 0 is bad" never landed.
    auto* const other_ctor = tr_ctorNew(session_);
    ASSERT_TRUE(other_ctor->set_metainfo_from_file(tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, "/perfect-pieces.torrent"sv }));
    tr_ctorSetPaused(other_ctor, TR_FORCE, true); // keep it local: never start it
    auto* const other = createTorrentAndWaitForVerifyDone(other_ctor);
    tr_ctorFree(other_ctor);
    ASSERT_NE(nullptr, other);
    blockingTorrentVerify(other);
    EXPECT_TRUE(tor->has_all());
    EXPECT_EQ(TR_STATUS_STOPPED, tor->activity());

    // Put the file back; a fresh verify owns its own results.
    ASSERT_EQ(0, unlink(path.c_str())) << tr_strerror(errno);
    createFileWithContents(path, std::string(size, '\0'));
    blockingTorrentVerify(tor);
    EXPECT_TRUE(tor->has_all());
#endif
}

} // namespace tr::test
