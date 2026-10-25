#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace Slic3r { namespace GUI {

// Extruder ids of a dual-extruder printer as the GUI orders them: the main extruder sits on
// the right of the sidebar, the deputy on the left.
constexpr int MAIN_EXTRUDER_ID   = 0;
constexpr int DEPUTY_EXTRUDER_ID = 1;

enum class ToolHeadComponent { Extruder, Nozzle, Hotend };

enum class ToolHeadNameCase {
    TitleCase,    // "Right Nozzle": panel titles, section headers
    SentenceCase, // "Right nozzle": static box labels
    LowerCase     // "right nozzle": inline text
};

// The name the GUI shows for one tool head of a dual-extruder printer. Translate the result
// with _L() at the call site; the words are kept as English source strings on purpose.
inline std::string toolhead_display_name(int ext_id, ToolHeadComponent component, ToolHeadNameCase name_case, bool short_name = false)
{
    const std::string side           = ext_id == DEPUTY_EXTRUDER_ID ? "Left" : "Right";
    const std::string component_name = component == ToolHeadComponent::Extruder ? "Extruder" :
                                       component == ToolHeadComponent::Hotend   ? "Hotend" :
                                                                                  "Nozzle";
    std::string result = side + " " + component_name;
    if (name_case == ToolHeadNameCase::SentenceCase)
        result[side.size() + 1] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[side.size() + 1])));
    else if (name_case == ToolHeadNameCase::LowerCase)
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return short_name ? side : result;
}

}} // namespace Slic3r::GUI
