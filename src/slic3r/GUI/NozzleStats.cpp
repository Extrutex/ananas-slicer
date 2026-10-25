#include "NozzleStats.hpp"

#include <algorithm>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI {

int getExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type)
{
    if (!preset_bundle)
        return 0;
    auto *opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionStrings>("extruder_nozzle_stats");
    if (!opt)
        return 0;
    const auto stats = get_extruder_nozzle_stats(opt->values);
    if (extruder_id < 0 || extruder_id >= (int) stats.size())
        return 0;
    const auto it = stats[extruder_id].find(volume_type);
    return it == stats[extruder_id].end() ? 0 : it->second;
}

int getExtruderNozzleCountTotal(PresetBundle *preset_bundle, int extruder_id)
{
    if (!preset_bundle)
        return 0;
    auto *opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionStrings>("extruder_nozzle_stats");
    if (!opt)
        return 0;
    const auto stats = get_extruder_nozzle_stats(opt->values);
    if (extruder_id < 0 || extruder_id >= (int) stats.size())
        return 0;
    int sum = 0;
    for (const auto &kv : stats[extruder_id])
        sum += kv.second;
    return sum;
}

void setExtruderNozzleCount(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type, int nozzle_count, bool clear_all)
{
    if (!preset_bundle || volume_type == nvtHybrid)
        return;
    auto &config = preset_bundle->printers.get_edited_preset().config;
    auto *opt    = config.option<ConfigOptionStrings>("extruder_nozzle_stats", true);
    if (!opt)
        return;

    const int extruder_count = preset_bundle->get_printer_extruder_count();
    auto      stats          = get_extruder_nozzle_stats(opt->values);
    if ((int) stats.size() < extruder_count)
        stats.resize(extruder_count);
    if (extruder_id < 0 || extruder_id >= (int) stats.size())
        return;

    if (clear_all)
        stats[extruder_id].clear();
    stats[extruder_id][volume_type] = nozzle_count;
    opt->values                     = save_extruder_nozzle_stats_to_string(stats);
}

void updateNozzleCountDisplay(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType volume_type)
{
    auto *plater = wxGetApp().plater();
    if (!preset_bundle || !plater)
        return;

    auto *max_nozzle_count = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    // A nullable-int nil is INT_MAX and would otherwise pass the gate.
    const bool support_multi_nozzle = max_nozzle_count &&
        std::any_of(max_nozzle_count->values.begin(), max_nozzle_count->values.end(),
                    [](int v) { return v > 1 && v != ConfigOptionIntsNullable::nil_value(); });

    int display_count = -1; // hides the badge
    if (support_multi_nozzle)
        display_count = volume_type == nvtHybrid ? getExtruderNozzleCountTotal(preset_bundle, extruder_id)
                                                 : getExtruderNozzleCount(preset_bundle, extruder_id, volume_type);
    plater->sidebar().set_extruder_nozzle_count(extruder_id, display_count);
}

void seedExtruderNozzleStats(PresetBundle *preset_bundle)
{
    if (!preset_bundle)
        return;
    auto *max_nozzle_count   = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    auto *nozzle_volume_type = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");

    const int extruder_count = preset_bundle->get_printer_extruder_count();
    for (int eid = 0; eid < extruder_count; ++eid) {
        NozzleVolumeType type = nvtStandard;
        if (nozzle_volume_type && eid < (int) nozzle_volume_type->values.size())
            type = NozzleVolumeType(nozzle_volume_type->values[eid]);
        // A fresh seed has no per-type breakdown to draw on, so a Hybrid extruder starts as all-Standard.
        if (type == nvtHybrid)
            type = nvtStandard;
        int count = 1;
        if (max_nozzle_count && eid < (int) max_nozzle_count->values.size() &&
            max_nozzle_count->values[eid] != ConfigOptionIntsNullable::nil_value())
            count = max_nozzle_count->values[eid];
        setExtruderNozzleCount(preset_bundle, eid, type, count, true);
    }
}

void onNozzleVolumeTypeSwitch(PresetBundle *preset_bundle, int extruder_id, NozzleVolumeType type)
{
    if (!preset_bundle || type == nvtHybrid)
        return;
    const int total = getExtruderNozzleCountTotal(preset_bundle, extruder_id);
    setExtruderNozzleCount(preset_bundle, extruder_id, type, total, true);
}

wxString get_nozzle_volume_type_name(NozzleVolumeType type)
{
    switch (type) {
    case nvtStandard: return _L("Standard");
    case nvtHighFlow: return _L("High Flow");
    case nvtHybrid: return _L("Hybrid");
    case nvtTPUHighFlow: return _L("TPU High Flow");
    case nvtE3DHighFlow: return _L("E3D High Flow");
    case nvtExtraHighFlow: return _L("Extra High Flow");
    default: return wxString();
    }
}

}} // namespace Slic3r::GUI
