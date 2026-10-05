// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

#define LIBTRANSMISSION_PORT_FORWARDING_MODULE

#include "libtransmission/constants.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/port-forwarding-upnp.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/types.h"
#include "libtransmission/utils.h" // for _(), tr_strerror()

#ifndef MINIUPNPC_API_VERSION
#error miniupnpc >= 1.7 is required
#endif

using namespace std::literals;

namespace
{
using Igd = tr_upnp_backend::Igd;

enum class UpnpState : uint8_t
{
    Idle,
    WillDiscover, // next action is to launch a discover job
    Discovering, // a discover job is running on a worker thread
    WillMap, // next action is to launch a map job
    Mapping, // a map job is running on a worker thread
    WillUnmap, // next action is to launch an unmap job
    Unmapping, // an unmap job is running on a worker thread
    Checking // a port-check job is running on a worker thread; the mapping stands until it says otherwise
};

// How long a clean shutdown waits for an in-flight job before giving the
// cleanup over to the worker. miniupnpc's own socket timeouts bound each
// round trip, so this is only a backstop against a gateway that never answers.
auto constexpr CloseWait = 3s;

// ---

enum : uint8_t
{
    UPNP_IGD_NONE = 0,
    UPNP_IGD_VALID_CONNECTED = 1,
    UPNP_IGD_VALID_NOT_CONNECTED = 2,
    UPNP_IGD_INVALID = 3
};

[[nodiscard]] UPNPDev* upnp_discover(int msec, char const* bindaddr)
{
    UPNPDev* ret = nullptr;
    auto have_err = bool{};

    // MINIUPNPC_API_VERSION >= 8 (adds ipv6 and error args)
    int err = UPNPDISCOVER_SUCCESS;

#if (MINIUPNPC_API_VERSION >= 14) // adds ttl
    ret = upnpDiscover(msec, bindaddr, nullptr, 0, 0, 2, &err);
#else
    ret = upnpDiscover(msec, bindaddr, nullptr, 0, 0, &err);
#endif

    have_err = err != UPNPDISCOVER_SUCCESS;

    if (have_err)
    {
        tr_logAddDebug(fmt::format("upnpDiscover failed: {} ({})", tr_strerror(errno), errno));
    }

    return ret;
}

// The real thing. Stateless, so one instance serves every job.
class MiniupnpcBackend final : public tr_upnp_backend
{
public:
    [[nodiscard]] std::optional<Igd> discover(std::string const& bindaddr) override
    {
        // If multicastif is not NULL, it will be used instead of the default
        // multicast interface for sending SSDP discover packets.
        char const* multicastif = std::empty(bindaddr) ? nullptr : bindaddr.c_str();
        auto* const devlist = upnp_discover(2000, multicastif);

        auto urls = UPNPUrls{};
        auto data = IGDdatas{};
        auto lanaddr = std::array<char, TrAddrStrlen>{};
        auto ret = std::optional<Igd>{};

        if (
#if (MINIUPNPC_API_VERSION >= 18)
            UPNP_GetValidIGD(devlist, &urls, &data, std::data(lanaddr), std::size(lanaddr) - 1, nullptr, 0)
#else
            UPNP_GetValidIGD(devlist, &urls, &data, std::data(lanaddr), std::size(lanaddr) - 1)
#endif
            == UPNP_IGD_VALID_CONNECTED)
        {
            ret = Igd{ .control_url = urls.controlURL, .service_type = data.first.servicetype, .lanaddr = std::data(lanaddr) };
        }
        else
        {
            tr_logAddDebug(fmt::format("UPNP_GetValidIGD failed: {} ({})", tr_strerror(errno), errno));
            tr_logAddDebug("If your router supports UPnP, please make sure UPnP is enabled!");
        }

        FreeUPNPUrls(&urls);
        freeUPNPDevlist(devlist);
        return ret;
    }

