#include "CollabSession.hpp"

#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/string.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/format.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoMmuSegmentation.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Collab {

using json = nlohmann::json;

static constexpr auto PROGRESS_SEND_INTERVAL = std::chrono::milliseconds(150);
static constexpr auto CURSOR_SEND_INTERVAL   = std::chrono::milliseconds(100);
static constexpr auto CURSOR_STALE_TIMEOUT   = std::chrono::seconds(3);

std::shared_ptr<CollabSession> CollabSessionManager::s_session;

// ---------------------------------------------------------------------------
// Helpers

static void notify(const std::string &text)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr && plater->get_notification_manager() != nullptr)
        plater->get_notification_manager()->push_notification(text);
}

ColorRGBA CollabSession::user_color_for_id(int user_id)
{
    static const std::vector<ColorRGBA> palette = {
        {0.90f, 0.30f, 0.25f, 1.0f}, // red
        {0.25f, 0.55f, 0.95f, 1.0f}, // blue
        {0.30f, 0.80f, 0.40f, 1.0f}, // green
        {0.95f, 0.65f, 0.15f, 1.0f}, // orange
        {0.70f, 0.35f, 0.90f, 1.0f}, // purple
        {0.20f, 0.80f, 0.80f, 1.0f}, // teal
        {0.95f, 0.45f, 0.70f, 1.0f}, // pink
        {0.75f, 0.75f, 0.25f, 1.0f}, // olive
    };
    return palette[size_t(std::max(user_id, 0)) % palette.size()];
}

static json user_to_json(const CollabSession::User &user)
{
    return json{{"id", user.id}, {"name", user.name}};
}

static CollabSession::User user_from_json(const json &j)
{
    CollabSession::User user;
    user.id    = j.value("id", -1);
    user.name  = j.value("name", std::string("user"));
    user.color = CollabSession::user_color_for_id(user.id);
    return user;
}

// ---------------------------------------------------------------------------
// CollabSessionManager

CollabSession *CollabSessionManager::get()
{
    return s_session ? s_session.get() : nullptr;
}

CollabSession *CollabSessionManager::start_hosting(const std::string &user_name, std::string &error)
{
    if (s_session) {
        error = _u8L("A collaboration session is already active.");
        return nullptr;
    }
    std::shared_ptr<CollabSession> session(new CollabSession(CollabSession::Role::Host));
    if (!session->start_hosting(user_name, error))
        return nullptr;
    s_session = session;
    return s_session.get();
}

CollabSession *CollabSessionManager::join(const std::string &link_str, const std::string &user_name, std::string &error)
{
    if (s_session) {
        error = _u8L("A collaboration session is already active.");
        return nullptr;
    }
    const auto link = parse_link(link_str);
    if (!link.has_value()) {
        error = _u8L("Invalid invite link.");
        return nullptr;
    }
    std::shared_ptr<CollabSession> session(new CollabSession(CollabSession::Role::Guest));
    if (!session->start_joining(*link, user_name, error))
        return nullptr;
    s_session = session;
    return s_session.get();
}

void CollabSessionManager::stop()
{
    if (!s_session)
        return;
    // Keep the object alive during teardown; callbacks may still reference it.
    std::shared_ptr<CollabSession> session = std::move(s_session);
    s_session.reset();
    session->shutdown(true);
}

bool CollabSessionManager::scene_locked()
{
    return s_session != nullptr && !s_session->is_applying_project();
}

bool CollabSessionManager::scene_locked_with_notice()
{
    if (!scene_locked())
        return false;
    notify(_u8L("Scene editing is locked during a collaboration session. Only color painting is allowed."));
    return true;
}

// ---------------------------------------------------------------------------
// Session lifecycle

CollabSession::~CollabSession()
{
    shutdown(false);
}

