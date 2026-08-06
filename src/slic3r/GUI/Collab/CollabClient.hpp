#ifndef slic3r_CollabClient_hpp_
#define slic3r_CollabClient_hpp_

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace Slic3r { namespace GUI { namespace Collab {

// WebSocket client used by session guests to connect to the host. Callbacks
// are invoked on the client's internal io thread; the receiver is responsible
// for marshalling to the UI thread.
class CollabClient
{
public:
    using MessageCallback    = std::function<void(const std::string &message)>;
    using ConnectCallback    = std::function<void(bool success, const std::string &error)>;
    using DisconnectCallback = std::function<void()>;

    CollabClient()  = default;
    ~CollabClient() { disconnect(); }

    CollabClient(const CollabClient &)            = delete;
    CollabClient &operator=(const CollabClient &) = delete;

    void set_message_callback(MessageCallback cb) { m_on_message = std::move(cb); }
    void set_connect_callback(ConnectCallback cb) { m_on_connect = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { m_on_disconnect = std::move(cb); }

    // Starts the io thread and connects asynchronously; the outcome is
    // reported through the connect callback.
    void connect(const std::string &host, unsigned short port);
    void disconnect();
    bool is_connected() const { return m_connected.load(); }

    void send(const std::string &message);

private:
    void read_next();
    void write_next();
    void handle_disconnect();

    std::unique_ptr<boost::asio::io_context> m_ioc;
    std::unique_ptr<boost::beast::websocket::stream<boost::asio::ip::tcp::socket>> m_ws;
    std::thread       m_io_thread;
    std::atomic<bool> m_connected { false };
    std::atomic<bool> m_stopping { false };

    boost::beast::flat_buffer m_buffer;
    std::deque<std::string>   m_send_queue;

    MessageCallback    m_on_message;
    ConnectCallback    m_on_connect;
    DisconnectCallback m_on_disconnect;
};

}}} // namespace Slic3r::GUI::Collab

#endif // slic3r_CollabClient_hpp_
