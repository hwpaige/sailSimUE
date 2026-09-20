#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Ocean/GerstnerWaterBodySampler.h"
#include "Sailing/Terrain/NantucketTerrainSubsystem.h"
#include "Sailing/Terrain/NantucketStructuresSubsystem.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"
#include "EngineUtils.h"
#include "WaterZoneActor.h"
#include "WaterMeshComponent.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyOceanComponent.h"
#include "WaterBodyExclusionVolume.h"
#include "WaterBodyIslandActor.h"
#include "Sailing/Nav/NavGeo.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "WaterBodyTypes.h"
#include "WaterWaves.h"
#include "GerstnerWaterWaves.h"
#include "WaterSplineComponent.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "Components/PrimitiveComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Math/RandomStream.h"

void USailOceanSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	EnsureSampler();
	if (Sampler.IsValid())
	{
		Sampler->SetSeaParams(FSeaParams::DefaultOpenOcean());
	}
	UE_LOG(LogSailSim, Log, TEXT("SailOceanSubsystem init backend=%s (flat plane)"),
		Sampler.IsValid() ? *Sampler->GetBackendName().ToString() : TEXT("none"));
}

void USailOceanSubsystem::Deinitialize()
{
	DestroyNightSkyActors();
	Sampler.Reset();
	bOpenOceanPrepared = false;
	bWaterZonesConfigured = false;
	bTerrainHidden = false;
	bIslandHoleCollapsed = false;
	bGerstnerWavesEnsured = false;
	bWaveRenderDataRefreshed = false;
	bMaterialsPolished = false;
	bSkySeamsFixed = false;
	bLegacySkyDomeHidden = false;
	bNightSkyActive = false;
	LastAppliedZoneExtentCm = 0.f;
	LastAppliedLocalTessCm = 0.f;
	Super::Deinitialize();
}

bool USailOceanSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId USailOceanSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USailOceanSubsystem, STATGROUP_Tickables);
}

void USailOceanSubsystem::Tick(float DeltaTime)
{
	// Keep Water zone under the boat (harbor is far from map origin).
	UWorld* World = GetWorld();
	if (!World || !bOpenOceanPrepared) return;
	if (World->IsPreviewWorld()) return;

	SAIL_PERF_SCOPE(Ocean);
	FollowAccum += DeltaTime;
	TerrainHideAccum += DeltaTime;

	// Cheap follow check ~2×/sec — avoid hammering water actors every frame.
	if (FollowAccum < 0.5f) return;
	FollowAccum = 0.f;

	FVector Focus = LastZoneBoatXY;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* P = PC->GetPawn())
		{
			Focus = P->GetActorLocation();
		}
	}

	// Zone extent is ~2.8–4 km full; half-extent ≈ 1.4–2 km. Slide when the boat
	// has drifted ~900 m from last zone center — WITHOUT MarkForRebuild.
	// (Old path: every 200 m → PrepareOpenOcean → full mesh rebuild = multi-frame hitch.)
	const float FollowThreshCm = 90000.f; // 900 m
	if (FVector::Dist2D(Focus, LastZoneBoatXY) > FollowThreshCm)
	{
		FollowWaterZone(Focus);
	}

	// WP can stream landscape proxies back near origin; re-hide occasionally (cheap if none).
	if (TerrainHideAccum > 12.f)
	{
		TerrainHideAccum = 0.f;
		HideIslandTerrain();
		bTerrainHidden = true;
	}

	if (bNightSkyActive)
	{
		UpdateNightSkyFollow(Focus);
	}
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
		// Force flat: ignore amplitude/chop even if callers pass wavey params.
		FSeaParams Flat = Params;
		Flat.AmplitudeCm = 0.f;
		Flat.Choppiness = 0.f;
		Sampler->SetSeaParams(Flat);
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

void USailOceanSubsystem::PrepareOpenOcean(
	const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Always re-hide default WP island near origin (not our streamed Nantucket tiles).
	HideIslandTerrain();
	bTerrainHidden = true;

	// Root cause of the "empty cell": Water Body Ocean mesh always carves a hole from its
	// central island spline (template matches the landscape island). Collapse once.
	if (!bIslandHoleCollapsed)
	{
		CollapseOceanIslandHole();
		bIslandHoleCollapsed = true;
	}

	// After first full configure: subsequent calls (BeginPlay×2, Possess, startup frames)
	// only need a cheap zone follow + sky recenter — NOT MarkForRebuild.
	if (bWaterZonesConfigured && bOpenOceanPrepared)
	{
		const float NeedFull = FMath::Clamp(ZoneExtentCm, 200000.f, 400000.f);
		const float LocalDiam = FMath::Clamp(LocalTessDiameterCm, 40000.f, 70000.f); // 400–700 m sailing budget
		const bool bExtentChanged =
			FMath::Abs(NeedFull - LastAppliedZoneExtentCm) > 1000.f
			|| FMath::Abs(LocalDiam - LastAppliedLocalTessCm) > 1000.f;
		if (bExtentChanged)
		{
			ConfigureWaterZones(BoatWorldPos, ZoneExtentCm, LocalTessDiameterCm);
		}
		else
		{
			FollowWaterZone(BoatWorldPos);
		}
		FixSkyAndAtmosphereSeams(BoatWorldPos);
		LastZoneBoatXY = BoatWorldPos;
		return;
	}

	// First setup: full zone configure (one MarkForRebuild).
	ConfigureWaterZones(BoatWorldPos, ZoneExtentCm, LocalTessDiameterCm);

	// Order matters for light→dark pop (AAA TOD pattern):
	// 1) Capture map sun/sky baseline while still bright
	// 2) Re-anchor atmosphere under the boat (no skylight bake)
	// 3) Apply TOD / intensity on top of baseline
	// 4) Leave SkyLight RealTimeCapture on so ambient tracks the sun
	CaptureEnvBaselineIfNeeded();
	FixSkyAndAtmosphereSeams(BoatWorldPos);
	ApplyVolumetricCloudIntensity();
	ApplyFogIntensity();
	LastZoneBoatXY = BoatWorldPos;

	// Continuous ocean: Gerstner must be on the body BEFORE WaterInfo/mesh consume it.
	// (Assigning after ConfigureWaterZones left a black void — wave GPU + WaterInfo stale.)
	if (!bGerstnerWavesEnsured)
	{
		EnsureGerstnerWaterWaves();
	}
	RefreshWaterWaveRenderData();
	// Always re-polish after WaterInfo refresh so Enable Waves / absorption stick on new MIDs.
	bMaterialsPolished = false;
	PolishWaterMaterials();

	bOpenOceanPrepared = true;
}

void USailOceanSubsystem::FollowWaterZone(const FVector& BoatWorldPos)
{
	UWorld* World = GetWorld();
	if (!World) return;

	const FVector ZoneLoc(BoatWorldPos.X, BoatWorldPos.Y, 0.f);
	auto MakeMovable = [](USceneComponent* Comp)
	{
		if (!Comp) return;
		if (Comp->Mobility != EComponentMobility::Movable)
		{
			Comp->SetMobility(EComponentMobility::Movable);
		}
	};

	int32 ZonesMoved = 0;
	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		AWaterZone* Zone = *It;
		if (!IsValid(Zone)) continue;
		MakeMovable(Zone->GetRootComponent());
		if (UWaterMeshComponent* WaterMesh = Zone->GetWaterMeshComponent())
		{
			MakeMovable(WaterMesh);
		}
		const FVector CurLoc = Zone->GetActorLocation();
		// Teleport only when meaningfully off-center. Never SetZoneExtent / MarkForRebuild —
		// those rebuild the entire water mesh and were the main open-ocean hitch.
		if (FVector::Dist2D(CurLoc, ZoneLoc) > 5000.f)
		{
			Zone->SetActorLocation(ZoneLoc, false, nullptr, ETeleportType::TeleportPhysics);
			++ZonesMoved;
		}
	}

	// Keep ocean body origin near zone (templates parent fill to body).
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* Body = *It;
		if (!IsValid(Body)) continue;
		MakeMovable(Body->GetRootComponent());
		if (UWaterBodyComponent* BodyComp = Body->GetWaterBodyComponent())
		{
			MakeMovable(BodyComp);
		}
		const FVector CurLoc = Body->GetActorLocation();
		if (FVector::Dist2D(CurLoc, ZoneLoc) > 25000.f)
		{
			// Preserve Z so waterline stays put.
			Body->SetActorLocation(
				FVector(ZoneLoc.X, ZoneLoc.Y, CurLoc.Z),
				false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

	LastZoneBoatXY = BoatWorldPos;
	if (ZonesMoved > 0)
	{
		UE_LOG(LogSailSim, Verbose,
			TEXT("Ocean: followed water zone to (%.0f, %.0f) — no mesh rebuild"),
			ZoneLoc.X, ZoneLoc.Y);
	}
}

void USailOceanSubsystem::EnsureOceanVisualCoverage(
	const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm)
{
	// Flat Water plane coverage (no second wave system / no fill plane).
	PrepareOpenOcean(BoatWorldPos, ZoneExtentCm, LocalTessDiameterCm);
}

void USailOceanSubsystem::EnsureNantucketWaterExclusion()
{
	UWorld* World = GetWorld();
	if (!World) return;

	if (NantucketWaterExclusion.IsValid())
	{
		bNantucketWaterExclusionReady = true;
		return;
	}

	// DEM / ENC Nantucket bbox (matches nantucket-geo NANTUCKET_BBOX + heightmap_meta).
	// Slight pad so the beach apron still meets water outside the box edge.
	constexpr double South = 41.235;
	constexpr double North = 41.395;
	constexpr double West = -70.32;
	constexpr double East = -70.02;
	double X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
	FNavGeo::LatLonToWorldCm(South, West, X0, Y0);
	FNavGeo::LatLonToWorldCm(North, East, X1, Y1);
	const float MinX = static_cast<float>(FMath::Min(X0, X1));
	const float MaxX = static_cast<float>(FMath::Max(X0, X1));
	const float MinY = static_cast<float>(FMath::Min(Y0, Y1));
	const float MaxY = static_cast<float>(FMath::Max(Y0, Y1));
	const float Pad = 25000.f; // 250 m pad
	const FVector Center((MinX + MaxX) * 0.5f, (MinY + MaxY) * 0.5f, 5000.f);
	// Default PhysicsVolume brush is ~200 cm cube (half-extent 100). Scale to cover island.
	const float HalfX = (MaxX - MinX) * 0.5f + Pad;
	const float HalfY = (MaxY - MinY) * 0.5f + Pad;
	const float HalfZ = 15000.f; // ±150 m vertical — covers DEM peaks + freeboard
	const FVector Scale(HalfX / 100.f, HalfY / 100.f, HalfZ / 100.f);

	FActorSpawnParameters Sp;
	Sp.Name = NAME_None;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWaterBodyExclusionVolume* Vol = World->SpawnActor<AWaterBodyExclusionVolume>(
		AWaterBodyExclusionVolume::StaticClass(), Center, FRotator::ZeroRotator, Sp);
	if (!Vol)
	{
		UE_LOG(LogSailSim, Warning, TEXT("Failed to spawn Nantucket water exclusion volume"));
		return;
	}
	Vol->SetActorLabel(TEXT("SailSim_NantucketWaterExclusion"));
	Vol->Tags.Add(FName(TEXT("SailSim_NantucketWaterExclusion")));
	// Empty list + RemoveWaterBodiesListFromExclusion ⇒ exclude ALL overlapping water bodies.
	Vol->ExclusionMode = EWaterExclusionMode::RemoveWaterBodiesListFromExclusion;
	Vol->WaterBodies.Reset();
	Vol->SetActorScale3D(Scale);
	if (UBrushComponent* Brush = Vol->GetBrushComponent())
	{
		Brush->SetMobility(EComponentMobility::Movable);
		Brush->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
	// Register with every ocean body so mesh gen punches a dry hole over the island.
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		if (UWaterBodyComponent* Comp = It->GetWaterBodyComponent())
		{
			Comp->AddExclusionVolume(Vol);
			FOnWaterBodyChangedParams BodyParams;
			BodyParams.bShapeOrPositionChanged = true;
			Comp->UpdateAll(BodyParams);
		}
	}
	FWaterExclusionVolumeChangedParams Params;
	Params.bUserTriggered = false;
	Vol->UpdateOverlappingWaterBodies(Params);

	NantucketWaterExclusion = Vol;
	bNantucketWaterExclusionReady = true;
	UE_LOG(LogSailSim, Log,
		TEXT("Nantucket water exclusion: center=(%.0f,%.0f) half=(%.0f x %.0f) cm — ocean sheet stops over land"),
		Center.X, Center.Y, HalfX, HalfY);
}

void USailOceanSubsystem::HideIslandTerrain()
{
	UWorld* World = GetWorld();
	if (!World) return;

	int32 HiddenLand = 0;
	// Landscape + all World Partition streaming proxies (Open World island).
	// WP may load more proxies after the first pass — call this again from PrepareOpenOcean.
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Land = *It;
		if (!IsValid(Land)) continue;
		Land->SetActorHiddenInGame(true);
		Land->SetActorEnableCollision(false);
		Land->SetActorTickEnabled(false);
		// Also hide every landscape component so streamed proxies don't flash grid.
		TInlineComponentArray<UPrimitiveComponent*> PrimComps(Land);
		for (UPrimitiveComponent* Prim : PrimComps)
		{
			if (!Prim) continue;
			Prim->SetVisibility(false, true);
			Prim->SetHiddenInGame(true, true);
			Prim->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		++HiddenLand;
	}

	// Foliage / ISMs often painted on the island — hide large ground cover clusters.
	int32 HiddenFoliage = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!IsValid(A) || A->IsA<AWaterBody>() || A->IsA<AWaterZone>())
		{
			continue;
		}
		const FString Name = A->GetClass()->GetName();
		const FString ActorName = A->GetName();
		const bool bFoliageLike =
			Name.Contains(TEXT("Foliage"))
			|| Name.Contains(TEXT("InstancedFoliage"))
			|| ActorName.Contains(TEXT("Foliage"))
			|| ActorName.Contains(TEXT("Tree"))
			|| ActorName.Contains(TEXT("Rock"))
			|| ActorName.Contains(TEXT("Bush"));
		if (!bFoliageLike)
		{
			// Also hide any HISM-heavy actors near origin (typical island props).
			bool bHasHism = false;
			for (UActorComponent* Comp : A->GetComponents())
			{
				if (Cast<UHierarchicalInstancedStaticMeshComponent>(Comp)
					|| Cast<UInstancedStaticMeshComponent>(Comp))
				{
					bHasHism = true;
					break;
				}
			}
			if (!bHasHism)
			{
				continue;
			}
			// Only strip dense foliage near map origin (default island), not distant props.
			if (A->GetActorLocation().Size2D() > 80000.f)
			{
				continue;
			}
		}

		A->SetActorHiddenInGame(true);
		A->SetActorEnableCollision(false);
		++HiddenFoliage;
	}

	// Separate WaterBodyIsland actors also punch holes — remove them fully.
	// Never touch streamed Nantucket land tiles.
	int32 IslandsRemoved = 0;
	{
		TArray<AWaterBodyIsland*> Islands;
		for (TActorIterator<AWaterBodyIsland> It(World); It; ++It)
		{
			Islands.Add(*It);
		}
		for (AWaterBodyIsland* Island : Islands)
		{
			if (!IsValid(Island)) continue;
			for (TActorIterator<AWaterBody> Bit(World); Bit; ++Bit)
			{
				if (UWaterBodyComponent* Comp = Bit->GetWaterBodyComponent())
				{
					Comp->RemoveIsland(Island);
				}
			}
			Island->Destroy();
			++IslandsRemoved;
		}
	}

	if (HiddenLand > 0 || HiddenFoliage > 0 || IslandsRemoved > 0)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("Open ocean: hid %d landscape proxy(s), %d foliage actor(s), destroyed %d WaterBodyIsland(s)"),
			HiddenLand, HiddenFoliage, IslandsRemoved);
	}
}

