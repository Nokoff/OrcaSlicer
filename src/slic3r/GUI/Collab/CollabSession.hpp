#ifndef slic3r_CollabSession_hpp_
#define slic3r_CollabSession_hpp_

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "CollabClient.hpp"
#include "CollabProtocol.hpp"
#include "CollabServer.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class ModelObject;
class ModelVolume;
class TriangleSelector;

namespace GUI { namespace Collab {

// A live collaborative painting session. One instance exists at a time and is
// owned by CollabSessionManager. All public methods must be called from the
// UI thread; network callbacks are marshalled onto the UI thread internally.
class CollabSession : public std::enable_shared_from_this<CollabSession>
{
public:
    enum class Role { Host, Guest };

    struct User
    {
        int         id = -1;
        std::string name;
        ColorRGBA   color;
    };

    struct RemoteCursor
    {
        int         user_id = -1;
        std::string name;
        ColorRGBA   color;
        Vec3d       position = Vec3d::Zero();
        double      radius   = 1.;
        std::chrono::steady_clock::time_point last_update;
    };

    ~CollabSession();

    Role               role() const { return m_role; }
    const std::string &invite_link() const { return m_link; }
    int                my_user_id() const { return m_my_user_id; }
    std::vector<User>  users() const;
    // Human readable connection status for dialogs.
    std::string        status_text() const;

    // ---- Painting integration (called by the paint gizmos) ----

    // Called when a stroke is about to paint the given volume. Returns false
    // when the volume is locked by another user (the stroke must skip it).
    // On success, the volume is (optimistically) claimed until end_stroke().
    bool try_begin_paint(const VolumeKey &key);
    // Throttled mid-drag broadcast of the given selector's live state.
    void paint_progress(const VolumeKey &key, const TriangleSelector &selector);
    // Called on mouse-up after the gizmo stored the stroke into the model.
    void end_stroke();
    // Throttled broadcast of the local brush position (world coordinates).
    void send_cursor(const Vec3d &world_position, double radius);

    // Broadcasts every volume whose paint annotation changed since the last
    // sync, and restores remote users' paint reverted by a local undo/redo.
    // Called after strokes, after undo/redo and after "reset selection".
    void sync_paint_state();

    // Remote brush cursors to render (stale entries are pruned).
    std::vector<RemoteCursor> remote_cursors();

    // Returns the user holding a claim on the given volume, or nullptr.
    const User *claim_holder(const VolumeKey &key) const;

    // True while the received project is being loaded on a guest; used to
    // bypass the scene edit lock.
    bool is_applying_project() const { return m_applying_project; }

    // Deterministic per-user color used for cursors/claim highlighting.
    static ColorRGBA user_color_for_id(int user_id);

private:
    friend class CollabSessionManager;

    explicit CollabSession(Role role) : m_role(role) {}

    // Startup (called by the manager).
    bool start_hosting(const std::string &user_name, std::string &error);
    bool start_joining(const SessionLink &link, const std::string &user_name, std::string &error);
    // Graceful teardown; notify_peers sends bye before closing.
    void shutdown(bool notify_peers);

    // ---- Networking (UI thread) ----
    void handle_server_message(int client_id, const nlohmann::json &msg);
    void handle_server_disconnect(int client_id);
    void handle_client_message(const nlohmann::json &msg);
    void handle_client_disconnected();
    void handle_client_connected(bool success, const std::string &error);

    void send_to_host_or_broadcast(const nlohmann::json &msg, int except_client_id = -1);
    void send_paint_message(const VolumeKey &key, const std::string &encoded_data, bool final_state);
    void host_send_project(int client_id);

    // ---- Model integration (UI thread) ----
    void apply_remote_paint(const VolumeKey &key, const TriangleSelector::TriangleSplittingData &data, int author, bool final_state);
    void apply_project(const std::string &project_name, const std::string &data_b64);
    // Marks the current annotation state of every volume as synced (no-op broadcast baseline).
    void baseline_synced_timestamps();
    void refresh_volume_ui(const VolumeKey &key, bool final_state);
    // Reload the gizmo selector of the given volume from data (if the paint gizmo is open on it).
    void update_open_gizmo_volume(const VolumeKey &key, const TriangleSelector::TriangleSplittingData &data);
    ModelVolume *resolve_volume(const VolumeKey &key) const;

    // ---- Claims ----
    void host_grant_or_deny_claim(const VolumeKey &key, int user_id, int client_id);
    void release_claims_of_user(int user_id, bool broadcast_msg);
    void revert_volume_from_model(const VolumeKey &key);

    void add_user(const User &user);
    void remove_user(int user_id);
    void end_session_with_notice(const std::string &reason);

    Role        m_role;
    std::string m_token;
    std::string m_link;
    std::string m_my_name;
    int         m_my_user_id  = -1;
    int         m_next_user_id = 1;
    bool        m_active      = false;
    bool        m_applying_project = false;
    // shutdown() must stay idempotent without keying off m_active:
    // end_session_with_notice() clears m_active before the deferred stop()
    // runs, which would otherwise make shutdown() a no-op.
    bool        m_shutdown_done = false;

    std::unique_ptr<CollabServer> m_server; // host only
    std::unique_ptr<CollabClient> m_client; // guest only

    std::map<int, User> m_users;              // user_id -> user
    std::map<int, int>  m_client_to_user;     // host only: ws client id -> user_id
    std::map<int, int>  m_user_to_client;     // host only: user_id -> ws client id

    // Stroke locks.
    std::map<VolumeKey, int> m_claims;          // volume -> holder user id
    std::set<VolumeKey>      m_my_stroke_claims; // claims held (or requested) for the current stroke
    std::set<VolumeKey>      m_blocked_volumes;  // claim denied during the current stroke

    // Sync bookkeeping.
    int64_t                       m_seq_counter = 0;              // host only
    std::map<VolumeKey, int64_t>  m_last_applied_seq;
    std::map<VolumeKey, uint64_t> m_synced_ts;   // annotation timestamp at last send/apply
    std::map<VolumeKey, int>      m_last_author; // last user who painted the volume
    std::map<VolumeKey, TriangleSelector::TriangleSplittingData> m_remote_cache;

    std::map<int, RemoteCursor> m_remote_cursors;

    std::chrono::steady_clock::time_point m_last_progress_send;
    std::chrono::steady_clock::time_point m_last_cursor_send;
};

// Owner of the (single) active session.
class CollabSessionManager
{
public:
    // The active session, or nullptr.
    static CollabSession *get();
    static bool           is_active() { return get() != nullptr; }

    // Starts hosting; returns the session or nullptr (error is filled in).
    static CollabSession *start_hosting(const std::string &user_name, std::string &error);
    // Joins a session from an invite link.
    static CollabSession *join(const std::string &link_str, const std::string &user_name, std::string &error);
    // Ends the active session (host: terminates for everyone; guest: leaves).
    static void stop();

    // True when scene editing (anything except color painting) is locked.
    static bool scene_locked();
    // Same, but pops a user notification when locked. Returns scene_locked().
    static bool scene_locked_with_notice();

private:
    static std::shared_ptr<CollabSession> s_session;
    friend class CollabSession;
};

}}} // namespace Slic3r::GUI::Collab

#endif // slic3r_CollabSession_hpp_
