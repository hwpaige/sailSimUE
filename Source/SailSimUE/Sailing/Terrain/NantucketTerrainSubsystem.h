#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NantucketTerrainSubsystem.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/** One streamed terrain cell (matches web stream tile). */
USTRUCT()
struct FNantucketTileDesc
{
	GENERATED_BODY()

	FString Id;
	int32 Tx = 0;
	int32 Ty = 0;
	FString FileLod0;
	FString FileLod1;
	FVector2D WorldMin = FVector2D::ZeroVector;
	FVector2D WorldMax = FVector2D::ZeroVector;
	FVector2D WorldCenter = FVector2D::ZeroVector;
};

/**
 * Distance-streamed Nantucket land mesh (USGS DEM bake from sail-sim).
 *
 * Game-dev principles:
 *  - Spatial partitioning (16×16 cells)
 *  - Hysteresis load/unload radii (no thrash at boundaries)
 *  - Two LODs (full near boat, reduced shell)
 *  - Async-friendly file loads; components pooled per tile key
 *  - Never load entire island (~65MB) into memory at once
 */
UCLASS()
class SAILSIMUE_API UNantucketTerrainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Enable streaming (default true if ue_manifest.json exists). */
	UPROPERTY(EditAnywhere, Category = "Terrain")
	bool bEnabled = true;

	/** Chebyshev tile radius to load (web loadRadius). */
	UPROPERTY(EditAnywhere, Category = "Terrain|Stream", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LoadRadius = 3;

	/** Unload outside this radius (web unloadRadius ≥ load). */
	UPROPERTY(EditAnywhere, Category = "Terrain|Stream", meta = (ClampMin = "2", ClampMax = "10"))
	int32 UnloadRadius = 5;

	/**
	 * Chebyshev radius using full-res LOD0. Keep ≥ LoadRadius when possible so
	 * LOD0/LOD1 edges don't leave water gaps between mismatched mesh densities.
	 */
	UPROPERTY(EditAnywhere, Category = "Terrain|Stream", meta = (ClampMin = "0", ClampMax = "8"))
	int32 Lod0Radius = 3;

	/** Seconds between stream updates. */
	UPROPERTY(EditAnywhere, Category = "Terrain|Stream")
	float UpdateIntervalSec = 0.25f;

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	void SetEnabled(bool bInEnabled);

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	bool IsStreaming() const { return bEnabled && bManifestLoaded; }

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	int32 GetResidentTileCount() const { return Resident.Num(); }

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	FString GetStatusLine() const;

	/** Force re-resolve content path + reload manifest. */
	UFUNCTION(BlueprintCallable, Category = "Terrain")
	bool ReloadManifest();

	/** Immediate stream pass around Focus (call after boat snaps to harbor). */
	UFUNCTION(BlueprintCallable, Category = "Terrain")
	void ForceStreamAround(FVector FocusWorld);

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	int32 GetResidentVertexCount() const { return ResidentVerts; }

private:
	struct FResidentTile
	{
		int32 Tx = 0;
		int32 Ty = 0;
		int32 Lod = 0; // 0 full, 1 reduced
		int32 VertCount = 0;
		TObjectPtr<UProceduralMeshComponent> Mesh;
	};

	bool bManifestLoaded = false;
	FString TerrainRoot;
	TArray<FNantucketTileDesc> TileDescs;
	TMap<uint64, int32> KeyToDescIndex;
	TMap<uint64, FResidentTile> Resident;
	float TimeSinceUpdate = 0.f;
	int32 StreamCols = 16;
	int32 StreamRows = 16;
	int32 ResidentVerts = 0;

	static uint64 TileKey(int32 Tx, int32 Ty) { return (uint64(uint32(Tx)) << 32) | uint32(Ty); }

	bool ResolveTerrainRoot(FString& OutRoot) const;
	bool LoadManifestFromRoot(const FString& Root);
	void UpdateStreaming(const FVector& FocusWorld);
	void EnsureTile(int32 Tx, int32 Ty, int32 Lod);
	void ReleaseTile(uint64 Key);
	UProceduralMeshComponent* CreateTileMesh(const FString& RelPath);
	FVector GetFocusLocation() const;
	UMaterialInterface* GetOrCreateTerrainMaterial();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TerrainMaterial;
};
