#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Sailing/Ocean/IOceanHeightSampler.h"
#include "SailOceanSubsystem.generated.h"

/** BP-friendly sample. */
USTRUCT(BlueprintType)
struct FOceanSampleBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	FVector Surface = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	FVector Normal = FVector::UpVector;

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	bool bValid = false;
};

/**
 * World subsystem owning the active ocean height backend (Phase 5).
 * Gameplay always samples world-space; backends swap Gerstner → FFT later.
 */
UCLASS()
class SAILSIMUE_API USailOceanSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	FOceanSampleBP SampleOceanBP(FVector WorldPos) const;

	FOceanSample SampleOcean(const FVector& WorldPos) const;

	void SetSeaParams(const FSeaParams& Params);
	FSeaParams GetSeaParams() const;
	FName GetBackendName() const;

	/**
	 * Visual coverage only: local tessellation is a render window around the view.
	 * The wave field remains world-space (boat moves through water).
	 */
	void EnsureOceanVisualCoverage(const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm);

private:
	TUniquePtr<IOceanHeightSampler> Sampler;
	void EnsureSampler();
};