void USailOceanSubsystem::CollapseOceanIslandHole()
{
	// Water Body Ocean mesh is always: outer OceanExtents rectangle MINUS a central hole
	// defined by the body's WaterSpline (Open World template matches the landscape island).
	// Hiding landscape does nothing about that hole — you must shrink the spline so the
	// hole is negligible. Then stock Water fills solidly; no second plane needed.
	UWorld* World = GetWorld();
	if (!World) return;

	// Tiny closed square in local space (~1 m). Mesh gen needs ≥3 spline segments.
	// Leave a sub-cm visual gap that is invisible at sailing scales.
	const float Half = 50.f; // cm
	const TArray<FVector> TinyLoop = {
		FVector(-Half, -Half, 0.f),
		FVector( Half, -Half, 0.f),
		FVector( Half,  Half, 0.f),
		FVector(-Half,  Half, 0.f),
	};

	int32 Collapsed = 0;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* Body = *It;
		if (!IsValid(Body)) continue;
		UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
		if (!Comp || Comp->GetWaterBodyType() != EWaterBodyType::Ocean) continue;

		UWaterSplineComponent* Spline = Comp->GetWaterSpline();
		if (!Spline) continue;

		// Measure previous hole so we only log meaningful collapses.
		const int32 PrevPts = Spline->GetNumberOfSplinePoints();
		FBox PrevBounds(ForceInit);
		for (int32 i = 0; i < PrevPts; ++i)
		{
			PrevBounds += Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::Local);
		}
		const float PrevExtent = FMath::Max(PrevBounds.GetExtent().X, PrevBounds.GetExtent().Y);

		// Already collapsed (re-entry / second call).
		if (PrevPts >= 3 && PrevExtent < 200.f)
		{
			continue;
		}

		Spline->SetClosedLoop(true, /*bUpdateSpline*/ false);
		Spline->SetSplinePoints(TinyLoop, ESplineCoordinateSpace::Local, /*bUpdateSpline*/ true);
		// Notify water body that the island outline changed.
		Spline->K2_SynchronizeAndBroadcastDataChange();

		FOnWaterBodyChangedParams Params;
		Params.bShapeOrPositionChanged = true;
		Params.bUserTriggered = true;
		Comp->UpdateAll(Params);
#if WITH_EDITOR
		Comp->UpdateWaterBodyRenderData();
#endif

		UE_LOG(LogSailSim, Log,
			TEXT("Collapsed ocean island hole on %s (prev island half-extent ~%.0f cm → %.0f cm)"),
			*Body->GetName(), PrevExtent, Half);
		++Collapsed;
	}

	if (Collapsed == 0)
	{
		UE_LOG(LogSailSim, Log, TEXT("Ocean island hole: no large spline to collapse (already tiny or no ocean body)"));
	}
}

void USailOceanSubsystem::ConfigureWaterZones(
	const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Harbor spawn is ~24 km from map origin — zone MUST be moved under the boat.
	// Extent alone (2–3 km) cannot cover a boat at NAV harbor coordinates.
	const float NeedFull = FMath::Clamp(ZoneExtentCm, 200000.f, 400000.f);
	// Local tess = high-detail water draw window (not physics).
	// Sailing-game GPU budget: 400–700 m diameter. Hard-cap 700 m — NEVER expand
	// to cover the Nantucket land load disc (that path blew tess to 2.8 km).
	// Farther water stays on cheap far LOD; land streaming is independent.
	constexpr float kLocalTessMinCm = 40000.f;  // 400 m
	constexpr float kLocalTessMaxCm = 70000.f;  // 700 m hard cap
	float LocalDiam = FMath::Clamp(LocalTessDiameterCm, kLocalTessMinCm, kLocalTessMaxCm);
	if (LocalTessDiameterCm > kLocalTessMaxCm + 1.f)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("Continuous ocean: clamping localTess %.0f→%.0f cm (sailing budget; no land-disc expand)"),
			LocalTessDiameterCm, LocalDiam);
	}
	const FVector ZoneLoc(BoatWorldPos.X, BoatWorldPos.Y, 0.f);

	// SetZoneExtent → OnExtentChanged → MarkForRebuild(All). Only call when values change.
	const bool bFirstConfigure = !bWaterZonesConfigured;
	const bool bExtentChanged =
		bFirstConfigure
		|| FMath::Abs(NeedFull - LastAppliedZoneExtentCm) > 1000.f
		|| FMath::Abs(LocalDiam - LastAppliedLocalTessCm) > 1000.f;

	// Template Open World left exclusion volumes / holes at the *map origin* island.
	// Only destroy those near origin. Keep / create the Nantucket exclusion so the
	// ocean sheet does not draw through dry DEM land (AAA water/land contract).
	if (bFirstConfigure)
	{
		int32 ExclDestroyed = 0;
		{
			TArray<AWaterBodyExclusionVolume*> Excl;
			for (TActorIterator<AWaterBodyExclusionVolume> It(World); It; ++It)
			{
				Excl.Add(*It);
			}
			for (AWaterBodyExclusionVolume* V : Excl)
			{
				if (!IsValid(V)) continue;
				// Preserve our Nantucket dry-land exclusion (and anything far from origin).
				const FString Label = V->GetActorNameOrLabel();
				if (Label.Contains(TEXT("NantucketWaterExclusion"))
					|| Label.Contains(TEXT("SailSim_Nantucket")))
				{
					continue;
				}
				// Template island exclusions live near map origin; Nantucket is ~20 km away.
				if (V->GetActorLocation().Size2D() > 120000.f)
				{
					continue;
				}
				for (TActorIterator<AWaterBody> Bit(World); Bit; ++Bit)
				{
					if (UWaterBodyComponent* Comp = Bit->GetWaterBodyComponent())
					{
						Comp->RemoveExclusionVolume(V);
					}
				}
				V->Destroy();
				++ExclDestroyed;
			}
		}
		if (ExclDestroyed > 0)
		{
			UE_LOG(LogSailSim, Log,
				TEXT("Open ocean: destroyed %d template water exclusion volume(s) near origin"),
				ExclDestroyed);
		}
		EnsureNantucketWaterExclusion();
	}

	// Harbor is ~24 km from map origin; Static mobility blocks SetActorLocation.
	auto MakeMovable = [](USceneComponent* Comp)
	{
		if (!Comp) return;
		if (Comp->Mobility != EComponentMobility::Movable)
		{
			Comp->SetMobility(EComponentMobility::Movable);
		}
	};

	int32 ZonesConfigured = 0;
	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		AWaterZone* Zone = *It;
		if (!IsValid(Zone)) continue;

		MakeMovable(Zone->GetRootComponent());
		if (UWaterMeshComponent* WaterMesh = Zone->GetWaterMeshComponent())
		{
			MakeMovable(WaterMesh);
		}

		// Keep zone centered on boat (Water local tess follows view, but zone
		// origin still bounds the water mesh / ocean fill).
		const FVector CurLoc = Zone->GetActorLocation();
		if (FVector::Dist2D(CurLoc, ZoneLoc) > 5000.f)
		{
			Zone->SetActorLocation(ZoneLoc, false, nullptr, ETeleportType::TeleportPhysics);
		}

		// Local tess = high-detail water under the view (and land). Far mesh
		// still fills the horizon outside this window.
		if (FBoolProperty* bLocalProp = FindFProperty<FBoolProperty>(
				AWaterZone::StaticClass(), TEXT("bEnableLocalOnlyTessellation")))
		{
			bLocalProp->SetPropertyValue_InContainer(Zone, true);
		}
		if (FStructProperty* LocalExtProp = FindFProperty<FStructProperty>(
				AWaterZone::StaticClass(), TEXT("LocalTessellationExtent")))
		{
			if (FVector* Ext = LocalExtProp->ContainerPtrToValuePtr<FVector>(Zone))
			{
				// Half-extent style in some UE versions is full diameter in others —
				// we store full diameter in LocalDiam and set XY equal (square window).
				*Ext = FVector(LocalDiam, LocalDiam, 20000.f);
			}
		}
		// PMC Nantucket land is not Landscape — auto-include does nothing useful
		// and can punch dry holes from the hidden origin island landscape.
		if (FBoolProperty* bLandProp = FindFProperty<FBoolProperty>(
				AWaterZone::StaticClass(), TEXT("bAutoIncludeLandscapesAsTerrain")))
		{
			bLandProp->SetPropertyValue_InContainer(Zone, false);
		}

		if (bExtentChanged)
		{
			// SetZoneExtent already MarkForRebuild(All) via OnExtentChanged — do not double-call.
			const FVector2D CurExt = Zone->GetZoneExtent();
			if (FMath::Abs(CurExt.X - NeedFull) > 1000.f || FMath::Abs(CurExt.Y - NeedFull) > 1000.f)
			{
				Zone->SetZoneExtent(FVector2D(NeedFull, NeedFull));
			}
			else
			{
				// Extents already match but first-time / tess props changed — one rebuild.
				Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);
			}
		}
		// else: location-only follow already done above — no mesh rebuild.

		if (UWaterMeshComponent* WaterMesh = Zone->GetWaterMeshComponent())
		{
			WaterMesh->SetVisibility(true);
			WaterMesh->SetHiddenInGame(false);
			// Smaller tiles near the local/far transition reduce huge quads that
			// climb over shoreline land (looks like water “displacing” terrain LOD).
			const float Tile = WaterMesh->GetTileSize();
			if (Tile < 6000.f || Tile > 12000.f)
			{
				WaterMesh->SetTileSize(10000.f); // 100 m
			}
			// Far-distance patch must reach true horizon (50 km).
			// Soft LOD handoff: same Gerstner field + continuous material normals (no hard crush at localTess edge).
			if (FFloatProperty* FarExt = FindFProperty<FFloatProperty>(
					UWaterMeshComponent::StaticClass(), TEXT("FarDistanceMeshExtent")))
			{
				FarExt->SetPropertyValue_InContainer(WaterMesh, 5000000.f);
			}
			// Prefer water that depth-tests cleanly against opaque land PMC.
			WaterMesh->SetCastShadow(false);
			WaterMesh->bAffectDynamicIndirectLighting = false;
		}

		++ZonesConfigured;
		UE_LOG(LogSailSim, Log,
			TEXT("Continuous ocean: zone=%s at boat=(%.0f,%.0f) loc=(%.0f,%.0f) localTess=%.0f extent=%.0f rebuild=%s backend=%s"),
			*Zone->GetName(), BoatWorldPos.X, BoatWorldPos.Y,
			Zone->GetActorLocation().X, Zone->GetActorLocation().Y,
			LocalDiam, NeedFull,
			bExtentChanged ? TEXT("yes") : TEXT("no"),
			*GetBackendName().ToString());
	}

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* Body = *It;
		if (!IsValid(Body)) continue;

		MakeMovable(Body->GetRootComponent());
		if (UWaterBodyComponent* BodyComp = Body->GetWaterBodyComponent())
		{
			MakeMovable(BodyComp);
		}

		// Ocean body was authored near map origin — relocate with the zone.
		const FVector BodyLoc = Body->GetActorLocation();
		if (FVector::Dist2D(BodyLoc, ZoneLoc) > 5000.f)
		{
			Body->SetActorLocation(
				FVector(ZoneLoc.X, ZoneLoc.Y, BodyLoc.Z),
				false, nullptr, ETeleportType::TeleportPhysics);
		}

		Body->SetActorHiddenInGame(false);
		UWaterBodyComponent* Comp = Body->GetWaterBodyComponent();
		if (!Comp) continue;

		Comp->SetVisibility(true);
		Comp->SetHiddenInGame(false);

		// Ocean body should not keep carving landscape (we hide terrain separately).
		if (FBoolProperty* bAffects = FindFProperty<FBoolProperty>(
				UWaterBodyComponent::StaticClass(), TEXT("bAffectsLandscape")))
		{
			bAffects->SetPropertyValue_InContainer(Comp, false);
		}

		// Full body update + zone rebuild only on first configure or extent change.
		// Re-entering PrepareOpenOcean (BeginPlay/Possess) used to MarkForRebuild every time.
		if (!bExtentChanged)
		{
			continue;
		}

		if (UWaterBodyOceanComponent* OceanComp = Cast<UWaterBodyOceanComponent>(Comp))
		{
			if (FStructProperty* OceanExtProp = FindFProperty<FStructProperty>(
					UWaterBodyOceanComponent::StaticClass(), TEXT("OceanExtents")))
			{
				if (FVector2D* Ext = OceanExtProp->ContainerPtrToValuePtr<FVector2D>(OceanComp))
				{
					const float Want = FMath::Max(NeedFull, 280000.f);
					*Ext = FVector2D(Want, Want);
				}
			}
#if WITH_EDITOR
			OceanComp->FillWaterZoneWithOcean();
#endif
		}

		FOnWaterBodyChangedParams Params;
		Params.bShapeOrPositionChanged = true;
		Params.bUserTriggered = true;
		Comp->UpdateAll(Params);
