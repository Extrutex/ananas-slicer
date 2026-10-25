#include "PrinterStatusStrip.hpp"

#include <cmath>

#include <wx/sizer.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "format.hpp"

namespace Slic3r { namespace GUI {

namespace {

wxString duration_text(double seconds)
{
    const int total = int(std::lround(seconds));
    return wxString::Format("%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

wxString state_text(const MoonrakerPrinterStatus &s)
{
    using Link = MoonrakerPrinterStatus::Link;
    switch (s.link) {
    case Link::Idle: return _L("No printer host set");
    case Link::Connecting: return _L("Connecting");
    case Link::Offline: return _L("Printer not reachable");
    case Link::Online: break;
    }
    if (s.klippy_state != "ready")
        return s.klippy_state == "startup" ? _L("Klipper starting") : _L("Klipper not ready");
    if (s.print_state == "printing") return _L("Printing");
    if (s.print_state == "paused") return _L("Paused");
    if (s.print_state == "complete") return _L("Print finished");
    if (s.print_state == "cancelled") return _L("Print cancelled");
    if (s.print_state == "error") return _L("Print error");
    return _L("Idle");
}

wxColour state_colour(const MoonrakerPrinterStatus &s)
{
    using Link = MoonrakerPrinterStatus::Link;
    if (s.link == Link::Offline || (s.link == Link::Online && (s.klippy_state == "error" || s.klippy_state == "shutdown" || s.print_state == "error")))
        return wxColour(0xE0, 0x4B, 0x3C);
    if (s.link == Link::Online && (s.print_state == "printing" || s.print_state == "paused"))
        return wxColour(0x1A, 0xA8, 0x6A);
    if (s.link == Link::Online)
        return wxColour(0x4C, 0x9B, 0xE8);
    return wxColour(0x8C, 0x8C, 0x8C);
}

wxString detail_text(const MoonrakerPrinterStatus &s)
{
    using Link = MoonrakerPrinterStatus::Link;
    if (s.link == Link::Idle)
        return _L("Enter the printer's Moonraker address in the printer settings to see its state here.");
    if (s.link == Link::Connecting)
        return from_u8(s.host);
    if (s.link == Link::Offline)
        return from_u8(s.host) + ": " + from_u8(s.error);
    if (s.klippy_state != "ready")
        return from_u8(s.message);

    wxString text;
    const bool active = s.print_state == "printing" || s.print_state == "paused";
    if (active || s.print_state == "complete" || s.print_state == "cancelled" || s.print_state == "error") {
        text = from_u8(s.filename);
        if (active) {
            text += wxString::Format("  %d%%", int(std::lround(s.progress * 100)));
            if (s.current_layer >= 0 && s.total_layer > 0)
                text += "  " + format_wxstr(_L("layer %1% of %2%"), s.current_layer, s.total_layer);
            text += "  " + duration_text(s.print_duration);
        }
    } else {
        text = from_u8(s.host);
    }
    if (!s.message.empty())
        text += "  " + from_u8(s.message);
    return text;
}

wxString temps_text(const MoonrakerPrinterStatus &s)
{
    if (s.link != MoonrakerPrinterStatus::Link::Online || s.klippy_state != "ready")
        return {};
    wxString text = format_wxstr(_L("Nozzle %1%/%2% ℃"), int(std::lround(s.nozzle_temp)), int(std::lround(s.nozzle_target)));
    if (s.has_bed)
        text += "   " + format_wxstr(_L("Bed %1%/%2% ℃"), int(std::lround(s.bed_temp)), int(std::lround(s.bed_target)));
    return text;
}

} // namespace

PrinterStatusStrip::PrinterStatusStrip(wxWindow *parent)
    : wxPanel(parent, wxID_ANY)
{
    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    m_state     = new Label(this, Label::Head_14, wxString());
    m_detail    = new Label(this, Label::Body_13, wxString(), wxST_ELLIPSIZE_END);
    m_temps     = new Label(this, Label::Body_13, wxString());
    sizer->Add(m_state, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
    sizer->Add(m_detail, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    sizer->Add(m_temps, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
    SetSizer(sizer);
    sizer->SetSizeHints(this);
    wxGetApp().UpdateDarkUIWin(this);
    update(MoonrakerPrinterStatus{});
}

void PrinterStatusStrip::update(const MoonrakerPrinterStatus &status)
{
    m_state->SetForegroundColour(state_colour(status));
    m_state->SetLabel(wxString::FromUTF8("\xE2\x97\x8F ") + state_text(status));
    m_detail->SetLabel(detail_text(status));
    m_detail->SetToolTip(detail_text(status));
    m_temps->SetLabel(temps_text(status));
    Layout();
}

}} // namespace Slic3r::GUI
