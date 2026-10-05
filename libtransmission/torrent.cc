// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> // EINVAL
#include <chrono>
#include <cstddef> // size_t
#include <ctime>
#include <map>
#include <sstream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/announcer.h"
#include "libtransmission/bandwidth.h"
#include "libtransmission/completion.h"
#include "libtransmission/crypto-utils.h" // for tr_sha1()
#include "libtransmission/error.h"
#include "libtransmission/file-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h" // tr_ioTestPiece()
#include "libtransmission/log.h"
#include "libtransmission/magnet-metainfo.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/resume.h"
#include "libtransmission/session.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/types.h"
#include "libtransmission/utils.h"
#include "libtransmission/version.h"
#include "libtransmission/web-utils.h"

struct tr_ctor;

using namespace std::literals;
using namespace tr::Values;

#define tr_return_if_fail(expr) \
    do \
    { \
        if (expr) [[likely]] \
        { \
        } \
        else \
        { \
            tr_logAddWarn(#expr); \
            return; \
        } \
    } while (0)
#define tr_return_val_if_fail(expr, val) \
    do \
    { \
        if (expr) [[likely]] \
        { \
        } \
        else \
        { \
            tr_logAddWarn(#expr); \
            return val; \
        } \
    } while (0)

// ---

void tr_torrent::Error::set_tracker_warning(tr_interned_string announce_url, std::string_view errmsg)
{
    announce_url_ = announce_url;
    errmsg_.assign(errmsg);
    error_type_ = tr_stat::Error::TrackerWarning;
}

void tr_torrent::Error::set_tracker_error(tr_interned_string announce_url, std::string_view errmsg)
{
    announce_url_ = announce_url;
    errmsg_.assign(errmsg);
    error_type_ = tr_stat::Error::TrackerError;
}

void tr_torrent::Error::set_local_error(std::string_view errmsg)
{
    announce_url_.clear();
    errmsg_.assign(errmsg);
    error_type_ = tr_stat::Error::LocalError;
}

void tr_torrent::Error::clear() noexcept
{
    announce_url_.clear();
    errmsg_.clear();
    error_type_ = tr_stat::Error::Ok;
}

void tr_torrent::Error::clear_if_tracker() noexcept
{
    if (is_tracker())
    {
        clear();
    }
}

// ---

std::string tr_torrentName(tr_torrent const* tor)
{
    return tor != nullptr ? tor->name() : ""s;
}

tr_torrent_id_t tr_torrentId(tr_torrent const* tor)
{
    return tor != nullptr ? tor->id() : -1;
}

tr_torrent* tr_torrentFindFromId(tr_session* session, tr_torrent_id_t id)
{
    return session != nullptr ? session->torrents().get(id) : nullptr;
}

tr_torrent* tr_torrentFindFromMetainfo(tr_session* session, tr_torrent_metainfo const* metainfo)
{
    if (session == nullptr || metainfo == nullptr)
    {
        return nullptr;
    }

    return session->torrents().get(metainfo->info_hash());
}

tr_torrent* tr_torrentFindFromMagnetLink(tr_session* session, std::string_view const magnet_link)
{
    return session->torrents().get(magnet_link);
}

bool tr_torrentSetMetainfoFromFile(tr_torrent* tor, tr_torrent_metainfo const* metainfo, char const* filename)
{
    if (tr_torrentHasMetadata(tor))
    {
        return false;
    }

    auto error = tr_error{};
    tor->use_metainfo_from_file(metainfo, filename, &error);
    if (error)
    {
        tor->error().set_local_error(
            fmt::format(
                fmt::runtime(_("Couldn't use metainfo from '{path}' for '{magnet}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("magnet", tor->magnet()),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        return false;
    }

    return true;
}

// ---

namespace
{
bool did_files_disappear(tr_torrent* tor, std::optional<bool> has_any_local_data = {})
{
    auto const has = has_any_local_data ? *has_any_local_data : tor->has_any_local_data();
    return tor->has_total() > 0 && !has;
}

bool set_local_error_if_files_disappeared(tr_torrent* tor, std::optional<bool> has_any_local_data = {})
{
    auto const files_disappeared = did_files_disappear(tor, has_any_local_data);

    if (files_disappeared)
    {
        tr_logAddTraceTor(tor, "[LAZY] uh oh, the files disappeared");
        tor->error().set_local_error(
            _("No data found! Ensure your drives are connected or use \"Set Location\". "
              "To re-download, use \"Verify Local Data\" and start the torrent afterwards."));
    }

    return files_disappeared;
}

// True if a removed torrent's files are still being deleted where this
// torrent would look for its own. See tr_session::deleting_paths_.
bool is_path_being_deleted(tr_torrent const* tor)
{
    auto const& paths = tor->session->deleting_paths_;
    auto const matches = [&paths, name = tor->name()](std::string_view const dir)
    {
        return !std::empty(dir) && std::ranges::find(paths, tr_pathbuf{ dir, '/', name }.sv()) != std::end(paths);
    };
    return matches(tor->download_dir().sv()) || matches(tor->incomplete_dir().sv());
}

// The delete of the files at `path` has finished: let any torrent added
// onto that path in the meantime take its first look at its files.
void on_files_deleted(tr_session* session, std::string const& path)
{
    auto const lock = session->unique_lock();

    auto& paths = session->deleting_paths_;
    if (auto const it = std::ranges::find(paths, path); it != std::end(paths))
    {
        paths.erase(it);
    }

    if (session->isClosing())
    {
        return;
    }

    for (auto* const tor : session->torrents())
    {
        tor->finish_deferred_init_if_clear();
    }
}

/* returns true if the seed ratio applies --
 * it applies if the torrent's a seed AND it has a seed ratio set */
bool tr_torrentGetSeedRatioBytes(tr_torrent const* tor, uint64_t* setme_left, uint64_t* setme_goal)
{
    bool seed_ratio_applies = false;

    TR_ASSERT(tr_isTorrent(tor));

    if (auto const seed_ratio = tor->effective_seed_ratio(); seed_ratio)
    {
        auto const uploaded = tor->bytes_uploaded_.ever();
        auto const baseline = tor->size_when_done();
        auto const goal = static_cast<uint64_t>(static_cast<double>(baseline) * *seed_ratio);

        if (setme_left != nullptr)
        {
            *setme_left = goal > uploaded ? goal - uploaded : 0;
        }

        if (setme_goal != nullptr)
        {
            *setme_goal = goal;
        }

        seed_ratio_applies = tor->is_done();
    }

    return seed_ratio_applies;
}

bool tr_torrentIsSeedRatioDone(tr_torrent const* tor)
{
    auto bytes_left = uint64_t{};
    return tr_torrentGetSeedRatioBytes(tor, &bytes_left, nullptr) && bytes_left == 0;
}
} // namespace

// --- PER-TORRENT UL / DL SPEEDS

void tr_torrentSetSpeedLimit_KBps(tr_torrent* const tor, tr_direction const dir, size_t const limit_kbyps)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
}

size_t tr_torrentGetSpeedLimit_KBps(tr_torrent const* const tor, tr_direction const dir)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return static_cast<size_t>(tor->speed_limit(dir).count(Speed::Units::KByps));
}

void tr_torrentUseSpeedLimit(tr_torrent* const tor, tr_direction const dir, bool const enabled)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->use_speed_limit(dir, enabled);
}

bool tr_torrentUsesSpeedLimit(tr_torrent const* const tor, tr_direction const dir)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->uses_speed_limit(dir);
}

void tr_torrentUseSessionLimits(tr_torrent* const tor, bool const enabled)
{
    tr_return_if_fail(tr_isTorrent(tor));

    if (tor->bandwidth().honor_parent_limits(tr_direction::Up, enabled) ||
        tor->bandwidth().honor_parent_limits(tr_direction::Down, enabled))
    {
        tor->set_dirty();
    }
}

bool tr_torrentUsesSessionLimits(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->uses_session_limits();
}

// --- Download Ratio

void tr_torrentSetRatioMode(tr_torrent* const tor, tr_ratiolimit mode)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_seed_ratio_mode(mode);
}

tr_ratiolimit tr_torrentGetRatioMode(tr_torrent const* const tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->seed_ratio_mode();
}

void tr_torrentSetRatioLimit(tr_torrent* const tor, double desired_ratio)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_seed_ratio(desired_ratio);
}

double tr_torrentGetRatioLimit(tr_torrent const* const tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->seed_ratio();
}

bool tr_torrentGetSeedRatio(tr_torrent const* const tor, double* ratio)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    auto const val = tor->effective_seed_ratio();

    if (ratio != nullptr && val)
    {
        *ratio = *val;
    }

    return val.has_value();
}

// ---

void tr_torrentSetIdleMode(tr_torrent* const tor, tr_idlelimit mode)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_idle_limit_mode(mode);
}

tr_idlelimit tr_torrentGetIdleMode(tr_torrent const* const tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->idle_limit_mode();
}

void tr_torrentSetIdleLimit(tr_torrent* const tor, uint16_t idle_minutes)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_idle_limit_minutes(idle_minutes);
}

uint16_t tr_torrentGetIdleLimit(tr_torrent const* const tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->idle_limit_minutes();
}

namespace
{
namespace script_helpers
{
[[nodiscard]] std::string build_labels_string(tr_torrent::labels_t const& labels)
{
    auto buf = std::stringstream{};

    for (auto it = std::begin(labels), end = std::end(labels); it != end;)
    {
        buf << it->sv();

        if (++it != end)
        {
            buf << ',';
        }
    }

    return buf.str();
}

[[nodiscard]] std::string buildTrackersString(tr_torrent const* tor)
{
    auto buf = std::stringstream{};

    for (size_t i = 0, n = tr_torrentTrackerCount(tor); i < n; ++i)
    {
        buf << tr_torrentTracker(tor, i).host_and_port;

        if (i < n)
        {
            buf << ',';
        }
    }

    return buf.str();
}

void torrentCallScript(tr_torrent const* tor, std::string const& script)
{
    if (std::empty(script))
    {
        return;
    }

    auto const now = tr_time();

    auto torrent_dir = tr_pathbuf{ tor->current_dir() };
    tr_sys_path_native_separators(std::data(torrent_dir));

    auto const cmd = std::array<char const*, 2>{ script.c_str(), nullptr };

    auto const id_str = std::to_string(tr_torrentId(tor));
    auto const labels_str = build_labels_string(tor->labels());
    auto const trackers_str = buildTrackersString(tor);
    auto const bytes_downloaded_str = std::to_string(tor->bytes_downloaded_.ever());
    auto const localtime_str = fmt::format("{:%a %b %d %T %Y%n}", *std::localtime(&now));
    auto const priority_str = std::to_string(tor->get_priority());

    auto const env = std::map<std::string_view, std::string_view>{
        { "TR_APP_VERSION"sv, SHORT_VERSION_STRING },
        { "TR_TIME_LOCALTIME"sv, localtime_str },
        { "TR_TORRENT_BYTES_DOWNLOADED"sv, bytes_downloaded_str },
        { "TR_TORRENT_DIR"sv, torrent_dir.c_str() },
        { "TR_TORRENT_HASH"sv, tor->info_hash_string() },
        { "TR_TORRENT_ID"sv, id_str },
        { "TR_TORRENT_LABELS"sv, labels_str },
        { "TR_TORRENT_NAME"sv, tor->name() },
        { "TR_TORRENT_PRIORITY"sv, priority_str },
        { "TR_TORRENT_TRACKERS"sv, trackers_str },
    };

    tr_logAddInfoTor(tor, fmt::format(fmt::runtime(_("Calling script '{path}'")), fmt::arg("path", script)));

    auto error = tr_error{};
    if (!tr_spawn_async(std::data(cmd), env, TR_IF_WIN32("\\", "/"), &error))
    {
        tr_logAddWarnTor(
            tor,
            fmt::format(
                fmt::runtime(_("Couldn't call script '{path}': {error} ({error_code})")),
                fmt::arg("path", script),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
    }
}
} // namespace script_helpers

void callScriptIfEnabled(tr_torrent const* tor, TrScript type)
{
    using namespace script_helpers;

    auto const* session = tor->session;

    if (tr_sessionIsScriptEnabled(session, type))
    {
        torrentCallScript(tor, session->script(type));
    }
}

} // namespace

// ---

void tr_torrent::stop_if_seed_limit_reached()
{
    if (!is_running() || is_stopping_ || !is_done())
    {
        return;
    }

    /* if we're seeding and reach our seed ratio limit, stop the torrent */
    if (tr_torrentIsSeedRatioDone(this))
    {
        tr_logAddInfoTor(this, _("Seed ratio reached; pausing torrent"));
        stop_soon();
        session->onRatioLimitHit(id());
    }
    /* if we're seeding and reach our inactivity limit, stop the torrent */
    else if (auto const secs_left = idle_seconds_left(tr_time()); secs_left && *secs_left <= time_t{})
    {
        tr_logAddInfoTor(this, _("Seeding idle limit reached; pausing torrent"));

        stop_soon();
        finished_seeding_by_idle_ = true;
        session->onIdleLimitHit(id());
    }

    if (is_stopping_)
    {
        callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_DONE_SEEDING);
    }
}

// --- Queue

void tr_torrentSetQueuePosition(tr_torrent* tor, size_t queue_position)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_queue_position(queue_position);
}

