#include "CommonDialogs.hpp"
#include "I18N.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Widgets/HyperLink.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>

#include "md4c/src/md4c-html.h"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_SECONDARY_CHECK_CONFIRM, wxCommandEvent);
wxDEFINE_EVENT(EVT_SECONDARY_CHECK_CANCEL, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_NOZZLE, wxCommandEvent);

UpdateVersionDialog::UpdateVersionDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("New version of ANANAS SLICER"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    wxBoxSizer *m_sizer_top  = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_desc = new wxBoxSizer(wxVERTICAL);

    auto sm    = create_scaled_bitmap(GUI_App::symbol_name(), nullptr, 64);
    m_brand = new wxStaticBitmap(this, wxID_ANY, sm, wxDefaultPosition, FromDIP(wxSize(64, 64)));

    m_text_up_info = new Label(this, Label::Head_14, wxEmptyString, LB_AUTO_WRAP);
    m_text_up_info->SetForegroundColour(wxColour(0x26, 0x2E, 0x30));

    // Store builds get updates from the Microsoft Store: wxID_YES opens the Store
    // product page there (see the EVT_SLIC3R_VERSION_ONLINE handler) instead of GitHub.
    auto github_link = new HyperLink(this, is_running_in_msix() ? _L("Check on Microsoft Store") : _L("Check on GitHub"), "", LB_AUTO_WRAP);
    github_link->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        EndModal(wxID_YES);
    });

    m_simplebook_release_note = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSIMPLE_BORDER);
    //m_simplebook_release_note->SetSize(wxSize(FromDIP(560), FromDIP(430)));
    m_simplebook_release_note->SetMinSize(FromDIP(wxSize(640,420)));
    m_simplebook_release_note->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));

    m_scrollwindows_release_note = new wxScrolledWindow(m_simplebook_release_note, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(430)), wxVSCROLL);
    m_scrollwindows_release_note->SetScrollRate(5, FromDIP(20));
    m_scrollwindows_release_note->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));

    //webview
    m_vebview_release_note = CreateTipView(m_simplebook_release_note);
    m_vebview_release_note->SetBackgroundColour(wxColour(0xF8, 0xF8, 0xF8));
    //m_vebview_release_note->SetSize(wxSize(FromDIP(560), FromDIP(430)));
    //m_vebview_release_note->SetMinSize(wxSize(FromDIP(560), FromDIP(430)));
    //m_vebview_release_note->SetMaxSize(wxSize(FromDIP(560), FromDIP(430)));
    if (wxGetApp().app_config->get_bool("developer_mode"))
        m_vebview_release_note->EnableAccessToDevTools();

    m_vebview_release_note->Bind(wxEVT_WEBVIEW_NAVIGATING,[=, count = 0](wxWebViewEvent& event) mutable {
        count++;
        if (count == 1) {
            m_vebview_release_note->SetPage(wxString::FromUTF8(html_source), "");
        } else if (count >= 3) {
            // Launch the default browser for links clicked by the user
            wxLaunchDefaultBrowser(event.GetURL());
            event.Veto();
        }
    });

	// fs::path ph(data_dir());
	// ph /= "resources/tooltip/releasenote.html";
	// if (!fs::exists(ph)) {
	// 	ph = resources_dir();
	// 	ph /= "tooltip/releasenote.html";
	// }
	// auto url = ph.string();
	// std::replace(url.begin(), url.end(), '\\', '/');
	// url = "file:///" + url;
 //    m_vebview_release_note->LoadURL(from_u8(url));

    m_simplebook_release_note->AddPage(m_scrollwindows_release_note, wxEmptyString, false);
    m_simplebook_release_note->AddPage(m_vebview_release_note, wxEmptyString, false);

    auto sizer_button = new wxBoxSizer(wxHORIZONTAL);

    m_button_download = new Button(this, is_running_in_msix() ? _L("Open Microsoft Store") : _L("Download"));
    m_button_download->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    m_button_download->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        EndModal(wxID_YES);
    });

    m_button_skip_version = new Button(this, _L("Skip this Version"));
    m_button_skip_version->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_button_skip_version->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        wxGetApp().set_skip_version(true);
        EndModal(wxID_NO);
    });

    m_cb_stable_only = new CheckBox(this);
    m_cb_stable_only->SetValue(wxGetApp().app_config->get_bool("check_stable_update_only"));
    m_cb_stable_only->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        wxGetApp().app_config->set_bool("check_stable_update_only", m_cb_stable_only->GetValue());
        e.Skip();
    });

    auto stable_only_label = new Label(this, _L("Check for stable updates only"));
    stable_only_label->SetFont(Label::Body_12);

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_button_cancel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        EndModal(wxID_NO);
    });

    //sizer_button->Add(m_remind_choice, 0, wxALL | wxEXPAND, FromDIP(5));

    sizer_button->Add(m_cb_stable_only     , 0, wxALIGN_CENTER);
    sizer_button->Add(stable_only_label    , 0, wxALIGN_CENTER | wxLEFT, FromDIP(5));
    sizer_button->AddStretchSpacer();
    sizer_button->Add(m_button_download    , 0, wxLEFT, FromDIP(10));
    sizer_button->Add(m_button_skip_version, 0, wxLEFT, FromDIP(10));
    sizer_button->Add(m_button_cancel      , 0, wxLEFT, FromDIP(10));

    m_sizer_desc->AddStretchSpacer();
    m_sizer_desc->Add(m_text_up_info, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
    m_sizer_desc->Add(github_link);
    m_sizer_desc->AddStretchSpacer();

    m_sizer_top->Add(m_brand     , 0, wxRIGHT  | wxALIGN_CENTER_VERTICAL, FromDIP(15));
    m_sizer_top->Add(m_sizer_desc, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

    m_sizer_main->Add(m_line_top               , 0, wxEXPAND);
    m_sizer_main->Add(m_sizer_top              , 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(15));
    m_sizer_main->Add(m_simplebook_release_note, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(15));
    m_sizer_main->Add(sizer_button             , 0, wxEXPAND | wxALL                   , FromDIP(15));

    SetSizer(m_sizer_main);
    Layout();
    Fit();

    SetMinSize(GetSize());

    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

UpdateVersionDialog::~UpdateVersionDialog() {}


wxWebView* UpdateVersionDialog::CreateTipView(wxWindow* parent)
{
	wxWebView* tipView = WebView::CreateWebView(parent, "");
	tipView->Bind(wxEVT_WEBVIEW_LOADED, &UpdateVersionDialog::OnLoaded, this);
	tipView->Bind(wxEVT_WEBVIEW_NAVIGATED, &UpdateVersionDialog::OnTitleChanged, this);
	tipView->Bind(wxEVT_WEBVIEW_ERROR, &UpdateVersionDialog::OnError, this);
	return tipView;
}

void UpdateVersionDialog::OnLoaded(wxWebViewEvent& event)
{
    event.Skip();
}

void UpdateVersionDialog::OnTitleChanged(wxWebViewEvent& event)
{
    //ShowReleaseNote();
    event.Skip();
}
void UpdateVersionDialog::OnError(wxWebViewEvent& event)
{
    event.Skip();
}

static std::string url_encode(const std::string& value) {
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;
	for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
		std::string::value_type c = (*i);

		// Keep alphanumeric and other accepted characters intact
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			escaped << c;
			continue;
		}

		// Any other characters are percent-encoded
		escaped << std::uppercase;
		escaped << '%' << std::setw(2) << int((unsigned char)c);
		escaped << std::nouppercase;
	}
	return escaped.str();
}