    [[nodiscard]] bool add_port_mapping(
        Igd const& igd,
        char const* proto,
        tr_port advertised_port,
        tr_port local_port,
        std::string const& desc) override
    {
        int const old_errno = errno;
        errno = 0;

        auto const advertised_port_str = std::to_string(advertised_port.host());
        auto const local_port_str = std::to_string(local_port.host());

        // MINIUPNPC_API_VERSION >= 8
        int const err = UPNP_AddPortMapping(
            igd.control_url.c_str(),
            igd.service_type.c_str(),
            advertised_port_str.c_str(),
            local_port_str.c_str(),
            igd.lanaddr.c_str(),
            desc.c_str(),
            proto,
            nullptr,
            nullptr);

        if (err != 0)
        {
            tr_logAddDebug(
                fmt::format("{} Port forwarding failed with error {}: {} ({})", proto, err, tr_strerror(errno), errno));
        }

        errno = old_errno;
        return err == 0;
    }

    void delete_port_mapping(Igd const& igd, char const* proto, tr_port advertised_port) override
    {
        auto const port_str = std::to_string(advertised_port.host());

        UPNP_DeletePortMapping(igd.control_url.c_str(), igd.service_type.c_str(), port_str.c_str(), proto, nullptr);
    }

    [[nodiscard]] bool has_port_mapping(Igd const& igd, char const* proto, tr_port advertised_port) override
    {
        auto int_client = std::array<char, 16>{};
        auto int_port = std::array<char, 6>{};

        auto const port_str = std::to_string(advertised_port.host());

#if (MINIUPNPC_API_VERSION >= 10) /* adds remoteHost arg */
        int const err = UPNP_GetSpecificPortMappingEntry(
            igd.control_url.c_str(),
            igd.service_type.c_str(),
            port_str.c_str(),
            proto,
            nullptr /*remoteHost*/,
            std::data(int_client),
            std::data(int_port),
            nullptr /*desc*/,
            nullptr /*enabled*/,
            nullptr /*duration*/);
#else // MINIUPNPC_API_VERSION >= 8 (adds desc, enabled and leaseDuration args)
        int const err = UPNP_GetSpecificPortMappingEntry(
            igd.control_url.c_str(),
            igd.service_type.c_str(),
            port_str.c_str(),
            proto,
            std::data(int_client),
            std::data(int_port),
            nullptr /*desc*/,
            nullptr /*enabled*/,
            nullptr /*duration*/);
#endif

        return err == UPNPCOMMAND_SUCCESS;
    }
};

template<typename T>
bool is_future_ready(std::future<T> const& future)
{
    return future.wait_for(0s) == std::future_status::ready;
}

// Runs `func` on a detached thread and returns the future for its result.
// The thread owns everything it touches (the backend, an Igd copy, ports),
// so the handle can go away while the job is still running.
template<typename Func>
[[nodiscard]] auto launch(Func&& func)
{
    auto task = std::packaged_task<std::invoke_result_t<Func>()>{ std::forward<Func>(func) };
    auto future = task.get_future();
    std::thread{ std::move(task) }.detach();
    return future;
}

void unmap_both(tr_upnp_backend& backend, Igd const& igd, tr_port advertised_port)
{
    backend.delete_port_mapping(igd, "TCP", advertised_port);
    backend.delete_port_mapping(igd, "UDP", advertised_port);
}
} // namespace

struct tr_upnp
{
    explicit tr_upnp(std::shared_ptr<tr_upnp_backend> backend_in)
        : backend{ std::move(backend_in) }
    {
    }

    tr_upnp(tr_upnp&&) = delete;
    tr_upnp(tr_upnp const&) = delete;
    tr_upnp& operator=(tr_upnp&&) = delete;
    tr_upnp& operator=(tr_upnp const&) = delete;

    ~tr_upnp()
    {
        TR_ASSERT(!isMapped);
        TR_ASSERT(!map_future && !unmap_future);
    }

    // Makes sure nothing stays mapped on the gateway: an unmap that is
    // already running is left to finish, a mapping that stands (e.g. closed
    // during a port check) is unmapped, and whatever a running map job ends
    // up mapping is unmapped once it reports. All on a worker; the caller
    // decides whether to wait for it.
    void close(bool wait_for_pending)
    {
        auto pending = std::optional<std::future<void>>{};

        if (map_future)
        {
            pending = launch(
                [backend = backend, igd = *igd, port = pending_advertised_port, future = std::move(*map_future)]() mutable
                {
                    if (future.get())
                    {
                        unmap_both(*backend, igd, port);
                    }
                });
            map_future.reset();
        }
        else if (isMapped)
        {
            pending = launch([backend = backend, igd = *igd, port = advertised_port]() { unmap_both(*backend, igd, port); });
        }
        else if (unmap_future)
        {
            pending = std::move(unmap_future);
            unmap_future.reset();
        }

        if (pending && wait_for_pending)
        {
            pending->wait_for(CloseWait);
        }

        isMapped = false;
        advertised_port = {};
        local_port = {};
        state = UpnpState::Idle;
    }

