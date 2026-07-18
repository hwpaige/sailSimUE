#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "NantucketStructuresSubsystem.generated.h"

class UProceduralMeshComponent;
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
	FVector2D WorldMin = FVector2D::ZeroVector;
	FVector2D WorldMax = FVector2D::ZeroVector;
	FVector2D WorldCenter = FVector2D::ZeroVector;
	int32 Vertices = 0;
};

/**
 * Distance-streamed Nantucket buildings & props (OSM houses, heroes, ENC lights,
 * trees, grass, hydrangeas) from sail-sim NAVT bake.
 *
 * Same spatial pattern as UNantucketTerrainSubsystem, tighter radii — harbor
 * tiles are ~500k verts of architecture detail.
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

	/** Chebyshev tile radius to load (web nav-structures ~1; default 2 for approach views). */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "1", ClampMax = "6"))
	int32 LoadRadius = 2;

	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "2", ClampMax = "8"))
	int32 UnloadRadius = 3;

	/** Full-res LOD0 within this chebyshev radius. */
	UPROPERTY(EditAnywhere, Category = "Structures|Stream", meta = (ClampMin = "0", ClampMax = "3"))
	int32 Lod0Radius = 1;

	UPROPERTY(EditAnywhere, Category = "Structures|Stream")
	float UpdateIntervalSec = 0.35f;

	UFUNCTION(BlueprintCallable, Category = "Structures")
	void SetEnabled(bool bInEnabled);

	UFUNCTION(BlueprintCallable, Category = "Structures")
	bool IsStreaming() const { return bEnabled && bManifestLoaded; }

	UFUNCTION(BlueprintCallable, Category = "Structures")
	int32 GetResidentTileCount() const { return Resident.Num(); }

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
		TObjectPtr<UProceduralMeshComponent> Mesh;
		TObjectPtr<UMaterialInstanceDynamic> Mid;
	};

	bool bManifestLoaded = false;
	FString StructuresRoot;
	TArray<FNantucketStructureTileDesc> TileDescs;
	TMap<uint64, int32> KeyToDescIndex;
	TMap<uint64, FResidentTile> Resident;
	float TimeSinceUpdate = 0.f;
	int32 StreamCols = 8;
	int32 StreamRows = 8;
	int32 ResidentVerts = 0;
	float Night01 = 0.f;
	float WindowEmissive = 3.2f;
	float LampEmissive = 5.0f;

	static uint64 TileKey(int32 Tx, int32 Ty) { return (uint64(uint32(Tx)) << 32) | uint32(Ty); }

	bool ResolveStructuresRoot(FString& OutRoot) const;
	bool LoadManifestFromRoot(const FString& Root);
	void UpdateStreaming(const FVector& FocusWorld);
	void EnsureTile(int32 Tx, int32 Ty, int32 Lod);
	void ReleaseTile(uint64 Key);
	UProceduralMeshComponent* CreateTileMesh(const FString& RelPath, int32* OutVerts, UMaterialInstanceDynamic** OutMid);
	void ApplyLightingToMid(UMaterialInstanceDynamic* Mid) const;
	void SyncNightFromOcean();
	void PublishPerfStats() const;
	FVector GetFocusLocation() const;
	UMaterialInterface* GetOrCreateStructureMaterial();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> StructureMaterial;
};
