#include "Sailing/SailBoatPawn.h"
#include "Sailing/BoatMeshFromJson.h"
#include "Sailing/BoatPresets.h"
#include "Sailing/OceanHeightSample.h"
#include "SailSimUE.h"
#include "WaterZoneActor.h"
#include "EngineUtils.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/InputComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;

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

	MainSailMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MainSailMesh"));
	MainSailMesh->SetupAttachment(BoatRoot);
	PrepLoft(MainSailMesh);

	JibSailMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("JibSailMesh"));
	JibSailMesh->SetupAttachment(BoatRoot);
	PrepLoft(JibSailMesh);

	HullCollision = CreateDefaultSubobject<UBoxComponent>(TEXT("HullCollision"));
	HullCollision->SetupAttachment(BoatRoot);
	HullCollision->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	HullCollision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HullCollision->SetGenerateOverlapEvents(false);
	HullCollision->SetBoxExtent(FVector(500.f, 160.f, 120.f));
	HullCollision->SetRelativeLocation(FVector(0.f, 0.f, -40.f));
	HullCollision->SetCanEverAffectNavigation(false);

	MastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mast"));
	MastMesh->SetupAttachment(BoatRoot);
	BoomMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Boom"));
	BoomMesh->SetupAttachment(BoatRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	UStaticMesh* Cyl = CylMesh.Succeeded() ? CylMesh.Object : nullptr;
	UMaterialInterface* BaseMat = BasicMat.Succeeded() ? BasicMat.Object : nullptr;

	auto SetupCyl = [BaseMat, Cyl](UStaticMeshComponent* Comp)
	{
		if (!Comp) return;
		if (Cyl) Comp->SetStaticMesh(Cyl);
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCastShadow(true);
		if (BaseMat) Comp->SetMaterial(0, BaseMat);
	};
	SetupCyl(MastMesh);
	SetupCyl(BoomMesh);
	MastMesh->SetVisibility(false);
	BoomMesh->SetVisibility(false);

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(BoatRoot);
	// Higher / farther chase so ocean horizon is visible under dusk lighting.
	SpringArm->TargetArmLength = 2200.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bInheritPitch = true;
	SpringArm->bInheritYaw = true;
	SpringArm->bInheritRoll = false; // keep horizon level when boat heels
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 10.f;
	SpringArm->bEnableCameraRotationLag = false; // snappier orbit response
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 320.f));
	OrbitYawDeg = -35.f;
	OrbitPitchDeg = -22.f;
	ApplyOrbitToSpringArm();

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
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
	StartupSkipFrames = 3;
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
	HullCollision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
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

void ASailBoatPawn::ApplyCachedSailingToDynamics()
{
	if (!bApplyJsonSailingParams || !CachedSailingParams.bValid)
	{
		return;
	}
	const FBoatJsonSailingParams& S = CachedSailingParams;
	Dynamics.ApplySailingParams(
		S.DispLb, S.BallastLb, S.BeamFt, S.LwlFt,
		S.DraftFt, S.TcFt, S.LateralArea, S.KeelArea,
		S.RudderArea, S.KeelSpan, S.ClrX, S.ClrZ,
		S.SaTotal, S.HullSpeedKn, S.GmFt,
		S.LoaFt, S.MastTopFt);
	Dynamics.Heading = 90.f;
	Dynamics.AutoTarget = Dynamics.Heading;
	if (S.LoaFt > 1.f)
	{
		HullLengthCm = S.LoaFt * 30.48f;
		if (SpringArm)
		{
			SpringArm->TargetArmLength = FMath::Clamp(HullLengthCm * 1.5f, 900.f, 2800.f);
		}
	}
	if (S.BeamFt > 1.f)
	{
		HullBeamCm = S.BeamFt * 30.48f;
	}
	UE_LOG(LogSailSim, Log,
		TEXT("SailBoatPawn: dynamics from JSON disp=%.0f lb SA=%.0f LOA=%.1f ft"),
		S.DispLb, S.SaTotal, S.LoaFt);
}

