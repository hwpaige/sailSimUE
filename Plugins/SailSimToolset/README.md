# SailSim Toolset

Editor MCP tools for Prefer-ON, PIE, and Design gates. They register on the existing editor toolset endpoint (`http://127.0.0.1:8765/mcp`) next to EditorToolset.

`CaptureViewport` and `CaptureEditorImage` stay on EditorToolset. They follow the free editor camera. Use `CapturePlayerView` for the possessed boat in PIE.

## Mac Live Coding

Rebuild **SailSimUE** and **SailSimToolset** together (`SailSimGetPerf` is in `SailSimUE.cpp`). `GetPerfSnapshot` calls that export so the plugin reads the same `FSailSimPerf` counters as the HUD. An inline `FSailSimPerf::Get()` inside the plugin DLL would be a different, empty copy. If Live Coding does not relink the game module, do one editor-target build.

After Live Coding, the log line for a good capture is:

`CapturePlayerView OK world=PIE ... cam=ActiveEditorViewport grab=ViewportFramebuffer view=PlayerCameraManager ... litOverride=0`

`cam` is which on-screen viewport was grabbed (`ActiveEditorViewport`, `PIEGameViewport`, or `LevelViewportPIE`). `grab=ViewportFramebuffer` means the pixels are that viewport's framebuffer (`GetViewportScreenShot`, the same read as editor HighResShot at 1x). `view` is the possessed camera pose. `litOverride=0` means show flags were left alone.

There is no scene-capture fallback. If the Lit viewport cannot be read, the call errors.

## CapturePlayerView

Grabs the **active editor/PIE Lit viewport framebuffer**. That is the image in the panel. Framing presets only move the possessed boat. The viewport's own Lit renderer (its show flags, exposure history, and post) draws the frame that is read back.

What it does:

