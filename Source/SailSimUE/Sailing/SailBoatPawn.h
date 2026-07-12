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
 * Root is at waterline; hull mesh hangs partly below. Heading 0° → +X, 90° → +Y.
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

	/** Fallback water height (cm) if Water plugin query fails. */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterSurfaceZ = 0.f;

	/**
	 * Height of actor root (waterline) above sampled water surface (cm).
	 * Keep small so the hull (which extends below root) sits in the water.
	 */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterlineOffsetCm = 8.f;

	/** Vertical blend toward water surface (1/s). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float FloatSmoothRate = 6.f;

	/** Visual LOA (J/105 ~10.5 m → 1050 cm). */
	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	bool bSampleWavePitch = true;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float OriginIslandRadiusCm = 25000.f;

	/** Open-water spawn XY (cm). ~800 m east of origin island. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(80000.f, 0.f);

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SmoothedWaterZ = 0.f;
	float SmoothedPitch = 0.f;
	bool bFloatInit = false;

	void BuildBoatMeshes();
	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;
	void OnMoveRight(float Value);
	void EnsureOpenWaterSpawn();
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const;
};
