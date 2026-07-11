#include "Sailing/SailBoatPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/SpringArmComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Components/InputComponent.h"

ASailBoatPawn::ASailBoatPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	HullMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HullMesh"));
	RootComponent = HullMesh;
	HullMesh->SetSimulatePhysics(false);
	HullMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HullMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);

	// Engine cube as LOA placeholder (scaled in BeginPlay)
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		HullMesh->SetStaticMesh(CubeMesh.Object);
	}

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 2800.f;
	SpringArm->bUsePawnControlRotation = false;
	SpringArm->bDoCollisionTest = false;
	SpringArm->SetRelativeRotation(FRotator(-18.f, 0.f, 0.f));
	SpringArm->SetRelativeLocation(FVector(0.f, 0.f, 200.f));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);

	// Possessable
	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void ASailBoatPawn::BeginPlay()
{
	Super::BeginPlay();
	Dynamics.InitJ105();
	// Scale cube: default cube is 100cm; LOA along X
	const float ScaleX = HullLengthCm / 100.f;
	const float ScaleY = (HullLengthCm * 0.32f) / 100.f; // ~beam 11 ft
	const float ScaleZ = (HullLengthCm * 0.18f) / 100.f;
	HullMesh->SetWorldScale3D(FVector(ScaleX, ScaleY, ScaleZ));
	// Float on water plane
	FVector Loc = GetActorLocation();
	Loc.Z = WaterSurfaceZ + ScaleZ * 50.f;
	SetActorLocation(Loc);
}

void ASailBoatPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	// A/D → Turn axis (configured in DefaultInput.ini)
	PlayerInputComponent->BindAxis(TEXT("Turn"), this, &ASailBoatPawn::OnMoveRight);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this, &ASailBoatPawn::OnMoveRight);
}

void ASailBoatPawn::OnMoveRight(float Value)
{
	HelmAxis = FMath::Clamp(Value, -1.f, 1.f);
	// Full lock at ±1 → ±35°
	SetHelmInput(HelmAxis * 35.f);
}

void ASailBoatPawn::SetHelmInput(float StarboardPositive)
{
	Dynamics.SetRudderStarboardPositive(StarboardPositive);
	if (FMath::Abs(StarboardPositive) < 0.5f && Dynamics.bManualHelm)
	{
		// Re-engage auto when helm centered
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

void ASailBoatPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Dynamics.Update(DeltaSeconds);
	ApplyDynamicsToTransform(DeltaSeconds);
	DrawHud();
}

void ASailBoatPawn::ApplyDynamicsToTransform(float DeltaSeconds)
{
	// Dynamics: heading 0° = north. UE world: +X forward often east in OpenWorld;
	// We define: Heading 0 → +X, Heading 90 → +Y (standard math yaw).
	const float Yaw = Dynamics.Heading;
	const float Heel = Dynamics.Phi; // +stbd → roll
	// Advance position from body velocities (ft/s → cm/s: 1 ft = 30.48 cm)
	constexpr float FtToCm = 30.48f;
	const float CosH = FMath::Cos(FMath::DegreesToRadians(Dynamics.Heading));
	const float SinH = FMath::Sin(FMath::DegreesToRadians(Dynamics.Heading));
	// Body +X forward, +Y stbd in UE mesh: world delta
	const float Vx = (Dynamics.U * CosH - Dynamics.Vsway * SinH) * FtToCm;
	const float Vy = (Dynamics.U * SinH + Dynamics.Vsway * CosH) * FtToCm;

	FVector Loc = GetActorLocation();
	Loc.X += Vx * DeltaSeconds;
	Loc.Y += Vy * DeltaSeconds;
	// Keep on water plane (placeholder buoyancy)
	const float HullHalfZ = HullMesh->GetComponentScale().Z * 50.f;
	Loc.Z = WaterSurfaceZ + HullHalfZ * 0.35f;

	const FRotator Rot(0.f, Yaw, Heel); // Pitch, Yaw, Roll
	SetActorLocationAndRotation(Loc, Rot, false, nullptr, ETeleportType::None);
}

void ASailBoatPawn::DrawHud() const
{
	if (!GEngine) return;
	const FString Line = FString::Printf(
		TEXT("SPD %.1f kn   HEEL %.0f°   HDG %.0f°   RUD %.0f°   AWA %.0f°   AWS %.1f kn   TWS %.0f @ %.0f°   %s"),
		Dynamics.GetSpeedKnots(),
		Dynamics.Phi,
		Dynamics.Heading,
		-Dynamics.Rudder, // show starboard-positive
		Dynamics.GetApparentWindAngleDeg(),
		Dynamics.GetApparentWindSpeedKn(),
		Dynamics.TrueWindSpeedKn,
		Dynamics.TrueWindDirDeg,
		Dynamics.bAutoHeading ? TEXT("AUTO") : TEXT("HELM"));
	GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::Cyan, Line);
	GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White, TEXT("A/D helm  |  center stick re-engages AUTO heading"));
}
