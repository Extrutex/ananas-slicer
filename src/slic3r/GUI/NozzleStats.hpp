#pragma once

#include <wx/string.h>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class PresetBundle;

namespace GUI {

// Per-extruder physical nozzle counts of a multi-nozzle extruder, kept in the edited printer
// preset's session-only `extruder_nozzle_stats` key (one encoded map per extruder, see
// get_extruder_nozzle_stats / save_extruder_nozzle_stats_to_string) and read by the
// multi-nozzle slicer (ToolOrdering::build_multi_nozzle_group_result). Every printer whose
// `extruder_max_nozzle_count` entries are all 1 keeps one nozzle per extruder and never shows
// the sidebar badge.

// Persist one extruder's count for a volume type. With clear_all the extruder's other volume
// types are reset first. Hybrid is a mix marker, not a nozzle type, and is ignored.
void setExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType type, int nozzle_count, bool clear_all);

// One extruder's count for a volume type, or its total. 0 when the stats are absent or the
// extruder is unknown.
int getExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type);
int getExtruderNozzleCountTotal(PresetBundle *preset_bundle, int extruder_id);

// Refresh the sidebar nozzle-count badge of one extruder: the physical nozzle count on a
// multi-nozzle printer, hidden (count -1) everywhere else.
void updateNozzleCountDisplay(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type);

// Reset `extruder_nozzle_stats` to its baseline for the selected printer: each extruder gets
// extruder_max_nozzle_count nozzles of its selected volume type. Called whenever the stats are
// absent, since a preset switch rebuilds the edited config without the session-only key.
void seedExtruderNozzleStats(PresetBundle *preset_bundle);

// The user switched one extruder's nozzle volume type: carry its total nozzle count over to the
// new type. No-op for Hybrid, which is a mix and not a type every nozzle shares.
void onNozzleVolumeTypeSwitch(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType type);

// The translated name of a nozzle volume type, as the settings show it.
wxString get_nozzle_volume_type_name(NozzleVolumeType type);

}} // namespace Slic3r::GUI
