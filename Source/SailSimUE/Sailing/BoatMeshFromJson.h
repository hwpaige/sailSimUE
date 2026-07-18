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

/** Jib genoa tracks + mainsheet lead + sheet winch/block routing (UE cm). */
struct FBoatJsonRunningRigging
{
	bool bJibTracksValid = false;
	FVector JibPortTrackFwd = FVector::ZeroVector;
	FVector JibPortTrackAft = FVector::ZeroVector;
	FVector JibStbdTrackFwd = FVector::ZeroVector;
	FVector JibStbdTrackAft = FVector::ZeroVector;

	bool bMainsheetValid = false;
	FVector MainsheetGooseneck = FVector::ZeroVector;
	FVector MainsheetLead = FVector::ZeroVector; // deck lead / traveler centerline
	float BoomLengthCm = 0.f;
	/** Boom vang (web MAST_VANG_DROP_FRAC / MAINVANG_FRAC). */
	float VangMastDropFrac = 0.06f;
	float VangBoomFrac = 0.25f;

	/**
	 * Sheet terminal points (J/105 layout):
	 *  jib  clew → track car → primary winch
	 *  spin clew → stern quarter block → cabin-top winch
	 * Port = −Y after load-time swap; stbd = +Y.
	 */
	bool bSheetLeadsValid = false;
	FVector JibPortWinch = FVector::ZeroVector;
	FVector JibStbdWinch = FVector::ZeroVector;
	FVector SpinPortBlock = FVector::ZeroVector;
	FVector SpinStbdBlock = FVector::ZeroVector;
	FVector SpinPortWinch = FVector::ZeroVector;
	FVector SpinStbdWinch = FVector::ZeroVector;
};

/** Full boat3d load result (mesh sections filled separately). */
struct FBoatJsonLoadResult
{
	bool bOk = false;
	int32 SectionCount = 0;
	FBoatJsonSailingParams Sailing;
	FBoatJsonSparEndpoints Spars;
	FBoatJsonRunningRigging Rigging;
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
		UProceduralMeshComponent* JibSailMesh = nullptr,
		/** When true, skip sail_* sections entirely (moored boats with sails down). */
		bool bSkipSailSections = false,
		/**
		 * Optional: keel/rudder go here (no shadow cast — underwater appendages
		 * should not darken the topsides). Null → pack into HullOrCombinedMesh.
		 */
		UProceduralMeshComponent* AppendagesMesh = nullptr);

	/** Deep-copy all procedural sections + materials from Src → Dst. */
	static void CopyProceduralMesh(UProceduralMeshComponent* Src, UProceduralMeshComponent* Dst);

	/** Parse sailing/spars only (no mesh rebuild) — for BeginPlay when mesh already loaded. */
	static bool LoadMetadataOnly(
		const FString& JsonPathOrContentRelative,
		FBoatJsonLoadResult& OutResult);

	static FString DefaultJ105Path();
};
