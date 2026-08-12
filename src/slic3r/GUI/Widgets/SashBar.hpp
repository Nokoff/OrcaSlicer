#ifndef slic3r_GUI_SashBar_hpp_
#define slic3r_GUI_SashBar_hpp_

#include <functional>

#include <wx/window.h>

// ORCA: A thin horizontal grab bar that lets the user redistribute the vertical
// space between the control sitting above it and the one sitting below it.
// The bar itself does not resize anything, it only reports the drag; the owner
// decides what the offset means.
class SashBar : public wxWindow
{
public:
    // thickness is given in DIP units, <= 0 uses the default
    explicit SashBar(wxWindow *parent, int thickness = 0);

    void Rescale();

    // Called when a drag starts, so the owner can snapshot the current sizes.
    void SetOnDragBegin(std::function<void()> cb) { m_on_drag_begin = std::move(cb); }

    // Called on every mouse move while dragging with the offset (in pixels)
    // relative to where the drag started, and once more with finished == true
    // when the drag ends. The offset is meaningless on the final call - the
    // drag may end because the capture was lost, not because the mouse was
    // released - so treat it purely as "the user is done, persist the result".
    void SetOnDrag(std::function<void(int offset, bool finished)> cb) { m_on_drag = std::move(cb); }

    // Called on a double click, to restore the default split.
    void SetOnReset(std::function<void()> cb) { m_on_reset = std::move(cb); }

    void SetColours(const wxColour &background, const wxColour &grip);

private:
    void paint(wxPaintEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void on_left_up(wxMouseEvent &evt);
    void on_motion(wxMouseEvent &evt);
    void on_dclick(wxMouseEvent &evt);
    void on_enter(wxMouseEvent &evt);
    void on_leave(wxMouseEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &evt);
    void stop_drag();

    std::function<void()>          m_on_drag_begin;
    std::function<void(int, bool)> m_on_drag;
    std::function<void()>          m_on_reset;

    int      m_thickness = 0; // DIP units
    wxColour m_background;
    wxColour m_grip;
    bool     m_hovered  = false;
    bool     m_dragging = false;
    int      m_origin_y = 0; // screen coordinate the drag started at
};

#endif // !slic3r_GUI_SashBar_hpp_
