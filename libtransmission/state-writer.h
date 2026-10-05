// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <chrono>
#include <condition_variable>
#include <cstddef> // size_t
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "libtransmission/error.h"

namespace tr
{

// Writes the session's small state files -- resume files, stats.json, the
// queue order -- on its own thread so that the session thread, and with it
// every JSON-RPC request, never waits on the disk that holds the config dir.
//
// Jobs run strictly in the order they were queued, so a save followed by a
// remove of the same file leaves no file, and the last of several saves of
// one file is the one on disk. A reader on the session thread that needs the
// file to be current calls flush() first.
class StateWriter
{
public:
    // Runs on the writer thread when the job is done. Must not block and
    // must not touch anything the job's owner may have freed: post to the
    // session thread and look the owner up there.
    using OnDone = std::function<void(tr_error const& error)>;

    StateWriter() = default;
    ~StateWriter();

    StateWriter(StateWriter const&) = delete;
    StateWriter(StateWriter&&) = delete;
    StateWriter& operator=(StateWriter const&) = delete;
    StateWriter& operator=(StateWriter&&) = delete;

    // Write `contents` to `filename` (via a temp file and rename, like tr_file_save()).
    void save(std::string filename, std::string contents, OnDone on_done = {});

    // Delete `filename`. A missing file is not an error.
    void remove(std::string filename, OnDone on_done = {});

    // Block until every job queued so far for `filename` has run.
    void flush(std::string_view filename);

    // Block until every queued job has run, or until `deadline`, whichever
    // is first, then stop the thread. Jobs still queued at the deadline are
    // dropped and reported as cancelled; a job in flight finishes on its own.
    void shutdown(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());

    [[nodiscard]] size_t pending() const;

private:
    struct Job
    {
        std::string filename;
        std::optional<std::string> contents; // nullopt: remove
        OnDone on_done;
    };

    void enqueue(Job job);
    void run();
    [[nodiscard]] bool has_job_for_unlocked(std::string_view filename) const;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::deque<Job> queue_;
    std::string in_flight_;
    std::thread thread_;
    bool stop_ = false;
    bool stopped_ = false;
};

} // namespace tr
