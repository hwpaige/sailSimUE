#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NoaaChartSubsystem.generated.h"

class UTexture2D;

/**
 * Loads NOAA raster chart tiles (XYZ PNG pyramid) for the mini-map.
 * Tile layout matches sail-sim: charts/nantucket/{z}/{x}/{y}.png
 */
UCLASS()
class SAILSIMUE_API UNoaaChartSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Root folder containing z/x/y.png (e.g. .../charts/nantucket). */
	UFUNCTION(BlueprintCallable, Category = "Nav|Chart")
	FString GetTileRoot() const { return TileRoot; }

	UFUNCTION(BlueprintCallable, Category = "Nav|Chart")
	bool HasTileRoot() const { return bRootValid; }

	/**
	 * Get a loaded tile texture, or kick off load and return null until ready.
	 * Parent cascade: if exact missing, try coarser parents (caller draws sub-rect).
	 */
	UTexture2D* RequestTile(int32 Z, int32 X, int32 Y);

	struct FResolvedTile
	{
		UTexture2D* Texture = nullptr;
		/** Source UV rect in 0..1 over the texture (for parent cascade). */
		float U0 = 0.f, V0 = 0.f, U1 = 1.f, V1 = 1.f;
		int32 SourceZ = 0;
	};

	/** Exact tile or best parent covering (Z,X,Y). */
	FResolvedTile ResolveTile(int32 Z, int32 X, int32 Y, int32 MinZ = 9);

	void SetMaxCacheEntries(int32 N) { MaxCache = FMath::Clamp(N, 32, 512); }

private:
	enum class ETileState : uint8 { Loading, Ready, Missing };

	struct FTileEntry
	{
		ETileState State = ETileState::Loading;
		TObjectPtr<UTexture2D> Texture = nullptr;
		double LastUseTime = 0.0;
	};

	FString TileRoot;
	bool bRootValid = false;
	int32 MaxCache = 256;
	TMap<FString, FTileEntry> Cache;

	static FString TileKey(int32 Z, int32 X, int32 Y);
	FString TileFilePath(int32 Z, int32 X, int32 Y) const;
	void ResolveTileRoot();
	UTexture2D* LoadTileSync(int32 Z, int32 X, int32 Y);
	void EvictIfNeeded();
};
