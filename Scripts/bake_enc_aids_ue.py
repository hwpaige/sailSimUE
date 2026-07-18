#!/usr/bin/env python3
"""Bake NOAA ENC maritime points → Content/Nav/enc_aids_nantucket.json for UEncAidSubsystem.

Maps S-57 kind / COLOUR / CATLAM / BOYSHP → SM_Buoy_1..9 mesh ids (IALA-B US):
  green/port can  → 1
  round / can alt → 2
  red/stbd nun    → 3
  mooring ball    → mix of 1 + 2 (round floating balls)
  pillar/spar     → 6
  beacon / light  → 7
  daymark         → 8
  special         → 9

Also synthesizes a dense floating mooring-ball field for Nantucket Harbor
(ENC only has sparse MORFAC points, mostly dolphins — not the private mooring
field boaters see in the basin).

Source: sail-sim frontend/public/coast/enc-maritime.json
  (npm run coast:maritime / Scripts/fetch-enc-maritime.mjs)
"""
from __future__ import annotations

import json
import collections
import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_CANDIDATES = [
    Path("/Users/harrison/PycharmProjects/sail-sim/frontend/public/coast/enc-maritime.json"),
    ROOT / "Content" / "Nav" / "enc-maritime.json",
]
OUT = ROOT / "Content" / "Nav" / "enc_aids_nantucket.json"

# Floating aids + navaids we place as buoy/beacon meshes.
PLACE_KINDS = {
    "buoy_lateral",
    "buoy_safe",
    "buoy_special",
    "buoy_cardinal",
    "buoy_danger",
    "beacon_lateral",
    "beacon_special",
    "beacon_safe",
    "mooring_point",
    "daymark",
    "light_float",
    "light",
    "fog_signal",
    "racon",
}

# Nantucket Harbor inner-basin floating mooring field (chart 13241 approx).
# West of the main channel fairway, east of the town docks / wharves.
# ENC MORFAC is sparse + mostly dolphins; this fills what boaters actually see.
HARBOR_MOORING_POLYS = [
    # Main field south of Brant Point, west of channel buoys 11–13
    {
        "name": "nantucket_inner_basin",
        "ring": [  # lon, lat — closed polygon, CCW
            (-70.0942, 41.2832),
            (-70.0888, 41.2834),
            (-70.0875, 41.2855),
            (-70.0878, 41.2878),
            (-70.0895, 41.2888),
            (-70.0925, 41.2885),
            (-70.0945, 41.2870),
            (-70.0948, 41.2848),
            (-70.0942, 41.2832),
        ],
        "spacing_m": 38.0,
        "jitter_m": 7.0,
        "seed": 13241,
    },
    # Pocket north of ferry slips / toward range lights
    {
        "name": "nantucket_brant_pocket",
        "ring": [
            (-70.0930, 41.2880),
            (-70.0900, 41.2882),
            (-70.0895, 41.2895),
            (-70.0912, 41.2902),
            (-70.0932, 41.2896),
            (-70.0930, 41.2880),
        ],
        "spacing_m": 42.0,
        "jitter_m": 6.0,
        "seed": 13242,
    },
]

# Keep a clear fairway around the channel (no synthetic balls in this corridor).
CHANNEL_FAIRWAY = {
    # approximate channel centerline points (lon, lat) → buffer_m
    "spine": [
        (-70.0878, 41.2899),  # buoy 13
        (-70.0904, 41.2919),  # buoy 11
        (-70.0946, 41.2935),  # buoy 10
        (-70.0941, 41.2956),  # buoy 9
        (-70.0955, 41.2959),  # buoy 8
        (-70.0967, 41.2987),  # buoy 6
    ],
    "half_width_m": 55.0,
}


def parse_colour(c):
    if c is None:
        return []
    if isinstance(c, (int, float)):
        return [int(c)]
    s = str(c).strip()
    if not s or s == "None":
        return []
    out = []
    for part in s.replace(";", ",").split(","):
        part = part.strip()
        try:
            out.append(int(float(part)))
        except Exception:
            pass
    return out


def shape_key(boyshp, bcnshp):
    s = (boyshp or bcnshp or "").lower().strip()
    if "conical" in s or "nun" in s:
        return "conical"
    if "can" in s or "cylindrical" in s:
        return "can"
    if "spherical" in s or "sphere" in s:
        return "spherical"
    if "pillar" in s:
        return "pillar"
    if "spar" in s:
        return "spar"
    if any(x in s for x in ("lattice", "beacon", "stake", "pole", "perch", "post")):
        return "beacon"
    return "unknown"


def is_dolphin(pr) -> bool:
    cat = str(pr.get("CATMOR") or "").lower().strip()
    return cat in ("dolphin", "1", "3", "pile", "post", "bollard")


def mooring_ball_mesh(seed_key) -> int:
    """Mix of SM_Buoy_1 and SM_Buoy_2 (round floating balls in the pack)."""
    h = hash(seed_key) if not isinstance(seed_key, int) else seed_key
    return 1 if (h & 1) == 0 else 2


