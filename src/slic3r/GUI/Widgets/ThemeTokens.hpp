#ifndef slic3r_GUI_ThemeTokens_hpp_
#define slic3r_GUI_ThemeTokens_hpp_

// Colour tokens of the application theme (docs/design/tokens.css, docs/design/BRAND.md §3).
//
// The UI is achromatic: carbon/steel/aluminium neutrals, and colour only for signals.
// Dark mode is the native mode; light mode keeps working through the "light keys" below,
// which StateColor maps onto the dark tokens (see gDarkColors in StateColor.cpp).
//
// Rule for the light keys: a light key must never be equal to a dark token, otherwise a
// colour that was already mapped to dark would be mapped a second time.

namespace Slic3r { namespace GUI { namespace Theme {

// ---- neutral scale (dark mode values) ---------------------------------------------------
constexpr const char *CARBON_950   = "#07090B"; // chrome: title bar, tab strip, status bar
constexpr const char *CARBON_900   = "#0B0E11"; // app ground
constexpr const char *CARBON_850   = "#0F1317"; // panels: sidebar, dialogs
constexpr const char *CARBON_800   = "#141A1F"; // raised: group headers; 3D viewport clear colour
constexpr const char *CARBON_750   = "#1A2127"; // numeric fields
constexpr const char *CARBON_700   = "#222A31"; // hover, selected row
constexpr const char *LINE_SUBTLE  = "#1C232A"; // row separators
constexpr const char *LINE         = "#262F37"; // panel / section borders
constexpr const char *STEEL_600    = "#2E3840"; // strong borders, button outlines
constexpr const char *STEEL_500    = "#3D4953"; // structural lines
constexpr const char *STEEL_400    = "#5C6B77"; // field borders, disabled ink, travel moves
constexpr const char *ALU_300      = "#8B99A4"; // muted ink: units, sources, captions
constexpr const char *ALU_200      = "#B4C0C9"; // secondary ink: parameter labels
constexpr const char *ALU_100      = "#D8E0E6"; // default ink
constexpr const char *ALU_50       = "#EEF3F6"; // strong ink, focus ring, primary button fill

// ---- signals: the only chromatic colours ------------------------------------------------
constexpr const char *SIGNAL_PATH    = "#4DB9FF"; // active G-code path / playhead only
constexpr const char *SIGNAL_LIMIT   = "#FF6F5E"; // hard-limit violations, invalid input, errors
constexpr const char *SIGNAL_CAUTION = "#FFC56A"; // value close to a limit, stale data
constexpr const char *SIGNAL_NOMINAL = "#74F0A7"; // inside data graphs only

constexpr const char *SIGNAL_LIMIT_SURFACE   = "#2A1513";
constexpr const char *SIGNAL_CAUTION_SURFACE = "#2A2112";
constexpr const char *SIGNAL_PATH_SURFACE    = "#0F2433";

// ---- accent roles ----------------------------------------------------------------------
// The accent is neutral: aluminium on carbon in dark mode, steel on white in light mode.
// Code uses the LIGHT_* keys (the role), StateColor::darkModeColorFor() turns them into
// the dark token.
constexpr const char *LIGHT_ACCENT         = "#3D4953"; // steel-500   -> dark: ALU_50
constexpr const char *LIGHT_ACCENT_HOVER   = "#2E3840"; // steel-600   -> dark: ALU_100
constexpr const char *LIGHT_ACCENT_CHECKED = "#C9D1D8"; // tint        -> dark: CARBON_700
constexpr const char *LIGHT_ACCENT_FOCUSED = "#E6EBEF"; // tint        -> dark: CARBON_750
constexpr const char *LIGHT_ON_ACCENT      = "#FEFEFE"; // ink on an accent fill -> dark: CARBON_950

// RGB triplets of the accent roles, for code that needs plain numbers (ImGui, GL).
constexpr unsigned char ACCENT_LIGHT_RGB[3]       = {0x3D, 0x49, 0x53};
constexpr unsigned char ACCENT_HOVER_LIGHT_RGB[3] = {0x2E, 0x38, 0x40};
constexpr unsigned char ACCENT_DARK_RGB[3]        = {0xEE, 0xF3, 0xF6};
constexpr unsigned char ACCENT_HOVER_DARK_RGB[3]  = {0xD8, 0xE0, 0xE6};

}}} // namespace Slic3r::GUI::Theme

#endif // slic3r_GUI_ThemeTokens_hpp_
