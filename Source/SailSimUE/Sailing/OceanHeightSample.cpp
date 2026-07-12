#include "Sailing/OceanHeightSample.h"
#include "EngineUtils.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

FOceanSample FOceanHeightSample::SampleAt(
	const UWorld* World,
	const FVector& WorldXY,
	float PreferZ,
	bool bUsePreferZ)
{
	FOceanSample Best;
	if (!World)
	{
		return Best;
	}

	const FVector Query(WorldXY.X, WorldXY.Y, 50000.f);
	float BestAbsDZ = TNumericLimits<float>::Max();

	for (TActorIterator<AWaterBody> It(const_cast<UWorld*>(World)); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp) continue;

		FVector Surf, Norm, Vel;
		float Depth = 0.f;
		if (!Comp->GetWaterSurfaceInfoAtLocation(Query, Surf, Norm, Vel, Depth, false))
		{
			continue;
		}

		const float RefZ = bUsePreferZ ? PreferZ : 0.f;
		const float Dz = FMath::Abs(Surf.Z - RefZ);
		if (!Best.bValid || Dz < BestAbsDZ)
		{
			BestAbsDZ = Dz;
			Best.Surface = Surf;
			Best.Normal = Norm;
			Best.Depth = Depth;
			Best.bValid = true;
		}
	}
	return Best;
}