#if WITH_EDITOR
		Comp->UpdateWaterBodyRenderData();
#endif
		// Zone already rebuilt via SetZoneExtent / MarkForRebuild above — avoid a second All rebuild.
	}

	LastAppliedZoneExtentCm = NeedFull;
	LastAppliedLocalTessCm = LocalDiam;
	bWaterZonesConfigured = ZonesConfigured > 0 || bWaterZonesConfigured;
}

void USailOceanSubsystem::SetVolumetricCloudIntensity(float Intensity01)
{
	VolumetricCloudIntensity = FMath::Clamp(Intensity01, 0.f, 1.f);
	ApplyVolumetricCloudIntensity();
}

void USailOceanSubsystem::SetFogIntensity(float Intensity01)
{
	FogIntensity = FMath::Clamp(Intensity01, 0.f, 1.f);
	ApplyFogIntensity();
}

void USailOceanSubsystem::HideLegacySkyDome()
{
	// Open World / mixed templates often leave a StaticMeshActor with SM_SkySphere +
	// M_SimpleSkyDome. That mesh paints a fixed sky while SkyAtmosphere paints another —
	// looking around shows a hard light/dark seam where they disagree.
	UWorld* World = GetWorld();
	if (!World) return;

	int32 Hidden = 0;
	auto IsLegacySkyAsset = [](const UObject* Obj) -> bool
	{
		if (!Obj) return false;
		const FString Path = Obj->GetPathName();
		const FString Name = Obj->GetName();
		return Path.Contains(TEXT("SM_SkySphere"))
			|| Path.Contains(TEXT("M_SimpleSkyDome"))
			|| Path.Contains(TEXT("/Engine/EngineSky/"))
			|| Name.Contains(TEXT("SkySphere"))
			|| Name.Contains(TEXT("SimpleSkyDome"))
			|| Name.Contains(TEXT("BP_Sky_Sphere"));
	};

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!IsValid(A)) continue;
		// Never touch water / atmosphere / fog / clouds — only mesh sky domes.
		if (A->IsA(AWaterBody::StaticClass()) || A->IsA(AWaterZone::StaticClass())
			|| A->IsA(AExponentialHeightFog::StaticClass())
			|| A->IsA(ASkyLight::StaticClass()))
		{
			continue;
		}
		const FString ActorName = A->GetName();
		const FString Label = A->GetActorNameOrLabel();
		// Our runtime night sky (stars / moon disc) reuses engine sphere meshes — leave them alone.
		if (Label.StartsWith(TEXT("SailSim_")) || ActorName.Contains(TEXT("SailSim_")))
		{
			continue;
		}
		bool bLegacy = ActorName.Contains(TEXT("SkySphere"))
			|| Label.Contains(TEXT("SkySphere"))
			|| Label.Contains(TEXT("Sky Dome"))
			|| Label.Contains(TEXT("SkyDome"));

		if (!bLegacy)
		{
			TInlineComponentArray<UStaticMeshComponent*> Meshes(A);
			for (UStaticMeshComponent* SMC : Meshes)
			{
				if (!SMC) continue;
				if (IsLegacySkyAsset(SMC->GetStaticMesh()))
				{
					bLegacy = true;
					break;
				}
				const int32 NumMats = SMC->GetNumMaterials();
				for (int32 Mi = 0; Mi < NumMats; ++Mi)
				{
					if (IsLegacySkyAsset(SMC->GetMaterial(Mi)))
					{
						bLegacy = true;
						break;
					}
				}
				if (bLegacy) break;
			}
		}

		if (!bLegacy) continue;

		A->SetActorHiddenInGame(true);
		A->SetActorEnableCollision(false);
		A->SetActorTickEnabled(false);
		TInlineComponentArray<UPrimitiveComponent*> PrimComps(A);
		for (UPrimitiveComponent* Prim : PrimComps)
		{
			if (!Prim) continue;
			Prim->SetVisibility(false, true);
			Prim->SetHiddenInGame(true, true);
			Prim->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		++Hidden;
		UE_LOG(LogSailSim, Log, TEXT("Hidden legacy sky dome actor: %s"), *Label);
	}

	bLegacySkyDomeHidden = true;
	if (Hidden > 0)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("Sky conflict resolved: hid %d legacy SkySphere/SkyDome mesh(es); SkyAtmosphere is the only sky"),
			Hidden);
	}
	else
	{
		UE_LOG(LogSailSim, Log, TEXT("No legacy SkySphere mesh found (SkyAtmosphere only)"));
	}
}

void USailOceanSubsystem::ApplyFogIntensity()
{
	UWorld* World = GetWorld();
	if (!World) return;

	const float Intensity = FogIntensity;
	const bool bOn = Intensity > KINDA_SMALL_NUMBER;
	int32 Touched = 0;

	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		AExponentialHeightFog* FogActor = *It;
		if (!IsValid(FogActor)) continue;
		UExponentialHeightFogComponent* Fog = FogActor->GetComponent();
		if (!Fog) continue;

		// Capture template densities once.
		if (CachedFogDensity < 0.f)
		{
			CachedFogDensity = FMath::Max(Fog->FogDensity, 0.0001f);
			CachedSecondFogDensity = Fog->SecondFogData.FogDensity;
		}

		if (!bOn)
		{
			// Fully off for testing.
			Fog->SetFogDensity(0.f);
			Fog->SetSecondFogDensity(0.f);
			Fog->SetVisibility(false);
			FogActor->SetActorHiddenInGame(true);
		}
		else
		{
			FogActor->SetActorHiddenInGame(false);
			Fog->SetVisibility(true);
			Fog->SetFogDensity(CachedFogDensity * Intensity);
			const float SecondBase = (CachedSecondFogDensity >= 0.f) ? CachedSecondFogDensity : 0.f;
			Fog->SetSecondFogDensity(SecondBase * Intensity);
		}
		++Touched;
	}

	UE_LOG(LogSailSim, Log, TEXT("Fog intensity=%.2f (%s) actors=%d"),
		Intensity, bOn ? TEXT("on") : TEXT("OFF"), Touched);
}

void USailOceanSubsystem::ApplyVolumetricCloudIntensity()
{
	UWorld* World = GetWorld();
	if (!World) return;

	const bool bOn = VolumetricCloudIntensity > KINDA_SMALL_NUMBER;
	const float Intensity = VolumetricCloudIntensity;

	// Hard off via cvar so no residual cloud pass (clean seam test).
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricCloud")))
	{
		CVar->Set(bOn ? 1 : 0, ECVF_SetByCode);
	}

	int32 Touched = 0;
	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		UVolumetricCloudComponent* Cloud = *It;
		if (!IsValid(Cloud) || Cloud->GetWorld() != World) continue;

		// Cache original layer height once so we can restore when slider returns to 1.
		if (CachedCloudLayerHeightKm < 0.f)
		{
			CachedCloudLayerHeightKm = FMath::Max(Cloud->LayerHeight, 0.1f);
		}

		const bool bVisible = bOn;
		Cloud->SetVisibility(bVisible, true);
		if (AActor* Owner = Cloud->GetOwner())
		{
			Owner->SetActorHiddenInGame(!bVisible);
			Owner->SetActorEnableCollision(false);
		}

		if (bOn)
		{
			// Intermediate intensity thins the layer (0 already hidden).
			const float BaseH = (CachedCloudLayerHeightKm > 0.f) ? CachedCloudLayerHeightKm : 10.f;
			Cloud->SetLayerHeight(FMath::Max(BaseH * Intensity, 0.1f));
		}
		Cloud->MarkRenderStateDirty();
		++Touched;
	}

	UE_LOG(LogSailSim, Log, TEXT("Volumetric clouds intensity=%.2f (%s) components=%d"),
		Intensity, bOn ? TEXT("on") : TEXT("OFF"), Touched);
}

void USailOceanSubsystem::FixSkyAndAtmosphereSeams(const FVector& BoatWorldPos)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Always re-hide: WP can stream the dome back in.
	HideLegacySkyDome();

	const FVector FocusXY(BoatWorldPos.X, BoatWorldPos.Y, 0.f);
	auto MakeMovable = [](USceneComponent* Comp)
	{
		if (!Comp) return;
		if (Comp->Mobility != EComponentMobility::Movable)
		{
			Comp->SetMobility(EComponentMobility::Movable);
		}
	};

	// Do NOT force r.SkyAtmosphere.FastSkyLUT=0 while leaving
	// r.SkyAtmosphere.AerialPerspectiveLUT.FastApplyOnOpaque=1 — UE warns that
	// "using FastAerialPerspective without FastSky" makes the sky look wrong.
	// Both default ON together; the dual-sky-dome hide is the real seam fix.

	int32 SkyMoved = 0;
	int32 CloudFixed = 0;
	int32 FogFixed = 0;
	int32 SkyLightFixed = 0;

	// --- Sky Atmosphere ---
	// PlanetTopAtAbsoluteWorldOrigin samples planet geometry from (0,0,0). At harbor (~24 km)
	// the horizon/aerial-perspective can hard-cut when looking certain azimuths.
	// Re-anchor planet top under the boat so sky is continuous in every look direction.
	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		USkyAtmosphereComponent* Atm = *It;
		if (!IsValid(Atm) || Atm->GetWorld() != World) continue;

		MakeMovable(Atm);
		// TransformMode is BlueprintReadOnly — set via reflection.
		if (FEnumProperty* ModeProp = FindFProperty<FEnumProperty>(
				USkyAtmosphereComponent::StaticClass(), TEXT("TransformMode")))
		{
			void* ValuePtr = ModeProp->ContainerPtrToValuePtr<void>(Atm);
			if (ValuePtr)
			{
				const uint8 Mode = static_cast<uint8>(ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform);
				ModeProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(Mode));
			}
		}
		else if (FByteProperty* ByteProp = FindFProperty<FByteProperty>(
				USkyAtmosphereComponent::StaticClass(), TEXT("TransformMode")))
		{
			ByteProp->SetPropertyValue_InContainer(
				Atm, static_cast<uint8>(ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform));
		}

		// Smooth aerial perspective so the sky doesn't hard-switch with view angle.
		if (FFloatProperty* APStart = FindFProperty<FFloatProperty>(
				USkyAtmosphereComponent::StaticClass(), TEXT("AerialPerspectiveStartDepth")))
		{
			APStart->SetPropertyValue_InContainer(Atm, 0.f);
		}

		if (AActor* Owner = Atm->GetOwner())
		{
			MakeMovable(Owner->GetRootComponent());
			const FVector Cur = Owner->GetActorLocation();
			if (FVector::Dist2D(Cur, FocusXY) > 5000.f)
			{
				Owner->SetActorLocation(
					FVector(FocusXY.X, FocusXY.Y, Cur.Z),
					false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
		else
		{
			const FVector Cur = Atm->GetComponentLocation();
			if (FVector::Dist2D(Cur, FocusXY) > 5000.f)
			{
				Atm->SetWorldLocation(FVector(FocusXY.X, FocusXY.Y, Cur.Z));
			}
		}
		Atm->MarkRenderStateDirty();
		++SkyMoved;
	}

	// --- Volumetric clouds ---
	// Default TracingMaxDistance = 50 km from cloud-layer entry. Looking near the horizon
	// the ray march hard-stops → bright cloud sky on one side, dark empty sky on the other.
	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		UVolumetricCloudComponent* Cloud = *It;
		if (!IsValid(Cloud) || Cloud->GetWorld() != World) continue;

		MakeMovable(Cloud);
		if (CachedCloudLayerHeightKm < 0.f)
		{
			CachedCloudLayerHeightKm = FMath::Max(Cloud->LayerHeight, 0.1f);
		}

		// DistanceFromPointOfView avoids clipping the cloud top / horizon when max distance is finite.
		if (FEnumProperty* ModeProp = FindFProperty<FEnumProperty>(
				UVolumetricCloudComponent::StaticClass(), TEXT("TracingMaxDistanceMode")))
		{
			void* ValuePtr = ModeProp->ContainerPtrToValuePtr<void>(Cloud);
			if (ValuePtr)
			{
				const uint8 Mode = static_cast<uint8>(
					EVolumetricCloudTracingMaxDistanceMode::DistanceFromPointOfView);
				ModeProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(Mode));
			}
		}
		// km units. 120 km was still a large volumetric march for sailing sky.
		// 50 km covers the visual dome; fog sells the far band.
		Cloud->SetTracingMaxDistance(50.f);
		Cloud->SetTracingStartMaxDistance(80.f);
		// Sample scales: keep shape, cut march cost (was 0.75).
		if (Cloud->ViewSampleCountScale > 0.55f)
		{
			Cloud->SetViewSampleCountScale(0.55f);
		}
		if (Cloud->ShadowViewSampleCountScale > 0.55f)
		{
			Cloud->SetShadowViewSampleCountScale(0.55f);
		}

		// Zero aerial-perspective start on clouds so cloud→sky doesn't hard-cut.
		if (FFloatProperty* R0 = FindFProperty<FFloatProperty>(
				UVolumetricCloudComponent::StaticClass(), TEXT("AerialPespectiveRayleighScatteringStartDistance")))
		{
			R0->SetPropertyValue_InContainer(Cloud, 0.f);
		}
		if (FFloatProperty* M0 = FindFProperty<FFloatProperty>(
				UVolumetricCloudComponent::StaticClass(), TEXT("AerialPespectiveMieScatteringStartDistance")))
		{
			M0->SetPropertyValue_InContainer(Cloud, 0.f);
		}
		// Soft long fades instead of a hard edge.
		if (FFloatProperty* Rf = FindFProperty<FFloatProperty>(
				UVolumetricCloudComponent::StaticClass(), TEXT("AerialPespectiveRayleighScatteringFadeDistance")))
		{
			Rf->SetPropertyValue_InContainer(Cloud, 50.f);
		}
		if (FFloatProperty* Mf = FindFProperty<FFloatProperty>(
				UVolumetricCloudComponent::StaticClass(), TEXT("AerialPespectiveMieScatteringFadeDistance")))
		{
			Mf->SetPropertyValue_InContainer(Cloud, 50.f);
		}

		if (AActor* Owner = Cloud->GetOwner())
		{
			MakeMovable(Owner->GetRootComponent());
			const FVector Cur = Owner->GetActorLocation();
			if (FVector::Dist2D(Cur, FocusXY) > 5000.f)
			{
				Owner->SetActorLocation(
					FVector(FocusXY.X, FocusXY.Y, Cur.Z),
					false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
		Cloud->MarkRenderStateDirty();
		++CloudFixed;
	}

	// --- Exponential height fog ---
	// Non-zero FogCutoffDistance draws a hard sphere: inside foggy (light), outside clear (dark).
	// Volumetric fog also creates a hard horizon band far from origin — disable it for open ocean.
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		AExponentialHeightFog* FogActor = *It;
		if (!IsValid(FogActor)) continue;
		MakeMovable(FogActor->GetRootComponent());

		const FVector Cur = FogActor->GetActorLocation();
		if (FVector::Dist2D(Cur, FocusXY) > 5000.f)
		{
			FogActor->SetActorLocation(
				FVector(FocusXY.X, FocusXY.Y, Cur.Z),
				false, nullptr, ETeleportType::TeleportPhysics);
		}

		if (UExponentialHeightFogComponent* Fog = FogActor->GetComponent())
		{
			// 0 = no cutoff (continuous sky fog in every look direction).
			Fog->SetFogCutoffDistance(0.f);
			Fog->SetStartDistance(0.f);
			// Volumetric fog often seams at its max distance when camera is far from origin.
			Fog->SetVolumetricFog(false);
		}
		++FogFixed;
	}

	// --- Sky light ---
	// KEEP RealTimeCapture ON for open-ocean + TOD. Forcing it off and baking a
	// one-shot cubemap was the light→dark flash: first frames used the bright
	// map/realtime look, then UpdateSkyCaptureContents replaced ambient with a
	// dark/stale capture (especially with Lumen + sun moved under the boat).
	// AAA open-world day/night (Fortnite-style TOD, many UE5 samples) either:
	//   • leave SkyLight Real Time Capture enabled, or
	//   • recapture only after the final sun pose, never mid-setup.
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		ASkyLight* Sky = *It;
		if (!IsValid(Sky)) continue;
		if (USkyLightComponent* Comp = Sky->GetLightComponent())
		{
			if (!Comp->bRealTimeCapture)
			{
				Comp->bRealTimeCapture = true;
				Comp->MarkRenderStateDirty();
			}
			// Outdoor floor so Lumen + land don't sink to black ambient.
			if (Comp->Intensity < 0.5f)
			{
				Comp->SetIntensity(1.f);
			}
			++SkyLightFixed;
		}
	}
	// Do NOT RequestSkyLightRecapture here — sun/TOD apply next and would thrash.

	bSkySeamsFixed = true;
	UE_LOG(LogSailSim, Log,
		TEXT("Sky seams: atm=%d clouds=%d fog=%d skylight=%d (realtime kept) under boat=(%.0f,%.0f)"),
		SkyMoved, CloudFixed, FogFixed, SkyLightFixed, FocusXY.X, FocusXY.Y);
}

