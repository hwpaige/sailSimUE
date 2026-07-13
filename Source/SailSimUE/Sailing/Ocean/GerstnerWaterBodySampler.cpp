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

void FGerstnerWaterBodySampler::ApplyProceduralSwell(
	const FVector& WorldXY, float TimeSec, FVector& InOutSurf, FVector& InOutNorm) const
{
	// World-space multi-Gerstner-ish swell driven by FSeaParams (not camera-attached).
	const float Amp = Sea.AmplitudeCm;
	const float Chop = FMath::Clamp(Sea.Choppiness, 0.f, 1.f);
	const float DirRad = FMath::DegreesToRadians(Sea.WaveDirDeg);
	const FVector2D Dir(FMath::Cos(DirRad), FMath::Sin(DirRad));
	const float X = WorldXY.X * 0.01f;
	const float Y = WorldXY.Y * 0.01f;
	const float T = TimeSec;

	// Three components: long swell + mid + chop
	const float A1 = Amp * 0.55f, K1 = 0.010f + 0.004f * (1.f - Chop), W1 = 0.9f;
	const float A2 = Amp * 0.30f, K2 = 0.018f + 0.01f * Chop, W2 = 1.5f;
	const float A3 = Amp * 0.18f * Chop, K3 = 0.032f, W3 = 2.2f;

	const float P1 = K1 * (X * Dir.X + Y * Dir.Y) + W1 * T;
	const float P2 = K2 * (X * Dir.X * 0.7f + Y * Dir.Y * 0.7f + X * 0.2f) + W2 * T + 1.1f;
	const float P3 = K3 * (-X * Dir.Y + Y * Dir.X) + W3 * T + 0.4f;

	const float S1 = FMath::Sin(P1), C1 = FMath::Cos(P1);
	const float S2 = FMath::Sin(P2), C2 = FMath::Cos(P2);
	const float S3 = FMath::Sin(P3), C3 = FMath::Cos(P3);

	InOutSurf.Z += A1 * S1 + A2 * S2 + A3 * S3;

	const float Dzx = (A1 * K1 * 0.01f * C1) * Dir.X
		+ (A2 * K2 * 0.01f * C2) * (Dir.X * 0.7f + 0.2f)
		+ (A3 * K3 * 0.01f * C3) * (-Dir.Y);
	const float Dzy = (A1 * K1 * 0.01f * C1) * Dir.Y
		+ (A2 * K2 * 0.01f * C2) * (Dir.Y * 0.7f)
		+ (A3 * K3 * 0.01f * C3) * (Dir.X);
	InOutNorm = FVector(-Dzx, -Dzy, 1.f).GetSafeNormal();
}

FOceanSample FGerstnerWaterBodySampler::Sample(const FVector& WorldPos) const
{
	FOceanSample Best;
	UWorld* W = World.Get();
	if (!W)
	{
		return Best;
	}

	const float TimeSec = Sea.TimeSec > 0.f ? Sea.TimeSec : W->GetTimeSeconds();
	const FVector Query(WorldPos.X, WorldPos.Y, WorldPos.Z);

	const EWaterBodyQueryFlags Flags =
		EWaterBodyQueryFlags::ComputeLocation
		| EWaterBodyQueryFlags::ComputeNormal
		| EWaterBodyQueryFlags::ComputeVelocity
		| EWaterBodyQueryFlags::IncludeWaves
		| EWaterBodyQueryFlags::SimpleWaves
		| EWaterBodyQueryFlags::IgnoreExclusionVolumes;

	float BestAbsDZ = TNumericLimits<float>::Max();
	bool bAnyWaveHeight = false;

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

		const float Dz = FMath::Abs(Surf.Z - WorldPos.Z);
		if (!Best.bValid || Dz < BestAbsDZ)
		{
			BestAbsDZ = Dz;
			Best.Surface = Surf;
			Best.Normal = Norm;
			Best.Velocity = Vel;
			Best.Depth = 0.f;
			Best.bValid = true;
		}
	}

	if (!Best.bValid)
	{
		// No water body: still provide world-space plane + swell so float works
		Best.Surface = FVector(WorldPos.X, WorldPos.Y, 0.f);
		Best.Normal = FVector::UpVector;
		Best.bValid = true;
		ApplyProceduralSwell(WorldPos, TimeSec, Best.Surface, Best.Normal);
		return Best;
	}

	// Blend plugin waves with our sea-param swell when plugin waves are weak
	if (!bAnyWaveHeight || Sea.AmplitudeCm > 5.f)
	{
		// Scale additive swell so we don't double when plugin already has big waves
		const float Mix = bAnyWaveHeight ? 0.35f : 1.f;
		FVector Surf = Best.Surface;
		FVector Norm = Best.Normal;
		const float SavedZ = Surf.Z;
		ApplyProceduralSwell(WorldPos, TimeSec, Surf, Norm);
		Best.Surface.Z = FMath::Lerp(SavedZ, Surf.Z, Mix);
		Best.Normal = FMath::Lerp(Best.Normal, Norm, Mix).GetSafeNormal();
	}

	return Best;
}