bool CollabSession::start_hosting(const std::string &user_name, std::string &error)
{
    m_my_name    = user_name.empty() ? _u8L("Host") : user_name;
    m_token      = generate_token();
    m_my_user_id = 0;

    m_server = std::make_unique<CollabServer>();

    std::weak_ptr<CollabSession> weak = weak_from_this();
    m_server->set_message_callback([weak](int client_id, const std::string &message) {
        json msg = json::parse(message, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
            return;
        wxGetApp().CallAfter([weak, client_id, msg]() {
            if (auto self = weak.lock(); self && self->m_active)
                self->handle_server_message(client_id, msg);
        });
    });
    m_server->set_disconnect_callback([weak](int client_id) {
        wxGetApp().CallAfter([weak, client_id]() {
            if (auto self = weak.lock(); self && self->m_active)
                self->handle_server_disconnect(client_id);
        });
    });

    const unsigned short port = m_server->start(DEFAULT_PORT, PORT_SEARCH_RANGE);
    if (port == 0) {
        error = _u8L("Could not start the collaboration server: no free network port.");
        m_server.reset();
        return false;
    }

    m_link = format_link({get_lan_ip(), port, m_token});

    User me;
    me.id    = m_my_user_id;
    me.name  = m_my_name;
    me.color = user_color_for_id(me.id);
    m_users.emplace(me.id, me);

    m_active = true;
    baseline_synced_timestamps();
    return true;
}

bool CollabSession::start_joining(const SessionLink &link, const std::string &user_name, std::string &error)
{
    m_my_name = user_name.empty() ? _u8L("Guest") : user_name;
    m_token   = link.token;
    m_link    = format_link(link);

    m_client = std::make_unique<CollabClient>();

    std::weak_ptr<CollabSession> weak = weak_from_this();
    m_client->set_connect_callback([weak](bool success, const std::string &err) {
        wxGetApp().CallAfter([weak, success, err]() {
            if (auto self = weak.lock(); self && self->m_active)
                self->handle_client_connected(success, err);
        });
    });
    m_client->set_message_callback([weak](const std::string &message) {
        json msg = json::parse(message, nullptr, false);
        if (msg.is_discarded() || !msg.is_object())
            return;
        wxGetApp().CallAfter([weak, msg]() {
            if (auto self = weak.lock(); self && self->m_active)
                self->handle_client_message(msg);
        });
    });
    m_client->set_disconnect_callback([weak]() {
        wxGetApp().CallAfter([weak]() {
            if (auto self = weak.lock(); self && self->m_active)
                self->handle_client_disconnected();
        });
    });

    m_active = true;
    m_client->connect(link.host, link.port);
    return true;
}

void CollabSession::shutdown(bool notify_peers)
{
    if (!m_active)
        return;
    m_active = false;

    if (m_role == Role::Host && m_server) {
        if (notify_peers)
            m_server->broadcast(json{{"type", MsgType::Bye}}.dump());
        m_server->stop();
        m_server.reset();
    }
    if (m_role == Role::Guest && m_client) {
        m_client->disconnect();
        m_client.reset();
    }
    m_users.clear();
    m_claims.clear();
    m_remote_cursors.clear();
}

std::vector<CollabSession::User> CollabSession::users() const
{
    std::vector<User> result;
    result.reserve(m_users.size());
    for (const auto &entry : m_users)
        result.push_back(entry.second);
    return result;
}

std::string CollabSession::status_text() const
{
    if (m_role == Role::Host)
        return format(_u8L("Hosting — %1% participant(s) connected"), m_users.size());
    if (m_client && m_client->is_connected() && m_my_user_id >= 0)
        return format(_u8L("Connected — %1% participant(s)"), m_users.size());
    return _u8L("Connecting...");
}

// ---------------------------------------------------------------------------
// Host message handling

void CollabSession::handle_server_message(int client_id, const json &msg)
{
    const std::string type = msg.value("type", std::string());

    if (type == MsgType::Hello) {
        if (msg.value("token", std::string()) != m_token) {
            m_server->send_to(client_id, json{{"type", MsgType::Error}, {"message", "Invalid session token."}}.dump());
            m_server->close_client(client_id);
            return;
        }
        if (msg.value("version", 0) != PROTOCOL_VERSION) {
            m_server->send_to(client_id, json{{"type", MsgType::Error}, {"message", "Incompatible OrcaSlicer version."}}.dump());
            m_server->close_client(client_id);
            return;
        }

        User user;
        user.id    = m_next_user_id++;
        user.name  = msg.value("name", std::string("guest"));
        user.color = user_color_for_id(user.id);
        m_client_to_user[client_id] = user.id;
        m_user_to_client[user.id]   = client_id;
        add_user(user);

        json users_json = json::array();
        for (const auto &entry : m_users)
            users_json.push_back(user_to_json(entry.second));
        m_server->send_to(client_id, json{{"type", MsgType::Welcome}, {"user_id", user.id}, {"users", users_json}}.dump());

        host_send_project(client_id);
        m_server->broadcast(json{{"type", MsgType::Join}, {"user", user_to_json(user)}}.dump(), client_id);
        notify(format(_u8L("%1% joined the collaboration session."), user.name));
        return;
    }

    const auto user_it = m_client_to_user.find(client_id);
    if (user_it == m_client_to_user.end())
        return; // Not authenticated.
    const int user_id = user_it->second;

    if (type == MsgType::Paint) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        const bool final_state = msg.value("fin", true);
        const auto data = decode_paint_data(msg.value("data", std::string()));
        if (!data.has_value() || resolve_volume(key) == nullptr)
            return;
        // The host is the ordering authority: stamp a sequence number and relay.
        const int64_t seq = ++m_seq_counter;
        m_last_applied_seq[key] = seq;
        // Ignore paint for volumes the host itself is currently painting.
        if (m_my_stroke_claims.find(key) == m_my_stroke_claims.end())
            apply_remote_paint(key, *data, user_id, final_state);
        json relay = msg;
        relay["seq"]  = seq;
        relay["user"] = user_id;
        m_server->broadcast(relay.dump(), client_id);
        return;
    }

    if (type == MsgType::Claim) {
        host_grant_or_deny_claim(VolumeKey{msg.value("obj", -1), msg.value("vol", -1)}, user_id, client_id);
        return;
    }

    if (type == MsgType::Release) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        if (auto it = m_claims.find(key); it != m_claims.end() && it->second == user_id) {
            m_claims.erase(it);
            m_server->broadcast(json{{"type", MsgType::Release}, {"obj", key.obj_idx}, {"vol", key.vol_idx}, {"user", user_id}}.dump(), client_id);
        }
        return;
    }

    if (type == MsgType::Cursor) {
        json relay = msg;
        relay["user"] = user_id;
        auto &cursor = m_remote_cursors[user_id];
        cursor.user_id  = user_id;
        cursor.position = Vec3d(msg.value("x", 0.), msg.value("y", 0.), msg.value("z", 0.));
        cursor.radius   = msg.value("r", 1.);
        if (auto it = m_users.find(user_id); it != m_users.end()) {
            cursor.name  = it->second.name;
            cursor.color = it->second.color;
        }
        cursor.last_update = std::chrono::steady_clock::now();
        m_server->broadcast(relay.dump(), client_id);
        if (Plater *plater = wxGetApp().plater(); plater != nullptr && plater->canvas3D() != nullptr)
            plater->canvas3D()->set_as_dirty();
        return;
    }

    if (type == MsgType::Bye)
        handle_server_disconnect(client_id);
}

