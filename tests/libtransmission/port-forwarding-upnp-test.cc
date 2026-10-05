// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#define LIBTRANSMISSION_PORT_FORWARDING_MODULE
#include <libtransmission/net.h>
#include <libtransmission/port-forwarding-upnp.h>
#include <libtransmission/types.h>

#include "test-fixtures.h"

using namespace std::literals;
using tr::test::waitFor;

namespace
{
// A gateway that answers whatever the test tells it to, and that can be
// held: while the gate is closed every call blocks inside the backend, the
// way a slow or dead router would. Pulses must come back regardless.
class FakeGateway final : public tr_upnp_backend
{
public:
    struct Call
    {
        std::string name;
        std::string proto;
        tr_port advertised_port;
        tr_port local_port;
        std::string desc;
    };

    [[nodiscard]] std::optional<Igd> discover(std::string const& /*bindaddr*/) override
    {
        record({ .name = "discover", .proto = {}, .advertised_port = {}, .local_port = {}, .desc = {} });
        return discover_answer;
    }

    [[nodiscard]] bool add_port_mapping(
        Igd const& /*igd*/,
        char const* proto,
        tr_port advertised_port,
        tr_port local_port,
        std::string const& desc) override
    {
        record({ .name = "add", .proto = proto, .advertised_port = advertised_port, .local_port = local_port, .desc = desc });
        return add_answer;
    }

    void delete_port_mapping(Igd const& /*igd*/, char const* proto, tr_port advertised_port) override
    {
        record({ .name = "delete", .proto = proto, .advertised_port = advertised_port, .local_port = {}, .desc = {} });
    }

    [[nodiscard]] bool has_port_mapping(Igd const& /*igd*/, char const* proto, tr_port advertised_port) override
    {
        record({ .name = "has", .proto = proto, .advertised_port = advertised_port, .local_port = {}, .desc = {} });
        return has_answer;
    }

    void close_gate()
    {
        auto const lock = std::scoped_lock{ mutex_ };
        gate_open_ = false;
    }

    void open_gate()
    {
        auto const lock = std::scoped_lock{ mutex_ };
        gate_open_ = true;
        cv_.notify_all();
    }

    // lets exactly `n` more calls through a closed gate
    void allow(size_t n)
    {
        auto const lock = std::scoped_lock{ mutex_ };
        permits_ += n;
        cv_.notify_all();
    }

    // calls made so far, in order, including ones still blocked on the gate
    [[nodiscard]] std::vector<Call> calls()
    {
        auto const lock = std::scoped_lock{ mutex_ };
        return calls_;
    }

    [[nodiscard]] size_t count(std::string_view name)
    {
        auto const lock = std::scoped_lock{ mutex_ };
        return std::count_if(std::begin(calls_), std::end(calls_), [name](auto const& call) { return call.name == name; });
    }

    // calls that returned to their caller
    [[nodiscard]] size_t completed()
    {
        auto const lock = std::scoped_lock{ mutex_ };
        return completed_;
    }

    std::optional<Igd> discover_answer = Igd{ .control_url = "http://192.0.2.1:5000/ctl",
                                              .service_type = "urn:schemas-upnp-org:service:WANIPConnection:1",
                                              .lanaddr = "192.0.2.2" };
    bool add_answer = true;
    bool has_answer = true;

private:
    void record(Call call)
    {
        auto lock = std::unique_lock{ mutex_ };
        calls_.emplace_back(std::move(call));
        cv_.wait(lock, [this] { return gate_open_ || permits_ > 0U; });
        if (!gate_open_)
        {
            --permits_;
        }
        ++completed_;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool gate_open_ = true;
    size_t permits_ = 0;
    std::vector<Call> calls_;
    size_t completed_ = 0;
};

auto constexpr AdvertisedPort = tr_port::from_host(51413);
auto constexpr LocalPort = tr_port::from_host(51414);
auto constexpr Timeout = 5s;

// the session thread must never notice a gateway round trip
auto constexpr PulseBudget = 100ms;

class PortForwardingUpnpTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        gateway_ = std::make_shared<FakeGateway>();
        upnp_ = tr_upnpInit(gateway_);
    }

    void TearDown() override
    {
        if (upnp_ != nullptr)
        {
            close(false);
        }
    }

    tr_port_forwarding_state pulse(
        bool is_enabled = true,
        bool do_port_check = false,
        tr_port advertised_port = AdvertisedPort,
        tr_port local_port = LocalPort)
    {
        auto const begin = std::chrono::steady_clock::now();
        auto const state = tr_upnpPulse(upnp_, advertised_port, local_port, is_enabled, do_port_check, "");
        auto const elapsed = std::chrono::steady_clock::now() - begin;
        EXPECT_LT(elapsed, PulseBudget) << "a pulse blocked on the gateway";
        return state;
    }

