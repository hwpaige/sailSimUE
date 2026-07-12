#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "SailBoatPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class USceneComponent;

/**
 * Placeholder keelboat (primitive rig) driven by FBoatDynamics (J/105 3-DOF VPP).
 * Root is at waterline; hull hangs partly below. Spawned by GameMode only (no AutoPossess)
 * to avoid double-boat glitches when a boat is also placed in the level.
 */
UCLASS()
class SAILSIMUE_API ASailBoatPawn : public APawn
{
	GENERATED_BODY()

public:
	ASailBoatPawn();

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetHelmInput(float StarboardPositive);

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetSpeedKnots() const { return Dynamics.GetSpeedKnots(); }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetHeelDeg() const { return Dynamics.Phi; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetHeadingDeg() const { return Dynamics.Heading; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetTrueWind(float SpeedKn, float DirDeg);

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BoatRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> HullMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> CabinMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> KeelMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MastMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> BoomMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MainSailMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> JibSailMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterSurfaceZ = 0.f;

	/** Root (waterline) above sampled water (cm). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterlineOffsetCm = 5.f;

	/** Max water-height change rate (cm/s) — kills float jitter. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float MaxWaterZSpeedCm = 120.f;

	/** Pitch blend rate (1/s). Lower = calmer. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float PitchSmoothRate = 2.5f;

	/** How often to re-sample bow/stern for pitch (seconds). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WavePitchSampleInterval = 0.1f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	bool bSampleWavePitch = true;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float OriginIslandRadiusCm = 25000.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(80000.f, 0.f);

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SmoothedWaterZ = 0.f;
	float SmoothedPitch = 0.f;
	float WavePitchSampleTimer = 0.f;
	float CachedWavePitch = 0.f;
	bool bFloatInit = false;
	bool bDynamicsReady = false;
	int32 StartupSkipFrames = 2;

	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;
	void OnMoveRight(float Value);
	void EnsureOpenWaterSpawn();
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const;
};