void tr_torrent::queue_move_top(std::span<tr_torrent* const> const torrents_in)
{
    tr_return_if_fail(std::ranges::all_of(torrents_in, tr_isTorrent));

    auto torrents = std::vector<tr_torrent*>(std::begin(torrents_in), std::end(torrents_in));
    std::ranges::sort(std::views::reverse(torrents), tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        tor->set_queue_position(tr_torrent_queue::MinQueuePosition);
    }
}

void tr_torrent::queue_move_up(std::span<tr_torrent* const> const torrents_in)
{
    tr_return_if_fail(std::ranges::all_of(torrents_in, tr_isTorrent));

    auto torrents = std::vector<tr_torrent*>(std::begin(torrents_in), std::end(torrents_in));
    std::ranges::sort(torrents, tr_torrent::CompareQueuePosition);
    for (auto last_consecutive_pos = tr_torrent_queue::MinQueuePosition; auto* const tor : torrents)
    {
        if (auto const pos = tor->queue_position(); pos != last_consecutive_pos)
        {
            tor->set_queue_position(pos - 1U);
        }
        else
        {
            ++last_consecutive_pos;
        }
    }
}

void tr_torrent::queue_move_down(std::span<tr_torrent* const> const torrents_in)
{
    tr_return_if_fail(!torrents_in.empty() && std::ranges::all_of(torrents_in, tr_isTorrent));

    auto torrents = std::vector<tr_torrent*>(std::begin(torrents_in), std::end(torrents_in));
    std::ranges::sort(std::views::reverse(torrents), tr_torrent::CompareQueuePosition);
    for (auto last_consecutive_pos =
             torrents.front()->session->torrent_queue().size() - 1U + tr_torrent_queue::MinQueuePosition;
         auto* const tor : torrents)
    {
        if (auto const pos = tor->queue_position(); pos != last_consecutive_pos)
        {
            tor->set_queue_position(pos + 1U);
        }
        else
        {
            --last_consecutive_pos;
        }
    }
}

void tr_torrent::queue_move_bottom(std::span<tr_torrent* const> const torrents_in)
{
    tr_return_if_fail(std::ranges::all_of(torrents_in, tr_isTorrent));

    auto torrents = std::vector<tr_torrent*>(std::begin(torrents_in), std::end(torrents_in));
    std::ranges::sort(torrents, tr_torrent::CompareQueuePosition);
    for (auto* const tor : torrents)
    {
        tor->set_queue_position(tr_torrent_queue::MaxQueuePosition);
    }
}

// --- Start, Stop

namespace
{
namespace start_stop_helpers
{
bool torrentShouldQueue(tr_torrent const* const tor)
{
    tr_direction const dir = tor->queue_direction();

    return tor->session->count_queue_free_slots(dir) == 0;
}

void freeTorrent(tr_torrent* tor)
{
    auto const lock = tor->unique_lock();

    TR_ASSERT(!tor->is_running());

    tr_session* session = tor->session;

    tor->doomed_(tor);

    session->announcer_->removeTorrent(tor);

    session->torrents().remove(tor, tr_time());

    if (!session->isClosing())
    {
        session->torrent_queue().remove(tor->id());
    }

    delete tor;
}
} // namespace start_stop_helpers
} // namespace

// has_any_local_data is true or false if we know whether or not local data exists,
// or unset if we don't know and need to check for ourselves
void tr_torrent::start(bool bypass_queue, std::optional<bool> has_any_local_data)
{
    using namespace start_stop_helpers;

    auto const lock = unique_lock();

    if (deferred_init_)
    {
        return; // finish_init() starts it if start_when_stable_
    }

    switch (activity())
    {
    case TR_STATUS_SEED:
    case TR_STATUS_DOWNLOAD:
        return; /* already started */

    case TR_STATUS_SEED_WAIT:
    case TR_STATUS_DOWNLOAD_WAIT:
        if (!bypass_queue)
        {
            return; /* already queued */
        }

        break;

    case TR_STATUS_CHECK:
    case TR_STATUS_CHECK_WAIT:
        /* verifying right now... wait until that's done so
         * we'll know what completeness to use/announce */
        return;

    case TR_STATUS_STOPPED:
        if (!bypass_queue && torrentShouldQueue(this))
        {
            set_is_queued();
            return;
        }

        break;
    }

    /* don't allow the torrent to be started if the files disappeared */
    if (set_local_error_if_files_disappeared(this, has_any_local_data))
    {
        return;
    }

    /* allow finished torrents to be resumed */
    if (tr_torrentIsSeedRatioDone(this))
    {
        tr_logAddInfoTor(this, _("Restarted manually -- disabling its seed ratio"));
        set_seed_ratio_mode(TR_RATIOLIMIT_UNLIMITED);
    }

    is_running_ = true;
    set_dirty();
    session->run_in_session_thread([this]() { start_in_session_thread(); });
}

void tr_torrent::start_in_session_thread()
{
    using namespace start_stop_helpers;

    TR_ASSERT(session->am_in_session_thread());
    auto const lock = unique_lock();

    // We are after `torrentStart` and before announcing to trackers/peers,
    // so now is the best time to create wanted empty files.
    create_empty_files();

    recheck_completeness();
    set_is_queued(false);

    time_t const now = tr_time();

    is_running_ = true;
    date_started_ = now;
    mark_changed();
    error().clear();
    finished_seeding_by_idle_ = false;

    bytes_uploaded_.start_new_session();
    bytes_downloaded_.start_new_session();
    bytes_corrupt_.start_new_session();
    set_dirty();

    session->announcer_->startTorrent(this);
    if (announce_completed_on_start_)
    {
        announce_completed_on_start_ = false;
        tr_announcerTorrentCompleted(this);
    }
    lpdAnnounceAt = now;
    started_(this);
}

void tr_torrent::stop_now()
{
    TR_ASSERT(session->am_in_session_thread());
    auto const lock = unique_lock();

    auto const now = tr_time();
    seconds_downloading_before_current_start_ = seconds_downloading(now);
    seconds_seeding_before_current_start_ = seconds_seeding(now);

    is_running_ = false;
    is_stopping_ = false;
    mark_changed();

    if (!session->isClosing())
    {
        tr_logAddInfoTor(this, _("Pausing torrent"));
    }

    session->verify_remove(this);

    stopped_(this);
    session->announcer_->stopTorrent(this);

    session->local_data.close_torrent(id());

    if (!is_deleting_)
    {
        save_resume_file();
    }

    set_is_queued(false);
}

// By-value: arguments are moved into the session-thread work item.
void tr_torrentRemoveInSessionThread(
    tr_torrent* tor,
    bool const delete_flag,
    tr_torrent_remove_func remove_func) // NOLINT(performance-unnecessary-value-param)
{
    auto const lock = tor->unique_lock();

    // A deferred torrent owns nothing on disk: the files under its path
    // belong to a removed torrent and are already being deleted.
    if (delete_flag && tor->has_metainfo() && !tor->deferred_init_)
    {
        tor->session->verify_remove(tor);

        // A torrent added onto this path before the delete finishes
        // waits for it; see is_path_being_deleted().
        auto path = std::string{ tr_pathbuf{ tor->current_dir(), '/', tor->name() } };
        tor->session->deleting_paths_.emplace_back(path);

        // LocalData deletes the files after the torrent's in-flight write,
        // so that write cannot recreate a file once it is deleted.
        tor->session->local_data.remove(
            tor->id(),
            tor->files(),
            tor->current_dir().sv(),
            tor->name(),
            std::move(remove_func),
            [session = tor->session,
             path = std::move(path),
             name = std::string{ tor->name() }](tr_torrent_id_t /*tor_id*/, tr_error const& error)
            {
                if (error)
                {
                    tr_logAddWarn(
                        fmt::format(
                            fmt::runtime(_("Couldn't remove all torrent files: {error} ({error_code})")),
                            fmt::arg("error", error.message()),
                            fmt::arg("error_code", error.code())),
                        name);
                }

                on_files_deleted(session, path);
            });
    }

    tr_torrentFreeInSessionThread(tor);
}

void tr_torrentStop(tr_torrent* tor)
{
    if (!tr_isTorrent(tor))
    {
        return;
    }

    auto const lock = tor->unique_lock();

    tor->start_when_stable_ = false;
    tor->set_dirty();
    tor->session->run_in_session_thread([tor]() { tor->stop_now(); });
}

void tr_torrentRemove(tr_torrent* tor, bool delete_flag, tr_torrent_remove_func remove_func)
{
    using namespace start_stop_helpers;

    tr_return_if_fail(tr_isTorrent(tor));

    tor->is_deleting_ = true;

    tor->session->run_in_session_thread(tr_torrentRemoveInSessionThread, tor, delete_flag, std::move(remove_func));
}

void tr_torrentFreeInSessionThread(tr_torrent* tor)
{
    using namespace start_stop_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->session->am_in_session_thread());

    if (!tor->session->isClosing())
    {
        tr_logAddInfoTor(tor, _("Removing torrent"));
    }

    tor->set_dirty(!tor->is_deleting_);
    tor->stop_now();

    if (tor->is_deleting_)
    {
        // These files are written on the state-writer thread; delete them
        // there too, so each delete runs after any save still queued.
        auto* const session = tor->session;
        for (auto const& [dir, suffix] : { std::pair{ session->torrentDir(), ".torrent"sv },
                                           std::pair{ session->torrentDir(), ".magnet"sv },
                                           std::pair{ session->resumeDir(), ".resume"sv } })
        {
            for (auto& filename : tr_torrent_metainfo::removable_files(dir, tor->name(), tor->info_hash_string(), suffix))
            {
                session->state_writer.remove(std::move(filename));
            }
        }
    }

    tor->session->local_data.forget(tor->id());
    freeTorrent(tor);
}

