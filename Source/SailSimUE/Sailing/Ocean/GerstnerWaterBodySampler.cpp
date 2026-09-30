#include "Sailing/Ocean/GerstnerWaterBodySampler.h"
#include "EngineUtils.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyTypes.h"
#include "Engine/World.h"

FGerstnerWaterBodySampler::FGerstnerWaterBodySampler(UWorld* InWorld)
	: World(InWorld)
{
}

void FGerstnerWaterBodySampler::SetSeaParams(const FSeaParams& Params)
{
	Sea = Params;
}

FOceanSample FGerstnerWaterBodySampler::Sample(const FVector& WorldPos) const
{
	FOceanSample Best;
	UWorld* W = World.Get();
	if (!W)
	{
		return Best;
	}

	// Continuous Gerstner field — same waves for visuals + height queries.
	const EWaterBodyQueryFlags Flags =
		EWaterBodyQueryFlags::ComputeLocation
		| EWaterBodyQueryFlags::ComputeNormal
		| EWaterBodyQueryFlags::IncludeWaves
		| EWaterBodyQueryFlags::SimpleWaves
		| EWaterBodyQueryFlags::IgnoreExclusionVolumes;

	float BestAbsDZ = TNumericLimits<float>::Max();
	const FVector Query(WorldPos.X, WorldPos.Y, WorldPos.Z);

	for (TActorIterator<AWaterBody> It(W); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp) continue;

		const TValueOrError<FWaterBodyQueryResult, EWaterBodyQueryError> QueryResult =
			Comp->TryQueryWaterInfoClosestToWorldLocation(Query, Flags);
		if (!QueryResult.HasValue())
		{
			continue;
		}

		const FWaterBodyQueryResult& R = QueryResult.GetValue();
		const FVector Surf = R.GetWaterSurfaceLocation();
		const FVector Norm = R.GetWaterSurfaceNormal().GetSafeNormal();
		const float Dz = FMath::Abs(Surf.Z - WorldPos.Z);
		if (!Best.bValid || Dz < BestAbsDZ)
		{
			BestAbsDZ = Dz;
			Best.Surface = Surf;
			Best.Normal = Norm.IsNearlyZero() ? FVector::UpVector : Norm;
			Best.Velocity = FVector::ZeroVector;
			Best.Depth = 0.f;
			Best.bValid = true;
		}
	}

	if (!Best.bValid)
	{
		// No water body: Z=0 plane under the boat.
		Best.Surface = FVector(WorldPos.X, WorldPos.Y, 0.f);
		Best.Normal = FVector::UpVector;
		Best.bValid = true;
	}

	return Best;
}
