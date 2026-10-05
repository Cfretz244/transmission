// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <utime.h>

#include <gtest/gtest.h>

#include <libtransmission/peer-common.h>
#include <libtransmission/transmission.h>

#include <libtransmission/announcer.h>
#include <libtransmission/block-info.h>
#include <libtransmission/file.h> // tr_sys_path_*()
#include <libtransmission/local-data.h>
#include <libtransmission/peer-mgr.h>
#include <libtransmission/quark.h>
#include <libtransmission/torrent-files.h>
#include <libtransmission/torrent.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/variant.h>

#include "test-fixtures.h"

using namespace std::literals;

namespace tr::test
{

auto constexpr MaxWaitMsec = 5000;

class IncompleteDirTest
    : public SessionTest
    , public ::testing::WithParamInterface<std::pair<std::string, std::string>>
{
protected:
    void SetUp() override
    {
        if (auto* map = settings()->get_if<tr_variant::Map>(); map != nullptr)
        {
            auto const download_dir = GetParam().second;
            map->insert_or_assign(TR_KEY_download_dir, download_dir);
            auto const incomplete_dir = GetParam().first;
            map->insert_or_assign(TR_KEY_incomplete_dir, incomplete_dir);
            map->insert_or_assign(TR_KEY_incomplete_dir_enabled, true);
        }

        SessionTest::SetUp();
    }

    static auto constexpr MaxWaitMsec = 3000;
};

TEST_P(IncompleteDirTest, incompleteDir)
{
    std::string const download_dir = tr_sessionGetDownloadDir(session_);
    std::string const incomplete_dir = tr_sessionGetIncompleteDir(session_);

    // init an incomplete torrent.
    // the test zero_torrent will be missing its first piece.
    tr_sessionSetIncompleteFileNamingEnabled(session_, true);
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto path = tr_pathbuf{};

    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 0).name, tr_torrent_files::PartialFileSuffix);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 0));
    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 1).name);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 1));
    EXPECT_EQ(tor->piece_size(), tr_torrentStat(tor).left_until_done);

    auto completeness = TR_LEECH;
    tr_sessionSetCompletenessCallback(
        session_,
        [&completeness](tr_torrent_id_t const /*tor_id*/, tr_completeness const c, bool const /*was_running*/) noexcept
        { completeness = c; });

    struct TestIncompleteDirData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        tr_piece_index_t pieceIndex = {};
        std::unique_ptr<tr::LocalData::BlockData> buf;
        bool done = {};
    };

    auto const test_incomplete_dir_threadfunc = [](TestIncompleteDirData* data) noexcept
    {
        data->session->local_data.write(
            data->tor->make_io_plan(data->tor->block_info().byte_span_for_block(data->block)),
            std::move(data->buf),
            [data](tr_torrent_id_t tor_id, tr_byte_span_t, tr_error const& error, bool created_file)
            {
                tr_torrent::on_local_write_done(*data->session, tor_id, error, created_file);
                data->session->run_in_session_thread(
                    [data, error]()
                    {
                        if (!error)
                        {
                            data->tor->on_block_received(data->block);
                        }

                        data->done = true;
                    });
            });
    };

    // now finish writing it
    {
        auto data = TestIncompleteDirData{};
        data.session = session_;
        data.tor = tor;

        auto const [begin, end] = tor->block_span_for_piece(data.pieceIndex);

        for (tr_block_index_t block_index = begin; block_index < end; ++block_index)
        {
            data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
            std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
            data.block = block_index;
            data.done = false;
            session_->run_in_session_thread(test_incomplete_dir_threadfunc, &data);

            auto const test = [&data]()
            {
                return data.done;
            };
            EXPECT_TRUE(waitFor(test, MaxWaitMsec));
        }
    }

    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor).left_until_done);

    auto test = [&completeness]()
    {
        return completeness != TR_LEECH;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_SEED, completeness);

    // completion is announced before the move out of the incomplete dir
    // finishes, so wait for every file to land in the download dir
    auto const n = tr_torrentFileCount(tor);
    auto const all_files_moved = [tor, n, &download_dir]()
    {
        for (tr_file_index_t i = 0; i < n; ++i)
        {
            auto const expected = tr_pathbuf{ download_dir, '/', tr_torrentFile(tor, i).name };
            if (expected != tr_torrentFindFile(tor, i))
            {
                return false;
            }
        }
        return true;
    };
    EXPECT_TRUE(waitFor(all_files_moved, MaxWaitMsec));

    // cleanup
    tr_torrentRemove(tor, true);
}