// ---

void tr_torrent::request_piece_check(tr_piece_index_t const piece)
{
    TR_ASSERT(session->am_in_session_thread());
    TR_ASSERT(piece < piece_count());

    if (is_piece_checked(piece))
    {
        piece_checked_(this, piece, true);
        return;
    }

    if (!requested_piece_checks_.insert(piece).second)
    {
        return; // a check for this piece is already queued; share its answer
    }

    auto const on_tested = [session = this->session](
                               tr_torrent_id_t tor_id,
                               tr_piece_index_t piece,
                               tr_error const& error,
                               std::optional<tr_sha1_digest_t> hash)
    {
        session->run_in_session_thread(
            [session, tor_id, piece, error, hash = std::move(hash)]()
            {
                auto* const tor = session->torrents().get(tor_id);
                if (tor == nullptr)
                {
                    return;
                }

                tor->requested_piece_checks_.erase(piece);

                if (error.code() == ECANCELED)
                {
                    // The torrent is going away; the piece is unchecked, not broken.
                    tor->piece_checked_(tor, piece, false);
                    return;
                }

                if (error)
                {
                    tor->error().set_local_error(
                        fmt::format(
                            fmt::runtime(_("Couldn't verify piece #{piece}: {error} ({error_code})")),
                            fmt::arg("piece", piece),
                            fmt::arg("error", error.message()),
                            fmt::arg("error_code", error.code())));
                    tor->piece_checked_(tor, piece, false);
                    return;
                }

                auto const passed = hash == tor->piece_hash(piece);
                tr_logAddTraceTor(tor, fmt::format("[LAZY] tested piece {}, pass=={}", piece, passed));
                tor->set_piece_is_checked(piece, passed);
                if (!passed)
                {
                    tor->error().set_local_error(fmt::format("Please Verify Local Data! Piece #{:d} is corrupt.", piece));
                }

                tor->piece_checked_(tor, piece, passed);
            });
    };

    session->local_data.test_piece(make_io_plan(block_info().byte_span_for_piece(piece)), piece, on_tested);
}

void tr_torrent::on_metainfo_updated()
{
    completion_ = tr_completion{ this, &block_info() };
    obfuscated_hash_ = tr_sha1::digest("req2"sv, info_hash());
    fpm_ = tr_file_piece_map{ metainfo_ };
    file_mtimes_.resize(file_count());
    file_priorities_ = tr_file_priorities{ &fpm_ };
    files_wanted_ = tr_files_wanted{ &fpm_ };
    checked_pieces_ = tr_bitfield{ size_t(piece_count()) };
    files_completed_ = tr_bitfield{ size_t(file_count()) };
}

void tr_torrent::on_metainfo_completed()
{
    if (deferred_init_)
    {
        // its files are still being deleted; finish_init() comes back here
        deferred_init_->is_new_torrent = true;
        return;
    }

    // we can look for files now that we know what files are in the torrent
    refresh_current_dir();

    callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_ADDED);

    // Potentially, we are in `tr_torrent::init`, and we don't want any file
    // created before `tr_torrent::start`, so we Verify but we don't Create
    // files. The verify worker also sniffs out a newly-added seed (files
    // that look complete and a first piece that hashes) so that it can skip
    // the full walk; those stats and that hash used to run here, on the
    // adding thread.
    start_verify(!session->shouldFullyVerifyAddedTorrents());
}

void tr_torrent::init(tr_ctor const& ctor)
{
    session = ctor.session();
    TR_ASSERT(session != nullptr);
    auto const lock = unique_lock();

    auto const now_sec = tr_time();

    on_metainfo_updated();

    if (auto dir = ctor.download_dir(TR_FORCE); !std::empty(dir))
    {
        download_dir_ = dir;
    }
    else if (dir = ctor.download_dir(TR_FALLBACK); !std::empty(dir))
    {
        download_dir_ = dir;
    }

    if (tr_sessionIsIncompleteDirEnabled(session))
    {
        auto const& dir = ctor.incomplete_dir();
        incomplete_dir_ = !std::empty(dir) ? dir : session->incompleteDir();
    }

    bandwidth().set_parent(&session->top_bandwidth_);
    bandwidth().set_priority(ctor.bandwidth_priority());
    error().clear();
    finished_seeding_by_idle_ = false;

    set_labels(ctor.labels());

    session->addTorrent(this);

    TR_ASSERT(bytes_downloaded_.during_this_session() == 0U);
    TR_ASSERT(bytes_uploaded_.during_this_session() == 0);

    mark_changed();

    // these are defaults that will be overwritten by the resume file
    date_added_ = now_sec;
    set_sequential_download(session->sequential_download());

    tr_resume::fields_t loaded = {};

    {
        // tr_resume::load() calls a lot of tr_torrentSetFoo() methods
        // that set things as dirty, but... these settings being loaded are
        // the same ones that would be saved back again, so don't let them
        // affect the 'is dirty' flag.
        auto const was_dirty = is_dirty();
        auto resume_helper = ResumeHelper{ *this };
        loaded = tr_resume::load(this, resume_helper, tr_resume::All, ctor);
        set_dirty(was_dirty);
        session->migrate_state_file(session->torrentDir(), name(), info_hash_string(), ".torrent"sv);
    }

    completeness_ = completion_.status();

    ctor.init_torrent_priorities(*this);
    ctor.init_torrent_wanted(*this);

    refresh_current_dir();

    if ((loaded & tr_resume::Speedlimit) == 0)
    {
        use_speed_limit(tr_direction::Up, false);
        set_speed_limit(tr_direction::Up, session->speed_limit(tr_direction::Up));
        use_speed_limit(tr_direction::Down, false);
        set_speed_limit(tr_direction::Down, session->speed_limit(tr_direction::Down));
        tr_torrentUseSessionLimits(this, true);
    }

    if ((loaded & tr_resume::Ratiolimit) == 0)
    {
        set_seed_ratio_mode(TR_RATIOLIMIT_GLOBAL);
        set_seed_ratio(session->desiredRatio());
    }

    if ((loaded & tr_resume::Idlelimit) == 0)
    {
        set_idle_limit_mode(TR_IDLELIMIT_GLOBAL);
        set_idle_limit_minutes(session->idleLimitMinutes());
    }

    auto has_any_local_data = std::optional<bool>{};
    if ((loaded & tr_resume::Progress) != 0)
    {
        // if tr_resume::load() loaded progress info, then initCheckedPieces()
        // has already looked for local data on the filesystem
        has_any_local_data = std::ranges::any_of(file_mtimes_, [](auto mtime) { return mtime > 0; });
    }

    auto file_path = std::string{ store_file() };

    // if we don't have a local .torrent or .magnet file already,
    // assume the torrent is new. (A remove of that file may still be
    // queued on the state writer if this torrent was just removed.)
    session->state_writer.flush(file_path);
    bool const is_new_torrent = !tr_sys_path_exists(file_path);

    if (is_new_torrent)
    {
        save_store_file(std::move(file_path), has_metainfo() ? std::string{ ctor.contents() } : magnet());
    }

    torrent_announcer = session->announcer_->addTorrent(this, &tr_torrent::on_tracker_response);

    if (is_path_being_deleted(this))
    {
        // A removed torrent's files are still being deleted under this
        // path. Looking at them now would make this torrent complete and
        // announce `left=0` for data about to vanish, so wait for the
        // delete (see on_files_deleted()).
        deferred_init_ = DeferredInit{ is_new_torrent, has_any_local_data };
    }
    else
    {
        finish_init(is_new_torrent, has_any_local_data);
    }

    // Recover from the bug reported at https://github.com/transmission/transmission/issues/6899
    if (is_done() && date_done_ == time_t{})
    {
        date_done_ = now_sec;
    }
}

void tr_torrent::finish_init(bool const is_new_torrent, std::optional<bool> const has_any_local_data)
{
    if (auto const has_metainfo = this->has_metainfo(); is_new_torrent && has_metainfo)
    {
        on_metainfo_completed();
    }
    else if (start_when_stable_)
    {
        auto const bypass_queue = !has_metainfo; // to fetch metainfo from peers
        start(bypass_queue, has_any_local_data);
    }
    else
    {
        set_local_error_if_files_disappeared(this, has_any_local_data);
    }
}

void tr_torrent::finish_deferred_init_if_clear()
{
    if (!deferred_init_ || is_path_being_deleted(this))
    {
        return;
    }

    auto const deferred = *deferred_init_;
    deferred_init_.reset();
    finish_init(deferred.is_new_torrent, deferred.has_any_local_data);
}

void tr_torrent::set_metainfo(tr_torrent_metainfo tm)
{
    TR_ASSERT(!has_metainfo());
    metainfo_ = std::move(tm);
    on_metainfo_updated();

    got_metainfo_(this);
    session->onMetadataCompleted(id());
    set_dirty();
    mark_edited();

    on_metainfo_completed();
    this->on_announce_list_changed();
}

tr_torrent* tr_torrentNew(tr_ctor* ctor, tr_torrent** setme_duplicate_of)
{
    TR_ASSERT(ctor != nullptr);
    auto* const session = ctor->session();
    TR_ASSERT(session != nullptr);

    // is the metainfo valid?
    auto metainfo = ctor->steal_metainfo();
    if (std::empty(metainfo.info_hash_string()))
    {
        return nullptr;
    }

    // is it a duplicate?
    if (auto* const duplicate_of = session->torrents().get(metainfo.info_hash()); duplicate_of != nullptr)
    {
        if (setme_duplicate_of != nullptr)
        {
            *setme_duplicate_of = duplicate_of;
        }

        return nullptr;
    }

    auto* const tor = new tr_torrent{ std::move(metainfo) };
    tor->verify_done_callback_ = ctor->steal_verify_done_callback();
    tor->init(*ctor);
    return tor;
}

// --- Location

namespace
{
namespace location_helpers
{
size_t buildSearchPathArray(tr_torrent const* tor, std::string_view* paths)
{
    auto* walk = paths;

    if (auto const& path = tor->download_dir(); !std::empty(path))
    {
        *walk++ = path.sv();
    }

    if (auto const& path = tor->incomplete_dir(); !std::empty(path))
    {
        *walk++ = path.sv();
    }

    return walk - paths;
}
} // namespace location_helpers
} // namespace

