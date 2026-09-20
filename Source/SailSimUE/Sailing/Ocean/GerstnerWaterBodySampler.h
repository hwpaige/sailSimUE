#pragma once

#include "Sailing/Ocean/IOceanHeightSampler.h"

/**
 * UE Water body sampler with Gerstner waves (IncludeWaves) for VPP/physics parity.
 * Height/normal come from the Water Body query (Gerstner when waves are assigned).
 * Class name kept for module continuity; backend name remains FlatWaterBody for logs.
 */
class FGerstnerWaterBodySampler : public IOceanHeightSampler
{
public:
	explicit FGerstnerWaterBodySampler(UWorld* InWorld);

	virtual FOceanSample Sample(const FVector& WorldPos) const override;
	virtual void SetSeaParams(const FSeaParams& Params) override;
	virtual FSeaParams GetSeaParams() const override { return Sea; }
	virtual FName GetBackendName() const override { return TEXT("FlatWaterBody"); }

	void SetWorld(UWorld* InWorld) { World = InWorld; }

private:
	TWeakObjectPtr<UWorld> World;
	FSeaParams Sea = FSeaParams::DefaultOpenOcean();
};
