"""pytest plugin that adapts the upstream OrcaSlicer regression suite to ANANAS SLICER.

Loaded into https://github.com/OrcaSlicer/orca-test-repo by run.sh (``-p ananas_slicer_orca_tests``).

ANANAS SLICER ships no Bambu Lab (BBL) vendor bundle, so every upstream test that loads a BBL
system preset, a fixture preset inheriting one, or a project whose embedded G-code uses a
Bambu-only placeholder removed from ANANAS SLICER cannot run here. Those tests are skipped with
a reason naming the dependency, so they stay visible in the report instead of vanishing.
Tests ANANAS SLICER intentionally answers differently are skipped with the divergence named.
Everything else runs unchanged and must pass.

Detection is data-driven wherever the suite exposes its inputs (case args, settings-import
recipes, module-level system preset paths), so upstream cases added later are classified
without touching this file. Only fixtures whose Bambu dependency lives inside a binary
3mf are listed by name.
"""
import json
import re
from pathlib import Path

import pytest

BBL_SYSTEM_DIR = "system/BBL/"
# Preset names from the BBL vendor bundle ("Bambu PLA Basic @BBL X1C", "Bambu Lab X1 Carbon
# 0.4 nozzle", "Bambu PLA Basic @base").
BBL_PRESET_NAME = re.compile(r"^Bambu\b|@BBL\b")

# Project fixtures whose embedded machine G-code uses scan_first_layer, a Bambu-only
# placeholder ANANAS SLICER removed (src/libslic3r/PrintConfig.cpp, e04596b255). Slicing them
# fails cleanly with "Not a variable name", which is the intended behaviour.
BBL_GCODE_MODELS = {
    "synthetic/cube_x1c_layer_range.3mf",
    "synthetic/cube_x1c_object_overrides.3mf",
}

# Cases where ANANAS SLICER's answer differs from upstream by design.
ANANAS_SLICER_DIVERGENCES = {
    "stl-without-any-preset": (
        "gcode_flavor is locked to klipper (38cbe00e58); upstream's default Marlin(legacy) flavor "
        "fails the G92 E0 validation that this case relies on, ANANAS SLICER's defaults validate"
    ),
}


def _fixture_inherits_bbl(path: Path) -> bool:
    if path.suffix != ".json" or not path.is_file():
        return False
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return False
    return isinstance(data, dict) and bool(BBL_PRESET_NAME.search(str(data.get("inherits", ""))))


def _case_reason(case: dict, repo_root: Path):
    if case["id"] in ANANAS_SLICER_DIVERGENCES:
        return "ANANAS SLICER divergence: " + ANANAS_SLICER_DIVERGENCES[case["id"]]
    if case.get("model") in BBL_GCODE_MODELS:
        return "project %s embeds Bambu-only machine G-code (scan_first_layer)" % case["model"]
    for arg in case.get("args", []):
        arg = str(arg)
        if BBL_SYSTEM_DIR in arg:
            return "loads BBL system presets, not shipped by ANANAS SLICER"
        if "{fixtures}" in arg or "{flat_fixtures}" in arg:
            for part in arg.split(";"):
                rel = part.replace("{fixtures}", "").replace("{flat_fixtures}", "").lstrip("/")
                if _fixture_inherits_bbl(repo_root / "test_projects" / "settings" / rel):
                    return "fixture preset %s inherits a BBL system preset" % rel
    return None


def _recipe_reason(recipe: dict, repo_root: Path):
    for ref in list(recipe.get("load_settings", [])) + list(recipe.get("load_filaments", [])):
        kind, _, rel = ref.partition(":")
        if kind == "sys" and rel.startswith(BBL_SYSTEM_DIR):
            return "loads BBL system preset %s, not shipped by ANANAS SLICER" % rel
        if kind == "fx" and _fixture_inherits_bbl(repo_root / "test_projects" / "settings" / rel):
            return "fixture preset %s inherits a BBL system preset" % rel
    return None


def _module_reason(module):
    for name in ("SYS_MACHINE", "SYS_PROCESS", "SYS_FILAMENT"):
        if str(getattr(module, name, "")).startswith(BBL_SYSTEM_DIR):
            return "module baseline %s is a BBL system preset, not shipped by ANANAS SLICER" % name
    return None


def pytest_collection_modifyitems(config, items):
    repo_root = Path(str(config.rootpath))
    for item in items:
        params = getattr(getattr(item, "callspec", None), "params", {})
        reason = None
        if "case" in params:
            reason = _case_reason(params["case"], repo_root)
        elif "recipe" in params:
            reason = _recipe_reason(params["recipe"], repo_root)
        # Module-wide baselines only matter to tests without their own recipe/case inputs.
        if reason is None and "case" not in params and "recipe" not in params:
            reason = _module_reason(item.module)
        if reason is not None:
            item.add_marker(pytest.mark.skip(reason="ANANAS SLICER: " + reason))