void tr_torrent::set_location(std::string_view location, bool move_from_old_path, int volatile* setme_state)
{
    if (setme_state != nullptr)
    {
        *setme_state = TR_LOC_MOVING;
    }

    auto const tor_id = id();

    session->run_in_session_thread(
        [session = this->session, tor_id, path = std::string(location), move_from_old_path, setme_state]() mutable
        {
            auto* const tor = session->torrents().get(tor_id);
            if (tor == nullptr)
            {
                return;
            }

            // A deferred torrent has nothing of its own to move: the files
            // under its current dir belong to a removed torrent and are
            // being deleted. Just point it at the new location.
            if (!move_from_old_path || tor->deferred_init_)
            {
                tor->set_download_dir(path);
                if (setme_state != nullptr)
                {
                    *setme_state = TR_LOC_DONE;
                }

                return;
            }

            // One move at a time. A move is planned from the torrent's
            // current dirs, which an in-flight move has already pointed at
            // its destination; if that move fails, a second one planned
            // behind it would find nothing there to move and "succeed",
            // leaving the torrent pointing away from its files.
            tor->pending_moves_.emplace_back(PendingMove{ std::move(path), setme_state });
            if (std::size(tor->pending_moves_) == 1U)
            {
                tor->start_next_move();
            }
        });
}

void tr_torrent::start_next_move()
{
    TR_ASSERT(session->am_in_session_thread());
    TR_ASSERT(!std::empty(pending_moves_));

    auto const tor_id = id();
    auto const& path = pending_moves_.front().path;
    auto* const setme_state = pending_moves_.front().setme_state;

    session->verify_remove(this);

    auto old_path = std::string{ current_dir() };
    auto const top_name = std::string{ name() };
    auto const old_download_dir = download_dir_;
    auto const old_incomplete_dir = incomplete_dir_;
    auto const old_current_dir = current_dir_;
    session->local_data.move(
        tor_id,
        files(),
        old_path,
        path,
        top_name,
        [session = this->session,
         tor_id,
         path,
         old_path = std::move(old_path),
         old_download_dir,
         old_incomplete_dir,
         old_current_dir,
         setme_state](tr_torrent_id_t, tr_error const& error) mutable
        {
            auto lock = session->unique_lock();
            auto* const tor = session->torrents().get(tor_id);
            if (!tor)
            {
                return;
            }

            if (error)
            {
                tor->download_dir_ = old_download_dir;
                tor->incomplete_dir_ = old_incomplete_dir;
                tor->current_dir_ = old_current_dir;
                tor->error().set_local_error(
                    fmt::format(
                        fmt::runtime(_("Couldn't move '{old_path}' to '{path}': {error} ({error_code})")),
                        fmt::arg("old_path", old_path),
                        fmt::arg("path", path),
                        fmt::arg("error", error.message()),
                        fmt::arg("error_code", error.code())));
                tr_torrentStop(tor);

                if (setme_state != nullptr)
                {
                    *setme_state = TR_LOC_ERROR;
                }
            }
            else
            {
                tor->set_download_dir(path);
                tor->incomplete_dir_.clear();
                tor->current_dir_ = tor->download_dir();

                if (setme_state != nullptr)
                {
                    *setme_state = TR_LOC_DONE;
                }
            }

            tor->pending_moves_.pop_front();
            if (!std::empty(tor->pending_moves_) && !session->isClosing())
            {
                tor->start_next_move();
            }
        });

    // Point the torrent at the destination now. LocalData runs the torrent's
    // tasks in order, so I/O planned from here on runs after the move and
    // must use the new paths; the callback reverts them if the move fails.
    download_dir_ = path;
    incomplete_dir_.clear();
    current_dir_ = download_dir_;
}

void tr_torrentSetLocation(tr_torrent* tor, char const* location, bool move_from_old_path, int volatile* setme_state)
{
    tr_return_if_fail(tr_isTorrent(tor));
    tr_return_if_fail(location != nullptr);
    tr_return_if_fail(*location != '\0');

    tor->set_location(location, move_from_old_path, setme_state);
}

std::optional<tr_torrent_files::FoundFile> tr_torrent::find_file(tr_file_index_t file_index) const
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(this, std::data(paths));
    return files().find(file_index, std::data(paths), n_paths);
}

tr_io_plan tr_torrent::make_io_plan(tr_byte_span_t const byte_span) const
{
    TR_ASSERT(session->am_in_session_thread());

    auto plan = tr_io_plan{};
    plan.tor_id = id();
    plan.byte_span = byte_span;
    plan.name = name();
    plan.download_dir = download_dir().sv();
    plan.incomplete_dir = incomplete_dir().sv();
    plan.current_dir = current_dir().sv();
    plan.prealloc = session->preallocationMode();
    plan.incomplete_file_naming = session->isIncompleteFileNamingEnabled();

    // An out-of-range span gets no files, so LocalData rejects it with EINVAL.
    if (!byte_span.is_valid() || byte_span.end > total_size())
    {
        return plan;
    }

    auto [file_index, file_offset] = fpm_.file_offset(byte_span.begin);
    for (auto n_left = byte_span.size(); n_left > 0U; ++file_index, file_offset = 0U)
    {
        auto const length = std::min(n_left, file_size(file_index) - file_offset);
        if (length > 0U)
        {
            plan.files.push_back({ .index = file_index,
                                   .offset = file_offset,
                                   .length = length,
                                   .size = file_size(file_index),
                                   .subpath = file_subpath(file_index),
                                   .wanted = file_is_wanted(file_index) });
        }
        n_left -= length;
    }

    return plan;
}

bool tr_torrent::has_any_local_data() const
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(this, std::data(paths));
    return files().has_any_local_data(std::data(paths), n_paths);
}

void tr_torrentSetDownloadDir(tr_torrent* tor, std::string_view const path)
{
    tr_return_if_fail(tr_isTorrent(tor));

    if (tor->download_dir_ != path)
    {
        tor->set_download_dir(path, true);
    }
}

std::string_view tr_torrentGetDownloadDir(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), "");

    return tor->download_dir().sv();
}

std::string_view tr_torrentGetCurrentDir(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), "");

    return tor->current_dir().sv();
}

void tr_torrentChangeMyPort(tr_torrent* tor)
{
    tr_return_if_fail(tr_isTorrent(tor));

    if (tor->is_running())
    {
        tr_announcerChangeMyPort(tor);
    }
}

// ---

namespace
{
namespace manual_update_helpers
{
void torrentManualUpdateImpl(tr_torrent* const tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    if (tor->is_running())
    {
        tr_announcerManualAnnounce(tor);
    }
}
} // namespace manual_update_helpers
} // namespace

void tr_torrentManualUpdate(tr_torrent* tor)
{
    using namespace manual_update_helpers;

    tr_return_if_fail(tr_isTorrent(tor));

    tor->session->run_in_session_thread(torrentManualUpdateImpl, tor);
}

bool tr_torrentCanManualUpdate(tr_torrent const* tor)
{
    return tr_isTorrent(tor) && tor->is_running() && tr_announcerCanManualAnnounce(tor);
}

// ---

tr_stat tr_torrent::stats() const
{
    static auto constexpr IsStalled = [](tr_torrent const* const tor, std::optional<time_t> idle_secs)
    {
        return tor->session->queueStalledEnabled() &&
            idle_secs > static_cast<time_t>(tor->session->queueStalledMinutes() * 60U);
    };

    auto const lock = unique_lock();

    auto const now_msec = tr_time_msec();
    auto const now_sec = tr_time();

    auto const swarm_stats = this->swarm != nullptr ? tr_swarmGetStats(this->swarm) : tr_swarm_stats{};
    auto const activity = this->activity();
    auto const idle_seconds = this->idle_seconds(now_sec);

    auto stats = tr_stat{};

    stats.id = this->id();
    stats.activity = activity;
    stats.error = this->error().error_type();
    stats.queue_position = queue_position();
    stats.idle_secs = idle_seconds ? *idle_seconds : time_t{ -1 };
    stats.is_stalled = IsStalled(this, idle_seconds);
    stats.error_string = this->error().errmsg();

    stats.peers_connected = swarm_stats.peer_count;
    stats.peers_sending_to_us = swarm_stats.active_peer_count[static_cast<uint8_t>(tr_direction::Down)];
    stats.peers_getting_from_us = swarm_stats.active_peer_count[static_cast<uint8_t>(tr_direction::Up)];
    stats.webseeds_sending_to_us = swarm_stats.active_webseed_count;

    for (int i = 0; i < TR_PEER_FROM_N_TYPES; i++)
    {
        stats.peers_from[i] = swarm_stats.peer_from_count[i];
        stats.known_peers_from[i] = swarm_stats.known_peer_from_count[i];
    }

    auto const piece_upload_speed = bandwidth().get_piece_speed(now_msec, tr_direction::Up);
    auto const piece_download_speed = bandwidth().get_piece_speed(now_msec, tr_direction::Down);
    stats.piece_upload_speed = piece_upload_speed;
    stats.piece_download_speed = piece_download_speed;

    stats.percent_complete = static_cast<float>(this->completion_.percent_complete());
    stats.metadata_percent_complete = static_cast<float>(get_metadata_percent());

    stats.percent_done = static_cast<float>(this->completion_.percent_done());
    stats.left_until_done = this->completion_.left_until_done();
    stats.size_when_done = this->completion_.size_when_done();

    auto const verify_progress = this->verify_progress();
    stats.recheck_progress = verify_progress.value_or(0.0);
    stats.activity_date = this->date_active_;
    stats.added_date = this->date_added_;
    stats.done_date = this->date_done_;
    stats.edit_date = this->date_edited_;
    stats.start_date = this->date_started_;
    stats.seconds_seeding = this->seconds_seeding(now_sec);
    stats.seconds_downloading = this->seconds_downloading(now_sec);

    stats.corrupt_ever = this->bytes_corrupt_.ever();
    stats.downloaded_ever = this->bytes_downloaded_.ever();
    stats.uploaded_ever = this->bytes_uploaded_.ever();
    stats.have_valid = this->completion_.has_valid();
    stats.have_unchecked = this->has_total() - stats.have_valid;
    stats.desired_available = tr_peerMgrGetDesiredAvailable(this);

    stats.upload_ratio = static_cast<float>(tr_getRatio(stats.uploaded_ever, this->size_when_done()));

    auto seed_ratio_bytes_left = uint64_t{};
    auto seed_ratio_bytes_goal = uint64_t{};
    bool const seed_ratio_applies = tr_torrentGetSeedRatioBytes(this, &seed_ratio_bytes_left, &seed_ratio_bytes_goal);

    // eta, etaIdle
    stats.eta = TR_ETA_NOT_AVAIL;
    stats.eta_idle = TR_ETA_NOT_AVAIL;
    if (activity == TR_STATUS_DOWNLOAD)
    {
        if (auto const eta_speed_byps = eta_speed_.update(now_msec, piece_download_speed).base_quantity(); eta_speed_byps == 0U)
        {
            stats.eta = TR_ETA_UNKNOWN;
        }
        else if (stats.left_until_done <= stats.desired_available || webseed_count() >= 1U)
        {
            stats.eta = static_cast<time_t>(stats.left_until_done / eta_speed_byps);
        }
    }
    else if (activity == TR_STATUS_SEED)
    {
        auto const eta_speed_byps = eta_speed_.update(now_msec, piece_upload_speed).base_quantity();

        if (seed_ratio_applies)
        {
            stats.eta = eta_speed_byps == 0U ? TR_ETA_UNKNOWN : static_cast<time_t>(seed_ratio_bytes_left / eta_speed_byps);
        }

        if (eta_speed_byps < 1U)
        {
            if (auto const secs_left = idle_seconds_left(now_sec); secs_left)
            {
                stats.eta_idle = *secs_left;
            }
        }
    }

    /* stats.haveValid is here to make sure a torrent isn't marked 'finished'
     * when the user hits "uncheck all" prior to starting the torrent... */
    stats.finished = this->finished_seeding_by_idle_ ||
        (seed_ratio_applies && seed_ratio_bytes_left == 0 && stats.have_valid != 0);

    if (!seed_ratio_applies || stats.finished)
    {
        stats.seed_ratio_percent_done = 1.0F;
    }
    else if (seed_ratio_bytes_goal == 0) /* impossible? safeguard for div by zero */
    {
        stats.seed_ratio_percent_done = 0.0F;
    }
    else
    {
        stats.seed_ratio_percent_done = static_cast<float>(seed_ratio_bytes_goal - seed_ratio_bytes_left) /
            static_cast<float>(seed_ratio_bytes_goal);
    }

    /* test some of the constraints */
    TR_ASSERT(stats.size_when_done <= this->total_size());
    TR_ASSERT(stats.left_until_done <= stats.size_when_done);
    TR_ASSERT(stats.desired_available <= stats.left_until_done);
    return stats;
}