void CollabSession::handle_server_disconnect(int client_id)
{
    const auto user_it = m_client_to_user.find(client_id);
    if (user_it == m_client_to_user.end())
        return;
    const int user_id = user_it->second;
    m_client_to_user.erase(user_it);
    m_user_to_client.erase(user_id);

    release_claims_of_user(user_id, true);
    const std::string name = m_users.count(user_id) ? m_users[user_id].name : std::string("guest");
    remove_user(user_id);
    m_server->broadcast(json{{"type", MsgType::Leave}, {"user", user_id}}.dump());
    notify(format(_u8L("%1% left the collaboration session."), name));
}

void CollabSession::host_grant_or_deny_claim(const VolumeKey &key, int user_id, int client_id)
{
    if (key.obj_idx < 0 || key.vol_idx < 0)
        return;
    if (auto it = m_claims.find(key); it != m_claims.end() && it->second != user_id) {
        m_server->send_to(client_id, json{{"type", MsgType::ClaimDenied}, {"obj", key.obj_idx}, {"vol", key.vol_idx}, {"holder", it->second}}.dump());
        return;
    }
    m_claims[key] = user_id;
    m_server->broadcast(json{{"type", MsgType::Claim}, {"obj", key.obj_idx}, {"vol", key.vol_idx}, {"user", user_id}}.dump());
}