    std::shared_ptr<tr_upnp_backend> backend;

    std::optional<Igd> igd;
    tr_port advertised_port;
    tr_port local_port;
    bool isMapped = false;
    UpnpState state = UpnpState::WillDiscover;

    // The ports a running map job was given; they become
    // advertised_port / local_port when it reports success.
    tr_port pending_advertised_port;
    tr_port pending_local_port;

    // Results of the jobs, each pending while `state` is the matching
    // in-flight state. Consumed by tr_upnpPulse(), never waited on.
    std::optional<std::future<std::optional<Igd>>> discover_future;
    std::optional<std::future<bool>> map_future;
    std::optional<std::future<void>> unmap_future;
    std::optional<std::future<bool>> check_future;
};

namespace
{
constexpr auto port_fwd_state(UpnpState upnp_state, bool is_mapped)
{
    switch (upnp_state)
    {
    case UpnpState::WillDiscover:
    case UpnpState::Discovering:
        return TR_PORT_UNMAPPED;

    case UpnpState::WillMap:
    case UpnpState::Mapping:
        return TR_PORT_MAPPING;

    case UpnpState::WillUnmap:
    case UpnpState::Unmapping:
        return TR_PORT_UNMAPPING;

    case UpnpState::Idle:
    case UpnpState::Checking:
        return is_mapped ? TR_PORT_MAPPED : TR_PORT_UNMAPPED;

    default: // UpnpState::FAILED:
        return TR_PORT_ERROR;
    }
}
} // namespace

// ---

tr_upnp* tr_upnpInit(std::shared_ptr<tr_upnp_backend> backend)
{
    if (!backend)
    {
        backend = std::make_shared<MiniupnpcBackend>();
    }

    return new tr_upnp{ std::move(backend) };
}

void tr_upnpClose(tr_upnp* handle, bool wait_for_pending)
{
    if (handle == nullptr)
    {
        return;
    }

    handle->close(wait_for_pending);
    delete handle;
}

bool tr_upnpIsBusy(tr_upnp const* handle)
{
    switch (handle->state)
    {
    case UpnpState::Discovering:
    case UpnpState::Mapping:
    case UpnpState::Unmapping:
    case UpnpState::Checking:
        return true;

    default:
        return false;
    }
}

