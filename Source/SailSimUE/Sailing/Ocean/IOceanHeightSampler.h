#pragma once

#include "CoreMinimal.h"
#include "Sailing/Ocean/SeaParams.h"

/** World-space ocean surface sample (boat moves through this field). */
struct FOceanSample
{
	FVector Surface = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	FVector Velocity = FVector::ZeroVector;
	float Depth = 0.f;
	bool bValid = false;
};

/**
 * Phase 5.2 — abstract height field so buoyancy / VPP / FX never hardcode
 * Water plugin vs FFT plugin vs custom Tessendorf.
 *
 * Semantics: Sample is always in **world space**. The boat moves; the field
 * is fixed in the world (rendering may only tessellate near the camera).
 */
class IOceanHeightSampler
{
public:
	virtual ~IOceanHeightSampler() = default;

	virtual FOceanSample Sample(const FVector& WorldPos) const = 0;
	virtual void SetSeaParams(const FSeaParams& Params) = 0;
	virtual FSeaParams GetSeaParams() const = 0;
	virtual FName GetBackendName() const = 0;
};
