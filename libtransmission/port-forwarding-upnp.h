// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef LIBTRANSMISSION_PORT_FORWARDING_MODULE
#error only the libtransmission port forwarding module should #include this header.
#endif

/**
 * @addtogroup port_forwarding Port Forwarding
 * @{
 */

#include <memory>
#include <optional>
#include <string>

#include "libtransmission/net.h" // for tr_port
#include "libtransmission/types.h" // for tr_port_forwarding_state

struct tr_upnp;

// The round trips to the gateway. Each one is a blocking network exchange
// with a multi-second timeout, so tr_upnp only ever runs them on a worker
// thread that holds its own copies of the arguments, and a backend must be
// safe to call from any thread. Tests substitute a fake.
class tr_upnp_backend
{
public:
    struct Igd
    {
        std::string control_url;
        std::string service_type;
        std::string lanaddr;
    };

    virtual ~tr_upnp_backend() = default;

    // SSDP discovery plus the fetch of the gateway's description.
    // Returns nullopt when no connected IGD was found.
    [[nodiscard]] virtual std::optional<Igd> discover(std::string const& bindaddr) = 0;

    // Returns true if the gateway accepted the mapping.
    [[nodiscard]] virtual bool add_port_mapping(
        Igd const& igd,
        char const* proto,
        tr_port advertised_port,
        tr_port local_port,
        std::string const& desc) = 0;

    virtual void delete_port_mapping(Igd const& igd, char const* proto, tr_port advertised_port) = 0;

    // Returns true if the gateway still has a mapping for the port.
    [[nodiscard]] virtual bool has_port_mapping(Igd const& igd, char const* proto, tr_port advertised_port) = 0;
};

// A null backend selects miniupnpc.
tr_upnp* tr_upnpInit(std::shared_ptr<tr_upnp_backend> backend = {});

// A mapping that is being created or removed when the handle is closed is
// still removed: with `wait_for_pending` the call blocks (bounded) until
// that is done, so a clean shutdown leaves nothing behind on the gateway;
// without it the worker finishes on its own and the call returns at once.
void tr_upnpClose(tr_upnp* handle, bool wait_for_pending);

tr_port_forwarding_state tr_upnpPulse(
    tr_upnp* handle,
    tr_port advertised_port,
    tr_port local_port,
    bool is_enabled,
    bool do_port_check,
    std::string bindaddr);

// True while a gateway round trip is in flight. The caller keeps pulsing
// frequently while this holds so the result is picked up promptly; the
// state alone does not show it, a port check leaves the state "mapped".
[[nodiscard]] bool tr_upnpIsBusy(tr_upnp const* handle);

/* @} */
