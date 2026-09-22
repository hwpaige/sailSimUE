// Copyright Sail Buddy. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/ToolsetImage.h"
#include "SailSimToolset.generated.h"

/**
 * Sail Buddy MCP helpers.
 * CaptureViewport annotations / CaptureTransform are already optional in EditorAppToolset —
 * Prefer-ON empty-grid Lit shots come from free-editor camera. CapturePlayerView fixes that.
 */
UCLASS()
class USailSimToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Capture the level viewport from the possessed PIE player camera (view target / camera boom).
	 * Prefer this over CaptureViewport during PIE — CaptureViewport follows the free editor camera
	 * and yields empty-grid Lit shots when the boat is elsewhere.
	 * Requires active PIE. No annotations (clean Lit for Design gelcoat).
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FToolsetImage CapturePlayerView();

	/**
	 * Find actors by case-insensitive substring on name, label, or class
	 * (e.g. "SailBoat", "ASailBoatPawn"). PIE world when playing, else editor world.
	 * Returns JSON array of {name,label,class,x,y,z}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString FindActorsByName(const FString& Query, int32 MaxResults = 50);

	/**
	 * Ensure PIE is running. If already playing, returns success JSON (does NOT error like StartPIE).
	 * If not playing, requests in-viewport PIE and returns {"IsPIERunning":false,"Requested":true}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString EnsurePIE();

	/**
	 * Editor + PIE package paths as JSON: {EditorLevel, PIELevel, IsPIERunning}.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetLevelPath();

	/**
	 * PIE player camera transform as JSON {x,y,z,pitch,yaw,roll,source}. Errors if not in PIE.
	 */
	UFUNCTION(meta = (AICallable), Category = "SailSimToolset")
	static FString GetPlayerCameraTransform();
};
