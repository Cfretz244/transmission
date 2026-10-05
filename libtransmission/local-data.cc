// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libtransmission/local-data.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h"
#include "libtransmission/log.h"
#include "libtransmission/open-files.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/transmission.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h"

namespace tr
{
namespace
{

// Upper bound on the default worker count. Disk parallelism rarely helps
// beyond a few outstanding requests, and each idle worker costs a thread.
auto constexpr MaxDefaultWorkers = size_t{ 4U };

[[nodiscard]] tr_error make_error(tr_error_code_t err)
{
    auto error = tr_error{};
    if (err != 0)
    {
        error.set_from_errno(err);
    }
    return error;
}

[[nodiscard]] tr_io_result make_result(tr_error_code_t err)
{
    auto result = tr_io_result{};
    result.error = make_error(err);
    return result;
}

void log_result(tr_io_result const& result, std::string_view const tor_name)
{
    if (!std::empty(result.log_message))
    {
        tr_logAddError(std::string{ result.log_message }, tor_name);
    }
}

class DefaultBackend final : public LocalData::Backend
{
public:
    explicit DefaultBackend(tr_open_files& open_files)
        : open_files_{ open_files }
    {
    }

    [[nodiscard]] tr_io_result read(tr_io_plan const& plan, LocalData::BlockData& setme) override
    {
        auto const len = plan.byte_span.is_valid() ? plan.byte_span.size() : 0U;
        if (len > setme.capacity())
        {
            return make_result(TR_ERROR_EINVAL);
        }

        auto const span_size = static_cast<size_t>(len);
        setme.resize(span_size);
        return tr_ioRead(plan, open_files_, 0U, std::span{ std::data(setme), span_size });
    }

    [[nodiscard]] tr_io_result test_piece(tr_io_plan const& plan, tr_sha1_digest_t& setme_hash) override
    {
        return tr_ioHashSpan(plan, open_files_, setme_hash);
    }

    [[nodiscard]] tr_io_result write(tr_io_plan const& plan, LocalData::BlockData const& data) override
    {
        auto const len = plan.byte_span.is_valid() ? plan.byte_span.size() : 0U;
        if (len > std::size(data))
        {
            return make_result(TR_ERROR_EINVAL);
        }

        return tr_ioWrite(plan, open_files_, std::span{ std::data(data), static_cast<size_t>(len) });
    }

    [[nodiscard]] tr_error_code_t move(
        tr_torrent_id_t /*id*/,
        tr_torrent_files const& files,
        std::string_view const old_parent,
        std::string_view const parent,
        std::string_view const parent_name) override
    {
        auto error = tr_error{};
        if (files.move(old_parent, parent, parent_name, &error))
        {
            return 0;
        }

        return error ? error.code() : EIO;
    }

    [[nodiscard]] tr_error remove(
        tr_torrent_id_t /*id*/,
        tr_torrent_files const& files,
        std::string_view const parent,
        std::string_view const name,
        tr_torrent_remove_func const& remove_func) override
    {
        auto error = tr_error{};
        files.remove(parent, name, remove_func ? remove_func : tr_torrent_remove_func{ tr_sys_path_remove }, &error);
        return error;
    }

    // Renames the path on disk only. The torrent's own record of its file
    // names is updated by the caller, on the session thread.
    [[nodiscard]] tr_error_code_t rename(
        tr_torrent_id_t /*id*/,
        std::string_view const base,
        std::string_view const oldpath,
        std::string_view const newname) override
    {
        auto src = tr_pathbuf{ base, '/', oldpath };
        if (!tr_sys_path_exists(src))
        {
            src += tr_torrent_files::PartialFileSuffix;
        }

        if (!tr_sys_path_exists(src))
        {
            return 0;
        }

        auto const parent = tr_sys_path_dirname(src);
        auto const tgt = tr_strv_ends_with(src, tr_torrent_files::PartialFileSuffix) ?
            tr_pathbuf{ parent, '/', newname, tr_torrent_files::PartialFileSuffix } :
            tr_pathbuf{ parent, '/', newname };
        if (tr_sys_path_exists(tgt))
        {
            return 0;
        }

        auto error = tr_error{};
        if (!tr_sys_path_rename(src, tgt, &error))
        {
            return error.code();
        }

        return 0;
    }

    void close_all() override
    {
        open_files_.close_all();
    }

    void close_torrent(tr_torrent_id_t const tor_id) override
    {
        open_files_.close_torrent(tor_id);
    }

