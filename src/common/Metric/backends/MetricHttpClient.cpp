/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "MetricHttpClient.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>

namespace Acore::MetricHttp
{
bool SendRequest(
    char const* backendName,
    std::string const& hostname,
    std::string const& port,
    std::string const& target,
    std::string const& body,
    std::vector<Header> const& headers,
    unsigned int expectedStatusCode)
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    using tcp = net::ip::tcp;

    try
    {
        net::io_context ioContext;
        tcp::resolver resolver(ioContext);
        beast::tcp_stream stream(ioContext);

        stream.connect(resolver.resolve(hostname, port));

        http::request<http::string_body> request(http::verb::post, target, 11);
        request.set(http::field::host, hostname + ':' + port);
        request.set(http::field::user_agent, "AzerothCore");
        request.set(http::field::accept, "*/*");
        request.set(http::field::content_type, "application/octet-stream");
        request.set("Content-Transfer-Encoding", "binary");

        for (Header const& header : headers)
            request.set(header.first, header.second);

        request.body() = body;
        request.prepare_payload();

        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(stream, buffer, response);

        boost::system::error_code shutdownError;
        stream.socket().shutdown(tcp::socket::shutdown_both, shutdownError);

        if (response.result_int() != expectedStatusCode)
        {
            LOG_ERROR("metric", "Error sending data to {}, returned HTTP code: {}. Body: {}",
                backendName, response.result_int(), response.body());
            return false;
        }

        return true;
    }
    catch (std::exception const& error)
    {
        LOG_ERROR("metric", "Error sending data to '{}:{}' for {}, disabling Metric. Error message: {}",
            hostname, port, backendName, error.what());
        return false;
    }
}
}
