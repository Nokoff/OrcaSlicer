#include "CollabClient.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace Collab {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

static constexpr std::size_t MAX_MESSAGE_SIZE = 512ull * 1024ull * 1024ull;

void CollabClient::connect(const std::string &host, unsigned short port)
{
    if (m_ioc != nullptr)
        return;

    m_stopping.store(false);
    m_ioc = std::make_unique<net::io_context>();
    m_ws  = std::make_unique<websocket::stream<tcp::socket>>(*m_ioc);

    m_io_thread = std::thread([this, host, port]() {
        try {
            tcp::resolver resolver(*m_ioc);
            const auto results = resolver.resolve(host, std::to_string(port));
            net::connect(m_ws->next_layer(), results);

            m_ws->read_message_max(MAX_MESSAGE_SIZE);
            m_ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
            m_ws->set_option(websocket::stream_base::decorator([](websocket::request_type &req) {
                req.set(beast::http::field::user_agent, "OrcaSlicer-Collab");
            }));
            m_ws->handshake(host + ":" + std::to_string(port), "/");

            m_connected.store(true);
            if (m_on_connect)
                m_on_connect(true, std::string());

            read_next();
            m_ioc->run();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "CollabClient: connection failed: " << e.what();
            if (!m_connected.load()) {
                if (m_on_connect)
                    m_on_connect(false, e.what());
            } else {
                handle_disconnect();
            }
        }
    });
}

void CollabClient::disconnect()
{
    if (m_ioc == nullptr)
        return;

    m_stopping.store(true);
    net::post(*m_ioc, [this]() {
        if (m_ws != nullptr) {
            beast::error_code ec;
            m_ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
            m_ws->next_layer().close(ec);
        }
    });
    m_ioc->stop();
    if (m_io_thread.joinable())
        m_io_thread.join();

    m_connected.store(false);
    m_send_queue.clear();
    m_ws.reset();
    m_ioc.reset();
}

void CollabClient::send(const std::string &message)
{
    if (m_ioc == nullptr || !m_connected.load())
        return;
    net::post(*m_ioc, [this, message]() {
        m_send_queue.push_back(message);
        if (m_send_queue.size() == 1)
            write_next();
    });
}

void CollabClient::read_next()
{
    m_ws->async_read(m_buffer, [this](beast::error_code ec, std::size_t) {
        if (ec) {
            handle_disconnect();
            return;
        }
        std::string message = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(m_buffer.size());
        if (m_on_message)
            m_on_message(message);
        read_next();
    });
}

void CollabClient::write_next()
{
    m_ws->text(true);
    m_ws->async_write(net::buffer(m_send_queue.front()), [this](beast::error_code ec, std::size_t) {
        if (ec) {
            handle_disconnect();
            return;
        }
        m_send_queue.pop_front();
        if (!m_send_queue.empty())
            write_next();
    });
}

void CollabClient::handle_disconnect()
{
    const bool was_connected = m_connected.exchange(false);
    if (was_connected && !m_stopping.load() && m_on_disconnect)
        m_on_disconnect();
}

}}} // namespace Slic3r::GUI::Collab