bool UpdateVersionDialog::ShowReleaseNote(std::string content)
{
	auto script = "window.showMarkdown('" + url_encode(content) + "', true);";
    RunScript(script);
    return true;
}

void UpdateVersionDialog::RunScript(std::string script)
{
    WebView::RunScript(m_vebview_release_note, script);
    script.clear();
}

void UpdateVersionDialog::on_dpi_changed(const wxRect &suggested_rect) {
    m_button_download->Rescale();
    m_button_skip_version->Rescale();
    m_button_cancel->Rescale();
}

std::vector<std::string> UpdateVersionDialog::splitWithStl(std::string str,std::string pattern)
{
    std::string::size_type pos;
    std::vector<std::string> result;
    str += pattern;
    int size = str.size();
    for (int i = 0; i < size; i++)
    {
        pos = str.find(pattern, i);
        if (pos < size)
        {
            std::string s = str.substr(i, pos - i);
            result.push_back(s);
            i = pos + pattern.size() - 1;
        }
    }
    return result;
}

void UpdateVersionDialog::update_version_info(wxString release_note, wxString version)
{
    //bbs check whether the web display is used
    bool use_web_link = false;
    url_line          = "";
    // Orca: not used in Orca Slicer
    // auto split_array = splitWithStl(release_note.ToStdString(), "###");
    // if (split_array.size() >= 3) {
    //     for (auto i = 0; i < split_array.size(); i++) {
    //         std::string url = split_array[i];
    //         if (std::strstr(url.c_str(), "http://") != NULL || std::strstr(url.c_str(), "https://") != NULL) {
    //             use_web_link = true;
    //             url_line     = url;
    //             break;
    //         }
    //     }
    // }

    // if (use_web_link) {
    //     m_brand->Hide();
    //     m_text_up_info->Hide();
    //     m_simplebook_release_note->SetSelection(1);
    //     m_vebview_release_note->LoadURL(from_u8(url_line));
    // }
    // else {
    //m_simplebook_release_note->SetMaxSize(wxSize(FromDIP(560), FromDIP(430)));
    m_simplebook_release_note->SetSelection(1);
    if (is_running_in_msix())
        m_text_up_info->SetLabel(wxString::Format(_L("New version available: %s. Please update ANANAS SLICER from the Microsoft Store."), version));
    else
        m_text_up_info->SetLabel(wxString::Format(_L("Click to download new version in default browser: %s"), version));
    auto data_buf_in = release_note.utf8_str();
    auto bg_color = StateColor::darkModeColorFor(wxColour("#FFFFFF")).GetAsString();
    auto fg_color = StateColor::darkModeColorFor(wxColour("#262E30")).GetAsString();
    auto style    = "body {color:" + fg_color + "; background-color:" + bg_color + "; font-family:sans-serif}"
                  + "a    {color:" + StateColor::darkModeColorFor(wxColour("#3D4953")).GetAsString(wxC2S_HTML_SYNTAX) + "}" // theme accent, matches hyperlink colors
                  + "img  {max-width:100%; height:auto}"  // fixes overflowing images
                  + "ul   {padding-inline-start: 20px}";  // reduce left padding on list items
    html_source = (boost::format("<html><head><style>%1%</style></head><body>") % style).str();
    md_html(data_buf_in.data(), data_buf_in.length(), [](const MD_CHAR* text, MD_SIZE size, void* userdata) {
        std::string* out_buf = (std::string*)userdata;
        out_buf->append(text, size);
    }, (void*) &html_source, MD_DIALECT_GITHUB | MD_FLAG_STRIKETHROUGH | MD_FLAG_WIKILINKS, 0);
    html_source.append("</body></html>");
    m_vebview_release_note->LoadURL(file_url_from_path(boost::filesystem::path(resources_dir()) / "web/guide/0/index.html"));

    SetMinSize(GetSize());
    SetMaxSize(GetSize());
    // }

    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    Fit();
}

