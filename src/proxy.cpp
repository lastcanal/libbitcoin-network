/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
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
#include <bitcoin/network/proxy.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <boost/iostreams/stream.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/network/shared_const_buffer.hpp>

namespace libbitcoin {
namespace network {

#define NAME "proxy"

using namespace message;
using std::placeholders::_1;
using std::placeholders::_2;

// TODO: this is made up, configure payload size guard for DoS protection.
static constexpr size_t max_payload_size = 10 * 1024 * 1024;

// BUGBUG: socket::cancel fails with error::operation_not_supported
// on Windows XP and Windows Server 2003, but handler invocation is required.
// We should enable BOOST_ASIO_ENABLE_CANCELIO and BOOST_ASIO_DISABLE_IOCP
// on these platforms only. See: bit.ly/1YC0p9e
void proxy::close(asio::socket_ptr socket)
{
    // Ignoring socket error codes creates exception safety.
    boost_code ignore;
    socket->shutdown(asio::socket::shutdown_both, ignore);
    socket->cancel(ignore);
}

// Cache the address for logging after stop.
config::authority proxy::authority_factory(asio::socket_ptr socket)
{
    boost_code ec;
    const auto endpoint = socket->remote_endpoint(ec);
    return ec ? config::authority() : config::authority(endpoint);
}

proxy::proxy(threadpool& pool, asio::socket_ptr socket, uint32_t magic)
  : stopped_(true),
    magic_(magic),
    authority_(authority_factory(socket)),
    socket_(socket),
    message_subscriber_(pool),
    stop_subscriber_(std::make_shared<stop_subscriber>(pool, NAME "_sub"))
{
}

proxy::~proxy()
{
    BITCOIN_ASSERT_MSG(stopped(), "The channel was not stopped.");
}

// Properties.
// ----------------------------------------------------------------------------

// public:
const config::authority& proxy::authority() const
{
    return authority_;
}

// Start sequence.
// ----------------------------------------------------------------------------

// public:
void proxy::start(result_handler handler)
{
    if (!stopped())
    {
        handler(error::operation_failed);
        return;
    }

    stopped_ = false;
    stop_subscriber_->start();
    message_subscriber_.start();

    // Allow for subscription before first read, so no messages are missed.
    handler(error::success);

    // Start the read cycle.
    read_heading();
}

// Stop sequence.
// ----------------------------------------------------------------------------

// public:
void proxy::stop(const code& ec)
{
    BITCOIN_ASSERT_MSG(ec, "The stop code must be an error code.");

    // Stop is thread safe and idempotent, allows subscription to be unguarded.
    if (stopped())
        return;

    stopped_ = true;

    // This prevents resubscription after stop is relayed.
    message_subscriber_.stop();

    // This fires all message subscriptions with the channel_stopped code.
    message_subscriber_.broadcast(error::channel_stopped);

    // This prevents resubscription after stop is relayed.
    stop_subscriber_->stop();

    // This fires all stop subscriptions with the channel stop reason code.
    stop_subscriber_->relay(ec);

    // Give channel opportunity to terminate timers.
    handle_stopping();

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);

    // The socket_ must be guarded against concurrent use.
    proxy::close(socket_);
    ///////////////////////////////////////////////////////////////////////////
}

void proxy::stop(const boost_code& ec)
{
    stop(error::boost_to_error_code(ec));
}

bool proxy::stopped() const
{
    return stopped_;
}

// Stop subscription sequence.
// ----------------------------------------------------------------------------

// public:
void proxy::subscribe_stop(result_handler handler)
{
    stop_subscriber_->subscribe(handler, error::channel_stopped);
}

// Read cycle (read continues until stop).
// ----------------------------------------------------------------------------

void proxy::read_heading()
{
    if (stopped())
        return;

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);

    // The socket_ must be guarded against concurrent use.
    using namespace boost::asio;
    async_read(*socket_, buffer(heading_buffer_),
        std::bind(&proxy::handle_read_heading,
            shared_from_this(), _1, _2));
    ///////////////////////////////////////////////////////////////////////////
}

