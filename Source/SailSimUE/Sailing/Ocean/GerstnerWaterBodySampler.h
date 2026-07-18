#pragma once

#include "Sailing/Ocean/IOceanHeightSampler.h"

/**
 * Flat UE Water body sampler — no Gerstner, no procedural swell.
 * Height is the water body's constant surface Z (one infinite plane).
 * Class name kept for module continuity; backend name is FlatWaterBody.
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
