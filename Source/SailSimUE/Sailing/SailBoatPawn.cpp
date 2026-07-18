#include "Sailing/SailBoatPawn.h"
#include "Sailing/BoatMeshFromJson.h"
#include "Sailing/BoatPresets.h"
#include "Sailing/BoatLoftOracle.h"
#include "Sailing/OceanHeightSample.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Ocean/BoatWakeSim.h"
#include "Sailing/Terrain/NantucketTerrainSubsystem.h"
#include "Sailing/Terrain/NantucketStructuresSubsystem.h"
#include "Sailing/SailSimPerf.h"
#include "Sailing/UI/SailSimUserPrefs.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Nav/NavWaypointSubsystem.h"
#include "Sailing/Nav/EncAidSubsystem.h"
#include "Sailing/Nav/MooredBoatSubsystem.h"
#include "Sailing/Wind/WindFieldSubsystem.h"
#include "Sailing/Wind/WindCompassRing.h"
#include "Engine/GameInstance.h"
#include "SailSimUE.h"
#include "EngineUtils.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/InputComponent.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "Misc/Paths.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	// Hull box is large; never abort spawn on world overlap (was: empty PIE = sky only).
	SpawnCollisionHandlingMethod = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	BoatRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BoatRoot"));
	RootComponent = BoatRoot;

	auto PrepLoft = [](UProceduralMeshComponent* Mesh)
	{
		if (!Mesh) return;
		Mesh->bUseAsyncCooking = false;
		Mesh->bUseComplexAsSimpleCollision = false;
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCastShadow(true);
		Mesh->bNeverDistanceCull = true;
		Mesh->SetReceivesDecals(false);
	};

	LoftMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LoftMesh"));
	LoftMesh->SetupAttachment(BoatRoot);
	PrepLoft(LoftMesh);

	// Keel/rudder: visible underwater, never cast shadows onto the topsides.
	AppendagesMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("AppendagesMesh"));
	AppendagesMesh->SetupAttachment(BoatRoot);
	PrepLoft(AppendagesMesh);
	AppendagesMesh->SetCastShadow(false);
	AppendagesMesh->bCastContactShadow = false;
	AppendagesMesh->bCastDynamicShadow = false;
	AppendagesMesh->bCastStaticShadow = false;
	AppendagesMesh->bCastVolumetricTranslucentShadow = false;
	AppendagesMesh->bCastInsetShadow = false;
	AppendagesMesh->bSelfShadowOnly = false;

	// Sails: deforming PMC every frame — casting shadows thrashes VSM pages.
	// Hull/spars still cast; sail self-shadow is not worth the RT cost at sea.
	auto PrepSail = [&PrepLoft](UProceduralMeshComponent* Mesh)
	{
		PrepLoft(Mesh);
		if (!Mesh) return;
		Mesh->SetCastShadow(false);
		Mesh->bCastDynamicShadow = false;
		Mesh->bCastStaticShadow = false;
		Mesh->bCastContactShadow = false;
		Mesh->bCastVolumetricTranslucentShadow = false;
		Mesh->bCastInsetShadow = false;
	};

	MainSailMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MainSailMesh"));
	MainSailMesh->SetupAttachment(BoatRoot);
	PrepSail(MainSailMesh);

	JibSailMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("JibSailMesh"));
	JibSailMesh->SetupAttachment(BoatRoot);
	PrepSail(JibSailMesh);

	SpinSailMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("SpinSailMesh"));
	SpinSailMesh->SetupAttachment(BoatRoot);
	PrepSail(SpinSailMesh);
	SpinSailMesh->SetVisibility(false);
	SpinSailMesh->SetHiddenInGame(true);

	BowspritMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Bowsprit"));
	BowspritMesh->SetupAttachment(BoatRoot);
	ForestayFoilMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ForestayFoil"));
	ForestayFoilMesh->SetupAttachment(BoatRoot);
	FurlerDrumMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FurlerDrum"));
	FurlerDrumMesh->SetupAttachment(BoatRoot);
	SpinSheetMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SpinSheet"));
	SpinSheetMesh->SetupAttachment(BoatRoot);
	SpinSheetToWinchMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SpinSheetToWinch"));
	SpinSheetToWinchMesh->SetupAttachment(BoatRoot);
	SpinLazySheetSegA = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SpinLazySheetSegA"));
	SpinLazySheetSegA->SetupAttachment(BoatRoot);
	SpinLazySheetSegB = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SpinLazySheetSegB"));
	SpinLazySheetSegB->SetupAttachment(BoatRoot);

	HullCollision = CreateDefaultSubobject<UBoxComponent>(TEXT("HullCollision"));
	HullCollision->SetupAttachment(BoatRoot);
	// Query-only: kinematic float does not need physics blocking, and BlockAll
	// made SpawnDefaultPawnAtTransform fail ("collision at the spawn location").
	HullCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	HullCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
	HullCollision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Overlap);
	HullCollision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
	HullCollision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	HullCollision->SetGenerateOverlapEvents(false);
	HullCollision->SetBoxExtent(FVector(500.f, 160.f, 120.f));
	HullCollision->SetRelativeLocation(FVector(0.f, 0.f, -40.f));
	HullCollision->SetCanEverAffectNavigation(false);
	HullCollision->SetHiddenInGame(true);

	MastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mast"));
	MastMesh->SetupAttachment(BoatRoot);
	BoomMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Boom"));
	BoomMesh->SetupAttachment(BoatRoot);

	JibTrackPortMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibTrackPort"));
	JibTrackStbdMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibTrackStbd"));
	JibCarPortMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibCarPort"));
	JibCarStbdMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibCarStbd"));
	TravelerTrackMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TravelerTrack"));
	TravelerCarMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TravelerCar"));
	MidboomBlockMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MidboomBlock"));
	MainsheetRopeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MainsheetRope"));
	VangMastBlockMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VangMastBlock"));
	VangBoomBlockMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VangBoomBlock"));
	VangRopeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VangRope"));
	JibSheetPortMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibSheetPort"));
	JibSheetStbdMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibSheetStbd"));
	JibSheetPortToWinchMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibSheetPortToWinch"));
	JibSheetStbdToWinchMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibSheetStbdToWinch"));
	JibLazySheetSegA = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibLazySheetSegA"));
	JibLazySheetSegB = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibLazySheetSegB"));
	StandingRigMeshes.SetNum(StandingRigCount);
	for (int32 I = 0; I < StandingRigCount; ++I)
	{
		StandingRigMeshes[I] = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("StandingRig_%d"), I));
	}
	for (UStaticMeshComponent* Comp : {
			JibTrackPortMesh, JibTrackStbdMesh, JibCarPortMesh, JibCarStbdMesh,
			TravelerTrackMesh, TravelerCarMesh, MidboomBlockMesh, MainsheetRopeMesh,
			VangMastBlockMesh, VangBoomBlockMesh, VangRopeMesh,
			JibSheetPortMesh, JibSheetStbdMesh, JibSheetPortToWinchMesh, JibSheetStbdToWinchMesh,
			JibLazySheetSegA, JibLazySheetSegB })
	{
		if (Comp) Comp->SetupAttachment(BoatRoot);
	}
	for (UStaticMeshComponent* Comp : StandingRigMeshes)
	{
		if (Comp) Comp->SetupAttachment(BoatRoot);
	}

	// COLREGS fixtures: housing + lens; spots for sectors, points for all-round.
	LightHousingMeshes.SetNum(BoatLightCount);
	LightLensMeshes.SetNum(BoatLightCount);
	// Spots: Port, Stbd, Stern, Steaming, DeckP, DeckS (6) — Anchor/Cabin use points
	LightSpots.SetNum(6);
	static const TCHAR* LightNames[] = {
		TEXT("LightPort"), TEXT("LightStbd"), TEXT("LightStern"), TEXT("LightSteaming"),
		TEXT("LightAnchor"), TEXT("LightDeckP"), TEXT("LightDeckS"), TEXT("LightCabin")
	};
	for (int32 I = 0; I < BoatLightCount; ++I)
	{
		LightHousingMeshes[I] = CreateDefaultSubobject<UStaticMeshComponent>(
			*FString::Printf(TEXT("%s_Housing"), LightNames[I]));
		LightHousingMeshes[I]->SetupAttachment(BoatRoot);
		LightLensMeshes[I] = CreateDefaultSubobject<UStaticMeshComponent>(LightNames[I]);
		LightLensMeshes[I]->SetupAttachment(BoatRoot);
	}
	static const TCHAR* SpotNames[] = {
		TEXT("SpotPort"), TEXT("SpotStbd"), TEXT("SpotStern"),
		TEXT("SpotSteaming"), TEXT("SpotDeckP"), TEXT("SpotDeckS")
	};
	for (int32 I = 0; I < 6; ++I)
	{
		LightSpots[I] = CreateDefaultSubobject<USpotLightComponent>(SpotNames[I]);
		LightSpots[I]->SetupAttachment(BoatRoot);
		LightSpots[I]->SetVisibility(false);
		LightSpots[I]->SetIntensity(0.f);
		LightSpots[I]->SetCastShadows(false);
		LightSpots[I]->SetMobility(EComponentMobility::Movable);
	}
	auto MakeGlowPoint = [this](const FName& Name) -> UPointLightComponent*
	{
		UPointLightComponent* P = CreateDefaultSubobject<UPointLightComponent>(Name);
		P->SetupAttachment(BoatRoot);
		P->SetVisibility(false);
		P->SetIntensity(0.f);
		P->SetCastShadows(false);
		P->SetMobility(EComponentMobility::Movable);
		return P;
	};
	LightPortGlow = MakeGlowPoint(TEXT("PointPortGlow"));
	LightStbdGlow = MakeGlowPoint(TEXT("PointStbdGlow"));
	LightSternGlow = MakeGlowPoint(TEXT("PointSternGlow"));
	LightAnchorPoint = MakeGlowPoint(TEXT("PointAnchor"));
	LightCabinPoint = MakeGlowPoint(TEXT("PointCabin"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	UStaticMesh* Cyl = CylMesh.Succeeded() ? CylMesh.Object : nullptr;
	UStaticMesh* Cube = CubeMesh.Succeeded() ? CubeMesh.Object : nullptr;
	UStaticMesh* Sphere = SphereMesh.Succeeded() ? SphereMesh.Object : nullptr;
	UMaterialInterface* BaseMat = BasicMat.Succeeded() ? BasicMat.Object : nullptr;

	auto SetupCyl = [BaseMat, Cyl](UStaticMeshComponent* Comp)
	{
		if (!Comp) return;
		if (Cyl) Comp->SetStaticMesh(Cyl);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(true);
		if (BaseMat) Comp->SetMaterial(0, BaseMat);
		Comp->SetVisibility(false);
		Comp->SetHiddenInGame(true);
	};
	auto SetupBox = [BaseMat, Cube](UStaticMeshComponent* Comp)
	{
		if (!Comp) return;
		if (Cube) Comp->SetStaticMesh(Cube);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(true);
		if (BaseMat) Comp->SetMaterial(0, BaseMat);
		Comp->SetVisibility(false);
		Comp->SetHiddenInGame(true);
	};
	SetupCyl(MastMesh);
	SetupCyl(BoomMesh);
	SetupCyl(BowspritMesh);
	SetupCyl(ForestayFoilMesh);
	SetupCyl(FurlerDrumMesh);
	SetupCyl(SpinSheetMesh);
	SetupCyl(SpinSheetToWinchMesh);
	SetupCyl(SpinLazySheetSegA);
	SetupCyl(SpinLazySheetSegB);
	SetupCyl(JibTrackPortMesh);
	SetupCyl(JibTrackStbdMesh);
	SetupCyl(TravelerTrackMesh);
	SetupCyl(MainsheetRopeMesh);
	SetupCyl(VangRopeMesh);
	SetupCyl(JibSheetPortMesh);
	SetupCyl(JibSheetStbdMesh);
	SetupCyl(JibSheetPortToWinchMesh);
	SetupCyl(JibSheetStbdToWinchMesh);
	SetupCyl(JibLazySheetSegA);
	SetupCyl(JibLazySheetSegB);
	for (UStaticMeshComponent* Comp : StandingRigMeshes)
	{
		SetupCyl(Comp);
		if (Comp)
		{
			Comp->SetCastShadow(false);
			Comp->bCastContactShadow = false;
		}
	}
	auto SetupLightMesh = [BaseMat, Sphere](UStaticMeshComponent* Comp, float Scale)
	{
		if (!Comp) return;
		if (Sphere) Comp->SetStaticMesh(Sphere);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(false);
		if (BaseMat) Comp->SetMaterial(0, BaseMat);
		Comp->SetVisibility(false);
		Comp->SetHiddenInGame(true);
		Comp->SetRelativeScale3D(FVector(Scale));
	};
	for (int32 I = 0; I < BoatLightCount; ++I)
	{
		SetupLightMesh(LightHousingMeshes[I], 0.055f);
		SetupLightMesh(LightLensMeshes[I], 0.038f);
	}
	SetupBox(JibCarPortMesh);
	SetupBox(JibCarStbdMesh);
	SetupBox(TravelerCarMesh);
	SetupBox(MidboomBlockMesh);
	SetupBox(VangMastBlockMesh);
	SetupBox(VangBoomBlockMesh);
	MastMesh->SetVisibility(false);
	BoomMesh->SetVisibility(false);

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(BoatRoot);
	// Do not inherit boat pitch/roll — wave pitch was pulling the camera into the hull.
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritYaw = true;
	SpringArm->bInheritRoll = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = false; // enable after spawn frames (RefreshChaseCamera)
	SpringArm->bEnableCameraRotationLag = false;
	// Low exterior chase — just above deck (not high bird's-eye).
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 120.f));
	SpringArm->SocketOffset = FVector(0.f, 0.f, 140.f);
	SpringArm->TargetOffset = FVector(0.f, 0.f, 80.f);
	OrbitYawDeg = 25.f;
	OrbitPitchDeg = -16.f;
	RefreshChaseCamera();

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	// Explicit socket — if attachment misses SpringEndpoint, camera sits at boom = inside hull.
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(72.f);
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	bFindCameraComponentWhenViewTarget = true;

	// Nantucket Harbor (web NAV_BOAT_START) — always recompute so geo stays single source of truth
	OpenWaterSpawnXY = FNavGeo::BoatStartWorldCm2D();
}

void ASailBoatPawn::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// Editor viewport + construction script: load loft so boat is visible without PIE.
	LoadLoftMesh(/*bApplyDynamics*/ false);
}

void ASailBoatPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Map-placed / World-Partition boats must never become the session boat.
	// Only GameMode-spawned pawns set bPlayerSessionBoat before/around possession.
	// If something else possesses a level boat, reject and self-destroy.
	if (!bPlayerSessionBoat)
	{
		UE_LOG(LogSailSim, Warning,
			TEXT("Rejecting possession of non-session boat %s at %s (map/WP orphan)"),
			*GetName(), *GetActorLocation().ToCompactString());
		if (AController* C = GetController())
		{
			C->UnPossess();
		}
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
		Destroy();
		return;
	}

	OrphanGraceFrames = 0;
	StartupSkipFrames = 5;
	WaterSnapFrames = 12;
	CameraLagEnableFrames = 8;
	// Harbor spawn XY + water under boat + terrain stream
	SnapToWaterSurface(/*bForceXY*/ true);
	EnsureOceanCoverage();
	if (UWorld* World = GetWorld())
	{
		if (UNantucketTerrainSubsystem* Terr = World->GetSubsystem<UNantucketTerrainSubsystem>())
		{
			Terr->ForceStreamAround(GetActorLocation());
		}
		if (UNantucketStructuresSubsystem* Structs = World->GetSubsystem<UNantucketStructuresSubsystem>())
		{
			Structs->ForceStreamAround(GetActorLocation());
		}
		// Stream world content once from possess (BeginPlay also calls ForceStream;
		// moored boats debounce duplicate calls).
		if (UMooredBoatSubsystem* Moored = World->GetSubsystem<UMooredBoatSubsystem>())
		{
			Moored->ForceStreamAround(GetActorLocation());
		}
		if (UEncAidSubsystem* Aids = World->GetSubsystem<UEncAidSubsystem>())
		{
			Aids->ForceStreamAround(GetActorLocation());
		}
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			Wind->SetBaseWind(Dynamics.TrueWindSpeedKn, Dynamics.TrueWindDirDeg);
			Wind->ForceFocus(GetActorLocation());
		}
	}
	RefreshChaseCamera();
	if (APlayerController* PC = Cast<APlayerController>(NewController))
	{
		PC->SetViewTarget(this);
	}
	UE_LOG(LogSailSim, Log, TEXT("Possessed player boat %s at %s (Nantucket Harbor)"),
		*GetName(), *GetActorLocation().ToCompactString());
}

void ASailBoatPawn::UnPossessed()
{
	Super::UnPossessed();
	// Don't clear bPlayerSessionBoat — still the session boat if briefly unpossessed.
}

void ASailBoatPawn::UpdateHullCollisionFromMesh()
{
	if (!HullCollision) return;

	// Prefer LOA/beam from JSON dims when available; fall back to mesh bounds.
	float HalfX = HullLengthCm * 0.48f;
	float HalfY = HullBeamCm * 0.48f;
	float HalfZ = FMath::Max(80.f, HullLengthCm * 0.08f);
	FVector Center(0.f, 0.f, -HalfZ * 0.35f);

	if (LoftMesh && LoftMesh->GetNumSections() > 0)
	{
		FBox Sphere(ForceInit);
		bool bAny = false;
		for (int32 S = 0; S < LoftMesh->GetNumSections(); ++S)
		{
			const FProcMeshSection* Sec = LoftMesh->GetProcMeshSection(S);
			if (!Sec) continue;
			for (const FProcMeshVertex& V : Sec->ProcVertexBuffer)
			{
				Sphere += V.Position;
				bAny = true;
			}
		}
		if (bAny)
		{
			const FVector Ext = Sphere.GetExtent();
			HalfX = FMath::Max(Ext.X, 50.f);
			HalfY = FMath::Max(Ext.Y, 30.f);
			HalfZ = FMath::Max(Ext.Z * 0.55f, 40.f);
			Center = Sphere.GetCenter();
			// Keep box center near waterline (root), slightly below deck
			Center.Z = FMath::Clamp(Center.Z * 0.35f, -HalfZ, 0.f);
		}
	}

	HullCollision->SetBoxExtent(FVector(HalfX, HalfY, HalfZ));
	HullCollision->SetRelativeLocation(Center);
	// Keep query-only (see constructor) — never re-enable blocking physics here.
	HullCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	UE_LOG(LogSailSim, Log, TEXT("HullCollision box half-ext (%.0f, %.0f, %.0f) at Z=%.0f"),
		HalfX, HalfY, HalfZ, Center.Z);
}

void ASailBoatPawn::PlaceSparFromEndpoints(UStaticMeshComponent* Comp, const FVector& A, const FVector& B)
{
	if (!Comp) return;
	const FVector Mid = (A + B) * 0.5f;
	const FVector Dir = B - A;
	const float Len = Dir.Size();
	if (Len < 1.f) return;
	Comp->SetVisibility(true);
	Comp->SetHiddenInGame(false);
	Comp->SetRelativeLocation(Mid);
	// BasicShapes Cylinder is Z-up. Scale Z = length/100 (default cyl height 100).
	Comp->SetRelativeScale3D(FVector(0.10f, 0.10f, Len / 100.f));
	Comp->SetRelativeRotation(FRotationMatrix::MakeFromZ(Dir.GetSafeNormal()).Rotator());
}

void ASailBoatPawn::UpdateJibFurlerVisuals(float MeshS)
{
	// Never draw the thick "foil" extrusion — standing rig already has a forestay
	// wire. A bad stay transform made that mesh start mid-deck and cut the mast.
	if (ForestayFoilMesh)
	{
		ForestayFoilMesh->SetVisibility(false);
		ForestayFoilMesh->SetHiddenInGame(true);
	}

	// Furler drum only: place at the real forestay tack in boat space.
	// Jib cloth stays are sail-local (mesh pivoted at mast) — transform through jib mesh.
	if (!FurlerDrumMesh || !JibSailMesh || !JibCloth.bStayValid)
	{
		if (FurlerDrumMesh)
		{
			FurlerDrumMesh->SetVisibility(false);
			FurlerDrumMesh->SetHiddenInGame(true);
		}
		return;
	}

	const FTransform JibXf = JibSailMesh->GetRelativeTransform();
	const FVector Tack = JibXf.TransformPosition(JibCloth.StayTackLocal);
	const FVector Head = JibXf.TransformPosition(JibCloth.StayHeadLocal);
	FVector Axis = Head - Tack;
	const float StayLen = Axis.Size();
	// Sanity: forestay must run forward of the mast and be long enough.
	if (StayLen < 80.f || Tack.X < (bMastPivotValid ? MastBaseLoc.X * MeshS - 20.f : -1.e9f))
	{
		// Stay endpoints look wrong — hide rather than draw a ghost stay.
		FurlerDrumMesh->SetVisibility(false);
		FurlerDrumMesh->SetHiddenInGame(true);
		return;
	}
	Axis /= StayLen;

	const float DrumH = 14.f * MeshS;
	const float DrumR = 0.14f;
	const FVector DrumCenter = Tack + Axis * (DrumH * 0.55f);
	const FVector DrumTop = DrumCenter + Axis * (DrumH * 0.5f);
	const FVector DrumBot = DrumCenter - Axis * (DrumH * 0.5f);
	PlaceSparFromEndpoints(FurlerDrumMesh, DrumBot, DrumTop);
	const FRotator BaseRot = FRotationMatrix::MakeFromZ(Axis).Rotator();
	const FQuat SpinQ(Axis, FurlerSpinRad);
	FurlerDrumMesh->SetRelativeRotation((SpinQ * BaseRot.Quaternion()).Rotator());
	FurlerDrumMesh->SetRelativeScale3D(FVector(DrumR, DrumR, DrumH / 100.f));
	FurlerDrumMesh->SetVisibility(true);
	FurlerDrumMesh->SetHiddenInGame(false);
}