ConfirmBeforeSendDialog::ConfirmBeforeSendDialog(wxWindow* parent, wxWindowID id, const wxString& title, enum VisibleButtons btn_style, const wxPoint& pos, const wxSize& size, long style, bool not_show_again_check)
    :DPIDialog(parent, id, title, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    m_sizer_main = new wxBoxSizer(wxVERTICAL);
    auto        m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));
    m_sizer_main->Add(m_line_top, 0, wxEXPAND, 0);
    m_sizer_main->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxBoxSizer* m_sizer_right = new wxBoxSizer(wxVERTICAL);

    m_sizer_right->Add(0, 0, 1, wxTOP, FromDIP(15));

    m_vebview_release_note = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_vebview_release_note->SetScrollRate(0, FromDIP(20));
    m_vebview_release_note->SetBackgroundColour(*wxWHITE);
    m_vebview_release_note->SetMinSize(wxSize(FromDIP(400), FromDIP(380)));
    m_sizer_right->Add(m_vebview_release_note, 0, wxEXPAND | wxRIGHT | wxLEFT, FromDIP(15));


    auto bottom_sizer = new wxBoxSizer(wxVERTICAL);
    auto sizer_button = new wxBoxSizer(wxHORIZONTAL);

    if (not_show_again_check) {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_show_again_checkbox = new wxCheckBox(this, wxID_ANY, _L("Don't show again"), wxDefaultPosition, wxDefaultSize, 0);
        m_show_again_checkbox->Bind(wxEVT_COMMAND_CHECKBOX_CLICKED, [this](wxCommandEvent& e) {
            not_show_again = !not_show_again;
            m_show_again_checkbox->SetValue(not_show_again);
        });
        checkbox_sizer->Add(FromDIP(15), 0, 0, 0);
        checkbox_sizer->Add(m_show_again_checkbox, 0, wxALL, FromDIP(5));
        bottom_sizer->Add(checkbox_sizer, 0, wxBOTTOM | wxEXPAND, 0);
    }
    m_button_ok = new Button(this, _L("Confirm"));
    m_button_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    m_button_ok->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        wxCommandEvent evt(EVT_SECONDARY_CHECK_CONFIRM, GetId());
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(evt);
        this->on_hide();
    });

    m_button_cancel = new Button(this, _L("Cancel"));
    m_button_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_button_cancel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        wxCommandEvent evt(EVT_SECONDARY_CHECK_CANCEL);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(evt);
        this->on_hide();
        });

    if (btn_style != CONFIRM_AND_CANCEL)
        m_button_cancel->Hide();
    else
        m_button_cancel->Show();

    m_button_update_nozzle = new Button(this, _L("Confirm and Update Nozzle"));
    m_button_update_nozzle->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    m_button_update_nozzle->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        wxCommandEvent evt(EVT_UPDATE_NOZZLE);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(evt);
        this->on_hide();
    });

    m_button_update_nozzle->Hide();

    sizer_button->AddStretchSpacer();
    sizer_button->Add(m_button_ok, 0, wxALL, FromDIP(5));
    sizer_button->Add(m_button_update_nozzle, 0, wxALL, FromDIP(5));
    sizer_button->Add(m_button_cancel, 0, wxALL, FromDIP(5));
    sizer_button->Add(FromDIP(5),0, 0, 0);
    bottom_sizer->Add(sizer_button, 0, wxEXPAND | wxRIGHT | wxLEFT, 0);


    m_sizer_right->Add(bottom_sizer, 0, wxEXPAND | wxRIGHT | wxLEFT, FromDIP(20));
    m_sizer_right->Add(0, 0, 0, wxTOP, FromDIP(10));

    m_sizer_main->Add(m_sizer_right, 0, wxBOTTOM | wxEXPAND, FromDIP(5));

    Bind(wxEVT_CLOSE_WINDOW, [this](auto& e) {this->on_hide(); });

    SetSizer(m_sizer_main);
    Layout();
    m_sizer_main->Fit(this);

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void ConfirmBeforeSendDialog::update_text(wxString text)
{
    wxBoxSizer* sizer_text_release_note = new wxBoxSizer(wxVERTICAL);
    if (!m_staticText_release_note){
        m_staticText_release_note = new Label(m_vebview_release_note, text, LB_AUTO_WRAP);
        wxBoxSizer* top_blank_sizer = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* bottom_blank_sizer = new wxBoxSizer(wxVERTICAL);
        top_blank_sizer->Add(FromDIP(5), 0, wxALIGN_CENTER | wxALL, FromDIP(5));
        bottom_blank_sizer->Add(FromDIP(5), 0, wxALIGN_CENTER | wxALL, FromDIP(5));

        sizer_text_release_note->Add(top_blank_sizer, 0, wxALIGN_CENTER | wxALL, FromDIP(5));
        sizer_text_release_note->Add(m_staticText_release_note, 0, wxALIGN_CENTER, FromDIP(5));
        sizer_text_release_note->Add(bottom_blank_sizer, 0, wxALIGN_CENTER | wxALL, FromDIP(5));
        m_vebview_release_note->SetSizer(sizer_text_release_note);
    }
    m_staticText_release_note->SetMaxSize(wxSize(FromDIP(380), -1));
    m_staticText_release_note->SetMinSize(wxSize(FromDIP(380), -1));
    m_staticText_release_note->SetLabelText(text);
    m_vebview_release_note->Layout();

    auto text_size = m_staticText_release_note->GetBestSize();
    if (text_size.y < FromDIP(380))
        m_vebview_release_note->SetMinSize(wxSize(FromDIP(400), text_size.y + FromDIP(25)));
    else {
        m_vebview_release_note->SetMinSize(wxSize(FromDIP(400), FromDIP(380)));
    }

    Layout();
    Fit();
}