void USailOceanSubsystem::EnsureGerstnerWaterWaves()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Stock Water plugin ocean Gerstner (Engine/Plugins/.../Content/Waves).
	UWaterWavesAsset* OceanAsset = LoadObject<UWaterWavesAsset>(
		nullptr, TEXT("/Water/Waves/GerstnerWaves_Ocean.GerstnerWaves_Ocean"));

	int32 Ensured = 0;
	int32 Assigned = 0;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* Body = *It;
		if (!IsValid(Body)) continue;

		UWaterWavesBase* Existing = Body->GetWaterWaves();
		const bool bHasWaves = Existing && Existing->GetWaterWaves() != nullptr;
		if (!bHasWaves)
		{
			if (OceanAsset)
			{
				UWaterWavesAssetReference* Ref = NewObject<UWaterWavesAssetReference>(
					Body, NAME_None, RF_Transactional);
				Ref->SetWaterWavesAsset(OceanAsset);
				Body->SetWaterWaves(Ref);
			}
			else
			{
				// Fallback: runtime Gerstner in NATIVE_OCEAN_UE58 calm→open cruise band (cm).
				UGerstnerWaterWaves* Waves = NewObject<UGerstnerWaterWaves>(
					Body, NAME_None, RF_Transactional);
				UGerstnerWaterWaveGeneratorSimple* Gen = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves);
				Gen->NumWaves = 24;
				Gen->MinWavelength = 500.f;
				Gen->MaxWavelength = 4000.f;
				Gen->MinAmplitude = 8.f;
				Gen->MaxAmplitude = 45.f;
				Gen->WindAngleDeg = 225.f;
				Gen->DirectionAngularSpreadDeg = 40.f;
				Gen->SmallWaveSteepness = 0.35f;
				Gen->LargeWaveSteepness = 0.2f;
				Waves->GerstnerWaveGenerator = Gen;
				Waves->RecomputeWaves(true);
				Body->SetWaterWaves(Waves);
			}
			++Assigned;
		}
		++Ensured;

		// Always nudge GPU wave buffers — needed when waves were already on the body
		// but WaterInfo was built while flattened / before Live Coding.
		if (UWaterBodyComponent* Comp = Body->GetWaterBodyComponent())
		{
			Comp->RequestGPUWaveDataUpdate();
		}
	}

	bGerstnerWavesEnsured = true;
	UE_LOG(LogSailSim, Log,
		TEXT("Continuous ocean: Gerstner WaterWaves on %d body(s) (assigned=%d, asset=%s) — localTess budget unchanged"),
		Ensured, Assigned, OceanAsset ? TEXT("GerstnerWaves_Ocean") : TEXT("runtime-fallback"));
}

void USailOceanSubsystem::RefreshWaterWaveRenderData()
{
	UWorld* World = GetWorld();
	if (!World) return;

	int32 Bodies = 0;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		AWaterBody* Body = *It;
		if (!IsValid(Body)) continue;
		if (UWaterBodyComponent* Comp = Body->GetWaterBodyComponent())
		{
			Comp->RequestGPUWaveDataUpdate();
			// Ensure MIDs exist so PolishWaterMaterials can stick Enable Waves / absorption.
			Comp->GetWaterMaterialInstance();
			Comp->GetWaterStaticMeshMaterialInstance();
			++Bodies;
		}
	}

	int32 Zones = 0;
	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		AWaterZone* Zone = *It;
		if (!IsValid(Zone)) continue;
		// WaterInfo must resample Gerstner after waves are assigned (fixes black void).
		Zone->ForceUpdateWaterInfoTexture();
		Zone->MarkForRebuild(
			EWaterZoneRebuildFlags::UpdateWaterInfoTexture | EWaterZoneRebuildFlags::UpdateWaterMesh);
		++Zones;
	}

	UE_LOG(LogSailSim, Log,
		TEXT("Continuous ocean: refreshed wave render data (bodies=%d zones=%d WaterInfo+mesh)"),
		Bodies, Zones);
	bWaveRenderDataRefreshed = true;
}

void USailOceanSubsystem::PolishWaterMaterials()
{
	UWorld* World = GetWorld();
	if (!World) return;

	int32 Polished = 0;
	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp) continue;

		auto ApplyToMid = [](UMaterialInstanceDynamic* MID)
		{
			if (!MID) return false;
			// Continuous ocean: material waves on; soft near→far normal falloff (no hard crush).
			MID->SetScalarParameterValue(TEXT("Enable Waves"), 1.f);
			MID->SetScalarParameterValue(TEXT("Enable Ocean Foam"), 0.15f);
			MID->SetScalarParameterValue(TEXT("Enable Foam"), 0.15f);
			// Near readable; far still carries the same field so tess edge does not read as a wall.
			MID->SetScalarParameterValue(TEXT("Default Near Normal Strength"), 0.85f);
			MID->SetScalarParameterValue(TEXT("Default Distant Normal Strength"), 0.50f);
			MID->SetScalarParameterValue(TEXT("Default Distant Normal StrengthB"), 0.35f);
			// Milder extinction so Gerstner + SLW does not read as a black void.
			MID->SetVectorParameterValue(TEXT("Absorption"), FLinearColor(0.22f, 0.05f, 0.03f, 1.f));
			MID->SetVectorParameterValue(TEXT("Scattering"), FLinearColor(0.03f, 0.14f, 0.16f, 1.f));
			MID->SetVectorParameterValue(TEXT("ColorScaleBehindWater"), FLinearColor(0.14f, 0.30f, 0.34f, 1.f));
			return true;
		};

		bool bAny = false;
		bAny |= ApplyToMid(Comp->GetWaterMaterialInstance());
		bAny |= ApplyToMid(Comp->GetWaterStaticMeshMaterialInstance());
		if (bAny)
		{
			++Polished;
		}
	}

	bMaterialsPolished = true;
	// Re-apply current chop so polish doesn't wipe puff response.
	if (SurfaceChopIntensity > 0.f)
	{
		const float Keep = SurfaceChopIntensity;
		SurfaceChopIntensity = -1.f;
		SetSurfaceChopIntensity(Keep);
	}
	UE_LOG(LogSailSim, Log, TEXT("Continuous ocean materials: polished %d water body MID(s) (waves on, soft far normals)"), Polished);
}

void USailOceanSubsystem::SetSurfaceChopIntensity(float Intensity01)
{
	const float I = FMath::Clamp(Intensity01, 0.f, 1.f);
	if (FMath::IsNearlyEqual(I, SurfaceChopIntensity, 0.01f) && bMaterialsPolished)
	{
		return;
	}
	SurfaceChopIntensity = I;

	UWorld* World = GetWorld();
	if (!World) return;

	// Continuous near→far normals (same wave field). Distant floor stays meaningful so
	// the localTess edge does not read as a flat tile wall. localTess budget unchanged.
	const float NearN = FMath::Lerp(0.75f, 1.35f, I);
	const float DistN = FMath::Lerp(0.45f, 0.70f, I);
	const float DistNB = FMath::Lerp(0.32f, 0.55f, I);
	const float Foam = FMath::Lerp(0.12f, 0.28f, I * I);

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp) continue;

		auto Apply = [&](UMaterialInstanceDynamic* MID)
		{
			if (!MID) return;
			MID->SetScalarParameterValue(TEXT("Enable Waves"), 1.f);
			MID->SetScalarParameterValue(TEXT("Default Near Normal Strength"), NearN);
			MID->SetScalarParameterValue(TEXT("Default Distant Normal Strength"), DistN);
			MID->SetScalarParameterValue(TEXT("Default Distant Normal StrengthB"), DistNB);
			MID->SetScalarParameterValue(TEXT("Enable Foam"), Foam > 0.02f ? 1.f : 0.f);
			MID->SetScalarParameterValue(TEXT("Enable Ocean Foam"), Foam > 0.02f ? Foam : 0.f);
		};
		Apply(Comp->GetWaterMaterialInstance());
		Apply(Comp->GetWaterStaticMeshMaterialInstance());
	}
}

void USailOceanSubsystem::SoftenHorizonFog()
{
	// Intentionally empty. Earlier open-ocean fog density/color overrides
	// crushed the map's SkyAtmosphere / directional light / cloud look.
	// Horizon blend is better done with water material far opacity, not global fog.
}


void USailOceanSubsystem::RequestSkyLightRecapture(const TCHAR* Reason)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// With RealTimeCapture left on (open ocean), a manual bake is unnecessary and
	// causes the classic bright→dark pop when it replaces a good realtime probe.
	bool bAnyRealtime = false;
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (!IsValid(*It)) continue;
		if (USkyLightComponent* Comp = It->GetLightComponent())
		{
			if (Comp->bRealTimeCapture)
			{
				bAnyRealtime = true;
				break;
			}
		}
	}
	if (bAnyRealtime)
	{
		UE_LOG(LogSailSim, Verbose,
			TEXT("SkyLight recapture skipped (%s) — RealTimeCapture active"),
			Reason ? Reason : TEXT("?"));
		return;
	}

	const float Now = World->GetTimeSeconds();
	if (Now - LastSkyCaptureWorldTime < MinSkyCaptureIntervalSec && LastSkyCaptureWorldTime >= 0.f)
	{
		bSkyCapturePending = true;
		return;
	}

	int32 N = 0;
	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (!IsValid(*It)) continue;
		if (USkyLightComponent* Comp = It->GetLightComponent())
		{
			Comp->SetCaptureIsDirty();
			Comp->MarkRenderStateDirty();
			++N;
		}
	}
	if (N > 0)
	{
		USkyLightComponent::UpdateSkyCaptureContents(World);
		LastSkyCaptureWorldTime = Now;
		bSkyCapturePending = false;
		UE_LOG(LogSailSim, Verbose, TEXT("SkyLight recapture (%s) n=%d"), Reason ? Reason : TEXT("?"), N);
	}
}

