#include "PrinterWebViewHandler.hpp"

#include "PrinterWebView.hpp"

namespace Slic3r {
namespace GUI {

PrinterWebViewHandler::PrinterWebViewHandler(PrinterWebView& owner)
    : m_owner(owner)
{
}

PrinterWebViewHandler::~PrinterWebViewHandler() = default;

void PrinterWebViewHandler::on_loaded(wxWebViewEvent &evt)
{
}

void PrinterWebViewHandler::on_script_message(wxWebViewEvent &evt)
{
}

PrinterWebView& PrinterWebViewHandler::owner() const
{
    return m_owner;
}

wxWebView* PrinterWebViewHandler::browser() const
{
    return m_owner.m_browser;
}

// No print host needs a script bridge into its web UI: Moonraker front ends (Mainsail, Fluidd)
// run on their own. A null handler makes the web view a plain browser.
std::unique_ptr<PrinterWebViewHandler> create_printer_webview_handler(PrinterWebView& owner)
{
    return nullptr;
}

} // GUI
} // Slic3r