void ASailBoatPawn::PlaceBoxAt(UStaticMeshComponent* Comp, const FVector& Center, const FVector& Scale, const FRotator& Rot)
{
	if (!Comp) return;
	Comp->SetVisibility(true);
	Comp->SetHiddenInGame(false);
	Comp->SetRelativeLocation(Center);
	Comp->SetRelativeScale3D(Scale);
	Comp->SetRelativeRotation(Rot);
}

void ASailBoatPawn::EnsureRunningRiggingBuilt()
{
	// Tint aluminum track / black blocks once materials are available
	auto Tint = [](UStaticMeshComponent* Comp, const FLinearColor& C)
	{
		if (!Comp) return;
		UMaterialInterface* Base = Comp->GetMaterial(0);
		if (!Base)
		{
			Base = LoadObject<UMaterialInterface>(
				nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		if (!Base) return;
		UMaterialInstanceDynamic* Mid = Comp->CreateAndSetMaterialInstanceDynamic(0);
		if (!Mid) return;
		Mid->SetVectorParameterValue(TEXT("Color"), C);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), C);
	};
	const FLinearColor TrackCol(0.45f, 0.48f, 0.52f, 1.f);   // aluminum rail
	const FLinearColor BlockCol(0.12f, 0.12f, 0.13f, 1.f);  // black block
	const FLinearColor RopeCol(0.18f, 0.14f, 0.10f, 1.f);   // sheet rope
	Tint(JibTrackPortMesh, TrackCol);
	Tint(JibTrackStbdMesh, TrackCol);
	Tint(TravelerTrackMesh, TrackCol);
	Tint(JibCarPortMesh, BlockCol);
	Tint(JibCarStbdMesh, BlockCol);
	Tint(TravelerCarMesh, BlockCol);
	Tint(MidboomBlockMesh, BlockCol);
	Tint(VangMastBlockMesh, BlockCol);
	Tint(VangBoomBlockMesh, BlockCol);
	Tint(MainsheetRopeMesh, RopeCol);
	Tint(VangRopeMesh, RopeCol);
	Tint(SpinSheetMesh, RopeCol);
	Tint(SpinSheetToWinchMesh, RopeCol);
	Tint(JibSheetPortMesh, RopeCol);
	Tint(JibSheetStbdMesh, RopeCol);
	Tint(JibSheetPortToWinchMesh, RopeCol);
	Tint(JibSheetStbdToWinchMesh, RopeCol);
	Tint(BowspritMesh, TrackCol);
	Tint(ForestayFoilMesh, TrackCol);
	// Furler drum: dark alloy (Harken MKIV look)
	Tint(FurlerDrumMesh, BlockCol);
	// Lazy sheet slightly browner / secondary (web opacity 0.5)
	const FLinearColor LazyRopeCol(0.22f, 0.18f, 0.13f, 1.f);
	Tint(JibLazySheetSegA, LazyRopeCol);
	Tint(JibLazySheetSegB, LazyRopeCol);
	Tint(SpinLazySheetSegA, LazyRopeCol);
	Tint(SpinLazySheetSegB, LazyRopeCol);
}

void ASailBoatPawn::UpdateRunningRigging()
{
	const float MeshS = ActiveSpec.MeshUniformScale();

	// --- Jib genoa tracks + cars (web railCyl + car meshes) ---
	if (CachedRigging.bJibTracksValid)
	{
		const FVector PFwd = CachedRigging.JibPortTrackFwd * MeshS;
		const FVector PAft = CachedRigging.JibPortTrackAft * MeshS;
		const FVector SFwd = CachedRigging.JibStbdTrackFwd * MeshS;
		const FVector SAft = CachedRigging.JibStbdTrackAft * MeshS;
		PlaceSparFromEndpoints(JibTrackPortMesh, PFwd, PAft);
		PlaceSparFromEndpoints(JibTrackStbdMesh, SFwd, SAft);
		// Thin track rails
		if (JibTrackPortMesh) JibTrackPortMesh->SetRelativeScale3D(
			FVector(0.04f, 0.04f, FVector::Dist(PFwd, PAft) / 100.f));
		if (JibTrackStbdMesh) JibTrackStbdMesh->SetRelativeScale3D(
			FVector(0.04f, 0.04f, FVector::Dist(SFwd, SAft) / 100.f));

		const float Car = FMath::Clamp(Dynamics.JibCar01, 0.f, 1.f);
		const FVector CarPort = FMath::Lerp(PFwd, PAft, Car) + FVector(0.f, 0.f, 3.f);
		const FVector CarStbd = FMath::Lerp(SFwd, SAft, Car) + FVector(0.f, 0.f, 3.f);
		PlaceBoxAt(JibCarPortMesh, CarPort, FVector(0.18f, 0.10f, 0.12f), FRotator::ZeroRotator);
		PlaceBoxAt(JibCarStbdMesh, CarStbd, FVector(0.18f, 0.10f, 0.12f), FRotator::ZeroRotator);

		// Active lee sheet (taut) + windward lazy sheet (slack, around mast).
		// Web: activeLead by wind; lazy routes clew → mast-wrap → lazyLead with sag.
		FVector JibClewBoat = CarPort; // fallback
		if (JibCloth.bInitialized && JibCloth.ClewIndex != INDEX_NONE && JibSailMesh)
		{
			const FVector ClewLocal = JibCloth.Pos[JibCloth.ClewIndex];
			JibClewBoat = JibSailMesh->GetRelativeTransform().TransformPosition(ClewLocal);
		}
		// Active = leeward car (same side as air flow / clew). Web:
		//   activeLead = (jibLeadPort.z * windVel.z >= 0) ? port : stbd
		// UE: +Y stbd; air vel Y is downwind stbd component.
		const FVector AirVel = GetApparentWindAirVelBoat();
		bool bLeePort = (GetLeeSideSign() > 0.f); // +1 lee → port
		if (FMath::Abs(AirVel.Y) > 0.08f)
		{
			// Air flowing to port (Y<0) → lee is port
			bLeePort = (AirVel.Y < 0.f);
		}
		else if (FMath::Abs(JibClewBoat.Y) > 5.f)
		{
			// Fallback: clew sits to leeward
			bLeePort = (JibClewBoat.Y < 0.f);
		}
		// Cars are corrected at load so Port.Y < 0 ≤ Stbd.Y
		const FVector ActiveCar = bLeePort ? CarPort : CarStbd;
		const FVector LazyCar = bLeePort ? CarStbd : CarPort;
		UStaticMeshComponent* LeeSheet = bLeePort ? JibSheetPortMesh : JibSheetStbdMesh;
		UStaticMeshComponent* WxSheet = bLeePort ? JibSheetStbdMesh : JibSheetPortMesh;

		// --- Active (leeward) sheet: clew → car (physics lead), then car → primary winch ---
		PlaceSparFromEndpoints(LeeSheet, ActiveCar, JibClewBoat);
		if (LeeSheet && LeeSheet->IsVisible())
		{
			const float L = FVector::Dist(ActiveCar, JibClewBoat);
			LeeSheet->SetRelativeScale3D(FVector(0.028f, 0.028f, L / 100.f));
		}

		// Primary winch terminals (JSON drum leads). Fallback: J/105 primaries sit
		// well aft of the genoa cars on the coaming (~5 ft / 150+ cm), not next to the car.
		const float WinchAft = 165.f * MeshS;
		const float WinchUp = 28.f * MeshS;
		FVector PortWinch(
			CarPort.X - WinchAft,
			CarPort.Y * 0.72f,
			CarPort.Z + WinchUp);
		FVector StbdWinch(
			CarStbd.X - WinchAft,
			CarStbd.Y * 0.72f,
			CarStbd.Z + WinchUp);
		if (CachedRigging.bSheetLeadsValid)
		{
			// sheet_leads are unscaled boat-cm (same as tracks); scale like the cars.
			PortWinch = CachedRigging.JibPortWinch * MeshS;
			StbdWinch = CachedRigging.JibStbdWinch * MeshS;
			// Keep drum lead above the car so the rope doesn't bury in the coaming.
			PortWinch.Z = FMath::Max(PortWinch.Z, CarPort.Z + 12.f * MeshS);
			StbdWinch.Z = FMath::Max(StbdWinch.Z, CarStbd.Z + 12.f * MeshS);
		}
		const FVector ActiveWinch = bLeePort ? PortWinch : StbdWinch;
		const FVector LazyWinch = bLeePort ? StbdWinch : PortWinch;
		UStaticMeshComponent* LeeToWinch = bLeePort ? JibSheetPortToWinchMesh : JibSheetStbdToWinchMesh;
		UStaticMeshComponent* WxToWinch = bLeePort ? JibSheetStbdToWinchMesh : JibSheetPortToWinchMesh;

		// Lee: taut car → primary (must span full coaming run to the winch drum).
		auto PlaceSheetSeg = [this](UStaticMeshComponent* C, const FVector& A, const FVector& B, float Rad)
		{
			if (!C) return;
			const float L = FVector::Dist(A, B);
			if (L < 1.f) return;
			PlaceSparFromEndpoints(C, A, B);
			C->SetRelativeScale3D(FVector(Rad, Rad, L / 100.f));
			C->SetVisibility(true);
			C->SetHiddenInGame(false);
		};
		// Slight lift at car so the sheet leaves the track block upward toward the drum.
		const FVector LeeCarExit = ActiveCar + FVector(0.f, 0.f, 6.f * MeshS);
		const FVector WxCarExit = LazyCar + FVector(0.f, 0.f, 6.f * MeshS);
		PlaceSheetSeg(LeeToWinch, LeeCarExit, ActiveWinch, 0.026f);

		// --- Lazy (windward) sheet: visual only, deep slack. Physics never uses
		// the windward lead — only the lee car constrains the clew.
		FVector MastPt = bMastPivotValid ? (MastBaseLoc * MeshS) : FVector::ZeroVector;
		if (CachedRigging.bMainsheetValid)
		{
			MastPt = CachedRigging.MainsheetGooseneck * MeshS;
		}
		const float WSign = bLeePort ? 1.f : -1.f; // windward side of mast (+Y stbd)
		// Hang nearly on deck so the rope cannot look load-bearing.
		const float BellyZ = FMath::Min(LazyCar.Z, MastPt.Z) - 6.f;
		const FVector Hang(
			FMath::Lerp(JibClewBoat.X, MastPt.X, 0.4f),
			FMath::Lerp(JibClewBoat.Y, LazyCar.Y * 0.25f, 0.55f),
			FMath::Lerp(JibClewBoat.Z, BellyZ, 0.9f));
		// Windward of mast, low, then up to lazy car
		const FVector HeapWx(
			FMath::Lerp(Hang.X, LazyCar.X, 0.55f),
			FMath::Lerp(Hang.Y, LazyCar.Y, 0.55f) + WSign * 30.f,
			BellyZ);

		PlaceSparFromEndpoints(WxSheet, JibClewBoat, Hang);
		PlaceSparFromEndpoints(JibLazySheetSegA, Hang, HeapWx);
		PlaceSparFromEndpoints(JibLazySheetSegB, HeapWx, LazyCar);
		// Lazy continues car → primary (full run to winch, thinner).
		PlaceSheetSeg(WxToWinch, WxCarExit, LazyWinch, 0.016f);

		auto ThinLazy = [](UStaticMeshComponent* C, const FVector& A, const FVector& B, float Rad = 0.014f)
		{
			if (!C) return;
			const float L = FVector::Dist(A, B);
			if (L < 1.f) return;
			C->SetRelativeScale3D(FVector(Rad, Rad, L / 100.f));
			C->SetVisibility(true);
			C->SetHiddenInGame(false);
		};
		ThinLazy(WxSheet, JibClewBoat, Hang);
		ThinLazy(JibLazySheetSegA, Hang, HeapWx);
		ThinLazy(JibLazySheetSegB, HeapWx, LazyCar);

		// Hide jib sheets when sail is mostly furled (tracks/cars stay).
		if (JibSet01 < 0.18f)
		{
			auto Hide = [](UStaticMeshComponent* C)
			{
				if (!C) return;
				C->SetVisibility(false);
				C->SetHiddenInGame(true);
			};
			Hide(JibSheetPortMesh);
			Hide(JibSheetStbdMesh);
			Hide(JibSheetPortToWinchMesh);
			Hide(JibSheetStbdToWinchMesh);
			Hide(JibLazySheetSegA);
			Hide(JibLazySheetSegB);
		}
	}

	// --- Mainsheet traveler + midboom sheet + boom vang (J/105 layout) ---
	// Traveler on cockpit sole, midboom sheet, rigid vang 25% along boom.
	if (bBoomEndpointsValid)
	{
		FVector BoomBase, BoomTip;
		GetBoomEndpointsBoat(MeshS, BoomBase, BoomTip);
		const float BoomLen = FMath::Max(1.f, FVector::Dist(BoomBase, BoomTip));

		// Gooseneck / traveler center: prefer JSON, else live boom base + deck sole.
		FVector Goose = BoomBase;
		FVector Lead;
		if (CachedRigging.bMainsheetValid)
		{
			Goose = CachedRigging.MainsheetGooseneck * MeshS;
			Lead = CachedRigging.MainsheetLead * MeshS;
			if (CachedRigging.VangBoomFrac > 0.01f) VangBoomFrac = CachedRigging.VangBoomFrac;
			if (CachedRigging.VangMastDropFrac > 0.01f) VangMastDropFrac = CachedRigging.VangMastDropFrac;
		}
		else
		{
			// Fallback: traveler ~mid-cockpit (0.52·E aft of goose), sole under boom
			const float Ecm = (CachedRigging.BoomLengthCm > 1.f)
				? CachedRigging.BoomLengthCm * MeshS
				: BoomLen;
			Lead = FVector(Goose.X - 0.52f * Ecm, 0.f, Goose.Z - FMath::Max(50.f, 0.55f * (Goose.Z - BoomBase.Z + 80.f)));
			// Keep lead near deck: if gooseneck Z is boom height, drop ~2 ft
			if (FMath::Abs(Lead.Z - Goose.Z) < 20.f)
			{
				Lead.Z = Goose.Z - 60.f * MeshS;
			}
		}

		// --- Traveler track (athwartships on cockpit sole) ---
		// J/105 traveler ~1.4 m / ~4.5 ft total span
		const float HalfY = TravelerHalfWidthCm * MeshS;
		const FVector TravPort(Lead.X, -HalfY, Lead.Z);
		const FVector TravStbd(Lead.X, HalfY, Lead.Z);
		PlaceSparFromEndpoints(TravelerTrackMesh, TravPort, TravStbd);
		if (TravelerTrackMesh)
		{
			TravelerTrackMesh->SetRelativeScale3D(
				FVector(0.055f, 0.055f, FVector::Dist(TravPort, TravStbd) / 100.f));
			TravelerTrackMesh->SetVisibility(true);
			TravelerTrackMesh->SetHiddenInGame(false);
		}

		// Car rides leeward with the boom. UE: +BoomYawDeg → boom to port (−Y).
		const float BoomFrac = FMath::Clamp(FMath::Abs(BoomYawDeg) / MaxBoomSwingDeg, 0.f, 1.f);
		const float SignYaw = (FMath::Abs(BoomYawDeg) < 0.5f) ? 0.f : -FMath::Sign(BoomYawDeg);
		const float CarY = SignYaw * HalfY * BoomFrac * 0.85f;
		const FVector TravCar(Lead.X, CarY, Lead.Z + 5.f);
		PlaceBoxAt(TravelerCarMesh, TravCar, FVector(0.16f, 0.26f, 0.12f), FRotator::ZeroRotator);
		if (TravelerCarMesh)
		{
			TravelerCarMesh->SetVisibility(true);
			TravelerCarMesh->SetHiddenInGame(false);
		}

		// Midboom sheet block (web MIDBOOM_FRAC = 0.5)
		const FVector MidBoom = FMath::Lerp(BoomBase, BoomTip, MidboomSheetFrac);
		const FVector SheetBlock = MidBoom + FVector(0.f, 0.f, -10.f);
		PlaceBoxAt(MidboomBlockMesh, SheetBlock, FVector(0.14f, 0.14f, 0.16f), FRotator::ZeroRotator);
		if (MidboomBlockMesh)
		{
			MidboomBlockMesh->SetVisibility(true);
			MidboomBlockMesh->SetHiddenInGame(false);
		}

		// Mainsheet rope: midboom block → traveler car
		PlaceSparFromEndpoints(MainsheetRopeMesh, SheetBlock, TravCar);
		if (MainsheetRopeMesh)
		{
			MainsheetRopeMesh->SetRelativeScale3D(
				FVector(0.032f, 0.032f, FVector::Dist(SheetBlock, TravCar) / 100.f));
			MainsheetRopeMesh->SetVisibility(true);
			MainsheetRopeMesh->SetHiddenInGame(false);
		}

		// --- Boom vang (web: mast drop 6%·E, boom attach 25%·E) ---
		const float Drop = FMath::Max(12.f, VangMastDropFrac * BoomLen);
		const FVector VangMast = Goose - FVector(0.f, 0.f, Drop);
		const FVector VangBoom = FMath::Lerp(BoomBase, BoomTip, FMath::Clamp(VangBoomFrac, 0.1f, 0.5f));
		PlaceBoxAt(VangMastBlockMesh, VangMast, FVector(0.12f, 0.12f, 0.14f), FRotator::ZeroRotator);
		PlaceBoxAt(VangBoomBlockMesh, VangBoom + FVector(0.f, 0.f, -6.f), FVector(0.12f, 0.12f, 0.12f), FRotator::ZeroRotator);
		PlaceSparFromEndpoints(VangRopeMesh, VangMast, VangBoom + FVector(0.f, 0.f, -6.f));
		if (VangRopeMesh)
		{
			VangRopeMesh->SetRelativeScale3D(
				FVector(0.030f, 0.030f, FVector::Dist(VangMast, VangBoom) / 100.f));
			VangRopeMesh->SetVisibility(true);
			VangRopeMesh->SetHiddenInGame(false);
		}
		if (VangMastBlockMesh)
		{
			VangMastBlockMesh->SetVisibility(true);
			VangMastBlockMesh->SetHiddenInGame(false);
		}
		if (VangBoomBlockMesh)
		{
			VangBoomBlockMesh->SetVisibility(true);
			VangBoomBlockMesh->SetHiddenInGame(false);
		}
	}
}