class CorruptPieceTest : public SessionTest
{
protected:
    static auto constexpr MaxWaitMsec = 3000;
};

// With hashing asynchronous, every block of a piece can be on disk before
// its hash is known. The torrent must not report itself complete, and must
// not count those bytes as complete for trackers, until the check answers.
TEST_F(CorruptPieceTest, doesNotCompleteOnCorruptPiece)
{
    // the test zero_torrent is missing its first piece
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const piece_bytes = tor->piece_size();
    EXPECT_EQ(piece_bytes, tr_torrentStat(tor).left_until_done);

    auto seen_seed = false;
    tr_sessionSetCompletenessCallback(
        session_,
        [&seen_seed](tr_torrent_id_t const /*tor_id*/, tr_completeness const c, bool const /*was_running*/) noexcept
        { seen_seed = seen_seed || c != TR_LEECH; });

    struct WriteData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        std::unique_ptr<tr::LocalData::BlockData> buf;
        bool done = {};
    };

    auto const write_block = [](WriteData* data) noexcept
    {
        data->session->local_data.write(
            data->tor->make_io_plan(data->tor->block_info().byte_span_for_block(data->block)),
            std::move(data->buf),
            [data](tr_torrent_id_t tor_id, tr_byte_span_t, tr_error const& error, bool created_file)
            {
                tr_torrent::on_local_write_done(*data->session, tor_id, error, created_file);
                data->session->run_in_session_thread(
                    [data, error]()
                    {
                        if (!error)
                        {
                            data->tor->on_block_received(data->block);
                        }

                        data->done = true;
                    });
            });
    };

    // write piece 0, with the last block corrupted
    auto const [begin, end] = tor->block_span_for_piece(0);
    for (tr_block_index_t block = begin; block < end; ++block)
    {
        auto data = WriteData{};
        data.session = session_;
        data.tor = tor;
        data.block = block;
        data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
        std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, block + 1U == end ? '\x7f' : '\0');
        session_->run_in_session_thread(write_block, &data);
        EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));
    }

    // every block is present and the hash check is in flight
    EXPECT_TRUE(waitFor([tor]() { return !tor->has_pending_piece_tests(); }, MaxWaitMsec));

    // the check failed, so the piece is missing again and nothing was ever reported complete
    EXPECT_EQ(piece_bytes, tr_torrentStat(tor).left_until_done);
    EXPECT_EQ(0U, tor->unverified_bytes());
    EXPECT_FALSE(tor->is_done());
    EXPECT_FALSE(seen_seed);
    EXPECT_EQ(piece_bytes, tr_torrentStat(tor).corrupt_ever);

    tr_torrentRemove(tor, true);
}

// A check that cannot read the piece back must not leave the piece
// counted as held: that would announce `left` short of the truth and, if
// it was the last piece, send `completed` for data nobody ever hashed.
TEST_F(CorruptPieceTest, dropsPieceWhenCheckCannotReadIt)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const piece_bytes = tor->piece_size();
    EXPECT_EQ(piece_bytes, tr_torrentStat(tor).left_until_done);

    struct WriteData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        std::unique_ptr<tr::LocalData::BlockData> buf;
        bool done = {};
    };

    // write every block of piece 0 but hold back the last on_block_received,
    // so the piece's check is not queued yet
    auto const [begin, end] = tor->block_span_for_piece(0);
    for (tr_block_index_t block = begin; block < end; ++block)
    {
        auto data = WriteData{};
        data.session = session_;
        data.tor = tor;
        data.block = block;
        data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
        std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
        session_->run_in_session_thread(
            [](WriteData* data_ptr, bool const is_last)
            {
                data_ptr->session->local_data.write(
                    data_ptr->tor->make_io_plan(data_ptr->tor->block_info().byte_span_for_block(data_ptr->block)),
                    std::move(data_ptr->buf),
                    [data_ptr, is_last](tr_torrent_id_t, tr_byte_span_t, tr_error const& error, bool)
                    {
                        data_ptr->session->run_in_session_thread(
                            [data_ptr, is_last, error]()
                            {
                                EXPECT_FALSE(error) << error;
                                if (!is_last)
                                {
                                    data_ptr->tor->on_block_received(data_ptr->block);
                                }
                                data_ptr->done = true;
                            });
                    });
            },
            &data,
            block + 1U == end);
        EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));
    }

    // make the piece unreadable: the file goes away on disk, and the torrent's
    // cached fd for it is closed by a task that sits ahead of the check in the
    // torrent's FIFO, so the check has to reopen the file and fails to
    auto const filename = std::string{ tr_torrentFindFile(tor, 0) };
    ASSERT_FALSE(std::empty(filename));
    ASSERT_TRUE(tr_sys_path_remove(filename));
    session_->local_data.close_torrent(tor->id());

    auto done = false;
    session_->run_in_session_thread(
        [tor, end = end, &done]()
        {
            tor->on_block_received(end - 1U);
            EXPECT_TRUE(tor->has_piece(0U));
            EXPECT_TRUE(tor->has_pending_piece_tests());
            done = true;
        });
    EXPECT_TRUE(waitFor([&done]() { return done; }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([tor]() { return !tor->has_pending_piece_tests(); }, MaxWaitMsec));

    // the check failed to read, so the piece is missing again
    EXPECT_FALSE(tor->has_piece(0U));
    EXPECT_EQ(piece_bytes, tr_torrentStat(tor).left_until_done);
    EXPECT_EQ(0U, tor->unverified_bytes());
    EXPECT_FALSE(tor->is_done());
    EXPECT_TRUE(tor->error().is_local_error());

    tr_torrentRemove(tor, true);
}