void proxy::handle_read_heading(const boost_code& ec, size_t)
{
    if (stopped())
        return;

    if (ec)
    {
        log::debug(LOG_NETWORK)
            << "Heading read failure [" << authority() << "] "
            << code(error::boost_to_error_code(ec)).message();
        stop(ec);
        return;
    }

    heading head;
    heading_stream istream(heading_buffer_);
    const auto parsed = head.from_data(istream);

    if (!parsed || head.magic != magic_)
    {
        log::warning(LOG_NETWORK) 
            << "Invalid heading from [" << authority() << "]";
        stop(error::bad_stream);
        return;
    }

    if (head.payload_size > max_payload_size)
    {
        log::warning(LOG_NETWORK)
            << "Oversized payload indicated by " << head.command
            << " heading from [" << authority() << "] ("
            << head.payload_size << " bytes)";
        stop(error::bad_stream);
        return;
    }

    ////log::debug(LOG_NETWORK)
    ////    << "Valid " << head.command << " heading from ["
    ////    << authority() << "] (" << head.payload_size << " bytes)";

    read_payload(head);
    handle_activity();
}

void proxy::read_payload(const heading& head)
{
    if (stopped())
        return;

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);

    payload_buffer_.resize(head.payload_size);

    // The socket_ must be guarded against concurrent use.
    using namespace boost::asio;
    async_read(*socket_, buffer(payload_buffer_, head.payload_size),
        std::bind(&proxy::handle_read_payload,
            shared_from_this(), _1, _2, head));
    ///////////////////////////////////////////////////////////////////////////
}

void proxy::handle_read_payload(const boost_code& ec, size_t,
    const heading& head)
{
    if (stopped())
        return;

    // Ignore read error here, client may have disconnected.

    if (head.checksum != bitcoin_checksum(payload_buffer_))
    {
        log::warning(LOG_NETWORK) 
            << "Invalid " << head.command << " checksum from ["
            << authority() << "]";
        stop(error::bad_stream);
        return;
    }

    // Parse and publish the payload to message subscribers.
    payload_source source(payload_buffer_);
    payload_stream istream(source);

    // Notify subscribers of the new message.
    const auto parse_error = message_subscriber_.load(head.type(), istream);
    const auto unconsumed = istream.peek() != std::istream::traits_type::eof();

    if (stopped())
        return;

    if (!parse_error)
    {
        if (unconsumed)
            log::warning(LOG_NETWORK)
            << "Valid " << head.command << " payload from ["
                << authority() << "] unused bytes remain.";
        else
            log::debug(LOG_NETWORK)
            << "Valid " << head.command << " payload from ["
                << authority() << "] (" << payload_buffer_.size() << " bytes)";
    }

    if (ec)
    {
        log::warning(LOG_NETWORK)
            << "Payload read failure [" << authority() << "] "
            << code(error::boost_to_error_code(ec)).message();
        stop(ec);
        return;
    }

    if (parse_error)
    {
        log::warning(LOG_NETWORK)
            << "Invalid " << head.command << " stream from ["
            << authority() << "] " << parse_error.message();
        stop(parse_error);
    }

    handle_activity();
    read_heading();
}

// Message send sequence.
// ----------------------------------------------------------------------------

void proxy::do_send(const data_chunk& message, result_handler handler,
    const std::string& command)
{
    if (stopped())
    {
        handler(error::channel_stopped);
        return;
    }

    log::debug(LOG_NETWORK)
        << "Sending " << command << " to [" << authority() << "] ("
        << message.size() << " bytes)";

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);

    // The socket_ must be guarded against concurrent use.
    const shared_const_buffer buffer(message);
    async_write(*socket_, buffer,
        std::bind(&proxy::handle_send,
            shared_from_this(), _1, handler));
    ///////////////////////////////////////////////////////////////////////////
}

void proxy::handle_send(const boost_code& ec, result_handler handler)
{
    const auto error = code(error::boost_to_error_code(ec));

    if (error)
        log::debug(LOG_NETWORK)
            << "Failure sending message to [" << authority() << "] "
            << error.message();

    handler(error);
}

} // namespace network
} // namespace libbitcoin