void CollabSession::host_send_project(int client_id)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    const auto temp_path = boost::filesystem::temp_directory_path() /
                           boost::filesystem::unique_path("orca_collab_%%%%%%%%.3mf");
    if (plater->export_3mf(temp_path, SaveStrategy::Silence) < 0) {
        BOOST_LOG_TRIVIAL(error) << "CollabSession: failed to export project for transfer";
        return;
    }

    std::ifstream file(temp_path.string(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    boost::system::error_code ec;
    boost::filesystem::remove(temp_path, ec);

    std::string name = plater->get_project_name().ToUTF8().data();
    m_server->send_to(client_id, json{{"type", MsgType::Project}, {"name", name}, {"data", base64_encode(content)}}.dump());
}

// ---------------------------------------------------------------------------
// Guest message handling

void CollabSession::handle_client_connected(bool success, const std::string &error)
{
    if (!success) {
        end_session_with_notice(format(_u8L("Could not connect to the collaboration session: %1%"), error));
        return;
    }
    m_client->send(json{{"type", MsgType::Hello}, {"token", m_token}, {"name", m_my_name}, {"version", PROTOCOL_VERSION}}.dump());
}

void CollabSession::handle_client_disconnected()
{
    end_session_with_notice(_u8L("Disconnected from the collaboration session."));
}

void CollabSession::handle_client_message(const json &msg)
{
    const std::string type = msg.value("type", std::string());

    if (type == MsgType::Welcome) {
        m_my_user_id = msg.value("user_id", -1);
        m_users.clear();
        if (msg.contains("users") && msg["users"].is_array())
            for (const auto &user_json : msg["users"])
                add_user(user_from_json(user_json));
        notify(_u8L("Joined the collaboration session."));
        return;
    }

    if (type == MsgType::Error) {
        end_session_with_notice(format(_u8L("Collaboration session error: %1%"), msg.value("message", std::string())));
        return;
    }

    if (type == MsgType::Project) {
        apply_project(msg.value("name", std::string("collab_project")), msg.value("data", std::string()));
        return;
    }

    if (type == MsgType::Paint) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        const int     author      = msg.value("user", -1);
        const int64_t seq         = msg.value("seq", int64_t(0));
        const bool    final_state = msg.value("fin", true);
        if (author == m_my_user_id)
            return; // Our own paint relayed back (should not happen).
        if (m_my_stroke_claims.find(key) != m_my_stroke_claims.end())
            return; // We are painting this volume; our claim protects it.
        if (auto it = m_last_applied_seq.find(key); it != m_last_applied_seq.end() && seq != 0 && seq < it->second)
            return; // Out of order, a newer state was already applied.
        const auto data = decode_paint_data(msg.value("data", std::string()));
        if (!data.has_value())
            return;
        if (seq != 0)
            m_last_applied_seq[key] = seq;
        apply_remote_paint(key, *data, author, final_state);
        return;
    }

    if (type == MsgType::Claim) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        m_claims[key] = msg.value("user", -1);
        return;
    }

    if (type == MsgType::ClaimDenied) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        m_my_stroke_claims.erase(key);
        m_blocked_volumes.insert(key);
        revert_volume_from_model(key);
        const int holder = msg.value("holder", -1);
        const std::string holder_name = m_users.count(holder) ? m_users[holder].name : _u8L("another user");
        notify(format(_u8L("%1% is painting this part right now."), holder_name));
        return;
    }

    if (type == MsgType::Release) {
        const VolumeKey key{msg.value("obj", -1), msg.value("vol", -1)};
        if (auto it = m_claims.find(key); it != m_claims.end() && it->second == msg.value("user", -1))
            m_claims.erase(it);
        return;
    }

    if (type == MsgType::Cursor) {
        const int user_id = msg.value("user", -1);
        if (user_id == m_my_user_id)
            return;
        auto &cursor = m_remote_cursors[user_id];
        cursor.user_id  = user_id;
        cursor.position = Vec3d(msg.value("x", 0.), msg.value("y", 0.), msg.value("z", 0.));
        cursor.radius   = msg.value("r", 1.);
        if (auto it = m_users.find(user_id); it != m_users.end()) {
            cursor.name  = it->second.name;
            cursor.color = it->second.color;
        }
        cursor.last_update = std::chrono::steady_clock::now();
        if (Plater *plater = wxGetApp().plater(); plater != nullptr && plater->canvas3D() != nullptr)
            plater->canvas3D()->set_as_dirty();
        return;
    }

    if (type == MsgType::Join) {
        if (msg.contains("user")) {
            const User user = user_from_json(msg["user"]);
            add_user(user);
            notify(format(_u8L("%1% joined the collaboration session."), user.name));
        }
        return;
    }

    if (type == MsgType::Leave) {
        const int user_id = msg.value("user", -1);
        release_claims_of_user(user_id, false);
        const std::string name = m_users.count(user_id) ? m_users[user_id].name : std::string("guest");
        remove_user(user_id);
        notify(format(_u8L("%1% left the collaboration session."), name));
        return;
    }

    if (type == MsgType::Bye)
        end_session_with_notice(_u8L("The host ended the collaboration session."));
}