void ASailBoatPawn::UpdateStandingRigging()
{
	// Web hBuildSpreadersAndStanding (J/105 single swept spreaders + wire).
	if (!bMastPivotValid || StandingRigMeshes.Num() < StandingRigCount) return;

	const float MeshS = ActiveSpec.MeshUniformScale();
	const FVector MastBase = MastBaseLoc * MeshS;
	const float MastH = FMath::Max(100.f, ActiveSpec.I * 30.48f * MeshS);
	const FVector MastTop = MastBase + FVector(0.f, 0.f, MastH);
	const float Mx = MastBase.X;
	const float Mz = MastBase.Z;
	const float HalfB = FMath::Max(80.f, HullBeamCm * 0.5f * MeshS);

	// Spreaders ~52% mast height, slight sweep aft + dihedral (web feet → cm)
	const float SprZ = Mz + MastH * 0.52f;
	const float SprHalf = FMath::Max(73.f, HalfB * 0.95f);
	const float SprAft = 0.55f * 30.48f * MeshS;
	const float SprUp = 0.22f * 30.48f * MeshS;
	const FVector Root(Mx, 0.f, SprZ);
	const FVector TipP(Mx - SprAft, -SprHalf, SprZ + SprUp);
	const FVector TipS(Mx - SprAft, SprHalf, SprZ + SprUp);

	// Chainplates on deck, slightly aft of mast
	const float CpZ = Mz + 0.12f * 30.48f * MeshS;
	const float CpX = Mx - 0.35f * 30.48f * MeshS;
	const FVector CpP(CpX, -HalfB * 0.88f, CpZ);
	const FVector CpS(CpX, HalfB * 0.88f, CpZ);
	const FVector Truck(Mx, 0.f, MastTop.Z - 0.15f * 30.48f * MeshS);

	// Forestay: stem ≈ mast + J forward; head near mast at I
	const float Jcm = FMath::Max(200.f, (ActiveSpec.J > 0.f ? ActiveSpec.J : 13.5f) * 30.48f * MeshS);
	const FVector ForestayTack(Mx + Jcm, 0.f, Mz + 0.15f * 30.48f * MeshS);
	const FVector ForestayHead(Mx, 0.f, FMath::Min(MastTop.Z - 2.f, Mz + MastH * 0.96f));

	// Backstay: truck → stern deck centerline
	const float LoaCm = FMath::Max(500.f, HullLengthCm * MeshS);
	const FVector BackstayDeck(Mx - LoaCm * 0.55f, 0.f, Mz + 0.2f * 30.48f * MeshS);

	auto PlaceWire = [this](int32 Idx, const FVector& A, const FVector& B, float Rad)
	{
		if (!StandingRigMeshes.IsValidIndex(Idx) || !StandingRigMeshes[Idx]) return;
		PlaceSparFromEndpoints(StandingRigMeshes[Idx], A, B);
		const float L = FVector::Dist(A, B);
		if (L > 1.f)
		{
			StandingRigMeshes[Idx]->SetRelativeScale3D(FVector(Rad, Rad, L / 100.f));
		}
	};

	// 0-1 spreaders (aluminum tube)
	PlaceWire(0, Root, TipP, 0.07f);
	PlaceWire(1, Root, TipS, 0.07f);
	// 2 collar
	PlaceWire(2, FVector(Mx, 0.f, SprZ - 3.5f), FVector(Mx, 0.f, SprZ + 3.5f), 0.14f);
	// 3-4 lowers
	PlaceWire(3, CpP, TipP, 0.022f);
	PlaceWire(4, CpS, TipS, 0.022f);
	// 5-6 uppers tip→truck
	PlaceWire(5, TipP, Truck, 0.020f);
	PlaceWire(6, TipS, Truck, 0.020f);
	// 7-8 cap shrouds
	PlaceWire(7, CpP, Truck, 0.019f);
	PlaceWire(8, CpS, Truck, 0.019f);
	// 9 forestay, 10 backstay
	PlaceWire(9, ForestayTack, ForestayHead, 0.024f);
	PlaceWire(10, MastTop, BackstayDeck, 0.021f);

	// Materials: spreaders aluminum; wire stainless
	UMaterialInterface* SparMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_Spar.MI_Yacht_Spar"));
	UMaterialInterface* RopeMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_Rope.MI_Yacht_Rope"));
	const FLinearColor Al(0.70f, 0.74f, 0.78f, 1.f);
	const FLinearColor Wire(0.38f, 0.42f, 0.46f, 1.f);
	auto Tint = [](UStaticMeshComponent* C, const FLinearColor& Col, float Met, float Rough)
	{
		if (!C || !C->IsVisible()) return;
		UMaterialInstanceDynamic* Mid = C->CreateAndSetMaterialInstanceDynamic(0);
		if (!Mid) return;
		Mid->SetVectorParameterValue(TEXT("Color"), Col);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Col);
		Mid->SetScalarParameterValue(TEXT("Metallic"), Met);
		Mid->SetScalarParameterValue(TEXT("Roughness"), Rough);
	};
	for (int32 I = 0; I < StandingRigCount; ++I)
	{
		UStaticMeshComponent* C = StandingRigMeshes[I];
		if (!C) continue;
		const bool bSpar = (I <= 2);
		if (bSpar && SparMat) C->SetMaterial(0, SparMat);
		else if (!bSpar && RopeMat) C->SetMaterial(0, RopeMat);
		else Tint(C, bSpar ? Al : Wire, bSpar ? 0.85f : 0.9f, bSpar ? 0.32f : 0.22f);
		if (bSpar && SparMat)
		{
			// ensure aluminum look on spar MI
			Tint(C, Al, 0.9f, 0.28f);
		}
		else if (!bSpar)
		{
			Tint(C, Wire, 0.92f, 0.18f);
		}
	}
}

void ASailBoatPawn::EnsureBoatLightsBuilt()
{
	if (bBoatLightsBuilt) return;
	bBoatLightsBuilt = true;
	// Positions applied in UpdateBoatLights from mast/beam.
	UpdateBoatLights();
}

void ASailBoatPawn::UpdateBoatLights()
{
	if (!bMastPivotValid && MastBaseLoc.IsNearlyZero()) return;
	EnsureBoatLightsBuilt();

	const float MeshS = ActiveSpec.MeshUniformScale();
	const FVector MastBase = MastBaseLoc * MeshS;
	const float MastH = FMath::Max(100.f, ActiveSpec.I * 30.48f * MeshS);
	const FVector MastTop = MastBase + FVector(0.f, 0.f, MastH);
	const float Mx = MastBase.X;
	const float Mz = MastBase.Z;
	const float HalfB = FMath::Max(80.f, HullBeamCm * 0.5f * MeshS);
	const float Jcm = FMath::Max(200.f, (ActiveSpec.J > 0.f ? ActiveSpec.J : 13.5f) * 30.48f * MeshS);
	const float LoaCm = FMath::Max(500.f, HullLengthCm * MeshS);
	const float Ft = 30.48f * MeshS;

	// Fixtures on the boat — bow is much narrower than max beam, so do NOT use
	// full HalfB at the stem (that hangs lights over open water).
	// Port/stbd sit on the bow pulpit / stemhead corners (~35–40% of max half-beam).
	const float BowHalfY = HalfB * 0.36f;
	const FVector PosPort(Mx + Jcm * 0.72f, -BowHalfY, Mz + 0.50f * Ft);
	const FVector PosStbd(Mx + Jcm * 0.72f, BowHalfY, Mz + 0.50f * Ft);
	// Stern light on the transom centerline (slightly inboard of the rail).
	const FVector PosStern(Mx - LoaCm * 0.46f, 0.f, Mz + 0.65f * Ft);
	const FVector PosSteam(Mx + 0.2f * Ft, 0.f, Mz + MastH * 0.65f);
	const FVector PosAnchor(Mx, 0.f, MastTop.Z + 0.12f * Ft);
	const float SprZ = Mz + MastH * 0.52f;
	const float SprHalf = FMath::Max(73.f, HalfB * 0.95f);
	const FVector PosDeckP(Mx - 0.30f * Ft, -SprHalf * 0.55f, SprZ - 0.10f * Ft);
	const FVector PosDeckS(Mx - 0.30f * Ft, SprHalf * 0.55f, SprZ - 0.10f * Ft);
	const FVector PosCabin(Mx + 0.15f * LoaCm * 0.05f, 0.f, Mz + 0.90f * Ft);

	const FVector Positions[BoatLightCount] = {
		PosPort, PosStbd, PosStern, PosSteam, PosAnchor, PosDeckP, PosDeckS, PosCabin
	};

	// Saturated COLREGS lens colors (must read as red/green even at distance).
	const FLinearColor Colors[BoatLightCount] = {
		FLinearColor(1.00f, 0.04f, 0.02f),   // port red
		FLinearColor(0.02f, 0.95f, 0.18f),   // stbd green
		FLinearColor(0.98f, 0.96f, 0.90f),   // stern white
		FLinearColor(0.97f, 0.96f, 0.92f),   // steaming
		FLinearColor(0.96f, 0.97f, 1.00f),   // anchor
		FLinearColor(1.00f, 0.82f, 0.55f),   // deck halogen
		FLinearColor(1.00f, 0.82f, 0.55f),
		FLinearColor(1.00f, 0.72f, 0.42f),   // cabin tungsten
	};

	const bool On[BoatLightCount] = {
		bBreakerNav, bBreakerNav, bBreakerNav,
		bBreakerSteaming, bBreakerAnchor,
		bBreakerDeck, bBreakerDeck, bBreakerCabin
	};

	// Day/night scale from the *sun* only (ignore moon / dim night lights).
	// Night → full output; bright day → fixtures dim so they don't look like floods.
	float Night01 = 1.f;
	if (UWorld* World = GetWorld())
	{
		// Prefer env preset when available.
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			const uint8 Preset = Ocean->GetActiveEnvPreset();
			// ESailEnvPreset: FairDay=0 … Night=3
			if (Preset == 3) // Night
			{
				Night01 = 1.f;
			}
			else if (Preset == 2) // Dusk
			{
				Night01 = 0.85f;
			}
			else if (Preset == 1) // Golden
			{
				Night01 = 0.55f;
			}
			else
			{
				// Fair / overcast / storm / fog: derive from brightest non-moon directional.
				float BestSun = 0.f;
				for (TActorIterator<ADirectionalLight> It(World); It; ++It)
				{
					ADirectionalLight* L = *It;
					if (!IsValid(L)) continue;
					if (L->GetActorNameOrLabel().Contains(TEXT("SailSim_Moon"))) continue;
					if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(L->GetLightComponent()))
					{
						if (C->IsVisible())
						{
							BestSun = FMath::Max(BestSun, C->Intensity);
						}
					}
				}
				// sun int 0..10 → night factor 1..0.15
				Night01 = FMath::GetMappedRangeValueClamped(
					FVector2D(0.3f, 8.f), FVector2D(1.f, 0.18f), BestSun);
			}
		}
	}
	const float VisScale = FMath::Clamp(Night01, 0.15f, 1.f);
	// Extra punch for lens emissives at night (exposure bias darkens the scene).
	const float LensEmMul = FMath::Lerp(1.2f, 8.f, VisScale);

	// Housing + lens larger so red/green read as fixtures, not dust motes.
	const FLinearColor HousingCol(0.05f, 0.05f, 0.055f);
	const float HousingScale[BoatLightCount] = {
		0.10f, 0.10f, 0.09f, 0.07f, 0.065f, 0.055f, 0.055f, 0.11f
	};
	const float LensScale[BoatLightCount] = {
		0.075f, 0.075f, 0.065f, 0.05f, 0.048f, 0.04f, 0.04f, 0.07f
	};

	for (int32 I = 0; I < BoatLightCount; ++I)
	{
		const FVector P = Positions[I];
		// Lens sits just above housing — keep on-boat, no big outboard offset.
		FVector LensOff = FVector(0.f, 0.f, HousingScale[I] * 28.f);
		if (I == 0) LensOff += FVector(2.f, -2.f, 2.f);  // tiny outboard + forward
		if (I == 1) LensOff += FVector(2.f, 2.f, 2.f);
		if (I == 2) LensOff += FVector(-3.f, 0.f, 2.f);

		if (LightHousingMeshes.IsValidIndex(I) && LightHousingMeshes[I])
		{
			UStaticMeshComponent* H = LightHousingMeshes[I];
			H->SetRelativeLocation(P);
			H->SetRelativeScale3D(FVector(HousingScale[I]));
			H->SetVisibility(true);
			H->SetHiddenInGame(false);
			H->SetCastShadow(false);
			if (UMaterialInstanceDynamic* Mid = H->CreateAndSetMaterialInstanceDynamic(0))
			{
				Mid->SetVectorParameterValue(TEXT("Color"), HousingCol);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), HousingCol);
				Mid->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 0.75f);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.4f);
			}
		}
		if (LightLensMeshes.IsValidIndex(I) && LightLensMeshes[I])
		{
			UStaticMeshComponent* Lens = LightLensMeshes[I];
			Lens->SetRelativeLocation(P + LensOff);
			Lens->SetRelativeScale3D(FVector(LensScale[I]));
			Lens->SetVisibility(true);
			Lens->SetHiddenInGame(false);
			Lens->SetCastShadow(false);
			// Lenses must not be depth-culled against dark hull at night.
			Lens->SetBoundsScale(2.f);
			if (UMaterialInstanceDynamic* Mid = Lens->CreateAndSetMaterialInstanceDynamic(0))
			{
				const bool bLit = On[I];
				// Off: dark tinted glass. On: saturated color + strong emissive.
				const FLinearColor Base = bLit ? Colors[I] : (Colors[I] * 0.12f);
				const float Em = bLit ? (2.5f * LensEmMul) : 0.f;
				Mid->SetVectorParameterValue(TEXT("Color"), Base);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Base * (bLit ? 0.35f : 0.5f));
				Mid->SetVectorParameterValue(TEXT("EmissiveColor"), bLit ? (Colors[I] * Em) : FLinearColor::Black);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Em);
				Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Em);
				Mid->SetScalarParameterValue(TEXT("Emissive"), Em);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.12f);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 0.02f);
			}
		}
	}

	// Spot light setup: +X is light axis.
	auto AimSpot = [](USpotLightComponent* Spot, const FVector& LocalDir)
	{
		if (!Spot) return;
		const FVector D = LocalDir.GetSafeNormal();
		Spot->SetRelativeRotation(FRotationMatrix::MakeFromX(D).Rotator());
	};
	auto ConfigureSpot = [&](USpotLightComponent* Spot, const FVector& Pos, const FVector& Dir,
		const FLinearColor& Col, bool bOn, float Cd, float RadiusCm, float OuterDeg, float InnerDeg)
	{
		if (!Spot) return;
		// Slightly outboard of fixture so the hull doesn't eat the first cm of the beam.
		Spot->SetRelativeLocation(Pos + Dir.GetSafeNormal() * 6.f);
		AimSpot(Spot, Dir);
		Spot->SetLightColor(Col);
		Spot->SetIntensityUnits(ELightUnits::Candelas);
		Spot->SetIntensity(bOn ? Cd * VisScale : 0.f);
		Spot->SetAttenuationRadius(RadiusCm);
		Spot->SetInnerConeAngle(InnerDeg);
		Spot->SetOuterConeAngle(OuterDeg);
		Spot->SetSourceRadius(2.5f);
		Spot->SetSoftSourceRadius(5.f);
		Spot->SetSpecularScale(0.15f);
		Spot->SetVolumetricScatteringIntensity(0.08f);
		Spot->SetIndirectLightingIntensity(0.25f);
		Spot->SetCastShadows(false);
		Spot->SetVisibility(bOn);
		Spot->SetHiddenInGame(!bOn);
		Spot->SetUseInverseSquaredFalloff(true);
	};
	auto ConfigureGlow = [&](UPointLightComponent* Pt, const FVector& Pos,
		const FLinearColor& Col, bool bOn, float Cd, float RadiusCm)
	{
		if (!Pt) return;
		Pt->SetRelativeLocation(Pos);
		Pt->SetLightColor(Col);
		Pt->SetIntensityUnits(ELightUnits::Candelas);
		// Glow is the "see the light itself" cue; keep it local but punchy.
		Pt->SetIntensity(bOn ? Cd * VisScale : 0.f);
		Pt->SetAttenuationRadius(RadiusCm);
		Pt->SetSourceRadius(3.f);
		Pt->SetSoftSourceRadius(8.f);
		Pt->SetSpecularScale(0.12f);
		Pt->SetVolumetricScatteringIntensity(0.06f);
		Pt->SetIndirectLightingIntensity(0.4f);
		Pt->SetCastShadows(false);
		Pt->SetVisibility(bOn);
		Pt->SetHiddenInGame(!bOn);
	};

	// COLREGS-ish sectors: sidelights ~112.5° (outer ≈ 56° half-angle from axis
	// if axis is sector center). Aim outboard + slightly forward.
	// Candela / radius tuned for night: light water 15–40 m, not flood the world.
	ConfigureSpot(LightSpots[0], PosPort, FVector(0.55f, -1.f, 0.02f), Colors[0], On[0],
		55.f, 2800.f, 70.f, 40.f);
	ConfigureSpot(LightSpots[1], PosStbd, FVector(0.55f, 1.f, 0.02f), Colors[1], On[1],
		55.f, 2800.f, 70.f, 40.f);
	ConfigureSpot(LightSpots[2], PosStern, FVector(-1.f, 0.f, 0.05f), Colors[2], On[2],
		40.f, 2400.f, 68.f, 38.f);
	ConfigureSpot(LightSpots[3], PosSteam, FVector(1.f, 0.f, -0.1f), Colors[3], On[3],
		70.f, 3500.f, 80.f, 45.f);
	// Deck floods: light the foredeck at night
	ConfigureSpot(LightSpots[4], PosDeckP, FVector(0.25f, 0.2f, -1.f), Colors[5], On[5],
		90.f, 2200.f, 58.f, 28.f);
	ConfigureSpot(LightSpots[5], PosDeckS, FVector(0.25f, -0.2f, -1.f), Colors[6], On[6],
		90.f, 2200.f, 58.f, 28.f);

	// Local colored glow at the fixture (no big outboard offset — stay on boat).
	ConfigureGlow(LightPortGlow, PosPort + FVector(0.f, -3.f, 4.f), Colors[0], On[0], 28.f, 900.f);
	ConfigureGlow(LightStbdGlow, PosStbd + FVector(0.f, 3.f, 4.f), Colors[1], On[1], 28.f, 900.f);
	ConfigureGlow(LightSternGlow, PosStern + FVector(-3.f, 0.f, 3.f), Colors[2], On[2], 18.f, 700.f);

	// Anchor: all-round masthead white — visible from a distance on a dark sea.
	if (LightAnchorPoint)
	{
		LightAnchorPoint->SetRelativeLocation(PosAnchor);
		LightAnchorPoint->SetLightColor(Colors[4]);
		LightAnchorPoint->SetIntensityUnits(ELightUnits::Candelas);
		LightAnchorPoint->SetIntensity(On[4] ? 65.f * VisScale : 0.f);
		LightAnchorPoint->SetAttenuationRadius(4500.f);
		LightAnchorPoint->SetSourceRadius(2.5f);
		LightAnchorPoint->SetSoftSourceRadius(6.f);
		LightAnchorPoint->SetSpecularScale(0.12f);
		LightAnchorPoint->SetVolumetricScatteringIntensity(0.05f);
		LightAnchorPoint->SetIndirectLightingIntensity(0.3f);
		LightAnchorPoint->SetCastShadows(false);
		LightAnchorPoint->SetVisibility(On[4]);
		LightAnchorPoint->SetHiddenInGame(!On[4]);
	}

	// Cabin: warm interior puddle under the coachroof
	if (LightCabinPoint)
	{
		LightCabinPoint->SetRelativeLocation(PosCabin);
		LightCabinPoint->SetLightColor(Colors[7]);
		LightCabinPoint->SetIntensityUnits(ELightUnits::Candelas);
		// Stronger when on so glass emissive + point light both read at night.
		LightCabinPoint->SetIntensity(On[7] ? 48.f * VisScale : 0.f);
		LightCabinPoint->SetAttenuationRadius(1100.f);
		LightCabinPoint->SetSourceRadius(22.f);
		LightCabinPoint->SetSoftSourceRadius(40.f);
		LightCabinPoint->SetSpecularScale(0.06f);
		LightCabinPoint->SetVolumetricScatteringIntensity(0.03f);
		LightCabinPoint->SetIndirectLightingIntensity(0.65f);
		LightCabinPoint->SetCastShadows(false);
		LightCabinPoint->SetVisibility(On[7]);
		LightCabinPoint->SetHiddenInGame(!On[7]);
	}

	// Opaque glass can't transmit the point light — bake warm emissive so
	// cabin light "shows through" the coachroof windows (MI_Yacht_Glass).
	if (LoftMesh)
	{
		const FLinearColor DarkGlass(0.08f, 0.12f, 0.16f, 1.f);
		const FLinearColor WarmGlass(1.00f, 0.72f, 0.42f, 1.f);
		const FLinearColor CabinShellOff(0.94f, 0.95f, 0.96f, 1.f);
		const FLinearColor CabinShellLit(1.00f, 0.88f, 0.72f, 1.f);
		const float GlassEm = On[7] ? FMath::Lerp(0.4f, 3.0f, VisScale) : 0.008f;
		const float ShellEm = On[7] ? FMath::Lerp(0.1f, 0.6f, VisScale) : 0.f;
		const FLinearColor GlassCol = On[7]
			? FLinearColor::LerpUsingHSV(DarkGlass, WarmGlass, FMath::Lerp(0.5f, 0.95f, VisScale))
			: DarkGlass;
		const FLinearColor ShellCol = On[7]
			? FLinearColor::LerpUsingHSV(CabinShellOff, CabinShellLit, FMath::Lerp(0.3f, 0.75f, VisScale))
			: CabinShellOff;

		const int32 NumMats = LoftMesh->GetNumMaterials();
		for (int32 Mi = 0; Mi < NumMats; ++Mi)
		{
			UMaterialInterface* Base = LoftMesh->GetMaterial(Mi);
			if (!Base) continue;
			const FString Path = Base->GetPathName();
			const bool bIsGlass = Path.Contains(TEXT("Glass")) || Path.Contains(TEXT("Window"));
			const bool bIsCabin = Path.Contains(TEXT("Cabin"));
			if (!bIsGlass && !bIsCabin) continue;

			UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Base);
			if (!Mid)
			{
				Mid = LoftMesh->CreateAndSetMaterialInstanceDynamic(Mi);
			}
			if (!Mid) continue;

			if (bIsGlass)
			{
				Mid->SetVectorParameterValue(TEXT("BaseColor"), GlassCol);
				Mid->SetVectorParameterValue(TEXT("Color"), GlassCol);
				Mid->SetVectorParameterValue(TEXT("EmissiveColor"), WarmGlass * GlassEm);
				Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), GlassEm);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), GlassEm);
				Mid->SetScalarParameterValue(TEXT("Emissive"), GlassEm);
				Mid->SetScalarParameterValue(TEXT("Roughness"), On[7] ? 0.12f : 0.05f);
			}
			else
			{
				Mid->SetVectorParameterValue(TEXT("BaseColor"), ShellCol);
				Mid->SetVectorParameterValue(TEXT("Color"), ShellCol);
				Mid->SetVectorParameterValue(TEXT("EmissiveColor"), WarmGlass * ShellEm);
				Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), ShellEm);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), ShellEm);
				Mid->SetScalarParameterValue(TEXT("Emissive"), ShellEm);
			}
		}
	}
}

void ASailBoatPawn::SetBreakerNav(bool bOn)
{
	bBreakerNav = bOn;
	// Mutual exclusion with anchor (COLREGS: not both under way + at anchor)
	if (bOn && bBreakerAnchor) bBreakerAnchor = false;
	UpdateBoatLights();
}

void ASailBoatPawn::SetBreakerSteaming(bool bOn)
{
	bBreakerSteaming = bOn;
	if (bOn && bBreakerAnchor) bBreakerAnchor = false;
	UpdateBoatLights();
}

