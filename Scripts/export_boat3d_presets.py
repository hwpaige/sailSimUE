#!/usr/bin/env python3
"""Export sail_geom.boat3d catalog presets for SailSimUE ProceduralMesh loader.

Usage:
  python3 Scripts/export_boat3d_presets.py           # all presets
  python3 Scripts/export_boat3d_presets.py j105      # one id

Writes Content/Data/<id>_boat3d.json (UE cm, Z-up).
"""
from __future__ import annotations

import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SAIL_SIM = os.path.abspath(os.path.join(ROOT, "..", "sail-sim", "backend"))
sys.path.insert(0, SAIL_SIM)

import sail_geom  # noqa: E402

FT_TO_CM = 30.48

# Mirror frontend boat-spec PRESETS (+ default trim).
PRESETS: dict[str, dict] = {
    "j105": {
        "units": "imperial",
        "name": "J/105",
        "loa": 34.4,
        "lwl": 29.5,
        "beam": 11.0,
        "disp": 7750,
        "ballast": 3340,
        "sail_area": 545,
        # J/Boats + class rules 6.4.2 (ft)
        "I": 40.6,
        "J": 13.5,
        "P": 41.5,
        "E": 14.6,
        "draft": 6.5,
        "hull": {
            "lcb": 54,
            "entry": 20,
            "transom": 66,
            "oh_fwd": 0.62,
            "transom_rake": -11.0,
            "counter_rise": 0.03,
        },
        "trim": {
            "jib_overlap": 100,
            "main_camber": 12,
            "main_draft": 45,
            "jib_camber": 14,
            "jib_draft": 38,
        },
    },
    "endeavour": {
        "units": "imperial",
        "name": "Endeavour",
        "loa": 129.6,
        "lwl": 83.3,
        "beam": 22.0,
        "disp": 320000,
        "ballast": 156000,
        "sail_area": 6500,
        "I": 135,
        "J": 48,
        "P": 155,
        "E": 42,
        "draft": 14.75,
        "hull": {
            "lcb": 55,
            "entry": 15,
            "transom": 18,
            "oh_fwd": 0.47,
            "transom_rake": 16.0,
            "counter_rise": 0.52,
        },
        "trim": {
            "jib_overlap": 100,
            "main_camber": 12,
            "main_draft": 45,
            "jib_camber": 14,
            "jib_draft": 38,
        },
    },
    "melges24": {
        "units": "imperial",
        "name": "Melges 24",
        "loa": 24.0,
        "lwl": 21.8,
        "beam": 8.2,
        "disp": 1650,
        "ballast": 1025,
        "sail_area": 260,
        "I": 28,
        "J": 8.5,
        "P": 28,
        "E": 10,
        "draft": 4.9,
        "hull": {
            "lcb": 56,
            "entry": 22,
            "transom": 70,
            "oh_fwd": 0.60,
            "transom_rake": -15.0,
            "counter_rise": 0.05,
        },
        "trim": {
            "jib_overlap": 100,
            "main_camber": 12,
            "main_draft": 45,
            "jib_camber": 14,
            "jib_draft": 38,
        },
    },
    "cruiser36": {
        "units": "imperial",
        "name": "Cruiser 36",
        "loa": 36.0,
        "lwl": 30.5,
        "beam": 12.0,
        "disp": 15000,
        "ballast": 6000,
        "sail_area": 620,
        "I": 48,
        "J": 15,
        "P": 42,
        "E": 15,
        "draft": 6.3,
        "hull": {
            "lcb": 55,
            "entry": 20,
            "transom": 45,
            "oh_fwd": 0.52,
            "transom_rake": 6.0,
            "counter_rise": 0.28,
        },
        "trim": {
            "jib_overlap": 100,
            "main_camber": 12,
            "main_draft": 45,
            "jib_camber": 14,
            "jib_draft": 38,
        },
    },
}


def grid_index(nx: int, ny: int) -> list[int]:
    idx: list[int] = []
    for i in range(nx - 1):
        for j in range(ny - 1):
            a = i * ny + j
            b = (i + 1) * ny + j
            c = (i + 1) * ny + (j + 1)
            d = i * ny + (j + 1)
            idx.extend([a, b, d, b, c, d])
    return idx


def py_verts_to_ue_cm(verts_flat, x_shift: float = 0.0) -> list[float]:
    out: list[float] = []
    for i in range(0, len(verts_flat), 3):
        out.extend(
            [
                (verts_flat[i] - x_shift) * FT_TO_CM,
                verts_flat[i + 1] * FT_TO_CM,
                verts_flat[i + 2] * FT_TO_CM,
            ]
        )
    return out


