#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/** Optional sailing/dims block from boat3d export (imperial / web units). */
struct FBoatJsonSailingParams
{
	bool bValid = false;
	float DispLb = 7750.f;
	float BallastLb = 3340.f;
	float BeamFt = 11.f;
	float LwlFt = 29.5f;
	float DraftFt = 6.5f;
	float TcFt = 1.2f;
	float LateralArea = 70.f;
	float KeelArea = 50.f;
	float RudderArea = 4.8f;
	float KeelSpan = 6.f;
	float ClrX = -1.f;
	float ClrZ = -3.f;
	float SaTotal = 545.f;
	float HullSpeedKn = 7.f;
	float GmFt = 4.6f;
	float LoaFt = 34.4f;
	float MastTopFt = 44.f;
};

/** Mast/boom endpoints in UE cm (from boat3d spars). */
struct FBoatJsonSparEndpoints
{
	bool bMastValid = false;
	bool bBoomValid = false;
	FVector MastBase = FVector::ZeroVector;
	FVector MastTop = FVector::ZeroVector;
	FVector BoomBase = FVector::ZeroVector;
	FVector BoomEnd = FVector::ZeroVector;
};

/** Full boat3d load result (mesh sections filled separately). */
struct FBoatJsonLoadResult
{
	bool bOk = false;
	int32 SectionCount = 0;
	FBoatJsonSailingParams Sailing;
	FBoatJsonSparEndpoints Spars;
	FString ResolvedPath;
};

/** Load sail_geom.boat3d export JSON into a ProceduralMeshComponent. */
struct FBoatMeshFromJson
{
	/**
	 * Parse + fill procedural meshes. Always reloads mesh sections.
	 * Hull sections → HullOrCombinedMesh. Optional Main/Jib sail meshes receive
	 * sail_* sections with verts pivoted around mast base (for sheet rotation).
	 * If sail meshes are null, sails go into HullOrCombinedMesh.
	 */
	static bool LoadIntoProceduralMesh(
		UProceduralMeshComponent* HullOrCombinedMesh,
		const FString& JsonPathOrContentRelative,
		UMaterialInterface* DefaultMaterial = nullptr,
		FBoatJsonLoadResult* OutResult = nullptr,
		UProceduralMeshComponent* MainSailMesh = nullptr,
		UProceduralMeshComponent* JibSailMesh = nullptr);

	/** Parse sailing/spars only (no mesh rebuild) — for BeginPlay when mesh already loaded. */
	static bool LoadMetadataOnly(
		const FString& JsonPathOrContentRelative,
		FBoatJsonLoadResult& OutResult);

	static FString DefaultJ105Path();
};
