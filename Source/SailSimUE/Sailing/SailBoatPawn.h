#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "SailBoatPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class UFloatingPawnMovement;

/**
 * Placeholder keelboat driven by FBoatDynamics (J/105 3-DOF VPP).
 * World mapping: +X = north/heading 0, +Y = east/heading 90 (UE left-handed: we use
 * X forward on boat, Y right — converted from dynamics heading each frame).
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

	/** Water surface height (cm) for float until real buoyancy. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterSurfaceZ = 0.f;

	/** Visual LOA scale (J/105 ~10.5 m → 1050 cm). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;

	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;

	void OnMoveRight(float Value);
};
