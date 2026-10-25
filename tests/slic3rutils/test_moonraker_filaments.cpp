#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/MoonrakerFilaments.hpp"

using namespace Slic3r;
using Source = MoonrakerFilamentState::Source;

TEST_CASE("Colours of every changer are normalised to RRGGBBAA", "[MoonrakerFilaments]")
{
    CHECK(MoonrakerFilaments::normalize_color("#ff0000") == "FF0000FF");
    CHECK(MoonrakerFilaments::normalize_color("0x00ff00") == "00FF00FF");
    CHECK(MoonrakerFilaments::normalize_color("1122AABB") == "1122AABB");
    CHECK(MoonrakerFilaments::normalize_color(" #ABCDEF ") == "ABCDEFFF");
    CHECK(MoonrakerFilaments::normalize_color("") == "00000000");
    CHECK(MoonrakerFilaments::normalize_color("red") == "00000000");
}

TEST_CASE("Material names are trimmed and uppercased, Qidi names collapse to the family", "[MoonrakerFilaments]")
{
    CHECK(MoonrakerFilaments::normalize_material(" petg ") == "PETG");
    CHECK(MoonrakerFilaments::normalize_material("PLA Rapido", true) == "PLA");
    CHECK(MoonrakerFilaments::normalize_material("Nylon-CF", true) == "PA");
    CHECK(MoonrakerFilaments::normalize_material("PA-CF") == "PA-CF");
}

TEST_CASE("The base URL keeps the user's scheme and adds the port only when there is none", "[MoonrakerFilaments]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("print_host", new ConfigOptionString("voron.local"));
    config.set_key_value("printhost_port", new ConfigOptionString(""));
    CHECK(MoonrakerFilaments::base_url(config) == "http://voron.local");

    config.set_key_value("printhost_port", new ConfigOptionString("7125"));
    CHECK(MoonrakerFilaments::base_url(config) == "http://voron.local:7125");

    config.set_key_value("print_host", new ConfigOptionString("https://printer.example.org/"));
    CHECK(MoonrakerFilaments::base_url(config) == "https://printer.example.org");

    config.set_key_value("print_host", new ConfigOptionString("192.168.1.20:80"));
    CHECK(MoonrakerFilaments::base_url(config) == "http://192.168.1.20:80");

    config.set_key_value("print_host", new ConfigOptionString("   "));
    CHECK(MoonrakerFilaments::base_url(config).empty());
}

TEST_CASE("AFC lane data becomes slots in lane order with empty lanes kept", "[MoonrakerFilaments]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"namespace":"lane_data","value":{
        "lane2":{"lane":"1","material":"PETG","color":"#00ff00","bed_temp":80,"nozzle_temp":240},
        "lane1":{"lane":"0","material":"PLA","color":"ff0000","bed_temp":"60","nozzle_temp":"210"},
        "lane3":{"lane":"2","material":"","color":""}}}})");
    MoonrakerFilamentState state;
    REQUIRE(MoonrakerFilaments::parse_lane_data(reply, state));
    CHECK(state.source == Source::LaneData);
    REQUIRE(state.slots.size() == 3);
    CHECK(state.slot_count() == 3);
    CHECK(state.loaded_count() == 2);
    CHECK(state.slots[0].index == 0);
    CHECK(state.slots[0].material == "PLA");
    CHECK(state.slots[0].color == "FF0000FF");
    CHECK(state.slots[0].bed_temp == 60);
    CHECK(state.slots[0].nozzle_temp == 210);
    CHECK(state.slots[1].material == "PETG");
    CHECK(state.slots[1].bed_temp == 80);
    CHECK_FALSE(state.slots[2].loaded);

    const auto empty = nlohmann::json::parse(R"({"result":{"namespace":"lane_data","value":{}}})");
    CHECK_FALSE(MoonrakerFilaments::parse_lane_data(empty, state));
    const auto missing = nlohmann::json::parse(R"({"error":{"code":404,"message":"Namespace 'lane_data' not found"}})");
    CHECK_FALSE(MoonrakerFilaments::parse_lane_data(missing, state));
}

TEST_CASE("Happy Hare gates map to slots, unknown and empty gates stay as placeholders", "[MoonrakerFilaments]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"status":{"mmu":{
        "num_gates":4,
        "gate_status":[1,0,-1,2],
        "gate_material":["PLA","","ABS","ASA"],
        "gate_color":["ff8800","","","0000ff"],
        "gate_temperature":[210,0,0,250]}}}})");
    MoonrakerFilamentState state;
    REQUIRE(MoonrakerFilaments::parse_happy_hare(reply, state));
    CHECK(state.source == Source::HappyHare);
    REQUIRE(state.slots.size() == 4);
    CHECK(state.loaded_count() == 2);
    CHECK(state.slots[0].loaded);
    CHECK(state.slots[0].material == "PLA");
    CHECK(state.slots[0].color == "FF8800FF");
    CHECK(state.slots[0].nozzle_temp == 210);
    CHECK_FALSE(state.slots[1].loaded);
    CHECK_FALSE(state.slots[2].loaded); // status -1 is unknown, never loaded
    CHECK(state.slots[3].loaded);
    CHECK(state.slots[3].material == "ASA");

    const auto no_mmu = nlohmann::json::parse(R"({"result":{"status":{"mmu":{}}}})");
    CHECK_FALSE(MoonrakerFilaments::parse_happy_hare(no_mmu, state));
}