def pick_mesh(kind, colour, shape, catlam, pr=None, lat=0.0, lon=0.0):
    pr = pr or {}
    if kind == "mooring_point":
        # Dolphins / posts → beacon mesh; floating warps → round ball mix
        if is_dolphin(pr):
            return 7
        oid = pr.get("OBJECTID")
        return mooring_ball_mesh(oid if oid is not None else (round(lat, 6), round(lon, 6)))
    if kind in ("beacon_lateral", "beacon_special", "beacon_safe", "light", "fog_signal", "racon"):
        return 7
    if kind == "daymark":
        return 8
    if kind in ("buoy_safe", "light_float"):
        return 6
    if kind == "buoy_special":
        return 9 if shape == "can" else 6
    if kind in ("buoy_cardinal", "buoy_danger"):
        return 6
    # US IALA-B: CATLAM 1 port=green can, 2 stbd=red nun
    is_port = (catlam == 1) or (colour == 4 and catlam != 2)
    is_stbd = (catlam == 2) or (colour == 3 and catlam != 1)
    if shape == "conical" or (is_stbd and shape != "can"):
        return 3 if is_stbd or colour == 3 else 4
    if shape == "can" or is_port:
        return 1 if is_port or colour == 4 else 2
    if shape == "spherical":
        return 5
    if shape in ("pillar", "spar"):
        return 6
    if colour == 3:
        return 3
    if colour == 4:
        return 1
    return 2


def meters_per_deg(lat_deg: float) -> tuple[float, float]:
    m_lat = 111320.0
    m_lon = 111320.0 * math.cos(math.radians(lat_deg))
    return m_lat, m_lon


def point_in_ring(lon: float, lat: float, ring: list[tuple[float, float]]) -> bool:
    """Ray-cast even-odd; ring is list of (lon, lat), first==last optional."""
    n = len(ring)
    if n < 3:
        return False
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = ring[i][0], ring[i][1]
        xj, yj = ring[j][0], ring[j][1]
        if ((yi > lat) != (yj > lat)) and (
            lon < (xj - xi) * (lat - yi) / (yj - yi + 1e-18) + xi
        ):
            inside = not inside
        j = i
    return inside


def dist_m(lon1, lat1, lon2, lat2) -> float:
    m_lat, m_lon = meters_per_deg(0.5 * (lat1 + lat2))
    return math.hypot((lat2 - lat1) * m_lat, (lon2 - lon1) * m_lon)


def dist_to_polyline_m(lon, lat, spine) -> float:
    best = 1e18
    for i in range(len(spine) - 1):
        x1, y1 = spine[i]
        x2, y2 = spine[i + 1]
        # project in local meters
        m_lat, m_lon = meters_per_deg(0.5 * (y1 + y2))
        ax = (x1) * m_lon
        ay = (y1) * m_lat
        bx = (x2) * m_lon
        by = (y2) * m_lat
        px = lon * m_lon
        py = lat * m_lat
        abx, aby = bx - ax, by - ay
        apx, apy = px - ax, py - ay
        ab2 = abx * abx + aby * aby
        t = 0.0 if ab2 < 1e-9 else max(0.0, min(1.0, (apx * abx + apy * aby) / ab2))
        cx, cy = ax + t * abx, ay + t * aby
        best = min(best, math.hypot(px - cx, py - cy))
    return best


def in_channel_fairway(lon: float, lat: float) -> bool:
    return dist_to_polyline_m(lon, lat, CHANNEL_FAIRWAY["spine"]) < CHANNEL_FAIRWAY["half_width_m"]


def synthesize_mooring_field() -> list[dict]:
    """Grid of floating mooring balls inside harbor polygons, jittered, fairway-cleared."""
    out = []
    oid = 9000000
    for poly in HARBOR_MOORING_POLYS:
        ring = poly["ring"]
        lons = [p[0] for p in ring]
        lats = [p[1] for p in ring]
        west, east = min(lons), max(lons)
        south, north = min(lats), max(lats)
        mid_lat = 0.5 * (south + north)
        m_lat, m_lon = meters_per_deg(mid_lat)
        spacing = float(poly["spacing_m"])
        jitter = float(poly["jitter_m"])
        dlat = spacing / m_lat
        dlon = spacing / m_lon
        rng = random.Random(int(poly["seed"]))
        lat = south + 0.5 * dlat
        row = 0
        while lat <= north:
            # hex packing offset alternate rows
            lon0 = west + 0.5 * dlon + (0.5 * dlon if row % 2 else 0.0)
            lon = lon0
            while lon <= east:
                jlat = lat + (rng.uniform(-1, 1) * jitter) / m_lat
                jlon = lon + (rng.uniform(-1, 1) * jitter) / m_lon
                if point_in_ring(jlon, jlat, ring) and not in_channel_fairway(jlon, jlat):
                    oid += 1
                    out.append(
                        {
                            "lat": jlat,
                            "lon": jlon,
                            "kind": "mooring_point",
                            "colour": 0,
                            "colours": [],
                            "catlam": 0,
                            "shape": "spherical",
                            "mesh": mooring_ball_mesh(oid),
                            "name": f"Harbor mooring {poly['name']}",
                            "objectId": oid,
                            "source": "synthetic_harbor_field",
                        }
                    )
                lon += dlon
            lat += dlat * math.sin(math.radians(60.0))  # hex row pitch
            row += 1
    return out


