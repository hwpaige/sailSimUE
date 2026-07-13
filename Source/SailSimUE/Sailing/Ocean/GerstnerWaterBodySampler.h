#pragma once

#include "Sailing/Ocean/IOceanHeightSampler.h"

/**
 * Backend A-interim: UE Water plugin Gerstner bodies (world-space queries with waves).
 * Will be replaced or supplemented by FFT backend without changing boat float code.
 */
class FGerstnerWaterBodySampler : public IOceanHeightSampler
{
public:
	explicit FGerstnerWaterBodySampler(UWorld* InWorld);

	virtual FOceanSample Sample(const FVector& WorldPos) const override;
	virtual void SetSeaParams(const FSeaParams& Params) override;
	virtual FSeaParams GetSeaParams() const override { return Sea; }
	virtual FName GetBackendName() const override { return TEXT("GerstnerWaterBody"); }

	void SetWorld(UWorld* InWorld) { World = InWorld; }

private:
	TWeakObjectPtr<UWorld> World;
	FSeaParams Sea = FSeaParams::DefaultOpenOcean();

	/** If Water body has no/low waves, add world-space swell (still world-fixed, not camera-parented). */
	void ApplyProceduralSwell(const FVector& WorldXY, float TimeSec, FVector& InOutSurf, FVector& InOutNorm) const;
};
