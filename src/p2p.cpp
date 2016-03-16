/**
 * Copyright (c) 2011-2018 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/network/p2p.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/network/channel.hpp>
#include <bitcoin/network/connections.hpp>
#include <bitcoin/network/hosts.hpp>
#include <bitcoin/network/protocols/protocol_address.hpp>
#include <bitcoin/network/protocols/protocol_ping.hpp>
#include <bitcoin/network/protocols/protocol_seed.hpp>
#include <bitcoin/network/protocols/protocol_version.hpp>
#include <bitcoin/network/sessions/session_inbound.hpp>
#include <bitcoin/network/sessions/session_manual.hpp>
#include <bitcoin/network/sessions/session_outbound.hpp>
#include <bitcoin/network/sessions/session_seed.hpp>
#include <bitcoin/network/settings.hpp>

namespace libbitcoin {
namespace network {

#define NAME "p2p"

using std::placeholders::_1;

// No-operation handler, used in default stop handling.
p2p::result_handler p2p::unhandled = [](code){};

p2p::p2p(const settings& settings)
  : stopped_(true),
    height_(0),
    settings_(settings),
    dispatch_(threadpool_, NAME),
    hosts_(threadpool_, settings_),
    connections_(std::make_shared<connections>(threadpool_)),
    subscriber_(std::make_shared<channel_subscriber>(threadpool_, NAME "_sub"))
{
}

// Properties.
// ----------------------------------------------------------------------------

const settings& p2p::network_settings() const
{
    return settings_;
}

// The blockchain height is set in our version message for handshake.
size_t p2p::height() const
{
    return height_;
}

// The height is set externally and is safe as an atomic.
void p2p::set_height(size_t value)
{
    height_ = value;
}

bool p2p::stopped() const
{
    return stopped_;
}

threadpool& p2p::thread_pool()
{
    return threadpool_;
}

// Start sequence.
// ----------------------------------------------------------------------------

void p2p::start(result_handler handler)
{
    if (!stopped())
    {
        handler(error::operation_failed);
        return;
    }

    stopped_ = false;
    subscriber_->start();

    threadpool_.join();
    threadpool_.spawn(settings_.threads, thread_priority::low);

    const auto manual_started_handler =
        std::bind(&p2p::handle_manual_started,
            this, _1, handler);

    // This instance is retained by stop handler and member references.
    auto manual = attach<session_manual>();
    manual->start(manual_started_handler);
    manual_.store(manual);

    ////handle_manual_started(error::success, handler);
}

void p2p::handle_manual_started(const code& ec, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        log::error(LOG_NETWORK)
            << "Error starting manual session: " << ec.message();
        handler(ec);
        return;
    }

    handle_hosts_loaded(hosts_.load(), handler);
}

void p2p::handle_hosts_loaded(const code& ec, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        log::error(LOG_NETWORK)
            << "Error loading host addresses: " << ec.message();
        handler(ec);
        return;
    }

    // The instance is retained by the stop handler (until shutdown).
    attach<session_seed>()->start(
        std::bind(&p2p::handle_hosts_seeded,
            this, _1, handler));
}

void p2p::handle_hosts_seeded(const code& ec, result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        log::error(LOG_NETWORK)
            << "Error seeding host addresses: " << ec.message();
        handler(ec);
        return;
    }

    // There is no way to guarantee subscription before handler execution.
    // So currently subscription for seed node connections is not supported.
    // Subscription after this return will capture connections established via
    // subsequent "run" and "connect" calls, and will clear on close/destruct.

    // This is the end of the start sequence.
    handler(error::success);
}

// Run sequence.
// ----------------------------------------------------------------------------

void p2p::run(result_handler handler)
{
    // This instance is retained by the stop handler (until shutdown).
    attach<session_inbound>()->start(
        std::bind(&p2p::handle_inbound_started,
            this, _1, handler));

    ////handle_inbound_started(error::success, handler);
}

void p2p::handle_inbound_started(const code& ec, result_handler handler)
{
    if (ec)
    {
        log::error(LOG_NETWORK)
            << "Error starting inbound session: " << ec.message();
        handler(ec);
        return;
    }

    // This instance is retained by the stop handler (until shutdown).
    attach<session_outbound>()->start(
        std::bind(&p2p::handle_outbound_started,
            this, _1, handler));
}

void p2p::handle_outbound_started(const code& ec, result_handler handler)
{
    if (ec)
    {
        log::error(LOG_NETWORK)
            << "Error starting outbound session: " << ec.message();
        handler(ec);
        return;
    }

    // This is the end of the run sequence.
    handler(error::success);
}

// Channel subscription.
// ----------------------------------------------------------------------------

void p2p::subscribe_connections(connect_handler handler)
{
    subscriber_->subscribe(handler, error::service_stopped, nullptr);
}

// Manual connections.
// ----------------------------------------------------------------------------

void p2p::connect(const std::string& hostname, uint16_t port)
{
    if (stopped())
        return;

    auto manual = manual_.load();
    if (manual)
        manual->connect(hostname, port);
}

void p2p::connect(const std::string& hostname, uint16_t port,
    channel_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr);
        return;
    }

    auto manual = manual_.load();
    if (manual)
        manual->connect(hostname, port, handler);
}

// Stop sequence.
// ----------------------------------------------------------------------------
// All shutdown actions must be queued by the end of the stop call.
// IOW queued shutdown operations must not enqueue additional work.

void p2p::stop(result_handler handler)
{
    // Stop is thread safe and idempotent, allows subscription to be unguarded.

    // Prevent subscription after stop.
    subscriber_->stop();
    subscriber_->relay(error::service_stopped, nullptr);

    // Must be after subscriber stop (why?).
    connections_->stop(error::service_stopped);
    manual_.store(nullptr);

    // Host save is expensive, so minimize repeats.
    const auto ec = stopped_ ? error::success : hosts_.save();
    stopped_ = true;

    if (ec)
        log::error(LOG_NETWORK)
            << "Error saving hosts file: " << ec.message();

    threadpool_.shutdown();

    // This is the end of the stop sequence.
    handler(ec);
}

// Destruct sequence.
// ----------------------------------------------------------------------------

p2p::~p2p()
{
    // A reference cycle cannot exist with this class, since we don't capture
    // shared pointers to it. Therefore this will always clear subscriptions.
    // This allows for shutdown based on destruct without need to call stop.
    p2p::close();
}

void p2p::close()
{
    p2p::stop(
        std::bind(&p2p::handle_stopped,
            this, _1));
}

void p2p::handle_stopped(const code&)
{
    // This is the end of the destruct sequence.
    threadpool_.join();
}

// Connections collection.
// ----------------------------------------------------------------------------

void p2p::connected(const address& address, truth_handler handler)
{
    connections_->exists(address, handler);
}

void p2p::store(channel::ptr channel, result_handler handler)
{
    const auto new_connection_handler =
        std::bind(&p2p::handle_new_connection,
            this, _1, channel, handler);

    connections_->store(channel, new_connection_handler);
}

void p2p::handle_new_connection(const code& ec, channel::ptr channel,
    result_handler handler)
{
    // Connection-in-use indicated here by error::address_in_use.
    handler(ec);
    
    if (!ec && channel->notify())
        subscriber_->relay(error::success, channel);
}

void p2p::remove(channel::ptr channel, result_handler handler)
{
    connections_->remove(channel, handler);
}

void p2p::connected_count(count_handler handler)
{
    connections_->count(handler);
}

// Hosts collection.
// ----------------------------------------------------------------------------

void p2p::fetch_address(address_handler handler)
{
    address out;
    handler(hosts_.fetch(out), out);
}

void p2p::store(const address& address, result_handler handler)
{
    handler(hosts_.store(address));
}

void p2p::store(const address::list& addresses, result_handler handler)
{
    hosts_.store(addresses, handler);
}

void p2p::remove(const address& address, result_handler handler)
{
    handler(hosts_.remove(address));
}

void p2p::address_count(count_handler handler)
{
    handler(hosts_.count());
}

} // namespace network
} // namespace libbitcoin