def dedupe_key(lat: float, lon: float, kind: str) -> tuple:
    # ~1.1 m grid; keep distinct kinds at same charted position (e.g. light on beacon)
    # but collapse pure light duplicates co-located with same-name beacons of equal kind.
    return (round(lat, 5), round(lon, 5), kind)


def main():
    src = next((p for p in SRC_CANDIDATES if p.exists()), None)
    if not src:
        raise SystemExit("enc-maritime.json not found — run sail-sim frontend coast:maritime")

    d = json.loads(src.read_text())
    aids = []
    counts = collections.Counter()
    seen = set()

    for feat in d["points"]["features"]:
        pr = feat.get("properties") or {}
        kind = pr.get("_kind")
        if kind not in PLACE_KINDS:
            continue
        geom = feat.get("geometry") or {}
        if geom.get("type") != "Point":
            continue
        coords = geom.get("coordinates") or []
        if len(coords) < 2:
            continue
        lon, lat = float(coords[0]), float(coords[1])
        key = dedupe_key(lat, lon, kind)
        if key in seen:
            continue
        seen.add(key)

        cols = parse_colour(pr.get("COLOUR"))
        colour = cols[0] if cols else 0
        try:
            catlam = int(pr.get("CATLAM")) if pr.get("CATLAM") not in (None, "None", "") else 0
        except Exception:
            catlam = 0
        shape = shape_key(pr.get("BOYSHP"), pr.get("BCNSHP"))
        mesh = pick_mesh(kind, colour, shape, catlam, pr, lat, lon)
        name = pr.get("OBJNAM") or ""
        rec = {
            "lat": lat,
            "lon": lon,
            "kind": kind,
            "colour": colour,
            "colours": cols,
            "catlam": catlam,
            "shape": shape,
            "mesh": mesh,
            "name": str(name) if name is not None else "",
            "objectId": pr.get("OBJECTID"),
            "source": "enc",
        }
        if kind == "mooring_point" and is_dolphin(pr):
            rec["shape"] = "dolphin"
            rec["name"] = rec["name"] or "Dolphin"
        elif kind == "mooring_point":
            rec["shape"] = "spherical"
        aids.append(rec)
        counts[(kind, mesh)] += 1

    # Synthetic floating mooring field (balls) — skip cells too close to ENC points
    existing_xy = [(a["lon"], a["lat"]) for a in aids]
    synth = synthesize_mooring_field()
    kept_synth = 0
    for s in synth:
        if any(dist_m(s["lon"], s["lat"], x, y) < 18.0 for x, y in existing_xy):
            continue
        key = dedupe_key(s["lat"], s["lon"], s["kind"])
        if key in seen:
            continue
        seen.add(key)
        aids.append(s)
        existing_xy.append((s["lon"], s["lat"]))
        counts[("mooring_point_synth", s["mesh"])] += 1
        kept_synth += 1

    aids.sort(key=lambda a: (a["lat"], a["lon"]))
    out = {
        "source": "enc-maritime.json (NOAA ENC Direct S-57) + synthetic harbor mooring field",
        "bbox": d.get("bbox"),
        "count": len(aids),
        "syntheticMoorings": kept_synth,
        "meshPathTemplate": "/Game/Buoys/StaticMesh/SM_Buoy_{mesh}.SM_Buoy_{mesh}",
        "note": (
            "US IALA-B: red/stbd→nun(3), green/port→can(1), mooring balls→mix SM_Buoy_1/2, "
            "dolphin/beacon/light→7, daymark→8; harbor mooring field synthesized"
        ),
        "countsByKindMesh": {f"{k[0]}:m{k[1]}": v for k, v in sorted(counts.items())},
        "aids": aids,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(out, indent=1))
    print(f"Wrote {OUT}")
    print(f"  total aids={len(aids)}  synthetic moorings={kept_synth}")
    print(f"  {out['countsByKindMesh']}")

    # Harbor summary around boat start
    H = dict(s=41.280, n=41.295, w=-70.100, e=-70.080)
    harbor = [
        a
        for a in aids
        if H["s"] <= a["lat"] <= H["n"] and H["w"] <= a["lon"] <= H["e"]
    ]
    hk = collections.Counter(a["kind"] for a in harbor)
    hm = collections.Counter(a["mesh"] for a in harbor)
    print(f"  harbor window aids={len(harbor)} kinds={dict(hk)} meshes={dict(hm)}")


if __name__ == "__main__":
    main()