1. Require `GEditor->PlayWorld` with `WorldType == PIE`.
2. Find the possessed session boat. Ignore the editor world.
3. Optional `FramingPreset` teleports that pawn. Orbit and zoom stay on the possessed camera.
4. Advance the spring arm and `PlayerCameraManager::UpdateCamera`, and `SetViewTarget` to the boat when needed, so the viewport's next present uses that pose.
5. Draw the active level viewport that is presenting PIE (else the PIE game window) for 16 frames. Show flags are not written.
6. `GetViewportScreenShot` on that viewport at its current size (HighResShot's 1x read). A magnified HighResShot is not used, because a higher-res re-render is a different image.

`EditorToolset.CaptureViewport` is still the free editor camera tool. This grab is the viewport that is actually presenting PIE.

`MinWorldSeconds` defaults to `0.5`. The call errors with `code=not_settled` until the PIE world has been running that long. Pass `0` to skip the time gate. The possessed boat is always required.

Coded errors (script error text, no image):

| code | meaning |
| --- | --- |
| `no_pie` | PIE is not running. Do not fall back to CaptureViewport. |
| `not_settled` | World time is under `MinWorldSeconds`. Retry. |
| `no_boom` | No possessed boat, or the player camera is not usable. |
| `no_viewport` | The active editor/PIE Lit viewport framebuffer was not available. No scene capture is returned. |
| `read_failed` / `encode_failed` | Viewport framebuffer read or PNG encode failed. No scene capture is returned. |
| `bad_framing` | Unknown `FramingPreset` string. |

### FramingPreset (SailSim_Ocean)

Optional second arg. Teleports the possessed pawn before capture (empty = leave boat where it is). Origins from `FNavGeo::BoatStartWorldCm2D()` (Nantucket Harbor 41.2850°N, 70.0900°W):

| preset | transform |
| --- | --- |
| `midHarborMoored` | Harbor basin + `BoatStartHeadingDeg` (90° east). Moored hulls visible L/R of the possessed camera. |
| `gelcoatHull` | Harbor + (−12 m N, +6 m E), yaw 135° — close gelcoat / near-hull fill from the possessed camera. |
| `horizon` | Harbor + 2.5 km north, yaw 0° — open water / horizon. Exposure may still be adapting after this jump. |

## RunPreferOnGate

One MCP call for Ops Prefer-ON. Composes EnsurePIE → SetCVars (DSF2 stick) → settle pump → assert scenery floor (~64, soft ~96) and report heroes (default 1) → midHarborMoored possessed-camera CPV.

```
SailSimToolset.RunPreferOnGate
```

Returns JSON (also written to `Saved/SailSim/last_prefer_on_gate.json`):

```
{ "ok": true, "frameMs_avg": 27.9, "fps": 35.8, "moored": 96,
  "mooringSceneryBudget": 96, "mooringSceneryFloor": 64,
  "heroesMaxBoats": 1, "heroesNearFullCap": 1, "heroesNear": 1,
  "cpvPath": ".../Saved/Screenshots/SailSim/ocean-<sha>-preferON-midHarborMoored-ActiveEditorViewport-....png",
  "captureSource": "ActiveEditorViewport", "grab": "ViewportFramebuffer",
  "viewSource": "PlayerCameraManager",
  "exposureFrames": 16, "litOverride": false,
  "sha": "018b9ab", "failCode": "", "error": "" }
```

`failCode` values: `wrong_map`, `pie_not_running`, `moored_count` (scenery below floor ~64 — does **not** claim success), `bad_framing`, `cpv_*`, `no_editor`. Does not change hero caps (`MaxBoats` / `MaxNearFullBoats` default 1), scenery budget, moored strip, or forced LOD. `ProfileGPUDump` remains available but is not part of this gate (parked for parse cost).

### Console fallback (no MCP schema refresh)

New `AICallable` UFUNCTIONs often need an editor **module reload** before they appear in the live MCP tool schema. Until then Ops can invoke the same gate via console (or MCP `ExecuteConsole`):

```
SailSim.RunPreferOnGate
```

MCP path when schema still lacks `RunPreferOnGate`:

```
SailSimToolset.ExecuteConsole  →  Commands="SailSim.RunPreferOnGate"
```

Live Coding that relinks `SailSimToolset` **does** pick up the `IConsoleManager` registration in `StartupModule` (console cmd works after Live Coding). MCP tool schema refresh still often needs a full editor module reload / editor restart — do **not** kill UnrealEditor without asking.

### Ops standing rule — kill CRC before MCP gate

TCP **8765** must be owned by **UnrealEditor**. macOS CrashReportClient frequently steals that port after a crash / editor boot race. Before any Prefer-ON MCP gate:

1. `lsof -nP -iTCP:8765 -sTCP:LISTEN` (or `sockstat`)
2. If **CrashReportClient** (or anything that is not UnrealEditor) → kill CRC; do **not** kill UnrealEditor.
3. If nothing is listening → start MCP (`ModelContextProtocol.StartServer` / `bAutoStartServer`).
4. `Scripts/ops_run_prefer_on_gate.py` enforces this: `failCode=mcp_port_stolen` or `mcp_down`.

### Compose script (schema-safe / tool-search)

Unreal MCP tool-search advertises only meta-tools (`list_toolsets` / `describe_toolset` / `call_tool`). The ops script must **not** treat that as `missing_tools`.

```
python3 Scripts/ops_run_prefer_on_gate.py
# optional:
#   SAILSIM_MCP_URL=http://127.0.0.1:8765/mcp
#   SAILSIM_TIMEOUT_S=120
#   SAILSIM_TOOLSET_WAIT_S=90   # wait/retry after editor boot / CRC clear
```

Flow:

1. Port guard on `:8765` (see above).
2. Initialize MCP session (`Mcp-Session-Id`).
3. Wait/retry (default 30–90s, backoff) until `SailSimToolset.SailSimToolset` appears via `list_toolsets` + `describe_toolset`. Only then may it fail `missing_tools` with: *SailSimToolset not registered yet; kill CrashReportClient on :8765 and ensure Unreal MCP bound.*
4. Prefer `call_tool` → `RunPreferOnGate` when describe shows it.
5. Else compose via `call_tool` (toolset `SailSimToolset.SailSimToolset`): EnsurePIE → SetCVars (Prefer-ON / DSF2) → poll GetPerfSnapshot until scenery `moored` ≥ floor ~64 (soft ~96) → CapturePlayerView (`FramingPreset=midHarborMoored` if schema has it; else CPV + note). Heroes (`MaxBoats` / `MaxNearFullBoats`, default 1) are reported and are not the pass bar.

Console fallback (no MCP / schema still stale after Live Coding):

```
SailSim.RunPreferOnGate
```

## StartPIE / EnsurePIE

Same function. Idempotent.

- Already playing: `{ "ok": true, "code": "running", "AlreadyRunning": true, "Settled": true|false, ... }`. This does **not** raise a script error.
- Not playing: queues in-viewport PIE and returns `{ "ok": true, "code": "requested" }`. Call again after the editor ticks.
- A repeat call while that start is still queued (about 5s): `{ "ok": true, "code": "already_requested" }`. That is not a failure.
- No editor / no map: `{ "ok": false, "code": "no_editor" | "no_editor_world", "error": "..." }`.

`Settled` means a possessed `ASailBoatPawn` exists and `WorldSeconds >= MinWorldSeconds`. `Streaming` is reported and does not by itself block settle or capture.

Use these instead of EditorToolset `StartPIE`, which fails opaquely when a session is already up.

The tools do not spin the game thread waiting for PIE. PIE starts on the editor tick; blocking inside the tool would deadlock that tick.

## GetPerfSnapshot

Live engine timers plus the HUD `[perf]` fields. No screenshot OCR.

- `fps`, `frameMs` from `GAverageFPS` / `GAverageMS` (same source as `stat fps`)
- `gpuMs` from `RHIGetGPUFrameCycles`
- `hud.moored`, `hud.aids`, `hud.tilesTLabel` (`tiles T=%d/%dk`), `hud.tilesHLabel`
- `hud.perfLine` matches the `[perf]` log line
- `hud.gtBucketsMs` is the SailSim game-thread EMA buckets when the performance chrome has been ticking

`moored`, `aids`, and tile counts are written by the subsystems. Wall / GT / GPU EMAs update when the performance chrome ticks.

## SetCVars / ExecuteConsole

One call, many entries. Separate lines with newlines. If there are no newlines, `;` separates entries. `name=value` and `name value` both work for `SetCVars`.

```
r.Lumen.Reflections.DownsampleFactor=2
r.Lumen.Reflections.Allow=1
```

`ExecuteConsole` runs on the PIE world when PIE is up, otherwise the editor world. Each result is `{ command, ok, error }`. `ok: false` with `code: "partial"` means at least one entry failed; the others still ran.

## ProfileGPUDump

Sets `r.ProfileGPU.ShowUI` to 0 for the call (restored after), runs `ProfileGPU`, presents one viewport frame, and writes `Saved/Profiling/SailSimProfileGPU-*.txt`.

Returned `bucketsMs`: `SingleLayerWater`, `LumenGI`, `LumenReflections`, `Lumen` (GI + reflections), `Shadows`, `Nanite`, `Other`.

- Bucket milliseconds sum the shallowest matching pass. Children of that same bucket are not added again.
- A parent whose children fall in different buckets is skipped so those children are counted on their own.
- `Other` is `totalGpuMs` minus the specific buckets. Overlap between buckets can shrink `Other`.
- `topEvents` lists the largest parsed lines so a missed marker name is still visible.
- `code: "profile_not_emitted"` means the frame did not log a parseable hierarchy. The dump file still has whatever was captured.

`profiledWorld` / `viewport` say which world was executed and which viewport was presented. Prefer a settled PIE session so the split is the game view.

## FindActorsByName / GetLevelPath / GetPlayerCameraTransform

- `FindActorsByName` searches the PIE world when playing, otherwise the editor world. Each hit includes `world` and `playerControlled`.
- `GetLevelPath` returns `EditorLevel`, `PIELevel`, `MapName`, and the same settle fields as EnsurePIE.
- `GetPlayerCameraTransform` is the possessed camera (`source`: `PlayerCamera` or `PlayerCameraManager`), plus `postProcessBlendWeight` and `editorCameraDistance`.

## LoadMap

Package path, or a short name under `/Game/Maps/` (`SailSim_Ocean` → `/Game/Maps/SailSim_Ocean`).

- If PIE is running, the tool requests end-play and does **not** load. Call again once `IsPIERunning` is false (`code: "ended_pie"`).
- If any world or content package is dirty, the tool refuses (`code: "unsaved_packages"`) so the editor does not open a modal save dialog.

## Gate sequence

**Prefer-ON (one call):** `RunPreferOnGate` — EnsurePIE + DSF2 SetCVars + scenery floor (~64, soft ~96) assert, heroes reported (default 1) + midHarborMoored possessed-camera CPV.

Manual / Design:

1. `EnsurePIE` or `StartPIE` until `code` is `running` and `Settled` is true.
2. `GetPlayerCameraTransform` — `source` is `PlayerCamera` or `PlayerCameraManager`, location is the possessed camera.
3. `CapturePlayerView` with `FramingPreset=midHarborMoored|gelcoatHull|horizon`. Accept `grab=ViewportFramebuffer` and `litOverride=0`. `cam` is `ActiveEditorViewport` when PIE is in the selected viewport.
4. `GetPerfSnapshot` for `moored=`, `aids=`, `tiles T=`.
5. `SetCVars` / `ExecuteConsole` for Prefer-ON A/Bs. `ProfileGPUDump` optional (parked for parse cost on the Prefer-ON gate).

## Ops — re-run Prefer-ON after the CPV fix

Score gelcoat / mid-harbor from a new shot. Older PNGs under `Saved/Screenshots/SailSim/` came from a scene capture (a second camera) and are not the Lit viewport. Accept only `grab=ViewportFramebuffer`.

1. TCP 8765 must be UnrealEditor. If CrashReportClient owns it, kill CrashReportClient only.
2. PIE `SailSim_Ocean` (or let the gate call EnsurePIE). Possessed boat, game viewport visible.
3. Live Coding must have relinked `SailSimToolset` (this capture path). If the MCP schema is stale, use the console command.
4. Run one of:

```
SailSim.RunPreferOnGate
```

```
python3 Scripts/ops_run_prefer_on_gate.py
```

5. Gelcoat close-up (same capture path, not a second Lit camera):

```
SailSimToolset.CapturePlayerView  MinWorldSeconds=0.5  FramingPreset=gelcoatHull
```

Pass checks in the log / `Saved/SailSim/last_prefer_on_gate.json`:

- `grab` is `ViewportFramebuffer`
- `captureSource` is `ActiveEditorViewport` (PIE in the selected viewport), or `PIEGameViewport` / `LevelViewportPIE` when that panel is the PIE surface
- `litOverride` is false
- `viewSource` is `PlayerCamera` or `PlayerCameraManager`
- `cpvPath` is under `Saved/Screenshots/SailSim/`

Prefer-ON cvars stay `r.Lumen.Reflections.Allow=1` and `DownsampleFactor=2`. Heroes stay `MaxBoats=1`. Do not lower `MooringSceneryInstanceCount`.

### Why older CPV color scores were wrong

Design shots were a second `ASceneCapture2D` (spring-arm socket, `ApplyViewMode(VMI_Lit)`, its own exposure). That path is not the Lit viewport, so gelcoat looked worse than the panel. A later build still fell back to that clone when the on-screen viewport failed a camera-distance test (`GetViewLocation` stays on the free editor camera during in-viewport PIE). The gate now refuses that fallback and reads the presenting viewport with `GetViewportScreenShot`.

### Remaining deltas (no editor visual PASS from this change)

- PNG size is the viewport's current size (same pixels as the panel). It is not forced to 1920×1080, and it is not a magnified HighResShot re-render.
- Slate widgets drawn on top of the viewport RT are outside this read, same as editor HighResShot.
- Sixteen presents after a framing teleport continue the viewport's own exposure. `horizon` (~2.5 km) can still be mid-adaptation. Harbor presets stay in the basin.
- Presets do not reset orbit or zoom.
- `EditorToolset.CaptureViewport` is still the free editor camera, not this grab.