class BlockWrittenTest : public SessionTest
{
protected:
    static auto constexpr MaxWaitMsec = 3000;

    struct WriteData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        std::unique_ptr<tr::LocalData::BlockData> buf;
        tr_block_source source = tr_block_source::Peer;
        std::string bad_dir; // if set, the write is planned into this unusable directory
        tr_error error;
        bool done = {};
    };

    // Writes one block the way peer-msgs and webseed do, except that the
    // sender is already gone (nullptr) when the write completes.
    static void write_from_departed_peer(WriteData* data)
    {
        auto plan = data->tor->make_io_plan(data->tor->block_info().byte_span_for_block(data->block));
        if (!std::empty(data->bad_dir))
        {
            plan.download_dir = data->bad_dir;
            plan.incomplete_dir = {};
            plan.current_dir = data->bad_dir;
        }

        data->tor->on_block_write_queued(data->block);
        data->session->local_data.write(
            std::move(plan),
            std::move(data->buf),
            [data](tr_torrent_id_t tor_id, tr_byte_span_t, tr_error const& error, bool created_file)
            {
                tr_torrent::on_local_write_done(*data->session, tor_id, error, created_file);
                if (auto* const tor = data->session->torrents().get(tor_id); tor != nullptr)
                {
                    tr_peerMgrBlockWritten(tor, nullptr, data->source, data->block, error);
                }
                data->error = error;
                data->done = true;
            });
    }

    // Writes every block of `piece`, zero-filled, from senders of kind
    // `source` that have all departed, and waits for the piece's hash check.
    void write_piece_from_departed_senders(tr_torrent* tor, tr_piece_index_t piece, tr_block_source source)
    {
        auto const [begin, end] = tor->block_span_for_piece(piece);
        for (auto block = begin; block < end; ++block)
        {
            auto data = WriteData{};
            data.session = session_;
            data.tor = tor;
            data.block = block;
            data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
            std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
            data.source = source;
            session_->run_in_session_thread(write_from_departed_peer, &data);
            EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));
            EXPECT_FALSE(data.error) << data.error;
        }

        EXPECT_TRUE(waitFor([tor]() { return !tor->has_pending_piece_tests(); }, MaxWaitMsec));
    }
};

// A completed piece is credited to the tracker as `downloaded` if peers
// sent it. That used to be decided by reading the senders' blame when the
// hash check answered, so a peer that disconnected in between cost the
// credit and the tracker was told less than the truth.
TEST_F(BlockWrittenTest, downloadedIsCreditedWithoutTheSender)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    ASSERT_EQ(0U, tr_announcerGetBytes(tor, TR_ANN_DOWN));

    write_piece_from_departed_senders(tor, 0U, tr_block_source::Peer);

    EXPECT_TRUE(tor->has_piece(0U));
    EXPECT_EQ(tor->piece_size(0U), tr_announcerGetBytes(tor, TR_ANN_DOWN));

    tr_torrentRemove(tor, true);
}