// ---------------------------------------------------------------------------
// Painting integration

bool CollabSession::try_begin_paint(const VolumeKey &key)
{
    if (!m_active || key.obj_idx < 0 || key.vol_idx < 0)
        return true;
    if (m_blocked_volumes.find(key) != m_blocked_volumes.end())
        return false;
    if (m_my_stroke_claims.find(key) != m_my_stroke_claims.end())
        return true; // Already claimed during this stroke.
    if (auto it = m_claims.find(key); it != m_claims.end() && it->second != m_my_user_id) {
        m_blocked_volumes.insert(key);
        const std::string holder_name = m_users.count(it->second) ? m_users[it->second].name : _u8L("another user");
        notify(format(_u8L("%1% is painting this part right now."), holder_name));
        return false;
    }

    m_my_stroke_claims.insert(key);
    if (m_role == Role::Host) {
        m_claims[key] = m_my_user_id;
        if (m_server)
            m_server->broadcast(json{{"type", MsgType::Claim}, {"obj", key.obj_idx}, {"vol", key.vol_idx}, {"user", m_my_user_id}}.dump());
    } else if (m_client) {
        // Optimistic: paint immediately, revert if the host denies the claim.
        m_client->send(json{{"type", MsgType::Claim}, {"obj", key.obj_idx}, {"vol", key.vol_idx}}.dump());
    }
    return true;
}

void CollabSession::paint_progress(const VolumeKey &key, const TriangleSelector &selector)
{
    if (!m_active || m_my_stroke_claims.find(key) == m_my_stroke_claims.end())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_progress_send < PROGRESS_SEND_INTERVAL)
        return;
    m_last_progress_send = now;
    send_paint_message(key, encode_paint_data(selector.serialize()), false);
}

void CollabSession::end_stroke()
{
    if (!m_active)
        return;
    // Broadcast final states first (sync uses annotation timestamps), then
    // release the locks so nobody paints over the volume before the final
    // state message is on the wire.
    sync_paint_state();
    for (const VolumeKey &key : m_my_stroke_claims) {
        if (m_role == Role::Host) {
            if (auto it = m_claims.find(key); it != m_claims.end() && it->second == m_my_user_id)
                m_claims.erase(it);
            if (m_server)
                m_server->broadcast(json{{"type", MsgType::Release}, {"obj", key.obj_idx}, {"vol", key.vol_idx}, {"user", m_my_user_id}}.dump());
        } else if (m_client) {
            m_client->send(json{{"type", MsgType::Release}, {"obj", key.obj_idx}, {"vol", key.vol_idx}}.dump());
        }
    }
    m_my_stroke_claims.clear();
    m_blocked_volumes.clear();
}

void CollabSession::send_cursor(const Vec3d &world_position, double radius)
{
    if (!m_active)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_cursor_send < CURSOR_SEND_INTERVAL)
        return;
    m_last_cursor_send = now;
    const json msg = {{"type", MsgType::Cursor}, {"user", m_my_user_id},
                      {"x", world_position.x()}, {"y", world_position.y()}, {"z", world_position.z()},
                      {"r", radius}};
    send_to_host_or_broadcast(msg);
}

