#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NantucketStructuresSubsystem.generated.h"

class UProceduralMeshComponent;
class UStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** One streamed structure cell (matches sail-sim structure tile grid). */
USTRUCT()
struct FNantucketStructureTileDesc
{
	GENERATED_BODY()

	FString Id;
	int32 Tx = 0;
	int32 Ty = 0;
	FString FileLod0;
	FString FileLod1;
	/** Optional cooked UE asset path, e.g. /Game/Structures/nantucket/Cooked/SM_Struct_4_2_LOD0.SM_Struct_4_2_LOD0 */
	FString StaticMesh;
	/** Optional cooked LOD1 StaticMesh path. */
	FString StaticMeshLod1;
	/** Foliage instance JSON path, e.g. tiles/5_2_foliage.json (HISM). */
	FString FileFoliage;
	/** World-space origin (cm) of cooked SM verts after localization; place actor here. */
	FVector StaticMeshOrigin = FVector::ZeroVector;
	FVector StaticMeshLod1Origin = FVector::ZeroVector;
	bool bHasStaticMeshOrigin = false;
	bool bHasStaticMeshLod1Origin = false;
	int32 FoliageInstanceCount = 0;
	FVector2D WorldMin = FVector2D::ZeroVector;
	FVector2D WorldMax = FVector2D::ZeroVector;
	FVector2D WorldCenter = FVector2D::ZeroVector;
	int32 Vertices = 0;
};

/**
 * Distance-streamed Nantucket buildings & props (OSM houses, heroes, ENC lights)
 * from sail-sim NAVT bake — optional cooked StaticMesh (+ Nanite) + PMC fallback.
 * Foliage streams as HISM instances from per-tile foliage JSON (not voxel meshes).
 *
 * Same spatial pattern as UNantucketTerrainSubsystem, tighter radii — harbor
 * tiles are ~500k verts of architecture detail.
 *
 * Importance (C3): when the boat is far from the island (nearest structure
 * tile center beyond OpenSeaSkipDistanceCm), unload all / skip loads.
 */
UCLASS()
class SAILSIMUE_API UNantucketStructuresSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	UPROPERTY(EditAnywhere, Category = "Structures")
	bool bEnabled = true;

	/**
	 * Chebyshev tile radius near harbor. While tiles are runtime PMC (no Nanite),
	 * LoadR=2 pulls ~2.2M verts; LoadR=1 keeps town readable at ~1/3 the cost.
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "1", ClampMax = "6"))
	int32 LoadRadius = 1;

	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "2", ClampMax = "8"))
	int32 UnloadRadius = 2;

	/**
	 * Full-res LOD0 only on the boat's tile (0). Shell uses *_l1.mesh.
	 * Harbor LOD0 tiles are 400–540k verts each — more than one is rarely worth it on Mac.
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "0", ClampMax = "3"))
	int32 Lod0Radius = 0;

	UPROPERTY(EditAnywhere, Category = "Structures|Stream")
	float UpdateIntervalSec = 0.35f;

	/**
	 * Beyond this 2D distance (cm) to the nearest structure tile center, skip
	 * loading and unload any resident structure tiles (open-sea importance).
	 * 250000 cm = 2.5 km.
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream|Importance", meta = (ClampMin = "50000.0"))
	float OpenSeaSkipDistanceCm = 250000.f;

	/**
	 * Within this distance: full LoadRadius / Lod0Radius (harbor / close approach).
	 * Between this and OpenSeaSkipDistanceCm: reduced ring (LoadRadius 1).
	 * 150000 cm = 1.5 km.
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream|Importance", meta = (ClampMin = "10000.0"))
	float HarborFullDistanceCm = 150000.f;

	/**
	 * Max 2D distance (cm) from focus to tile center to load vegetation layer.
	 * Buildings stream farther; trees only near boat (~2.5 km default disc, tighten later).
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream|Vegetation", meta = (ClampMin = "20000.0"))
	float VegLoadDistanceCm = 250000.f;

	/**
	 * When true, use cooked SM+Nanite when the asset has ~full vert count
	 * (≥85% of manifest expected). Stubs fall back to full NAVT PMC automatically.
	 * Keep false while cooks are OBJ (geometry+Nanite ok, vertex colors lost).
	 * Flip true once PLY/GeometryScript cook preserves the shingle palette.
	 * Cook: Scripts/cook_nantucket_structures_nanite.py (editor).
	 */
	UPROPERTY(EditAnywhere, Category = "Structures|Cook")
	bool bPreferCookedStaticMesh = false;

	UFUNCTION(BlueprintCallable, Category = "Structures")
	void SetEnabled(bool bInEnabled);

	UFUNCTION(BlueprintCallable, Category = "Structures")
	bool IsStreaming() const { return bEnabled && bManifestLoaded; }

	UFUNCTION(BlueprintCallable, Category = "Structures")
	int32 GetResidentTileCount() const { return Resident.Num(); }

	UFUNCTION(BlueprintCallable, Category = "Structures")
	int32 GetResidentSmCount() const { return ResidentSm; }

	UFUNCTION(BlueprintCallable, Category = "Structures")
	int32 GetResidentPmcCount() const { return ResidentPmc; }

	UFUNCTION(BlueprintCallable, Category = "Structures")
	FString GetStatusLine() const;

	UFUNCTION(BlueprintCallable, Category = "Structures")
	bool ReloadManifest();

	UFUNCTION(BlueprintCallable, Category = "Structures")
	void ForceStreamAround(FVector FocusWorld);

	/** Night lighting 0 = day, 1 = full night window/lamp glow (from env preset). */
	UFUNCTION(BlueprintCallable, Category = "Structures|Lights")
	void SetNightLighting(float Night01);

	UFUNCTION(BlueprintCallable, Category = "Structures|Lights")
	float GetNightLighting() const { return Night01; }

	/** Calendar season 0..1 (winter→spring→summer→autumn) for foliage tint. */
	UFUNCTION(BlueprintCallable, Category = "Structures|Season")
	void SetSeason(float Season01);

	UFUNCTION(BlueprintCallable, Category = "Structures|Season")
	float GetSeason() const { return Season01; }

	/** Resident streamed vertex count (for profiling). */
	UFUNCTION(BlueprintCallable, Category = "Structures")
	int32 GetResidentVertexCount() const { return ResidentVerts; }