// Webseed downloads don't belong in announce totals.
TEST_F(BlockWrittenTest, webseedBlocksAreNotCreditedAsDownloaded)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);

    write_piece_from_departed_senders(tor, 0U, tr_block_source::Webseed);

    EXPECT_TRUE(tor->has_piece(0U));
    EXPECT_EQ(0U, tr_announcerGetBytes(tor, TR_ANN_DOWN));

    tr_torrentRemove(tor, true);
}

// A peer can disconnect while its block is still queued for writing.
// The block is the torrent's, not the peer's: it must still be recorded.
TEST_F(BlockWrittenTest, blockFromDepartedPeerIsStillHeld)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const block = tor->block_span_for_piece(0).begin;
    ASSERT_FALSE(tor->has_block(block));

    auto data = WriteData{};
    data.session = session_;
    data.tor = tor;
    data.block = block;
    data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
    std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
    session_->run_in_session_thread(write_from_departed_peer, &data);
    EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));

    EXPECT_FALSE(data.error) << data.error;
    EXPECT_TRUE(tor->has_block(block));
    EXPECT_FALSE(tor->error().is_local_error());

    tr_torrentRemove(tor, true);
}

// A block whose write fails never reached disk, so it must not be
// recorded as held; that would leave a hole that is never re-requested
// and announce `left` short of the truth.
TEST_F(BlockWrittenTest, blockWhoseWriteFailedIsNotHeld)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const block = tor->block_span_for_piece(0).begin;
    auto const had_total = tor->has_total();

    // a regular file where the plan expects a directory, so the write
    // cannot create its file
    auto const not_a_dir = tr_pathbuf{ sandboxDir(), "/not-a-dir" };
    createFileWithContents(not_a_dir, "x");

    auto data = WriteData{};
    data.session = session_;
    data.tor = tor;
    data.block = block;
    data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
    std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
    data.bad_dir = tr_pathbuf{ not_a_dir, "/sub" };
    session_->run_in_session_thread(write_from_departed_peer, &data);
    EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));

    EXPECT_TRUE(data.error);
    EXPECT_FALSE(tor->has_block(block));
    EXPECT_FALSE(tor->is_block_write_pending(block)); // free to be requested again
    EXPECT_EQ(had_total, tor->has_total());
    EXPECT_TRUE(tor->error().is_local_error());

    tr_torrentRemove(tor, true);
}

// Between a block's arrival and its write landing, has_block() is still
// false. Peers, webseeds and the wishlist used to treat that window as
// "not held", so an endgame duplicate was accepted and written over the
// first copy (possibly after the piece had been verified), and a wishlist
// rebuild re-requested the block. The torrent now reports the block as
// pending for that window.
TEST_F(BlockWrittenTest, blockIsPendingUntilItsWriteLands)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const block = tor->block_span_for_piece(0).begin;
    ASSERT_FALSE(tor->has_block_or_write_pending(block));

    auto data = WriteData{};
    data.session = session_;
    data.tor = tor;
    data.block = block;
    data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
    std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');

    // the completion runs on the session thread too, so it cannot have
    // happened yet when these are read right after the submit
    auto held_at_submit = true;
    auto pending_at_submit = false;
    session_->run_in_session_thread(
        [&data, &held_at_submit, &pending_at_submit]()
        {
            write_from_departed_peer(&data);
            held_at_submit = data.tor->has_block(data.block);
            pending_at_submit = data.tor->is_block_write_pending(data.block);
        });
    EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));

    EXPECT_FALSE(held_at_submit);
    EXPECT_TRUE(pending_at_submit);
    EXPECT_FALSE(data.error) << data.error;
    EXPECT_TRUE(tor->has_block(block));
    EXPECT_FALSE(tor->is_block_write_pending(block));

    tr_torrentRemove(tor, true);
}

