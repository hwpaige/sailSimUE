// Copyright Sail Buddy. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "SailSimToolset.generated.h"

/**
 * Sail Buddy MCP helpers for Prefer-ON / PIE / Design gates.
 *
 * CaptureViewport and CaptureEditorImage stay on EditorToolset (free editor camera).
 * CapturePlayerView renders the PIE scene from the possessed ASailBoatPawn camera boom.
 * EditorToolset.StartPIE fails opaquely when a session is already running — use StartPIE / EnsurePIE here.
 */
UCLASS()
class USailSimToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Lit screenshot of the possessed ASailBoatPawn from its camera boom during PIE.
	 * Renders PlayWorld->Scene directly (not the free editor camera, not the editor viewport grid).
	 * MinWorldSeconds: require the PIE world to have been running at least this long (0 skips the time gate).
	 * FramingPreset (SailSim_Ocean known-good pawn teleports; empty = leave boat where it is):
	 *   midHarborMoored — Nantucket Harbor basin (FNavGeo::BoatStart); moored hulls visible L/R of player
	 *   gelcoatHull     — same basin, yawed for close lit gelcoat / near-hull fill
	 *   horizon         — ~2.5 km north of harbor, open water / horizon
	 * Fails with a coded error if PIE is down, the session boat is missing, or the boom transform is not usable.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FToolsetImage CapturePlayerView(float MinWorldSeconds = 0.5f, const FString& FramingPreset = TEXT(""));

	/**
	 * One-shot Prefer-ON gate for Ops. Composes existing tools (does not rewrite them):
	 * 1) EnsurePIE (already-playing = success; expects SailSim_Ocean)
	 * 2) SetCVars Prefer-ON stick (Lumen Reflections Allow=1, DownsampleFactor=2 / DSF2)
	 * 3) Short settle pump (viewport present) until frame samples stabilize
	 * 4) Assert mid-harbor moored >= scenery floor (~64; soft target = MooringSceneryInstanceCount, default 96);
	 *    else failCode=moored_count. Reports scenery count + heroes (MaxBoats / MaxNearFullBoats, default 1).
	 *    Harbor fill is scenery count only.
	 * 5) CapturePlayerView FramingPreset=midHarborMoored (noon Lit CPV saved under Saved/Screenshots)
	 * 6) JSON: {frameMs_avg, fps, moored, mooringSceneryBudget, mooringSceneryFloor,
	 *    heroesMaxBoats, heroesNearFullCap, heroesNear, cpvPath, sha, ok, failCode?}
	 * Does not raise MaxBoats (heroes stay 1). Scenery is additive HISM. Prefer-ON / DSF2 unchanged.
	 * ProfileGPUDump stays parked.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString RunPreferOnGate();

	/**
	 * Find actors by case-insensitive substring on name, label, or class
	 * (e.g. "SailBoat", "ASailBoatPawn"). PIE world when playing, else editor world.
	 * Returns a JSON array of {name,label,class,x,y,z,world,playerControlled}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString FindActorsByName(const FString& Query, int32 MaxResults = 50);

	/**
	 * Ensure PIE is running. Idempotent: already playing returns ok JSON (does not error).
	 * If not playing, queues in-viewport PIE and returns code "requested" — call again after the editor ticks.
	 * A second call while the start is still queued returns code "already_requested" (not a failure).
	 * MinWorldSeconds is the settle threshold reported on Settled (possessed boat + world time).
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString EnsurePIE(float MinWorldSeconds = 0.5f);

	/**
	 * Same contract as EnsurePIE. Prefer this over EditorToolset.StartPIE, which returns an opaque
	 * failure when PIE is already running.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString StartPIE(float MinWorldSeconds = 0.5f);

	/**
	 * Editor + PIE package paths as JSON: {EditorLevel, PIELevel, IsPIERunning, Settled, ...}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetLevelPath();

	/**
	 * PIE player camera boom transform as JSON. Errors if not in PIE or the boom is unresolved.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetPlayerCameraTransform();

	/**
	 * FPS, frame ms, GPU ms, and [perf] HUD fields (moored, aids, tiles/T, GT buckets) from
	 * FSailSimPerf / engine unit timers. No screenshot OCR.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetPerfSnapshot();

	/**
	 * Set many console variables in one call. Newline-separated "name=value" or "name value"
	 * (semicolons accepted when there are no newlines). Example:
	 * r.Lumen.Reflections.DownsampleFactor=2
	 * r.Lumen.Reflections.Allow=1
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString SetCVars(const FString& Assignments);

	/**
	 * Run many console commands in one call. Same newline / semicolon batching as SetCVars.
	 * Uses the PIE world when playing, otherwise the editor world.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString ExecuteConsole(const FString& Commands);

	/**
	 * Trigger ProfileGPU (UI suppressed), present one frame, and return a structured split:
	 * SingleLayerWater, LumenGI, LumenReflections, Shadows, Nanite, Other, plus the dump path.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString ProfileGPUDump();

	/**
	 * Open a map by package path (short names resolve under /Game/Maps/).
	 * If PIE is running, PIE is ended and the map is NOT loaded in this call — call again once
	 * IsPIERunning is false. Refuses while packages are dirty so the editor does not modal-prompt.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString LoadMap(const FString& MapPath);
};