def fin_mesh(outline: dict, x_shift: float) -> dict | None:
    xs, zs = outline["x"], outline["z"]
    n = len(xs)
    if n < 3:
        return None
    xmin, xmax = min(xs), max(xs)
    th = max(0.12, min(1.2, 0.05 * (xmax - xmin)))
    pos: list[float] = []
    for s in (th, -th):
        for i in range(n):
            pos.extend(
                [
                    (xs[i] - x_shift) * FT_TO_CM,
                    s * FT_TO_CM,
                    zs[i] * FT_TO_CM,
                ]
            )
    idx: list[int] = []
    for i in range(n):
        a, b = i, (i + 1) % n
        c, d = n + i, n + (i + 1) % n
        idx.extend([a, b, d, a, d, c])
    cx = sum(xs) / n - x_shift
    cz = sum(zs) / n
    f0 = len(pos) // 3
    pos.extend([cx * FT_TO_CM, th * FT_TO_CM, cz * FT_TO_CM])
    for i in range(n):
        idx.extend([f0, i, (i + 1) % n])
    f1 = len(pos) // 3
    pos.extend([cx * FT_TO_CM, -th * FT_TO_CM, cz * FT_TO_CM])
    for i in range(n):
        idx.extend([f1, n + (i + 1) % n, n + i])
    return {"verts": pos, "indices": idx}


def pt3(v, x_shift: float) -> list[float]:
    return [
        (v[0] - x_shift) * FT_TO_CM,
        v[1] * FT_TO_CM,
        v[2] * FT_TO_CM,
    ]