void USailOceanSubsystem::CaptureEnvBaselineIfNeeded()
{
	if (EnvBaseline.bValid) return;
	UWorld* World = GetWorld();
	if (!World) return;

	// Prefer atmosphere sun light; fall back to brightest directional.
	float BestInt = -1.f;
	UDirectionalLightComponent* BestComp = nullptr;
	ADirectionalLight* BestActor = nullptr;
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		ADirectionalLight* Sun = *It;
		if (!IsValid(Sun)) continue;
		UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
		if (!C) continue;
		const bool bAtm = C->IsUsedAsAtmosphereSunLight();
		const bool bBetter = (BestComp == nullptr)
			|| (bAtm && !BestComp->IsUsedAsAtmosphereSunLight())
			|| (bAtm == BestComp->IsUsedAsAtmosphereSunLight() && C->Intensity > BestInt);
		if (bBetter)
		{
			BestInt = C->Intensity;
			BestComp = C;
			BestActor = Sun;
		}
	}
	if (BestComp && BestActor)
	{
		EnvBaseline.SunIntensity = BestComp->Intensity;
		EnvBaseline.SunColor = BestComp->LightColor;
		// Component world rotation is the true light direction (actor may be identity).
		EnvBaseline.SunRotation = BestComp->GetComponentRotation();
		if (EnvBaseline.SunRotation.Equals(FRotator::ZeroRotator, 0.05f))
		{
			EnvBaseline.SunRotation = BestActor->GetActorRotation();
		}
		EnvBaseline.SunSourceAngle = BestComp->LightSourceAngle;
	}
	else
	{
		EnvBaseline.SunIntensity = 10.f;
		// UE pitch = -elevation. Noon ~55° above horizon → pitch -55.
		EnvBaseline.SunRotation = FRotator(-55.f, -40.f, 0.f);
		EnvBaseline.SunSourceAngle = 0.5357f;
	}
	// UE convention: directional light +X is light travel direction, so
	// Pitch = -Elevation (negative pitch = sun above horizon). Map templates
	// often ship pitch≈0 (horizon) or even positive (below horizon = black).
	// Elevation = -Pitch; require elev ≥ 20° for a usable Fair Day noon.
	{
		const float Elev = -EnvBaseline.SunRotation.Pitch;
		if (Elev < 20.f)
		{
			UE_LOG(LogSailSim, Log,
				TEXT("Env baseline sun elev=%.1f° (pitch=%.1f) too low for noon — clamping elev to 55° (pitch -55)"),
				Elev, EnvBaseline.SunRotation.Pitch);
			EnvBaseline.SunRotation.Pitch = -55.f;
			if (FMath::IsNearlyZero(EnvBaseline.SunRotation.Yaw))
			{
				EnvBaseline.SunRotation.Yaw = -40.f;
			}
		}
	}
	if (EnvBaseline.SunIntensity < 1.f)
	{
		EnvBaseline.SunIntensity = 10.f;
	}

	// AAA outdoor: mark the sun as atmosphere light and apply pose immediately so
	// the first lit frame is not "horizon pitch 0 → then dark after TOD".
	if (BestComp && BestActor)
	{
		if (USceneComponent* Root = BestActor->GetRootComponent())
		{
			if (Root->Mobility != EComponentMobility::Movable)
			{
				Root->SetMobility(EComponentMobility::Movable);
			}
		}
		if (BestComp->Mobility != EComponentMobility::Movable)
		{
			BestComp->SetMobility(EComponentMobility::Movable);
		}
		BestActor->SetActorRotation(EnvBaseline.SunRotation);
		BestComp->SetWorldRotation(EnvBaseline.SunRotation);
		BestComp->SetIntensity(EnvBaseline.SunIntensity);
		BestComp->SetLightColor(EnvBaseline.SunColor);
		BestComp->SetAtmosphereSunLight(true);
		BestComp->SetAtmosphereSunLightIndex(0);
		BestComp->SetVisibility(true);
		BestComp->MarkRenderStateDirty();
	}

	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (USkyLightComponent* C = It->GetLightComponent())
		{
			EnvBaseline.SkyLightIntensity = C->Intensity;
			EnvBaseline.SkyLightColor = C->LightColor;
			break;
		}
	}
	// Empty/zero skylight capture → near-black ambient after first recapture.
	if (EnvBaseline.SkyLightIntensity < 0.35f)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("Env baseline skylight intensity=%.3f too low — using 1.0"),
			EnvBaseline.SkyLightIntensity);
		EnvBaseline.SkyLightIntensity = 1.f;
		if (EnvBaseline.SkyLightColor.GetLuminance() < 0.05f)
		{
			EnvBaseline.SkyLightColor = FLinearColor::White;
		}
	}

	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			EnvBaseline.FogDensity = FMath::Max(Fog->FogDensity, 0.00005f);
			EnvBaseline.SecondFogDensity = Fog->SecondFogData.FogDensity;
			EnvBaseline.FogInscattering = Fog->FogInscatteringLuminance;
			EnvBaseline.FogHeightFalloff = Fog->FogHeightFalloff;
			EnvBaseline.FogStartDistance = Fog->StartDistance;
			EnvBaseline.FogMaxOpacity = Fog->FogMaxOpacity;
			break;
		}
	}
	if (CachedFogDensity < 0.f)
	{
		CachedFogDensity = EnvBaseline.FogDensity;
		CachedSecondFogDensity = EnvBaseline.SecondFogDensity;
	}

	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		UVolumetricCloudComponent* Cloud = *It;
		if (!IsValid(Cloud) || Cloud->GetWorld() != World) continue;
		EnvBaseline.CloudLayerBottomKm = Cloud->LayerBottomAltitude;
		if (CachedCloudLayerHeightKm < 0.f)
		{
			CachedCloudLayerHeightKm = FMath::Max(Cloud->LayerHeight, 0.1f);
		}
		break;
	}

	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		USkyAtmosphereComponent* Atm = *It;
		if (!IsValid(Atm) || Atm->GetWorld() != World) continue;
		EnvBaseline.AtmMulti = Atm->MultiScatteringFactor;
		EnvBaseline.AtmRayleigh = Atm->RayleighScatteringScale;
		EnvBaseline.AtmMie = Atm->MieScatteringScale;
		EnvBaseline.AtmSkyLuminance = Atm->SkyLuminanceFactor;
		EnvBaseline.AtmHeightFogContribution = Atm->HeightFogContribution;
		EnvBaseline.bAtmCaptured = true;
		break;
	}

	EnvBaseline.CloudIntensity = VolumetricCloudIntensity;
	EnvBaseline.FogIntensity = FogIntensity;
	EnvBaseline.bValid = true;
	UE_LOG(LogSailSim, Log,
		TEXT("Env baseline captured: sunPitch=%.1f yaw=%.1f elev=%.1f int=%.2f fogDens=%.5f falloff=%.2f clouds=%.2f (Fair Day)"),
		EnvBaseline.SunRotation.Pitch, EnvBaseline.SunRotation.Yaw,
		-EnvBaseline.SunRotation.Pitch, // elev = -pitch (UE directional)
		EnvBaseline.SunIntensity, EnvBaseline.FogDensity,
		EnvBaseline.FogHeightFalloff, EnvBaseline.CloudIntensity);

	// Re-apply noon (Fair Day) so baseline fog/clouds/sun match the captured map look.
	if (bTimeOfDayDriven)
	{
		TimeOfDayHours = 12.f;
		ApplyTimeOfDayInternal();
	}
}

FSailEnvPresetDesc USailOceanSubsystem::LerpEnvDesc(const FSailEnvPresetDesc& A, const FSailEnvPresetDesc& B, float T)
{
	const float U = FMath::Clamp(T, 0.f, 1.f);
	FSailEnvPresetDesc O = A;
	O.Name = B.Name;
	O.Blurb = B.Blurb;
	O.SunElevationDeg = FMath::Lerp(A.SunElevationDeg, B.SunElevationDeg, U);
	O.SunYawOffsetDeg = FMath::Lerp(A.SunYawOffsetDeg, B.SunYawOffsetDeg, U);
	O.SunIntensityMul = FMath::Lerp(A.SunIntensityMul, B.SunIntensityMul, U);
	O.SunColor = FMath::Lerp(A.SunColor, B.SunColor, U);
	O.SunSourceAngleMul = FMath::Lerp(A.SunSourceAngleMul, B.SunSourceAngleMul, U);
	O.SkyLightIntensityMul = FMath::Lerp(A.SkyLightIntensityMul, B.SkyLightIntensityMul, U);
	O.SkyLightColor = FMath::Lerp(A.SkyLightColor, B.SkyLightColor, U);
	O.CloudIntensity = FMath::Lerp(A.CloudIntensity, B.CloudIntensity, U);
	O.CloudBottomOffsetKm = FMath::Lerp(A.CloudBottomOffsetKm, B.CloudBottomOffsetKm, U);
	O.FogIntensity = FMath::Lerp(A.FogIntensity, B.FogIntensity, U);
	O.FogDensityMul = FMath::Lerp(A.FogDensityMul, B.FogDensityMul, U);
	O.FogInscattering = FMath::Lerp(A.FogInscattering, B.FogInscattering, U);
	O.bOverrideFogColor = A.bOverrideFogColor || B.bOverrideFogColor;
	O.FogHeightFalloff = (A.FogHeightFalloff >= 0.f && B.FogHeightFalloff >= 0.f)
		? FMath::Lerp(A.FogHeightFalloff, B.FogHeightFalloff, U)
		: (B.FogHeightFalloff >= 0.f ? B.FogHeightFalloff : A.FogHeightFalloff);
	O.FogStartDistanceCm = (A.FogStartDistanceCm >= 0.f && B.FogStartDistanceCm >= 0.f)
		? FMath::Lerp(A.FogStartDistanceCm, B.FogStartDistanceCm, U)
		: (B.FogStartDistanceCm >= 0.f ? B.FogStartDistanceCm : A.FogStartDistanceCm);
	O.SurfaceChop = FMath::Lerp(A.SurfaceChop, B.SurfaceChop, U);
	O.AtmosphereMultiScatter = FMath::Lerp(A.AtmosphereMultiScatter, B.AtmosphereMultiScatter, U);
	O.AtmosphereRayleighMul = FMath::Lerp(A.AtmosphereRayleighMul, B.AtmosphereRayleighMul, U);
	O.AtmosphereMieMul = FMath::Lerp(A.AtmosphereMieMul, B.AtmosphereMieMul, U);
	O.SkyLuminance = FMath::Lerp(A.SkyLuminance, B.SkyLuminance, U);
	O.HeightFogContribution = (A.HeightFogContribution >= 0.f && B.HeightFogContribution >= 0.f)
		? FMath::Lerp(A.HeightFogContribution, B.HeightFogContribution, U)
		: (B.HeightFogContribution >= 0.f ? B.HeightFogContribution : A.HeightFogContribution);
	O.SuggestedWindKn = -1.f;
	return O;
}

FSailEnvPresetDesc USailOceanSubsystem::BuildTimeOfDayDesc() const
{
	// All daytime phases are derived from the captured Fair Day baseline so golden
	// hour / dusk keep the same polished look — only elevation and gentle warmth change.
	// No purple fog or candy-orange sun.
	const FSailEnvPresetDesc NightTable = GetSailEnvPresetDesc(ESailEnvPreset::Night);

	FSailEnvPresetDesc Fair = GetSailEnvPresetDesc(ESailEnvPreset::FairDay);
	// Elevation (deg above horizon) — not UE pitch. elev = -pitch.
	const float CapturedElev = EnvBaseline.bValid ? (-EnvBaseline.SunRotation.Pitch) : Fair.SunElevationDeg;
	const float FairElev = (CapturedElev >= 20.f) ? CapturedElev : FMath::Max(Fair.SunElevationDeg, 55.f);
	const FLinearColor FairSun = EnvBaseline.bValid ? EnvBaseline.SunColor : Fair.SunColor;
	const FLinearColor FairSky = EnvBaseline.bValid ? EnvBaseline.SkyLightColor : Fair.SkyLightColor;
	if (EnvBaseline.bValid)
	{
		Fair.SunElevationDeg = FairElev;
		Fair.SunYawOffsetDeg = 0.f;
		Fair.SunIntensityMul = 1.f;
		Fair.SunColor = FairSun;
		Fair.SunSourceAngleMul = 1.f;
		Fair.SkyLightIntensityMul = 1.f;
		Fair.SkyLightColor = FairSky;
		Fair.CloudIntensity = 1.f;
		Fair.CloudBottomOffsetKm = 0.f;
		Fair.FogIntensity = 1.f;
		Fair.FogDensityMul = 1.f;
		Fair.FogInscattering = EnvBaseline.FogInscattering;
		Fair.bOverrideFogColor = false;
		Fair.FogHeightFalloff = -1.f;
		Fair.FogStartDistanceCm = -1.f;
		Fair.SurfaceChop = 0.08f;
		Fair.AtmosphereMultiScatter = 1.f;
		Fair.AtmosphereRayleighMul = 1.f;
		Fair.AtmosphereMieMul = 1.f;
		Fair.SkyLuminance = FLinearColor::White;
		Fair.HeightFogContribution = -1.f;
		Fair.Name = TEXT("Fair Day");
	}

	// Subtle late-day warmth (still Fair family).
	FSailEnvPresetDesc Golden = Fair;
	Golden.Name = TEXT("Golden Hour");
	Golden.SunElevationDeg = FMath::Clamp(FairElev - 4.f, 2.f, 18.f);
	Golden.SunYawOffsetDeg = 6.f;
	Golden.SunIntensityMul = 0.94f;
	// ~12% warm lean on the captured sun color — not pure orange.
	Golden.SunColor = FMath::Lerp(FairSun, FLinearColor(1.f, 0.93f, 0.84f), 0.18f);
	Golden.SunSourceAngleMul = 1.12f;
	Golden.SkyLightIntensityMul = 0.96f;
	Golden.SkyLightColor = FMath::Lerp(FairSky, FLinearColor(1.f, 0.98f, 0.96f), 0.15f);
	Golden.FogDensityMul = 1.04f;
	Golden.bOverrideFogColor = false;
	Golden.AtmosphereMieMul = 1.08f;
	Golden.SkyLuminance = FLinearColor(1.f, 0.995f, 0.985f);
	Golden.SurfaceChop = 0.09f;

	// Dusk: dimmer Fair Day, cool-neutral ambient (no violet).
	FSailEnvPresetDesc Dusk = Fair;
	Dusk.Name = TEXT("Dusk");
	Dusk.SunElevationDeg = FMath::Clamp(FMath::Min(FairElev, 3.5f) - 1.5f, -1.5f, 6.f);
	Dusk.SunYawOffsetDeg = 12.f;
	Dusk.SunIntensityMul = 0.48f;
	Dusk.SunColor = FMath::Lerp(FairSun, FLinearColor(1.f, 0.91f, 0.82f), 0.28f);
	Dusk.SunSourceAngleMul = 1.45f;
	Dusk.SkyLightIntensityMul = 0.58f;
	Dusk.SkyLightColor = FMath::Lerp(FairSky, FLinearColor(0.94f, 0.95f, 0.98f), 0.35f);
	Dusk.FogDensityMul = 1.06f;
	Dusk.FogInscattering = FMath::Lerp(EnvBaseline.bValid ? EnvBaseline.FogInscattering : FLinearColor(0.45f, 0.55f, 0.7f),
		FLinearColor(0.42f, 0.48f, 0.56f), 0.25f);
	Dusk.bOverrideFogColor = false; // never force purple fog
	Dusk.FogStartDistanceCm = 200.f;
	Dusk.AtmosphereMultiScatter = 0.92f;
	Dusk.AtmosphereMieMul = 1.15f;
	Dusk.SkyLuminance = FLinearColor(0.94f, 0.95f, 0.97f);
	Dusk.HeightFogContribution = 1.05f;
	Dusk.SurfaceChop = 0.10f;

	// Night stays a true night table, but blend approach softens purple via Lerp from Dusk.
	FSailEnvPresetDesc Night = NightTable;
	Night.SunColor = FMath::Lerp(Dusk.SunColor, NightTable.SunColor, 0.85f);
	Night.SkyLightColor = FMath::Lerp(FLinearColor(0.55f, 0.60f, 0.72f), NightTable.SkyLightColor, 0.7f);
	Night.FogInscattering = FMath::Lerp(
		EnvBaseline.bValid ? EnvBaseline.FogInscattering : FLinearColor(0.02f, 0.03f, 0.04f),
		FLinearColor(0.02f, 0.025f, 0.04f), 0.8f);
	Night.SkyLuminance = FLinearColor(0.04f, 0.05f, 0.07f);

	// Dawn: mirror of soft dusk, not Night×Golden candy.
	FSailEnvPresetDesc Dawn = Dusk;
	Dawn.Name = TEXT("Dawn");
	Dawn.SunElevationDeg = FMath::Clamp(FMath::Max(FairElev * 0.15f, 4.f), 3.f, 10.f);
	Dawn.SunYawOffsetDeg = -10.f;
	Dawn.SunIntensityMul = 0.52f;
	Dawn.SunColor = FMath::Lerp(FairSun, FLinearColor(1.f, 0.93f, 0.86f), 0.22f);
	Dawn.SkyLightIntensityMul = 0.50f;
	Dawn.SkyLightColor = FMath::Lerp(FairSky, FLinearColor(0.96f, 0.97f, 0.99f), 0.3f);

	struct FKey
	{
		float Hour;
		const FSailEnvPresetDesc* Desc;
	};
	const FKey Keys[] = {
		{ 0.f, &Night },
		{ 5.0f, &Night },
		{ 6.2f, &Dawn },
		{ 8.0f, &Fair },
		{ 12.0f, &Fair },
		{ 16.5f, &Fair },
		{ 17.8f, &Golden },
		{ 19.4f, &Dusk },
		{ 21.2f, &Night },
		{ 24.0f, &Night },
	};
	const int32 N = UE_ARRAY_COUNT(Keys);
	const float H = FMath::Fmod(FMath::Max(0.f, TimeOfDayHours), 24.f);

	for (int32 I = 0; I < N - 1; ++I)
	{
		if (H >= Keys[I].Hour && H <= Keys[I + 1].Hour)
		{
			const float Span = FMath::Max(1e-3f, Keys[I + 1].Hour - Keys[I].Hour);
			const float T = (H - Keys[I].Hour) / Span;
			// Smoothstep so midday↔golden doesn't snap into a yellow cast.
			const float Ts = T * T * (3.f - 2.f * T);
			FSailEnvPresetDesc Out = LerpEnvDesc(*Keys[I].Desc, *Keys[I + 1].Desc, Ts);
			Out.Name = (Ts < 0.5f) ? Keys[I].Desc->Name : Keys[I + 1].Desc->Name;
			return Out;
		}
	}
	return Fair;
}

