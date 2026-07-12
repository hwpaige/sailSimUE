#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "SailBoatPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;

/**
 * Placeholder keelboat driven by FBoatDynamics (J/105 3-DOF VPP).
 * Heading 0° → world +X, 90° → +Y. Float Z from Water plugin when available.
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
	TObjectPtr<UStaticMeshComponent> HullMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	/** Fallback water height (cm) if Water plugin query fails. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterSurfaceZ = 0.f;

	/** How far the mesh sits relative to sampled water (fraction of half-height). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float FloatDraftFraction = 0.35f;

	/** Vertical blend toward water surface (1/s). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float FloatSmoothRate = 4.f;

	/** Visual LOA (J/105 ~10.5 m → 1050 cm). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	/** Sample wave pitch from bow/stern water heights. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	bool bSampleWavePitch = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SmoothedWaterZ = 0.f;
	float SmoothedPitch = 0.f;
	bool bFloatInit = false;

	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;
	void OnMoveRight(float Value);

	/** Returns true if Water plugin gave a surface sample. */
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal) const;
};