void ConfirmBeforeSendDialog::update_text(std::vector<ConfirmBeforeSendInfo> texts, bool enable_warning_clr /*= true*/)
{
    wxBoxSizer* sizer_text_release_note = new wxBoxSizer(wxVERTICAL);
    m_vebview_release_note->SetSizer(sizer_text_release_note);


    auto height = 0;
    for (auto text : texts) {

        Label* label_item = nullptr;
        if (text.wiki_url.empty())
        {
            label_item = new Label(m_vebview_release_note, text.text, LB_AUTO_WRAP);
        }
        else
        {
            label_item = new Label(m_vebview_release_note, text.text + " " + _L("Please refer to Wiki before use->"), LB_AUTO_WRAP);
            label_item->Bind(wxEVT_LEFT_DOWN, [text](wxMouseEvent& e) { wxLaunchDefaultBrowser(text.wiki_url);});
            label_item->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_HAND); });
            label_item->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_ARROW); });
        }

        if (enable_warning_clr && text.level == ConfirmBeforeSendInfo::InfoLevel::Warning)
        {
            label_item->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00));
        }

        label_item->SetMaxSize(wxSize(FromDIP(494), -1));
        label_item->SetMinSize(wxSize(FromDIP(494), -1));
        label_item->Wrap(FromDIP(494));
        label_item->Layout();

        sizer_text_release_note->Add(label_item, 0, wxALIGN_CENTER | wxALL, FromDIP(3));
        height += label_item->GetSize().y;
    }

    m_vebview_release_note->Layout();
    if (height < FromDIP(500))
        m_vebview_release_note->SetMinSize(wxSize(-1, height + FromDIP(25)));
    else {
        m_vebview_release_note->SetMinSize(wxSize(-1, FromDIP(500)));
    }

    Layout();
    Fit();
}

