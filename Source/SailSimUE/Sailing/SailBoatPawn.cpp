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
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
	SpringArm->TargetArmLength = 2400.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 10.f;
	SpringArm->bEnableCameraRotationLag = true;
	SpringArm->CameraRotationLagSpeed = 12.f;
	SpringArm->SetRelativeRotation(FRotator(-12.f, 0.f, 0.f));
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 180.f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
}

void ASailBoatPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	StartupSkipFrames = 3;
}

void ASailBoatPawn::LoadLoftMesh()
{
	UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	const FString Path = FPaths::ProjectContentDir() / BoatJsonRelativePath;
	const bool bOk = FBoatMeshFromJson::LoadIntoProceduralMesh(LoftMesh, Path, BaseMat);
	if (!bOk)
	{
		UE_LOG(LogTemp, Warning, TEXT("SailBoatPawn: loft JSON failed (%s) — no hull mesh"), *Path);
		return;
	}

	// Spars from JSON endpoints
	FString JsonStr;
	if (FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			const TSharedPtr<FJsonObject>* Spars = nullptr;
			if (Root->TryGetObjectField(TEXT("spars"), Spars) && Spars && (*Spars).IsValid())
			{
				auto PlaceSpar = [](UStaticMeshComponent* Comp, const TSharedPtr<FJsonObject>& Spar,
					const TCHAR* AName, const TCHAR* BName)
				{
					if (!Comp || !Spar.IsValid()) return;
					const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* B = nullptr;
					if (!Spar->TryGetArrayField(AName, A) || !Spar->TryGetArrayField(BName, B)) return;
					if (A->Num() < 3 || B->Num() < 3) return;
					const FVector PA((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
					const FVector PB((*B)[0]->AsNumber(), (*B)[1]->AsNumber(), (*B)[2]->AsNumber());
					const FVector Mid = (PA + PB) * 0.5f;
					const FVector Dir = (PB - PA);
					const float Len = Dir.Size();
					if (Len < 1.f) return;
					Comp->SetVisibility(true);
					Comp->SetRelativeLocation(Mid);
					// Cylinder default axis is +Z in UE? Actually UE cylinder is Z-up height.
					// BasicShapes Cylinder is along Z. Scale Z = length/100.
					Comp->SetRelativeScale3D(FVector(0.12f, 0.12f, Len / 100.f));
					Comp->SetRelativeRotation(FRotationMatrix::MakeFromZ(Dir.GetSafeNormal()).Rotator());
				};

				const TSharedPtr<FJsonObject>* Mast = nullptr;
				const TSharedPtr<FJsonObject>* Boom = nullptr;
				if ((*Spars)->TryGetObjectField(TEXT("mast"), Mast) && Mast)
				{
					PlaceSpar(MastMesh, *Mast, TEXT("base"), TEXT("top"));
				}
				if ((*Spars)->TryGetObjectField(TEXT("boom"), Boom) && Boom)
				{
					PlaceSpar(BoomMesh, *Boom, TEXT("base"), TEXT("end"));
				}
			}
		}
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
	Dynamics.InitJ105();
	Dynamics.Heading = 90.f;
	Dynamics.AutoTarget = Dynamics.Heading;

	LoadLoftMesh();

	// Tint spars
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
	Tint(MastMesh, FLinearColor(0.7f, 0.7f, 0.72f));
	Tint(BoomMesh, FLinearColor(0.7f, 0.7f, 0.72f));

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
}

void ASailBoatPawn::OnMoveRight(float Value)
{
	HelmAxis = FMath::Clamp(Value, -1.f, 1.f);
	SetHelmInput(HelmAxis * 35.f);
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
		TEXT("SPD %.1f kn   HEEL %.0f°   HDG %.0f°   RUD %.0f°   AWA %.0f°   AWS %.1f   TWS %.0f@%.0f   XY(%.0f,%.0f)   %s"),
		Dynamics.GetSpeedKnots(),
		Dynamics.Phi,
		Dynamics.Heading,
		-Dynamics.Rudder,
		Dynamics.GetApparentWindAngleDeg(),
		Dynamics.GetApparentWindSpeedKn(),
		Dynamics.TrueWindSpeedKn,
		Dynamics.TrueWindDirDeg,
		GetActorLocation().X,
		GetActorLocation().Y,
		Dynamics.bAutoHeading ? TEXT("AUTO") : TEXT("HELM"));
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan, Line);
	GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White,
		TEXT("A/D helm | center=AUTO | mesh = sail_geom J/105 loft"));
}