void CollabSession::sync_paint_state()
{
    if (!m_active)
        return;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    const Model &model = plater->model();

    for (int obj_idx = 0; obj_idx < int(model.objects.size()); ++obj_idx) {
        const ModelObject *mo = model.objects[obj_idx];
        int vol_idx = -1;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv->is_model_part())
                continue;
            ++vol_idx;
            const VolumeKey key{obj_idx, vol_idx};
            const uint64_t ts = mv->mmu_segmentation_facets.timestamp();
            const auto synced_it = m_synced_ts.find(key);
            if (synced_it != m_synced_ts.end() && synced_it->second == ts)
                continue;

            const auto author_it = m_last_author.find(key);
            if (author_it != m_last_author.end() && author_it->second != m_my_user_id) {
                // The last write came from a remote user and the local state
                // diverged (typically a local undo reverted it). Restore the
                // remote state instead of broadcasting stale data.
                if (auto cache_it = m_remote_cache.find(key); cache_it != m_remote_cache.end()) {
                    apply_remote_paint(key, cache_it->second, author_it->second, true);
                    continue;
                }
            }

            m_synced_ts[key]  = ts;
            m_last_author[key] = m_my_user_id;
            send_paint_message(key, encode_paint_data(mv->mmu_segmentation_facets.get_data()), true);
        }
    }
}

void CollabSession::send_paint_message(const VolumeKey &key, const std::string &encoded_data, bool final_state)
{
    json msg = {{"type", MsgType::Paint}, {"obj", key.obj_idx}, {"vol", key.vol_idx},
                {"user", m_my_user_id}, {"fin", final_state}, {"data", encoded_data}};
    if (m_role == Role::Host) {
        const int64_t seq = ++m_seq_counter;
        m_last_applied_seq[key] = seq;
        msg["seq"] = seq;
    } else {
        msg["seq"] = 0; // The host assigns the authoritative sequence number.
    }
    send_to_host_or_broadcast(msg);
}

void CollabSession::send_to_host_or_broadcast(const json &msg, int except_client_id)
{
    if (m_role == Role::Host) {
        if (m_server)
            m_server->broadcast(msg.dump(), except_client_id);
    } else {
        if (m_client)
            m_client->send(msg.dump());
    }
}

std::vector<CollabSession::RemoteCursor> CollabSession::remote_cursors()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<RemoteCursor> result;
    for (auto it = m_remote_cursors.begin(); it != m_remote_cursors.end();) {
        if (now - it->second.last_update > CURSOR_STALE_TIMEOUT) {
            it = m_remote_cursors.erase(it);
        } else {
            result.push_back(it->second);
            ++it;
        }
    }
    return result;
}

const CollabSession::User *CollabSession::claim_holder(const VolumeKey &key) const
{
    const auto it = m_claims.find(key);
    if (it == m_claims.end())
        return nullptr;
    const auto user_it = m_users.find(it->second);
    return user_it == m_users.end() ? nullptr : &user_it->second;
}

// ---------------------------------------------------------------------------
// Model integration

ModelVolume *CollabSession::resolve_volume(const VolumeKey &key) const
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || key.obj_idx < 0 || key.obj_idx >= int(plater->model().objects.size()))
        return nullptr;
    ModelObject *mo = plater->model().objects[key.obj_idx];
    int vol_idx = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        if (++vol_idx == key.vol_idx)
            return mv;
    }
    return nullptr;
}

void CollabSession::apply_remote_paint(const VolumeKey &key, const TriangleSelector::TriangleSplittingData &data, int author, bool final_state)
{
    ModelVolume *mv = resolve_volume(key);
    if (mv == nullptr)
        return;

    try {
        TriangleSelector selector(mv->mesh());
        selector.deserialize(data);
        if (mv->mmu_segmentation_facets.set(selector)) {
            m_synced_ts[key] = mv->mmu_segmentation_facets.timestamp();
            update_open_gizmo_volume(key, data);
            refresh_volume_ui(key, final_state);
        }
        m_last_author[key]  = author;
        m_remote_cache[key] = data;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "CollabSession: failed to apply remote paint: " << e.what();
    }
}

void CollabSession::update_open_gizmo_volume(const VolumeKey &key, const TriangleSelector::TriangleSplittingData &data)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || plater->canvas3D() == nullptr)
        return;
    GLCanvas3D *canvas = plater->canvas3D();
    GLGizmosManager &gizmos = canvas->get_gizmos_manager();
    if (gizmos.get_current_type() != GLGizmosManager::MmSegmentation)
        return;
    if (canvas->get_selection().get_object_idx() != key.obj_idx)
        return;
    if (auto *gizmo = dynamic_cast<GLGizmoMmuSegmentation *>(gizmos.get_gizmo(GLGizmosManager::MmSegmentation)))
        gizmo->collab_update_volume(key.vol_idx, data);
}

