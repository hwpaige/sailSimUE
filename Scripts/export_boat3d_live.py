#!/usr/bin/env python3
"""Re-loft a live boat spec via sail_geom (Phase 3.3 Python oracle).

Usage:
  python3 Scripts/export_boat3d_live.py Content/Data/live_spec.json

Reads a JSON written by SailSimUE (imperial effective dims), runs sail_geom.boat3d,
writes Content/Data/live_boat3d.json for the ProceduralMesh loader.
"""
from __future__ import annotations

import json
import os
import sys

# Reuse mesh packing from catalog exporter
sys.path.insert(0, os.path.dirname(__file__))
from export_boat3d_presets import (  # noqa: E402
    PRESETS,
    ROOT,
    export_one,
)

# Monkey-patch export path: export_one always writes <id>_boat3d.json.
# We'll call a local variant.


def build_spec_from_live(live: dict) -> tuple[str, dict]:
    base_id = str(live.get("preset_id") or live.get("id") or "j105").lower()
    if base_id not in PRESETS:
        base_id = "j105"
    spec = json.loads(json.dumps(PRESETS[base_id]))  # deep copy

    # Apply effective dimensions from UE
    def f(key, default=None):
        if key in live and live[key] is not None:
            return float(live[key])
        return default

    loa = f("loa", spec["loa"])
    lwl = f("lwl", spec["lwl"])
    beam = f("beam", spec["beam"])
    draft = f("draft", spec["draft"])
    disp = f("disp_lb", f("disp", spec["disp"]))
    ballast = f("ballast_lb", f("ballast", spec["ballast"]))
    sa = f("sail_area", spec["sail_area"])
    I = f("I", spec["I"])
    J = f("J", spec["J"])
    P = f("P", spec["P"])
    E = f("E", spec["E"])

    spec.update(
        {
            "name": live.get("name") or spec["name"],
            "loa": loa,
            "lwl": lwl,
            "beam": beam,
            "draft": draft,
            "disp": disp,
            "ballast": ballast,
            "sail_area": sa,
            "I": I,
            "J": J,
            "P": P,
            "E": E,
        }
    )
    return base_id, spec


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: export_boat3d_live.py <live_spec.json>", file=sys.stderr)
        sys.exit(2)
    spec_path = sys.argv[1]
    with open(spec_path) as fh:
        live = json.load(fh)

    base_id, spec = build_spec_from_live(live)
    # Write under live id — export_one uses preset_id for filename
    out = export_one("live", spec, n_st=56 if base_id != "endeavour" else 72, nh=14 if base_id != "endeavour" else 16)
    # export_one writes live_boat3d.json when preset_id is "live"
    print("oracle_ok", out)
    # Also stamp source
    with open(out) as fh:
        data = json.load(fh)
    data["source"] = f"sail_geom.boat3d LIVE from {base_id}"
    data["live_spec"] = live
    with open(out, "w") as fh:
        json.dump(data, fh, separators=(",", ":"))
    print("rewrote", out, "bytes", os.path.getsize(out))


if __name__ == "__main__":
    main()