void ASailBoatPawn::SetBreakerAnchor(bool bOn)
{
	bBreakerAnchor = bOn;
	if (bOn)
	{
		// Anchor light alone while at anchor
		bBreakerNav = false;
		bBreakerSteaming = false;
	}
	UpdateBoatLights();
}

void ASailBoatPawn::SetBreakerDeck(bool bOn)
{
	bBreakerDeck = bOn;
	UpdateBoatLights();
}

void ASailBoatPawn::SetBreakerCabin(bool bOn)
{
	bBreakerCabin = bOn;
	UpdateBoatLights();
}

void ASailBoatPawn::SetAllBreakersOff()
{
	bBreakerNav = bBreakerSteaming = bBreakerAnchor = bBreakerDeck = bBreakerCabin = false;
	UpdateBoatLights();
}

void ASailBoatPawn::ApplyCachedSailingToDynamics()
{
	// Prefer ActiveSpec (catalog + live scales). Seed from JSON when available.
	if (bApplyJsonSailingParams && CachedSailingParams.bValid)
	{
		const FBoatJsonSailingParams& S = CachedSailingParams;
		ActiveSpec.Loa = S.LoaFt > 0.f ? S.LoaFt : ActiveSpec.Loa;
		ActiveSpec.Lwl = S.LwlFt > 0.f ? S.LwlFt : ActiveSpec.Lwl;
		ActiveSpec.Beam = S.BeamFt > 0.f ? S.BeamFt : ActiveSpec.Beam;
		ActiveSpec.Draft = S.DraftFt > 0.f ? S.DraftFt : ActiveSpec.Draft;
		ActiveSpec.DispLb = S.DispLb > 0.f ? S.DispLb : ActiveSpec.DispLb;
		ActiveSpec.BallastLb = S.BallastLb > 0.f ? S.BallastLb : ActiveSpec.BallastLb;
		ActiveSpec.SailArea = S.SaTotal > 0.f ? S.SaTotal : ActiveSpec.SailArea;
	}
	ApplyActiveSpecToBoat();
}

void ASailBoatPawn::ApplyActiveSpecToBoat()
{
	const float Loa = ActiveSpec.EffectiveLoa();
	const float Lwl = ActiveSpec.EffectiveLwl();
	const float Beam = ActiveSpec.EffectiveBeam();
	const float Draft = ActiveSpec.EffectiveDraft();
	const float Disp = ActiveSpec.EffectiveDisp();
	const float SA = ActiveSpec.EffectiveSail();
	const float Ballast = ActiveSpec.BallastLb * ActiveSpec.ScaleLoa * ActiveSpec.ScaleBeam * ActiveSpec.ScaleDraft;
	const float HullSpeed = 1.34f * FMath::Sqrt(FMath::Max(Lwl, 1.f));
	const float MastTop = ActiveSpec.I * ActiveSpec.ScaleLoa;
	const float LatArea = FMath::Max(20.f, Lwl * Draft * 0.85f);
	const float KeelArea = FMath::Max(8.f, Draft * Beam * 0.35f);
	const float KeelSpan = Draft * 0.9f;
	const float RudArea = FMath::Max(2.f, KeelArea * 0.12f);
	const float Tc = FMath::Max(0.5f, Draft * 0.18f);
	const float Gm = FMath::Clamp(Beam * 0.38f, 3.5f, 6.5f);

	const float PrevHdg = Dynamics.Heading;
	const float PrevAuto = Dynamics.AutoTarget;
	const bool bWasAuto = Dynamics.bAutoHeading;
	const float PrevSheet = Dynamics.SheetEase;
	const float PrevTws = Dynamics.TrueWindSpeedKn;
	const float PrevTwd = Dynamics.TrueWindDirDeg;

	Dynamics.ApplySailingParams(
		Disp, Ballast, Beam, Lwl, Draft, Tc,
		LatArea, KeelArea, RudArea, KeelSpan,
		-1.f, -Draft * 0.45f, SA, HullSpeed, Gm,
		Loa, MastTop);

	Dynamics.Heading = PrevHdg > 0.f ? PrevHdg : 90.f;
	Dynamics.AutoTarget = PrevAuto > 0.f ? PrevAuto : Dynamics.Heading;
	Dynamics.bAutoHeading = bWasAuto;
	Dynamics.SheetEase = PrevSheet;
	Dynamics.TrueWindSpeedKn = PrevTws;
	Dynamics.TrueWindDirDeg = PrevTwd;

	HullLengthCm = Loa * 30.48f;
	HullBeamCm = Beam * 30.48f;

	// Visual scale of loft relative to JSON (JSON is 1.0 catalog). Absolute, not cumulative.
	const float MeshScale = ActiveSpec.MeshUniformScale();
	const FVector MeshS(MeshScale, MeshScale, MeshScale);
	if (LoftMesh) LoftMesh->SetRelativeScale3D(MeshS);
	if (AppendagesMesh) AppendagesMesh->SetRelativeScale3D(MeshS);
	if (MainSailMesh)
	{
		MainSailMesh->SetRelativeScale3D(MeshS);
		// Pivot at scaled mast base so sail-local verts map to S * boat-space loft points.
		if (bMastPivotValid) MainSailMesh->SetRelativeLocation(MastBaseLoc * MeshScale);
	}
	if (JibSailMesh)
	{
		JibSailMesh->SetRelativeScale3D(MeshS);
		if (bMastPivotValid) JibSailMesh->SetRelativeLocation(MastBaseLoc * MeshScale);
	}
	// Re-place spars at scaled endpoints (absolute sizes)
	if (bMastPivotValid && MastMesh)
	{
		PlaceSparFromEndpoints(MastMesh, MastBaseLoc * MeshScale,
			MastBaseLoc * MeshScale + FVector(0.f, 0.f, FMath::Max(100.f, ActiveSpec.I * 30.48f * MeshScale)));
	}
	if (bBoomEndpointsValid && BoomMesh)
	{
		PlaceSparFromEndpoints(BoomMesh, BoomBaseLoc * MeshScale, BoomEndLoc * MeshScale);
	}
	UpdateHullCollisionFromMesh();
	// Collision box should track mesh scale
	if (HullCollision)
	{
		HullCollision->SetRelativeScale3D(MeshS);
	}
	RefreshChaseCamera();

	UE_LOG(LogSailSim, Log,
		TEXT("Spec %s: LOA=%.1fft (x%.2f) Beam=%.1f Draft=%.1f Disp=%.0f SA=%.0f meshScale=%.2f"),
		*ActiveSpec.Name, Loa, ActiveSpec.ScaleLoa, Beam, Draft, Disp, SA, MeshScale);
}

void ASailBoatPawn::SetLiveLoaScale(float Scale)
{
	ActiveSpec.ScaleLoa = FMath::Clamp(Scale, 0.5f, 1.6f);
	// Keep beam/draft proportional-ish for a simple "size" slider
	ActiveSpec.ScaleBeam = FMath::Lerp(1.f, ActiveSpec.ScaleLoa, 0.65f);
	ActiveSpec.ScaleDraft = FMath::Lerp(1.f, ActiveSpec.ScaleLoa, 0.55f);
	ActiveSpec.ScaleSail = FMath::Square(FMath::Lerp(1.f, ActiveSpec.ScaleLoa, 0.85f));
	ApplyActiveSpecToBoat();
	// Rebuild cloth rest pose under new scale
	EnsureSailClothBuilt(/*bForceRebuild*/ true);
}

void ASailBoatPawn::AdjustLiveLoaScale(float Delta)
{
	SetLiveLoaScale(ActiveSpec.ScaleLoa + Delta);
	if (bAutoOracleOnScale)
	{
		RebuildLoftFromOracle();
	}
}

FString ASailBoatPawn::GetSpecSummary() const
{
	return FString::Printf(
		TEXT("%s  LOA %.1f′  Bm %.1f′  Dr %.1f′  SA %.0f  ×%.2f%s"),
		*ActiveSpec.Name,
		ActiveSpec.EffectiveLoa(),
		ActiveSpec.EffectiveBeam(),
		ActiveSpec.EffectiveDraft(),
		ActiveSpec.EffectiveSail(),
		ActiveSpec.ScaleLoa,
		ActiveSpec.bLiveLoftGeometry ? TEXT(" live") : TEXT(""));
}

bool ASailBoatPawn::RebuildLoftFromOracle()
{
	FString Err;
	if (!FBoatLoftOracle::RebuildLiveLoft(ActiveSpec, &Err))
	{
		UE_LOG(LogSailSim, Warning, TEXT("RebuildLoftFromOracle failed: %s"), *Err);
		return false;
	}

	// Bake scales into base dims; geometry now matches effective size.
	ActiveSpec.BakeScalesIntoBase();
	ActivePresetId = ActiveSpec.Id;
	BoatJsonRelativePath = FBoatLoftOracle::LiveBoatJsonRelative();
	bLoftMeshLoaded = false;
	LoadedLoftPath.Reset();
	LoadLoftMesh(/*bApplyDynamics*/ true);
	// Reset visual scale to 1 (geometry is absolute)
	const FVector One(1.f, 1.f, 1.f);
	if (LoftMesh) LoftMesh->SetRelativeScale3D(One);
	if (AppendagesMesh) AppendagesMesh->SetRelativeScale3D(One);
	if (MainSailMesh) MainSailMesh->SetRelativeScale3D(One);
	if (JibSailMesh) JibSailMesh->SetRelativeScale3D(One);
	if (HullCollision) HullCollision->SetRelativeScale3D(One);
	ApplyActiveSpecToBoat();
	EnsureSailClothBuilt(/*bForceRebuild*/ true);

	SnapToWaterSurface(false);
	UE_LOG(LogSailSim, Log, TEXT("Live loft loaded: %s"), *BoatJsonRelativePath);
	return bLoftMeshLoaded;
}

void ASailBoatPawn::LoadLoftMesh(bool bApplyDynamics)
{
	if (!LoftMesh)
	{
		return;
	}

	const FString Path = FPaths::ProjectContentDir() / BoatJsonRelativePath;

	// Skip full mesh rebuild if already loaded for this path (PIE: OnConstruction then BeginPlay).
	// Bump LoftGeomVersion whenever hull geometry/paint layout changes so old caches rebuild.
	static constexpr int32 LoftGeomVersion = 3; // v3: keel/rudder on no-shadow appendages mesh
	static int32 LoadedLoftGeomVersion = -1;
	if (bLoftMeshLoaded && LoadedLoftPath == Path && LoadedLoftGeomVersion == LoftGeomVersion)
	{
		ApplyPlayerHullPaint(LoftMesh);
		if (bApplyDynamics)
		{
			if (!CachedSailingParams.bValid)
			{
				FBoatJsonLoadResult Meta;
				if (FBoatMeshFromJson::LoadMetadataOnly(Path, Meta))
				{
					CachedSailingParams = Meta.Sailing;
					CachedRigging = Meta.Rigging;
				}
			}
			ApplyCachedSailingToDynamics();
		}
		// Critical: early-return used to skip cloth forever if OnConstruction
		// built mesh without a successful cloth init (or after Clear).
		EnsureSailClothBuilt(/*bForceRebuild*/ false);
		return;
	}
	LoadedLoftGeomVersion = LoftGeomVersion;

	// Prefer hull paint (local-Z stripes); then solid gelcoat; then BasicShape.
	UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_HullPaint.MI_Yacht_HullPaint"));
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_Gelcoat.MI_Yacht_Gelcoat"));
	}
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR.M_Yacht_PBR"));
	}
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}

	FBoatJsonLoadResult Result;
	const bool bOk = FBoatMeshFromJson::LoadIntoProceduralMesh(
		LoftMesh, Path, BaseMat, &Result, MainSailMesh, JibSailMesh,
		/*bSkipSailSections*/ false, AppendagesMesh);
	if (!bOk)
	{
		bLoftMeshLoaded = false;
		LoadedLoftPath.Reset();
		UE_LOG(LogSailSim, Warning, TEXT("SailBoatPawn: loft JSON failed (%s) — no hull mesh"), *Path);
		return;
	}

	bLoftMeshLoaded = true;
	LoadedLoftPath = Path;
	CachedSailingParams = Result.Sailing;
	CachedRigging = Result.Rigging;
	LoftMesh->SetVisibility(true);
	LoftMesh->SetHiddenInGame(false);
	LoftMesh->MarkRenderStateDirty();
	// Player hull: navy topsides + red cove stripe (HullPaint local-Z bands).
	ApplyPlayerHullPaint(LoftMesh);
	if (AppendagesMesh)
	{
		// Re-assert no shadows after load (PrepMesh / cook can re-enable flags).
		AppendagesMesh->SetCastShadow(false);
		AppendagesMesh->bCastContactShadow = false;
		AppendagesMesh->bCastDynamicShadow = false;
		AppendagesMesh->bCastStaticShadow = false;
		AppendagesMesh->bCastVolumetricTranslucentShadow = false;
		AppendagesMesh->bCastInsetShadow = false;
		AppendagesMesh->bSelfShadowOnly = false;
		AppendagesMesh->SetVisibility(true);
		AppendagesMesh->SetHiddenInGame(false);
		AppendagesMesh->MarkRenderStateDirty();
	}
	if (MainSailMesh)
	{
		MainSailMesh->SetVisibility(true);
		MainSailMesh->SetHiddenInGame(false);
		MainSailMesh->MarkRenderStateDirty();
	}
	if (JibSailMesh)
	{
		JibSailMesh->SetVisibility(true);
		JibSailMesh->SetHiddenInGame(false);
		JibSailMesh->MarkRenderStateDirty();
	}

	if (Result.Spars.bMastValid)
	{
		MastBaseLoc = Result.Spars.MastBase;
		bMastPivotValid = true;
		PlaceSparFromEndpoints(MastMesh, Result.Spars.MastBase, Result.Spars.MastTop);
	}
	else
	{
		bMastPivotValid = false;
	}
	if (Result.Spars.bBoomValid)
	{
		BoomBaseLoc = Result.Spars.BoomBase;
		BoomEndLoc = Result.Spars.BoomEnd;
		// Hard-in = centerline: same length in XY, Y locked to gooseneck (true centerline).
		{
			const FVector D = BoomEndLoc - BoomBaseLoc;
			const float LenXY = FMath::Max(1.f, FVector2D(D.X, D.Y).Size());
			// Aft is −X in loft; keep Z (boom height).
			BoomEndCenterlineLoc = BoomBaseLoc + FVector(-LenXY, 0.f, D.Z);
		}
		bBoomEndpointsValid = true;
		PlaceSparFromEndpoints(BoomMesh, BoomBaseLoc, BoomEndCenterlineLoc);
	}
	else
	{
		bBoomEndpointsValid = false;
	}
	UpdateBoomFromSheet();

	EnsureSailClothBuilt(/*bForceRebuild*/ true);
	EnsureSpinAndSpritBuilt(/*bForceRebuild*/ true);
	EnsureRunningRiggingBuilt();
	UpdateRunningRigging();
	UpdateStandingRigging();
	EnsureBoatLightsBuilt();
	UpdateBoatLights();
	UpdateHullCollisionFromMesh();

	// Aluminum spars — yacht MI when available, else tint BasicShape.
	if (UMaterialInterface* SparMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_Spar.MI_Yacht_Spar")))
	{
		if (MastMesh && MastMesh->IsVisible()) MastMesh->SetMaterial(0, SparMat);
		if (BoomMesh && BoomMesh->IsVisible()) BoomMesh->SetMaterial(0, SparMat);
		if (BowspritMesh && BowspritMesh->IsVisible()) BowspritMesh->SetMaterial(0, SparMat);
	}
	else
	{
		auto Tint = [](UStaticMeshComponent* Comp, FLinearColor Color)
		{
			if (!Comp || !Comp->IsVisible()) return;
			UMaterialInterface* Base = Comp->GetMaterial(0);
			if (!Base) return;
			UMaterialInstanceDynamic* Mid = Comp->CreateAndSetMaterialInstanceDynamic(0);
			if (!Mid) return;
			Mid->SetVectorParameterValue(TEXT("Color"), Color);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), Color);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 1.f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.28f);
		};
		const FLinearColor Al(0.913f, 0.921f, 0.925f);
		Tint(MastMesh, Al);
		Tint(BoomMesh, Al);
	}

	if (bApplyDynamics)
	{
		ApplyCachedSailingToDynamics();
	}
}

void ASailBoatPawn::ApplyPlayerHullPaint(UProceduralMeshComponent* HullMesh)
{
	if (!HullMesh) return;
	// Freshly polished navy gelcoat — dark but not pure black so sky/water speculars read.
	// Real Awlgrip/gelcoat: sharp clearcoat, soft environment mirror of water & sky.
	const FLinearColor Navy = FLinearColor::FromSRGBColor(FColor(10, 34, 78));
	const FLinearColor RedStripe = FLinearColor::FromSRGBColor(FColor(196, 28, 32));
	const FLinearColor Boot = FLinearColor::FromSRGBColor(FColor(10, 12, 18));
	const FLinearColor Antifoul = FLinearColor::FromSRGBColor(FColor(42, 28, 22));

	auto IsHullPaintMat = [](UMaterialInterface* M) -> bool
	{
		if (!M) return false;
		for (UMaterialInterface* Walk = M; Walk; )
		{
			const FString Name = Walk->GetName();
			if (Name.Contains(TEXT("HullPaint")) || Name.Contains(TEXT("Gelcoat")))
			{
				return true;
			}
			if (const UMaterialInstance* MI = Cast<UMaterialInstance>(Walk))
			{
				Walk = MI->Parent;
			}
			else
			{
				break;
			}
		}
		return false;
	};

	const int32 N = HullMesh->GetNumMaterials();
	for (int32 I = 0; I < N; ++I)
	{
		UMaterialInterface* M = HullMesh->GetMaterial(I);
		if (!IsHullPaintMat(M)) continue;

		UMaterialInstanceDynamic* Mid = HullMesh->CreateAndSetMaterialInstanceDynamic(I);
		if (!Mid) continue;
		Mid->SetVectorParameterValue(TEXT("ColorTopsides"), Navy);
		Mid->SetVectorParameterValue(TEXT("ColorStripe"), RedStripe);
		Mid->SetVectorParameterValue(TEXT("ColorBoot"), Boot);
		Mid->SetVectorParameterValue(TEXT("ColorAntifoul"), Antifoul);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Navy);
		Mid->SetVectorParameterValue(TEXT("Color"), Navy);
		// Cove stripe band height (local Z cm on loft hull)
		Mid->SetScalarParameterValue(TEXT("ZStripeLo"), 50.f);
		Mid->SetScalarParameterValue(TEXT("ZStripeHi"), 66.f);

		// Painted hull — soft specular, no wet-varnish clearcoat layer.
		Mid->SetScalarParameterValue(TEXT("RoughTopsides"), 0.14f);
		Mid->SetScalarParameterValue(TEXT("Roughness"), 0.14f);
		Mid->SetScalarParameterValue(TEXT("RoughStripe"), 0.16f);
		Mid->SetScalarParameterValue(TEXT("RoughBoot"), 0.24f);
		Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.f);
		Mid->SetScalarParameterValue(TEXT("ClearCoatRoughness"), 1.f);
		Mid->SetScalarParameterValue(TEXT("ClearCoatBoost"), 0.f);
		Mid->SetScalarParameterValue(TEXT("Specular"), 0.42f);
		Mid->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
	}
}

void ASailBoatPawn::EnsureOpenWaterSpawn()
{
	SnapToWaterSurface(/*bForceXY*/ bForceOpenWaterSpawn ||
		FVector2D(GetActorLocation().X, GetActorLocation().Y).Size() < OriginIslandRadiusCm);
}

void ASailBoatPawn::SnapToWaterSurface(bool bForceXY)
{
	FVector Loc = GetActorLocation();
	if (bForceXY)
	{
		// Always use harbor geo (or latest OpenWaterSpawnXY if designer overrode)
		if (OpenWaterSpawnXY.IsNearlyZero())
		{
			OpenWaterSpawnXY = FNavGeo::BoatStartWorldCm2D();
		}
		Loc.X = OpenWaterSpawnXY.X;
		Loc.Y = OpenWaterSpawnXY.Y;
	}

	FVector Surf, Norm;
	if (SampleWaterSurface(Loc, Surf, Norm))
	{
		SmoothedWaterZ = Surf.Z;
		Loc.Z = Surf.Z + WaterlineOffsetCm;
		bFloatInit = true;
		VerticalVelZ = 0.f;
	}
	else
	{
		SmoothedWaterZ = WaterSurfaceZ;
		Loc.Z = WaterSurfaceZ + WaterlineOffsetCm;
	}
	SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
}