tr_stat tr_torrentStat(tr_torrent* const tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->stats();
}

std::vector<tr_stat> tr_torrentStat(tr_torrent* const* torrents, size_t n_torrents)
{
    tr_return_val_if_fail(torrents != nullptr, {});
    tr_return_val_if_fail(std::all_of(torrents, torrents + n_torrents, tr_isTorrent), {});

    auto ret = std::vector<tr_stat>{};

    if (n_torrents != 0U)
    {
        ret.reserve(n_torrents);

        auto const lock = torrents[0]->unique_lock();

        for (size_t idx = 0U; idx != n_torrents; ++idx)
        {
            ret.emplace_back(torrents[idx]->stats());
        }
    }

    return ret;
}

// ---

tr_file_view tr_torrentFile(tr_torrent const* tor, tr_file_index_t file)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    auto const& subpath = tor->file_subpath(file);
    auto const priority = tor->file_priorities_.file_priority(file);
    auto const wanted = tor->files_wanted_.file_wanted(file);
    auto const length = tor->file_size(file);
    auto const [begin, end] = tor->piece_span_for_file(file);

    if (tor->is_seed() || length == 0)
    {
        return {
            .name = subpath.c_str(),
            .have = length,
            .length = length,
            .progress = 1.0,
            .beginPiece = begin,
            .endPiece = end,
            .priority = priority,
            .wanted = wanted,
        };
    }

    auto const have = tor->completion_.count_has_bytes_in_span(tor->byte_span_for_file(file));
    return {
        .name = subpath.c_str(),
        .have = have,
        .length = length,
        .progress = have >= length ? 1.0 : static_cast<double>(have) / static_cast<double>(length),
        .beginPiece = begin,
        .endPiece = end,
        .priority = priority,
        .wanted = wanted,
    };
}

size_t tr_torrentFileCount(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->file_count();
}

tr_webseed_view tr_torrentWebseed(tr_torrent const* tor, size_t nth)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tr_peerMgrWebseed(tor, nth);
}

size_t tr_torrentWebseedCount(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->webseed_count();
}

tr_tracker_view tr_torrentTracker(tr_torrent const* tor, size_t i)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tr_announcerTracker(tor, i);
}

size_t tr_torrentTrackerCount(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tr_announcerTrackerCount(tor);
}

tr_torrent_view tr_torrentView(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return {
        .name = tor->name().c_str(),
        .hash_string = tor->info_hash_string().c_str(),
        .comment = tor->comment().c_str(),
        .creator = tor->creator().c_str(),
        .source = tor->source().c_str(),
        .total_size = tor->total_size(),
        .date_created = tor->date_created(),
        .piece_size = tor->piece_size(),
        .n_pieces = tor->piece_count(),
        .is_private = tor->is_private(),
        .is_folder = tor->file_count() > 1 || (tor->file_count() == 1 && tr_strv_contains(tor->file_subpath(0), '/')),
    };
}

std::string tr_torrentFilename(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return std::string{ tor->torrent_file() };
}

// ---

std::vector<tr_peer_stat> tr_torrentPeers(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tr_peerMgrPeerStats(tor);
}

void tr_torrentAvailability(tr_torrent const* tor, int8_t* tab, int size)
{
    tr_return_if_fail(tr_isTorrent(tor));

    if (tab != nullptr && size > 0)
    {
        tr_peerMgrTorrentAvailability(tor, tab, size);
    }
}

void tr_torrentAmountFinished(tr_torrent const* tor, float* tabs, int n_tabs)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->amount_done_bins(tabs, n_tabs);
}

// --- Start/Stop Callback

void tr_torrentStart(tr_torrent* tor)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->start_when_stable_ = true;
    tor->start(false /*bypass_queue*/, {});
}

void tr_torrentStartNow(tr_torrent* tor)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->start_when_stable_ = true;
    tor->start(true /*bypass_queue*/, {});
}

// ---

void tr_torrentVerify(tr_torrent* tor)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->start_verify(false);
}

void tr_torrent::start_verify(bool const sniff_new_seed)
{
    session->run_in_session_thread(
        [tor = this, session = this->session, tor_id = id(), sniff_new_seed]()
        {
            TR_ASSERT(session->am_in_session_thread());
            auto const lock = session->unique_lock();

            if (tor != session->torrents().get(tor_id) || tor->is_deleting_)
            {
                return;
            }

            if (tor->deferred_init_)
            {
                return; // its files are still being deleted; finish_init() checks them
            }

            session->verify_remove(tor);

            if (!tor->has_metainfo())
            {
                return;
            }

            if (tor->is_running())
            {
                tor->stop_now();
            }

            // Whether the files have disappeared is learned from the verify
            // itself (see VerifyMediator::on_verify_done), not by statting
            // them all here.
            session->verify_add(tor, sniff_new_seed);
        });
}

void tr_torrent::set_verify_state(VerifyState const state)
{
    TR_ASSERT(state == VerifyState::None || state == VerifyState::Queued || state == VerifyState::Active);

    verify_state_ = state;
    verify_progress_ = {};
    mark_changed();
}

void tr_torrent::on_verify_removed()
{
    ++verify_generation_;

    if (verify_state_ != VerifyState::None)
    {
        set_verify_state(VerifyState::None);
    }
}

tr_torrent::VerifyMediator::VerifyMediator(tr_torrent const* const tor, bool const sniff_new_seed)
    : session_{ tor->session }
    , poster_{ tor->session->session_thread_poster() }
    , tor_id_{ tor->id() }
    , generation_{ tor->verify_generation_ }
    , metainfo_{ tor->metainfo_ }
    , sniff_new_seed_{ sniff_new_seed }
    , date_added_{ tor->date_added_ }
    , had_data_{ tor->has_total() > 0U }
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(tor, std::data(paths));
    search_paths_.assign(std::begin(paths), std::begin(paths) + n_paths);
}

std::optional<std::string> tr_torrent::VerifyMediator::find_file(tr_file_index_t const file_index) const
{
    auto paths = std::vector<std::string_view>{ std::begin(search_paths_), std::end(search_paths_) };
    if (auto const found = metainfo_.files().find(file_index, std::data(paths), std::size(paths)); found)
    {
        return std::string{ found->filename().sv() };
    }

    return {};
}

// (called from tr_verify_worker's thread)
bool tr_torrent::VerifyMediator::is_complete_copy_on_disk() const
{
    auto paths = std::vector<std::string_view>{ std::begin(search_paths_), std::end(search_paths_) };
    auto const& files = metainfo_.files();

    for (tr_file_index_t i = 0, n = files.file_count(); i < n; ++i)
    {
        // it's not a new seed if a file is missing
        auto const found = files.find(i, std::data(paths), std::size(paths));
        if (!found)
        {
            return false;
        }

        // it's not a new seed if a file is partial
        if (tr_strv_ends_with(found->filename(), tr_torrent_files::PartialFileSuffix))
        {
            return false;
        }

        // it's not a new seed if a file size is wrong
        if (found->size != files.file_size(i))
        {
            return false;
        }

        // it's not a new seed if it was modified after it was added
        if (found->last_modified_at >= date_added_)
        {
            return false;
        }
    }

    // Same-named files of the right size may still hold other content
    // (e.g. a different release of the same title), and a torrent that
    // skips the verify step announces `left=0` on the strength of this
    // check alone, so the worker hashes the first piece before believing it.
    return true;
}

void tr_torrent::VerifyMediator::post(std::function<void(tr_torrent&)> func) const
{
    poster_->post(
        [session = session_, tor_id = tor_id_, generation = generation_, func = std::move(func)]()
        {
            auto const lock = session->unique_lock();

            auto* const tor = session->torrents().get(tor_id);
            if (tor == nullptr || tor->is_deleting_ || tor->verify_generation_ != generation)
            {
                return;
            }

            func(*tor);
        });
}

// While LocalData moves the torrent, its dirs already name the destination,
// so a file the move has not reached yet is not found and keeps its suffix.
void tr_torrent::update_file_path(tr_file_index_t file, std::optional<bool> has_file) const
{
    using namespace location_helpers;

    auto paths = std::array<std::string_view, 4>{};
    auto const n_paths = buildSearchPathArray(this, std::data(paths));
    auto bases = std::vector<std::string>{ std::begin(paths), std::begin(paths) + n_paths };

    auto const has = has_file ? *has_file : this->has_file(file);
    auto const wants_suffix = session->isIncompleteFileNamingEnabled() && !has;

    // The lookup and the rename run on the torrent's LocalData worker, after
    // the I/O already queued for it, so a slow disk never holds the session
    // thread here. The callback only reports a failure.
    session->local_data.update_file_path(
        id(),
        file,
        std::move(bases),
        std::string{ file_subpath(file) },
        wants_suffix,
        [session = this->session](tr_torrent_id_t const tor_id, tr_file_index_t const file_num, tr_error const& error)
        {
            if (!error || error.code() == ECANCELED)
            {
                return;
            }

            auto const lock = session->unique_lock();
            if (auto const* const tor = session->torrents().get(tor_id); tor != nullptr)
            {
                tr_logAddErrorTor(
                    tor,
                    fmt::format(
                        fmt::runtime(_("Couldn't rename '{path}': {error} ({error_code})")),
                        fmt::arg("path", tor->file_subpath(file_num)),
                        fmt::arg("error", error.message()),
                        fmt::arg("error_code", error.code())));
            }
        });
}

// Called from tr_verify_worker::add(), on the session thread.
void tr_torrent::VerifyMediator::on_verify_queued()
{
    TR_ASSERT(session_->am_in_session_thread());

    if (auto* const tor = session_->torrents().get(tor_id_); tor != nullptr)
    {
        tr_logAddTraceTor(tor, "Queued for verification");
        tor->set_verify_state(VerifyState::Queued);
    }
}

// (called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::on_verify_started()
{
    time_started_ = tr_time();
    last_flush_ = std::chrono::steady_clock::now();

    post(
        [](tr_torrent& tor)
        {
            tr_logAddDebugTor(&tor, "Verifying torrent");
            tor.set_verify_state(VerifyState::Active);
        });
}

