#include "Sailing/OceanHeightSample.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Engine/World.h"

FOceanSample FOceanHeightSample::SampleAt(
	const UWorld* World,
	const FVector& WorldXY,
	float PreferZ,
	bool bUsePreferZ)
{
	if (!World)
	{
		return {};
	}

	if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const FVector Q(WorldXY.X, WorldXY.Y, bUsePreferZ ? PreferZ : WorldXY.Z);
		return Ocean->SampleOcean(Q);
	}

	// Subsystem unavailable (very early init) — empty; float code has fallbacks
	return {};
}