void ASailBoatPawn::LoadLoftMesh(bool bApplyDynamics)
{
	if (!LoftMesh)
	{
		return;
	}

	const FString Path = FPaths::ProjectContentDir() / BoatJsonRelativePath;

	// Skip full mesh rebuild if already loaded for this path (PIE: OnConstruction then BeginPlay)
	if (bLoftMeshLoaded && LoadedLoftPath == Path)
	{
		if (bApplyDynamics)
		{
			if (!CachedSailingParams.bValid)
			{
				FBoatJsonLoadResult Meta;
				if (FBoatMeshFromJson::LoadMetadataOnly(Path, Meta))
				{
					CachedSailingParams = Meta.Sailing;
				}
			}
			ApplyCachedSailingToDynamics();
		}
		return;
	}

	UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}

	FBoatJsonLoadResult Result;
	const bool bOk = FBoatMeshFromJson::LoadIntoProceduralMesh(
		LoftMesh, Path, BaseMat, &Result, MainSailMesh, JibSailMesh);
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
	LoftMesh->SetVisibility(true);
	LoftMesh->SetHiddenInGame(false);
	LoftMesh->MarkRenderStateDirty();
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
		bBoomEndpointsValid = true;
		PlaceSparFromEndpoints(BoomMesh, BoomBaseLoc, BoomEndLoc);
	}
	else
	{
		bBoomEndpointsValid = false;
	}
	UpdateBoomFromSheet();

	MainCloth.Clear();
	JibCloth.Clear();
	if (bEnableSailCloth)
	{
		if (MainSailMesh) MainCloth.BuildFromMesh(MainSailMesh, 0);
		if (JibSailMesh) JibCloth.BuildFromMesh(JibSailMesh, 0);
	}
	UpdateHullCollisionFromMesh();

	auto Tint = [](UStaticMeshComponent* Comp, FLinearColor Color)
	{
		if (!Comp || !Comp->IsVisible()) return;
		UMaterialInterface* Base = Comp->GetMaterial(0);
		if (!Base) return;
		UMaterialInstanceDynamic* Mid = Comp->CreateAndSetMaterialInstanceDynamic(0);
		if (!Mid) return;
		Mid->SetVectorParameterValue(TEXT("Color"), Color);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Color);
	};
	Tint(MastMesh, FLinearColor(0.55f, 0.56f, 0.58f));
	Tint(BoomMesh, FLinearColor(0.55f, 0.56f, 0.58f));

	if (bApplyDynamics)
	{
		ApplyCachedSailingToDynamics();
	}
}

