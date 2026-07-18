#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
struct FSailClothSim;

/**
 * Telltales (yarn streamers) + main-sail race markings, ported from
 * sail-sim webgl-utils.sailing.js (hUpdateTellTales / hUpdateJibBodyTellTales /
 * hBuildSailMarkingsTexture / hSampleMainMarkingsTSL).
 *
 * Markings are a high-res atlas texture (logo + numbers) on a light cloth-following
 * UV mesh — not blocky per-ink-cell geometry.
 *
 * Units: cloth positions are sail-local cm (same as FSailClothSim).
 */
struct FSailVisualExtras
{
	/** Default RRS-style identity (web hStyle.sailNumber). */
	FString SailNumber = TEXT("USA 52735");

	bool bEnabled = true;
	bool bTellTales = true;
	/** Jib yarns only (main still draws). Off while the Code Zero / kite is set. */
	bool bJibTellTales = true;
	bool bSailMarkings = true;
	/** Visible fibreglass/carbon batten rods on main + jib. */
	bool bBattenRods = true;

	void Clear();
	void EnsureBuilt(UProceduralMeshComponent* MainSail, UProceduralMeshComponent* JibSail);
	void Update(
		float DeltaSeconds,
		const FSailClothSim& MainCloth,
		const FSailClothSim& JibCloth,
		const FVector& WindLocalMain,
		const FVector& WindLocalJib);

private:
	struct FTubeTale
	{
		TObjectPtr<UProceduralMeshComponent> Mesh;
		TArray<FVector> Pts;
	};

	/** Live batten rod (cloth-following tapered tube). */
	struct FBattenRod
	{
		TObjectPtr<UProceduralMeshComponent> Mesh;
		TArray<FVector> Pts;
	};

	/** Cloth-following textured island (logo or numbers). */
	struct FMarkPatch
	{
		TObjectPtr<UProceduralMeshComponent> Mesh;
		TObjectPtr<UMaterialInstanceDynamic> Mid;
		/** Island centre in sail UV (u luff→leech, v foot→head). */
		float Cu = 0.5f;
		float Cv = 0.5f;
		float Du = 0.2f;
		float Dv = 0.1f;
		/** Texture UV rect in markings atlas. */
		FVector2D TexMin = FVector2D(0.f, 0.f);
		FVector2D TexMax = FVector2D(1.f, 1.f);
		/** Cloth-grid resolution (segments, not verts). */
		int32 GridU = 32;
		int32 GridV = 32;
		int32 CachedVertCount = 0;
	};

	TWeakObjectPtr<UProceduralMeshComponent> MainSailW;
	TWeakObjectPtr<UProceduralMeshComponent> JibSailW;

	TArray<FTubeTale> MainLeechTales;
	TArray<FTubeTale> JibLeechTales;
	TArray<FTubeTale> JibBodyTales; // red/green pairs × stations
	TArray<FBattenRod> MainBattenRods;
	TArray<FBattenRod> JibBattenRods;

	FMarkPatch LogoPatch;
	FMarkPatch NumberPatch;

	TObjectPtr<UTexture2D> MarkingsTex = nullptr;
	TObjectPtr<UMaterialInterface> YarnMatOrange = nullptr;
	TObjectPtr<UMaterialInterface> YarnMatRed = nullptr;
	TObjectPtr<UMaterialInterface> YarnMatGreen = nullptr;
	TObjectPtr<UMaterialInterface> BattenMat = nullptr;
	TObjectPtr<UMaterialInterface> MarkBaseMat = nullptr;

	float Phase = 0.f;
	bool bBuilt = false;

	UProceduralMeshComponent* MakeChildMesh(UProceduralMeshComponent* Parent, const FName& Name);
	UMaterialInterface* MakeSolidMat(FLinearColor Color, float Emissive = 0.25f);
	UMaterialInstanceDynamic* MakeMarkingsMat(UTexture2D* Tex);
	UTexture2D* LoadMarkingsTexture();

	void EnsureLeechTales(UProceduralMeshComponent* Sail, TArray<FTubeTale>& Out, int32 Count, UMaterialInterface* Mat);
	void EnsureBattenRods(UProceduralMeshComponent* Sail, TArray<FBattenRod>& Out, int32 Count);
	void EnsureJibBodyTales(UProceduralMeshComponent* Sail);
	void EnsureMarkPatches(UProceduralMeshComponent* MainSail);

	void UpdateLeechTales(
		const FSailClothSim& Cloth,
		TArray<FTubeTale>& Tales,
		const TArray<float>& BattenV,
		const FVector& WindLocal,
		float RadiusCm);

	void UpdateBattenRods(
		const FSailClothSim& Cloth,
		TArray<FBattenRod>& Rods,
		float RadiusCm);

	void UpdateJibBodyTales(const FSailClothSim& Cloth, const FVector& WindLocal);
	void UpdateMarkPatch(const FSailClothSim& Cloth, FMarkPatch& Patch);

	/**
	 * Round yarn tube along centerline (web THREE.TubeGeometry / hRefreshTubeGeo).
	 * Flat ribbons vanish edge-on; tubes stay visible like the original app.
	 * @param ForwardThinScale radius multiplier at centerline[0] (batten: thin tip).
	 * @param AftThickScale radius multiplier at centerline end (batten: full section).
	 */
	static void BuildYarnTubeMesh(
		UProceduralMeshComponent* Mesh,
		const TArray<FVector>& Centerline,
		float RadiusCm,
		UMaterialInterface* Mat,
		float ForwardThinScale = 1.0f,
		float AftThickScale = 0.55f);

	static FVector SampleCloth(
		const FSailClothSim& Cloth,
		float V01,
		float U01,
		FVector* OutNormal = nullptr);
};
