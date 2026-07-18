# Oceanology NextGen → SailSimUE integration

Official docs: https://galidar.com/oceanology-nextgen/setup

> **Status 2026-07-12:** Fab reports **no compatible engine** for this Mac / UE 5.8 install.  
> **Do not block SailSim on Oceanology.** Use **native Water Gerstner** instead:  
> [`NATIVE_OCEAN_UE58.md`](./NATIVE_OCEAN_UE58.md)

## Hard compatibility note (read this first)

Oceanology NextGen **1.3.x** documents:

| Requirement | Official support | Your machine |
|-------------|------------------|--------------|
| Engine | UE **5.5–5.7** | **5.8** |
| OS | **Windows** | **macOS** |
| Graphics | **DirectX 12 + SM6** | **Metal** |

Fab “no compatible engine” matches that matrix. Prefer **refund** or a future Windows target. Primary ocean work is native Water (see `NATIVE_OCEAN_UE58.md`).

---

## Phase A — Install the plugin (you do this)

### A1. Install from Fab Library

1. Open **Epic Games Launcher**.
2. Go to **Unreal Engine → Library → Fab Library** (or [fab.com/library](https://www.fab.com/library)).
3. Find **Oceanology NextGen**.
4. Click the **▼** next to **Install to Engine**.
5. Prefer **Create Project** if **Install to Engine** has no 5.8 slot — that always downloads files.
6. Or install to the closest engine slot Fab offers (5.6 / 5.7), then copy (see A2).

### A2. Put files into SailSimUE

Target path (must exist after install):

```
/Users/harrison/PycharmProjects/SailSimUE/Plugins/OceanologyNextGen/
  OceanologyNextGen.uplugin   # name may vary slightly
  Source/
  Content/
```

**If Fab installed to Engine:**

```bash
# Find the plugin (example; name may be OceanologyNextGen / Oceanology)
find "/Users/Shared/Epic Games" -name "*.uplugin" 2>/dev/null | grep -i ocean

# Copy into project (adjust source folder name)
mkdir -p /Users/harrison/PycharmProjects/SailSimUE/Plugins
cp -R "/Users/Shared/Epic Games/UE_5.x/Engine/Plugins/Marketplace/OceanologyNextGen" \
      /Users/harrison/PycharmProjects/SailSimUE/Plugins/
```

**If Fab used “Create Project”:** copy that project’s `Plugins/OceanologyNextGen` folder into SailSimUE’s `Plugins/`.

### A3. Enable in the editor

1. Open **SailSimUE** (`SailSimUE.uproject`) in UE 5.8.
2. If prompted to rebuild modules → allow (may fail on Mac; note the error).
3. **Edit → Plugins** → search **ocean** → enable **OCEANOLOGY NEXT-GEN**.
4. Restart editor.
5. Content Browser settings → enable **Show Plugin Content**.
6. Open a map under  
   `All → Plugins → OCEANOLOGY NEXT-GEN Content → Maps`  
   and **Play**. Confirm ocean renders.

### A4. Project settings (Windows / when plugin loads)

| Path | Setting | Value |
|------|---------|--------|
| Platforms → Windows | Default RHI | DirectX 12 |
| Platforms → Windows | D3D12 Shader Model 6 | On |
| Engine → Rendering | Generate Mesh Distance Fields | On |

On Mac these Windows RHI settings do nothing; Metal must work on its own or the plugin is unusable here.

### A5. Tell me when ready

Reply with one of:

- `Oceanology in Plugins/ and demo map PIE works`
- `Plugin files present but PIE fails: <paste error>`
- `Fab won’t install on 5.8/Mac`

Then I wire C++ (`FOceanologyOceanSampler`) and map actors.

---

## Phase B — Level setup (after plugin enables)

Do this in **`Content/Maps/SailSim_Ocean`** (or a new `SailSim_Oceanology` map).

### B1. Actors to place (Place Actors / + menu → search “oceanology”)

| Actor | Role |
|-------|------|
| **Oceanology Infinite Ocean** | Full-horizon FFT/Gerstner surface. Enable **Infinity** (editor + game). |
| **Oceanology Manager** | Central init / time / systems. |
| **Oceanology Water Volume** | Region for buoyancy + surface queries. Scale large (e.g. 200×200×20) so the boat stays inside. Link **Oceanology Water** → Infinite Ocean. Enable **Enable Buoyancy in Area**. |

Optional: sky/time controller from plugin content if you want their lighting.

### B2. Stock UE Water

With Oceanology driving visuals:

1. Hide or remove **WaterZone** / **Water Body Ocean** actors (or leave them disabled) so you don’t get two surfaces.
2. Optionally hide **Landscape** for pure open ocean.

Keep our boat / GameMode / HUD as-is.

### B3. Do **not** drive SailSim with OceanBuoyancy physics

Oceanology’s **Ocean Buoyancy** component is for Chaos physics boats (pontoons + Simulate Physics).

SailSim uses **custom VPP + multipoint kinematic float** via `USailOceanSubsystem` / `IOceanHeightSampler`.  
If we attach OceanBuoyancy to `ASailBoatPawn`, physics will fight the VPP.

Integration rule:

- **Oceanology** = render ocean + **height samples** for our float.
- **SailSim** = helm, sails, cloth, VPP, boat transform.

---

## Phase C — Code integration (I do this after A5)

Already in tree:

```
Sailing/Ocean/
  IOceanHeightSampler.h
  GerstnerWaterBodySampler.*   # current backend
  SailOceanSubsystem.*         # boat samples here
```

Planned:

1. Soft-enable plugin in `SailSimUE.uproject` when files exist.
2. Optional module dependency in `SailSimUE.Build.cs` (or reflection if headers are awkward).
3. `FOceanologyOceanSampler : IOceanHeightSampler`  
   - Query wave height/normal at world XY from Oceanology (wave solver / water body API).  
   - Fall back to Gerstner if actor missing.
4. `USailOceanSubsystem::EnsureSampler` prefers Oceanology when present.
5. `EnsureOceanVisualCoverage` no-ops or only ensures Infinite Ocean + Volume exist when Oceanology is active.
6. HUD line: `backend=OceanologyNextGen` so you can confirm in PIE.

Boat float code (`SnapToWaterSurface`, multipoint) stays unchanged.

---

## Phase D — Visual checklist

When integration works you should see:

- [ ] Ocean to the **horizon** (not one grid square)
- [ ] FFT chop + swell motion
- [ ] Single player boat on water
- [ ] Output Log: `SailOceanSubsystem init backend=OceanologyNextGen` (or similar)
- [ ] A/D helm still works; boat rides waves
- [ ] No double ocean (stock Water + Oceanology)

---

## If Mac is a dead end

| Option | Effort | Notes |
|--------|--------|--------|
| Keep UE Water + local tess + swell | Low | Current path; improve coverage/look |
| Windows machine / cloud GPU for Oceanology | Medium | Best use of the purchase |
| Custom Tessendorf (Phase 5 plan B) | High | Full control, Mac-native, months |
| WaterAdvanced FFT patches (engine experimental) | Medium | Limited; not full open ocean |

---

## Quick command: is the plugin on disk?

```bash
ls /Users/harrison/PycharmProjects/SailSimUE/Plugins
find /Users/harrison/PycharmProjects/SailSimUE/Plugins -name "*.uplugin" 2>/dev/null
find "/Users/Shared/Epic Games" -iname "*Oceanology*" 2>/dev/null | head
```

Right now (pre-install): no `Plugins/` folder and no Oceanology under Engine — **purchase only; files not in SailSimUE yet**.
