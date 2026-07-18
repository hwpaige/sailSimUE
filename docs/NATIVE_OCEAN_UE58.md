# Native ocean on UE 5.8 (Mac) — no marketplace FFT

**You are on Unreal Engine 5.8**, not 4.8. Oceanology NextGen targets **Windows + DX12** and older 5.x slots, so Fab’s “no compatible engine” is expected. Refund if possible; keep purchase only if you later develop on Windows.

## What UE 5.8 ships natively

| Plugin | Role | Waves | Mac? | Open-ocean fit |
|--------|------|-------|------|----------------|
| **Water** (enabled in SailSimUE) | Water Body Ocean / Lake / River, WaterZone mesh, buoyancy queries | **Gerstner only** (official) | Yes (already built for Mac) | **Primary path** |
| **WaterAdvanced** (enabled) | Niagara Fluids shallow water, river interaction, experimental **FFT ocean patch** | FFT patch is a **local normal/height RT**, not a full infinite FFT ocean | Experimental; may work | Secondary detail / wake, not the main sea |
| **WaterExtras** | Extra samples / tools | — | Yes | Optional |
| Landmass | Landscape carving for water | — | Yes | Shore only |

Epic docs (5.8): *“Unreal Engine only provides the Gerstner wave simulation model”* for the Water Waves asset. You can plug in custom models in C++/BP, but stock is Gerstner.

There is **no** production “native multi-cascade Tessendorf FFT open ocean” like water-pro / Oceanology on Mac in 5.8.

### WaterAdvanced “FFT” is not Oceanology

`UFFTOceanPatchSubsystem` + `Grid2D_OceanPatch` Niagara system generate an FFT-ish **patch** (render targets / normals) aimed at shallow-water interaction stacks. It does **not** replace Water Body Ocean as a horizon-to-horizon sea. Use later for local chop/wake if needed.

---

## Recommended architecture for SailSim (best practical path)

```
Visual mesh     → WaterZone + Water Body Ocean (local tessellation = draw window)
Wave height     → UGerstnerWaterWaves / Water Body query (IncludeWaves)  [already in FGerstnerWaterBodySampler]
Extra swell     → FSeaParams procedural Gerstner blend (already in sampler)
Gameplay float  → USailOceanSubsystem multipoint (keep VPP; do not switch to Chaos buoyancy)
Optional FX     → Niagara wake / WaterAdvanced shallow tools near hull
Future FFT      → custom Tessendorf OR Windows-only Oceanology (not Mac default)
```

This is the correct “native” stack: **Gerstner mesh + world-space height queries + camera-following tessellation**.

---

## How to make stock Water look like open ocean

### 1. Actors (level)

1. Keep **Water Zone** + **Water Body Ocean** (Open World template usually has these).
2. Select **Water Body Ocean** → ensure **Water Waves** asset is set (default often `GerstnerWaves_Ocean`).
3. On Water Zone: **Enable Local Only Tessellation** (SailSim code already forces this at runtime).
4. Optional: hide/delete **Landscape** so you don’t sail on an island square.
5. Ocean actor: use **Fill Water Zone With Ocean** (editor) if the mesh doesn’t fill the zone.

### 2. Author a stronger Gerstner asset (biggest visual win)

Create: Content Browser → **Water → Water Waves** → duplicate / create → Waves Source = **Gerstner**.

Suggested starting ranges (cm units; tune in PIE):

| Property | Calm cruise | Open Atlantic | Storm mood |
|----------|-------------|---------------|------------|
| Num Waves | 16–24 | 24–32 | 32–48 |
| Min / Max Wavelength | 400–2000 | 500–6000 | 300–8000 |
| Min / Max Amplitude | 5–25 | 15–80 | 40–200 |
| Dominant Wind Angle | match sim wind | match sim wind | match sim wind |
| Angular Spread | 20–40° | 30–60° | 45–90° |
| Small / Large Steepness | low | medium | high |

Notes:

- Macro shape = waves asset; micro chop = **water material normals** (don’t expect mesh tess alone to look FFT).
- After editing, water bodies must refresh wave data (`RecomputeWaves` path); reassign asset or touch the Water Body if mesh looks stale.
- Cap **Num Waves** if FPS tanks on Mac.

Assign the asset on **Water Body Ocean → Water Waves**.

### 3. Water material (micro detail)

On the ocean water material / MI:

- Stronger **tiling normals** at 2–3 scales (wind-aligned scroll).
- Foam from depth / wave steepness if the material exposes it.
- Absorption / scattering toward deep blue-green for open sea.
- Avoid relying only on Gerstner displacement for “sparkle.”

### 4. Coverage (why you saw one square)

| Cause | Fix |
|-------|-----|
| Local tess off / tiny zone | Code enables local tess + reasonable extent; verify PIE log |
| Camera far outside tess diameter | Increase local tess diameter on boat / subsystem |
| Island landscape | Hide Landscape for pure ocean |
| Mesh not rebuilt | Fill Water Zone With Ocean; MarkForRebuild |

Local tess = **render window**, not “boat trapped.” Physics queries use world XY forever.

### 5. Height queries (already wired)

`FGerstnerWaterBodySampler` uses:

```
EWaterBodyQueryFlags::ComputeLocation | ComputeNormal | IncludeWaves | SimpleWaves
```

plus procedural swell. That is the correct native API (`GetWaveHeightAtPosition` under the hood). Keep this; do not replace with Oceanology buoyancy on Mac.

---

## FFT: realistic options

| Option | Works on Mac 5.8? | Effort | Quality |
|--------|-------------------|--------|---------|
| Stock Gerstner + material | Yes | Low | Good sailing sim if tuned |
| WaterAdvanced FFT patch | Maybe | Medium | Local detail only |
| Custom Tessendorf multi-cascade | Yes (you own it) | High (months) | Matches water-pro bar |
| Oceanology / most Fab FFT oceans | No (Windows) | Paid | Best off-the-shelf on PC |
| Niagara material fake ocean | Yes | Medium | Cinematics; weak buoyancy |

**Shipping decision for SailSimUE on Mac:**  
**Native Water + strong Gerstner asset + material + local tess** is the production path. FFT is a later optional backend behind `IOceanHeightSampler`, not a Fab dependency.

---

## What to do with the Oceanology purchase

1. Request refund if Fab allows (incompatible platform/engine).
2. Or keep for a future **Windows** shipping target only.
3. Do **not** block SailSim on it.

---

## Implementation backlog (code / content)

- [x] Runtime open-ocean mode (`USailOceanSubsystem::PrepareOpenOcean`)
  - Hide Landscape / streaming proxies + island foliage
  - Local tess + larger zone, smaller water tiles
  - 28-wave Gerstner spectrum from `FSeaParams` / TWS
  - Water MID foam + absorption polish
  - Horizon fog soft-blend
- [ ] Optional: WaterAdvanced Niagara wake near hull (not FFT sea)
- [ ] Later: custom FFT sampler if Mac visuals still short of bar
- [ ] Sea-mood data assets (dusk/storm presets)
