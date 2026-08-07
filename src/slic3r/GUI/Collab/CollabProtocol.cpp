#include "CollabProtocol.hpp"

#include <random>
#include <sstream>

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/log/trivial.hpp>

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

namespace Slic3r { namespace GUI { namespace Collab {

static constexpr const char *LINK_SCHEME = "orca-collab://";

std::string format_link(const SessionLink &link)
{
    return std::string(LINK_SCHEME) + link.host + ":" + std::to_string(link.port) + "/" + link.token;
}

std::optional<SessionLink> parse_link(const std::string &link_str)
{
    std::string s = link_str;
    // Trim whitespace.
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::nullopt;
    const auto last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);

    if (s.rfind(LINK_SCHEME, 0) != 0)
        return std::nullopt;
    s = s.substr(std::string(LINK_SCHEME).size());

    const auto slash = s.find('/');
    if (slash == std::string::npos)
        return std::nullopt;
    const std::string host_port = s.substr(0, slash);
    std::string token = s.substr(slash + 1);
    // Tolerate a trailing slash.
    if (!token.empty() && token.back() == '/')
        token.pop_back();

    // IPv6 addresses would contain multiple colons; use the last one as separator.
    const auto colon = host_port.rfind(':');
    if (colon == std::string::npos || token.empty())
        return std::nullopt;

    SessionLink link;
    link.host  = host_port.substr(0, colon);
    link.token = token;
    try {
        const int port = std::stoi(host_port.substr(colon + 1));
        if (port <= 0 || port > 65535)
            return std::nullopt;
        link.port = static_cast<unsigned short>(port);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    if (link.host.empty())
        return std::nullopt;
    return link;
}

std::string generate_token()
{
    static constexpr const char *hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string token(16, '0');
    for (char &c : token)
        c = hex[dist(gen)];
    return token;
}

std::string get_lan_ip()
{
    try {
        // Connecting a UDP socket does not send any packet; it just makes the
        // OS pick the outgoing interface, whose address is the LAN IP.
        boost::asio::io_context ioc;
        boost::asio::ip::udp::socket socket(ioc);
        socket.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("8.8.8.8"), 53));
        const auto address = socket.local_endpoint().address();
        socket.close();
        if (!address.is_unspecified() && !address.is_loopback())
            return address.to_string();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "CollabProtocol::get_lan_ip failed: " << e.what();
    }
    return "127.0.0.1";
}

std::string base64_encode(const std::string &data)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out;
    out.resize(b64::encoded_size(data.size()));
    out.resize(b64::encode(out.data(), data.data(), data.size()));
    return out;
}

std::string base64_decode(const std::string &data)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out;
    out.resize(b64::decoded_size(data.size()));
    const auto result = b64::decode(out.data(), data.data(), data.size());
    out.resize(result.first);
    return out;
}

std::string encode_paint_data(const TriangleSelector::TriangleSplittingData &data)
{
    std::ostringstream oss(std::ios::binary);
    {
        cereal::BinaryOutputArchive archive(oss);
        // TriangleSplittingData provides a cereal serialize() member.
        archive(const_cast<TriangleSelector::TriangleSplittingData &>(data));
    }
    return base64_encode(oss.str());
}

std::optional<TriangleSelector::TriangleSplittingData> decode_paint_data(const std::string &encoded)
{
    try {
        std::istringstream iss(base64_decode(encoded), std::ios::binary);
        cereal::BinaryInputArchive archive(iss);
        TriangleSelector::TriangleSplittingData data;
        archive(data);
        return data;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "CollabProtocol::decode_paint_data failed: " << e.what();
        return std::nullopt;
    }
}

}}} // namespace Slic3r::GUI::Collab