    // pulses until the state machine reports `wanted`
    [[nodiscard]] bool pulseUntil(
        tr_port_forwarding_state wanted,
        bool is_enabled = true,
        tr_port advertised_port = AdvertisedPort,
        tr_port local_port = LocalPort)
    {
        return waitFor(
            [&] { return pulse(is_enabled, false, advertised_port, local_port) == wanted && !tr_upnpIsBusy(upnp_); },
            Timeout);
    }

    void mapped()
    {
        ASSERT_TRUE(pulseUntil(TR_PORT_MAPPED));
        ASSERT_EQ(1U, gateway_->count("discover"));
        ASSERT_EQ(2U, gateway_->count("add"));
    }

    void close(bool wait_for_pending)
    {
        auto const begin = std::chrono::steady_clock::now();
        tr_upnpClose(upnp_, wait_for_pending);
        upnp_ = nullptr;
        close_took_ = std::chrono::steady_clock::now() - begin;
    }

    std::shared_ptr<FakeGateway> gateway_;
    tr_upnp* upnp_ = nullptr;
    std::chrono::steady_clock::duration close_took_ = {};
};

TEST_F(PortForwardingUpnpTest, mapsWithoutBlockingThePulse)
{
    // every round trip is held: the state machine must still report
    // progress and give the pulse back
    gateway_->close_gate();

    EXPECT_EQ(TR_PORT_UNMAPPED, pulse());
    EXPECT_TRUE(tr_upnpIsBusy(upnp_));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("discover") == 1U; }, Timeout));
    EXPECT_EQ(TR_PORT_UNMAPPED, pulse());
    EXPECT_EQ(0U, gateway_->completed());

    // the discover returns; the map that follows is held
    gateway_->allow(1);
    EXPECT_TRUE(waitFor([this] { return pulse() == TR_PORT_MAPPING; }, Timeout));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("add") == 1U; }, Timeout));
    EXPECT_EQ(TR_PORT_MAPPING, pulse());
    EXPECT_EQ(TR_PORT_MAPPING, pulse());
    EXPECT_EQ(1U, gateway_->completed()); // the discover

    gateway_->open_gate();
    mapped();

    // both protocols were asked for, from the right port to the right port
    auto const calls = gateway_->calls();
    ASSERT_EQ(3U, std::size(calls));
    EXPECT_EQ("TCP", calls[1].proto);
    EXPECT_EQ("UDP", calls[2].proto);
    for (auto const& call : { calls[1], calls[2] })
    {
        EXPECT_EQ(AdvertisedPort, call.advertised_port);
        EXPECT_EQ(LocalPort, call.local_port);
        EXPECT_EQ("Transmission at 51414", call.desc);
    }

    // mapped and idle: nothing more goes to the gateway
    EXPECT_EQ(TR_PORT_MAPPED, pulse());
    EXPECT_FALSE(tr_upnpIsBusy(upnp_));
    EXPECT_EQ(3U, std::size(gateway_->calls()));
}

TEST_F(PortForwardingUpnpTest, keepsDiscoveringWhileNoGatewayAnswers)
{
    gateway_->discover_answer.reset();

    EXPECT_TRUE(waitFor([this] { return pulse() == TR_PORT_UNMAPPED && gateway_->count("discover") >= 3U; }, Timeout));
    EXPECT_EQ(0U, gateway_->count("add"));

    // a gateway shows up: the next discovery finds it and the mapping follows
    gateway_->discover_answer = FakeGateway::Igd{ .control_url = "http://192.0.2.1/ctl", .service_type = "svc", .lanaddr = "192.0.2.2" };
    EXPECT_TRUE(pulseUntil(TR_PORT_MAPPED));
}

TEST_F(PortForwardingUpnpTest, remapsWhenThePortChanges)
{
    mapped();

    auto const new_advertised = tr_port::from_host(60000);
    auto const new_local = tr_port::from_host(60001);

    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(true, false, new_advertised, new_local));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 1U; }, Timeout));
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(true, false, new_advertised, new_local));
    gateway_->open_gate();

    EXPECT_TRUE(pulseUntil(TR_PORT_MAPPED, true, new_advertised, new_local));

    auto const calls = gateway_->calls();
    ASSERT_EQ(7U, std::size(calls)); // discover, add x2, delete x2, add x2
    EXPECT_EQ("delete", calls[3].name);
    EXPECT_EQ(AdvertisedPort, calls[3].advertised_port);
    EXPECT_EQ("delete", calls[4].name);
    EXPECT_EQ(AdvertisedPort, calls[4].advertised_port);
    EXPECT_EQ("add", calls[5].name);
    EXPECT_EQ(new_advertised, calls[5].advertised_port);
    EXPECT_EQ(new_local, calls[5].local_port);
    EXPECT_EQ("add", calls[6].name);
}

