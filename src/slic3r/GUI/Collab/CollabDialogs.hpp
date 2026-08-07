#ifndef slic3r_CollabDialogs_hpp_
#define slic3r_CollabDialogs_hpp_

#include <wx/dialog.h>

class wxListBox;
class wxStaticText;
class wxTextCtrl;
class wxTimer;
class wxTimerEvent;

namespace Slic3r { namespace GUI { namespace Collab {

// Shows the invite link and the connected users of the active session.
// Also used right after starting to host. Closing the dialog keeps the
// session running; the "End Session" button terminates it.
class SessionInfoDialog : public wxDialog
{
public:
    explicit SessionInfoDialog(wxWindow *parent);
    ~SessionInfoDialog() override;

private:
    void refresh(wxTimerEvent &);

    wxTextCtrl   *m_link_field   = nullptr;
    wxStaticText *m_status_label = nullptr;
    wxListBox    *m_user_list    = nullptr;
    wxTimer      *m_refresh_timer = nullptr;
};

// Asks for a display name and an invite link, then joins the session.
class JoinSessionDialog : public wxDialog
{
public:
    explicit JoinSessionDialog(wxWindow *parent);

private:
    void on_join();

    wxTextCtrl *m_name_field = nullptr;
    wxTextCtrl *m_link_field = nullptr;
};

// Menu entry points.
void start_session_from_menu(wxWindow *parent);
void join_session_from_menu(wxWindow *parent);
void show_session_info(wxWindow *parent);
void copy_invite_link_to_clipboard();
void leave_session_from_menu();

}}} // namespace Slic3r::GUI::Collab

#endif // slic3r_CollabDialogs_hpp_
