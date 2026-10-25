#ifndef slic3r_GUI_CommonDialogs_hpp_
#define slic3r_GUI_CommonDialogs_hpp_

// Dialogs used outside the device/monitor code (version check, preferences,
// main frame).

#include <string>
#include <vector>

#include <wx/wx.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/statbmp.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/webview.h>

#include "GUI_Utils.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"

namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(EVT_SECONDARY_CHECK_CONFIRM, wxCommandEvent);
wxDECLARE_EVENT(EVT_SECONDARY_CHECK_CANCEL, wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_NOZZLE, wxCommandEvent);

class UpdateVersionDialog : public DPIDialog
{
public:
    UpdateVersionDialog(wxWindow *parent = nullptr);
    ~UpdateVersionDialog();

    wxWebView* CreateTipView(wxWindow* parent);
    void OnLoaded(wxWebViewEvent& event);
    void OnTitleChanged(wxWebViewEvent& event);
    void OnError(wxWebViewEvent& event);
    bool ShowReleaseNote(std::string content);
    void RunScript(std::string script);
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void update_version_info(wxString release_note, wxString version);
    std::vector<std::string> splitWithStl(std::string str, std::string pattern);

    wxStaticBitmap*   m_brand{nullptr};
    Label *           m_text_up_info{nullptr};
    wxWebView*        m_vebview_release_note{nullptr};
    wxSimplebook*     m_simplebook_release_note{nullptr};
    wxScrolledWindow* m_scrollwindows_release_note{nullptr};
    wxBoxSizer *      sizer_text_release_note{nullptr};
    Label *           m_staticText_release_note{nullptr};
    wxStaticBitmap*   m_bitmap_open_in_browser;
    Button*           m_button_skip_version;
    CheckBox*         m_cb_stable_only;
    Button*           m_button_download;
    Button*           m_button_cancel;
    std::string       url_line;
    std::string       html_source;
};

struct ConfirmBeforeSendInfo
{
public:
    enum InfoLevel {
        Normal = 0,
        Warning = 1
    };
    InfoLevel level;
    wxString text;
    wxString wiki_url;
    ConfirmBeforeSendInfo(const wxString& txt, const wxString& url = wxEmptyString, InfoLevel lev = Normal) : text(txt), wiki_url(url), level(lev){}
};

class ConfirmBeforeSendDialog : public DPIDialog
{
public:
    enum VisibleButtons { // ORCA VisibleButtons instead ButtonStyle 
        ONLY_CONFIRM = 0,
        CONFIRM_AND_CANCEL = 1,
        MAX_STYLE_NUM = 2
    };
    ConfirmBeforeSendDialog(
        wxWindow* parent,
        wxWindowID      id = wxID_ANY,
        const wxString& title = wxEmptyString,
        enum VisibleButtons btn_style = CONFIRM_AND_CANCEL, // ORCA VisibleButtons instead ButtonStyle 
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION,
        bool not_show_again_check = false
    );
    void update_text(wxString text);
    void update_text(std::vector<ConfirmBeforeSendInfo> texts, bool enable_warning_clr = true);
    void on_show();
    void on_hide();
    void update_btn_label(wxString ok_btn_text, wxString cancel_btn_text);
    void rescale();
    void on_dpi_changed(const wxRect& suggested_rect);
    void show_update_nozzle_button(bool show = false);
    void hide_button_ok();
    void edit_cancel_button_txt(const wxString& txt, bool switch_green = false);
    void disable_button_ok();
    void enable_button_ok();
    wxString format_text(wxString str, int warp);

    ~ConfirmBeforeSendDialog();

protected:
    wxBoxSizer* m_sizer_main;
    wxScrolledWindow* m_vebview_release_note{ nullptr };
    Label* m_staticText_release_note{ nullptr };
    Button* m_button_ok;
    Button* m_button_cancel;
    Button* m_button_update_nozzle;
    wxCheckBox* m_show_again_checkbox;
    bool not_show_again = false;
    std::string show_again_config_text = "";
};

}} // namespace Slic3r::GUI

#endif