void USailOceanSubsystem::ApplyTimeOfDayInternal()
{
	CaptureEnvBaselineIfNeeded();
	if (!EnvBaseline.bValid) return;

	bTimeOfDayDriven = true;
	const FSailEnvPresetDesc Desc = BuildTimeOfDayDesc();

	// Map to discrete preset for systems that key off ActiveEnvPreset (lights, compass).
	const float NightAmt = GetNightAmount();
	if (NightAmt > 0.72f)
	{
		ActiveEnvPreset = ESailEnvPreset::Night;
	}
	else if (NightAmt > 0.35f)
	{
		ActiveEnvPreset = ESailEnvPreset::Dusk;
	}
	else if (NightAmt > 0.12f)
	{
		ActiveEnvPreset = ESailEnvPreset::GoldenHour;
	}
	else
	{
		ActiveEnvPreset = ESailEnvPreset::FairDay;
	}

	// Fog color / falloff only — density stays on user FogIntensity slider.
	ApplyFogLook(Desc);
	// Keep baseline cloud layer altitude for fair; slight dusk/night drop is OK.
	ApplyCloudLayerLook(Desc);
	// Do NOT call SetFogIntensity / SetVolumetricCloudIntensity — user sliders win.

	ApplySunAndSky(Desc);
	ApplyAtmosphereLook(Desc);
	ApplyNightSky(NightAmt > 0.55f);

	// Re-apply density scales after fog look rewrote CachedFogDensity.
	ApplyFogIntensity();
}

void USailOceanSubsystem::SetTimeOfDayHours(float Hours0To24)
{
	TimeOfDayHours = FMath::Fmod(Hours0To24, 24.f);
	if (TimeOfDayHours < 0.f) TimeOfDayHours += 24.f;
	ApplyTimeOfDayInternal();
}

void USailOceanSubsystem::SetTimeOfDay01(float Norm01)
{
	SetTimeOfDayHours(FMath::Clamp(Norm01, 0.f, 1.f) * 24.f);
}

void USailOceanSubsystem::SetSeason01(float InSeason01)
{
	Season01 = FMath::Clamp(InSeason01, 0.f, 1.f);
	// Always push (prefs restore / first tile stream may share the default 0.5).
	ApplySeasonToWorld();
}

FString USailOceanSubsystem::GetSeasonLabel() const
{
	// Continuous year labels (peaks at W/Sp/Su/A).
	const float S = FMath::Clamp(Season01, 0.f, 1.f);
	if (S < 0.0625f || S >= 0.9375f) return TEXT("Winter");
	if (S < 0.1875f) return TEXT("Late Winter");
	if (S < 0.3125f) return TEXT("Spring");
	if (S < 0.4375f) return TEXT("Late Spring");
	if (S < 0.5625f) return TEXT("Summer");
	if (S < 0.6875f) return TEXT("Late Summer");
	if (S < 0.8125f) return TEXT("Autumn");
	return TEXT("Late Autumn");
}

void USailOceanSubsystem::ApplySeasonToWorld()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Global MPC so any material with CollectionParameter "Season01" updates instantly.
	static const TCHAR* MpcPath = TEXT("/Game/Materials/Navt/MPC_Season.MPC_Season");
	if (UMaterialParameterCollection* Mpc = LoadObject<UMaterialParameterCollection>(nullptr, MpcPath))
	{
		if (UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(Mpc))
		{
			Inst->SetScalarParameterValue(FName(TEXT("Season01")), Season01);
		}
	}

	// Structure / veg MIDs (Night + Season scalars).
	if (UNantucketStructuresSubsystem* Structs = World->GetSubsystem<UNantucketStructuresSubsystem>())
	{
		Structs->SetSeason(Season01);
	}
	// Terrain land cover MIDs.
	if (UNantucketTerrainSubsystem* Terrain = World->GetSubsystem<UNantucketTerrainSubsystem>())
	{
		Terrain->SetSeason(Season01);
	}
}

FString USailOceanSubsystem::GetTimeOfDayLabel() const
{
	const float H = FMath::Fmod(FMath::Max(0.f, TimeOfDayHours), 24.f);
	const int32 Hi = FMath::FloorToInt(H);
	const int32 Mi = FMath::Clamp(FMath::RoundToInt((H - Hi) * 60.f), 0, 59);
	const FSailEnvPresetDesc Desc = BuildTimeOfDayDesc();
	return FString::Printf(TEXT("%02d:%02d  %s"), Hi, Mi, Desc.Name);
}

float USailOceanSubsystem::GetNightAmount() const
{
	// 0 at midday, ramps through dusk, full night late evening / early morning.
	const float H = FMath::Fmod(FMath::Max(0.f, TimeOfDayHours), 24.f);
	if (H >= 8.f && H <= 16.5f) return 0.f;
	if (H > 16.5f && H < 21.5f)
	{
		return FMath::Clamp((H - 16.5f) / 5.f, 0.f, 1.f);
	}
	if (H >= 21.5f || H <= 5.0f) return 1.f;
	// 5 → 8 dawn
	return FMath::Clamp(1.f - (H - 5.f) / 3.f, 0.f, 1.f);
}

void USailOceanSubsystem::ApplySunAndSky(const FSailEnvPresetDesc& Desc)
{
	UWorld* World = GetWorld();
	if (!World || !EnvBaseline.bValid) return;

	// Fair Day restore: discrete Fair button OR time-of-day near noon.
	// Previously TOD noon forced the non-fair path and rebuilt sun pitch from the
	// desc table — that could darken the scene one frame after the map's bright
	// authored look (user saw "loads light then turns dark").
	const bool bFair = ((ActiveEnvPreset == ESailEnvPreset::FairDay) && !bTimeOfDayDriven)
		|| (bTimeOfDayDriven && GetNightAmount() < 0.08f
			&& FMath::Abs(TimeOfDayHours - 12.f) < 3.5f);
	const bool bNight = (ActiveEnvPreset == ESailEnvPreset::Night)
		|| (bTimeOfDayDriven && GetNightAmount() > 0.92f);
	ADirectionalLight* Moon = NightMoonLight.Get();

	// UE directional: light travels along +X. Pitch = -Elevation so noon elev +55
	// → pitch -55 (rays into the world). Positive pitch = sun below horizon = black.
	// Fair / noon TOD restores the captured rotator (already in UE pitch).
	FRotator SunRot = EnvBaseline.SunRotation;
	{
		const float Yaw = EnvBaseline.SunRotation.Yaw + Desc.SunYawOffsetDeg;
		if (bFair && (-EnvBaseline.SunRotation.Pitch) >= 20.f)
		{
			SunRot = FRotator(EnvBaseline.SunRotation.Pitch, Yaw, 0.f);
		}
		else
		{
			// Desc.SunElevationDeg is above-horizon degrees (may be negative at night).
			SunRot = FRotator(-Desc.SunElevationDeg, Yaw, 0.f);
		}
	}

	// Night: force absolute zero on the day sun — residual atm light washes stars.
	const float SunInt = bNight
		? 0.f
		: FMath::Max(0.f, EnvBaseline.SunIntensity * Desc.SunIntensityMul);
	const FLinearColor SunCol = bFair ? EnvBaseline.SunColor : Desc.SunColor;
	const float SrcAng = bFair
		? EnvBaseline.SunSourceAngle
		: FMath::Clamp(EnvBaseline.SunSourceAngle * Desc.SunSourceAngleMul, 0.1f, 12.f);
	const float SkyFloor = bNight ? 0.005f : 0.01f;
	const float SkyInt = FMath::Max(SkyFloor, EnvBaseline.SkyLightIntensity * Desc.SkyLightIntensityMul);
	const FLinearColor SkyCol = bFair ? EnvBaseline.SkyLightColor : Desc.SkyLightColor;

	// Skip sky-capture rebuild if sun/sky haven't meaningfully changed (avoids lighting flash).
	const bool bSameAsLast =
		LastAppliedSunInt >= 0.f
		&& FMath::IsNearlyEqual(SunInt, LastAppliedSunInt, 0.02f)
		&& FMath::IsNearlyEqual(SkyInt, LastAppliedSkyInt, 0.02f)
		&& SunRot.Equals(LastAppliedSunRot, 0.15f)
		&& SunCol.Equals(LastAppliedSunCol, 0.01f)
		&& SkyCol.Equals(LastAppliedSkyCol, 0.01f)
		&& bNight == bLastAppliedNight;

	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		ADirectionalLight* Sun = *It;
		if (!IsValid(Sun) || Sun == Moon) continue;
		// Skip any other runtime moon we tagged.
		if (Sun->GetActorNameOrLabel().Contains(TEXT("SailSim_Moon"))) continue;

		if (USceneComponent* Root = Sun->GetRootComponent())
		{
			if (Root->Mobility != EComponentMobility::Movable)
			{
				Root->SetMobility(EComponentMobility::Movable);
			}
		}
		Sun->SetActorRotation(SunRot);
		if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			if (C->Mobility != EComponentMobility::Movable)
			{
				C->SetMobility(EComponentMobility::Movable);
			}
			C->SetWorldRotation(SunRot);
			C->SetIntensity(SunInt);
			C->SetLightColor(SunCol);
			C->SetLightSourceAngle(SrcAng);
			if (bNight)
			{
				// Drop day sun out of the atmosphere entirely; moon is light index 1.
				C->SetAtmosphereSunDiskColorScale(FLinearColor::Black);
				C->SetAtmosphereSunLight(false);
				C->SetVisibility(false);
			}
			else
			{
				C->SetAtmosphereSunDiskColorScale(SunCol);
				C->SetAtmosphereSunLight(true);
				C->SetAtmosphereSunLightIndex(0);
				C->SetVisibility(true);
			}
			if (!bSameAsLast)
			{
				C->MarkRenderStateDirty();
			}
		}
	}

	for (TActorIterator<ASkyLight> It(World); It; ++It)
	{
		if (USkyLightComponent* C = It->GetLightComponent())
		{
			C->SetIntensity(SkyInt);
			C->SetLightColor(SkyCol);
			if (!bSameAsLast)
			{
				C->MarkRenderStateDirty();
			}
		}
	}
	if (!bSameAsLast)
	{
		RequestSkyLightRecapture(TEXT("sun-sky-change"));
		LastAppliedSunInt = SunInt;
		LastAppliedSkyInt = SkyInt;
		LastAppliedSunRot = SunRot;
		LastAppliedSunCol = SunCol;
		LastAppliedSkyCol = SkyCol;
		bLastAppliedNight = bNight;
	}
}

// ---------------------------------------------------------------------------
// Night sky: moon (atm light index 1) + star dome + distant moon disc
// ---------------------------------------------------------------------------