// (called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::on_piece_checked(tr_piece_index_t const piece, bool const has_piece)
{
    static auto constexpr MaxBatch = size_t{ 4096U };
    static auto constexpr FlushInterval = std::chrono::milliseconds{ 100 };

    checked_.emplace_back(piece, has_piece);

    if (std::size(checked_) >= MaxBatch || std::chrono::steady_clock::now() - last_flush_ >= FlushInterval)
    {
        flush_checked_pieces();
    }
}

// (called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::flush_checked_pieces()
{
    last_flush_ = std::chrono::steady_clock::now();

    if (std::empty(checked_))
    {
        return;
    }

    post(
        [checked = std::move(checked_), piece_count = metainfo_.piece_count()](tr_torrent& tor)
        {
            auto dirty = false;

            for (auto const& [piece, has_piece] : checked)
            {
                if (!has_piece || !tor.has_piece(piece))
                {
                    tor.set_has_piece(piece, has_piece);
                    dirty = true;
                }

                tor.checked_pieces_.set(piece, true);
            }

            if (dirty)
            {
                tor.set_dirty();
            }

            tor.mark_changed();
            tor.verify_progress_ = std::clamp(
                static_cast<float>(checked.back().first + 1U) / static_cast<float>(piece_count),
                0.0F,
                1.0F);
        });

    checked_.clear();
}

// (called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::on_new_seed_found()
{
    TR_ASSERT(std::empty(checked_));

    post(
        [](tr_torrent& tor)
        {
            tr_logAddDebugTor(&tor, "Files look complete and the first piece hashes: a new seed, skipping the verify");

            tor.set_piece_is_checked(0U, true);
            tor.completion_.set_has_all();
            tor.set_verify_state(VerifyState::None);
            tor.recheck_completeness();
            tor.date_done_ = tor.date_added_; // Must be after recheck_completeness()

            if (tor.verify_done_callback_)
            {
                tor.verify_done_callback_(&tor);
            }

            if (tor.start_when_stable_)
            {
                tor.start(false, true /*has_any_local_data*/);
            }
        });
}

// (called from tr_verify_worker's thread)
void tr_torrent::VerifyMediator::on_verify_done(bool const aborted, bool const found_any_file)
{
    if (aborted)
    {
        // tr_session::verify_remove() already reset the torrent's verify
        // state and made this verify's generation stale.
        checked_.clear();
        return;
    }

    flush_checked_pieces();

    post(
        [time_started = time_started_, had_data = had_data_, found_any_file](tr_torrent& tor)
        {
            if (time_started.has_value())
            {
                auto const total_size = tor.total_size();
                auto const duration_secs = tr_time() - *time_started;
                tr_logAddDebugTor(
                    &tor,
                    fmt::format(
                        "Verification is done. It took {} seconds to verify {} bytes ({} bytes per second)",
                        duration_secs,
                        total_size,
                        total_size / (1 + duration_secs)));
            }

            tor.set_verify_state(VerifyState::None);

            // A torrent that had data and now has none of its files on disk
            // stays paused rather than re-downloading over a missing drive.
            if (had_data && !found_any_file)
            {
                tor.error().set_local_error(
                    _("Paused torrent as no data was found! Ensure your drives are connected or use \"Set Location\", "
                      "then use \"Verify Local Data\" again. To re-download, start the torrent."));
                tor.start_when_stable_ = false;
            }

            for (tr_file_index_t file = 0, n_files = tor.file_count(); file < n_files; ++file)
            {
                tor.update_file_path(file, {});
            }

            tor.recheck_completeness();

            if (tor.verify_done_callback_)
            {
                tor.verify_done_callback_(&tor);
            }

            if (tor.start_when_stable_)
            {
                tor.start(false, !tor.checked_pieces_.has_none());
            }
        });
}

// ---

void tr_torrent::save_store_file(std::string filename, std::string contents)
{
    session->state_writer.save(
        filename,
        std::move(contents),
        [session = this->session, tor_id = id(), filename](tr_error const& error)
        {
            if (!error)
            {
                return;
            }

            session->run_in_session_thread(
                [session, tor_id, filename, message = std::string{ error.message() }, code = error.code()]()
                {
                    if (auto* const tor = session->torrents().get(tor_id); tor != nullptr)
                    {
                        tor->error().set_local_error(
                            fmt::format(
                                fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
                                fmt::arg("path", filename),
                                fmt::arg("error", message),
                                fmt::arg("error_code", code)));
                    }
                });
        });
}

void tr_torrent::save_resume_file()
{
    if (!is_dirty())
    {
        return;
    }

    set_dirty(false);
    auto helper = ResumeHelper{ *this };
    tr_resume::save(this, helper);
}

// --- Completeness

namespace
{
namespace completeness_helpers
{
[[nodiscard]] constexpr char const* get_completion_string(int type)
{
    switch (type)
    {
    case TR_PARTIAL_SEED:
        /* Translators: this is a minor point that's safe to skip over, but FYI:
           "Complete" and "Done" are specific, different terms in Transmission:
           "Complete" means we've downloaded every file in the torrent.
           "Done" means we're done downloading the files we wanted, but NOT all
           that exist */
        return "Done";

    case TR_SEED:
        return "Complete";

    default:
        return "Incomplete";
    }
}
} // namespace completeness_helpers
} // namespace

void tr_torrent::create_empty_files() const
{
    auto const base = current_dir();
    TR_ASSERT(!std::empty(base));
    if (!has_metainfo() || std::empty(base))
    {
        return;
    }

    auto const file_count = this->file_count();
    for (tr_file_index_t file_index = 0U; file_index < file_count; ++file_index)
    {
        if (file_size(file_index) != 0U || !file_is_wanted(file_index) || find_file(file_index))
        {
            continue;
        }

        // torrent contains a wanted zero-bytes file and that file isn't on disk yet.
        // We attempt to create that file.
        auto filename = tr_pathbuf{};
        auto const& subpath = file_subpath(file_index);
        filename.assign(base, '/', subpath);

        // create subfolders, if any
        tr_sys_dir_create(tr_sys_path_dirname(filename), TR_SYS_DIR_CREATE_PARENTS, 0777);

        // create the file
        if (auto const fd = tr_sys_file_open(filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_SEQUENTIAL, 0666);
            fd != TR_BAD_SYS_FILE)
        {
            tr_sys_file_close(fd);
        }
    }
}

void tr_torrent::recheck_completeness()
{
    using namespace completeness_helpers;

    auto const lock = unique_lock();

    needs_completeness_check_ = false;

    auto const new_completeness = completion_.status();

    // A piece whose hash check is still queued may yet fail. Do not report
    // the torrent complete (and so send a `completed` announce) until every
    // check has answered; the last answer re-arms this check. Any arrival
    // in a done state is gated, not just LEECH -> done: a partial seed
    // cannot become a seed without passing through LEECH today (unwanted
    // pieces are never requested, and changing wants rechecks at once),
    // but the announce must not depend on that staying true.
    if (new_completeness != TR_LEECH && completeness_ != new_completeness && has_pending_piece_tests())
    {
        needs_completeness_check_ = true;
        return;
    }

    if (completeness_ != new_completeness)
    {
        bool const recent_change = bytes_downloaded_.during_this_session() != 0U;
        bool const was_running = is_running();

        if (new_completeness != TR_LEECH && was_running && session->shouldFullyVerifyCompleteTorrents())
        {
            tr_torrentVerify(this);
            return;
        }

        tr_logAddTraceTor(
            this,
            fmt::format(
                "State changed from {} to {}",
                get_completion_string(completeness_),
                get_completion_string(new_completeness)));

        completeness_ = new_completeness;

        if (is_done())
        {
            session->local_data.close_torrent(id());

            if (recent_change)
            {
                // https://www.bittorrent.org/beps/bep_0003.html
                // ...and one using completed is sent when the download is complete.
                // No completed is sent if the file was complete when started.
                //
                // The last piece's hash check answers asynchronously, so
                // this can run after the torrent was stopped and `stopped`
                // already announced. Don't queue `completed` behind that:
                // send it when the torrent next starts, after `started`.
                if (is_running())
                {
                    tr_announcerTorrentCompleted(this);
                }
                else
                {
                    announce_completed_on_start_ = true;
                }
            }
            date_done_ = tr_time();

            if (current_dir() == incomplete_dir())
            {
                set_location(download_dir(), true, nullptr);
            }

            done_(this, recent_change);
        }

        session->onTorrentCompletenessChanged(id(), completeness_, was_running);

        set_dirty();
        mark_changed();

        if (is_done())
        {
            save_resume_file();
            callScriptIfEnabled(this, TR_SCRIPT_ON_TORRENT_DONE);
        }
    }
}

// --- File DND

void tr_torrentSetFileDLs(tr_torrent* tor, tr_file_index_t const* files, tr_file_index_t n_files, bool wanted)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_files_wanted(files, n_files, wanted);
}

// ---

void tr_torrent::set_labels(labels_t const& new_labels)
{
    auto const lock = unique_lock();
    labels_.clear();

    for (auto label : new_labels)
    {
        if (std::ranges::find(labels_, label) == std::ranges::end(labels_))
        {
            labels_.push_back(label);
        }
    }
    labels_.shrink_to_fit();
    set_dirty();
    mark_edited();
}

// ---

void tr_torrent::set_bandwidth_group(std::string_view group_name) noexcept
{
    group_name = tr_strv_strip(group_name);

    auto const lock = this->unique_lock();

    if (std::empty(group_name))
    {
        this->bandwidth_group_ = tr_interned_string{};
        this->bandwidth().set_parent(&this->session->top_bandwidth_);
    }
    else
    {
        this->bandwidth_group_ = group_name;
        this->bandwidth().set_parent(&this->session->getBandwidthGroup(group_name));
    }

    this->set_dirty();
}

// ---

tr_priority_t tr_torrentGetPriority(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->get_priority();
}

void tr_torrentSetPriority(tr_torrent* const tor, tr_priority_t const priority)
{
    tr_return_if_fail(tr_isTorrent(tor));
    tr_return_if_fail(tr_isPriority(priority));

    if (tor->bandwidth().get_priority() != priority)
    {
        tor->bandwidth().set_priority(priority);

        tor->set_dirty();
    }
}

// ---

void tr_torrentSetPeerLimit(tr_torrent* tor, uint16_t max_connected_peers)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_peer_limit(max_connected_peers);
}

uint16_t tr_torrentGetPeerLimit(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->peer_limit();
}

// ---

tr_block_span_t tr_torrent::block_span_for_file(tr_file_index_t const file) const noexcept
{
    auto const [begin_byte, end_byte] = byte_span_for_file(file);

    // N.B. If the last file in the torrent is 0 bytes, and the torrent size is a multiple of block size,
    // then the computed block index will be past-the-end. We handle this with std::min.
    auto const begin_block = std::min(byte_loc(begin_byte).block, block_count() - 1U);

    if (begin_byte >= end_byte) // 0-byte file
    {
        return { .begin = begin_block, .end = begin_block + 1 };
    }

    auto const final_block = byte_loc(end_byte - 1).block;
    auto const end_block = final_block + 1;
    return { .begin = begin_block, .end = end_block };
}

// ---

void tr_torrent::set_file_priorities(tr_file_index_t const* files, tr_file_index_t file_count, tr_priority_t priority)
{
    if (std::ranges::any_of(
            files,
            files + file_count,
            [this, priority](tr_file_index_t file) { return priority != file_priorities_.file_priority(file); }))
    {
        file_priorities_.set(files, file_count, priority);
        priority_changed_(this, files, file_count, priority);
        set_dirty();
        mark_changed();
    }
}