    void close_file(tr_torrent_id_t const tor_id, tr_file_index_t const file_num) override
    {
        open_files_.close_file(tor_id, file_num);
    }

private:
    tr_open_files& open_files_;
};

} // namespace

// ---

/**
 * Worker pool with one FIFO queue per torrent.
 *
 * A torrent whose queue is non-empty and that has no task running sits
 * in `runnable_ids_`. A worker pops a runnable torrent, marks it active,
 * runs its front task, and on completion re-queues the torrent if more
 * tasks arrived. So at most one task per torrent runs at a time, and
 * tasks for one torrent run in enqueue order.
 *
 * Reads and tests are "read-like": they are discarded at shutdown
 * because nothing is lost by dropping them. Writes, moves, renames,
 * removes and closes are drained before shutdown returns.
 */
class LocalData::Impl
{
private:
    enum class Op : std::uint8_t
    {
        Read,
        Test,
        Write,
        CloseFile,
        CloseTorrent,
        Move,
        Remove,
        Rename
    };

    struct Task
    {
        tr_torrent_id_t id = -1;
        Op op = Op::Read;
        uint64_t write_bytes = 0;
        std::function<void()> run;
        std::function<void()> cancel;
    };

public:
    Impl(std::unique_ptr<Backend> backend, Dispatcher dispatcher, size_t worker_count)
        : backend_{ std::move(backend) }
        , dispatcher_{ std::move(dispatcher) }
    {
        if (!dispatcher_)
        {
            dispatcher_ = [](std::function<void()> func) { func(); };
        }

        if (worker_count == 0U)
        {
            worker_count = std::clamp(static_cast<size_t>(std::thread::hardware_concurrency()), size_t{ 1U }, MaxDefaultWorkers);
        }

        workers_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i)
        {
            workers_.emplace_back(&Impl::worker_thread, this);
        }
    }

    Impl(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
        shutdown(std::chrono::steady_clock::time_point::max());
    }

    void read(tr_io_plan plan, OnRead on_read)
    {
        auto callback = std::make_shared<OnRead>(std::move(on_read));
        auto const id = plan.tor_id;
        auto const byte_span = plan.byte_span;

        auto task = Task{
            .id = id,
            .op = Op::Read,
            .run =
                [this, plan = std::move(plan), callback]()
            {
                auto data = std::make_unique<BlockData>();
                auto result = backend_->read(plan, *data);
                if (result.error)
                {
                    data.reset();
                }
                dispatch(
                    [id = plan.tor_id,
                     byte_span = plan.byte_span,
                     name = plan.name,
                     callback,
                     result = std::move(result),
                     data = std::shared_ptr<BlockData>{ std::move(data) }]() mutable
                    {
                        log_result(result, name);
                        auto block = data ? std::make_unique<BlockData>(std::move(*data)) : nullptr;
                        (*callback)(id, byte_span, result.error, std::move(block));
                    });
            },
            .cancel = [this, id, byte_span, callback]()
            { dispatch([id, byte_span, callback]() { (*callback)(id, byte_span, make_error(ECANCELED), nullptr); }); },
        };

        enqueue(std::move(task));
    }

    void test_piece(tr_io_plan plan, tr_piece_index_t piece, OnTest on_test)
    {
        auto callback = std::make_shared<OnTest>(std::move(on_test));
        auto const id = plan.tor_id;

        auto task = Task{
            .id = id,
            .op = Op::Test,
            .run =
                [this, plan = std::move(plan), piece, callback]()
            {
                auto hash = tr_sha1_digest_t{};
                auto result = backend_->test_piece(plan, hash);
                auto const maybe_hash = result.error ? std::nullopt : std::optional<tr_sha1_digest_t>{ hash };
                dispatch(
                    [id = plan.tor_id, name = plan.name, piece, callback, result = std::move(result), maybe_hash]()
                    {
                        log_result(result, name);
                        (*callback)(id, piece, result.error, maybe_hash);
                    });
            },
            .cancel = [this, id, piece, callback]()
            { dispatch([id, piece, callback]() { (*callback)(id, piece, make_error(ECANCELED), std::nullopt); }); },
        };

        enqueue(std::move(task));
    }