void CollabSession::refresh_volume_ui(const VolumeKey &key, bool final_state)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    GLCanvas3D *canvas = plater->canvas3D();
    if (canvas != nullptr)
        canvas->set_as_dirty();
    if (!final_state)
        return;

    // The heavier updates run only when a stroke is finished.
    if (wxGetApp().obj_list() != nullptr)
        wxGetApp().obj_list()->update_info_items(size_t(key.obj_idx));
    plater->get_partplate_list().notify_instance_update(key.obj_idx, 0);
    if (canvas != nullptr)
        canvas->post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    plater->notify_filament_usage_changed();
}

void CollabSession::revert_volume_from_model(const VolumeKey &key)
{
    ModelVolume *mv = resolve_volume(key);
    if (mv == nullptr)
        return;
    update_open_gizmo_volume(key, mv->mmu_segmentation_facets.get_data());
    if (Plater *plater = wxGetApp().plater(); plater != nullptr && plater->canvas3D() != nullptr)
        plater->canvas3D()->set_as_dirty();
}

void CollabSession::apply_project(const std::string &project_name, const std::string &data_b64)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    const std::string content = base64_decode(data_b64);
    if (content.empty()) {
        end_session_with_notice(_u8L("Received an empty project from the host."));
        return;
    }

    std::string safe_name = project_name.empty() ? std::string("collab_project") : project_name;
    for (char &c : safe_name)
        if (c == '/' || c == '\\' || c == ':')
            c = '_';
    const auto temp_path = boost::filesystem::temp_directory_path() /
                           boost::filesystem::unique_path("orca_collab_%%%%%%%%_" + safe_name + ".3mf");
    {
        std::ofstream file(temp_path.string(), std::ios::binary);
        file.write(content.data(), std::streamsize(content.size()));
    }

    m_applying_project = true;
    try {
        plater->new_project(true /* skip_confirm */, true /* silent */);
        plater->load_files(std::vector<boost::filesystem::path>{temp_path},
                           LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "CollabSession: failed to load received project: " << e.what();
    }
    m_applying_project = false;

    boost::system::error_code ec;
    boost::filesystem::remove(temp_path, ec);

    baseline_synced_timestamps();
    notify(_u8L("Project received from the host. Happy painting!"));
}

void CollabSession::baseline_synced_timestamps()
{
    m_synced_ts.clear();
    m_last_applied_seq.clear();
    m_last_author.clear();
    m_remote_cache.clear();

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    const Model &model = plater->model();
    for (int obj_idx = 0; obj_idx < int(model.objects.size()); ++obj_idx) {
        int vol_idx = -1;
        for (const ModelVolume *mv : model.objects[obj_idx]->volumes) {
            if (!mv->is_model_part())
                continue;
            ++vol_idx;
            m_synced_ts[VolumeKey{obj_idx, vol_idx}] = mv->mmu_segmentation_facets.timestamp();
        }
    }
}

// ---------------------------------------------------------------------------
// Users

void CollabSession::add_user(const User &user)
{
    m_users[user.id] = user;
}

void CollabSession::remove_user(int user_id)
{
    m_users.erase(user_id);
    m_remote_cursors.erase(user_id);
}

void CollabSession::release_claims_of_user(int user_id, bool broadcast_msg)
{
    for (auto it = m_claims.begin(); it != m_claims.end();) {
        if (it->second == user_id) {
            if (broadcast_msg && m_role == Role::Host && m_server)
                m_server->broadcast(json{{"type", MsgType::Release}, {"obj", it->first.obj_idx}, {"vol", it->first.vol_idx}, {"user", user_id}}.dump());
            it = m_claims.erase(it);
        } else {
            ++it;
        }
    }
}

void CollabSession::end_session_with_notice(const std::string &reason)
{
    notify(reason);
    // Defer: we may be inside one of this session's own callbacks.
    wxGetApp().CallAfter([]() { CollabSessionManager::stop(); });
    m_active = false;
}

}}} // namespace Slic3r::GUI::Collab