TEST_CASE("A Qidi box reads its slot variables, runout sensors and the filament dictionary", "[MoonrakerFilaments]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"status":{
        "save_variables":{"variables":{"box_count":1,
            "color_slot0":2,"filament_slot0":1,"vendor_slot0":0,
            "color_slot1":1,"filament_slot1":11,"vendor_slot1":1,
            "color_slot2":1,"filament_slot2":1,"vendor_slot2":0,
            "color_slot3":1,"filament_slot3":1,"vendor_slot3":0}},
        "box_stepper slot0":{"runout_button":0},
        "box_stepper slot1":{"runout_button":0},
        "box_stepper slot2":{"runout_button":1},
        "box_stepper slot3":{"runout_button":null}}}})");
    const std::string cfg = "[colordict]\n1 = FFFFFF\n2 = 00FF00\n\n[fila1]\nfilament = PLA Rapido\n\n[fila11]\nfilament = ABS\n";
    MoonrakerFilamentState state;
    REQUIRE(MoonrakerFilaments::parse_qidi_box(reply, cfg, state));
    CHECK(state.source == Source::QidiBox);
    REQUIRE(state.slots.size() == 4);
    CHECK(state.loaded_count() == 2);
    CHECK(state.slots[0].material == "PLA");
    CHECK(state.slots[0].color == "00FF00FF");
    CHECK(state.slots[1].material == "ABS");
    CHECK(state.slots[1].color == "FFFFFFFF");
    CHECK_FALSE(state.slots[2].loaded);
    CHECK_FALSE(state.slots[3].loaded);

    const auto no_box = nlohmann::json::parse(R"({"result":{"status":{"save_variables":{"variables":{"foo":1}}}}})");
    CHECK_FALSE(MoonrakerFilaments::parse_qidi_box(no_box, cfg, state));
}

TEST_CASE("A Snapmaker toolhead combines type and sub type and takes NFC temperatures", "[MoonrakerFilaments]")
{
    const auto reply = nlohmann::json::parse(R"({"result":{"status":{
        "print_task_config":{
            "filament_exist":[true,false,true],
            "filament_type":["PLA","","PETG"],
            "filament_sub_type":["SnapSpeed","","CF"],
            "filament_color_rgba":["FF0000FF","","0000FFFF"],
            "filament_vendor":["Snapmaker","","Polymaker"]},
        "filament_detect":{"info":[{"VENDOR":"Snapmaker","BED_TEMP":60,"FIRST_LAYER_TEMP":215},{"VENDOR":"NONE"},{"VENDOR":"NONE"}]}}}})");
    MoonrakerFilamentState state;
    REQUIRE(MoonrakerFilaments::parse_snapmaker(reply, state));
    CHECK(state.source == Source::Snapmaker);
    REQUIRE(state.slots.size() == 3);
    CHECK(state.slots[0].material == "PLA HIGH SPEED");
    CHECK(state.slots[0].vendor == "Snapmaker");
    CHECK(state.slots[0].bed_temp == 60);
    CHECK(state.slots[0].nozzle_temp == 215);
    CHECK_FALSE(state.slots[1].loaded);
    CHECK(state.slots[2].material == "PETG-CF");
    CHECK(state.slots[2].nozzle_temp == 0);

    const auto no_toolhead = nlohmann::json::parse(R"({"result":{"status":{}}})");
    CHECK_FALSE(MoonrakerFilaments::parse_snapmaker(no_toolhead, state));
}

TEST_CASE("Filament ids resolve by vendor and colour first, then by type, else stay unknown", "[MoonrakerFilaments]")
{
    PresetCollection filaments(Preset::TYPE_FILAMENT, Preset::filament_options(),
                               static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()));
    auto add = [&](const std::string &name, const std::string &vendor, const std::string &type, const std::string &colour, const std::string &id) {
        DynamicPrintConfig config(filaments.default_preset().config);
        config.set_key_value("filament_vendor", new ConfigOptionStrings{vendor});
        config.set_key_value("filament_type", new ConfigOptionStrings{type});
        config.set_key_value("default_filament_colour", new ConfigOptionStrings{colour});
        Preset &preset         = filaments.load_preset(std::string(), name, config, /*select=*/false);
        preset.is_system       = true;
        preset.is_visible      = true;
        preset.is_compatible   = true;
        preset.filament_id     = id;
    };
    add("Generic PLA @System", "Generic", "PLA", "#FFFFFF", "OFGENPLA");
    add("Generic PETG @System", "Generic", "PETG", "#FFFFFF", "OFGENPTG");
    add("Polymaker PolyLite PLA Red @System", "Polymaker", "PLA", "#FF0000", "OFPOLRED");
    add("Polymaker PolyLite PLA Blue @System", "Polymaker", "PLA", "#0000FF", "OFPOLBLU");

    MoonrakerFilamentState state;
    state.source = Source::Snapmaker;
    state.slots  = {
        {0, true, "PLA", "EE1010FF", "Polymaker"},
        {1, true, "PETG", "00FF00FF", ""},
        {2, true, "PLA HIGH SPEED", "00FF00FF", ""},
        {3, true, "PVB", "00FF00FF", ""},
        {4, false, "", "00000000", ""},
    };
    MoonrakerFilaments::resolve_filament_ids(state, filaments);
    CHECK(state.slots[0].filament_id == "OFPOLRED");
    CHECK(state.slots[1].filament_id == "OFGENPTG");
    CHECK(state.slots[2].filament_id == "OFGENPLA");
    CHECK(state.slots[3].filament_id == UNKNOWN_FILAMENT_ID);
    CHECK(state.slots[4].filament_id.empty());
}
