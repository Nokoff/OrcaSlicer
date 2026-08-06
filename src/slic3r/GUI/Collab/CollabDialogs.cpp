#include "CollabDialogs.hpp"

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include "CollabSession.hpp"

#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Collab {

static void copy_to_clipboard(const wxString &text)
{
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

// ---------------------------------------------------------------------------
// SessionInfoDialog

SessionInfoDialog::SessionInfoDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Collaboration Session"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *hint = new wxStaticText(this, wxID_ANY,
        _L("Share this link with people on your local network so they can join and paint with you:"));
    hint->Wrap(FromDIP(420));
    main_sizer->Add(hint, 0, wxALL | wxEXPAND, FromDIP(10));

    auto *link_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_link_field = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxSize(FromDIP(340), -1), wxTE_READONLY);
    link_sizer->Add(m_link_field, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto *copy_btn = new wxButton(this, wxID_ANY, _L("Copy Link"));
    copy_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        copy_to_clipboard(m_link_field->GetValue());
        if (wxGetApp().plater() != nullptr && wxGetApp().plater()->get_notification_manager() != nullptr)
            wxGetApp().plater()->get_notification_manager()->push_notification(_u8L("Invite link copied to clipboard."));
    });
    link_sizer->Add(copy_btn, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(link_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(10));

    m_status_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    main_sizer->Add(m_status_label, 0, wxALL, FromDIP(10));

    main_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Participants:")), 0, wxLEFT | wxRIGHT, FromDIP(10));
    m_user_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(120)));
    main_sizer->Add(m_user_list, 1, wxALL | wxEXPAND, FromDIP(10));

    auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *end_btn = new wxButton(this, wxID_ANY,
        CollabSessionManager::get() != nullptr && CollabSessionManager::get()->role() == CollabSession::Role::Host ?
            _L("End Session") : _L("Leave Session"));
    end_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        CollabSessionManager::stop();
        EndModal(wxID_CLOSE);
    });
    button_sizer->Add(end_btn, 0, wxRIGHT, FromDIP(6));
    auto *close_btn = new wxButton(this, wxID_CLOSE, _L("Close"));
    close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    button_sizer->Add(close_btn, 0);
    main_sizer->Add(button_sizer, 0, wxALL | wxALIGN_RIGHT, FromDIP(10));

    SetSizerAndFit(main_sizer);
    CenterOnParent();

    m_refresh_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &SessionInfoDialog::refresh, this);
    m_refresh_timer->Start(1000);

    wxTimerEvent evt;
    refresh(evt);
}

SessionInfoDialog::~SessionInfoDialog()
{
    if (m_refresh_timer != nullptr) {
        m_refresh_timer->Stop();
        delete m_refresh_timer;
    }
}

void SessionInfoDialog::refresh(wxTimerEvent &)
{
    CollabSession *session = CollabSessionManager::get();
    if (session == nullptr) {
        m_status_label->SetLabel(_L("The session has ended."));
        m_user_list->Clear();
        return;
    }
    m_link_field->SetValue(wxString::FromUTF8(session->invite_link()));
    m_status_label->SetLabel(wxString::FromUTF8(session->status_text()));

    wxArrayString entries;
    for (const CollabSession::User &user : session->users()) {
        wxString entry = wxString::FromUTF8(user.name);
        if (user.id == session->my_user_id())
            entry += _L(" (you)");
        if (user.id == 0)
            entry += _L(" [host]");
        entries.Add(entry);
    }
    m_user_list->Set(entries);
}

// ---------------------------------------------------------------------------
// JoinSessionDialog

JoinSessionDialog::JoinSessionDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Join Collaboration Session"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
{
    auto *main_sizer = new wxBoxSizer(wxVERTICAL);

    auto *warning = new wxStaticText(this, wxID_ANY,
        _L("Joining a session replaces your current project with the host's project."));
    warning->Wrap(FromDIP(400));
    main_sizer->Add(warning, 0, wxALL | wxEXPAND, FromDIP(10));

    auto *grid = new wxFlexGridSizer(2, 2, FromDIP(6), FromDIP(6));
    grid->AddGrowableCol(1, 1);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Your name:")), 0, wxALIGN_CENTER_VERTICAL);
    m_name_field = new wxTextCtrl(this, wxID_ANY, wxGetUserId(), wxDefaultPosition, wxSize(FromDIP(300), -1));
    grid->Add(m_name_field, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, _L("Invite link:")), 0, wxALIGN_CENTER_VERTICAL);
    m_link_field = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(300), -1));
    m_link_field->SetHint("orca-collab://192.168.1.20:14700/token");
    grid->Add(m_link_field, 1, wxEXPAND);
    main_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(10));

    // Pre-fill from the clipboard if it contains an invite link.
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            const std::string clip = data.GetText().ToUTF8().data();
            if (parse_link(clip).has_value())
                m_link_field->SetValue(data.GetText());
        }
        wxTheClipboard->Close();
    }

    auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *join_btn = new wxButton(this, wxID_OK, _L("Join"));
    join_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_join(); });
    button_sizer->Add(join_btn, 0, wxRIGHT, FromDIP(6));
    auto *cancel_btn = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    button_sizer->Add(cancel_btn, 0);
    main_sizer->Add(button_sizer, 0, wxALL | wxALIGN_RIGHT, FromDIP(10));

    SetSizerAndFit(main_sizer);
    CenterOnParent();
}

void JoinSessionDialog::on_join()
{
    const std::string link = m_link_field->GetValue().ToUTF8().data();
    const std::string name = m_name_field->GetValue().ToUTF8().data();
    if (!parse_link(link).has_value()) {
        wxMessageBox(_L("The invite link is not valid. It should look like:\norca-collab://192.168.1.20:14700/token"),
                     _L("Join Collaboration Session"), wxICON_WARNING | wxOK, this);
        return;
    }

    std::string error;
    if (CollabSessionManager::join(link, name, error) == nullptr) {
        wxMessageBox(wxString::FromUTF8(error), _L("Join Collaboration Session"), wxICON_ERROR | wxOK, this);
        return;
    }
    EndModal(wxID_OK);
}

// ---------------------------------------------------------------------------
// Menu entry points

void start_session_from_menu(wxWindow *parent)
{
    if (CollabSessionManager::is_active()) {
        show_session_info(parent);
        return;
    }
    std::string error;
    if (CollabSessionManager::start_hosting(wxGetUserId().ToUTF8().data(), error) == nullptr) {
        wxMessageBox(wxString::FromUTF8(error), _L("Collaboration Session"), wxICON_ERROR | wxOK, parent);
        return;
    }
    show_session_info(parent);
}

void join_session_from_menu(wxWindow *parent)
{
    if (CollabSessionManager::is_active()) {
        show_session_info(parent);
        return;
    }
    JoinSessionDialog dialog(parent);
    if (dialog.ShowModal() == wxID_OK)
        show_session_info(parent);
}

void show_session_info(wxWindow *parent)
{
    if (!CollabSessionManager::is_active())
        return;
    SessionInfoDialog dialog(parent);
    dialog.ShowModal();
}

void copy_invite_link_to_clipboard()
{
    if (CollabSession *session = CollabSessionManager::get()) {
        copy_to_clipboard(wxString::FromUTF8(session->invite_link()));
        if (wxGetApp().plater() != nullptr && wxGetApp().plater()->get_notification_manager() != nullptr)
            wxGetApp().plater()->get_notification_manager()->push_notification(_u8L("Invite link copied to clipboard."));
    }
}

void leave_session_from_menu()
{
    CollabSessionManager::stop();
}

}}} // namespace Slic3r::GUI::Collab