static UTexture2D* MakeProceduralStarTexture(int32 Size, int32 StarCount)
{
	UTexture2D* Tex = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8);
	if (!Tex || !Tex->GetPlatformData() || Tex->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}
	Tex->CompressionSettings = TC_Default;
	Tex->SRGB = true;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->Filter = TF_Bilinear;

	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memzero(Data, static_cast<SIZE_T>(Size) * Size * 4);
	uint8* Pixels = static_cast<uint8*>(Data);

	FRandomStream Rng(0x5A17);
	for (int32 I = 0; I < StarCount; ++I)
	{
		const int32 X = Rng.RandRange(1, Size - 2);
		const int32 Y = Rng.RandRange(1, Size - 2);
		const float Bright = Rng.FRandRange(0.35f, 1.f);
		const bool bBlue = Rng.FRand() < 0.25f;
		const uint8 R = static_cast<uint8>(255.f * Bright * (bBlue ? 0.85f : 1.f));
		const uint8 G = static_cast<uint8>(255.f * Bright * (bBlue ? 0.9f : 0.97f));
		const uint8 B = static_cast<uint8>(255.f * Bright);
		const uint8 A = static_cast<uint8>(255.f * Bright);
		const int32 Idx = (Y * Size + X) * 4;
		Pixels[Idx + 0] = B;
		Pixels[Idx + 1] = G;
		Pixels[Idx + 2] = R;
		Pixels[Idx + 3] = A;
		// Tiny cross for brighter stars so they read at distance
		if (Bright > 0.75f)
		{
			auto Plot = [&](int32 PX, int32 PY, float Scale)
			{
				if (PX < 0 || PY < 0 || PX >= Size || PY >= Size) return;
				const int32 J = (PY * Size + PX) * 4;
				const uint8 V = static_cast<uint8>(255.f * Bright * Scale);
				Pixels[J + 0] = FMath::Max(Pixels[J + 0], static_cast<uint8>(V * (bBlue ? 0.9f : 1.f)));
				Pixels[J + 1] = FMath::Max(Pixels[J + 1], V);
				Pixels[J + 2] = FMath::Max(Pixels[J + 2], V);
				Pixels[J + 3] = FMath::Max(Pixels[J + 3], V);
			};
			Plot(X + 1, Y, 0.55f);
			Plot(X - 1, Y, 0.55f);
			Plot(X, Y + 1, 0.55f);
			Plot(X, Y - 1, 0.55f);
		}
	}
	Mip.BulkData.Unlock();
	Tex->UpdateResource();
	return Tex;
}

/** Solid unlit/emissive MID — no texture dependency (stars + moon disc). */
static UMaterialInstanceDynamic* MakeSolidEmissiveMid(FLinearColor Color, float EmissiveBoost)
{
	const TCHAR* Paths[] = {
		TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"),
		TEXT("/Game/Materials/Yacht/M_Yacht_PBR.M_Yacht_PBR"),
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
		TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
	};
	UMaterialInterface* Base = nullptr;
	for (const TCHAR* P : Paths)
	{
		Base = LoadObject<UMaterialInterface>(nullptr, P);
		if (Base) break;
	}
	if (!Base) return nullptr;
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return nullptr;
	const FLinearColor Em = Color * EmissiveBoost;
	Mid->SetVectorParameterValue(TEXT("Color"), Color);
	Mid->SetVectorParameterValue(TEXT("BaseColor"), Color);
	Mid->SetVectorParameterValue(TEXT("EmissiveColor"), Em);
	Mid->SetVectorParameterValue(TEXT("Emissive"), Em);
	Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), EmissiveBoost);
	Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), EmissiveBoost);
	Mid->SetScalarParameterValue(TEXT("Emissive"), EmissiveBoost);
	Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 1.f);
	Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	return Mid;
}

// Shell radius for stars/moon disc (cm). Close enough for precision & culling,
// far enough to read as sky (no parallax while sailing locally).
static constexpr float GNightSkyShellCm = 180000.f; // 1.8 km

void USailOceanSubsystem::EnsureNightSkyActors()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Drop legacy textured-dome actors from earlier builds (wrong mats / 80 km clip).
	auto NeedsRebuild = [](AActor* A) -> bool
	{
		if (!IsValid(A)) return true;
		TInlineComponentArray<UInstancedStaticMeshComponent*> ISMs(A);
		return ISMs.Num() == 0;
	};
	if (NightStarDome.IsValid() && NeedsRebuild(NightStarDome.Get()))
	{
		NightStarDome->Destroy();
		NightStarDome.Reset();
		NightStarMid.Reset();
	}
	if (NightMoonDisc.IsValid())
	{
		// Moon disc is a plain SMC — rebuild if tagged old (huge scale)
		if (UStaticMeshComponent* SMC = NightMoonDisc->FindComponentByClass<UStaticMeshComponent>())
		{
			if (SMC->GetComponentScale().GetAbsMax() > 100.f)
			{
				NightMoonDisc->Destroy();
				NightMoonDisc.Reset();
				NightMoonDiscMid.Reset();
			}
		}
	}

	// --- Moon directional light (atmosphere light index 1) ---
	if (!NightMoonLight.IsValid())
	{
		FActorSpawnParameters SP;
		SP.Name = MakeUniqueObjectName(World->GetCurrentLevel(), ADirectionalLight::StaticClass(), TEXT("SailSim_Moon"));
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ADirectionalLight* Moon = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(), FVector::ZeroVector, FRotator(-42.f, 200.f, 0.f), SP);
		if (Moon)
		{
#if WITH_EDITOR
			Moon->SetActorLabel(TEXT("SailSim_Moon"));
#endif
			if (USceneComponent* Root = Moon->GetRootComponent())
			{
				Root->SetMobility(EComponentMobility::Movable);
			}
			if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Moon->GetLightComponent()))
			{
				C->SetMobility(EComponentMobility::Movable);
				C->SetAtmosphereSunLight(true);
				C->SetAtmosphereSunLightIndex(1);
				C->SetIntensity(0.f);
				C->SetLightColor(FLinearColor(0.72f, 0.8f, 1.f));
				C->SetLightSourceAngle(2.5f);
				C->SetAtmosphereSunDiskColorScale(FLinearColor(1.2f, 1.3f, 1.6f));
				C->SetCastShadows(true);
				C->SetVisibility(false);
				C->MarkRenderStateDirty();
			}
			NightMoonLight = Moon;
		}
	}

	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!SphereMesh)
	{
		UE_LOG(LogSailSim, Warning, TEXT("Night sky: BasicShapes/Sphere missing"));
		return;
	}

	// --- Stars: instanced emissive spheres on a shell (always visible, no texture) ---
	if (!NightStarDome.IsValid())
	{
		FActorSpawnParameters SP;
		SP.Name = MakeUniqueObjectName(World->GetCurrentLevel(), AActor::StaticClass(), TEXT("SailSim_StarDome"));
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Dome = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SP);
		if (Dome)
		{
#if WITH_EDITOR
			Dome->SetActorLabel(TEXT("SailSim_StarDome"));
#endif
			USceneComponent* Root = NewObject<USceneComponent>(Dome, TEXT("Root"));
			Root->SetMobility(EComponentMobility::Movable);
			Dome->SetRootComponent(Root);
			Root->RegisterComponent();

			UInstancedStaticMeshComponent* ISM = NewObject<UInstancedStaticMeshComponent>(Dome, TEXT("StarISM"));
			ISM->SetMobility(EComponentMobility::Movable);
			ISM->SetupAttachment(Root);
			ISM->SetStaticMesh(SphereMesh);
			ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			ISM->SetCastShadow(false);
			ISM->SetReceivesDecals(false);
			ISM->SetVisibility(false);
			ISM->bAffectDynamicIndirectLighting = false;
			ISM->SetCullDistances(0, 0); // never distance-cull
			ISM->SetBoundsScale(50.f);
			ISM->RegisterComponent();

			UMaterialInstanceDynamic* Mid = MakeSolidEmissiveMid(
				FLinearColor(0.92f, 0.95f, 1.f), 40.f);
			if (Mid)
			{
				ISM->SetMaterial(0, Mid);
				NightStarMid = Mid;
			}

			// Fibonacci sphere — even coverage; bias slightly above the horizon.
			constexpr int32 NumStars = 1600;
			const float Golden = UE_PI * (3.f - FMath::Sqrt(5.f));
			FRandomStream Rng(0xC01D);
			for (int32 I = 0; I < NumStars; ++I)
			{
				const float Y = 1.f - (static_cast<float>(I) / static_cast<float>(NumStars - 1)) * 2.f; // -1..1
				// Soft floor so we still get stars near horizon but fewer below.
				if (Y < -0.2f && Rng.FRand() < 0.65f) continue;
				const float Rad = FMath::Sqrt(FMath::Max(0.f, 1.f - Y * Y));
				const float Theta = Golden * static_cast<float>(I);
				FVector Dir(FMath::Cos(Theta) * Rad, FMath::Sin(Theta) * Rad, Y);
				Dir.Normalize();

				// Angular size ~0.03–0.12° at shell distance → diameter few meters.
				const float Mag = Rng.FRand(); // brightness class
				const float DiamCm = FMath::Lerp(80.f, 420.f, Mag * Mag); // brighter = larger
				const float Scale = DiamCm / 100.f; // BasicShapes sphere diameter 100 cm
				const FVector Loc = Dir * GNightSkyShellCm;
				ISM->AddInstance(FTransform(FRotator::ZeroRotator, Loc, FVector(Scale)), false);
			}
			ISM->MarkRenderStateDirty();

			NightStarDome = Dome;
			UE_LOG(LogSailSim, Log, TEXT("Night sky: spawned %d star instances @ %.0fm"),
				ISM->GetInstanceCount(), GNightSkyShellCm * 0.01f);
		}
	}

	// --- Moon disc: bright emissive sphere on the same shell ---
	if (!NightMoonDisc.IsValid())
	{
		FActorSpawnParameters SP;
		SP.Name = MakeUniqueObjectName(World->GetCurrentLevel(), AActor::StaticClass(), TEXT("SailSim_MoonDisc"));
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Disc = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SP);
		if (Disc)
		{
#if WITH_EDITOR
			Disc->SetActorLabel(TEXT("SailSim_MoonDisc"));
#endif
			USceneComponent* Root = NewObject<USceneComponent>(Disc, TEXT("Root"));
			Root->SetMobility(EComponentMobility::Movable);
			Disc->SetRootComponent(Root);
			Root->RegisterComponent();

			UStaticMeshComponent* SMC = NewObject<UStaticMeshComponent>(Disc, TEXT("Disc"));
			SMC->SetMobility(EComponentMobility::Movable);
			SMC->SetupAttachment(Root);
			SMC->SetStaticMesh(SphereMesh);
			SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SMC->SetCastShadow(false);
			SMC->SetReceivesDecals(false);
			SMC->SetVisibility(false);
			SMC->bAffectDynamicIndirectLighting = false;
			SMC->SetCullDistance(0.f);
			// ~0.8° angular size at 1.8 km → diameter ≈ 25 m
			const float DiamCm = 2.f * GNightSkyShellCm * FMath::Tan(FMath::DegreesToRadians(0.4f));
			SMC->SetWorldScale3D(FVector(DiamCm / 100.f));
			SMC->SetBoundsScale(20.f);
			SMC->RegisterComponent();

			UMaterialInstanceDynamic* Mid = MakeSolidEmissiveMid(
				FLinearColor(0.95f, 0.96f, 1.f), 80.f);
			if (Mid)
			{
				SMC->SetMaterial(0, Mid);
				NightMoonDiscMid = Mid;
			}
			NightMoonDisc = Disc;
			UE_LOG(LogSailSim, Log, TEXT("Night sky: moon disc diam=%.0fm"), DiamCm * 0.01f);
		}
	}
}

void USailOceanSubsystem::DestroyNightSkyActors()
{
	auto Kill = [](TWeakObjectPtr<AActor>& W)
	{
		if (AActor* A = W.Get())
		{
			A->Destroy();
		}
		W.Reset();
	};
	if (ADirectionalLight* M = NightMoonLight.Get())
	{
		M->Destroy();
	}
	NightMoonLight.Reset();
	Kill(NightStarDome);
	Kill(NightMoonDisc);
	NightStarMid.Reset();
	NightMoonDiscMid.Reset();
}

void USailOceanSubsystem::UpdateNightSkyFollow(const FVector& BoatWorldPos)
{
	const FVector Center(BoatWorldPos.X, BoatWorldPos.Y, BoatWorldPos.Z);

	if (AActor* Dome = NightStarDome.Get())
	{
		Dome->SetActorLocation(Center);
	}

	// Moon disc on the same shell as stars, along atmosphere light +X (moon sky dir).
	if (AActor* Disc = NightMoonDisc.Get())
	{
		FVector MoonDir = FVector(0.6f, 0.2f, 0.75f).GetSafeNormal();
		if (ADirectionalLight* Moon = NightMoonLight.Get())
		{
			if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Moon->GetLightComponent()))
			{
				MoonDir = C->GetComponentTransform().GetUnitAxis(EAxis::X).GetSafeNormal();
			}
		}
		// Keep slightly inside the star shell so it isn't z-fought with stars.
		Disc->SetActorLocation(Center + MoonDir * (GNightSkyShellCm * 0.98f));
	}
}

