#pragma once

#include <wx/panel.h>

#include "slic3r/Utils/MoonrakerStatus.hpp"

class Label;

namespace Slic3r { namespace GUI {

// One line above the web Device tab: whether the printer answers, what it is doing and how
// hot it is. Fed by MoonrakerStatus; the browser below stays the full printer UI.
class PrinterStatusStrip : public wxPanel
{
public:
    explicit PrinterStatusStrip(wxWindow *parent);

    void update(const MoonrakerPrinterStatus &status);

private:
    Label *m_state  = nullptr; // "● Printing"
    Label *m_detail = nullptr; // file, progress, layer, time
    Label *m_temps  = nullptr; // nozzle and bed
};

}} // namespace Slic3r::GUI