void ASailBoatPawn::EnsureOceanCoverage()
{
	if (!bEnsureOceanCoverage) return;
	UWorld* World = GetWorld();
	if (!World) return;

	// Phase 5: one flat UE Water plane under boat → horizon (no Gerstner).
	if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		FSeaParams Sea = Ocean->GetSeaParams();
		Sea.WindSpeedKn = Dynamics.TrueWindSpeedKn;
		Sea.WindDirDeg = Dynamics.TrueWindDirDeg;
		Sea.AmplitudeCm = 0.f;
		Sea.Choppiness = 0.f;
		Ocean->SetSeaParams(Sea);
		Ocean->PrepareOpenOcean(
			GetActorLocation(), WaterZoneExtentCm, LocalWaterTessellationDiameterCm);
	}
}

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();

	// Wake is disabled — destroy any leftover patch from prior hot-reload / PIE /
	// stale base dylib that still spawned BoatWakeMesh components.
	bEnableWake = false;
	BoatWake.bEnabled = false;
	BoatWake.Clear();
	{
		TArray<UActorComponent*> Comps;
		GetComponents(Comps);
		for (UActorComponent* C : Comps)
		{
			if (!C) continue;
			const FString N = C->GetName();
			if (N.Contains(TEXT("BoatWake")) || N.Contains(TEXT("WakeMesh")))
			{
				if (UProceduralMeshComponent* PMC = Cast<UProceduralMeshComponent>(C))
				{
					PMC->ClearAllMeshSections();
					PMC->SetVisibility(false);
					PMC->SetHiddenInGame(true);
				}
				C->DestroyComponent();
			}
		}
	}

	// BeginPlay runs before Possess for default pawns — never destroy here.
	// Orphans (map/WP) self-destroy in Tick after OrphanGraceFrames if still unpossessed.
	const bool bGame = GetWorld() && GetWorld()->IsGameWorld() && !GetWorld()->IsPreviewWorld();
	if (bGame && !bPlayerSessionBoat && !IsPlayerControlled())
	{
		OrphanGraceFrames = 10;
		// Light path: still load mesh so brief WP flash is consistent, but hide until destroyed.
		LoadLoftMesh(/*bApplyDynamics*/ false);
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
		UE_LOG(LogSailSim, Log, TEXT("Pending orphan boat %s (destroy if never possessed)"), *GetName());
		return;
	}

	// Defaults first; LoadLoftMesh may override from boat3d sailing block.
	Dynamics.InitJ105();
	// Harbor dock heading (web NAV_BOAT_START.heading = 90° = east)
	Dynamics.Heading = FNavGeo::BoatStartHeadingDeg;
	Dynamics.AutoTarget = Dynamics.Heading;
	OpenWaterSpawnXY = FNavGeo::BoatStartWorldCm2D();
	if (ActiveSpec.Id.IsEmpty())
	{
		ActiveSpec = FBoatSpec::FromPresetId(ActivePresetId.IsEmpty() ? TEXT("j105") : ActivePresetId);
	}

	LoadLoftMesh(/*bApplyDynamics*/ true);
	// Always re-verify cloth after load (handles PIE mesh-cache early return)
	EnsureSailClothBuilt(/*bForceRebuild*/ !MainCloth.bInitialized || !JibCloth.bInitialized);

	// Restore sail trim / wind / sky from disk AFTER InitJ105 + loft (which reset
	// defaults). Chrome also applies for map size; both paths share SailSimUserPrefs.
	if (bPlayerSessionBoat)
	{
		FSailSimUserPrefs Prefs;
		Prefs.Load();
		Prefs.Apply(this, GetWorld());
	}

	EnsureOceanCoverage(); // move water zone under harbor spawn
	SnapToWaterSurface(/*bForceXY*/ true);
	// Stream land tiles + ENC aids immediately around harbor (don't wait for first tick)
	if (UWorld* World = GetWorld())
	{
		if (UNantucketTerrainSubsystem* Terr = World->GetSubsystem<UNantucketTerrainSubsystem>())
		{
			Terr->ForceStreamAround(GetActorLocation());
		}
		if (UNantucketStructuresSubsystem* Structs = World->GetSubsystem<UNantucketStructuresSubsystem>())
		{
			Structs->ForceStreamAround(GetActorLocation());
		}
		if (UMooredBoatSubsystem* Moored = World->GetSubsystem<UMooredBoatSubsystem>())
		{
			Moored->ForceStreamAround(GetActorLocation());
		}
		if (UEncAidSubsystem* Aids = World->GetSubsystem<UEncAidSubsystem>())
		{
			Aids->ForceStreamAround(GetActorLocation());
		}
	}
	// Re-center water after snap (XY is now definitive harbor position)
	EnsureOceanCoverage();
	WaterSnapFrames = 12;

	{
		const FVector Loc = GetActorLocation();
		FVector Surf, Norm;
		if (SampleWaterSurface(Loc, Surf, Norm))
		{
			FVector Surf2, Norm2;
			const bool b2 = SampleWaterSurface(Loc + FVector(400.f, 0.f, 0.f), Surf2, Norm2);
			UE_LOG(LogSailSim, Log,
				TEXT("SailBoatPawn: water at XY(%.0f,%.0f) Z=%.1f  neighbor dZ=%.1f (waves %s)"),
				Loc.X, Loc.Y, Surf.Z,
				b2 ? (Surf2.Z - Surf.Z) : 0.f,
				(b2 && FMath::Abs(Surf2.Z - Surf.Z) > 0.5f) ? TEXT("OK") : TEXT("flat/fallback"));
		}
	}

	CameraLagEnableFrames = 8;
	RefreshChaseCamera();
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetViewTarget(this);
	}

	static bool bGoldenRan = false;
	if (!bGoldenRan && bPlayerSessionBoat)
	{
		bGoldenRan = true;
		FBoatDynamics::RunGoldenSelfCheck();
	}
}

void ASailBoatPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ASailBoatPawn::OnMoveRight);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &ASailBoatPawn::OnMoveRight);
	PlayerInputComponent->BindAxis(TEXT("Sheet"), this, &ASailBoatPawn::OnSheetAxis);
	PlayerInputComponent->BindAxis(TEXT("Outhaul"), this, &ASailBoatPawn::OnOuthaulAxis);
	PlayerInputComponent->BindAxis(TEXT("Vang"), this, &ASailBoatPawn::OnVangAxis);
	PlayerInputComponent->BindAxis(TEXT("JibCar"), this, &ASailBoatPawn::OnJibCarAxis);
	PlayerInputComponent->BindAxis(TEXT("JibLeech"), this, &ASailBoatPawn::OnJibLeechAxis);
	PlayerInputComponent->BindAxis(TEXT("LookYaw"), this, &ASailBoatPawn::OnLookYaw);
	PlayerInputComponent->BindAxis(TEXT("LookPitch"), this, &ASailBoatPawn::OnLookPitch);
	PlayerInputComponent->BindAxis(TEXT("CameraZoom"), this, &ASailBoatPawn::OnCameraZoom);
	PlayerInputComponent->BindAction(TEXT("OrbitCamera"), IE_Pressed, this, &ASailBoatPawn::OnOrbitPressed);
	PlayerInputComponent->BindAction(TEXT("OrbitCamera"), IE_Released, this, &ASailBoatPawn::OnOrbitReleased);
	PlayerInputComponent->BindAction(TEXT("ToggleSailing"), IE_Pressed, this, &ASailBoatPawn::OnToggleSailing);
	PlayerInputComponent->BindAction(TEXT("WindSpeedUp"), IE_Pressed, this, &ASailBoatPawn::OnWindSpeedUp);
	PlayerInputComponent->BindAction(TEXT("WindSpeedDown"), IE_Pressed, this, &ASailBoatPawn::OnWindSpeedDown);
	PlayerInputComponent->BindAction(TEXT("WindDirLeft"), IE_Pressed, this, &ASailBoatPawn::OnWindDirLeft);
	PlayerInputComponent->BindAction(TEXT("WindDirRight"), IE_Pressed, this, &ASailBoatPawn::OnWindDirRight);
	PlayerInputComponent->BindAction(TEXT("Preset1"), IE_Pressed, this, &ASailBoatPawn::OnPreset1);
	PlayerInputComponent->BindAction(TEXT("Preset2"), IE_Pressed, this, &ASailBoatPawn::OnPreset2);
	PlayerInputComponent->BindAction(TEXT("Preset3"), IE_Pressed, this, &ASailBoatPawn::OnPreset3);
	PlayerInputComponent->BindAction(TEXT("Preset4"), IE_Pressed, this, &ASailBoatPawn::OnPreset4);
	PlayerInputComponent->BindAction(TEXT("LoaScaleUp"), IE_Pressed, this, &ASailBoatPawn::OnLoaScaleUp);
	PlayerInputComponent->BindAction(TEXT("LoaScaleDown"), IE_Pressed, this, &ASailBoatPawn::OnLoaScaleDown);
	PlayerInputComponent->BindAction(TEXT("RebuildLoft"), IE_Pressed, this, &ASailBoatPawn::OnRebuildLoft);
}

void ASailBoatPawn::ApplyOrbitToSpringArm()
{
	if (!SpringArm) return;
	OrbitPitchDeg = FMath::Clamp(OrbitPitchDeg, MinOrbitPitchDeg, MaxOrbitPitchDeg);
	OrbitYawDeg = FMath::UnwindDegrees(OrbitYawDeg);
	// Pitch/Yaw only — never roll with the hull.
	SpringArm->SetRelativeRotation(FRotator(OrbitPitchDeg, OrbitYawDeg, 0.f));
}

void ASailBoatPawn::RefreshChaseCamera()
{
	if (!SpringArm) return;

	const float Arm = FMath::Clamp(
		HullLengthCm * CameraArmLengthLoaScale,
		FMath::Max(MinArmLengthCm, 2000.f),
		MaxArmLengthCm);
	SpringArm->TargetArmLength = Arm;
	SpringArm->bInheritPitch = false;
	SpringArm->bInheritYaw = true;
	SpringArm->bInheritRoll = false;
	SpringArm->bDoCollisionTest = false;
	// Lag off until after teleport/spawn so we don't start at PlayerStart origin.
	SpringArm->bEnableCameraLag = (CameraLagEnableFrames <= 0);
	SpringArm->CameraLagSpeed = 8.f;
	SpringArm->bEnableCameraRotationLag = false;
	// Lower chase: near deck height so the boat fills the frame without looking top-down.
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 120.f));
	SpringArm->SocketOffset = FVector(0.f, 0.f, 140.f);
	SpringArm->TargetOffset = FVector(0.f, 0.f, 80.f);

	// Default pitch ~−16° (horizon-ish). Only reset if still at zero / flat.
	if (FMath::Abs(OrbitPitchDeg) < 1.f)
	{
		OrbitPitchDeg = -16.f;
	}
	OrbitPitchDeg = FMath::Clamp(OrbitPitchDeg, MinOrbitPitchDeg, MaxOrbitPitchDeg);
	ApplyOrbitToSpringArm();

	if (Camera)
	{
		Camera->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
		Camera->bUsePawnControlRotation = false;
		Camera->SetActive(true);
	}

	// Verbose only — was spamming every StartupSkip frame
	UE_LOG(LogSailSim, Verbose, TEXT("Chase cam arm=%.0f cm pitch=%.1f yaw=%.1f LOA=%.0f"),
		Arm, OrbitPitchDeg, OrbitYawDeg, HullLengthCm);
}

void ASailBoatPawn::OnOrbitPressed()
{
	bOrbitRMBHeld = true;
}

void ASailBoatPawn::OnOrbitReleased()
{
	bOrbitRMBHeld = false;
}

void ASailBoatPawn::OnLookYaw(float Value)
{
	if (FMath::IsNearlyZero(Value)) return;
	if (bRequireRMBToOrbit && !bOrbitRMBHeld) return;
	OrbitYawDeg += Value * OrbitYawSpeed;
	ApplyOrbitToSpringArm();
}

void ASailBoatPawn::OnLookPitch(float Value)
{
	if (FMath::IsNearlyZero(Value)) return;
	if (bRequireRMBToOrbit && !bOrbitRMBHeld) return;
	// Drag mouse up → camera elevates (more negative pitch, top-down)
	OrbitPitchDeg -= Value * OrbitPitchSpeed;
	ApplyOrbitToSpringArm();
}

void ASailBoatPawn::OnCameraZoom(float Value)
{
	if (!SpringArm || FMath::IsNearlyZero(Value)) return;
	// Wheel up (positive) zooms in
	SpringArm->TargetArmLength = FMath::Clamp(
		SpringArm->TargetArmLength - Value * ZoomSpeedCm,
		MinArmLengthCm,
		MaxArmLengthCm);
}