def export_one(preset_id: str, spec: dict, n_st: int = 56, nh: int = 14) -> str:
    # Large J-class: slightly denser stations
    if preset_id == "endeavour":
        n_st, nh = 72, 16
    b = sail_geom.boat3d(spec, n_st=n_st, nh=nh)
    xs = b["hull"]["verts"][0::3]
    x_shift = 0.5 * (min(xs) + max(xs))

    meshes: list[dict] = []
    h = b["hull"]
    meshes.append(
        {
            "name": "hull",
            "color": [0.92, 0.93, 0.95, 1.0],
            "verts": py_verts_to_ue_cm(h["verts"], x_shift),
            "indices": grid_index(h["n_st"], h["n_sec"]),
            "double_sided": False,
        }
    )
    if b.get("deck") and b["deck"].get("verts"):
        d = b["deck"]
        idx = grid_index(d["n_st"], d["nw"])
        if b.get("transomDeckStitch") and b["transomDeckStitch"].get("indices"):
            idx = idx + list(b["transomDeckStitch"]["indices"])
        meshes.append(
            {
                "name": "deck",
                "color": [0.85, 0.87, 0.90, 1.0],
                "verts": py_verts_to_ue_cm(d["verts"], x_shift),
                "indices": idx,
                "double_sided": True,
            }
        )
    cab = b.get("cabin") or {}
    for key, col, iname in [
        ("", [0.90, 0.91, 0.93, 1.0], "cabin"),
        ("win_", [0.08, 0.10, 0.14, 1.0], "cabin_windows"),
        ("hatch_", [0.84, 0.86, 0.89, 1.0], "cabin_hatch"),
        ("hatch_frame_", [0.12, 0.08, 0.06, 1.0], "cabin_hatch_frame"),
    ]:
        vk, ik = f"{key}verts", f"{key}indices"
        if cab.get(vk) and cab.get(ik):
            meshes.append(
                {
                    "name": iname,
                    "color": col,
                    "verts": py_verts_to_ue_cm(cab[vk], x_shift),
                    "indices": list(cab[ik]),
                    "double_sided": True,
                }
            )
    ck = b.get("cockpit") or {}
    if ck.get("verts") and ck.get("indices"):
        meshes.append(
            {
                "name": "cockpit",
                "color": [0.82, 0.84, 0.87, 1.0],
                "verts": py_verts_to_ue_cm(ck["verts"], x_shift),
                "indices": list(ck["indices"]),
                "double_sided": True,
            }
        )
    wn = b.get("winches") or {}
    if wn.get("verts") and wn.get("indices"):
        meshes.append(
            {
                "name": "winches",
                "color": [0.12, 0.13, 0.14, 1.0],
                "verts": py_verts_to_ue_cm(wn["verts"], x_shift),
                "indices": list(wn["indices"]),
                "double_sided": False,
            }
        )
    bl = (wn.get("blocks") or b.get("blocks") or {})
    if bl.get("verts") and bl.get("indices"):
        meshes.append(
            {
                "name": "sheet_blocks",
                "color": [0.10, 0.10, 0.11, 1.0],
                "verts": py_verts_to_ue_cm(bl["verts"], x_shift),
                "indices": list(bl["indices"]),
                "double_sided": False,
            }
        )
    tc = b.get("transomCap") or {}
    if tc.get("verts") and tc.get("indices"):
        meshes.append(
            {
                "name": "transom",
                "color": [0.90, 0.91, 0.93, 1.0],
                "verts": py_verts_to_ue_cm(tc["verts"], x_shift),
                "indices": list(tc["indices"]),
                "double_sided": True,
            }
        )
    for name, outline, col in [
        ("keel", b.get("keel"), [0.18, 0.18, 0.20, 1.0]),
        ("rudder", b.get("rudder"), [0.22, 0.22, 0.25, 1.0]),
    ]:
        if outline:
            m = fin_mesh(outline, x_shift)
            if m:
                m["name"] = name
                m["color"] = col
                m["double_sided"] = True
                meshes.append(m)
    for s in b.get("sails") or []:
        if not s.get("verts"):
            continue
        meshes.append(
            {
                "name": f"sail_{s['name']}",
                "color": [0.96, 0.96, 0.94, 1.0],
                "verts": py_verts_to_ue_cm(s["verts"], x_shift),
                "indices": grid_index(s["nu"], s["nw"]),
                "double_sided": True,
            }
        )

    spars = {
        "mast": {
            "base": pt3(b["mast"]["base"], x_shift),
            "top": pt3(b["mast"]["top"], x_shift),
        },
        "boom": {
            "base": pt3(b["boom"]["base"], x_shift),
            "end": pt3(b["boom"]["end"], x_shift),
        },
    }
    # Running rigging (must ship with mesh — traveler/mainsheet/vang need these)
    jibsheet = None
    if b.get("jibsheet"):
        js = b["jibsheet"]
        jibsheet = {
            "port_track_fwd": pt3(js["port_track_fwd"], x_shift),
            "port_track_aft": pt3(js["port_track_aft"], x_shift),
            "stbd_track_fwd": pt3(js["stbd_track_fwd"], x_shift),
            "stbd_track_aft": pt3(js["stbd_track_aft"], x_shift),
            "foot_length": float(js.get("foot_length", 0.0)) * FT_TO_CM,
        }
    mainsheet = None
    if b.get("mainsheet"):
        ms = b["mainsheet"]
        mainsheet = {
            "gooseneck": pt3(ms["gooseneck"], x_shift),
            "lead": pt3(ms["lead"], x_shift),
            "boom_length": float(ms.get("boom_length", 0.0)) * FT_TO_CM,
            "vang_mast_drop_frac": float(ms.get("vang_mast_drop_frac", 0.06)),
            "vang_boom_frac": float(ms.get("vang_boom_frac", 0.25)),
        }
    sheet_leads = None
    sl = b.get("sheet_leads") or (b.get("winches") or {}).get("sheet_leads") or {}
    if sl:
        sheet_leads = {}
        for k, v in sl.items():
            if v and len(v) >= 3:
                sheet_leads[k] = pt3(v, x_shift)
    out = {
        "name": b["name"],
        "preset_id": preset_id,
        "units": "cm",
        "source": f"sail_geom.boat3d {b['name']}",
        "x_shift_ft": x_shift,
        "dims_ft": b["dims"],
        "sailing": b["sailing"],
        "meshes": meshes,
        "spars": spars,
        "jibsheet": jibsheet,
        "mainsheet": mainsheet,
        "sheet_leads": sheet_leads,
    }
    path = os.path.join(ROOT, "Content", "Data", f"{preset_id}_boat3d.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print("wrote", path, "bytes", os.path.getsize(path), "meshes", len(meshes))
    return path


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if args:
        ids = args
    else:
        ids = list(PRESETS.keys())
    for pid in ids:
        if pid not in PRESETS:
            print("unknown preset", pid, "known", list(PRESETS.keys()), file=sys.stderr)
            sys.exit(1)
        export_one(pid, PRESETS[pid])


if __name__ == "__main__":
    main()