// A corrupt piece found by an upload check, or a piece check that errors,
// sets a local error without stopping the torrent. A write that then
// fails used to be gated on "no local error yet", so the torrent stayed
// running and dropped every block it was sent (each one re-requested,
// never written) while telling the tracker it was still downloading.
TEST_F(BlockWrittenTest, writeErrorStopsTorrentThatAlreadyHasALocalError)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto const block = tor->block_span_for_piece(0).begin;

    // the torrent must be running to observe the stop; keep it off the
    // network first: no trackers, no local peer discovery
    tr_sessionSetLPDEnabled(session_, false);
    ASSERT_TRUE(tor->set_announce_list(tr_announce_list{}));
    tr_torrentStartNow(tor);
    ASSERT_TRUE(waitFor([tor]() { return tor->is_running(); }, MaxWaitMsec));

    // an earlier local error of the kind that leaves the torrent running
    auto error_set = false;
    session_->run_in_session_thread(
        [tor, &error_set]()
        {
            tor->error().set_local_error("Please Verify Local Data! Piece #0 is corrupt.");
            error_set = true;
        });
    ASSERT_TRUE(waitFor([&error_set]() { return error_set; }, MaxWaitMsec));
    ASSERT_TRUE(tor->error().is_local_error());
    ASSERT_TRUE(tor->is_running());

    auto const not_a_dir = tr_pathbuf{ sandboxDir(), "/not-a-dir" };
    createFileWithContents(not_a_dir, "x");

    auto data = WriteData{};
    data.session = session_;
    data.tor = tor;
    data.block = block;
    data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
    std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
    data.bad_dir = tr_pathbuf{ not_a_dir, "/sub" };
    session_->run_in_session_thread(write_from_departed_peer, &data);
    EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));

    EXPECT_TRUE(data.error);
    EXPECT_FALSE(tor->has_block(block));
    EXPECT_TRUE(waitFor([tor]() { return !tor->is_running(); }, MaxWaitMsec));

    tr_torrentRemove(tor, true);
}

// A piece whose last block lands just before the session closes still has
// its hash check queued. Closing must let that check finish and record its
// answer before the torrent's resume file is saved, so the piece is held
// after a restart instead of downloaded again (or, worse, saved unverified).
// The 32 KiB check usually wins the race against close even without that,
// so LocalData.ShutdownDrainsPieceTests is the deterministic guard; this
// covers the session-level path end to end.
TEST_F(BlockWrittenTest, pieceCheckedAtCloseIsHeldAfterRestart)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    ASSERT_FALSE(tor->has_piece(0U));
    auto const [begin, end] = tor->block_span_for_piece(0);

    // write every block of piece 0, holding back the last on_block_received
    for (tr_block_index_t block = begin; block < end; ++block)
    {
        auto data = WriteData{};
        data.session = session_;
        data.tor = tor;
        data.block = block;
        data.buf = std::make_unique<tr::LocalData::BlockData>(tr_block_info::BlockSize);
        std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
        auto const is_last = block + 1U == end;
        session_->run_in_session_thread(
            [](WriteData* data_ptr, bool const skip_receive)
            {
                data_ptr->session->local_data.write(
                    data_ptr->tor->make_io_plan(data_ptr->tor->block_info().byte_span_for_block(data_ptr->block)),
                    std::move(data_ptr->buf),
                    [data_ptr, skip_receive](tr_torrent_id_t, tr_byte_span_t, tr_error const& error, bool)
                    {
                        EXPECT_FALSE(error) << error;
                        if (!skip_receive)
                        {
                            data_ptr->tor->on_block_received(data_ptr->block);
                        }
                        data_ptr->done = true;
                    });
            },
            &data,
            is_last);
        EXPECT_TRUE(waitFor([&data]() { return data.done; }, MaxWaitMsec));
    }

    // queue the check and close the session right behind it
    session_->run_in_session_thread([tor, last = end - 1U]() { tor->on_block_received(last); });
    restartSession();

    // the resume file written at close must hold the verified piece
    auto* const ctor = zeroTorrentCtor();
    auto* const reloaded = tr_torrentNew(ctor, nullptr);
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, reloaded);
    EXPECT_TRUE(reloaded->has_piece(0U));
    EXPECT_TRUE(reloaded->has_all());
}

INSTANTIATE_TEST_SUITE_P(
    IncompleteDir,
    IncompleteDirTest,
    ::testing::Values(
        // what happens when incompleteDir is a subdir of downloadDir
        std::make_pair(std::string{ "Downloads/Incomplete" }, std::string{ "Downloads" }),
        // test what happens when downloadDir is a subdir of incompleteDir
        std::make_pair(std::string{ "Downloads" }, std::string{ "Downloads/Complete" }),
        // test what happens when downloadDir and incompleteDir are siblings
        std::make_pair(std::string{ "Incomplete" }, std::string{ "Downloads" })));