    void write(tr_io_plan plan, std::unique_ptr<BlockData> data, OnWrite on_write)
    {
        auto callback = std::make_shared<OnWrite>(std::move(on_write));
        auto const id = plan.tor_id;
        auto const byte_span = plan.byte_span;

        if (!byte_span.is_valid() || data == nullptr || byte_span.size() > std::size(*data))
        {
            dispatch([id, byte_span, callback]() { (*callback)(id, byte_span, make_error(EINVAL), false); });
            return;
        }

        auto write_data = std::shared_ptr<BlockData>{ std::move(data) };

        auto task = Task{
            .id = id,
            .op = Op::Write,
            .write_bytes = byte_span.size(),
            .run =
                [this, plan = std::move(plan), write_data, callback]()
            {
                auto result = backend_->write(plan, *write_data);
                dispatch(
                    [id = plan.tor_id, byte_span = plan.byte_span, name = plan.name, callback, result = std::move(result)]()
                    {
                        log_result(result, name);
                        (*callback)(id, byte_span, result.error, result.created_file);
                    });
            },
            .cancel = [this, id, byte_span, callback]()
            { dispatch([id, byte_span, callback]() { (*callback)(id, byte_span, make_error(ECANCELED), false); }); },
        };

        enqueue(std::move(task));
    }

    void close_torrent(tr_torrent_id_t const tor_id)
    {
        enqueue(Task{
            .id = tor_id,
            .op = Op::CloseTorrent,
            .run = [this, tor_id]() { backend_->close_torrent(tor_id); },
            .cancel = {},
        });
    }

    void close_file(tr_torrent_id_t const tor_id, tr_file_index_t const file_num)
    {
        enqueue(Task{
            .id = tor_id,
            .op = Op::CloseFile,
            .run = [this, tor_id, file_num]() { backend_->close_file(tor_id, file_num); },
            .cancel = {},
        });
    }

    void close_all()
    {
        auto lock = std::unique_lock(mutex_);
        // After shutdown() the workers are joined: nothing is queued or
        // active any more, and nothing is left to signal idle_cv_.
        idle_cv_.wait(lock, [this]() { return stopping_workers_ || (std::empty(queues_) && std::empty(active_ids_)); });
        backend_->close_all();
    }

    void rename(
        tr_torrent_id_t const tor_id,
        std::string_view base,
        std::string_view oldpath,
        std::string_view newname,
        tr_torrent_rename_done_func callback)
    {
        auto callback_ptr = std::make_shared<tr_torrent_rename_done_func>(std::move(callback));
        auto const oldpath_str = std::string{ oldpath };
        auto const newname_str = std::string{ newname };

        auto const notify = [this, tor_id, oldpath_str, newname_str, callback_ptr](tr_error_code_t const err)
        {
            if (*callback_ptr == nullptr)
            {
                return;
            }
            dispatch([tor_id, oldpath_str, newname_str, callback_ptr, err]()
                     { (*callback_ptr)(tor_id, oldpath_str, newname_str, make_error(err)); });
        };

        enqueue(Task{
            .id = tor_id,
            .op = Op::Rename,
            .run =
                [this, tor_id, base = std::string{ base }, oldpath_str, newname_str, notify]()
            {
                backend_->close_torrent(tor_id);
                notify(backend_->rename(tor_id, base, oldpath_str, newname_str));
            },
            .cancel = [notify]() { notify(ECANCELED); },
        });
    }

    void move(
        tr_torrent_id_t const tor_id,
        tr_torrent_files files,
        std::string_view old_parent,
        std::string_view parent,
        std::string_view parent_name,
        OnMove on_move)
    {
        auto callback = std::make_shared<OnMove>(std::move(on_move));
        auto const notify = [this, tor_id, callback](tr_error_code_t const err)
        { dispatch([tor_id, callback, err]() { (*callback)(tor_id, make_error(err)); }); };

        enqueue(Task{
            .id = tor_id,
            .op = Op::Move,
            .run =
                [this,
                 tor_id,
                 files = std::move(files),
                 old_parent = std::string{ old_parent },
                 parent = std::string{ parent },
                 parent_name = std::string{ parent_name },
                 notify]()
            {
                backend_->close_torrent(tor_id);
                notify(backend_->move(tor_id, files, old_parent, parent, parent_name));
            },
            .cancel = [notify]() { notify(ECANCELED); },
        });
    }