void ASailBoatPawn::EnsureOpenWaterSpawn()
{
	FVector Loc = GetActorLocation();
	const float Dist2D = FVector2D(Loc.X, Loc.Y).Size();
	if (bForceOpenWaterSpawn || Dist2D < OriginIslandRadiusCm)
	{
		Loc.X = OpenWaterSpawnXY.X;
		Loc.Y = OpenWaterSpawnXY.Y;
		SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void ASailBoatPawn::EnsureOceanCoverage()
{
	// Intentionally a no-op at runtime.
	// Moving / resizing Static WaterZone or Ocean components at PIE spams
	// "Mobility has to be 'Movable'" and can break tessellation. Zone extent
	// was enlarged in the map (see PLAN.md); boat spawns near origin so it
	// is already covered. Enable only for diagnostics if debugging water.
	if (!bEnsureOceanCoverage) return;
	// Optional one-shot presence check (no actor mutation).
	static bool bLoggedOnce = false;
	if (bLoggedOnce) return;
	bLoggedOnce = true;
	UWorld* World = GetWorld();
	if (!World) return;
	int32 Zones = 0;
	for (TActorIterator<AWaterZone> It(World); It; ++It)
	{
		++Zones;
	}
	if (Zones == 0)
	{
		UE_LOG(LogSailSim, Warning, TEXT("No AWaterZone in level — ocean mesh will not render"));
	}
	else
	{
		UE_LOG(LogSailSim, Verbose, TEXT("WaterZone present (%d) — not mutating (Static mobility)"), Zones);
	}
}

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();
	// Defaults first; LoadLoftMesh may override from boat3d sailing block.
	Dynamics.InitJ105();
	Dynamics.Heading = 90.f;
	Dynamics.AutoTarget = Dynamics.Heading;

	LoadLoftMesh(/*bApplyDynamics*/ true);

	EnsureOpenWaterSpawn();
	EnsureOceanCoverage();

	FVector Loc = GetActorLocation();
	FVector Surf, Norm;
	if (SampleWaterSurface(Loc, Surf, Norm))
	{
		SmoothedWaterZ = Surf.Z;
		// Sample a second point to log whether wave height varies (buoyancy diagnostic)
		FVector Surf2, Norm2;
		const bool b2 = SampleWaterSurface(Loc + FVector(400.f, 0.f, 0.f), Surf2, Norm2);
		UE_LOG(LogSailSim, Log,
			TEXT("SailBoatPawn: water at XY(%.0f,%.0f) Z=%.1f  neighbor dZ=%.1f (waves %s)"),
			Loc.X, Loc.Y, Surf.Z,
			b2 ? (Surf2.Z - Surf.Z) : 0.f,
			(b2 && FMath::Abs(Surf2.Z - Surf.Z) > 0.5f) ? TEXT("OK") : TEXT("flat/fallback"));
	}
	else
	{
		SmoothedWaterZ = WaterSurfaceZ;
		UE_LOG(LogSailSim, Warning,
			TEXT("SailBoatPawn: NO water surface at XY(%.0f,%.0f) — floating at Z=%.1f (void risk)"),
			Loc.X, Loc.Y, SmoothedWaterZ);
	}
	bFloatInit = true;
	VerticalVelZ = 0.f;
	Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;
	SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);

	FBoatDynamics::RunGoldenSelfCheck();
}

void ASailBoatPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ASailBoatPawn::OnMoveRight);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &ASailBoatPawn::OnMoveRight);
	PlayerInputComponent->BindAxis(TEXT("Sheet"), this, &ASailBoatPawn::OnSheetAxis);
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
}

void ASailBoatPawn::ApplyOrbitToSpringArm()
{
	if (!SpringArm) return;
	OrbitPitchDeg = FMath::Clamp(OrbitPitchDeg, MinOrbitPitchDeg, MaxOrbitPitchDeg);
	// Normalize yaw to keep values sane
	OrbitYawDeg = FMath::UnwindDegrees(OrbitYawDeg);
	SpringArm->SetRelativeRotation(FRotator(OrbitPitchDeg, OrbitYawDeg, 0.f));
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
	HelmAxis = FMath::Clamp(Value, -1.f, 1.f);
	SetHelmInput(HelmAxis * 35.f);
}

void ASailBoatPawn::OnSheetAxis(float Value)
{
	// Hold W to sheet in, S to ease (rate-limited via continuous axis)
	SheetAxis = FMath::Clamp(Value, -1.f, 1.f);
}

void ASailBoatPawn::SetHelmInput(float StarboardPositive)
{
	Dynamics.SetRudderStarboardPositive(StarboardPositive);
	if (FMath::Abs(StarboardPositive) < 0.5f && Dynamics.bManualHelm)
	{
		Dynamics.bManualHelm = false;
		Dynamics.bAutoHeading = true;
		Dynamics.AutoTarget = Dynamics.Heading;
		Dynamics.AutoI = 0.f;
	}
}

void ASailBoatPawn::SetSheetEase(float Ease01)
{
	Dynamics.SetSheetEase(Ease01);
	UpdateBoomFromSheet();
}