tr_port_forwarding_state tr_upnpPulse(
    tr_upnp* handle,
    tr_port advertised_port,
    tr_port local_port,
    bool is_enabled,
    bool do_port_check,
    std::string bindaddr)
{
    // Pick up the result of whatever job is running.

    if (handle->state == UpnpState::Discovering && is_future_ready(*handle->discover_future))
    {
        handle->igd = handle->discover_future->get();
        handle->discover_future.reset();

        if (handle->igd)
        {
            tr_logAddInfo(
                fmt::format(
                    fmt::runtime(_("Found Internet Gateway Device '{url}'")),
                    fmt::arg("url", handle->igd->control_url)));
            tr_logAddInfo(fmt::format(fmt::runtime(_("Local Address is '{address}'")), fmt::arg("address", handle->igd->lanaddr)));
            handle->state = UpnpState::Idle;
        }
        else
        {
            handle->state = UpnpState::WillDiscover;
        }
    }

    if (handle->state == UpnpState::Mapping && is_future_ready(*handle->map_future))
    {
        handle->isMapped = handle->map_future->get();
        handle->map_future.reset();

        if (handle->isMapped)
        {
            tr_logAddInfo(
                fmt::format(
                    fmt::runtime(_("Forwarded local port {local_port} to {advertised_port}")),
                    fmt::arg("local_port", handle->pending_local_port.host()),
                    fmt::arg("advertised_port", handle->pending_advertised_port.host())));
            handle->advertised_port = handle->pending_advertised_port;
            handle->local_port = handle->pending_local_port;
        }
        else
        {
            tr_logAddInfo(_("If your router supports UPnP, please make sure UPnP is enabled!"));
            handle->advertised_port = {};
            handle->local_port = {};
        }

        handle->pending_advertised_port = {};
        handle->pending_local_port = {};
        handle->state = UpnpState::Idle;
    }

    if (handle->state == UpnpState::Unmapping && is_future_ready(*handle->unmap_future))
    {
        handle->unmap_future->get();
        handle->unmap_future.reset();
        handle->state = UpnpState::Idle;
    }

    if (handle->state == UpnpState::Checking && is_future_ready(*handle->check_future))
    {
        auto const still_mapped = handle->check_future->get();
        handle->check_future.reset();
        handle->state = UpnpState::Idle;

        if (!still_mapped)
        {
            tr_logAddInfo(
                fmt::format(
                    fmt::runtime(_("Local port {local_port} is not forwarded to {advertised_port}")),
                    fmt::arg("local_port", handle->local_port.host()),
                    fmt::arg("advertised_port", handle->advertised_port.host())));
            handle->isMapped = false;
        }
    }

    // Decide what to do next and launch it. Each launch is a round trip to
    // the gateway, so it runs on its own thread with its own copies.

    if (is_enabled && handle->state == UpnpState::WillDiscover)
    {
        TR_ASSERT(!handle->discover_future);
        handle->discover_future = launch([backend = handle->backend, bindaddr = std::move(bindaddr)]()
                                         { return backend->discover(bindaddr); });
        handle->state = UpnpState::Discovering;
    }

    if (handle->state == UpnpState::Idle && handle->isMapped &&
        (!is_enabled || handle->advertised_port != advertised_port || handle->local_port != local_port))
    {
        handle->state = UpnpState::WillUnmap;
    }

    if (is_enabled && handle->state == UpnpState::Idle && handle->isMapped && do_port_check)
    {
        TR_ASSERT(!handle->check_future);
        TR_ASSERT(handle->igd);
        handle->check_future = launch(
            [backend = handle->backend, igd = *handle->igd, port = handle->advertised_port]()
            { return backend->has_port_mapping(igd, "TCP", port) && backend->has_port_mapping(igd, "UDP", port); });
        handle->state = UpnpState::Checking;
    }

    if (handle->state == UpnpState::WillUnmap)
    {
        TR_ASSERT(!handle->unmap_future);
        TR_ASSERT(handle->igd);
        handle->unmap_future = launch([backend = handle->backend, igd = *handle->igd, port = handle->advertised_port]()
                                      { unmap_both(*backend, igd, port); });

        tr_logAddInfo(
            fmt::format(
                fmt::runtime(_("Stopping port forwarding through '{url}', service '{type}'")),
                fmt::arg("url", handle->igd->control_url),
                fmt::arg("type", handle->igd->service_type)));

        handle->isMapped = false;
        handle->advertised_port = {};
        handle->local_port = {};
        handle->state = UpnpState::Unmapping;
    }

    if (handle->state == UpnpState::Idle && is_enabled && !handle->isMapped)
    {
        handle->state = UpnpState::WillMap;
    }

    if (handle->state == UpnpState::WillMap)
    {
        if (!handle->igd)
        {
            // nothing to map through; find a gateway first
            handle->state = UpnpState::WillDiscover;
        }
        else
        {
            TR_ASSERT(!handle->map_future);
            handle->pending_advertised_port = advertised_port;
            handle->pending_local_port = local_port;
            handle->map_future = launch(
                [backend = handle->backend, igd = *handle->igd, advertised_port, local_port]()
                {
                    auto const desc = fmt::format("Transmission at {:d}", local_port.host());
                    auto const ok_tcp = backend->add_port_mapping(igd, "TCP", advertised_port, local_port, desc);
                    auto const ok_udp = backend->add_port_mapping(igd, "UDP", advertised_port, local_port, desc);
                    return ok_tcp || ok_udp;
                });

            tr_logAddDebug(
                fmt::format(
                    fmt::runtime(_("Port forwarding through '{url}', service '{type}'. (local address: {address}:{port})")),
                    fmt::arg("url", handle->igd->control_url),
                    fmt::arg("type", handle->igd->service_type),
                    fmt::arg("address", handle->igd->lanaddr),
                    fmt::arg("port", local_port.host())));

            handle->state = UpnpState::Mapping;
        }
    }

    return port_fwd_state(handle->state, handle->isMapped);
}