    void remove(
        tr_torrent_id_t const tor_id,
        tr_torrent_files files,
        std::string_view parent,
        std::string_view name,
        tr_torrent_remove_func remove_func,
        OnRemove on_remove)
    {
        auto canceled = std::vector<std::function<void()>>{};

        auto task = Task{
            .id = tor_id,
            .op = Op::Remove,
            .run =
                [this,
                 tor_id,
                 files = std::move(files),
                 parent = std::string{ parent },
                 name = std::string{ name },
                 remove_func = std::move(remove_func),
                 on_remove = std::move(on_remove)]()
            {
                backend_->close_torrent(tor_id);
                auto error = backend_->remove(tor_id, files, parent, name, remove_func);
                if (on_remove)
                {
                    dispatch([tor_id, on_remove, error = std::move(error)]() { on_remove(tor_id, error); });
                }
            },
            .cancel = {},
        };

        {
            auto const lock = std::lock_guard(mutex_);

            // The torrent's data is about to be deleted, so queued work on it is moot.
            discard_queued_unlocked(tor_id, canceled);

            auto& queue = queues_[tor_id];
            auto const was_empty = std::empty(queue);
            queue.emplace_back(std::move(task));
            ++pending_non_read_;
            if (was_empty)
            {
                runnable_ids_.push_back(tor_id);
            }
        }

        for (auto& cancel : canceled)
        {
            cancel();
        }

        work_cv_.notify_one();
    }

    void forget(tr_torrent_id_t const tor_id)
    {
        auto canceled = std::vector<std::function<void()>>{};

        {
            auto const lock = std::lock_guard(mutex_);
            discard_queued_unlocked(tor_id, canceled);
            if (auto const it = queues_.find(tor_id); it != std::end(queues_) && std::empty(it->second))
            {
                queues_.erase(it);
            }

            if (std::empty(queues_) && std::empty(active_ids_))
            {
                idle_cv_.notify_all();
            }
        }

        for (auto& cancel : canceled)
        {
            cancel();
        }
    }

    void shutdown(std::chrono::steady_clock::time_point const deadline)
    {
        auto canceled = std::vector<std::function<void()>>{};

        {
            auto lock = std::unique_lock(mutex_);
            if (stopping_workers_)
            {
                return;
            }

            shutting_down_ = true;

            for (auto queue_it = std::begin(queues_); queue_it != std::end(queues_);)
            {
                auto& queue = queue_it->second;
                auto it = std::begin(queue);
                while (it != std::end(queue))
                {
                    if (!is_cancelled_at_shutdown(it->op))
                    {
                        ++it;
                        continue;
                    }

                    discard_unlocked(*it, canceled);
                    it = queue.erase(it);
                }

                // An emptied queue must not linger: close_all() waits for
                // `queues_` to be empty, and no worker will erase it later.
                queue_it = std::empty(queue) ? queues_.erase(queue_it) : std::next(queue_it);
            }

            auto const drained = [this]()
            {
                return pending_non_read_ == 0U && active_non_read_ == 0U;
            };
            if (deadline == std::chrono::steady_clock::time_point::max())
            {
                drained_cv_.wait(lock, drained);
            }
            else if (!drained_cv_.wait_until(lock, deadline, drained))
            {
                // Out of time. Nothing queued may start now; the tasks
                // already in the backend finish when the workers are joined.
                for (auto& [id, queue] : queues_)
                {
                    for (auto& task : queue)
                    {
                        discard_unlocked(task, canceled);
                    }
                }
                queues_.clear();
                runnable_ids_.clear();
            }

            stopping_workers_ = true;
        }

        for (auto& cancel : canceled)
        {
            cancel();
        }

        work_cv_.notify_all();

        for (auto& worker : workers_)
        {
            worker.join();
        }
        workers_.clear();
    }

    [[nodiscard]] uint64_t enqueued_write_bytes() const
    {
        auto const lock = std::lock_guard(mutex_);
        return enqueued_write_bytes_;
    }

    [[nodiscard]] uint64_t enqueued_write_bytes(tr_torrent_id_t const id) const
    {
        auto const lock = std::lock_guard(mutex_);
        auto const it = enqueued_write_bytes_by_id_.find(id);
        return it != std::end(enqueued_write_bytes_by_id_) ? it->second : 0U;
    }

    [[nodiscard]] bool is_active(tr_torrent_id_t const id) const
    {
        auto const lock = std::lock_guard(mutex_);
        return active_ids_.contains(id);
    }

private:
    void release_write_bytes_unlocked(tr_torrent_id_t const id, uint64_t const n_bytes)
    {
        enqueued_write_bytes_ -= n_bytes;
        if (auto it = enqueued_write_bytes_by_id_.find(id); it != std::end(enqueued_write_bytes_by_id_))
        {
            it->second -= n_bytes;
            if (it->second == 0U)
            {
                enqueued_write_bytes_by_id_.erase(it);
            }
        }
    }