void ASailBoatPawn::OnMoveRight(float Value)
{
	// Rate input only while held. Release freezes angle (sticky tiller).
	// Does NOT snap to ±35° — partial/held axis slews at HelmRateDegS.
	HelmAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::OnSheetAxis(float Value)
{
	// Hold W to sheet in, S to ease (rate-limited via continuous axis)
	SheetAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::OnOuthaulAxis(float Value)
{
	OuthaulAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::OnVangAxis(float Value)
{
	VangAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::OnJibCarAxis(float Value)
{
	JibCarAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::OnJibLeechAxis(float Value)
{
	JibLeechAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::SetHelmInput(float StarboardPositive)
{
	// Sticky held tiller angle. Disengages autopilot when |helm| > 0.5°
	// (web: hand on tiller). Does not re-engage when centered — use AUTO.
	HeldHelmStarboardDeg = FMath::Clamp(StarboardPositive, -35.f, 35.f);
	Dynamics.SetRudderStarboardPositive(HeldHelmStarboardDeg);
}

void ASailBoatPawn::SetAutoHeading(bool bEnabled)
{
	if (bEnabled)
	{
		// Pilot takes the helm — zero sticky tiller so hand-off is clean
		HeldHelmStarboardDeg = 0.f;
		// Web hEngageAutopilot(captureLive): NAV re-seeds the route from the chart.
		if (Dynamics.AutoMode == FBoatDynamics::EAutoMode::Nav)
		{
			if (!AcquireNavRoute())
			{
				UE_LOG(LogSailSim, Warning, TEXT("[nav] AUTO engage failed — no waypoints on chart"));
				Dynamics.DisengageAutopilot();
			}
			return;
		}
		Dynamics.EngageAutopilot(true);
	}
	else
	{
		// Stand by: leave tiller at current rudder so no jump when going manual
		HeldHelmStarboardDeg = -Dynamics.Rudder; // model ↔ starboard convention
		Dynamics.DisengageAutopilot();
	}
}

void ASailBoatPawn::ToggleAutoHeading()
{
	SetAutoHeading(!Dynamics.bAutoHeading);
}

void ASailBoatPawn::SetAutoTargetHeading(float Deg)
{
	Dynamics.AutoMode = FBoatDynamics::EAutoMode::Hdg;
	Dynamics.AutoTarget = FBoatDynamics::Wrap360(Deg);
	Dynamics.AutoI = 0.f;
	if (!Dynamics.bAutoHeading)
	{
		Dynamics.EngageAutopilot(false);
	}
}

void ASailBoatPawn::NudgeAutoTarget(float DeltaDeg)
{
	if (Dynamics.AutoMode == FBoatDynamics::EAutoMode::Nav)
	{
		return; // bearing is live from route
	}
	if (Dynamics.AutoMode == FBoatDynamics::EAutoMode::Awa)
	{
		SetAutoAwaTarget(Dynamics.AutoAwaTarget + DeltaDeg);
		return;
	}
	SetAutoTargetHeading(FBoatDynamics::Wrap360(Dynamics.AutoTarget + DeltaDeg));
}

void ASailBoatPawn::SelectAutoMode(uint8 ModeIndex)
{
	using EMode = FBoatDynamics::EAutoMode;
	const EMode Want = (ModeIndex == 1) ? EMode::Awa
		: (ModeIndex == 2) ? EMode::Nav
		: EMode::Hdg;

	// Click active engaged mode → stand by (web hSelectAutoMode).
	if (Dynamics.bAutoHeading && Dynamics.AutoMode == Want)
	{
		Dynamics.DisengageAutopilot();
		return;
	}

	if (Want == EMode::Nav)
	{
		// Engage NAV if the chart has a route; else leave standby with warning.
		if (!AcquireNavRoute())
		{
			UE_LOG(LogSailSim, Warning, TEXT("[nav] NAV mode needs at least one waypoint on the chart (+ WP)"));
			Dynamics.AutoMode = EMode::Nav;
			Dynamics.DisengageAutopilot();
			return;
		}
		return;
	}

	if (Want == EMode::Awa)
	{
		Dynamics.AutoMode = EMode::Awa;
		Dynamics.AutoAwaTarget = Dynamics.GetApparentWindAngleDeg();
		Dynamics.AutoI = 0.f;
		Dynamics.EngageAutopilot(false);
		return;
	}

	// HDG — capture live heading and hold.
	Dynamics.AutoMode = EMode::Hdg;
	Dynamics.AutoTarget = Dynamics.Heading;
	Dynamics.AutoI = 0.f;
	Dynamics.EngageAutopilot(false);
}

void ASailBoatPawn::SetAutoAwaTarget(float SignedDeg)
{
	Dynamics.SetAutoAwaTarget(SignedDeg);
}

void ASailBoatPawn::StartTack()
{
	Dynamics.StartTack();
}

void ASailBoatPawn::SetAutoTrim(bool bOn)
{
	Dynamics.SetAutoTrim(bOn);
}

void ASailBoatPawn::ToggleAutoTrim()
{
	Dynamics.SetAutoTrim(!Dynamics.bAutoTrim);
}

/** Resolve chart route store (same GI the mini-map writes into). */
static UNavWaypointSubsystem* SailSimGetNav(const AActor* Boat)
{
	if (!Boat) return nullptr;
	UGameInstance* GI = Boat->GetGameInstance();
	if (!GI && Boat->GetWorld())
	{
		GI = Boat->GetWorld()->GetGameInstance();
	}
	return GI ? GI->GetSubsystem<UNavWaypointSubsystem>() : nullptr;
}

/**
 * Bearing (deg) and distance (nm) from boat world XY (cm) to a lat/lon mark.
 * Same LatLonToWorldCm frame as boat spawn / motion (+X north, +Y east).
 */
static void SailSimNavRangeBearing(const FVector& BoatLocCm, double WpLat, double WpLon,
	double& OutDistNm, double& OutBrgDeg)
{
	double Wx = 0.0, Wy = 0.0;
	FNavGeo::LatLonToWorldCm(WpLat, WpLon, Wx, Wy);
	const double DxCm = Wx - double(BoatLocCm.X); // +X = north
	const double DyCm = Wy - double(BoatLocCm.Y); // +Y = east
	const double DistFt = FMath::Sqrt(DxCm * DxCm + DyCm * DyCm) / FNavGeo::CmPerFt;
	OutDistNm = DistFt / FNavGeo::FtPerNm;
	if (DistFt < 1e-3)
	{
		OutBrgDeg = 0.0;
		return;
	}
	// atan2(east, north) → 0° north, 90° east (matches Dynamics.Heading).
	OutBrgDeg = FMath::RadiansToDegrees(FMath::Atan2(DyCm, DxCm));
	if (OutBrgDeg < 0.0) OutBrgDeg += 360.0;
}

/**
 * Heading command so *course over ground* aims at the mark.
 * Aiming the bow at the WP leaves a leeward miss (Beta / Vsway crab).
 * With way on: AutoTarget = Heading + (Brg − COG)  →  equilibrium COG = Brg.
 * Nearly stopped: fall back to raw bearing (no reliable COG).
 */
static float SailSimNavHeadingCmd(const FBoatDynamics& Dyn, double BrgDeg)
{
	const float Brg = FBoatDynamics::Wrap360(static_cast<float>(BrgDeg));
	// Need a few tenths of a knot for COG to be meaningful.
	if (Dyn.V < 0.6f) // ft/s ≈ 0.35 kn
	{
		return Brg;
	}
	const float Hrad = FMath::DegreesToRadians(Dyn.Heading);
	const float CosH = FMath::Cos(Hrad);
	const float SinH = FMath::Sin(Hrad);
	// World velocity (same axes as ApplyDynamicsToTransform).
	const float Vx = Dyn.U * CosH - Dyn.Vsway * SinH; // north
	const float Vy = Dyn.U * SinH + Dyn.Vsway * CosH; // east
	const float Cog = FBoatDynamics::Wrap360(
		FMath::RadiansToDegrees(FMath::Atan2(Vy, Vx)));
	const float CogErr = FBoatDynamics::Wrap180(Brg - Cog);
	// Command a heading that cancels the COG error (includes leeway).
	return FBoatDynamics::Wrap360(Dyn.Heading + CogErr);
}

/** Arrival radius (nm). Fixed 0.01 nm ≈ 61 ft (~1.8 boat lengths on a J/105). */
static double SailSimNavArrivalNm(float /*SpeedKn*/)
{
	return 0.01;
}

FString ASailBoatPawn::GetAutoTargetDisplayText() const
{
	if (Dynamics.AutoMode == FBoatDynamics::EAutoMode::Awa)
	{
		const float A = Dynamics.AutoAwaTarget;
		const TCHAR Side = (A >= 0.f) ? TEXT('S') : TEXT('P');
		return FString::Printf(TEXT("%c%03.0f°"), Side, FMath::Abs(A));
	}
	if (Dynamics.AutoMode == FBoatDynamics::EAutoMode::Nav)
	{
		// Show true bearing to the mark (not leeway-adjusted helm command).
		float BrgShow = FBoatDynamics::Wrap360(Dynamics.AutoTarget);
		if (UNavWaypointSubsystem* Nav = SailSimGetNav(this))
		{
			FNavWaypoint Wp;
			if (Nav->GetWaypoint(Dynamics.NavWpIndex, Wp))
			{
				double Dist = 0.0, Brg = 0.0;
				SailSimNavRangeBearing(GetActorLocation(), Wp.Lat, Wp.Lon, Dist, Brg);
				BrgShow = FBoatDynamics::Wrap360(static_cast<float>(Brg));
			}
		}
		return FString::Printf(TEXT("WP%d %03.0f°"), Dynamics.NavWpIndex + 1, BrgShow);
	}
	return FString::Printf(TEXT("%03.0f°"), FBoatDynamics::Wrap360(Dynamics.AutoTarget));
}

bool ASailBoatPawn::AcquireNavRoute()
{
	UNavWaypointSubsystem* Nav = SailSimGetNav(this);
	if (!Nav || Nav->Num() == 0)
	{
		UE_LOG(LogSailSim, Warning, TEXT("[nav] AcquireNavRoute: no waypoints (Nav=%s Num=%d)"),
			Nav ? TEXT("ok") : TEXT("null"), Nav ? Nav->Num() : 0);
		return false;
	}

	// Always start at WP1 (index 0). Selected index defaults to last-added WP.
	const FVector Loc = GetActorLocation();
	constexpr double OnTopNm = 0.025; // only skip if literally under the keel
	int32 Idx = 0;
	while (Idx < Nav->Num())
	{
		FNavWaypoint W;
		if (!Nav->GetWaypoint(Idx, W)) break;
		double D = 0.0, B = 0.0;
		SailSimNavRangeBearing(Loc, W.Lat, W.Lon, D, B);
		if (D > OnTopNm) break;
		UE_LOG(LogSailSim, Log, TEXT("[nav] Acquire: already on WP%d (%.3fnm) — skip"), Idx + 1, D);
		++Idx;
	}
	if (Idx >= Nav->Num())
	{
		Dynamics.AutoMode = FBoatDynamics::EAutoMode::Hdg;
		Dynamics.AutoTarget = Dynamics.Heading;
		Dynamics.AutoI = 0.f;
		Dynamics.EngageAutopilot(false);
		Nav->SetSelectedIndex(Nav->Num() - 1);
		UE_LOG(LogSailSim, Log, TEXT("[nav] Acquire: all marks under keel — HDG hold"));
		return true;
	}

	FNavWaypoint Active;
	if (!Nav->GetWaypoint(Idx, Active)) return false;

	double DistNm = 0.0, BrgDeg = 0.0;
	SailSimNavRangeBearing(Loc, Active.Lat, Active.Lon, DistNm, BrgDeg);

	Dynamics.AutoMode = FBoatDynamics::EAutoMode::Nav;
	Dynamics.NavWpIndex = Idx;
	NavTrackIndex = Idx;
	NavTrackMinDistNm = static_cast<float>(DistNm);
	NavTrackInitialDistNm = static_cast<float>(DistNm);
	bNavApproachedActive = false;
	Nav->SetSelectedIndex(Idx);
	Dynamics.AutoTarget = SailSimNavHeadingCmd(Dynamics, BrgDeg);
	Dynamics.AutoI = 0.f;
	HeldHelmStarboardDeg = 0.f;
	Dynamics.EngageAutopilot(false);

	UE_LOG(LogSailSim, Log,
		TEXT("[nav] Acquire → WP%d/%d  dist=%.2fnm  brg=%.0f°  cmd=%.0f°  hdg=%.0f°  beta=%+.1f°  boatXY=(%.0f,%.0f)"),
		Idx + 1, Nav->Num(), DistNm, BrgDeg, Dynamics.AutoTarget, Dynamics.Heading, Dynamics.Beta,
		Loc.X, Loc.Y);
	return true;
}

void ASailBoatPawn::UpdateNavTarget()
{
	if (Dynamics.AutoMode != FBoatDynamics::EAutoMode::Nav) return;

	UNavWaypointSubsystem* Nav = SailSimGetNav(this);
	if (!Nav || Nav->Num() == 0)
	{
		Dynamics.AutoMode = FBoatDynamics::EAutoMode::Hdg;
		Dynamics.AutoTarget = Dynamics.Heading;
		Dynamics.AutoI = 0.f;
		UE_LOG(LogSailSim, Warning, TEXT("[nav] Route empty mid-nav — HDG hold"));
		return;
	}

	if (Dynamics.NavWpIndex < 0) Dynamics.NavWpIndex = 0;
	if (Dynamics.NavWpIndex >= Nav->Num())
	{
		Dynamics.AutoMode = FBoatDynamics::EAutoMode::Hdg;
		Dynamics.AutoTarget = Dynamics.Heading;
		Dynamics.AutoI = 0.f;
		return;
	}

	const FVector Loc = GetActorLocation();
	const double ArrivalNm = SailSimNavArrivalNm(Dynamics.GetSpeedKnots());

	// Web hNavUpdateTarget: at most one advance per tick (radial gate only).
	FNavWaypoint Wp;
	if (!Nav->GetWaypoint(Dynamics.NavWpIndex, Wp)) return;

	double DistNm = 0.0, BrgDeg = 0.0;
	SailSimNavRangeBearing(Loc, Wp.Lat, Wp.Lon, DistNm, BrgDeg);

	if (NavTrackIndex != Dynamics.NavWpIndex)
	{
		NavTrackIndex = Dynamics.NavWpIndex;
		NavTrackMinDistNm = static_cast<float>(DistNm);
		NavTrackInitialDistNm = static_cast<float>(DistNm);
		bNavApproachedActive = false;
	}
	else
	{
		NavTrackMinDistNm = FMath::Min(NavTrackMinDistNm, static_cast<float>(DistNm));
		// "Approached" = we closed range meaningfully (used only for diagnostics).
		if (DistNm < double(NavTrackInitialDistNm) * 0.9
			|| DistNm < double(NavTrackInitialDistNm) - 0.03)
		{
			bNavApproachedActive = true;
		}
	}

	// Advance only when inside the tight radial gate (web hNavUpdateTarget).
	// No abeam / early-capture — that was peeling off to the next WP too soon.
	const bool bArrived = DistNm <= ArrivalNm;

	if (bArrived)
	{
		const int32 Passed = Dynamics.NavWpIndex;
		++Dynamics.NavWpIndex;
		Dynamics.AutoI *= 0.5f;
		NavTrackIndex = INDEX_NONE;
		bNavApproachedActive = false;
		UE_LOG(LogSailSim, Log, TEXT("[nav] Arrived WP%d (%.3fnm ≤ %.3fnm) → aim WP%d / %d"),
			Passed + 1, DistNm, ArrivalNm, Dynamics.NavWpIndex + 1, Nav->Num());

		if (Dynamics.NavWpIndex >= Nav->Num())
		{
			Dynamics.AutoMode = FBoatDynamics::EAutoMode::Hdg;
			Dynamics.AutoTarget = Dynamics.Heading;
			Dynamics.AutoI = 0.f;
			Nav->SetSelectedIndex(Nav->Num() - 1);
			UE_LOG(LogSailSim, Log, TEXT("[nav] Route complete — HDG hold %.0f°"), Dynamics.Heading);
			return;
		}
		if (!Nav->GetWaypoint(Dynamics.NavWpIndex, Wp)) return;
		SailSimNavRangeBearing(Loc, Wp.Lat, Wp.Lon, DistNm, BrgDeg);
		NavTrackIndex = Dynamics.NavWpIndex;
		NavTrackMinDistNm = static_cast<float>(DistNm);
		NavTrackInitialDistNm = static_cast<float>(DistNm);
		bNavApproachedActive = false;
	}

	if (Nav->GetSelectedIndex() != Dynamics.NavWpIndex)
	{
		Nav->SetSelectedIndex(Dynamics.NavWpIndex);
	}
	// Steer so track (COG) hits the mark — not just the bow bearing.
	Dynamics.AutoTarget = SailSimNavHeadingCmd(Dynamics, BrgDeg);

	// Periodic diagnostics (every ~2 s).
	static double LastNavLog = -1000.0;
	const double Now = FPlatformTime::Seconds();
	if (Now - LastNavLog > 2.0)
	{
		LastNavLog = Now;
		const float Err = FBoatDynamics::Wrap180(Dynamics.AutoTarget - Dynamics.Heading);
		UE_LOG(LogSailSim, Log,
			TEXT("[nav] track WP%d/%d  dist=%.2fnm  brg=%.0f°  cmd=%.0f°  hdg=%.0f°  beta=%+.1f°  err=%+.0f°  sog=%.1fkt"),
			Dynamics.NavWpIndex + 1, Nav->Num(), DistNm, BrgDeg, Dynamics.AutoTarget,
			Dynamics.Heading, Dynamics.Beta, Err, Dynamics.GetSpeedKnots());
	}
}

void ASailBoatPawn::SetSheetEase(float Ease01)
{
	Dynamics.SetSheetEase(Ease01);
	UpdateBoomFromSheet();
}

void ASailBoatPawn::SetOuthaul(float V01)
{
	Dynamics.SetOuthaul(V01);
	MainCloth.Outhaul01 = Dynamics.Outhaul01;
}

void ASailBoatPawn::SetVang(float V01)
{
	Dynamics.SetVang(V01);
	MainCloth.Vang01 = Dynamics.Vang01;
}

void ASailBoatPawn::SetSpinSheetEase(float Ease01)
{
	SpinSheetEase01 = FMath::Clamp(Ease01, 0.f, 1.f);
}

void ASailBoatPawn::SetJibSet(bool bSet)
{
	JibSetTarget01 = bSet ? 1.f : 0.f;
}

void ASailBoatPawn::ToggleJibSet()
{
	SetJibSet(!IsJibSet());
}

void ASailBoatPawn::SetKiteSet(bool bSet)
{
	KiteSetTarget01 = bSet ? 1.f : 0.f;
}

void ASailBoatPawn::ToggleKiteSet()
{
	SetKiteSet(!IsKiteSet());
}

void ASailBoatPawn::SetJibCar(float V01)
{
	Dynamics.SetJibCar(V01);
}

void ASailBoatPawn::SetJibLuffTension(float V01)
{
	Dynamics.SetJibLuffTension(V01);
	JibCloth.LuffTension01 = Dynamics.JibLuffTension01;
}

void ASailBoatPawn::SetJibLeechTension(float V01)
{
	Dynamics.SetJibLeechTension(V01);
	JibCloth.LeechTension01 = Dynamics.JibLeechTension01;
	MainCloth.LeechTension01 = FMath::Lerp(0.05f, 0.20f, Dynamics.JibLeechTension01);
}

float ASailBoatPawn::GetWindFromSign() const
{
	// +1 wind FROM starboard = Dynamics LeeSign (stbd tack).
	return static_cast<float>(Dynamics.GetLeeSign());
}

float ASailBoatPawn::GetLeeSideSign() const
{
	// Boom +yaw → port for aft boom. LeeSign +1 (lee port) → boom +yaw.
	return static_cast<float>(Dynamics.GetLeeSign());
}

FVector ASailBoatPawn::GetTrueWindBoatVector() const
{
	const float RelFrom = Dynamics.GetTrueWindFromRelDeg();
	const float R = FMath::DegreesToRadians(RelFrom);
	return FVector(FMath::Cos(R), FMath::Sin(R), 0.f);
}

FVector ASailBoatPawn::GetApparentWindBoatVector() const
{
	// Apparent FROM unit (UE +X bow, +Y stbd).
	const float Awa = Dynamics.GetApparentWindAngleDeg();
	const float R = FMath::DegreesToRadians(Awa);
	return FVector(FMath::Cos(R), FMath::Sin(R), 0.f);
}

FVector ASailBoatPawn::GetApparentWindAirVelBoat() const
{
	// Air velocity (downwind); dyn +Z stbd → UE +Y.
	float Dx = 0.f, Dz = 0.f;
	Dynamics.GetApparentWindAirVelUnit(Dx, Dz);
	return FVector(Dx, Dz, 0.f);
}

void ASailBoatPawn::GetBoomEndpointsBoat(float MeshS, FVector& OutBase, FVector& OutTip) const
{
	OutBase = BoomBaseLoc * MeshS;
	if (!bBoomEndpointsValid)
	{
		OutTip = OutBase + FVector(-200.f * MeshS, 0.f, BoomRiseCm * MeshS);
		return;
	}
	const FVector LocalEnd = (BoomEndCenterlineLoc - BoomBaseLoc) * MeshS;
	const float BoomLen = FMath::Max(1.f, LocalEnd.Size());
	// Yaw about gooseneck (UE +Z up); then apply vang rise at the tip.
	const FQuat Q(FVector::UpVector, FMath::DegreesToRadians(BoomYawDeg));
	FVector Horiz = Q.RotateVector(FVector(LocalEnd.X, LocalEnd.Y, 0.f));
	float HLen = Horiz.Size();
	// Target tip rise from vang (scaled): hard on → 0, eased → MaxBoomRiseFrac·L
	const float Rise = BoomRiseCm * MeshS;
	// Keep ~constant boom length when tip lifts
	if (HLen > 1.f && Rise > 0.f && Rise < BoomLen * 0.95f)
	{
		const float H = FMath::Sqrt(FMath::Max(1.f, BoomLen * BoomLen - Rise * Rise));
		Horiz *= (H / HLen);
	}
	OutTip = OutBase + Horiz + FVector(0.f, 0.f, Rise);
}

void ASailBoatPawn::UpdateBoomFromSheet(float DeltaSeconds)
{
	// Sheet is a *max angle* constraint (like a real mainsheet), not a motor position.
	// Wind always tries to blow the boom fully to leeward; sheet ease sets how far it may go.
	// Sheeting in shortens the max → boom is hauled in; easing lets it run out under wind.
	const float Ease = FMath::Clamp(Dynamics.SheetEase, 0.f, 1.f);
	const float LeeBoomSign = GetLeeSideSign(); // +1 → port (wind from stbd), −1 → stbd
	const float SheetMaxAbs = Ease * MaxBoomSwingDeg;
	// Desired free position under wind: hard against the sheet on the lee side.
	const float WindTarget = LeeBoomSign * SheetMaxAbs;

	auto MoveToward = [](float Cur, float Target, float MaxStep)
	{
		const float D = Target - Cur;
		if (FMath::Abs(D) <= MaxStep) return Target;
		return Cur + FMath::Sign(D) * MaxStep;
	};

	if (DeltaSeconds <= 0.f)
	{
		// Settle / immediate: sit on the sheet (loaded boom).
		BoomYawDeg = WindTarget;
	}
	else
	{
		const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.05f);
		const float AbsBoom = FMath::Abs(BoomYawDeg);
		// Sheet shortened past boom, or boom on windward side after tack → haul in/across.
		const bool bWrongSide = (BoomYawDeg * LeeBoomSign < -0.5f);
		const bool bOverSheet = (AbsBoom > SheetMaxAbs + 0.25f);
		if (bOverSheet || bWrongSide)
		{
			const float Step = BoomSheetInRateDeg * Dt;
			BoomYawDeg = MoveToward(BoomYawDeg, WindTarget, Step);
		}
		else
		{
			// Free: wind blows boom out toward the sheet (lee) limit.
			const float Step = BoomSwingOutRateDeg * Dt
				* FMath::Clamp(Dynamics.TrueWindSpeedKn / 12.f, 0.35f, 1.5f);
			BoomYawDeg = MoveToward(BoomYawDeg, WindTarget, Step);
		}
		// Hard sheet cone: only leeward of centerline, |angle| ≤ sheet max.
		if (LeeBoomSign > 0.f)
		{
			BoomYawDeg = FMath::Clamp(BoomYawDeg, 0.f, SheetMaxAbs);
		}
		else
		{
			BoomYawDeg = FMath::Clamp(BoomYawDeg, -SheetMaxAbs, 0.f);
		}
	}

	// Vang sets boom tip height (unscaled cm). 0 = hard on / low; 1 = eased / tip up.
	{
		const float BoomLen = bBoomEndpointsValid
			? FMath::Max(1.f, FVector::Dist(BoomBaseLoc, BoomEndCenterlineLoc))
			: 400.f;
		const float MaxRise = BoomLen * FMath::Clamp(MaxBoomRiseFrac, 0.02f, 0.35f);
		const float TargetRise = MaxRise * FMath::Clamp(Dynamics.Vang01, 0.f, 1.f);
		if (DeltaSeconds <= 0.f)
		{
			BoomRiseCm = TargetRise;
		}
		else
		{
			// Smooth so the spar doesn't pop when dragging the slider
			BoomRiseCm = FMath::FInterpTo(BoomRiseCm, TargetRise, DeltaSeconds, 6.f);
		}
	}

	// MeshUniformScale grows hull/sails about the boat root. Boom endpoints and
	// mast pivot must use the same scale or the spar drifts away from the clew.
	const float MeshS = ActiveSpec.MeshUniformScale();
	if (bBoomEndpointsValid && BoomMesh)
	{
		FVector Base, Tip;
		GetBoomEndpointsBoat(MeshS, Base, Tip);
		PlaceSparFromEndpoints(BoomMesh, Base, Tip);
	}

	if (MainSailMesh)
	{
		if (bMastPivotValid) MainSailMesh->SetRelativeLocation(MastBaseLoc * MeshS);
		const float RigidYaw = (bEnableSailCloth && MainCloth.bInitialized) ? 0.f : BoomYawDeg;
		MainSailMesh->SetRelativeRotation(FRotator(0.f, RigidYaw, 0.f));
	}
	if (JibSailMesh)
	{
		if (bMastPivotValid) JibSailMesh->SetRelativeLocation(MastBaseLoc * MeshS);
		const float RigidYaw = (bEnableSailCloth && JibCloth.bInitialized) ? 0.f : BoomYawDeg * 0.9f;
		JibSailMesh->SetRelativeRotation(FRotator(0.f, RigidYaw, 0.f));
	}
}

void ASailBoatPawn::SettleSailClothImmediate()
{
	if (!bEnableSailCloth) return;
	UpdateBoomFromSheet(/*snap*/ -1.f);
	for (int32 I = 0; I < 10; ++I)
	{
		UpdateBoomFromSheet(1.f / 60.f);
		UpdateSailCloth(1.f / 60.f);
	}
}

void ASailBoatPawn::ApplyClothForceToDynamics()
{
	if (!bEnableSailCloth)
	{
		Dynamics.ClothForceScale = 1.f;
		return;
	}
	float Q = 0.f;
	float W = 0.f;
	if (MainCloth.bInitialized)
	{
		Q += MainCloth.GetForceScale() * 0.55f;
		W += 0.55f;
	}
	if (JibCloth.bInitialized && SpinDeploy01 < 0.35f)
	{
		Q += JibCloth.GetForceScale() * 0.35f;
		W += 0.35f;
	}
	if (SpinCloth.bInitialized && SpinDeploy01 > 0.2f)
	{
		// Big kite — boost drive when flying (class ~1.2× white-sail area)
		const float Sw = 0.55f * SpinDeploy01;
		Q += SpinCloth.GetForceScale() * 1.15f * Sw;
		W += Sw;
	}
	Dynamics.ClothForceScale = (W > 0.f) ? (Q / W) : 1.f;
}

float ASailBoatPawn::GetTrueWindAngleAbsDeg() const
{
	// 0 = head-to-wind, 90 = beam reach, 180 = dead run
	const float H = Dynamics.Heading;
	const float Tw = Dynamics.TrueWindDirDeg;
	float Diff = FMath::Abs(FMath::FindDeltaAngleDegrees(H, Tw));
	// FindDeltaAngle is -180..180; Abs → 0..180. Wind FROM direction:
	// TWA is angle between bow and wind-from — if heading into wind, Diff small.
	// Dynamics: TrueWindDir is FROM direction (meteorological). Heading is bow.
	// TWA = |heading - windFrom| wrapped 0..180.
	return FMath::Clamp(Diff, 0.f, 180.f);
}

void ASailBoatPawn::EnsureSpinAndSpritBuilt(bool bForceRebuild)
{
	if (!bEnableSailCloth || !SpinSailMesh) return;
	if (bSpinMeshBuilt && SpinCloth.bInitialized && !bForceRebuild) return;

	const float MeshS = ActiveSpec.MeshUniformScale();
	const float FT = 30.48f;
	// Code Zero (J/105-scale ft): shorter foot than class A2 (22.3'), flatter
	// reaching cut. Luff ~I to sprit tip, SMG~70% SF built in cloth loft.
	const float Icm = FMath::Max(200.f, ActiveSpec.I * FT);
	const float Jcm = FMath::Max(150.f, ActiveSpec.J * FT);
	const float FootCm = 19.5f * FT; // CZ foot ~ mid genoa / short of class A2

	// Stem ≈ mast + J forward (forestay tack region)
	const FVector Mast = bMastPivotValid ? MastBaseLoc : FVector(0.f, 0.f, 80.f);
	const FVector Stem(Mast.X + Jcm, 0.f, Mast.Z + 15.f);
	const FVector SpritBase = Stem;
	// J/105 sprit fully out ≈ 6.5' past stem (SpritExtendCm)
	const FVector SpritTipExt = Stem + FVector(SpritExtendCm, 0.f, 6.f);

	// Corners in boat space (spin mesh at identity). Head near masthead (ISP-ish).
	const FVector HeadBoat = Mast + FVector(0.f, 0.f, Icm * 0.95f);
	const FVector TackBoat = SpritTipExt; // fully extended rest shape
	// Code Zero clew sits low like a large genoa (near boom / rail height),
	// NOT high on the leech like a runner A2. Old 0.30·I put it ~15' up.
	const float ClewZ = Mast.Z + FMath::Clamp(Icm * 0.09f, 70.f, 140.f);
	const FVector ClewBoat = FVector(
		Mast.X - FootCm * 0.22f, // a bit further aft so the foot isn't vertical
		FootCm * 0.48f,          // out toward sheet lead
		ClewZ);

	// Build cloth in component-local = boat root (spin mesh at identity)
	SpinSailMesh->SetRelativeLocation(FVector::ZeroVector);
	SpinSailMesh->SetRelativeRotation(FRotator::ZeroRotator);
	SpinSailMesh->SetRelativeScale3D(FVector(MeshS));

	// Cloth verts are unscaled (like main/jib); component scale applies visual
	const FVector TackL = TackBoat;
	const FVector HeadL = HeadBoat;
	const FVector ClewL = ClewBoat;

	const bool bOk = SpinCloth.BuildFromFlatTriangle(
		SpinSailMesh, 0, ESailRigKind::AsymSpin,
		TackL, HeadL, ClewL, 26, 14);
	if (!bOk)
	{
		UE_LOG(LogSailSim, Warning, TEXT("Spin cloth build failed"));
		return;
	}

	// Solid two-sided nylon (translucent masters read too ghosty even at high α).
	// Prefer yacht two-sided PBR; fall back to translucent only if missing.
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR_TwoSided.M_Yacht_PBR_TwoSided"));
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_Sail.MI_Yacht_Sail"));
	}
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineDebugMaterials/M_SimpleTranslucent.M_SimpleTranslucent"));
	}
	auto MakeSpinMid = [this, Base](const FLinearColor& Nylon) -> UMaterialInstanceDynamic*
	{
		if (!Base) return nullptr;
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
		if (!Mid) return nullptr;
		FLinearColor C = Nylon;
		C.A = 1.f;
		Mid->SetVectorParameterValue(TEXT("Color"), C);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), C);
		Mid->SetVectorParameterValue(TEXT("TintColor"), C);
		Mid->SetScalarParameterValue(TEXT("Roughness"), 0.80f);
		Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
		Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
		Mid->SetScalarParameterValue(TEXT("OpacityMask"), 1.f);
		Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
		return Mid;
	};
	// Section 0 = sail body, section 1 = seam ribbons
	if (UMaterialInstanceDynamic* MidA = MakeSpinMid(FLinearColor(0.05f, 0.18f, 0.48f)))
	{
		SpinSailMesh->SetMaterial(0, MidA);
	}
	if (UMaterialInstanceDynamic* MidS = MakeSpinMid(FLinearColor(0.02f, 0.05f, 0.12f)))
	{
		MidS->SetScalarParameterValue(TEXT("Roughness"), 0.90f);
		SpinSailMesh->SetMaterial(1, MidS);
	}
	// Match main/jib: no sail shadows (VSM thrash on deforming PMC).
	SpinSailMesh->SetCastShadow(false);
	SpinSailMesh->bCastDynamicShadow = false;
	SpinSailMesh->bCastStaticShadow = false;
	SpinSailMesh->bCastContactShadow = false;

	// Sprit retracted initially
	SpritExtend01 = 0.f;
	SpinDeploy01 = 0.f;
	if (BowspritMesh)
	{
		// Base at stem, tip retracts to stem when 0
		PlaceSparFromEndpoints(BowspritMesh, SpritBase, SpritBase + FVector(5.f, 0.f, 0.f));
		BowspritMesh->SetVisibility(true);
		BowspritMesh->SetHiddenInGame(false);
	}
	bSpinMeshBuilt = true;
	UE_LOG(LogSailSim, Log,
		TEXT("Code Zero + sprit built (sprit %.1f' past stem, deploy TWA %.0f–%.0f°, foot %.1f')"),
		SpritExtendCm / FT, SpinDeployTwaStartDeg, SpinDeployTwaFullDeg, FootCm / FT);
}

void ASailBoatPawn::UpdateSpinAndSprit(float DeltaSeconds)
{
	if (!bEnableSailCloth) return;
	EnsureSpinAndSpritBuilt(false);
	if (!SpinCloth.bInitialized || !SpinSailMesh) return;

	const float MeshS = ActiveSpec.MeshUniformScale();
	const float FT = 30.48f;
	const float Icm = FMath::Max(200.f, ActiveSpec.I * FT);
	const float Jcm = FMath::Max(150.f, ActiveSpec.J * FT);

	const FVector Mast = bMastPivotValid ? MastBaseLoc : FVector(0.f, 0.f, 80.f);
	const FVector Stem(Mast.X + Jcm, 0.f, Mast.Z + 15.f);
	const FVector HeadBoat = Mast + FVector(0.f, 0.f, Icm * 0.95f);

	// Manual kite only: SET → hoist and stay up at any TWA; DOUSED → drop.
	// No auto-douse upwind (user asked to keep the kite until the helm button).
	const float Twa = GetTrueWindAngleAbsDeg();
	const float Target = FMath::Clamp(KiteSetTarget01, 0.f, 1.f);
	// Smooth set/douse
	const float Rate = (Target > SpinDeploy01) ? 0.55f : 0.85f; // hoist slower than douse
	SpinDeploy01 = FMath::FInterpTo(SpinDeploy01, Target, DeltaSeconds, Rate);
	// Sprit leads the hoist slightly
	const float SpritT = FMath::Clamp(SpinDeploy01 * 1.15f, 0.f, 1.f);
	SpritExtend01 = FMath::FInterpTo(SpritExtend01, SpritT, DeltaSeconds, 1.2f);

	const FVector SpritTip = Stem + FVector(SpritExtendCm * SpritExtend01, 0.f, 6.f * SpritExtend01);
	if (BowspritMesh)
	{
		const FVector TipVis = (SpritExtend01 < 0.05f)
			? Stem + FVector(8.f, 0.f, 0.f)
			: SpritTip;
		PlaceSparFromEndpoints(BowspritMesh, Stem, TipVis);
		if (BowspritMesh)
		{
			const float L = FVector::Dist(Stem, TipVis);
			BowspritMesh->SetRelativeScale3D(FVector(0.08f, 0.08f, L / 100.f));
			BowspritMesh->SetVisibility(true);
			BowspritMesh->SetHiddenInGame(false);
		}
	}

	const bool bShow = SpinDeploy01 > 0.08f && SpritExtend01 > 0.12f;
	SpinSailMesh->SetVisibility(bShow);
	SpinSailMesh->SetHiddenInGame(!bShow);
	// Jib stays up with the kite — set/douse is manual (SetJibSet / helm button).

	if (!bShow)
	{
		auto HideSpinRope = [](UStaticMeshComponent* C)
		{
			if (!C) return;
			C->SetVisibility(false);
			C->SetHiddenInGame(true);
		};
		HideSpinRope(SpinSheetMesh);
		HideSpinRope(SpinSheetToWinchMesh);
		HideSpinRope(SpinLazySheetSegA);
		HideSpinRope(SpinLazySheetSegB);
		return;
	}

	// Lee side from VPP — kite must fill to leeward of the boat
	const int32 Lee = Dynamics.GetLeeSign();
	const bool bLeeToStarboard = (Lee < 0);
	const float LeeY = bLeeToStarboard ? 1.f : -1.f;

	// J/105: spin sheet turns at stern-quarter rail block, then all the way forward
	// to the cabin-top winch. sheet_leads are unscaled boat-cm (same as tracks).
	const float LeadY = FMath::Max(140.f, HullBeamCm * 0.55f);
	FVector BlkPort(Mast.X - HullLengthCm * 0.42f, -LeadY, Mast.Z + 28.f);
	FVector BlkStbd(Mast.X - HullLengthCm * 0.42f, LeadY, Mast.Z + 28.f);
	// Cabin winches sit on coachroof near companionway — well forward of the quarters.
	FVector CabPort(Mast.X - HullLengthCm * 0.05f, -HullBeamCm * 0.20f, Mast.Z + 110.f);
	FVector CabStbd(Mast.X - HullLengthCm * 0.05f, HullBeamCm * 0.20f, Mast.Z + 110.f);
	if (CachedRigging.bSheetLeadsValid)
	{
		BlkPort = CachedRigging.SpinPortBlock;
		BlkStbd = CachedRigging.SpinStbdBlock;
		CabPort = CachedRigging.SpinPortWinch;
		CabStbd = CachedRigging.SpinStbdWinch;
	}
	// Cloth constraint uses unscaled boat cm (spin mesh verts are unscaled).
	const FVector LeadPort = BlkPort;
	const FVector LeadStbd = BlkStbd;

	// Spin cloth lives in unscaled boat cm (component scale = MeshS)
	SpinCloth.SetStayEndpoints(SpritTip, HeadBoat);

	const FVector AirVelBoat = GetApparentWindAirVelBoat();
	const float AwsKn = FMath::Max(Dynamics.GetApparentWindSpeedKn(), Dynamics.TrueWindSpeedKn * 0.9f);
	// User spin-sheet slider (0 hard … 1 eased). Mild bias deeper on a run.
	const float DeepBias = FMath::Clamp((Twa - SpinDeployTwaStartDeg) / 70.f, 0.f, 0.20f);
	const float SpinEase = FMath::Clamp(SpinSheetEase01 + DeepBias * 0.5f, 0.f, 1.f);

	const FTransform SpinXf = SpinSailMesh->GetRelativeTransform();
	const FVector WindLocal = SpinXf.InverseTransformVectorNoScale(AirVelBoat).GetSafeNormal();

	// Soften aero when partially set
	const float AeroSave = SpinCloth.AeroK;
	SpinCloth.AeroK = AeroSave * (0.35f + 0.65f * SpinDeploy01);
	SpinCloth.Step(
		DeltaSeconds, WindLocal, AwsKn * SpinDeploy01,
		FVector::ZeroVector, SpinEase,
		LeadPort, LeadStbd, bLeeToStarboard);
	// One-shot lee flip only when clew is clearly windward (cheap, no thrash)
	SpinCloth.ForceAsymLeeSide(LeeY);
	SpinCloth.AeroK = AeroSave;
	SpinCloth.PushToMesh(SpinSailMesh);
	SpinSailMesh->SetRelativeScale3D(FVector(MeshS));
	// No MarkRenderStateDirty — same as main/jib (UpdateMeshSection is enough).

	// Sheet ropes: clew → quarter block → cabin winch (active lee) + lazy windward.
	// Visual space is MeshS-scaled boat-cm (spin mesh component scale = MeshS).
	if (SpinCloth.ClewIndex != INDEX_NONE)
	{
		const FVector ClewLocal = SpinCloth.Pos[SpinCloth.ClewIndex];
		const FVector ClewBoat = SpinSailMesh->GetRelativeTransform().TransformPosition(ClewLocal);
		const FVector LeeBlk = (bLeeToStarboard ? BlkStbd : BlkPort) * MeshS;
		const FVector WxBlk = (bLeeToStarboard ? BlkPort : BlkStbd) * MeshS;
		FVector LeeCab = (bLeeToStarboard ? CabStbd : CabPort) * MeshS;
		FVector WxCab = (bLeeToStarboard ? CabPort : CabStbd) * MeshS;
		// Drum lead above cabin top so the long run clearly terminates on the winch.
		LeeCab.Z = FMath::Max(LeeCab.Z, LeeBlk.Z + 40.f * MeshS);
		WxCab.Z = FMath::Max(WxCab.Z, WxBlk.Z + 40.f * MeshS);

		auto PlaceRope = [this](UStaticMeshComponent* C, const FVector& A, const FVector& B, float Rad)
		{
			if (!C) return;
			const float L = FVector::Dist(A, B);
			if (L < 1.f) return;
			PlaceSparFromEndpoints(C, A, B);
			C->SetRelativeScale3D(FVector(Rad, Rad, L / 100.f));
			C->SetVisibility(true);
			C->SetHiddenInGame(false);
		};

		// Active (leeward): taut clew → rail block → cabin-top winch (full length)
		PlaceRope(SpinSheetMesh, ClewBoat, LeeBlk, 0.028f);
		PlaceRope(SpinSheetToWinchMesh, LeeBlk, LeeCab, 0.026f);

		// Lazy (windward): slack clew → opposite rail block → cabin winch
		PlaceRope(SpinLazySheetSegA, ClewBoat, WxBlk, 0.016f);
		PlaceRope(SpinLazySheetSegB, WxBlk, WxCab, 0.016f);
	}
}

void ASailBoatPawn::EnsureSailClothBuilt(bool bForceRebuild)
{
	if (!bEnableSailCloth)
	{
		MainCloth.Clear();
		JibCloth.Clear();
		return;
	}

	const bool bNeedMain = MainSailMesh
		&& MainSailMesh->GetNumSections() > 0
		&& (bForceRebuild || !MainCloth.bInitialized);
	const bool bNeedJib = JibSailMesh
		&& JibSailMesh->GetNumSections() > 0
		&& (bForceRebuild || !JibCloth.bInitialized);

	if (bNeedMain)
	{
		MainCloth.Clear();
		const bool bOk = MainCloth.BuildFromMesh(MainSailMesh, 0, ESailRigKind::Main);
		UE_LOG(LogSailSim, Log, TEXT("EnsureSailCloth MAIN: %s (sections=%d)"),
			bOk ? TEXT("ok") : TEXT("FAIL"), MainSailMesh->GetNumSections());
	}
	if (bNeedJib)
	{
		JibCloth.Clear();
		const bool bOk = JibCloth.BuildFromMesh(JibSailMesh, 0, ESailRigKind::Jib);
		UE_LOG(LogSailSim, Log, TEXT("EnsureSailCloth JIB: %s (sections=%d)"),
			bOk ? TEXT("ok") : TEXT("FAIL"), JibSailMesh->GetNumSections());
	}

	if (bNeedMain || bNeedJib)
	{
		SettleSailClothImmediate();
	}
}

void ASailBoatPawn::UpdateSailCloth(float DeltaSeconds)
{
	if (!bEnableSailCloth) return;

	// Hot-reload / early LoadLoftMesh skip can leave cloth uninitialized
	if (!MainCloth.bInitialized || !JibCloth.bInitialized)
	{
		EnsureSailClothBuilt(false);
	}

	// Manual jib set/douse — roller furling (smooth wrap onto forestay).
	// Same moderate rate both ways so the full roll is visible end-to-end.
	const float JibFurlRate = 1.05f;
	JibSet01 = FMath::FInterpTo(JibSet01, JibSetTarget01, DeltaSeconds, JibFurlRate);
	// Cloth always visible: fully furled is a tight roll on the forestay foil.
	const bool bJibVisible = true;
	// Sheets only when the sail is mostly out.
	const bool bJibSheetsOut = JibSet01 > 0.18f;
	if (JibSailMesh)
	{
		JibSailMesh->SetVisibility(bJibVisible);
		JibSailMesh->SetHiddenInGame(!bJibVisible);
	}
	auto HideJibRope = [bJibSheetsOut](UStaticMeshComponent* C)
	{
		if (!C) return;
		C->SetVisibility(bJibSheetsOut);
		C->SetHiddenInGame(!bJibSheetsOut);
	};
	HideJibRope(JibSheetPortMesh);
	HideJibRope(JibSheetStbdMesh);
	HideJibRope(JibSheetPortToWinchMesh);
	HideJibRope(JibSheetStbdToWinchMesh);
	HideJibRope(JibLazySheetSegA);
	HideJibRope(JibLazySheetSegB);
	const bool bJibFlying = JibSet01 > 0.08f; // aero / telltales threshold

	const float Ease = FMath::Clamp(Dynamics.SheetEase, 0.f, 1.f);
	// Single source of truth: Dynamics.LeeSign (+1 = lee port = wind from stbd).
	const int32 Lee = Dynamics.GetLeeSign();
	const bool bLeeToStarboard = (Lee < 0); // lee starboard only on port tack

	// Push live trim into cloth (web setOuthaul / setVang / leech line)
	MainCloth.Outhaul01 = Dynamics.Outhaul01;
	MainCloth.Vang01 = Dynamics.Vang01;
	// Keep main leech shortened so it doesn't hang open.
	MainCloth.LeechTension01 = FMath::Clamp(
		0.62f + 0.20f * Dynamics.JibLeechTension01, 0.f, 1.f);
	JibCloth.LuffTension01 = Dynamics.JibLuffTension01;
	JibCloth.LeechTension01 = Dynamics.JibLeechTension01;
	JibCloth.Outhaul01 = 0.5f;

	// Cloth pressure uses air velocity (downwind), not FROM.
	// Pass in boat space; convert with each sail's component transform (incl. scale).
	const FVector AirVelBoat = GetApparentWindAirVelBoat();
	const float AwsKn = FMath::Max(Dynamics.GetApparentWindSpeedKn(), Dynamics.TrueWindSpeedKn * 0.85f);

	// --- Main: boom direction from free tip; outhaul sets distance along boom ---
	if (MainCloth.bInitialized && MainSailMesh)
	{
		const float MeshS = ActiveSpec.MeshUniformScale();
		// Class E in sail-local cm (proc verts are unscaled; component scale is visual).
		if (bBoomEndpointsValid)
		{
			MainCloth.BoomLenCm = FVector::Dist(BoomBaseLoc, BoomEndCenterlineLoc);
		}
		FVector BoomTipBoat = BoomEndCenterlineLoc * MeshS;
		FVector GooseBoat = BoomBaseLoc * MeshS;
		if (bBoomEndpointsValid)
		{
			GetBoomEndpointsBoat(MeshS, GooseBoat, BoomTipBoat);
		}
		// Relative transform = sail local (matches ProcMesh verts + component scale)
		const FTransform SailXf = MainSailMesh->GetRelativeTransform();
		const FVector ClewLocal = SailXf.InverseTransformPosition(BoomTipBoat);
		// Keep gooseneck tack aligned with the spar root in sail-local space.
		const FVector GooseLocal = SailXf.InverseTransformPosition(GooseBoat);
		if (GooseLocal.SizeSquared() > 1.f)
		{
			MainCloth.StayTackLocal = GooseLocal;
		}
		const FVector WindLocalMain = SailXf.InverseTransformVectorNoScale(AirVelBoat).GetSafeNormal();
		MainCloth.Step(
			DeltaSeconds, WindLocalMain, AwsKn,
			ClewLocal, Ease, FVector::ZeroVector, FVector::ZeroVector, bLeeToStarboard);
		// UpdateMeshSection already uploads verts — MarkRenderStateDirty forces a full
		// RT proxy rebuild every frame (expensive; not needed for section vertex updates).
		MainCloth.PushToMesh(MainSailMesh);

		// --- Jib: lee lead + roller furling morph onto forestay ---
		FVector WindLocalJib = WindLocalMain;
		if (bJibVisible && JibCloth.bInitialized && JibSailMesh)
		{
			const float MeshSJib = ActiveSpec.MeshUniformScale();
			const float Car = FMath::Clamp(Dynamics.JibCar01, 0.f, 1.f);
			FVector LeadPortBoat, LeadStbdBoat;
			if (CachedRigging.bJibTracksValid)
			{
				LeadPortBoat = FMath::Lerp(
					CachedRigging.JibPortTrackFwd, CachedRigging.JibPortTrackAft, Car) * MeshSJib
					+ FVector(0.f, 0.f, 3.f);
				LeadStbdBoat = FMath::Lerp(
					CachedRigging.JibStbdTrackFwd, CachedRigging.JibStbdTrackAft, Car) * MeshSJib
					+ FVector(0.f, 0.f, 3.f);
			}
			else
			{
				const FVector Mast = bMastPivotValid ? MastBaseLoc : FVector::ZeroVector;
				const float TrackLen = FMath::Max(100.f, HullLengthCm * 0.12f);
				const float LeadX = Mast.X - TrackLen * 0.15f - TrackLen * Car;
				const float LeadY = FMath::Max(60.f, HullBeamCm * 0.38f);
				const float LeadZ = Mast.Z + 25.f;
				LeadPortBoat = FVector(LeadX, -LeadY, LeadZ);
				LeadStbdBoat = FVector(LeadX, LeadY, LeadZ);
			}

			const FTransform JibXf = JibSailMesh->GetRelativeTransform();
			const FVector LeadPortLocal = JibXf.InverseTransformPosition(LeadPortBoat);
			const FVector LeadStbdLocal = JibXf.InverseTransformPosition(LeadStbdBoat);
			WindLocalJib = JibXf.InverseTransformVectorNoScale(AirVelBoat).GetSafeNormal();

			// Roller-furl owns the mesh until nearly fully set. Never SnapRigToStay
			// mid-furl (that collapsed the sail) and never Step on furled verts
			// (that popped them open).
			const float SetAmt = FMath::Clamp(JibSet01, 0.f, 1.f);
			constexpr float kFurlDriveMaxSet = 0.92f; // below this: pure geometric wrap
			if (SetAmt < kFurlDriveMaxSet)
			{
				bJibFurlDriveActive = true;
				FurlerSpinRad = JibCloth.ApplyRollerFurl(SetAmt);
			}
			else
			{
				if (bJibFurlDriveActive)
				{
					// Hand off: seed loft so Verlet doesn't explode from sausage shape.
					JibCloth.SeedOpenFromLoft();
					bJibFurlDriveActive = false;
				}
				const float AeroSave = JibCloth.AeroK;
				JibCloth.AeroK = AeroSave * SetAmt * SetAmt;
				JibCloth.Step(
					DeltaSeconds, WindLocalJib, AwsKn * SetAmt,
					FVector::ZeroVector,
					Ease,
					LeadPortLocal, LeadStbdLocal,
					bLeeToStarboard);
				JibCloth.AeroK = AeroSave;
				// Tiny residual wrap near full-set (smooth last few degrees of drum).
				FurlerSpinRad = JibCloth.ApplyRollerFurl(SetAmt);
			}
			JibCloth.PushToMesh(JibSailMesh);

			// Forestay foil + spinning furler drum at the tack.
			UpdateJibFurlerVisuals(MeshSJib);
		}

		if (bEnableSailVisualExtras)
		{
			SailVisuals.SailNumber = SailNumber;
			SailVisuals.bEnabled = true;
			SailVisuals.bJibTellTales = bJibFlying;
			SailVisuals.EnsureBuilt(MainSailMesh, JibSailMesh);
			SailVisuals.Update(DeltaSeconds, MainCloth, JibCloth, WindLocalMain, WindLocalJib);
		}
	}
	else if (bJibVisible && JibCloth.bInitialized && JibSailMesh)
	{
		// Main missing — still step/furl jib alone
		const float MeshSJib = ActiveSpec.MeshUniformScale();
		const float Car = FMath::Clamp(Dynamics.JibCar01, 0.f, 1.f);
		FVector LeadPortBoat, LeadStbdBoat;
		if (CachedRigging.bJibTracksValid)
		{
			LeadPortBoat = FMath::Lerp(
				CachedRigging.JibPortTrackFwd, CachedRigging.JibPortTrackAft, Car) * MeshSJib
				+ FVector(0.f, 0.f, 3.f);
			LeadStbdBoat = FMath::Lerp(
				CachedRigging.JibStbdTrackFwd, CachedRigging.JibStbdTrackAft, Car) * MeshSJib
				+ FVector(0.f, 0.f, 3.f);
		}
		else
		{
			const FVector Mast = bMastPivotValid ? MastBaseLoc : FVector::ZeroVector;
			const float TrackLen = FMath::Max(100.f, HullLengthCm * 0.12f);
			const float LeadX = Mast.X - TrackLen * 0.15f - TrackLen * Car;
			const float LeadY = FMath::Max(60.f, HullBeamCm * 0.38f);
			const float LeadZ = Mast.Z + 25.f;
			LeadPortBoat = FVector(LeadX, -LeadY, LeadZ);
			LeadStbdBoat = FVector(LeadX, LeadY, LeadZ);
		}
		const FTransform JibXf = JibSailMesh->GetRelativeTransform();
		const FVector WindLocalJib = JibXf.InverseTransformVectorNoScale(AirVelBoat).GetSafeNormal();
		const float SetAmt = FMath::Clamp(JibSet01, 0.f, 1.f);
		constexpr float kFurlDriveMaxSet = 0.92f;
		if (SetAmt < kFurlDriveMaxSet)
		{
			bJibFurlDriveActive = true;
			FurlerSpinRad = JibCloth.ApplyRollerFurl(SetAmt);
		}
		else
		{
			if (bJibFurlDriveActive)
			{
				JibCloth.SeedOpenFromLoft();
				bJibFurlDriveActive = false;
			}
			JibCloth.Step(
				DeltaSeconds, WindLocalJib, AwsKn * SetAmt,
				FVector::ZeroVector, Ease,
				JibXf.InverseTransformPosition(LeadPortBoat),
				JibXf.InverseTransformPosition(LeadStbdBoat),
				bLeeToStarboard);
			FurlerSpinRad = JibCloth.ApplyRollerFurl(SetAmt);
		}
		JibCloth.PushToMesh(JibSailMesh);
		UpdateJibFurlerVisuals(MeshSJib);
	}

	ApplyClothForceToDynamics();
}

void ASailBoatPawn::UpdateWindCompass(float DeltaSeconds)
{
	// Web hUpdateWindCompass: flat ring on water, true (cyan) + apparent (amber).
	WindCompass.Update(this, DeltaSeconds);
}

void ASailBoatPawn::SampleMultiPointBuoyancy(
	const FVector& Loc, float CosH, float SinH,
	float& OutTargetZ, float& OutWavePitchDeg, float& OutWaveRollDeg) const
{
	OutTargetZ = bFloatInit ? SmoothedWaterZ : WaterSurfaceZ;
	OutWavePitchDeg = 0.f;
	OutWaveRollDeg = 0.f;

	const FVector Fwd(CosH, SinH, 0.f);
	const FVector Right(-SinH, CosH, 0.f);
	const float HalfLoa = HullLengthCm * 0.42f;
	const float HalfBeam = HullBeamCm * 0.42f;

	struct FProbe { FVector Offset; float W; };
	const FProbe Probes[] = {
		{ FVector::ZeroVector, 1.0f },
		{ Fwd * HalfLoa, 0.9f },
		{ -Fwd * HalfLoa, 0.9f },
		{ Right * HalfBeam, 0.75f },
		{ -Right * HalfBeam, 0.75f },
		{ Fwd * HalfLoa * 0.5f + Right * HalfBeam * 0.5f, 0.5f },
		{ Fwd * HalfLoa * 0.5f - Right * HalfBeam * 0.5f, 0.5f },
	};

	float SumZ = 0.f;
	float SumW = 0.f;
	FVector BowS = FVector::ZeroVector, SternS = FVector::ZeroVector;
	FVector PortS = FVector::ZeroVector, StbdS = FVector::ZeroVector;
	bool bBow = false, bStern = false, bPort = false, bStbd = false;

	for (int32 I = 0; I < UE_ARRAY_COUNT(Probes); ++I)
	{
		FVector Surf, Norm;
		if (!SampleWaterSurface(Loc + Probes[I].Offset, Surf, Norm)) continue;
		SumZ += Surf.Z * Probes[I].W;
		SumW += Probes[I].W;
		if (I == 1) { BowS = Surf; bBow = true; }
		if (I == 2) { SternS = Surf; bStern = true; }
		if (I == 3) { StbdS = Surf; bStbd = true; }
		if (I == 4) { PortS = Surf; bPort = true; }
	}

	if (SumW > 0.f)
	{
		OutTargetZ = SumZ / SumW;
	}

	if (bBow && bStern)
	{
		const float Span = FMath::Max(HalfLoa * 2.f, 1.f);
		OutWavePitchDeg = FMath::Clamp(
			FMath::RadiansToDegrees(FMath::Atan2(BowS.Z - SternS.Z, Span)),
			-MaxWavePitchDeg, MaxWavePitchDeg);
	}
	if (bPort && bStbd)
	{
		const float Span = FMath::Max(HalfBeam * 2.f, 1.f);
		// +roll = starboard down (UE roll)
		OutWaveRollDeg = FMath::Clamp(
			FMath::RadiansToDegrees(FMath::Atan2(StbdS.Z - PortS.Z, Span)),
			-MaxWaveRollDeg, MaxWaveRollDeg);
	}
}

void ASailBoatPawn::SetTrueWind(float SpeedKn, float DirDeg)
{
	// API is DISPLAY knots (matches HUD / sliders). Convert to physics for VPP.
	const float Phys = FBoatDynamics::WindPhysicsFromDisplay(
		FMath::Clamp(SpeedKn, 0.f, FBoatDynamics::WindDisplayMaxKn));
	Dynamics.TrueWindSpeedKn = FMath::Clamp(Phys, 0.f, FBoatDynamics::WindPhysicsMaxKn());
	float Dir = FMath::Fmod(DirDeg, 360.f);
	if (Dir < 0.f) Dir += 360.f;
	Dynamics.TrueWindDirDeg = Dir;
	// Base (synoptic) wind for the spatial field — puffs layer on top each tick.
	if (UWorld* World = GetWorld())
	{
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			Wind->SetBaseWind(Dynamics.TrueWindSpeedKn, Dynamics.TrueWindDirDeg);
		}
	}
}

