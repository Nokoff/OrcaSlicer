#include "CollabServer.hpp"

#include <deque>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace Collab {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// Maximum size of a single WebSocket message. Project transfers (base64 3MF)
// can be large.
static constexpr std::size_t MAX_MESSAGE_SIZE = 512ull * 1024ull * 1024ull;

// One connected guest. Owns the WebSocket stream; all of its operations run
// on the server io thread (single-threaded io_context, no strand needed).
class CollabServer::Peer : public std::enable_shared_from_this<CollabServer::Peer>
{
public:
    Peer(CollabServer &server, int client_id, tcp::socket socket)
        : m_server(server), m_client_id(client_id), m_ws(std::move(socket))
    {}

    int id() const { return m_client_id; }

    void start()
    {
        m_ws.read_message_max(MAX_MESSAGE_SIZE);
        m_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        auto self = shared_from_this();
        m_ws.async_accept([self](beast::error_code ec) {
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id
                                           << " websocket handshake failed: " << ec.message();
                self->m_server.remove_peer(self->m_client_id);
                return;
            }
            BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id << " websocket handshake OK";
            self->read_next();
        });
    }

    void send(const std::string &message)
    {
        if (m_closing)
            return;
        m_send_queue.push_back(message);
        if (m_send_queue.size() == 1)
            write_next();
    }

    // Graceful close: let whatever is already queued (typically the Error
    // message explaining a rejected handshake) reach the guest before the
    // socket goes away. A bare close() here discards it, leaving the guest
    // with an unexplained disconnect.
    void close_after_flush()
    {
        if (m_send_queue.empty())
            close();
        else
            m_closing = true;
    }

    void close()
    {
        beast::error_code ec;
        m_ws.next_layer().shutdown(tcp::socket::shutdown_both, ec);
        m_ws.next_layer().close(ec);
    }

private:
    void read_next()
    {
        auto self = shared_from_this();
        m_ws.async_read(m_buffer, [self](beast::error_code ec, std::size_t) {
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id
                                           << " read ended: " << ec.message();
                self->m_server.remove_peer(self->m_client_id);
                return;
            }
            std::string message = beast::buffers_to_string(self->m_buffer.data());
            BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id << " received "
                                       << message.size() << " bytes";
            self->m_buffer.consume(self->m_buffer.size());
            if (self->m_server.m_on_message)
                self->m_server.m_on_message(self->m_client_id, message);
            self->read_next();
        });
    }

    void write_next()
    {
        auto self = shared_from_this();
        m_ws.text(true);
        m_ws.async_write(net::buffer(m_send_queue.front()), [self](beast::error_code ec, std::size_t bytes) {
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id
                                           << " write failed: " << ec.message();
                self->m_server.remove_peer(self->m_client_id);
                return;
            }
            BOOST_LOG_TRIVIAL(warning) << "CollabServer: peer " << self->m_client_id << " sent " << bytes << " bytes";
            self->m_send_queue.pop_front();
            if (!self->m_send_queue.empty())
                self->write_next();
            else if (self->m_closing)
                self->close();
        });
    }

    CollabServer                  &m_server;
    int                            m_client_id;
    websocket::stream<tcp::socket> m_ws;
    beast::flat_buffer             m_buffer;
    std::deque<std::string>        m_send_queue;
    bool                           m_closing = false;
};

unsigned short CollabServer::start(unsigned short port, unsigned short range)
{
    if (m_running.load())
        return 0;

    m_ioc = std::make_unique<net::io_context>();

    unsigned short bound_port = 0;
    for (unsigned short p = port; p <= port + range; ++p) {
        try {
            m_acceptor = std::make_unique<tcp::acceptor>(*m_ioc, tcp::endpoint(tcp::v4(), p));
            bound_port = p;
            break;
        } catch (const std::exception &) {
            m_acceptor.reset();
        }
    }
    if (bound_port == 0) {
        BOOST_LOG_TRIVIAL(error) << "CollabServer: no free port in range " << port << "-" << (port + range);
        m_ioc.reset();
        return 0;
    }

    m_running.store(true);
    accept_next();
    m_io_thread = std::thread([this]() {
        try {
            m_ioc->run();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "CollabServer io thread exception: " << e.what();
        }
    });

    BOOST_LOG_TRIVIAL(warning) << "CollabServer: listening on port " << bound_port;
    return bound_port;
}

void CollabServer::stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_ioc) {
        net::post(*m_ioc, [this]() {
            boost::system::error_code ec;
            if (m_acceptor)
                m_acceptor->close(ec);
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto &peer : m_peers)
                peer.second->close();
        });
    }
    if (m_io_thread.joinable())
        m_io_thread.join();

    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        m_peers.clear();
    }
    m_acceptor.reset();
    m_ioc.reset();
    BOOST_LOG_TRIVIAL(info) << "CollabServer: stopped";
}

void CollabServer::accept_next()
{
    m_acceptor->async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (ec) {
            if (m_running.load())
                BOOST_LOG_TRIVIAL(warning) << "CollabServer: accept failed: " << ec.message();
            return;
        }
        std::string remote;
        try {
            remote = socket.remote_endpoint().address().to_string();
        } catch (const std::exception &) {}

        int client_id;
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            client_id = m_next_client_id++;
            peer      = std::make_shared<Peer>(*this, client_id, std::move(socket));
            m_peers.emplace(client_id, peer);
        }
        BOOST_LOG_TRIVIAL(warning) << "CollabServer: TCP accepted peer " << client_id << " from " << remote;
        peer->start();
        accept_next();
    });
}

void CollabServer::remove_peer(int client_id)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        removed = m_peers.erase(client_id) > 0;
    }
    if (removed && m_on_disconnect)
        m_on_disconnect(client_id);
}

void CollabServer::send_to(int client_id, const std::string &message)
{
    if (!m_running.load() || m_ioc == nullptr)
        return;
    net::post(*m_ioc, [this, client_id, message]() {
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            auto it = m_peers.find(client_id);
            if (it != m_peers.end())
                peer = it->second;
        }
        if (peer)
            peer->send(message);
    });
}

void CollabServer::broadcast(const std::string &message, int except_client_id)
{
    if (!m_running.load() || m_ioc == nullptr)
        return;
    net::post(*m_ioc, [this, message, except_client_id]() {
        std::vector<std::shared_ptr<Peer>> peers;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            for (auto &entry : m_peers)
                if (entry.first != except_client_id)
                    peers.push_back(entry.second);
        }
        for (auto &peer : peers)
            peer->send(message);
    });
}

void CollabServer::close_client(int client_id)
{
    if (!m_running.load() || m_ioc == nullptr)
        return;
    net::post(*m_ioc, [this, client_id]() {
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(m_peers_mutex);
            auto it = m_peers.find(client_id);
            if (it != m_peers.end())
                peer = it->second;
        }
        if (peer)
            peer->close_after_flush();
    });
}

}}} // namespace Slic3r::GUI::Collab
