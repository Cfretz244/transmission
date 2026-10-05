// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility> // std::move

#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/types.h"

class tr_verify_worker
{
public:
    class Mediator
    {
    public:
        virtual ~Mediator() = default;

        [[nodiscard]] virtual tr_torrent_metainfo const& metainfo() const = 0;
        [[nodiscard]] virtual std::optional<std::string> find_file(tr_file_index_t file_index) const = 0;

        // A torrent added over what looks like a complete copy of its files
        // (every file present, right size, not partial, written before the
        // torrent was added) is taken for a seed once its first piece
        // hashes: the worker reports it with on_new_seed_found() instead of
        // walking the whole copy. Both are called on the worker's thread.
        [[nodiscard]] virtual bool sniff_new_seed() const = 0;
        [[nodiscard]] virtual bool is_complete_copy_on_disk() const = 0;

        virtual void on_verify_queued() = 0;
        virtual void on_verify_started() = 0;
        virtual void on_piece_checked(tr_piece_index_t piece, bool has_piece) = 0;

        // Exactly one of these ends a verify that was started.
        virtual void on_new_seed_found() = 0;
        virtual void on_verify_done(bool aborted, bool found_any_file) = 0;
    };

    tr_verify_worker() = default;
    ~tr_verify_worker();

    tr_verify_worker(tr_verify_worker const&) = delete;
    tr_verify_worker(tr_verify_worker&&) = delete;
    tr_verify_worker& operator=(tr_verify_worker const&) = delete;
    tr_verify_worker& operator=(tr_verify_worker&&) = delete;

    void add(std::unique_ptr<Mediator> mediator, tr_priority_t priority);

    void remove(tr_sha1_digest_t const& info_hash);

    void set_sleep_per_seconds_during_verify(std::chrono::milliseconds sleep_per_seconds_during_verify);

    [[nodiscard]] constexpr auto sleep_per_seconds_during_verify() const noexcept
    {
        return sleep_per_seconds_during_verify_;
    }

private:
    struct Node
    {
        Node(std::unique_ptr<Mediator> mediator, tr_priority_t priority) noexcept
            : mediator_{ std::move(mediator) }
            , priority_{ priority }
        {
        }

        [[nodiscard]] int compare(Node const& that) const noexcept; // <=>

        [[nodiscard]] auto operator<(Node const& that) const noexcept
        {
            return compare(that) < 0;
        }

        [[nodiscard]] bool matches(tr_sha1_digest_t const& info_hash) const noexcept
        {
            return mediator_->metainfo().info_hash() == info_hash;
        }

        std::unique_ptr<Mediator> mediator_;
        tr_priority_t priority_;
    };

    static void verify_torrent(
        Mediator& verify_mediator,
        std::atomic<bool> const& abort_flag,
        std::chrono::milliseconds sleep_per_seconds_during_verify);

    void verify_thread_func();

    std::mutex verify_mutex_;

    std::set<Node> todo_;
    std::optional<Node> current_node_;

    std::optional<std::thread::id> verify_thread_id_;

    std::atomic<bool> stop_current_ = false;

    std::chrono::milliseconds sleep_per_seconds_during_verify_ = {};
};