/***
****
***/

class PieceCheckTest : public SessionTest
{
protected:
    static auto constexpr MaxWaitMsec = 3000;

    struct CheckLog
    {
        size_t n_answers = 0;
        size_t n_passed = 0;
    };

    // Connects `log` to the torrent's piece_checked_ signal. Session thread only.
    static void log_checks(tr_torrent* tor, CheckLog* log)
    {
        tor->piece_checked_.connect(
            [log](tr_torrent*, tr_piece_index_t, bool const passed)
            {
                ++log->n_answers;
                if (passed)
                {
                    ++log->n_passed;
                }
            });
    }

    // Adds the zero torrent over complete, zero-filled files whose mtimes
    // are old enough for the add-time seed check to skip the full verify,
    // so only piece 0 is checked and the rest are held but unchecked.
    [[nodiscard]] tr_torrent* addZeroSeed()
    {
        auto* const ctor = zeroTorrentCtor();
        auto const& metainfo = *tr_ctorGetMetainfo(ctor);
        auto const dir = tr_sessionGetDownloadDir(session_);
        auto const mtime = time(nullptr) - 3600;
        for (tr_file_index_t i = 0, n = metainfo.file_count(); i < n; ++i)
        {
            auto const& subpath = metainfo.file_subpath(i);
            auto const filename = tr_pathbuf{ dir, '/', subpath };
            tr_sys_dir_create(tr_sys_path_dirname(filename), TR_SYS_DIR_CREATE_PARENTS, 0700);
            auto const contents = std::vector<char>(metainfo.file_size(i), '\0');
            auto const fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0600);
            EXPECT_NE(TR_BAD_SYS_FILE, fd);
            EXPECT_TRUE(tr_sys_file_write(fd, std::data(contents), std::size(contents), nullptr));
            EXPECT_TRUE(tr_sys_file_close(fd));
            auto const times = utimbuf{ mtime, mtime };
            EXPECT_EQ(0, utime(filename.c_str(), &times));
        }

        auto* const tor = createTorrentAndWaitForVerifyDone(ctor);
        tr_ctorFree(ctor);
        EXPECT_NE(nullptr, tor);
        EXPECT_TRUE(tor->has_all());
        return tor;
    }

    // Queues a read of `piece`'s first block behind whatever is already in
    // the torrent's LocalData FIFO, and waits for it. Anything queued before
    // the call, e.g. a piece check, has answered by the time this returns.
    void drain_local_data(tr_torrent* tor, tr_piece_index_t piece)
    {
        auto done = false;
        session_->local_data.read(
            tor->make_io_plan(tor->block_info().byte_span_for_block(tor->block_span_for_piece(piece).begin)),
            [&done](tr_torrent_id_t, tr_byte_span_t, tr_error const&, std::unique_ptr<tr::LocalData::BlockData>)
            { done = true; });
        EXPECT_TRUE(waitFor([&done]() { return done; }, MaxWaitMsec));
    }
};

// A peer asking for every block of an unchecked piece used to queue one
// full-piece hash per block. Requests for a piece whose check is already
// in flight must share it, and each request still gets its answer.
TEST_F(PieceCheckTest, requestsForOnePieceShareOneCheck)
{
    auto* const tor = addZeroSeed();
    auto constexpr Piece = tr_piece_index_t{ 1U };
    ASSERT_TRUE(tor->has_piece(Piece));
    ASSERT_FALSE(tor->is_piece_checked(Piece)); // only piece 0 is hashed at add time

    auto log = CheckLog{};
    auto requested = false;
    session_->run_in_session_thread(
        [tor, &log, &requested]()
        {
            log_checks(tor, &log);
            tor->request_piece_check(Piece);
            tor->request_piece_check(Piece);
            requested = true;
        });
    EXPECT_TRUE(waitFor([&requested]() { return requested; }, MaxWaitMsec));
    drain_local_data(tor, Piece);

    // one check answered once, and the piece is now known-good
    EXPECT_TRUE(waitFor([&log]() { return log.n_answers != 0U; }, MaxWaitMsec));
    EXPECT_EQ(1U, log.n_answers);
    EXPECT_EQ(1U, log.n_passed);
    EXPECT_TRUE(tor->is_piece_checked(Piece));
    EXPECT_FALSE(tor->error().is_local_error());

    // a request for a checked piece answers at once, without another hash
    auto answered = false;
    session_->run_in_session_thread(
        [tor, &log, &answered]()
        {
            tor->request_piece_check(Piece);
            answered = log.n_answers == 2U;
        });
    EXPECT_TRUE(waitFor([&answered]() { return answered; }, MaxWaitMsec));
    EXPECT_EQ(2U, log.n_passed);

    tr_torrentRemove(tor, true);
}

