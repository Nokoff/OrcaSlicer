#include "SashBar.hpp"
#include "StateColor.hpp"

#include <algorithm>

#include <wx/dcclient.h>
#include <wx/utils.h>

SashBar::SashBar(wxWindow *parent, int thickness)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    m_thickness  = thickness > 0 ? thickness : 8;
    m_background = parent->GetBackgroundColour();
    m_grip       = wxColour("#CECECE");

    Rescale();

    SetCursor(wxCursor(wxCURSOR_SIZENS));
    DisableFocusFromKeyboard();

    Bind(wxEVT_PAINT, &SashBar::paint, this);
    Bind(wxEVT_LEFT_DOWN, &SashBar::on_left_down, this);
    Bind(wxEVT_LEFT_UP, &SashBar::on_left_up, this);
    Bind(wxEVT_LEFT_DCLICK, &SashBar::on_dclick, this);
    Bind(wxEVT_MOTION, &SashBar::on_motion, this);
    Bind(wxEVT_ENTER_WINDOW, &SashBar::on_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &SashBar::on_leave, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &SashBar::on_capture_lost, this);
}

void SashBar::Rescale()
{
    const int thickness = FromDIP(m_thickness);
    SetMinSize(wxSize(-1, thickness));
    SetMaxSize(wxSize(-1, thickness));
}

void SashBar::SetColours(const wxColour &background, const wxColour &grip)
{
    m_background = background;
    m_grip       = grip;
    Refresh();
}

void SashBar::paint(wxPaintEvent &evt)
{
    wxPaintDC  dc(this);
    const wxSize size = GetSize();

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(m_background)));
    dc.DrawRectangle(0, 0, size.x, size.y);

    // A short centred grip, emphasised while hovered or dragged.
    const bool active = m_hovered || m_dragging;
    const int  w      = std::max(FromDIP(16), std::min(size.x / 4, FromDIP(32)));
    const int  h      = active ? FromDIP(4) : FromDIP(2);
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(active ? wxColour("#009688") : m_grip)));
    dc.DrawRoundedRectangle((size.x - w) / 2, (size.y - h) / 2, w, h, h / 2.);
}

void SashBar::on_left_down(wxMouseEvent &evt)
{
    if (m_dragging)
        return;
    m_dragging = true;
    m_origin_y = wxGetMousePosition().y;
    if (!HasCapture())
        CaptureMouse();
    if (m_on_drag_begin)
        m_on_drag_begin();
    Refresh();
}

void SashBar::on_motion(wxMouseEvent &evt)
{
    if (!m_dragging) {
        evt.Skip();
        return;
    }
    if (m_on_drag)
        m_on_drag(wxGetMousePosition().y - m_origin_y, false);
}

void SashBar::on_left_up(wxMouseEvent &evt) { stop_drag(); }

void SashBar::on_dclick(wxMouseEvent &evt)
{
    stop_drag();
    if (m_on_reset)
        m_on_reset();
}

void SashBar::on_capture_lost(wxMouseCaptureLostEvent &evt) { stop_drag(); }

void SashBar::stop_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
    if (m_on_drag)
        m_on_drag(0, true);
    Refresh();
}

void SashBar::on_enter(wxMouseEvent &evt)
{
    m_hovered = true;
    Refresh();
    evt.Skip();
}

void SashBar::on_leave(wxMouseEvent &evt)
{
    m_hovered = false;
    Refresh();
    evt.Skip();
}
