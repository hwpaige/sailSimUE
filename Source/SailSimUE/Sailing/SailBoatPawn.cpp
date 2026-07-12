#include "Sailing/SailBoatPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "Components/InputComponent.h"
#include "EngineUtils.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HullMesh"));
	RootComponent = HullMesh;
	HullMesh->SetSimulatePhysics(false);
	HullMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HullMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		HullMesh->SetStaticMesh(CubeMesh.Object);
	}

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 3200.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 3.f;
	SpringArm->bEnableCameraRotationLag = true;
	SpringArm->CameraRotationLagSpeed = 4.f;
	SpringArm->SetRelativeRotation(FRotator(-16.f, 0.f, 0.f));
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 280.f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);

	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();
	Dynamics.InitJ105();

	const float ScaleX = HullLengthCm / 100.f;
	const float ScaleY = (HullLengthCm * 0.32f) / 100.f;
	const float ScaleZ = (HullLengthCm * 0.18f) / 100.f;
	HullMesh->SetWorldScale3D(FVector(ScaleX, ScaleY, ScaleZ));

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
	const float HullHalfZ = ScaleZ * 50.f;
	Loc.Z = SmoothedWaterZ + HullHalfZ * FloatDraftFraction;
	SetActorLocation(Loc);
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

bool ASailBoatPawn::SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Query slightly above expected water so ocean bodies can project down
	const FVector Query(WorldXY.X, WorldXY.Y, WorldXY.Z + 50000.f);
	bool bAny = false;
	float BestAbsDZ = TNumericLimits<float>::Max();

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp)
		{
			continue;
		}
		FVector Surf, Norm, Vel;
		float Depth = 0.f;
		if (Comp->GetWaterSurfaceInfoAtLocation(Query, Surf, Norm, Vel, Depth, /*bIncludeDepth*/ false))
		{
			const float Dz = FMath::Abs(Surf.Z - WorldXY.Z);
			if (!bAny || Dz < BestAbsDZ)
			{
				BestAbsDZ = Dz;
				OutSurface = Surf;
				OutNormal = Norm;
				bAny = true;
			}
		}
	}
	return bAny;
}

void ASailBoatPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Dynamics.Update(DeltaSeconds);
	ApplyDynamicsToTransform(DeltaSeconds);
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

	// --- Water float (center) + optional bow/stern for pitch ---
	const float HullHalfZ = HullMesh->GetComponentScale().Z * 50.f;
	const float HalfLoa = HullLengthCm * 0.45f;

	FVector CenterSurf, CenterN;
	float TargetZ = WaterSurfaceZ;
	float WavePitch = 0.f;

	if (SampleWaterSurface(Loc, CenterSurf, CenterN))
	{
		TargetZ = CenterSurf.Z;
		if (bSampleWavePitch)
		{
			const FVector Fwd(CosH, SinH, 0.f);
			FVector BowS, BowN, SternS, SternN;
			const FVector BowXY = Loc + Fwd * HalfLoa;
			const FVector SternXY = Loc - Fwd * HalfLoa;
			const bool bBow = SampleWaterSurface(BowXY, BowS, BowN);
			const bool bStern = SampleWaterSurface(SternXY, SternS, SternN);
			if (bBow && bStern)
			{
				const float DZ = BowS.Z - SternS.Z;
				WavePitch = FMath::RadiansToDegrees(FMath::Atan2(DZ, HalfLoa * 2.f));
				WavePitch = FMath::Clamp(WavePitch, -12.f, 12.f);
			}
		}
	}

	if (!bFloatInit)
	{
		SmoothedWaterZ = TargetZ;
		SmoothedPitch = WavePitch;
		bFloatInit = true;
	}
	else
	{
		const float A = 1.f - FMath::Exp(-FloatSmoothRate * DeltaSeconds);
		SmoothedWaterZ = FMath::Lerp(SmoothedWaterZ, TargetZ, A);
		SmoothedPitch = FMath::Lerp(SmoothedPitch, WavePitch, A);
	}

	Loc.Z = SmoothedWaterZ + HullHalfZ * FloatDraftFraction;

	// Pitch = wave, Yaw = heading, Roll = heel (UE Rotator: Pitch, Yaw, Roll)
	const FRotator Rot(SmoothedPitch, Yaw, Heel);
	SetActorLocationAndRotation(Loc, Rot, false, nullptr, ETeleportType::None);
}

void ASailBoatPawn::DrawHud() const
{
	if (!GEngine) return;
	const FString Line = FString::Printf(
		TEXT("SPD %.1f kn   HEEL %.0f°   PITCH %.1f°   HDG %.0f°   RUD %.0f°   AWA %.0f°   AWS %.1f   TWS %.0f@%.0f   WZ %.0f   %s"),
		Dynamics.GetSpeedKnots(),
		Dynamics.Phi,
		SmoothedPitch,
		Dynamics.Heading,
		-Dynamics.Rudder,
		Dynamics.GetApparentWindAngleDeg(),
		Dynamics.GetApparentWindSpeedKn(),
		Dynamics.TrueWindSpeedKn,
		Dynamics.TrueWindDirDeg,
		SmoothedWaterZ,
		Dynamics.bAutoHeading ? TEXT("AUTO") : TEXT("HELM"));
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan, Line);
	GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White, TEXT("A/D helm  |  center re-engages AUTO  |  water Z from Water plugin when available"));
}