void USailOceanSubsystem::ApplyNightSky(bool bNight)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// No-op when already in this night state (avoid per-call sky recapture flash).
	const bool bAlready = (bNightSkyActive == bNight)
		&& (!bNight || (NightMoonLight.IsValid() && NightStarDome.IsValid()));
	if (bAlready)
	{
		return;
	}

	if (bNight)
	{
		EnsureNightSkyActors();
	}

	bNightSkyActive = bNight;

	// Moon light — dim scene fill + bright atmosphere disk (mesh disc is the main moon).
	if (ADirectionalLight* Moon = NightMoonLight.Get())
	{
		const float MoonElev = 42.f; // above horizon
		const float MoonYaw = EnvBaseline.SunRotation.Yaw + 160.f;
		// UE pitch = -elevation (same convention as day sun).
		const FRotator MoonRot(-MoonElev, MoonYaw, 0.f);
		Moon->SetActorRotation(MoonRot);
		if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(Moon->GetLightComponent()))
		{
			C->SetWorldRotation(MoonRot);
			C->SetAtmosphereSunLight(bNight);
			C->SetAtmosphereSunLightIndex(1);
			if (bNight)
			{
				C->SetIntensity(0.1f);
				C->SetLightColor(FLinearColor(0.65f, 0.72f, 0.95f));
				C->SetLightSourceAngle(2.8f);
				// Hot disk so atmosphere can show a moon even without the mesh.
				C->SetAtmosphereSunDiskColorScale(FLinearColor(2.5f, 2.6f, 3.f));
				C->SetVisibility(true);
				C->SetCastShadows(true);
			}
			else
			{
				C->SetIntensity(0.f);
				C->SetAtmosphereSunLight(false);
				C->SetVisibility(false);
			}
			C->MarkRenderStateDirty();
		}
	}

	// Stars (ISM emissive spheres)
	if (AActor* Dome = NightStarDome.Get())
	{
		Dome->SetActorHiddenInGame(!bNight);
		TInlineComponentArray<UPrimitiveComponent*> Prim(Dome);
		for (UPrimitiveComponent* P : Prim)
		{
			if (!P) continue;
			P->SetVisibility(bNight, true);
			P->SetHiddenInGame(!bNight, true);
		}
		if (UMaterialInstanceDynamic* Mid = NightStarMid.Get())
		{
			const float Boost = bNight ? 40.f : 0.f;
			const FLinearColor Col(0.95f, 0.97f, 1.f);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Boost);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Boost);
			Mid->SetScalarParameterValue(TEXT("Emissive"), Boost);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), Col * Boost);
			Mid->SetVectorParameterValue(TEXT("Color"), Col);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), Col);
		}
	}

	// Moon disc mesh
	if (AActor* Disc = NightMoonDisc.Get())
	{
		Disc->SetActorHiddenInGame(!bNight);
		TInlineComponentArray<UPrimitiveComponent*> Prim(Disc);
		for (UPrimitiveComponent* P : Prim)
		{
			if (!P) continue;
			P->SetVisibility(bNight, true);
			P->SetHiddenInGame(!bNight, true);
		}
		if (UMaterialInstanceDynamic* Mid = NightMoonDiscMid.Get())
		{
			const float Boost = bNight ? 80.f : 0.f;
			const FLinearColor Col(0.96f, 0.97f, 1.f);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Boost);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Boost);
			Mid->SetScalarParameterValue(TEXT("Emissive"), Boost);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), Col * Boost);
			Mid->SetVectorParameterValue(TEXT("Color"), Col);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), Col);
		}
	}

	if (bNight)
	{
		FVector Focus = LastZoneBoatXY;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* P = PC->GetPawn())
			{
				Focus = P->GetActorLocation();
			}
		}
		UpdateNightSkyFollow(Focus);
	}

	// Realtime skylight follows moon/stars without a forced bake (avoids day flash).
	if (bNight)
	{
		RequestSkyLightRecapture(TEXT("night-sky-on"));
	}

	UE_LOG(LogSailSim, Log, TEXT("Night sky %s (moon=%s stars=%s disc=%s)"),
		bNight ? TEXT("ON") : TEXT("OFF"),
		NightMoonLight.IsValid() ? TEXT("ok") : TEXT("missing"),
		NightStarDome.IsValid() ? TEXT("ok") : TEXT("missing"),
		NightMoonDisc.IsValid() ? TEXT("ok") : TEXT("missing"));
}

void USailOceanSubsystem::ApplyAtmosphereLook(const FSailEnvPresetDesc& Desc)
{
	UWorld* World = GetWorld();
	if (!World || !EnvBaseline.bValid) return;

	const bool bFair = (ActiveEnvPreset == ESailEnvPreset::FairDay) && !bTimeOfDayDriven;
	const bool bNight = (ActiveEnvPreset == ESailEnvPreset::Night)
		|| (bTimeOfDayDriven && GetNightAmount() > 0.92f);
	// Near-noon time-of-day still restores exact Fair atmosphere.
	const bool bFairLook = bFair
		|| (bTimeOfDayDriven && GetNightAmount() < 0.05f
			&& FMath::Abs(TimeOfDayHours - 12.f) < 4.5f);

	for (TObjectIterator<USkyAtmosphereComponent> It; It; ++It)
	{
		USkyAtmosphereComponent* Atm = *It;
		if (!IsValid(Atm) || Atm->GetWorld() != World) continue;

		if (!EnvBaseline.bAtmCaptured)
		{
			EnvBaseline.AtmMulti = Atm->MultiScatteringFactor;
			EnvBaseline.AtmRayleigh = Atm->RayleighScatteringScale;
			EnvBaseline.AtmMie = Atm->MieScatteringScale;
			EnvBaseline.AtmSkyLuminance = Atm->SkyLuminanceFactor;
			EnvBaseline.AtmHeightFogContribution = Atm->HeightFogContribution;
			EnvBaseline.bAtmCaptured = true;
		}

		if (bFairLook)
		{
			Atm->SetMultiScatteringFactor(EnvBaseline.AtmMulti);
			Atm->SetRayleighScatteringScale(EnvBaseline.AtmRayleigh);
			Atm->SetMieScatteringScale(EnvBaseline.AtmMie);
			Atm->SetSkyLuminanceFactor(EnvBaseline.AtmSkyLuminance);
			Atm->SetHeightFogContribution(EnvBaseline.AtmHeightFogContribution);
			Atm->SetSkyAndAerialPerspectiveLuminanceFactor(FLinearColor::White);
		}
		else
		{
			Atm->SetMultiScatteringFactor(EnvBaseline.AtmMulti * Desc.AtmosphereMultiScatter);
			Atm->SetRayleighScatteringScale(EnvBaseline.AtmRayleigh * Desc.AtmosphereRayleighMul);
			Atm->SetMieScatteringScale(EnvBaseline.AtmMie * Desc.AtmosphereMieMul);
			Atm->SetSkyLuminanceFactor(Desc.SkyLuminance);
			if (Desc.HeightFogContribution >= 0.f)
			{
				Atm->SetHeightFogContribution(Desc.HeightFogContribution);
			}
			else
			{
				Atm->SetHeightFogContribution(EnvBaseline.AtmHeightFogContribution);
			}
			// Night: crush residual sky glow so stars read; other presets keep white.
			Atm->SetSkyAndAerialPerspectiveLuminanceFactor(
				bNight ? FLinearColor(0.04f, 0.05f, 0.09f) : FLinearColor::White);
		}
		Atm->MarkRenderStateDirty();
	}

	// Camera exposure via PPS. IMPORTANT: do NOT clamp Min/Max brightness on day
	// after first frames — that was the light→dark pop (default AE is bright, then
	// we overwrote with a tight EV range and the image sank). AAA outdoor: slow
	// adaptation + small bias only; night keeps a tighter range for stars.
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		APawn* Pawn = *It;
		if (!IsValid(Pawn) || !Pawn->IsPlayerControlled()) continue;
		TArray<UCameraComponent*> Cams;
		Pawn->GetComponents<UCameraComponent>(Cams);
		for (UCameraComponent* Cam : Cams)
		{
			if (!Cam) continue;
			FPostProcessSettings& PPS = Cam->PostProcessSettings;
			Cam->PostProcessBlendWeight = 1.f;
			PPS.bOverride_AutoExposureSpeedUp = true;
			PPS.AutoExposureSpeedUp = 2.0f;
			PPS.bOverride_AutoExposureSpeedDown = true;
			PPS.AutoExposureSpeedDown = 1.0f;
			PPS.bOverride_AutoExposureBias = true;
			PPS.AutoExposureBias = bNight ? -1.2f : 0.35f; // slight day lift for land
			if (bNight)
			{
				PPS.bOverride_AutoExposureMaxBrightness = true;
				PPS.bOverride_AutoExposureMinBrightness = true;
				PPS.AutoExposureMaxBrightness = 0.8f;
				PPS.AutoExposureMinBrightness = 0.04f;
			}
			else
			{
				// Day: leave engine default EV histogram range alone (no min/max override).
				PPS.bOverride_AutoExposureMaxBrightness = false;
				PPS.bOverride_AutoExposureMinBrightness = false;
			}
		}
	}
}

void USailOceanSubsystem::ApplyFogLook(const FSailEnvPresetDesc& Desc)
{
	UWorld* World = GetWorld();
	if (!World || !EnvBaseline.bValid) return;

	const bool bFairLook = ((ActiveEnvPreset == ESailEnvPreset::FairDay) && !bTimeOfDayDriven)
		|| (bTimeOfDayDriven && GetNightAmount() < 0.08f && FMath::Abs(TimeOfDayHours - 12.f) < 5.f);
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		AExponentialHeightFog* FogActor = *It;
		if (!IsValid(FogActor)) continue;
		UExponentialHeightFogComponent* Fog = FogActor->GetComponent();
		if (!Fog) continue;

		// Prefer baseline fog color always for Fair/golden; only night tints slightly.
		// Avoid purple/orange fog washes that fight the polished Fair Day look.
		if (bFairLook || !Desc.bOverrideFogColor)
		{
			const float NightW = bTimeOfDayDriven ? FMath::Clamp((GetNightAmount() - 0.55f) / 0.45f, 0.f, 1.f) : 0.f;
			if (NightW > KINDA_SMALL_NUMBER)
			{
				Fog->SetFogInscatteringColor(FMath::Lerp(EnvBaseline.FogInscattering, Desc.FogInscattering, NightW * 0.35f));
			}
			else
			{
				Fog->SetFogInscatteringColor(EnvBaseline.FogInscattering);
			}
		}
		else
		{
			Fog->SetFogInscatteringColor(FMath::Lerp(EnvBaseline.FogInscattering, Desc.FogInscattering, 0.4f));
		}

		// Always restore / set falloff so switching Fog→Fair is clean.
		const float Falloff = bFairLook
			? EnvBaseline.FogHeightFalloff
			: (Desc.FogHeightFalloff >= 0.f ? Desc.FogHeightFalloff : EnvBaseline.FogHeightFalloff);
		Fog->SetFogHeightFalloff(Falloff);

		const float StartDist = bFairLook
			? EnvBaseline.FogStartDistance
			: (Desc.FogStartDistanceCm >= 0.f ? Desc.FogStartDistanceCm : EnvBaseline.FogStartDistance);
		Fog->SetStartDistance(StartDist);

		Fog->SetFogMaxOpacity(EnvBaseline.FogMaxOpacity);

		// Density scale for ApplyFogIntensity — keep ~baseline at day; mild dusk boost only.
		const float DensMul = bFairLook ? 1.f : FMath::Lerp(1.f, FMath::Max(0.05f, Desc.FogDensityMul),
			bTimeOfDayDriven ? GetNightAmount() : 1.f);
		CachedFogDensity = EnvBaseline.FogDensity * DensMul;
		CachedSecondFogDensity = EnvBaseline.SecondFogDensity * DensMul;
	}
}

void USailOceanSubsystem::ApplyCloudLayerLook(const FSailEnvPresetDesc& Desc)
{
	UWorld* World = GetWorld();
	if (!World || !EnvBaseline.bValid) return;

	const bool bFairLook = ((ActiveEnvPreset == ESailEnvPreset::FairDay) && !bTimeOfDayDriven)
		|| (bTimeOfDayDriven && GetNightAmount() < 0.15f);
	for (TObjectIterator<UVolumetricCloudComponent> It; It; ++It)
	{
		UVolumetricCloudComponent* Cloud = *It;
		if (!IsValid(Cloud) || Cloud->GetWorld() != World) continue;

		if (CachedCloudLayerHeightKm < 0.f)
		{
			CachedCloudLayerHeightKm = FMath::Max(Cloud->LayerHeight, 0.1f);
		}

		if (bFairLook)
		{
			Cloud->SetLayerBottomAltitude(EnvBaseline.CloudLayerBottomKm);
		}
		else
		{
			const float Bottom = FMath::Max(0.f, EnvBaseline.CloudLayerBottomKm + Desc.CloudBottomOffsetKm);
			Cloud->SetLayerBottomAltitude(Bottom);
		}
		Cloud->MarkRenderStateDirty();
	}
}

float USailOceanSubsystem::ApplyEnvPreset(uint8 PresetId)
{
	const ESailEnvPreset Id = static_cast<ESailEnvPreset>(
		FMath::Clamp(static_cast<int32>(PresetId), 0, static_cast<int32>(ESailEnvPreset::Count) - 1));
	CaptureEnvBaselineIfNeeded();
	if (!EnvBaseline.bValid)
	{
		UE_LOG(LogSailSim, Warning, TEXT("ApplyEnvPreset: no baseline / no world"));
		return -1.f;
	}

	// Diurnal cycle presets → drive the time-of-day path (keeps fog/cloud sliders).
	// Mood presets (Overcast/Storm/Fog) stay discrete.
	if (Id == ESailEnvPreset::FairDay || Id == ESailEnvPreset::GoldenHour
		|| Id == ESailEnvPreset::Dusk || Id == ESailEnvPreset::Night)
	{
		switch (Id)
		{
		case ESailEnvPreset::Night: TimeOfDayHours = 0.f; break;
		case ESailEnvPreset::Dusk: TimeOfDayHours = 19.4f; break;
		case ESailEnvPreset::GoldenHour: TimeOfDayHours = 17.8f; break;
		default: TimeOfDayHours = 12.f; break;
		}
		ApplyTimeOfDayInternal();
		return GetSailEnvPresetDesc(Id).SuggestedWindKn;
	}

	bTimeOfDayDriven = false;
	ActiveEnvPreset = Id;
	const FSailEnvPresetDesc& Desc = GetSailEnvPresetDesc(Id);

	// Order: fog density/color → cloud layer → intensity sliders → sun + atmosphere.
	ApplyFogLook(Desc);
	ApplyCloudLayerLook(Desc);
	SetVolumetricCloudIntensity(Desc.CloudIntensity);
	SetFogIntensity(Desc.FogIntensity);
	SetSurfaceChopIntensity(Desc.SurfaceChop);

	ApplySunAndSky(Desc);
	ApplyAtmosphereLook(Desc);
	ApplyNightSky(false);

	UE_LOG(LogSailSim, Log,
		TEXT("Env preset → %s elev=%.1f sunMul=%.2f skyMul=%.2f clouds=%.2f fog=%.2f densMul=%.2f chop=%.2f wind=%.1f"),
		Desc.Name, Desc.SunElevationDeg, Desc.SunIntensityMul, Desc.SkyLightIntensityMul,
		Desc.CloudIntensity, Desc.FogIntensity, Desc.FogDensityMul,
		Desc.SurfaceChop, Desc.SuggestedWindKn);
	return Desc.SuggestedWindKn;
}

FString USailOceanSubsystem::GetEnvPresetName(uint8 PresetId) const
{
	const ESailEnvPreset Id = static_cast<ESailEnvPreset>(
		FMath::Clamp(static_cast<int32>(PresetId), 0, static_cast<int32>(ESailEnvPreset::Count) - 1));
	return FString(GetSailEnvPresetDesc(Id).Name);
}
