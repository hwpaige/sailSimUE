#include "Sailing/SailBoatPawn.h"
#include "Sailing/BoatMeshFromJson.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "Components/InputComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "Misc/Paths.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;

	BoatRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BoatRoot"));
	RootComponent = BoatRoot;

	LoftMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LoftMesh"));
	LoftMesh->SetupAttachment(BoatRoot);
	LoftMesh->bUseAsyncCooking = true;
	LoftMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	LoftMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	LoftMesh->SetGenerateOverlapEvents(false);
	LoftMesh->SetMobility(EComponentMobility::Movable);
	LoftMesh->SetCastShadow(true);
	LoftMesh->bNeverDistanceCull = true;
	LoftMesh->SetReceivesDecals(false);

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
	// Closer chase cam so ~10 m LOA loft fills the frame (was 24 m arm).
	SpringArm->TargetArmLength = 1600.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bInheritPitch = true;
	SpringArm->bInheritYaw = true;
	SpringArm->bInheritRoll = false; // keep horizon level when boat heels
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 10.f;
	SpringArm->bEnableCameraRotationLag = false; // snappier orbit response
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 220.f));
	OrbitYawDeg = -25.f;
	OrbitPitchDeg = -18.f;
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
		// Keep chase cam proportional to boat length
		if (SpringArm)
		{
			SpringArm->TargetArmLength = FMath::Clamp(HullLengthCm * 1.5f, 900.f, 2800.f);
		}
	}
	UE_LOG(LogTemp, Log,
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
	const bool bOk = FBoatMeshFromJson::LoadIntoProceduralMesh(LoftMesh, Path, BaseMat, &Result);
	if (!bOk)
	{
		bLoftMeshLoaded = false;
		LoadedLoftPath.Reset();
		UE_LOG(LogTemp, Warning, TEXT("SailBoatPawn: loft JSON failed (%s) — no hull mesh"), *Path);
		return;
	}

	bLoftMeshLoaded = true;
	LoadedLoftPath = Path;
	CachedSailingParams = Result.Sailing;
	LoftMesh->SetVisibility(true);
	LoftMesh->SetHiddenInGame(false);
	LoftMesh->MarkRenderStateDirty();

	if (Result.Spars.bMastValid)
	{
		PlaceSparFromEndpoints(MastMesh, Result.Spars.MastBase, Result.Spars.MastTop);
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

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();
	// Defaults first; LoadLoftMesh may override from boat3d sailing block.
	Dynamics.InitJ105();
	Dynamics.Heading = 90.f;
	Dynamics.AutoTarget = Dynamics.Heading;

	LoadLoftMesh(/*bApplyDynamics*/ true);

	EnsureOpenWaterSpawn();

	FVector Loc = GetActorLocation();
	FVector Surf, Norm;
	if (SampleWaterSurface(Loc, Surf, Norm))
	{
		SmoothedWaterZ = Surf.Z;
	}
	else
	{
		SmoothedWaterZ = WaterSurfaceZ;
	}
	bFloatInit = true;
	Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;
	SetActorLocation(Loc, false, nullptr, ETeleportType::TeleportPhysics);
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
	if (!bBoomEndpointsValid || !BoomMesh) return;
	// Swing boom out to leeward as sheet eases (visual only until cloth)
	const float SwingDeg = Dynamics.SheetEase * 55.f;
	const float Sign = Dynamics.GetApparentWindAngleDeg() >= 0.f ? 1.f : -1.f;
	const FVector LocalEnd = BoomEndLoc - BoomBaseLoc;
	const FQuat Q(FVector::UpVector, FMath::DegreesToRadians(Sign * SwingDeg));
	const FVector Swung = BoomBaseLoc + Q.RotateVector(LocalEnd);
	PlaceSparFromEndpoints(BoomMesh, BoomBaseLoc, Swung);
}

void ASailBoatPawn::SetTrueWind(float SpeedKn, float DirDeg)
{
	Dynamics.TrueWindSpeedKn = SpeedKn;
	Dynamics.TrueWindDirDeg = DirDeg;
}

bool ASailBoatPawn::SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth) const
{
	UWorld* World = GetWorld();
	if (!World) return false;

	const FVector Query(WorldXY.X, WorldXY.Y, 50000.f);
	bool bAny = false;
	float BestAbsDZ = TNumericLimits<float>::Max();
	float BestDepth = 0.f;

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp) continue;
		FVector Surf, Norm, Vel;
		float Depth = 0.f;
		if (Comp->GetWaterSurfaceInfoAtLocation(Query, Surf, Norm, Vel, Depth, false))
		{
			const float Dz = FMath::Abs(Surf.Z - (bFloatInit ? SmoothedWaterZ : 0.f));
			if (!bAny || Dz < BestAbsDZ)
			{
				BestAbsDZ = Dz;
				BestDepth = Depth;
				OutSurface = Surf;
				OutNormal = Norm;
				bAny = true;
			}
		}
	}
	if (bAny && OutDepth) *OutDepth = BestDepth;
	return bAny;
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
		FVector Loc = GetActorLocation();
		// Re-assert open water on first frames (PlayerStart may reset XY)
		EnsureOpenWaterSpawn();
		Loc = GetActorLocation();
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
	DrawHud();
}

