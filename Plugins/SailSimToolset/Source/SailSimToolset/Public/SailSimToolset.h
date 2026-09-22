// Copyright Sail Buddy. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "SailSimToolset.generated.h"

/**
 * Sail Buddy MCP tool surface for Prefer-ON / PIE / Design gates.
 * CapturePlayerView must show PlayWorld player (ASailBoatPawn boom) — not the free editor grid.
 */
UCLASS()
class USailSimToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Capture the live PIE player view (possessed ASailBoatPawn camera boom / game viewport).
	 * Prefer this over CaptureViewport during PIE — free-editor CaptureViewport yields empty grid.
	 * Requires active PIE. No annotations (clean Lit for Design gelcoat).
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FToolsetImage CapturePlayerView();

	/**
	 * Find actors by case-insensitive substring on name, label, or class
	 * (e.g. "SailBoat", "ASailBoatPawn"). PIE world when playing, else editor world.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString FindActorsByName(const FString& Query, int32 MaxResults = 50);

	/**
	 * Ensure PIE is running. Idempotent: if already playing, returns success JSON
	 * (does NOT error like StartPIE). Includes Settled=true when a player pawn exists.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString EnsurePIE();

	/** Editor + PIE package paths as JSON: {EditorLevel, PIELevel, IsPIERunning}. */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetLevelPath();

	/** PIE player camera transform JSON {x,y,z,pitch,yaw,roll,source}. */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetPlayerCameraTransform();

	/**
	 * Prefer-ON perf snapshot from FSailSimPerf (no screenshot OCR).
	 * JSON: fps, frameMs, gpuMs, gtMs, rtMs, moored, aids, tiles, bottleneck.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetPerfSnapshot();

	/**
	 * Batch set console variables. CVarsText format: "a=1;b=2" or one "name=value" per line.
	 * Returns JSON {ok, applied:[...], failed:[...]}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString SetCVars(const FString& CVarsText);

	/**
	 * Execute a console command in the editor/PIE world (e.g. "ProfileGPU", "stat unit").
	 * Returns JSON {ok, command, world}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString ExecuteConsole(const FString& Command);

	/**
	 * Request a ProfileGPU dump and return Prefer-ON snapshot context.
	 * Structured SLW/Lumen/shadows require the dump log; JSON includes snapshot + profilingDir hint.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString ProfileGPUDump();
};
