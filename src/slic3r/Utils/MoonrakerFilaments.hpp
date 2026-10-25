#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class DynamicPrintConfig;
class PresetCollection;

// One slot of the filament changer a Klipper printer reports through Moonraker.
struct MoonrakerFilamentSlot
{
    int         index       = 0;          // changer-wide, 0-based, the T number of the slot
    bool        loaded      = false;      // false: the slot exists and holds nothing
    std::string material;                 // trimmed and uppercased, as the changer reports it ("PLA", "PETG")
    std::string color       = "00000000"; // RRGGBBAA, uppercase hex
    std::string vendor;                   // as reported; empty when the changer has no vendor field
    int         nozzle_temp = 0;          // 0: not reported
    int         bed_temp    = 0;          // 0: not reported
    std::string filament_id;              // the preset filament_id resolve_filament_ids() picked, UNKNOWN_FILAMENT_ID when none fits
};

struct MoonrakerFilamentState
{
    enum class Source { None, LaneData, HappyHare, QidiBox, Snapmaker };

    Source                             source = Source::None;
    std::vector<MoonrakerFilamentSlot> slots; // index ascending, one entry per slot the changer reports

    bool empty() const { return slots.empty(); }
    int  slot_count() const { return slots.empty() ? 0 : slots.back().index + 1; }
    int  loaded_count() const;
};

// The filament changer behind a Moonraker printer, read with plain HTTP requests against the
// printer preset's host. Four changer families are recognised, probed in this order:
//   1. Moonraker database namespace `lane_data` (AFC, Happy Hare since 2026-02),
//   2. Happy Hare's `mmu` printer object,
//   3. a Qidi box (`save_variables` slot variables plus `box_stepper slotN` runout sensors),
//   4. a Snapmaker toolhead (`print_task_config`, with `filament_detect` NFC temperatures).
// The parsers are separate from the transport so they can be tested on recorded replies.
namespace MoonrakerFilaments {

// http://host[:port] from print_host and printhost_port, with the scheme the user typed kept.
// Empty when the preset has no host.
std::string base_url(const DynamicPrintConfig &printer_config);

// Reads the changer. On a transport failure `error` names it and Source::None is returned; a
// reachable printer without a known changer returns Source::None with an empty error. When
// `filaments` is given every loaded slot gets its filament_id resolved against it.
MoonrakerFilamentState fetch(const DynamicPrintConfig &printer_config, const PresetCollection *filaments, std::string &error);

// Parsers. Each takes the JSON reply of the request named in the comment, fills `out` and
// returns false when the reply does not describe that changer.
bool parse_lane_data(const nlohmann::json &reply, MoonrakerFilamentState &out);  // GET /server/database/item?namespace=lane_data
bool parse_happy_hare(const nlohmann::json &reply, MoonrakerFilamentState &out); // GET /printer/objects/query?mmu
bool parse_qidi_box(const nlohmann::json &reply, const std::string &filament_list_cfg, MoonrakerFilamentState &out);
    // GET /printer/objects/query?save_variables=variables&box_stepper%20slot0=runout_button&... and the
    // text of GET /server/files/config/officiall_filas_list.cfg
bool parse_snapmaker(const nlohmann::json &reply, MoonrakerFilamentState &out); // GET /printer/objects/query?print_task_config&filament_detect

// "#RRGGBB", "0xRRGGBB", "RRGGBBAA" and friends to RRGGBBAA; "00000000" when the input is not a colour.
std::string normalize_color(const std::string &color);
// Trimmed and uppercased; a Qidi name such as "PLA Rapido" collapses to its family ("PLA").
std::string normalize_material(const std::string &material, bool collapse_to_family = false);

// Picks a preset filament_id for every loaded slot: a visible, compatible system base preset of
// the slot's material, the vendor's closest colour match first when the changer reports a
// vendor, else UNKNOWN_FILAMENT_ID so the sync falls back to a generic preset of that type.
void resolve_filament_ids(MoonrakerFilamentState &state, const PresetCollection &filaments);

} // namespace MoonrakerFilaments

} // namespace Slic3r