private:
	struct FResidentTile
	{
		int32 Tx = 0;
		int32 Ty = 0;
		int32 Lod = 0;
		int32 VertCount = 0;
		/** True when drawn via cooked UStaticMeshComponent; false = PMC path. */
		bool bStaticMesh = false;
		TObjectPtr<UStaticMeshComponent> StaticMeshComp;
		TObjectPtr<UProceduralMeshComponent> ProcMesh;
		TObjectPtr<UMaterialInstanceDynamic> Mid;
		/** HISM foliage actor (one actor, multiple HISM comps by species). */
		TObjectPtr<AActor> FoliageActor;
		int32 FoliageInstanceCount = 0;
		TArray<TObjectPtr<UMaterialInstanceDynamic>> FoliageMids;
	};

	bool bManifestLoaded = false;
	/** True after open-sea skip path last stream pass (status chrome). */
	bool bOpenSeaSkipped = false;
	FString StructuresRoot;
	TArray<FNantucketStructureTileDesc> TileDescs;
	TMap<uint64, int32> KeyToDescIndex;
	TMap<uint64, FResidentTile> Resident;
	float TimeSinceUpdate = 0.f;
	int32 StreamCols = 8;
	int32 StreamRows = 8;
	int32 ResidentVerts = 0;
	int32 ResidentSm = 0;
	int32 ResidentPmc = 0;
	int32 ResidentVegTiles = 0;
	int32 ResidentFoliageInstances = 0;
	float Night01 = 0.f;
	float Season01 = 0.5f;
	float WindowEmissive = 3.2f;
	float LampEmissive = 5.0f;

	/** Cached prototype meshes per species (oak/cedar/…). */
	UPROPERTY()
	TMap<FString, TObjectPtr<UStaticMesh>> FoliageMeshes;

	static uint64 TileKey(int32 Tx, int32 Ty) { return (uint64(uint32(Tx)) << 32) | uint32(Ty); }

	bool ResolveStructuresRoot(FString& OutRoot) const;
	bool LoadManifestFromRoot(const FString& Root);
	void UpdateStreaming(const FVector& FocusWorld);
	void EnsureTile(int32 Tx, int32 Ty, int32 Lod, const FVector& FocusWorld);
	void EnsureFoliageLayer(FResidentTile& R, const FNantucketStructureTileDesc& Desc, const FVector& FocusWorld);
	void ReleaseTile(uint64 Key);
	void UnloadAllResident();
	UProceduralMeshComponent* CreateTileMesh(const FString& RelPath, int32* OutVerts, UMaterialInstanceDynamic** OutMid);
	UStaticMeshComponent* CreateTileStaticMesh(
		const FString& AssetPath, int32 FallbackVerts, const FVector& WorldOrigin,
		int32* OutVerts, UMaterialInstanceDynamic** OutMid);
	/** Load mesh_origins.json leaf -> world origin for cooked assets. */
	bool LoadMeshOriginFromCooked(const FString& AssetPath, FVector& OutOrigin) const;
	UStaticMesh* LoadCookedStaticMesh(const FString& AssetPath) const;
	UStaticMesh* GetOrCreateFoliageMesh(const FString& Species);
	void ApplyLightingToMid(UMaterialInstanceDynamic* Mid) const;
	void ApplyLightingToFoliageMids(FResidentTile& R) const;
	void SyncNightFromOcean();
	void PublishPerfStats() const;
	FVector GetFocusLocation() const;
	UMaterialInterface* GetOrCreateStructureMaterial();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> StructureMaterial;
};
