// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error.h"
#include "libtransmission/file-utils.h" // tr_file_save()
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/state-writer.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"

using namespace std::literals;

namespace tr
{

StateWriter::~StateWriter()
{
    shutdown();
}

void StateWriter::save(std::string filename, std::string contents, OnDone on_done)
{
    enqueue(Job{ std::move(filename), std::move(contents), std::move(on_done) });
}

void StateWriter::remove(std::string filename, OnDone on_done)
{
    enqueue(Job{ std::move(filename), std::nullopt, std::move(on_done) });
}

void StateWriter::enqueue(Job job)
{
    auto lock = std::unique_lock(mutex_);

    if (stop_)
    {
        // Shut down: nothing may start now. Report it rather than lose it silently.
        lock.unlock();
        if (job.on_done)
        {
            auto error = tr_error{};
            error.set(ECANCELED, "state writer is shut down"sv);
            job.on_done(error);
        }
        return;
    }

    queue_.emplace_back(std::move(job));
    if (!thread_.joinable())
    {
        thread_ = std::thread{ [this]()
                               {
                                   run();
                               } };
    }
    work_cv_.notify_one();
}

bool StateWriter::has_job_for_unlocked(std::string_view const filename) const
{
    if (in_flight_ == filename)
    {
        return true;
    }

    for (auto const& job : queue_)
    {
        if (job.filename == filename)
        {
            return true;
        }
    }

    return false;
}

void StateWriter::flush(std::string_view const filename)
{
    auto lock = std::unique_lock(mutex_);
    done_cv_.wait(lock, [this, filename]() { return !has_job_for_unlocked(filename); });
}

size_t StateWriter::pending() const
{
    auto const lock = std::lock_guard(mutex_);
    return std::size(queue_) + (std::empty(in_flight_) ? 0U : 1U);
}

void StateWriter::run()
{
    auto lock = std::unique_lock(mutex_);
    for (;;)
    {
        work_cv_.wait(lock, [this]() { return stop_ || !std::empty(queue_); });
        if (std::empty(queue_))
        {
            break; // stop_ with nothing left
        }

        auto job = std::move(queue_.front());
        queue_.pop_front();
        in_flight_ = job.filename;
        lock.unlock();

        auto error = tr_error{};
        if (job.contents)
        {
            tr_file_save(job.filename, *job.contents, &error);
        }
        else if (!tr_sys_path_remove(job.filename, &error) && error.code() == ENOENT)
        {
            error = {};
        }

        if (job.on_done)
        {
            job.on_done(error);
        }

        lock.lock();
        in_flight_.clear();
        done_cv_.notify_all();
    }
}

void StateWriter::shutdown(std::chrono::steady_clock::time_point const deadline)
{
    auto dropped = std::deque<Job>{};

    {
        auto lock = std::unique_lock(mutex_);
        if (stopped_)
        {
            return;
        }

        auto const drained = [this]()
        {
            return std::empty(queue_) && std::empty(in_flight_);
        };
        if (deadline == std::chrono::steady_clock::time_point::max())
        {
            done_cv_.wait(lock, drained);
        }
        else if (!done_cv_.wait_until(lock, deadline, drained))
        {
            // Out of time. The job in flight finishes when the thread is
            // joined; nothing queued behind it may start.
            dropped.swap(queue_);
        }

        stop_ = true;
        stopped_ = true;
    }

    work_cv_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }

    for (auto& job : dropped)
    {
        tr_logAddWarn(fmt::format("Couldn't save '{}': out of time at shutdown", job.filename));
        if (job.on_done)
        {
            auto error = tr_error{};
            error.set(ECANCELED, "out of time at shutdown"sv);
            job.on_done(error);
        }
    }
}

} // namespace tr
