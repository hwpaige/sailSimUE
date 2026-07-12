#include "Sailing/OceanHeightSample.h"
#include "EngineUtils.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyTypes.h"
#include "Engine/World.h"

namespace OceanSamplePrivate
{
	/** Visible fallback swell if the Water body has no Gerstner asset (or waves are zero). */
	static void ApplyFallbackSwell(const FVector& WorldXY, float TimeSec, FVector& InOutSurf, FVector& InOutNorm)
	{
		// Wavelengths ~25–60 m, amplitudes ~15–40 cm — readable bob/pitch without looking stormy.
		const float X = WorldXY.X * 0.01f; // cm → m-ish scale for k
		const float Y = WorldXY.Y * 0.01f;
		const float T = TimeSec;

		const float A1 = 28.f, K1 = 0.012f, W1 = 1.1f, P1 = 0.3f;
		const float A2 = 18.f, K2 = 0.021f, W2 = 1.7f, P2 = 1.7f;
		const float A3 = 12.f, K3 = 0.033f, W3 = 2.3f, P3 = 0.9f;

		const float S1 = FMath::Sin(K1 * X + W1 * T + P1);
		const float C1 = FMath::Cos(K1 * X + W1 * T + P1);
		const float S2 = FMath::Sin(K2 * (X * 0.7f + Y * 0.7f) + W2 * T + P2);
		const float C2 = FMath::Cos(K2 * (X * 0.7f + Y * 0.7f) + W2 * T + P2);
		const float S3 = FMath::Sin(K3 * Y + W3 * T + P3);
		const float C3 = FMath::Cos(K3 * Y + W3 * T + P3);

		const float Dz = A1 * S1 + A2 * S2 + A3 * S3;
		InOutSurf.Z += Dz;

		// Approximate normal from partial derivatives
		const float Dzx = A1 * K1 * 0.01f * C1 + A2 * K2 * 0.01f * 0.7f * C2;
		const float Dzy = A2 * K2 * 0.01f * 0.7f * C2 + A3 * K3 * 0.01f * C3;
		InOutNorm = FVector(-Dzx, -Dzy, 1.f).GetSafeNormal();
	}
}

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

	const FVector Query(WorldXY.X, WorldXY.Y, bUsePreferZ ? PreferZ : 0.f);
	float BestAbsDZ = TNumericLimits<float>::Max();
	bool bAnyWaveHeight = false;

	// IMPORTANT: GetWaterSurfaceInfoAtLocation does NOT include Gerstner waves.
	// Buoyancy must use TryQueryWaterInfo… with IncludeWaves.
	const EWaterBodyQueryFlags Flags =
		EWaterBodyQueryFlags::ComputeLocation
		| EWaterBodyQueryFlags::ComputeNormal
		| EWaterBodyQueryFlags::ComputeVelocity
		| EWaterBodyQueryFlags::IncludeWaves
		| EWaterBodyQueryFlags::SimpleWaves
		| EWaterBodyQueryFlags::IgnoreExclusionVolumes;

	for (TActorIterator<AWaterBody> It(const_cast<UWorld*>(World)); It; ++It)
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
		FVector Surf = R.GetWaterSurfaceLocation();
		FVector Norm = R.GetWaterSurfaceNormal();
		const FVector Vel = R.GetVelocity();

		if (Comp->HasWaves())
		{
			const float WaveH = R.GetWaveInfo().Height;
			if (FMath::Abs(WaveH) > 0.5f)
			{
				bAnyWaveHeight = true;
			}
		}

		const float RefZ = bUsePreferZ ? PreferZ : Surf.Z;
		const float Dz = FMath::Abs(Surf.Z - RefZ);
		if (!Best.bValid || Dz < BestAbsDZ)
		{
			BestAbsDZ = Dz;
			Best.Surface = Surf;
			Best.Normal = Norm;
			Best.Depth = 0.f;
			Best.bValid = true;
			(void)Vel;
		}
	}

	// If water body has no waves / zero amplitude, synthesize a gentle swell so
	// multipoint buoyancy still produces pitch/roll/bob.
	if (Best.bValid && !bAnyWaveHeight)
	{
		const float Time = World->GetTimeSeconds();
		OceanSamplePrivate::ApplyFallbackSwell(WorldXY, Time, Best.Surface, Best.Normal);
	}

	return Best;
}