TEST_F(PortForwardingUpnpTest, unmapsWhenDisabled)
{
    mapped();

    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(false));
    EXPECT_TRUE(tr_upnpIsBusy(upnp_));
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(false));
    gateway_->open_gate();

    EXPECT_TRUE(pulseUntil(TR_PORT_UNMAPPED, false));
    EXPECT_EQ(2U, gateway_->count("delete"));

    // disabled and unmapped: the gateway is left alone
    EXPECT_EQ(TR_PORT_UNMAPPED, pulse(false));
    EXPECT_EQ(5U, std::size(gateway_->calls()));
}

TEST_F(PortForwardingUpnpTest, portCheckStaysMappedUntilTheGatewayAnswers)
{
    mapped();

    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_MAPPED, pulse(true, true));
    EXPECT_TRUE(tr_upnpIsBusy(upnp_));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("has") == 1U; }, Timeout));

    // a second check request while one is running does not start another
    EXPECT_EQ(TR_PORT_MAPPED, pulse(true, true));
    gateway_->open_gate();
    EXPECT_TRUE(waitFor([this] { return pulse() == TR_PORT_MAPPED && !tr_upnpIsBusy(upnp_); }, Timeout));
    EXPECT_EQ(2U, gateway_->count("has"));
    EXPECT_EQ(2U, gateway_->count("add"));
}

TEST_F(PortForwardingUpnpTest, portCheckThatFailsRemaps)
{
    mapped();

    gateway_->has_answer = false;
    EXPECT_EQ(TR_PORT_MAPPED, pulse(true, true));
    EXPECT_TRUE(waitFor([this] { return pulse() == TR_PORT_MAPPING; }, Timeout));
    EXPECT_TRUE(pulseUntil(TR_PORT_MAPPED));
    EXPECT_EQ(4U, gateway_->count("add"));
}

TEST_F(PortForwardingUpnpTest, mappingRefusedRetries)
{
    gateway_->add_answer = false;
    EXPECT_TRUE(waitFor([this] { return pulse() != TR_PORT_MAPPED && gateway_->count("add") >= 4U; }, Timeout));

    gateway_->add_answer = true;
    EXPECT_TRUE(pulseUntil(TR_PORT_MAPPED));
}

TEST_F(PortForwardingUpnpTest, closeWithoutWaitingLeavesTheUnmapToTheWorker)
{
    mapped();

    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(false));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 1U; }, Timeout));

    close(false);
    EXPECT_LT(close_took_, PulseBudget);

    gateway_->open_gate();
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 2U && gateway_->completed() == 5U; }, Timeout));
}

TEST_F(PortForwardingUpnpTest, closeWhileMappingUnmapsWhatTheJobMapped)
{
    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_UNMAPPED, pulse());
    gateway_->allow(1); // the discover
    EXPECT_TRUE(waitFor([this] { return pulse() == TR_PORT_MAPPING; }, Timeout));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("add") == 1U; }, Timeout));

    // the mapping is still being created; closing must not block and
    // must not leave it behind once it exists
    EXPECT_EQ(TR_PORT_MAPPING, pulse(false));
    close(false);
    EXPECT_LT(close_took_, PulseBudget);

    gateway_->open_gate();
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 2U; }, Timeout));
    auto const calls = gateway_->calls();
    EXPECT_EQ(AdvertisedPort, calls.back().advertised_port);
}

TEST_F(PortForwardingUpnpTest, closeWaitingUnmapsBeforeReturning)
{
    mapped();

    // close straight from mapped, e.g. shut down during a port check
    gateway_->has_answer = true;
    EXPECT_EQ(TR_PORT_MAPPED, pulse(true, true));
    EXPECT_TRUE(tr_upnpIsBusy(upnp_));

    close(true);
    EXPECT_EQ(2U, gateway_->count("delete"));
    EXPECT_GE(gateway_->completed(), 5U);
}

TEST_F(PortForwardingUpnpTest, closeWaitingGivesUpOnADeadGateway)
{
    mapped();

    gateway_->close_gate();
    EXPECT_EQ(TR_PORT_UNMAPPING, pulse(false));
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 1U; }, Timeout));

    // bounded: the wait ends without the gateway and the worker finishes later
    close(true);
    EXPECT_GE(close_took_, 1s);
    EXPECT_LT(close_took_, Timeout);
    EXPECT_EQ(1U, gateway_->count("delete"));

    gateway_->open_gate();
    EXPECT_TRUE(waitFor([this] { return gateway_->count("delete") == 2U; }, Timeout));
}
} // namespace