void ConfirmBeforeSendDialog::on_show()
{
    wxGetApp().UpdateDlgDarkUI(this);
    // recover button color
    wxMouseEvent evt_ok(wxEVT_LEFT_UP);
    m_button_ok->GetEventHandler()->ProcessEvent(evt_ok);
    wxMouseEvent evt_cancel(wxEVT_LEFT_UP);
    m_button_cancel->GetEventHandler()->ProcessEvent(evt_cancel);
    CenterOnScreen();
    this->ShowModal();
}

void ConfirmBeforeSendDialog::on_hide()
{
    if (m_show_again_checkbox != nullptr && not_show_again && show_again_config_text != "")
        wxGetApp().app_config->set(show_again_config_text, "1");
    EndModal(wxID_OK);
}

void ConfirmBeforeSendDialog::update_btn_label(wxString ok_btn_text, wxString cancel_btn_text)
{
    m_button_ok->SetLabel(ok_btn_text);
    m_button_cancel->SetLabel(cancel_btn_text);
    rescale();
}

wxString ConfirmBeforeSendDialog::format_text(wxString str, int warp)
{
    Label st (this, str);
    wxString out_txt      = str;
    wxString count_txt    = "";
    int      new_line_pos = 0;

    for (int i = 0; i < str.length(); i++) {
        auto text_size = st.GetTextExtent(count_txt);
        if (text_size.x < warp) {
            count_txt += str[i];
        } else {
            out_txt.insert(i - 1, '\n');
            count_txt = "";
        }
    }
    return out_txt;
}

ConfirmBeforeSendDialog::~ConfirmBeforeSendDialog()
{

}

void ConfirmBeforeSendDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    rescale();
}

void ConfirmBeforeSendDialog::show_update_nozzle_button(bool show)
{
    m_button_update_nozzle->Show(show);
    Layout();
}

void ConfirmBeforeSendDialog::hide_button_ok()
{
    m_button_ok->Hide();
}

void ConfirmBeforeSendDialog::edit_cancel_button_txt(const wxString& txt, bool switch_green)
{
    m_button_cancel->SetLabel(txt);

    if (switch_green)
    {
        StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(46, 56, 64), StateColor::Pressed),
                                std::pair<wxColour, int>(wxColour(46, 56, 64), StateColor::Hovered),
                                std::pair<wxColour, int>(wxColour(61, 73, 83), StateColor::Normal));
        m_button_cancel->SetBackgroundColor(btn_bg_green);
        m_button_cancel->SetBorderColor(*wxWHITE);
        m_button_cancel->SetTextColor(wxColour("#FEFEFE"));
    }
}

void ConfirmBeforeSendDialog::disable_button_ok()
{
    m_button_ok->Disable(); // ORCA enabling / disabling buttons with conditions enough to change its style
}

void ConfirmBeforeSendDialog::enable_button_ok()
{
    m_button_ok->Enable(); // ORCA enabling / disabling buttons with conditions enough to change its style
}

void ConfirmBeforeSendDialog::rescale()
{
    m_button_ok->Rescale();
    m_button_cancel->Rescale();
}

}} // namespace Slic3r::GUI
