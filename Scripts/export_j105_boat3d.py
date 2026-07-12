#!/usr/bin/env python3
"""Export sail_geom.boat3d J/105 mesh JSON for SailSimUE ProceduralMesh loader.

Usage (from SailSimUE or sail-sim):
  python3 Scripts/export_j105_boat3d.py
Writes Content/Data/j105_boat3d.json (UE cm, Z-up).
"""
from __future__ import annotations

import json
import os
import sys

# sail-sim backend next to SailSimUE
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SAIL_SIM = os.path.abspath(os.path.join(ROOT, "..", "sail-sim", "backend"))
sys.path.insert(0, SAIL_SIM)

import sail_geom  # noqa: E402

FT_TO_CM = 30.48


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


def main() -> None:
    spec = {
        "units": "imperial",
        "name": "J/105",
        "loa": 34.4,
        "lwl": 29.5,
        "beam": 11.0,
        "disp": 7750,
        "ballast": 3340,
        "sail_area": 545,
        "I": 44,
        "J": 13.5,
        "P": 40,
        "E": 13.5,
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
    }
    b = sail_geom.boat3d(spec, n_st=56, nh=14)
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
            "base": [
                (b["mast"]["base"][0] - x_shift) * FT_TO_CM,
                b["mast"]["base"][1] * FT_TO_CM,
                b["mast"]["base"][2] * FT_TO_CM,
            ],
            "top": [
                (b["mast"]["top"][0] - x_shift) * FT_TO_CM,
                b["mast"]["top"][1] * FT_TO_CM,
                b["mast"]["top"][2] * FT_TO_CM,
            ],
        },
        "boom": {
            "base": [
                (b["boom"]["base"][0] - x_shift) * FT_TO_CM,
                b["boom"]["base"][1] * FT_TO_CM,
                b["boom"]["base"][2] * FT_TO_CM,
            ],
            "end": [
                (b["boom"]["end"][0] - x_shift) * FT_TO_CM,
                b["boom"]["end"][1] * FT_TO_CM,
                b["boom"]["end"][2] * FT_TO_CM,
            ],
        },
    }
    out = {
        "name": b["name"],
        "units": "cm",
        "source": "sail_geom.boat3d J/105",
        "x_shift_ft": x_shift,
        "dims_ft": b["dims"],
        "sailing": b["sailing"],
        "meshes": meshes,
        "spars": spars,
    }
    path = os.path.join(ROOT, "Content", "Data", "j105_boat3d.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(out, f, separators=(",", ":"))
    print("wrote", path, "bytes", os.path.getsize(path))


if __name__ == "__main__":
    main()
