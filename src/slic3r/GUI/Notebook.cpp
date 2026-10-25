#include "Notebook.hpp"

//#ifdef _WIN32

#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"

//BBS set font size
#include "Widgets/Label.hpp"
#include "Widgets/ThemeTokens.hpp"

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/sizer.h>

wxDEFINE_EVENT(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, wxCommandEvent);

// Main tab strip colours. The strip is chrome in both colour modes, so the tokens are
// used directly; only the strip ground goes through the dark map.
static void apply_tab_style(Button* btn, bool selected)
{
    using namespace Slic3r::GUI;
    btn->SetBackgroundColor(StateColor(
        std::pair{wxColour(selected ? Theme::CARBON_700 : Theme::CARBON_800), (int) StateColor::Hovered},
        std::pair{selected ? wxColour(Theme::CARBON_700) : wxColour("#3B4446"), (int) StateColor::Normal}));
    btn->SetTextColor(StateColor(
        std::pair{wxColour(selected ? Theme::ALU_50 : Theme::ALU_100), (int) StateColor::Hovered},
        std::pair{wxColour(selected ? Theme::ALU_50 : Theme::ALU_200), (int) StateColor::Normal}));
}

ButtonsListCtrl::ButtonsListCtrl(wxWindow *parent, wxBoxSizer* side_tools) :
    wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
{
#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__

    wxColour default_btn_bg;
#ifdef __APPLE__
    default_btn_bg = wxColour("#3B4446"); // Gradient #414B4E
#else
    default_btn_bg = wxColour("#3B4446"); // Gradient #414B4E
#endif

   
    SetBackgroundColour(default_btn_bg);

    int em = em_unit(this);// Slic3r::GUI::wxGetApp().em_unit();
    // BBS: no gap
    m_btn_margin = 0; // std::lround(0.3 * em);
    m_line_margin = std::lround(0.1 * em);

    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    this->SetSizer(m_sizer);

    m_buttons_sizer = new wxFlexGridSizer(1, m_btn_margin, m_btn_margin);
    m_sizer->Add(m_buttons_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    if (side_tools != NULL) {
        m_sizer->AddStretchSpacer(1);
        for (size_t idx = 0; idx < side_tools->GetItemCount(); idx++) {
            wxSizerItem* item = side_tools->GetItem(idx);
            wxWindow* item_win = item->GetWindow();
            if (item_win) {
                item_win->Reparent(this);
            }
        }
        m_sizer->Add(side_tools, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, m_btn_margin);
    }

    // BBS: disable custom paint
    //this->Bind(wxEVT_PAINT, &ButtonsListCtrl::OnPaint, this);
    Bind(wxEVT_SYS_COLOUR_CHANGED, [](auto& e){
    });
}

void ButtonsListCtrl::OnPaint(wxPaintEvent&)
{
    //Slic3r::GUI::wxGetApp().UpdateDarkUI(this);
    const wxSize sz = GetSize();
    wxPaintDC dc(this);

    if (m_selection < 0 || m_selection >= (int)m_pageButtons.size())
        return;

    // The tab strip is chrome: dark in both colour modes. The selected tab is marked by a
    // 2 px aluminium edge, not by an accent fill (docs/design/BRAND.md §3.2).
    const wxColour default_btn_bg = StateColor::darkModeColorFor(wxColour("#3B4446"));
    const wxColour edge_color(Slic3r::GUI::Theme::ALU_50);
    const wxColour line_color(Slic3r::GUI::Theme::LINE);

    for (int idx = 0; idx < int(m_pageButtons.size()); idx++) {
        Button* btn = m_pageButtons[idx];
        wxPoint pos = btn->GetPosition();
        wxSize size = btn->GetSize();
        dc.SetPen(default_btn_bg);
        dc.SetBrush(default_btn_bg);
        dc.DrawRectangle(pos.x, pos.y + size.y, size.x, sz.y - size.y);
    }
    dc.SetPen(line_color);
    dc.SetBrush(line_color);
    dc.DrawRectangle(0, sz.y - m_line_margin, sz.x, m_line_margin);

    Button* sel_btn = m_pageButtons[m_selection];
    const int edge  = std::max(m_line_margin, FromDIP(2));
    dc.SetPen(edge_color);
    dc.SetBrush(edge_color);
    dc.DrawRectangle(sel_btn->GetPosition().x, sz.y - edge, sel_btn->GetSize().x, edge);
}

void ButtonsListCtrl::UpdateMode()
{
    //m_mode_sizer->SetMode(Slic3r::GUI::wxGetApp().get_mode());
}

void ButtonsListCtrl::Rescale()
{
    //m_mode_sizer->msw_rescale();
    int em = em_unit(this);
    for (Button* btn : m_pageButtons) {
        //BBS
        btn->SetMinSize({(btn->GetLabel().empty() ? 40 : 132) * em / 10, 36 * em / 10});
        btn->Rescale();
    }

    // BBS: no gap
    //m_btn_margin = std::lround(0.3 * em);
    //m_line_margin = std::lround(0.1 * em);
    //m_buttons_sizer->SetVGap(m_btn_margin);
    //m_buttons_sizer->SetHGap(m_btn_margin);

    m_sizer->Layout();
}

void ButtonsListCtrl::SetSelection(int sel)
{
    if (m_selection == sel && sel >= 0 && sel < static_cast<int>(m_pageButtons.size()))
        return;
    if (m_selection >= 0 && m_selection < static_cast<int>(m_pageButtons.size()))
        apply_tab_style(m_pageButtons[m_selection], false);

    if (sel < 0 || sel >= static_cast<int>(m_pageButtons.size())) {
        m_selection = -1;
        Refresh();
        return;
    }

    m_selection = sel;

    apply_tab_style(m_pageButtons[m_selection], true);
    
    Refresh();
}

bool ButtonsListCtrl::InsertPage(size_t n, const wxString &text, bool bSelect /* = false*/, const std::string &bmp_name /* = ""*/, const wxBitmap &bmp /* = wxNullBitmap */)
{
    Button * btn = new Button(this, text, bmp_name, wxNO_BORDER);
    btn->SetCornerRadius(0);

    if (bmp_name.empty() && bmp.IsOk())
        btn->SetIcon(bmp);

    // The label no longer carries a leading space, so widen the icon<->text gap to keep the
    // original spacing between a tab's icon and its caption.
    {
        wxClientDC dc(btn);
        dc.SetFont(btn->GetFont());
        int space_w = 0;
        dc.GetTextExtent(" ", &space_w, nullptr);
        btn->SetIconSpacing(5 + space_w);
    }

    int em = em_unit(this);
    //BBS set size for button
    btn->SetMinSize({(text.empty() ? 40 : 136) * em / 10, 36 * em / 10});

    apply_tab_style(btn, false);
    btn->Bind(wxEVT_BUTTON, [this, btn](wxCommandEvent& event) {
        if (auto it = std::find(m_pageButtons.begin(), m_pageButtons.end(), btn); it != m_pageButtons.end()) {
            auto sel = it - m_pageButtons.begin();
            //do it later
            //SetSelection(sel);
            
            wxCommandEvent evt = wxCommandEvent(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED);
            evt.SetId(sel);
            wxPostEvent(this->GetParent(), evt);
        }
    });
    Slic3r::GUI::wxGetApp().UpdateDarkUI(btn);
    m_pageButtons.insert(m_pageButtons.begin() + n, btn);
    m_pageLabels.insert(m_pageLabels.begin() + n, text); // ORCA
    m_pageIcons.insert(m_pageIcons.begin() + n, bmp_name);
    m_buttons_sizer->Insert(n, new wxSizerItem(btn));
    m_buttons_sizer->SetCols(m_buttons_sizer->GetCols() + 1);
    m_sizer->Layout();
    return true;
}

void ButtonsListCtrl::RemovePage(size_t n)
{
    if (n >= m_pageButtons.size())
        return;

    if (m_selection == static_cast<int>(n))
        m_selection = -1;
    else if (m_selection > static_cast<int>(n))
        --m_selection;

    Button* btn = m_pageButtons[n];
    m_pageButtons.erase(m_pageButtons.begin() + n);
    m_pageLabels.erase(m_pageLabels.begin() + n); // ORCA
    m_pageIcons.erase(m_pageIcons.begin() + n);
    m_buttons_sizer->Remove(n);
#if __WXOSX__
    RemoveChild(btn);
#else
    btn->Reparent(nullptr);
#endif
    btn->Destroy();
    m_sizer->Layout();
}

bool ButtonsListCtrl::SetPageImage(size_t n, const std::string& bmp_name) const
{
    if (n >= m_pageButtons.size())
        return false;
     
    // BBS
    //return m_pageButtons[n]->SetBitmap_(bmp_name);
    ScalableBitmap bitmap(NULL, bmp_name);
    //m_pageButtons[n]->SetBitmap_(bitmap);
    return true;
}

void ButtonsListCtrl::SetPageText(size_t n, const wxString& strText)
{
    Button* btn = m_pageButtons[n];
    btn->SetLabel(strText);
    if(!strText.empty())  // ORCA
        m_pageLabels[n] = strText;
}

// ORCA
void ButtonsListCtrl::SetCompact(size_t n, bool compact)
{
    int em = em_unit(this);
    Button* btn = m_pageButtons[n];
    btn->SetMinSize({(compact ? 40 : 136) * em / 10, 36 * em / 10});
    btn->SetLabel(compact ? "" : m_pageLabels[n]);
}

wxString ButtonsListCtrl::GetPageText(size_t n) const
{
    Button* btn = m_pageButtons[n];
    return btn->GetLabel();
}

// ORCA
wxString ButtonsListCtrl::GetPageLabel(size_t n) const
{
    return n < m_pageLabels.size() ? m_pageLabels[n] : wxString();
}

// ORCA
void ButtonsListCtrl::SetOverflowButton(wxWindow* button)
{
    if (m_overflow_button == button)
        return;

    if (m_overflow_button != nullptr)
        m_sizer->Detach(m_overflow_button);

    m_overflow_button = button;

    if (m_overflow_button != nullptr)
        // Right after the tab buttons (index 0), ahead of any stretch spacer / side_tools.
        m_sizer->Insert(1, m_overflow_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, m_btn_margin);

    m_sizer->Layout();
}

//#endif // _WIN32

void Notebook::Init()
{
    // We don't need any border as we don't have anything to separate the
    // page contents from.
    SetInternalBorder(0);

    // No effects by default.
    m_showEffect = m_hideEffect = wxSHOW_EFFECT_NONE;

    m_showTimeout = m_hideTimeout = 0;

    m_pageNames.clear();

    /* On Linux, Gstreamer wxMediaCtrl does not seem to get along well with
     * 32-bit X11 visuals (the overlay does not work).  Is this a wxWindows
     * bug?  Is this a Gstreamer bug?  No idea, but it is our problem ... 
     * and anyway, this transparency thing just isn't all that interesting,
     * so we just don't do it on Linux. 
     */
#ifndef __WXGTK__
    SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
#endif
}