// ---

bool tr_torrent::set_announce_list(std::string_view announce_list_str)
{
    auto ann = tr_announce_list{};
    return ann.parse(announce_list_str) && set_announce_list(std::move(ann));
}

bool tr_torrent::set_announce_list(tr_announce_list announce_list)
{
    auto const lock = unique_lock();

    auto& tgt = metainfo_.announce_list();

    tgt = std::move(announce_list);

    // save the changes: the .torrent is rebuilt from the one on disk, so
    // wait for any write of it still queued, then queue the new one
    auto read_error = tr_error{};
    if (has_metainfo())
    {
        auto filename = std::string{ torrent_file() };
        session->state_writer.flush(filename);
        if (auto contents = tgt.to_torrent_file_contents(filename, &read_error); contents)
        {
            save_store_file(std::move(filename), std::move(*contents));
        }
    }
    else
    {
        save_store_file(std::string{ magnet_file() }, magnet());
    }

    on_announce_list_changed();

    if (read_error.has_value())
    {
        error().set_local_error(
            fmt::format(
                fmt::runtime(_("Couldn't save '{path}': {error} ({error_code})")),
                fmt::arg("path", torrent_file()),
                fmt::arg("error", read_error.message()),
                fmt::arg("error_code", read_error.code())));
        return false;
    }

    return true;
}

void tr_torrent::on_announce_list_changed()
{
    // if we had a tracker-related error on this torrent,
    // and that tracker's been removed,
    // then clear the error
    if (auto const& error_url = error_.announce_url(); !std::empty(error_url))
    {
        auto const& ann = metainfo().announce_list();
        if (std::ranges::none_of(ann, [error_url](auto const& tracker) { return tracker.announce == error_url; }))
        {
            error_.clear();
        }
    }

    mark_edited();

    session->announcer_->resetTorrent(this);
}

void tr_torrent::on_tracker_response(tr_tracker_event const* event)
{
    switch (event->type)
    {
    case tr_tracker_event::Type::Peers:
        tr_logAddTraceTor(this, fmt::format("Got {} peers from tracker", std::size(event->pex)));
        tr_peerMgrAddPex(this, TR_PEER_FROM_TRACKER, std::data(event->pex), std::size(event->pex));
        break;

    case tr_tracker_event::Type::Counts:
        if (is_private() && (event->leechers == 0 || event->downloaders == 0))
        {
            swarm_is_all_upload_only_(this);
        }

        break;

    case tr_tracker_event::Type::Warning:
        tr_logAddWarnTor(
            this,
            fmt::format(
                fmt::runtime(_("Tracker warning: '{warning}' ({url})")),
                fmt::arg("warning", event->text),
                fmt::arg("url", tr_urlTrackerLogName(event->announce_url))));
        error_.set_tracker_warning(event->announce_url, event->text);
        break;

    case tr_tracker_event::Type::Error:
        error_.set_tracker_error(event->announce_url, event->text);
        break;

    case tr_tracker_event::Type::ErrorClear:
        error_.clear_if_tracker();
        break;
    }
}

bool tr_torrentSetTrackerList(tr_torrent* tor, std::string_view const txt)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->set_announce_list(txt);
}

std::string tr_torrentGetTrackerList(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->announce_list().to_string();
}

// ---

uint64_t tr_torrentGetBytesLeftToAllocate(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    uint64_t bytes_left = 0;

    for (tr_file_index_t i = 0, n = tor->file_count(); i < n; ++i)
    {
        if (auto const wanted = tor->files_wanted_.file_wanted(i); !wanted)
        {
            continue;
        }

        auto const length = tor->file_size(i);
        bytes_left += length;

        auto const found = tor->find_file(i);
        if (found)
        {
            bytes_left -= found->size;
        }
    }

    return bytes_left;
}

// ---

std::string_view tr_torrent::primary_mime_type() const
{
    return files().primary_mime_type();
}

// ---

void tr_torrent::on_file_completed(tr_file_index_t const file_num)
{
    /* close the file so that we can reopen in read-only mode as needed */
    session->local_data.close_file(id(), file_num);

    /* now that the file is complete and closed, we can start watching its
     * mtime timestamp for changes to know if we need to reverify pieces */
    file_mtimes_[file_num] = tr_time();

    /* if the torrent's current filename isn't the same as the one in the
     * metadata -- for example, if it had the ".part" suffix appended to
     * it until now -- then rename it to match the one in the metadata */
    update_file_path(file_num, true);
}

void tr_torrent::on_piece_completed(tr_piece_index_t const piece)
{
    piece_completed_(this, piece);

    // bookkeeping
    set_needs_completeness_check();

    // if this piece completes any file, invoke the fileCompleted func for it
    for (auto [file, file_end] = fpm_.file_span_for_piece(piece); file < file_end; ++file)
    {
        if (has_file(file) && !files_completed_.test(file))
        {
            files_completed_.set(file, true);
            on_file_completed(file);
        }
    }
}

void tr_torrent::on_piece_failed(tr_piece_index_t const piece)
{
    tr_logAddDebugTor(this, fmt::format("Piece {}, which was just downloaded, failed its checksum test", piece));

    auto const n = piece_size(piece);
    bytes_corrupt_ += n;
    bytes_downloaded_.reduce(n);
    set_has_piece(piece, false);
    got_bad_piece_(this, piece);
}

void tr_torrent::on_block_received(tr_block_index_t const block)
{
    TR_ASSERT(this->session->am_in_session_thread());

    if (has_block(block))
    {
        tr_logAddDebugTor(this, "we have this block already...");
        bytes_downloaded_.reduce(block_size(block));
        return;
    }

    set_dirty();

    completion_.add_block(block);

    auto const on_tested = [session = this->session](
                               tr_torrent_id_t tor_id,
                               tr_piece_index_t piece,
                               tr_error const& error,
                               std::optional<tr_sha1_digest_t> hash)
    {
        session->run_in_session_thread(
            [session, tor_id, piece, error, hash = std::move(hash)]()
            {
                if (auto* const tor = session->torrents().get(tor_id))
                {
                    --tor->n_pending_piece_tests_;
                    tor->pending_piece_test_bytes_ -= tor->piece_size(piece);

                    // The piece was cleared (a verify, or an earlier failed
                    // check) while this check was queued. Whatever is on
                    // disk now will get its own check; this answer is stale.
                    if (!tor->has_piece(piece))
                    {
                        return;
                    }

                    if (error.code() == ECANCELED)
                    {
                        // The check was discarded because the torrent is
                        // going away. Its bytes are probably fine but were
                        // never hashed, so they must not be saved as complete.
                        tor->set_has_piece(piece, false);
                        tor->set_dirty();
                        return;
                    }

                    if (error)
                    {
                        // Same as main: a piece that can't be read back
                        // counts as a failed check, so it is cleared and
                        // downloaded again rather than announced as held.
                        tor->error().set_local_error(
                            fmt::format(
                                fmt::runtime(_("Couldn't verify piece #{piece}: {error} ({error_code})")),
                                fmt::arg("piece", piece),
                                fmt::arg("error", error.message()),
                                fmt::arg("error_code", error.code())));
                        tor->on_piece_failed(piece);
                        return;
                    }

                    if (hash == tor->piece_hash(piece))
                    {
                        // Peers will request this piece as soon as they see
                        // our HAVE; this check is proof enough for them.
                        tor->set_piece_is_checked(piece, true);
                        tor->on_piece_completed(piece);
                    }
                    else
                    {
                        tor->on_piece_failed(piece);
                    }
                }
            });
    };

    auto const block_loc = this->block_loc(block);
    auto const first_piece = block_loc.piece;
    auto const last_piece = byte_loc(block_loc.byte + block_size(block) - 1).piece;
    for (auto piece = first_piece; piece <= last_piece; ++piece)
    {
        if (has_piece(piece))
        {
            ++n_pending_piece_tests_;
            pending_piece_test_bytes_ += piece_size(piece);
            session->local_data.test_piece(make_io_plan(block_info().byte_span_for_piece(piece)), piece, on_tested);
        }
    }
}

void tr_torrent::on_local_write_done(
    tr_session& session,
    tr_torrent_id_t const tor_id,
    tr_error const& error,
    bool const created_file)
{
    TR_ASSERT(session.am_in_session_thread());

    if (created_file)
    {
        session.add_file_created();
    }

    auto* const tor = session.torrents().get(tor_id);
    if (tor == nullptr || !error || error.code() == ECANCELED)
    {
        return;
    }

    // if IO failed, set torrent's error if not already set
    if (!tor->error().is_local_error())
    {
        tor->error().set_local_error(error.message());
    }

    // A torrent that cannot write is dropping every block it is sent, so
    // it must stop even when an earlier local error that does not stop
    // the torrent (a corrupt piece found by an upload check, a failed
    // piece check) is the one already on display.
    if (tor->is_running())
    {
        tr_torrentStop(tor);
    }
}

// ---

std::string tr_torrentFindFile(tr_torrent const* tor, tr_file_index_t file_num)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    auto const found = tor->find_file(file_num);
    return std::string{ found ? found->filename().sv() : ""sv };
}

void tr_torrent::set_download_dir(std::string_view path, bool is_new_torrent)
{
    download_dir_ = path;
    mark_edited();
    set_dirty();

    if (deferred_init_)
    {
        // Not looking at files yet (see DeferredInit). If the new dir is
        // not under a pending delete, finish_init() looks at them now.
        current_dir_ = std::empty(incomplete_dir()) ? download_dir() : incomplete_dir();
        finish_deferred_init_if_clear();
        return;
    }

    refresh_current_dir();

    if (is_new_torrent)
    {
        start_verify(!session->shouldFullyVerifyAddedTorrents());
    }
    else if (error_.is_local_error() && !set_local_error_if_files_disappeared(this))
    {
        error_.clear();
    }
}

// decide whether we should be looking for files in downloadDir or incompleteDir
void tr_torrent::refresh_current_dir()
{
    auto dir = tr_interned_string{};

    if (std::empty(incomplete_dir()))
    {
        dir = download_dir();
    }
    else if (!has_metainfo()) // no files to find
    {
        dir = incomplete_dir();
    }
    else
    {
        auto const found = find_file(0);
        dir = found ? tr_interned_string{ found->base() } : incomplete_dir();
    }

    TR_ASSERT(!std::empty(dir));
    TR_ASSERT(dir == download_dir() || dir == incomplete_dir());

    current_dir_ = dir;
}

// --- RENAME

namespace
{
namespace rename_helpers
{
bool renameArgsAreValid(tr_torrent const* tor, std::string_view oldpath, std::string_view newname)
{
    if (std::empty(oldpath) || std::empty(newname) || newname == "."sv || newname == ".."sv || tr_strv_contains(newname, '/'))
    {
        return false;
    }

    auto const newpath = tr_strv_contains(oldpath, '/') ? tr_pathbuf{ tr_sys_path_dirname(oldpath), '/', newname } :
                                                          tr_pathbuf{ newname };

    if (newpath == oldpath)
    {
        return true;
    }

    auto const newpath_as_dir = tr_pathbuf{ newpath, '/' };
    auto const n_files = tor->file_count();

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const& name = tor->file_subpath(i);
        if (newpath == name || tr_strv_starts_with(name, newpath_as_dir))
        {
            return false;
        }
    }