void ASailBoatPawn::SetSailing(bool bEnabled)
{
	Dynamics.bSailing = bEnabled;
}

void ASailBoatPawn::AdjustTrueWindSpeed(float DeltaKn)
{
	// Delta and API are DISPLAY knots.
	float BasePhys = Dynamics.TrueWindSpeedKn;
	float BaseDir = Dynamics.TrueWindDirDeg;
	if (UWorld* World = GetWorld())
	{
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			BasePhys = Wind->BaseSpeedKn;
			BaseDir = Wind->BaseDirFromDeg;
		}
	}
	const float BaseDisp = FBoatDynamics::WindDisplayFromPhysics(BasePhys);
	SetTrueWind(BaseDisp + DeltaKn, BaseDir);
}

void ASailBoatPawn::AdjustTrueWindDir(float DeltaDeg)
{
	float BasePhys = Dynamics.TrueWindSpeedKn;
	float BaseDir = Dynamics.TrueWindDirDeg;
	if (UWorld* World = GetWorld())
	{
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			BasePhys = Wind->BaseSpeedKn;
			BaseDir = Wind->BaseDirFromDeg;
		}
	}
	const float BaseDisp = FBoatDynamics::WindDisplayFromPhysics(BasePhys);
	SetTrueWind(BaseDisp, BaseDir + DeltaDeg);
}