    void dispatch(std::function<void()> func)
    {
        dispatcher_(std::move(func));
    }

    // Takes a task out of the accounting and collects its cancel callback.
    // The caller erases the task and runs the callbacks after unlocking.
    void discard_unlocked(Task& task, std::vector<std::function<void()>>& canceled)
    {
        if (task.op == Op::Write)
        {
            release_write_bytes_unlocked(task.id, task.write_bytes);
        }

        if (!is_cancelled_at_shutdown(task.op))
        {
            --pending_non_read_;
            if (shutting_down_ && pending_non_read_ == 0U && active_non_read_ == 0U)
            {
                drained_cv_.notify_all();
            }
        }

        if (task.cancel)
        {
            canceled.emplace_back(std::move(task.cancel));
        }
    }

    // Discards a torrent's queued tasks except closes and removes, which
    // report to no one and must still run.
    void discard_queued_unlocked(tr_torrent_id_t const tor_id, std::vector<std::function<void()>>& canceled)
    {
        auto const queue_it = queues_.find(tor_id);
        if (queue_it == std::end(queues_))
        {
            return;
        }

        auto& queue = queue_it->second;
        auto it = std::begin(queue);
        while (it != std::end(queue))
        {
            if (it->op == Op::CloseFile || it->op == Op::CloseTorrent || it->op == Op::Remove)
            {
                ++it;
                continue;
            }

            discard_unlocked(*it, canceled);
            it = queue.erase(it);
        }
    }

    void enqueue(Task task)
    {
        auto cancel = std::function<void()>{};

        {
            auto const lock = std::lock_guard(mutex_);

            if (shutting_down_ && is_cancelled_at_shutdown(task.op))
            {
                cancel = std::move(task.cancel);
            }
            else
            {
                if (task.op == Op::Write)
                {
                    enqueued_write_bytes_ += task.write_bytes;
                    enqueued_write_bytes_by_id_[task.id] += task.write_bytes;
                }

                if (!is_cancelled_at_shutdown(task.op))
                {
                    ++pending_non_read_;
                }

                auto& queue = queues_[task.id];
                auto const id = task.id;
                queue.emplace_back(std::move(task));
                if (std::size(queue) == 1U)
                {
                    runnable_ids_.push_back(id);
                }
            }
        }

        if (cancel)
        {
            cancel();
            return;
        }

        work_cv_.notify_one();
    }

    [[nodiscard]] bool has_runnable_task_unlocked() const
    {
        return std::ranges::any_of(
            runnable_ids_,
            [this](tr_torrent_id_t const id)
            { return !active_ids_.contains(id) && queues_.contains(id) && !std::empty(queues_.at(id)); });
    }

    [[nodiscard]] bool dequeue_next_task_unlocked(Task& setme)
    {
        while (!std::empty(runnable_ids_))
        {
            auto const id = runnable_ids_.front();
            runnable_ids_.pop_front();

            if (active_ids_.contains(id))
            {
                continue;
            }

            auto it = queues_.find(id);
            if (it == std::end(queues_) || std::empty(it->second))
            {
                continue;
            }

            setme = std::move(it->second.front());
            it->second.pop_front();
            active_ids_.insert(id);

            if (!is_cancelled_at_shutdown(setme.op))
            {
                --pending_non_read_;
                ++active_non_read_;
            }

            if (std::empty(it->second))
            {
                queues_.erase(it);
            }

            return true;
        }

        return false;
    }

    void worker_thread()
    {
        while (true)
        {
            auto task = Task{};

            {
                auto lock = std::unique_lock(mutex_);
                work_cv_.wait(lock, [this]() { return stopping_workers_ || has_runnable_task_unlocked(); });

                if (stopping_workers_)
                {
                    return;
                }

                if (!dequeue_next_task_unlocked(task))
                {
                    continue;
                }
            }

            if (task.run)
            {
                task.run();
            }

            {
                auto const lock = std::lock_guard(mutex_);
                active_ids_.erase(task.id);

                if (task.op == Op::Write)
                {
                    release_write_bytes_unlocked(task.id, task.write_bytes);
                }

                if (!is_cancelled_at_shutdown(task.op))
                {
                    --active_non_read_;
                    if (shutting_down_ && pending_non_read_ == 0U && active_non_read_ == 0U)
                    {
                        drained_cv_.notify_all();
                    }
                }

                if (auto it = queues_.find(task.id); it != std::end(queues_) && !std::empty(it->second))
                {
                    runnable_ids_.push_back(task.id);
                }

                if (std::empty(queues_) && std::empty(active_ids_))
                {
                    idle_cv_.notify_all();
                }
            }

            work_cv_.notify_one();
        }
    }

