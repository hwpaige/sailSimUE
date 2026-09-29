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
 * CapturePlayerView is a 1x HighResShot of the active editor/PIE Lit viewport during its Draw.
 * Framing presets only move the possessed camera. Show flags, exposure, post, and time of day stay on that viewport.
 * EditorToolset.StartPIE fails opaquely when a session is already running — use StartPIE / EnsurePIE here.
 */
UCLASS()
class USailSimToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * 1x HighResShot of the active editor/PIE Lit viewport (its Draw, LDR, no resolution multiplier).
	 * Framing presets move the possessed camera; that viewport's Lit path presents the shot.
	 * Does not spawn a scene capture and does not change show flags, sky, exposure, or time of day.
	 * MinWorldSeconds: require the PIE world to have been running at least this long (0 skips the time gate).
	 * FramingPreset (SailSim_Ocean pawn teleports, then the possessed camera is what gets shot;
	 * empty = leave the boat where it is):
	 *   midHarborMoored — Nantucket Harbor basin (FNavGeo::BoatStart); moored hulls visible L/R of player
	 *   gelcoatHull     — same basin, yawed for close gelcoat / near-hull fill
	 *   horizon         — ~2.5 km north of harbor, open water / horizon
	 * Fails with a coded error if PIE is down, the session boat is missing, or the camera is not usable.
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
	 * 5) Assert the running binary logged "hull slot only" and a multi-slot mats= line.
	 *    Missing proof or a binary older than Source/SailSimUE → failCode stale_binary or
	 *    tip_not_in_binary and no HighResShot.
	 * 6) CapturePlayerView FramingPreset=midHarborMoored (1x Lit viewport grab under Saved/Screenshots)
	 * 7) JSON includes gitHead, binaryMtime, tipInBinary, sceneryProof, matsSummary,
	 *    frameMs_avg, fps, moored, mooringSceneryBudget, mooringSceneryFloor,
	 *    heroesMaxBoats, heroesNearFullCap, heroesNear, cpvPath, captureSource, grab,
	 *    viewSource, exposureFrames, litOverride, sha, ok, failCode.
	 * ExpectedSha: optional full or short git SHA. Empty uses HEAD. Mismatch fails closed.
	 * Does not raise MaxBoats (heroes stay 1). Scenery is additive HISM. Prefer-ON / DSF2 unchanged.
	 * ProfileGPUDump stays parked. Does not run Live Coding; call AssertModuleFresh for that.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString RunPreferOnGate(const FString& ExpectedSha = TEXT(""));

	/**
	 * Fail-closed check that the loaded SailSimUE / SailSimToolset binaries are newer than
	 * their sources and that git HEAD matches ExpectedSha (empty = current HEAD must be readable).
	 * If SailSimUE.log already has a scenery material line, it must include "hull slot only"
	 * and multi-slot mats=. Does not capture.
	 * bLiveCompile: queue LiveCoding.Compile and return immediately (does not block the game thread).
	 * If that command is missing, code=live_compile_failed and ubtHint is the editor-target UBT line.
	 * Writes Saved/SailSim/last_module_fresh.json.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString AssertModuleFresh(const FString& ExpectedSha = TEXT(""), bool bLiveCompile = false);

	/** Same as AssertModuleFresh. Ops can call this before RunPreferOnGate. */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString EnsureTipInBinary(const FString& ExpectedSha = TEXT(""), bool bLiveCompile = false);

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
	 * PIE possessed-camera transform as JSON (camera component / camera manager when they agree).
	 * Errors if not in PIE or the camera is unresolved.
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
	 * Trigger ProfileGPU (UI suppressed) and wait until a new dump file under Saved/Profiling parses.
	 * Returns profile_timeout with expectedDumpDir, dumpPath, and timeoutSec when the file never
	 * arrives. Hierarchy scrape of the captured log is used only after that wait.
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