void ASailBoatPawn::OnToggleSailing()
{
	SetSailing(!Dynamics.bSailing);
}

void ASailBoatPawn::OnWindSpeedUp()
{
	AdjustTrueWindSpeed(1.f);
}

void ASailBoatPawn::OnWindSpeedDown()
{
	AdjustTrueWindSpeed(-1.f);
}

void ASailBoatPawn::OnWindDirLeft()
{
	// Counter-clockwise wind direction (met from)
	AdjustTrueWindDir(-5.f);
}

void ASailBoatPawn::OnWindDirRight()
{
	AdjustTrueWindDir(5.f);
}

FString ASailBoatPawn::GetBoatDisplayName() const
{
	if (!ActiveSpec.Name.IsEmpty()) return ActiveSpec.Name;
	if (const FBoatPreset* P = FBoatPresets::Find(ActivePresetId))
	{
		return P->DisplayName;
	}
	return ActivePresetId.IsEmpty() ? TEXT("Boat") : ActivePresetId;
}

bool ASailBoatPawn::SetBoatPreset(const FString& PresetId)
{
	const FBoatPreset* P = FBoatPresets::Find(PresetId);
	if (!P)
	{
		UE_LOG(LogSailSim, Warning, TEXT("SailBoatPawn: unknown preset '%s'"), *PresetId);
		return false;
	}
	ActivePresetId = P->Id;
	BoatJsonRelativePath = P->JsonRelativePath;
	const float KeepLoaScale = ActiveSpec.ScaleLoa;
	ActiveSpec = FBoatSpec::FromPresetId(P->Id);
	ActiveSpec.ScaleLoa = KeepLoaScale;
	ActiveSpec.ScaleBeam = FMath::Lerp(1.f, KeepLoaScale, 0.65f);
	ActiveSpec.ScaleDraft = FMath::Lerp(1.f, KeepLoaScale, 0.55f);
	ActiveSpec.ScaleSail = FMath::Square(FMath::Lerp(1.f, KeepLoaScale, 0.85f));
	// Force mesh rebuild even if previous path cached
	bLoftMeshLoaded = false;
	LoadedLoftPath.Reset();
	LoadLoftMesh(/*bApplyDynamics*/ true);
	ApplyActiveSpecToBoat();
	UE_LOG(LogSailSim, Log, TEXT("SailBoatPawn: switched preset to %s (%s)"), *P->DisplayName, *P->JsonRelativePath);
	return bLoftMeshLoaded;
}

void ASailBoatPawn::OnPreset1() { SetBoatPreset(TEXT("j105")); }
void ASailBoatPawn::OnPreset2() { SetBoatPreset(TEXT("endeavour")); }
void ASailBoatPawn::OnPreset3() { SetBoatPreset(TEXT("melges24")); }
void ASailBoatPawn::OnPreset4() { SetBoatPreset(TEXT("cruiser36")); }
void ASailBoatPawn::OnLoaScaleUp() { AdjustLiveLoaScale(0.05f); }
void ASailBoatPawn::OnLoaScaleDown() { AdjustLiveLoaScale(-0.05f); }
void ASailBoatPawn::OnRebuildLoft() { RebuildLoftFromOracle(); }

bool ASailBoatPawn::SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth) const
{
	// Prefer Phase-5 ocean subsystem (world-space height field).
	if (UWorld* World = GetWorld())
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			const FVector Q(WorldXY.X, WorldXY.Y, bFloatInit ? SmoothedWaterZ : WorldXY.Z);
			const FOceanSample S = Ocean->SampleOcean(Q);
			if (S.bValid)
			{
				OutSurface = S.Surface;
				OutNormal = S.Normal;
				if (OutDepth) *OutDepth = S.Depth;
				return true;
			}
		}
	}

	const FOceanSample S = FOceanHeightSample::SampleAt(
		GetWorld(), WorldXY, bFloatInit ? SmoothedWaterZ : 0.f, bFloatInit);
	if (!S.bValid) return false;
	OutSurface = S.Surface;
	OutNormal = S.Normal;
	if (OutDepth) *OutDepth = S.Depth;
	return true;
}

void ASailBoatPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// World-Partition can re-stream the map SailBoatPawn after GameMode cleanup.
	// Kill any boat that never became the player session boat.
	if (!IsPlayerControlled() && !bPlayerSessionBoat)
	{
		if (OrphanGraceFrames > 0)
		{
			--OrphanGraceFrames;
		}
		else
		{
			UE_LOG(LogSailSim, Log, TEXT("Destroying orphan/level boat %s"), *GetName());
			Destroy();
		}
		return;
	}

	if (!IsPlayerControlled())
	{
		return;
	}

	if (StartupSkipFrames > 0)
	{
		--StartupSkipFrames;
		SnapToWaterSurface(/*bForceXY*/ true);
		if (StartupSkipFrames == 3)
		{
			EnsureOceanCoverage();
		}
		// Keep sails settled during spawn skip (was skipping cloth → jib tack popped on first trim).
		UpdateBoomFromSheet(DeltaSeconds);
		UpdateSailCloth(DeltaSeconds);
		UpdateSpinAndSprit(DeltaSeconds);
		UpdateWindCompass(DeltaSeconds);
		RefreshChaseCamera();
		return;
	}

	// Re-snap Z for a few frames after zone rebuild (height field may lag one frame).
	if (WaterSnapFrames > 0)
	{
		--WaterSnapFrames;
		SnapToWaterSurface(/*bForceXY*/ WaterSnapFrames > 8);
	}

	if (CameraLagEnableFrames > 0)
	{
		--CameraLagEnableFrames;
		if (CameraLagEnableFrames == 0 && SpringArm)
		{
			SpringArm->bEnableCameraLag = true;
		}
	}

	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 1.f / 20.f);

	// Local true wind from spatial field (base + puffs: gusts, lifts, headers).
	// SampleSmoothedWindAt low-passes dir/speed so shifts build over tens of seconds
	// (realistic cadence) instead of jumping every frame through noise/puff edges.
	if (UWorld* World = GetWorld())
	{
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			// SetFocus only — ForceFocus rebuilds puffs (and used to log) every frame.
			Wind->SetFocus(GetActorLocation());
			const FWindSample S = Wind->SampleSmoothedWindAt(GetActorLocation(), Dt);
			Dynamics.TrueWindSpeedKn = S.SpeedKn;
			Dynamics.TrueWindDirDeg = S.DirFromDeg;
		}
	}

	// NAV pilot: refresh bearing to active waypoint each frame (web hNavUpdateTarget).
	if (Dynamics.bAutoHeading && Dynamics.AutoMode == FBoatDynamics::EAutoMode::Nav)
	{
		UpdateNavTarget();
	}

	// --- CPU profiling scopes (Settings → PERFORMANCE) ---
	{
		SAIL_PERF_SCOPE(Dynamics);
		// Sticky tiller (restored): A/D rates the held angle; release keeps it.
		// No snap-to-stop, no auto return to center. Tack owns rudder during maneuver.
		if (!Dynamics.bTacking)
		{
			if (FMath::Abs(HelmAxis) > 0.05f)
			{
				if (Dynamics.bAutoHeading)
				{
					// Hand on tiller — seamless take-over from pilot
					HeldHelmStarboardDeg = -Dynamics.Rudder;
				}
				const float HelmRateDegS = 35.f; // °/s at full A or D
				HeldHelmStarboardDeg = FMath::Clamp(
					HeldHelmStarboardDeg + HelmAxis * HelmRateDegS * Dt, -35.f, 35.f);
			}
			if (!Dynamics.bAutoHeading || FMath::Abs(HelmAxis) > 0.05f)
			{
				Dynamics.SetRudderStarboardPositive(HeldHelmStarboardDeg);
			}
		}

		if (FMath::Abs(SheetAxis) > 0.05f)
		{
			// W (+1) sheets in, S (−1) eases out — main boom max angle + jib lee sheet length
			Dynamics.SetSheetEase(Dynamics.SheetEase - SheetAxis * 0.55f * Dt);
		}
		if (FMath::Abs(OuthaulAxis) > 0.05f)
		{
			// O = on (increase), P = ease
			SetOuthaul(Dynamics.Outhaul01 + OuthaulAxis * 0.55f * Dt);
		}
		if (FMath::Abs(VangAxis) > 0.05f)
		{
			// V = hard on (decrease Vang01), B = ease off
			SetVang(Dynamics.Vang01 + VangAxis * 0.55f * Dt);
		}
		if (FMath::Abs(JibCarAxis) > 0.05f)
		{
			// I = aft, U = forward
			SetJibCar(Dynamics.JibCar01 + JibCarAxis * 0.45f * Dt);
		}
		if (FMath::Abs(JibLeechAxis) > 0.05f)
		{
			// M = tighter leech, N = ease
			SetJibLeechTension(Dynamics.JibLeechTension01 + JibLeechAxis * 0.55f * Dt);
		}
		Dynamics.Update(Dt);
	}

	// Water height / buoyancy samples (CPU) — separated from boat dynamics
	{
		SAIL_PERF_SCOPE(Water);
		ApplyDynamicsToTransform(Dt);
	}

	{
		SAIL_PERF_SCOPE(Rigging);
		// Free boom under wind + sheet constraint (not a direct position servo).
		UpdateBoomFromSheet(Dt);
		UpdateRunningRigging();
		UpdateStandingRigging();
		UpdateBoatLights();
		UpdateWindCompass(Dt);
	}

	{
		SAIL_PERF_SCOPE(Sails);
		UpdateSailCloth(Dt);
		UpdateSpinAndSprit(Dt);
	}
	// Instruments drawn by ASailSimHUD
	// Wake intentionally disabled (procedural patch looked wrong on flat water).
}

void ASailBoatPawn::UpdateWake(float DeltaSeconds)
{
	// Disabled for now — clear any leftover mesh from earlier sessions/hot-reload.
	(void)DeltaSeconds;
	BoatWake.bEnabled = false;
	BoatWake.Clear();
}

void ASailBoatPawn::ApplyDynamicsToTransform(float DeltaSeconds)
{
	const float Yaw = Dynamics.Heading;
	const float VppHeel = Dynamics.Phi;
	constexpr float FtToCm = 30.48f;
	const float CosH = FMath::Cos(FMath::DegreesToRadians(Dynamics.Heading));
	const float SinH = FMath::Sin(FMath::DegreesToRadians(Dynamics.Heading));
	const float Vx = (Dynamics.U * CosH - Dynamics.Vsway * SinH) * FtToCm;
	const float Vy = (Dynamics.U * SinH + Dynamics.Vsway * CosH) * FtToCm;

	FVector Loc = GetActorLocation();
	Loc.X += Vx * DeltaSeconds;
	Loc.Y += Vy * DeltaSeconds;

	float TargetZ = bFloatInit ? SmoothedWaterZ : WaterSurfaceZ;
	float WavePitch = CachedWavePitch;
	float WaveRoll = CachedWaveRoll;

	WaveSampleTimer -= DeltaSeconds;
	if (bMultiPointBuoyancy && WaveSampleTimer <= 0.f)
	{
		WaveSampleTimer = WaveSampleInterval;
		SampleMultiPointBuoyancy(Loc, CosH, SinH, TargetZ, WavePitch, WaveRoll);
		CachedWavePitch = WavePitch;
		CachedWaveRoll = WaveRoll;
	}
	else if (!bMultiPointBuoyancy)
	{
		FVector CenterSurf, CenterN;
		if (SampleWaterSurface(Loc, CenterSurf, CenterN))
		{
			TargetZ = CenterSurf.Z;
		}
	}
	else
	{
		// Keep last wave samples; still track mean Z cheaply at center
		FVector CenterSurf, CenterN;
		if (SampleWaterSurface(Loc, CenterSurf, CenterN))
		{
			TargetZ = FMath::Lerp(TargetZ, CenterSurf.Z, 0.35f);
		}
	}

	const float DesiredZ = TargetZ + WaterlineOffsetCm;

	if (!bFloatInit)
	{
		SmoothedWaterZ = TargetZ;
		Loc.Z = DesiredZ;
		VerticalVelZ = 0.f;
		bFloatInit = true;
	}
	else
	{
		// Soft spring buoyancy (critically-ish damped toward DesiredZ)
		const float Err = DesiredZ - Loc.Z;
		const float Accel = Err * BuoyancyStiffness - VerticalVelZ * BuoyancyDamping;
		VerticalVelZ += Accel * DeltaSeconds;
		const float MaxV = MaxWaterZSpeedCm;
		VerticalVelZ = FMath::Clamp(VerticalVelZ, -MaxV, MaxV);
		Loc.Z += VerticalVelZ * DeltaSeconds;
		SmoothedWaterZ = Loc.Z - WaterlineOffsetCm;
	}

	SmoothedPitch = FMath::Lerp(SmoothedPitch, CachedWavePitch,
		1.f - FMath::Exp(-WavePitchSmoothRate * DeltaSeconds));
	SmoothedWaveRoll = FMath::Lerp(SmoothedWaveRoll, CachedWaveRoll,
		1.f - FMath::Exp(-WaveRollSmoothRate * DeltaSeconds));

	// VPP heel is authoritative; waves add high-frequency roll
	const float Roll = VppHeel + SmoothedWaveRoll * WaveRollGain;
	SetActorLocationAndRotation(Loc, FRotator(SmoothedPitch, Yaw, Roll), false, nullptr, ETeleportType::None);
}