void ASailBoatPawn::UpdateBoomFromSheet()
{
	// Rigid boom + sail-root yaw (cloth deforms on top when enabled)
	const float SwingDeg = Dynamics.SheetEase * 55.f;
	const float Sign = Dynamics.GetApparentWindAngleDeg() >= 0.f ? 1.f : -1.f;
	const float Yaw = Sign * SwingDeg;

	if (bBoomEndpointsValid && BoomMesh)
	{
		const FVector LocalEnd = BoomEndLoc - BoomBaseLoc;
		const FQuat Q(FVector::UpVector, FMath::DegreesToRadians(Yaw));
		const FVector Swung = BoomBaseLoc + Q.RotateVector(LocalEnd);
		PlaceSparFromEndpoints(BoomMesh, BoomBaseLoc, Swung);
	}

	if (MainSailMesh)
	{
		if (bMastPivotValid) MainSailMesh->SetRelativeLocation(MastBaseLoc);
		// When cloth is active, keep yaw small — cloth handles fill; else rigid sheet
		const float RigidYaw = (bEnableSailCloth && MainCloth.bInitialized) ? Yaw * 0.25f : Yaw;
		MainSailMesh->SetRelativeRotation(FRotator(0.f, RigidYaw, 0.f));
	}
	if (JibSailMesh)
	{
		if (bMastPivotValid) JibSailMesh->SetRelativeLocation(MastBaseLoc);
		const float RigidYaw = (bEnableSailCloth && JibCloth.bInitialized) ? Yaw * 0.2f : Yaw * 0.9f;
		JibSailMesh->SetRelativeRotation(FRotator(0.f, RigidYaw, 0.f));
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
	int32 N = 0;
	if (MainCloth.bInitialized)
	{
		Q += MainCloth.LastFillQuality * 0.6f;
		++N;
	}
	if (JibCloth.bInitialized)
	{
		Q += JibCloth.LastFillQuality * 0.4f;
		++N;
	}
	if (N == 0)
	{
		Dynamics.ClothForceScale = 1.f;
		return;
	}
	// Main+jib weighted; if only one built, renormalize
	const float W = (MainCloth.bInitialized ? 0.6f : 0.f) + (JibCloth.bInitialized ? 0.4f : 0.f);
	Dynamics.ClothForceScale = (W > 0.f) ? (Q / W) : 1.f;
}

void ASailBoatPawn::UpdateSailCloth(float DeltaSeconds)
{
	if (!bEnableSailCloth) return;

	const float SwingDeg = Dynamics.SheetEase * 55.f;
	const float Sign = Dynamics.GetApparentWindAngleDeg() >= 0.f ? 1.f : -1.f;
	const float Yaw = Sign * SwingDeg;

	// Apparent wind in boat frame (approx): from AWA
	const float Awa = Dynamics.GetApparentWindAngleDeg();
	const FVector WindBoat(
		FMath::Cos(FMath::DegreesToRadians(Awa)),
		FMath::Sin(FMath::DegreesToRadians(Awa)),
		0.f);

	auto StepOne = [&](FSailClothSim& Cloth, UProceduralMeshComponent* Mesh, float YawScale)
	{
		if (!Cloth.bInitialized || !Mesh) return;
		const FTransform SailRel(FRotator(0.f, Yaw * YawScale, 0.f),
			bMastPivotValid ? MastBaseLoc : FVector::ZeroVector);
		// Boom tip in boat space → sail local
		FVector BoomTipBoat = BoomEndLoc;
		if (bBoomEndpointsValid)
		{
			const FQuat Q(FVector::UpVector, FMath::DegreesToRadians(Yaw));
			BoomTipBoat = BoomBaseLoc + Q.RotateVector(BoomEndLoc - BoomBaseLoc);
		}
		const FVector ClewLocal = SailRel.InverseTransformPosition(BoomTipBoat);
		const FVector WindLocal = SailRel.InverseTransformVectorNoScale(WindBoat).GetSafeNormal();
		Cloth.Step(DeltaSeconds, WindLocal, Dynamics.GetApparentWindSpeedKn(), ClewLocal, Dynamics.SheetEase);
		Cloth.PushToMesh(Mesh);
	};

	StepOne(MainCloth, MainSailMesh, 0.25f);
	// Jib: aim clew slightly forward of boom base
	if (JibCloth.bInitialized && JibSailMesh)
	{
		const FTransform SailRel(FRotator(0.f, Yaw * 0.2f, 0.f),
			bMastPivotValid ? MastBaseLoc : FVector::ZeroVector);
		FVector JibClewBoat = MastBaseLoc + FVector(-HullLengthCm * 0.15f, Sign * HullBeamCm * 0.35f * Dynamics.SheetEase, HullLengthCm * 0.08f);
		if (bMastPivotValid)
		{
			JibClewBoat = MastBaseLoc + FVector(-200.f, Sign * (80.f + Dynamics.SheetEase * 220.f), 250.f);
		}
		const FVector ClewLocal = SailRel.InverseTransformPosition(JibClewBoat);
		const FVector WindLocal = SailRel.InverseTransformVectorNoScale(WindBoat).GetSafeNormal();
		JibCloth.Step(DeltaSeconds, WindLocal, Dynamics.GetApparentWindSpeedKn(), ClewLocal, Dynamics.SheetEase);
		JibCloth.PushToMesh(JibSailMesh);
	}

	ApplyClothForceToDynamics();
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
	Dynamics.TrueWindSpeedKn = FMath::Clamp(SpeedKn, 0.f, 60.f);
	float Dir = FMath::Fmod(DirDeg, 360.f);
	if (Dir < 0.f) Dir += 360.f;
	Dynamics.TrueWindDirDeg = Dir;
}

void ASailBoatPawn::SetSailing(bool bEnabled)
{
	Dynamics.bSailing = bEnabled;
}

void ASailBoatPawn::AdjustTrueWindSpeed(float DeltaKn)
{
	SetTrueWind(Dynamics.TrueWindSpeedKn + DeltaKn, Dynamics.TrueWindDirDeg);
}

void ASailBoatPawn::AdjustTrueWindDir(float DeltaDeg)
{
	SetTrueWind(Dynamics.TrueWindSpeedKn, Dynamics.TrueWindDirDeg + DeltaDeg);
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
	// Force mesh rebuild even if previous path cached
	bLoftMeshLoaded = false;
	LoadedLoftPath.Reset();
	LoadLoftMesh(/*bApplyDynamics*/ true);
	UE_LOG(LogSailSim, Log, TEXT("SailBoatPawn: switched preset to %s (%s)"), *P->DisplayName, *P->JsonRelativePath);
	return bLoftMeshLoaded;
}

void ASailBoatPawn::OnPreset1() { SetBoatPreset(TEXT("j105")); }
void ASailBoatPawn::OnPreset2() { SetBoatPreset(TEXT("endeavour")); }
void ASailBoatPawn::OnPreset3() { SetBoatPreset(TEXT("melges24")); }
void ASailBoatPawn::OnPreset4() { SetBoatPreset(TEXT("cruiser36")); }

bool ASailBoatPawn::SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth) const
{
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

	if (!IsPlayerControlled())
	{
		return;
	}

	if (StartupSkipFrames > 0)
	{
		--StartupSkipFrames;
		// Re-assert open water on first frames (PlayerStart may reset XY)
		EnsureOpenWaterSpawn();
		if (StartupSkipFrames == 2)
		{
			EnsureOceanCoverage(); // diagnostic only when enabled
		}
		FVector Loc = GetActorLocation();
		FVector Surf, Norm;
		if (SampleWaterSurface(Loc, Surf, Norm))
		{
			SmoothedWaterZ = Surf.Z;
			Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;
			SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
		}
		return;
	}

	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 1.f / 20.f);
	if (FMath::Abs(SheetAxis) > 0.05f)
	{
		// W (+1) sheets in, S (−1) eases out
		Dynamics.SetSheetEase(Dynamics.SheetEase - SheetAxis * 0.45f * Dt);
		UpdateBoomFromSheet();
	}
	Dynamics.Update(Dt);
	ApplyDynamicsToTransform(Dt);
	UpdateSailCloth(Dt);
	// Instruments drawn by ASailSimHUD
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


