#include "Sailing/SailBoatPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/Engine.h"
#include "Components/InputComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	BoatRoot = CreateDefaultSubobject<USceneComponent>(TEXT("BoatRoot"));
	RootComponent = BoatRoot;

	// Meshes filled in constructor via BuildBoatMeshes-equivalent inline
	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Hull"));
	HullMesh->SetupAttachment(BoatRoot);
	CabinMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Cabin"));
	CabinMesh->SetupAttachment(BoatRoot);
	KeelMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Keel"));
	KeelMesh->SetupAttachment(BoatRoot);
	MastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mast"));
	MastMesh->SetupAttachment(BoatRoot);
	BoomMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Boom"));
	BoomMesh->SetupAttachment(BoatRoot);
	MainSailMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MainSail"));
	MainSailMesh->SetupAttachment(BoatRoot);
	JibSailMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("JibSail"));
	JibSailMesh->SetupAttachment(BoatRoot);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMat(TEXT("/Engine/BasicShapes/BasicShapeMaterial"));

	UStaticMesh* Cube = CubeMesh.Succeeded() ? CubeMesh.Object : nullptr;
	UStaticMesh* Cyl = CylMesh.Succeeded() ? CylMesh.Object : nullptr;
	UMaterialInterface* BaseMat = BasicMat.Succeeded() ? BasicMat.Object : nullptr;

	auto SetupMesh = [BaseMat](UStaticMeshComponent* Comp, UStaticMesh* Mesh, bool bCollision)
	{
		if (!Comp) return;
		if (Mesh) Comp->SetStaticMesh(Mesh);
		Comp->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		if (bCollision)
		{
			Comp->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		}
		Comp->SetCastShadow(true);
		if (BaseMat)
		{
			Comp->SetMaterial(0, BaseMat);
		}
	};

	SetupMesh(HullMesh, Cube, true);
	SetupMesh(CabinMesh, Cube, false);
	SetupMesh(KeelMesh, Cube, false);
	SetupMesh(MastMesh, Cyl, false);
	SetupMesh(BoomMesh, Cyl, false);
	SetupMesh(MainSailMesh, Cube, false);
	SetupMesh(JibSailMesh, Cube, false);

	// Relative layout: root = waterline / CG; +X bow
	// Engine cube = 100cm; cylinder = 100cm tall, 100cm diameter
	const float Loa = 1050.f;
	const float Beam = 340.f;
	const float HullDepth = 120.f; // freeboard+draft visual
	// Hull: center slightly below waterline so bottom is immersed
	HullMesh->SetRelativeLocation(FVector(0.f, 0.f, -HullDepth * 0.25f));
	HullMesh->SetRelativeScale3D(FVector(Loa / 100.f, Beam / 100.f, HullDepth / 100.f));

	// Cabin trunk (cockpit house)
	CabinMesh->SetRelativeLocation(FVector(-80.f, 0.f, 55.f));
	CabinMesh->SetRelativeScale3D(FVector(3.2f, 2.4f, 0.9f));

	// Fin keel
	KeelMesh->SetRelativeLocation(FVector(-40.f, 0.f, -HullDepth * 0.25f - 100.f));
	KeelMesh->SetRelativeScale3D(FVector(1.8f, 0.12f, 2.0f));

	// Mast ~14 m above deck
	const float MastH = 1400.f;
	MastMesh->SetRelativeLocation(FVector(-50.f, 0.f, MastH * 0.5f + 40.f));
	MastMesh->SetRelativeScale3D(FVector(0.18f, 0.18f, MastH / 100.f));

	// Boom
	BoomMesh->SetRelativeLocation(FVector(200.f, 0.f, 90.f));
	BoomMesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f)); // lay along X
	BoomMesh->SetRelativeScale3D(FVector(0.12f, 0.12f, 4.5f));

	// Mainsail: thin plane on starboard-ish of mast (visual only)
	MainSailMesh->SetRelativeLocation(FVector(180.f, 15.f, 700.f));
	MainSailMesh->SetRelativeScale3D(FVector(4.0f, 0.04f, 11.0f));

	// Jib forward of mast
	JibSailMesh->SetRelativeLocation(FVector(280.f, -12.f, 550.f));
	JibSailMesh->SetRelativeRotation(FRotator(0.f, 12.f, 0.f));
	JibSailMesh->SetRelativeScale3D(FVector(2.8f, 0.035f, 8.5f));

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(BoatRoot);
	SpringArm->TargetArmLength = 2800.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 3.f;
	SpringArm->bEnableCameraRotationLag = true;
	SpringArm->CameraRotationLagSpeed = 4.f;
	SpringArm->SetRelativeRotation(FRotator(-14.f, 0.f, 0.f));
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 220.f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);

	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void ASailBoatPawn::EnsureOpenWaterSpawn()
{
	FVector Loc = GetActorLocation();
	const float Dist2D = FVector2D(Loc.X, Loc.Y).Size();
	if (bForceOpenWaterSpawn || Dist2D < OriginIslandRadiusCm)
	{
		Loc.X = OpenWaterSpawnXY.X;
		Loc.Y = OpenWaterSpawnXY.Y;
		SetActorLocation(Loc);
	}
}

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();
	Dynamics.InitJ105();

	// Tint materials (hull white, sails off-white, spars dark)
	auto Tint = [](UStaticMeshComponent* Comp, FLinearColor Color, float Metallic = 0.f, float Rough = 0.7f)
	{
		if (!Comp) return;
		UMaterialInterface* Base = Comp->GetMaterial(0);
		if (!Base) return;
		UMaterialInstanceDynamic* Mid = Comp->CreateAndSetMaterialInstanceDynamic(0);
		if (!Mid) return;
		// BasicShapeMaterial uses "Color" parameter on many engine builds
		Mid->SetVectorParameterValue(TEXT("Color"), Color);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Color);
		Mid->SetScalarParameterValue(TEXT("Metallic"), Metallic);
		Mid->SetScalarParameterValue(TEXT("Roughness"), Rough);
	};
	Tint(HullMesh, FLinearColor(0.92f, 0.93f, 0.95f));
	Tint(CabinMesh, FLinearColor(0.85f, 0.88f, 0.92f));
	Tint(KeelMesh, FLinearColor(0.15f, 0.15f, 0.18f), 0.2f, 0.5f);
	Tint(MastMesh, FLinearColor(0.75f, 0.75f, 0.78f), 0.4f, 0.35f);
	Tint(BoomMesh, FLinearColor(0.75f, 0.75f, 0.78f), 0.4f, 0.35f);
	Tint(MainSailMesh, FLinearColor(0.96f, 0.96f, 0.94f), 0.f, 0.85f);
	Tint(JibSailMesh, FLinearColor(0.96f, 0.96f, 0.94f), 0.f, 0.85f);

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
	// Root sits at waterline (+ small freeboard offset)
	Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;
	SetActorLocation(Loc);

	Dynamics.Heading = 90.f;
	Dynamics.AutoTarget = Dynamics.Heading;
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
	if (!World)
	{
		return false;
	}

	const FVector Query(WorldXY.X, WorldXY.Y, FMath::Max(WorldXY.Z, 0.f) + 100000.f);
	bool bAny = false;
	float BestAbsDZ = TNumericLimits<float>::Max();
	float BestDepth = 0.f;

	for (TActorIterator<AWaterBody> It(World); It; ++It)
	{
		UWaterBodyComponent* Comp = It->GetWaterBodyComponent();
		if (!Comp)
		{
			continue;
		}
		FVector Surf, Norm, Vel;
		float Depth = 0.f;
		if (Comp->GetWaterSurfaceInfoAtLocation(Query, Surf, Norm, Vel, Depth, true))
		{
			const float Dz = FMath::Abs(Surf.Z - WorldXY.Z);
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
	if (bAny && OutDepth)
	{
		*OutDepth = BestDepth;
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
			if (SampleWaterSurface(Loc + Fwd * HalfLoa, BowS, BowN) &&
				SampleWaterSurface(Loc - Fwd * HalfLoa, SternS, SternN))
			{
				const float DZ = BowS.Z - SternS.Z;
				WavePitch = FMath::Clamp(
					FMath::RadiansToDegrees(FMath::Atan2(DZ, HalfLoa * 2.f)),
					-12.f, 12.f);
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

	// Root = waterline
	Loc.Z = SmoothedWaterZ + WaterlineOffsetCm;

	const FRotator Rot(SmoothedPitch, Yaw, Heel);
	SetActorLocationAndRotation(Loc, Rot, false, nullptr, ETeleportType::None);

	// Sheet main boom slightly with heel for a bit of life
	if (BoomMesh)
	{
		const float BoomYaw = FMath::Clamp(Dynamics.Phi * 0.35f, -25.f, 25.f);
		BoomMesh->SetRelativeRotation(FRotator(0.f, BoomYaw, 90.f));
		if (MainSailMesh)
		{
			MainSailMesh->SetRelativeRotation(FRotator(0.f, BoomYaw * 0.9f, 0.f));
		}
	}
}

void ASailBoatPawn::DrawHud() const
{
	if (!GEngine) return;
	const FString Line = FString::Printf(
		TEXT("SPD %.1f kn   HEEL %.0f°   PITCH %.1f°   HDG %.0f°   RUD %.0f°   AWA %.0f°   AWS %.1f   TWS %.0f@%.0f   WL %.0f   %s"),
		Dynamics.GetSpeedKnots(),
		Dynamics.Phi,
		SmoothedPitch,
		Dynamics.Heading,
		-Dynamics.Rudder,
		Dynamics.GetApparentWindAngleDeg(),
		Dynamics.GetApparentWindSpeedKn(),
		Dynamics.TrueWindSpeedKn,
		Dynamics.TrueWindDirDeg,
		SmoothedWaterZ + WaterlineOffsetCm,
		Dynamics.bAutoHeading ? TEXT("AUTO") : TEXT("HELM"));
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan, Line);
	GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White,
		TEXT("A/D helm | center=AUTO | primitive J/105 stand-in (hull+keel+mast+sails)"));
}
