#ifndef slic3r_CollabServer_hpp_
#define slic3r_CollabServer_hpp_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace Slic3r { namespace GUI { namespace Collab {

// Embedded WebSocket server run by the session host. Accepts multiple guest
// connections and relays text messages. All callbacks are invoked on the
// server's internal io thread; the receiver is responsible for marshalling
// to the UI thread.
class CollabServer
{
public:
    using MessageCallback    = std::function<void(int client_id, const std::string &message)>;
    using DisconnectCallback = std::function<void(int client_id)>;

    CollabServer()  = default;
    ~CollabServer() { stop(); }

    CollabServer(const CollabServer &)            = delete;
    CollabServer &operator=(const CollabServer &) = delete;

    void set_message_callback(MessageCallback cb) { m_on_message = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { m_on_disconnect = std::move(cb); }

    // Binds the first free port in [port, port + range] and starts the io
    // thread. Returns the bound port, or 0 on failure.
    unsigned short start(unsigned short port, unsigned short range);
    void stop();
    bool is_running() const { return m_running.load(); }

    void send_to(int client_id, const std::string &message);
    // Broadcasts to all connected clients, optionally skipping one.
    void broadcast(const std::string &message, int except_client_id = -1);
    void close_client(int client_id);

private:
    class Peer;
    friend class Peer;

    void accept_next();
    void remove_peer(int client_id);

    std::unique_ptr<boost::asio::io_context>        m_ioc;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    std::thread                                     m_io_thread;
    std::atomic<bool>                               m_running { false };

    std::mutex                             m_peers_mutex;
    std::map<int, std::shared_ptr<Peer>>   m_peers;
    int                                    m_next_client_id = 1;

    MessageCallback    m_on_message;
    DisconnectCallback m_on_disconnect;
};

}}} // namespace Slic3r::GUI::Collab

#endif // slic3r_CollabServer_hpp_