    return true;
}

auto renameFindAffectedFiles(tr_torrent const* tor, std::string_view oldpath)
{
    auto indices = std::vector<tr_file_index_t>{};
    auto const oldpath_as_dir = tr_pathbuf{ oldpath, '/' };
    auto const n_files = tor->file_count();

    for (tr_file_index_t i = 0; i < n_files; ++i)
    {
        auto const& name = tor->file_subpath(i);
        if (name == oldpath || tr_strv_starts_with(name, oldpath_as_dir))
        {
            indices.push_back(i);
        }
    }

    return indices;
}

auto renamePath(tr_torrent const* tor, std::string_view oldpath, std::string_view newname)
{
    tr_error_code_t err = 0;

    auto const base = tor->is_done() || std::empty(tor->incomplete_dir()) ? tor->download_dir() : tor->incomplete_dir();

    auto src = tr_pathbuf{ base, '/', oldpath };

    if (!tr_sys_path_exists(src)) /* check for it as a partial */
    {
        src += tr_torrent_files::PartialFileSuffix;
    }

    if (tr_sys_path_exists(src))
    {
        auto const parent = tr_sys_path_dirname(src);
        auto const tgt = tr_strv_ends_with(src, tr_torrent_files::PartialFileSuffix) ?
            tr_pathbuf{ parent, '/', newname, tr_torrent_files::PartialFileSuffix } :
            tr_pathbuf{ parent, '/', newname };

        auto tmp = errno;
        bool const tgt_exists = tr_sys_path_exists(tgt);
        errno = tmp;

        if (!tgt_exists)
        {
            tmp = errno;

            if (auto error = tr_error{}; !tr_sys_path_rename(src, tgt, &error))
            {
                err = error.code();
            }

            errno = tmp;
        }
    }

    return err;
}

void renameTorrentFileString(tr_torrent* tor, std::string_view oldpath, std::string_view newname, tr_file_index_t file_index)
{
    auto name = std::string{};
    auto const subpath = std::string_view{ tor->file_subpath(file_index) };
    auto const oldpath_len = std::size(oldpath);

    if (!tr_strv_contains(oldpath, '/'))
    {
        if (oldpath_len >= std::size(subpath))
        {
            name = newname;
        }
        else
        {
            name = fmt::format("{:s}/{:s}"sv, newname, subpath.substr(oldpath_len + 1));
        }
    }
    else
    {
        auto const tmp = tr_sys_path_dirname(oldpath);

        if (std::empty(tmp))
        {
            return;
        }

        if (oldpath_len >= std::size(subpath))
        {
            name = fmt::format("{:s}/{:s}"sv, tmp, newname);
        }
        else
        {
            name = fmt::format("{:s}/{:s}/{:s}"sv, tmp, newname, subpath.substr(oldpath_len + 1));
        }
    }

    if (subpath != name)
    {
        tor->set_file_subpath(file_index, name);
    }
}

} // namespace rename_helpers
} // namespace

void tr_torrent::rename_path_in_session_thread(
    std::string_view const oldpath,
    std::string_view const newname,
    tr_torrent_rename_done_func const& callback)
{
    using namespace rename_helpers;

    auto const finish = [this, &callback](std::string_view const old, std::string_view const name, tr_error_code_t const code)
    {
        mark_changed();

        if (callback != nullptr)
        {
            auto rename_error = tr_error{};
            if (code != 0)
            {
                rename_error.set_from_errno(code);
            }

            callback(id(), old, name, rename_error);
        }
    };

    if (!renameArgsAreValid(this, oldpath, newname))
    {
        finish(oldpath, newname, EINVAL);
        return;
    }

    auto const file_indices = renameFindAffectedFiles(this, oldpath);
    if (std::empty(file_indices))
    {
        finish(oldpath, newname, EINVAL);
        return;
    }

    auto const base = is_done() || std::empty(incomplete_dir()) ? download_dir() : incomplete_dir();

    // Rename the torrent's file strings now, so that I/O planned from here
    // on uses the new names. LocalData runs the torrent's tasks in order,
    // so writes already queued under the old names land before the on-disk
    // rename below moves them; the callback undoes the strings if it fails.
    for (auto const& file_index : file_indices)
    {
        renameTorrentFileString(this, oldpath, newname, file_index);
    }

    auto const renamed_top = std::size(file_indices) == file_count() && !tr_strv_contains(oldpath, '/');
    if (renamed_top)
    {
        set_name(newname);
    }

    mark_edited();
    set_dirty();

    if (deferred_init_)
    {
        // Nothing of this torrent's is on disk yet: the files under the old
        // name belong to a removed torrent and are being deleted. Renaming
        // the top would move them out from under that delete and make this
        // torrent complete once it looks. The strings are enough.
        finish_deferred_init_if_clear();
        finish(oldpath, newname, 0);
        return;
    }

    session->local_data.rename(
        id(),
        base.sv(),
        oldpath,
        newname,
        [session = this->session,
         file_indices,
         renamed_top,
         callback](tr_torrent_id_t const tor_id, std::string_view const old, std::string_view const name, tr_error const& error)
        {
            auto const lock = session->unique_lock();

            if (auto* const tor = session->torrents().get(tor_id); tor != nullptr)
            {
                if (error)
                {
                    // put the strings back: the new path is `dirname(old)/name`
                    auto const newpath = tr_strv_contains(old, '/') ? tr_pathbuf{ tr_sys_path_dirname(old), '/', name } :
                                                                      tr_pathbuf{ name };
                    auto const oldname = tr_sys_path_basename(old);
                    for (auto const& file_index : file_indices)
                    {
                        renameTorrentFileString(tor, newpath, oldname, file_index);
                    }

                    if (renamed_top)
                    {
                        tor->set_name(oldname);
                    }

                    tor->mark_edited();
                    tor->set_dirty();
                }

                tor->mark_changed();

                // A verify in flight reads a copy of the names taken when it
                // was queued, so now that the files have moved under it,
                // start it over against the names they have now.
                if (tor->verify_state_ != VerifyState::None)
                {
                    session->verify_remove(tor);
                    session->verify_add(tor);
                }
            }

            if (callback != nullptr)
            {
                callback(tor_id, old, name, error);
            }
        });
}

void tr_torrent::rename_path(std::string_view oldpath, std::string_view newname, tr_torrent_rename_done_func&& callback)
{
    this->session->run_in_session_thread(
        [this, oldpath = std::string(oldpath), newname = std::string(newname), cb = std::move(callback)]
        { rename_path_in_session_thread(oldpath, newname, cb); });
}

void tr_torrentRenamePath(
    tr_torrent* tor,
    std::string_view const oldpath,
    std::string_view const newname,
    tr_torrent_rename_done_func callback)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->rename_path(oldpath, newname, std::move(callback));
}

// ---

void tr_torrentSetFilePriorities(
    tr_torrent* tor,
    tr_file_index_t const* files,
    tr_file_index_t file_count,
    tr_priority_t priority)
{
    tr_return_if_fail(tr_isTorrent(tor));

    tor->set_file_priorities(files, file_count, priority);
}

bool tr_torrentHasMetadata(tr_torrent const* tor)
{
    tr_return_val_if_fail(tr_isTorrent(tor), {});

    return tor->has_metainfo();
}

void tr_torrent::mark_edited()
{
    auto const now = tr_time();
    bump_date_edited(now);
    bump_date_changed(now);
}

void tr_torrent::mark_changed()
{
    this->bump_date_changed(tr_time());
}

void tr_torrent::set_piece_is_checked(tr_piece_index_t const piece, bool const passed)
{
    mark_changed();
    set_dirty();
    checked_pieces_.set(piece, passed);
}

// --- RESUME HELPER

tr_bitfield const& tr_torrent::ResumeHelper::checked_pieces() const noexcept
{
    return tor_.checked_pieces_;
}

void tr_torrent::ResumeHelper::load_checked_pieces(tr_bitfield const& checked, time_t const* mtimes /*file_count()*/)
{
    TR_ASSERT(std::size(checked) == tor_.piece_count());
    tor_.checked_pieces_ = checked;

    auto const n_files = tor_.file_count();
    tor_.file_mtimes_.resize(n_files);

    for (size_t file = 0; file < n_files; ++file)
    {
        auto const found = tor_.find_file(file);
        auto const mtime = found ? found->last_modified_at : 0;

        tor_.file_mtimes_[file] = mtime;

        // if a file has changed, mark its pieces as unchecked
        if (mtime == 0 || mtime != mtimes[file])
        {
            auto const [piece_begin, piece_end] = tor_.piece_span_for_file(file);
            tor_.checked_pieces_.unset_span(piece_begin, piece_end);
        }
    }
}

// ---

tr_bitfield const& tr_torrent::ResumeHelper::blocks() const noexcept
{
    return tor_.completion_.blocks();
}

void tr_torrent::ResumeHelper::load_blocks(tr_bitfield blocks)
{
    tor_.completion_.set_blocks(std::move(blocks));
}

// ---

time_t tr_torrent::ResumeHelper::date_active() const noexcept
{
    return tor_.date_active_;
}

// ---

time_t tr_torrent::ResumeHelper::date_added() const noexcept
{
    return tor_.date_added_;
}

void tr_torrent::ResumeHelper::load_date_added(time_t when) noexcept
{
    tor_.date_added_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::date_done() const noexcept
{
    return tor_.date_done_;
}

void tr_torrent::ResumeHelper::load_date_done(time_t when) noexcept
{
    tor_.date_done_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::seconds_downloading(time_t now) const noexcept
{
    return tor_.seconds_downloading(now);
}

void tr_torrent::ResumeHelper::load_seconds_downloading_before_current_start(time_t when) noexcept
{
    tor_.seconds_downloading_before_current_start_ = when;
}

// ---

time_t tr_torrent::ResumeHelper::seconds_seeding(time_t now) const noexcept
{
    return tor_.seconds_seeding(now);
}

void tr_torrent::ResumeHelper::load_seconds_seeding_before_current_start(time_t when) noexcept
{
    tor_.seconds_seeding_before_current_start_ = when;
}

// ---

void tr_torrent::ResumeHelper::load_download_dir(std::string_view const dir) noexcept
{
    bool const is_current_dir = tor_.current_dir_ == tor_.download_dir_;
    tor_.download_dir_ = dir;
    if (is_current_dir)
    {
        tor_.current_dir_ = tor_.download_dir_;
    }
}

void tr_torrent::ResumeHelper::load_incomplete_dir(std::string_view const dir) noexcept
{
    bool const is_current_dir = tor_.current_dir_ == tor_.incomplete_dir_;
    tor_.incomplete_dir_ = dir;
    if (is_current_dir)
    {
        tor_.current_dir_ = tor_.incomplete_dir_;
    }
}

// ---

void tr_torrent::ResumeHelper::load_start_when_stable(bool const val) noexcept
{
    tor_.start_when_stable_ = val;
}

bool tr_torrent::ResumeHelper::start_when_stable() const noexcept
{
    return tor_.start_when_stable_;
}

// ---

std::vector<time_t> const& tr_torrent::ResumeHelper::file_mtimes() const noexcept
{
    return tor_.file_mtimes_;
}