// A piece whose bytes changed under us must not be served: the check fails,
// the piece stays unchecked, and the torrent reports a local error.
TEST_F(PieceCheckTest, corruptPieceFailsItsCheck)
{
    auto* const tor = addZeroSeed();
    auto constexpr Piece = tr_piece_index_t{ 1U };
    ASSERT_TRUE(tor->has_piece(Piece));
    ASSERT_FALSE(tor->is_piece_checked(Piece));

    // flip a byte in the middle of the piece on disk
    auto const [file, offset] = tor->file_offset(tor->piece_loc(Piece, tor->piece_size(Piece) / 2U));
    auto const filename = std::string{ tr_torrentFindFile(tor, file) };
    ASSERT_FALSE(std::empty(filename));
    auto const fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE, 0600);
    ASSERT_NE(TR_BAD_SYS_FILE, fd);
    auto constexpr Byte = char{ '\x7f' };
    EXPECT_TRUE(tr_sys_file_write_at(fd, &Byte, 1U, offset, nullptr));
    EXPECT_TRUE(tr_sys_file_close(fd));

    auto log = CheckLog{};
    auto requested = false;
    session_->run_in_session_thread(
        [tor, &log, &requested]()
        {
            log_checks(tor, &log);
            tor->request_piece_check(Piece);
            requested = true;
        });
    EXPECT_TRUE(waitFor([&requested]() { return requested; }, MaxWaitMsec));
    EXPECT_TRUE(waitFor([&log]() { return log.n_answers != 0U; }, MaxWaitMsec));

    EXPECT_EQ(1U, log.n_answers);
    EXPECT_EQ(0U, log.n_passed);
    EXPECT_FALSE(tor->is_piece_checked(Piece));
    EXPECT_TRUE(tor->error().is_local_error());

    tr_torrentRemove(tor, true);
}

using MoveTest = SessionTest;

TEST_F(MoveTest, setLocation)
{
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/target"sv };
    tr_sys_dir_create(target_dir, TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    // init a torrent.
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor).left_until_done);

    // now move it
    auto state = -1;
    tr_torrentSetLocation(tor, target_dir, true, &state);
    auto test = [&state]()
    {
        return state == TR_LOC_DONE;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_LOC_DONE, state);

    // confirm the torrent is still complete after being moved
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor).left_until_done);

    // confirm the files really got moved
    sync();
    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ target_dir, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }

    // cleanup
    tr_torrentRemove(tor, true);
}

// Two moves queued back to back, A->B then B->C, where the move to B
// fails (B is a file, not a directory). The torrent must end up pointing
// where its files actually are: a move planned from the dirs a failed
// move had set would find nothing to move and "succeed", leaving the
// torrent at C with its files still at A.
TEST_F(MoveTest, moveQueuedBehindAFailedMoveStartsFromWhereTheFilesAre)
{
    auto const dir_a = std::string{ tr_sessionGetDownloadDir(session_) };
    auto const not_a_dir = tr_pathbuf{ session_->configDir(), "/not-a-dir"sv };
    createFileWithContents(not_a_dir, "x");
    auto const dir_c = tr_pathbuf{ session_->configDir(), "/target"sv };

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor).left_until_done);

    auto state_b = -1;
    auto state_c = -1;
    tr_torrentSetLocation(tor, not_a_dir, true, &state_b);
    tr_torrentSetLocation(tor, dir_c, true, &state_c);
    EXPECT_TRUE(waitFor([&state_b, &state_c]() { return state_b != TR_LOC_MOVING && state_c != TR_LOC_MOVING; }, MaxWaitMsec));
    EXPECT_EQ(TR_LOC_ERROR, state_b);
    EXPECT_EQ(TR_LOC_DONE, state_c);

    // the second move started from A, where the files were, and landed them in C
    sync();
    EXPECT_EQ(dir_c, tor->download_dir());
    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ dir_c, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor).left_until_done);

    // cleanup
    tr_torrentRemove(tor, true);
}

} // namespace tr::test
