#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EncAidSubsystem.generated.h"

class UStaticMesh;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/** One ENC aid-to-navigation record (from Content/Nav/enc_aids_nantucket.json). */
USTRUCT()
struct FEncAidDesc
{
	GENERATED_BODY()

	double Lat = 0.0;
	double Lon = 0.0;
	/** SM_Buoy_N index 1..9 */
	int32 MeshId = 1;
	FString Kind;
	FString Name;
	int32 Colour = 0;
	int32 CatLam = 0;
	FString Shape;
	/** UE world cm (+X north, +Y east) at waterplane Z=0. */
	FVector WorldCm = FVector::ZeroVector;
};

/**
 * Places Content/Buoys static meshes at NOAA ENC S-57 aid positions
 * (lateral buoys, beacons, mooring balls, daymarks, etc.).
 *
 * ISM actors are anchored near the boat; materials are forced to the
 * InstancedStaticMeshes usage flag so cold PIE always draws (pack materials
 * often lack that flag until a mid-session recompile).
 */
UCLASS()
class SAILSIMUE_API UEncAidSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	UPROPERTY(EditAnywhere, Category = "ENC|Aids")
	bool bEnabled = true;

	/** Show aids within this radius of focus (cm). Default ~8 km. */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids", meta = (ClampMin = "50000", ClampMax = "500000"))
	float LoadRadiusCm = 120000.f;

	/** Unload hysteresis (cm). */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids", meta = (ClampMin = "60000", ClampMax = "600000"))
	float UnloadRadiusCm = 160000.f;

	/**
	 * Base scale for channel navaids (lateral/special/beacon/daymark).
	 * Pack meshes are ~0.5–1 m at scale 1; 3× reads from chase cam at harbor distances.
	 */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids", meta = (ClampMin = "0.01", ClampMax = "20"))
	float MeshScale = 3.0f;

	/** Scale for floating mooring balls (SM_Buoy_1/2 mix). */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids", meta = (ClampMin = "0.01", ClampMax = "20"))
	float MooringScale = 2.5f;

	/** Vertical offset for navaids (cm). Positive = lift mesh up. */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids")
	float WaterlineOffsetCm = 0.f;

	/** Extra lift for floating mooring balls so they sit on the surface. */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids")
	float MooringZOffsetCm = 30.f;

	/**
	 * Private mooring field: mostly one quaint look (white + blue stripe + red bottom)
	 * with a few barn red / green / navy eccentrics. Soft plastic sheen — no wet clearcoat.
	 */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids|Moorings")
	bool bColorfulMooringBalls = true;

	/** Soft PE sheen only (0 = chalk, 1 = slightly glossier plastic). Clearcoat is always off. */
	UPROPERTY(EditAnywhere, Category = "ENC|Aids|Moorings", meta = (ClampMin = "0", ClampMax = "1"))
	float MooringBallGloss = 0.35f;

	UPROPERTY(EditAnywhere, Category = "ENC|Aids")
	float UpdateIntervalSec = 0.5f;

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	void SetEnabled(bool bInEnabled);

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	bool ReloadData();

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	void ForceStreamAround(FVector FocusWorld);

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	int32 GetAidCount() const { return Aids.Num(); }

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	int32 GetVisibleCount() const { return VisibleCount; }

	UFUNCTION(BlueprintCallable, Category = "ENC|Aids")
	FString GetStatusLine() const;

	/**
	 * World-space top of the nearest streamed mooring ball (metal ring / crown).
	 * Uses live ISM instance transforms + mesh AABB so scale/Z match what you see.
	 * Returns false if no mooring instance is streamed nearby.
	 */
	bool FindMooringRingWorld(const FVector& NearWorld, FVector& OutRingWorld,
		float MaxDistCm = 2500.f) const;

	/** Actor Z used for floating mooring instances (Waterline + MooringZOffset). */
	float GetMooringActorZ() const { return WaterlineOffsetCm + MooringZOffsetCm; }

private:
	TArray<FEncAidDesc> Aids;
	bool bDataLoaded = false;
	float Accum = 0.f;
	int32 VisibleCount = 0;
	FVector LastFocus = FVector::ZeroVector;

	/** Per mesh-id ISM actor for channel navaids (1..9). */
	UPROPERTY()
	TMap<int32, TObjectPtr<AActor>> IsmByMesh;

	/**
	 * Mooring balls: separate ISMs so they can use shiny solid colours without
	 * recolouring lateral marks that share SM_Buoy_1/2.
	 * Key = StyleIdx * 10 + MeshId  (StyleIdx = Color * NumPatterns + Pattern).
	 */
	UPROPERTY()
	TMap<int32, TObjectPtr<AActor>> IsmByMooring;

	/** One MID per named harbor "look" (not a full color×pattern matrix). */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> MooringColorMats;

	/** Count of FMooringLook recipes in EnsureMooringColorMaterials. */
	static constexpr int32 MooringLookCount = 8;

	/** Which aid indices currently have instances. */
	TSet<int32> Resident;

	/** Fixed ISM root — set once per session stream. */
	FVector IsmAnchor = FVector::ZeroVector;
	bool bAnchorValid = false;

	/** Re-place a few times after first stream (shader/ISM material compile on cold start). */
	int32 DeferredRepassLeft = 0;
	float DeferredRepassTimer = 0.f;

	bool ResolveDataPath(FString& OutPath) const;
	bool LoadAidsFromJson(const FString& AbsPath);
	UStaticMesh* LoadBuoyMesh(int32 MeshId) const;
	void PrepareMeshMaterialsForIsm(UStaticMesh* Mesh, UInstancedStaticMeshComponent* Ism) const;
	UInstancedStaticMeshComponent* EnsureIsm(int32 MeshId, const FVector& AnchorWorld);
	/** Mooring-only ISM with solid shiny colour (does not share navaid materials). */
	UInstancedStaticMeshComponent* EnsureMooringIsm(int32 MeshId, int32 StyleIndex, const FVector& AnchorWorld);
	void EnsureMooringColorMaterials();
	/** Stable weighted pick among named mooring looks. */
	int32 PickMooringStyleIndex(int32 AidIndex, const FEncAidDesc& A) const;
	static int32 MooringIsmKey(int32 StyleIndex, int32 MeshId) { return StyleIndex * 10 + MeshId; }
	float MooringMeshMinZ = -50.f;
	float MooringMeshMaxZ = 50.f;
	void RebuildAround(const FVector& Focus, bool bForceRebuild);
	void ClearAllInstances();
	void ClearInstancesKeepActors();
	FVector GetFocusLocation() const;
};
