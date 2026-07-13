#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Ocean/GerstnerWaterBodySampler.h"
#include "SailSimUE.h"
#include "EngineUtils.h"
#include "WaterZoneActor.h"
#include "WaterMeshComponent.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "UObject/UnrealType.h"

void USailOceanSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	EnsureSampler();
	UE_LOG(LogSailSim, Log, TEXT("SailOceanSubsystem init backend=%s"),
		Sampler.IsValid() ? *Sampler->GetBackendName().ToString() : TEXT("none"));
}

void USailOceanSubsystem::Deinitialize()
{
	Sampler.Reset();
	Super::Deinitialize();
}

bool USailOceanSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

void USailOceanSubsystem::EnsureSampler()
{
	if (!Sampler.IsValid())
	{
		Sampler = MakeUnique<FGerstnerWaterBodySampler>(GetWorld());
	}
	else if (FGerstnerWaterBodySampler* G = static_cast<FGerstnerWaterBodySampler*>(Sampler.Get()))
	{
		G->SetWorld(GetWorld());
	}
}

FOceanSample USailOceanSubsystem::SampleOcean(const FVector& WorldPos) const
{
	if (!Sampler.IsValid())
	{
		const_cast<USailOceanSubsystem*>(this)->EnsureSampler();
	}
	if (!Sampler.IsValid())
	{
		return {};
	}
	// Keep sea time synced for procedural swell
	FSeaParams P = Sampler->GetSeaParams();
	if (UWorld* W = GetWorld())
	{
		P.TimeSec = W->GetTimeSeconds();
		const_cast<IOceanHeightSampler*>(Sampler.Get())->SetSeaParams(P);
	}
	return Sampler->Sample(WorldPos);
}

FOceanSampleBP USailOceanSubsystem::SampleOceanBP(FVector WorldPos) const
{
	const FOceanSample S = SampleOcean(WorldPos);
	FOceanSampleBP Out;
	Out.Surface = S.Surface;
	Out.Normal = S.Normal;
	Out.bValid = S.bValid;
	return Out;
}

void USailOceanSubsystem::SetSeaParams(const FSeaParams& Params)
{
	EnsureSampler();
	if (Sampler.IsValid())
	{
		Sampler->SetSeaParams(Params);
	}
}

FSeaParams USailOceanSubsystem::GetSeaParams() const
{
	return Sampler.IsValid() ? Sampler->GetSeaParams() : FSeaParams::DefaultOpenOcean();
}

FName USailOceanSubsystem::GetBackendName() const
{
	return Sampler.IsValid() ? Sampler->GetBackendName() : NAME_None;
}

void USailOceanSubsystem::EnsureOceanVisualCoverage(
	const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm)
{
	UWorld* World = GetWorld();
	if (!World) return;

	const float NeedFull = FMath::Clamp(ZoneExtentCm, 100000.f, 250000.f);
	const float LocalDiam = FMath::Clamp(LocalTessDiameterCm, 40000.f, 150000.f);

	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		AWaterZone* Zone = *It;

		// Local tessellation = render window around the view; wave field stays world-space.
		if (FBoolProperty* bLocalProp = FindFProperty<FBoolProperty>(
				AWaterZone::StaticClass(), TEXT("bEnableLocalOnlyTessellation")))
		{
			if (!bLocalProp->GetPropertyValue_InContainer(Zone))
			{
				bLocalProp->SetPropertyValue_InContainer(Zone, true);
				UE_LOG(LogSailSim, Log, TEXT("Ocean: LocalOnlyTessellation ON (%s)"), *Zone->GetName());
			}
		}
		if (FStructProperty* LocalExtProp = FindFProperty<FStructProperty>(
				AWaterZone::StaticClass(), TEXT("LocalTessellationExtent")))
		{
			if (FVector* Ext = LocalExtProp->ContainerPtrToValuePtr<FVector>(Zone))
			{
				*Ext = FVector(LocalDiam, LocalDiam, 20000.f);
			}
		}

		const FVector2D Cur = Zone->GetZoneExtent();
		if (Cur.X < NeedFull || Cur.Y < NeedFull || Cur.X > 300000.f || Cur.Y > 300000.f)
		{
			Zone->SetZoneExtent(FVector2D(NeedFull, NeedFull));
		}
		Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);

		if (UWaterMeshComponent* WaterMesh = Zone->GetWaterMeshComponent())
		{
			WaterMesh->SetVisibility(true);
			WaterMesh->SetHiddenInGame(false);
			if (WaterMesh->GetTileSize() > 3200.f)
			{
				WaterMesh->SetTileSize(2400.f);
			}
		}

		UE_LOG(LogSailSim, Log,
			TEXT("Ocean visual: zone=%s boat=(%.0f,%.0f) localTess=ON diam=%.0f extent=%.0f backend=%s"),
			*Zone->GetName(), BoatWorldPos.X, BoatWorldPos.Y, LocalDiam, NeedFull,
			*GetBackendName().ToString());
	}

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(false);
		if (UWaterBodyComponent* Comp = It->GetWaterBodyComponent())
		{
			Comp->SetVisibility(true);
			Comp->SetHiddenInGame(false);
		}
	}
}