    // Peer reads are pointless once the session is closing, so shutdown
    // cancels them. Everything else is drained: writes and moves so the
    // data lands, piece checks so a piece whose last block arrived just
    // before close is verified rather than dropped from the resume file.
    [[nodiscard]] static bool is_cancelled_at_shutdown(Op const op)
    {
        return op == Op::Read;
    }

    std::unique_ptr<Backend> backend_;
    Dispatcher dispatcher_;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable drained_cv_;
    std::condition_variable idle_cv_;

    std::unordered_map<tr_torrent_id_t, std::deque<Task>> queues_;
    std::deque<tr_torrent_id_t> runnable_ids_;
    std::unordered_set<tr_torrent_id_t> active_ids_;
    std::vector<std::thread> workers_;

    uint64_t enqueued_write_bytes_ = 0;
    std::unordered_map<tr_torrent_id_t, uint64_t> enqueued_write_bytes_by_id_;
    size_t pending_non_read_ = 0;
    size_t active_non_read_ = 0;

    bool shutting_down_ = false;
    bool stopping_workers_ = false;
};

// ---

LocalData::LocalData(tr_open_files& open_files, Dispatcher dispatcher, size_t worker_count)
    : impl_{ std::make_unique<Impl>(std::make_unique<DefaultBackend>(open_files), std::move(dispatcher), worker_count) }
{
}

LocalData::LocalData(std::unique_ptr<Backend> backend, Dispatcher dispatcher, size_t worker_count)
    : impl_{ std::make_unique<Impl>(std::move(backend), std::move(dispatcher), worker_count) }
{
}

LocalData::~LocalData() = default;

void LocalData::read(tr_io_plan plan, OnRead on_read)
{
    impl_->read(std::move(plan), std::move(on_read));
}

void LocalData::test_piece(tr_io_plan plan, tr_piece_index_t const piece, OnTest on_test)
{
    impl_->test_piece(std::move(plan), piece, std::move(on_test));
}

void LocalData::write(tr_io_plan plan, std::unique_ptr<BlockData> data, OnWrite on_write)
{
    impl_->write(std::move(plan), std::move(data), std::move(on_write));
}

void LocalData::close_torrent(tr_torrent_id_t const tor_id)
{
    impl_->close_torrent(tor_id);
}

void LocalData::close_file(tr_torrent_id_t const tor_id, tr_file_index_t const file_num)
{
    impl_->close_file(tor_id, file_num);
}

void LocalData::close_all()
{
    impl_->close_all();
}

void LocalData::move(
    tr_torrent_id_t const id,
    tr_torrent_files files,
    std::string_view const old_parent,
    std::string_view const parent,
    std::string_view const parent_name,
    OnMove on_move)
{
    impl_->move(id, std::move(files), old_parent, parent, parent_name, std::move(on_move));
}

void LocalData::remove(
    tr_torrent_id_t const id,
    tr_torrent_files files,
    std::string_view const parent,
    std::string_view const name,
    tr_torrent_remove_func remove_func,
    OnRemove on_remove)
{
    impl_->remove(id, std::move(files), parent, name, std::move(remove_func), std::move(on_remove));
}

void LocalData::rename(
    tr_torrent_id_t const id,
    std::string_view const base,
    std::string_view const oldpath,
    std::string_view const newname,
    tr_torrent_rename_done_func callback)
{
    impl_->rename(id, base, oldpath, newname, std::move(callback));
}

void LocalData::forget(tr_torrent_id_t const id)
{
    impl_->forget(id);
}

void LocalData::shutdown(std::chrono::steady_clock::time_point const deadline)
{
    impl_->shutdown(deadline);
}

uint64_t LocalData::enqueued_write_bytes() const
{
    return impl_->enqueued_write_bytes();
}

uint64_t LocalData::enqueued_write_bytes(tr_torrent_id_t const id) const
{
    return impl_->enqueued_write_bytes(id);
}

bool LocalData::is_active(tr_torrent_id_t const id) const
{
    return impl_->is_active(id);
}

} // namespace tr