void ASailBoatPawn::ApplyDynamicsToTransform(float DeltaSeconds)
{
	const float Yaw = Dynamics.Heading;
	const float Heel = Dynamics.Phi;
	constexpr float FtToCm = 30.48f;
	const float CosH = FMath::Cos(FMath::DegreesToRadians(Dynamics.Heading));
	const float SinH = FMath::Sin(FMath::DegreesToRadians(Dynamics.Heading));
	const float Vx = (Dynamics.U * CosH - Dynamics.Vsway * SinH) * FtToCm;
	const float Vy = (Dynamics.U * SinH + Dynamics.Vsway * CosH) * FtToCm;

	FVector Loc = GetActorLocation();
	Loc.X += Vx * DeltaSeconds;
	Loc.Y += Vy * DeltaSeconds;

	const float HalfLoa = HullLengthCm * 0.45f;
	FVector CenterSurf, CenterN;
	float TargetZ = bFloatInit ? SmoothedWaterZ : WaterSurfaceZ;
	if (SampleWaterSurface(Loc, CenterSurf, CenterN))
	{
		TargetZ = CenterSurf.Z;
	}

	if (!bFloatInit)
	{
		SmoothedWaterZ = TargetZ;
		bFloatInit = true;
	}
	else
	{
		const float MaxStep = MaxWaterZSpeedCm * DeltaSeconds;
		SmoothedWaterZ += FMath::Clamp(TargetZ - SmoothedWaterZ, -MaxStep, MaxStep);
	}

	WavePitchSampleTimer -= DeltaSeconds;
	if (bSampleWavePitch && WavePitchSampleTimer <= 0.f)
	{
		WavePitchSampleTimer = WavePitchSampleInterval;
		const FVector Fwd(CosH, SinH, 0.f);
		FVector BowS, BowN, SternS, SternN;
		if (SampleWaterSurface(Loc + Fwd * HalfLoa, BowS, BowN) &&
			SampleWaterSurface(Loc - Fwd * HalfLoa, SternS, SternN))
		{
			const float DZ = BowS.Z - SternS.Z;
			CachedWavePitch = FMath::Clamp(
				FMath::RadiansToDegrees(FMath::Atan2(DZ, HalfLoa * 2.f)), -8.f, 8.f);
		}
	}
	SmoothedPitch = FMath::Lerp(SmoothedPitch, CachedWavePitch,
		1.f - FMath::Exp(-PitchSmoothRate * DeltaSeconds));

	Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;
	SetActorLocationAndRotation(Loc, FRotator(SmoothedPitch, Yaw, Heel), false, nullptr, ETeleportType::None);
}

void ASailBoatPawn::DrawHud() const
{
	if (!GEngine || !IsPlayerControlled()) return;
	const FString Line = FString::Printf(
		TEXT("SPD %.1f kn   HEEL %.0f°   HDG %.0f°   RUD %.0f°   SHEET %.0f%%   AWA %.0f°   AWS %.1f   TWS %.0f@%.0f   %s"),
		Dynamics.GetSpeedKnots(),
		Dynamics.Phi,
		Dynamics.Heading,
		-Dynamics.Rudder,
		Dynamics.SheetEase * 100.f,
		Dynamics.GetApparentWindAngleDeg(),
		Dynamics.GetApparentWindSpeedKn(),
		Dynamics.TrueWindSpeedKn,
		Dynamics.TrueWindDirDeg,
		Dynamics.bAutoHeading ? TEXT("AUTO") : TEXT("HELM"));
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan, Line);
	GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White,
		TEXT("A/D helm | W/S sheet | RMB+drag orbit | scroll zoom"));
}
