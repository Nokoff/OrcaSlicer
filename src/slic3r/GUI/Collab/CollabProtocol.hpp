#ifndef slic3r_CollabProtocol_hpp_
#define slic3r_CollabProtocol_hpp_

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r { namespace GUI { namespace Collab {

// Protocol version, bumped on incompatible wire format changes.
constexpr int PROTOCOL_VERSION = 1;

// Default TCP port for the embedded collaboration server. If busy, the host
// tries the following ports up to DEFAULT_PORT + PORT_SEARCH_RANGE.
constexpr unsigned short DEFAULT_PORT      = 14700;
constexpr unsigned short PORT_SEARCH_RANGE = 20;

// Message types exchanged between host and guests. All messages are JSON
// objects with a "type" field; binary payloads are base64-encoded strings.
namespace MsgType {
    // guest -> host: authentication + user info. {token, name, version}
    constexpr const char *Hello = "hello";
    // host -> guest: accepted. {user_id, users:[{id,name,color}]}
    constexpr const char *Welcome = "welcome";
    // host -> guest: refused / protocol error. {message}
    constexpr const char *Error = "error";
    // host -> guest: full project as 3MF. {name, data}
    constexpr const char *Project = "project";
    // both directions: full paint state of one volume.
    // {obj, vol, seq, user, fin, data}
    constexpr const char *Paint = "paint";
    // guest -> host: request a stroke lock. {obj, vol}
    // host -> all: lock granted (also used as notification). {obj, vol, user}
    constexpr const char *Claim = "claim";
    // host -> guest: lock request refused. {obj, vol, holder}
    constexpr const char *ClaimDenied = "claim_denied";
    // both directions: stroke lock released. {obj, vol, user}
    constexpr const char *Release = "release";
    // both directions: brush cursor position in world coords. {user, x, y, z, r}
    constexpr const char *Cursor = "cursor";
    // host -> all: a user joined. {user:{id,name,color}}
    constexpr const char *Join = "join";
    // host -> all: a user left. {user}
    constexpr const char *Leave = "leave";
    // host -> all: session is being terminated.
    constexpr const char *Bye = "bye";
} // namespace MsgType

// Identifies a paintable volume: index of the ModelObject in Model::objects
// and index of the ModelVolume among the object's model-part volumes only
// (matching the triangle selector indices used by the paint gizmos).
struct VolumeKey
{
    int obj_idx = -1;
    int vol_idx = -1;

    bool operator<(const VolumeKey &rhs) const
    {
        return obj_idx != rhs.obj_idx ? obj_idx < rhs.obj_idx : vol_idx < rhs.vol_idx;
    }
    bool operator==(const VolumeKey &rhs) const { return obj_idx == rhs.obj_idx && vol_idx == rhs.vol_idx; }
};

// Session invite link handling: orca-collab://<host>:<port>/<token>
struct SessionLink
{
    std::string host;
    unsigned short port = 0;
    std::string token;
};

std::string format_link(const SessionLink &link);
std::optional<SessionLink> parse_link(const std::string &link_str);

// Generates a random hexadecimal session token.
std::string generate_token();

// Returns the local LAN IP address usable by peers on the same network,
// or "127.0.0.1" if it cannot be determined.
std::string get_lan_ip();

// base64 helpers (Boost.Beast implementation).
std::string base64_encode(const std::string &data);
std::string base64_decode(const std::string &data);

// Serialization of the compact per-volume paint state for the wire.
std::string encode_paint_data(const TriangleSelector::TriangleSplittingData &data);
std::optional<TriangleSelector::TriangleSplittingData> decode_paint_data(const std::string &encoded);

}}} // namespace Slic3r::GUI::Collab

#endif // slic3r_CollabProtocol_hpp_
