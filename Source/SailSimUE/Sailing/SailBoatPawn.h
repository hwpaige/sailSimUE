#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "SailBoatPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class USceneComponent;
class UProceduralMeshComponent;

/**
 * J/105 from sail_geom.boat3d (procedural mesh) + FBoatDynamics 3-DOF VPP.
 * Root at waterline; open-water spawn away from default landscape island.
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

	/** Lofted hull/deck/cabin/sails from Content/Data/j105_boat3d.json */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> LoftMesh;

	/** Mast cylinder (JSON only has endpoints). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MastMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> BoomMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterSurfaceZ = 0.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WaterlineOffsetCm = 4.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float MaxWaterZSpeedCm = 100.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float PitchSmoothRate = 2.0f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float WavePitchSampleInterval = 0.12f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	bool bSampleWavePitch = true;

	/** Content-relative path to boat3d export. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Mesh")
	FString BoatJsonRelativePath = TEXT("Data/j105_boat3d.json");

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float OriginIslandRadiusCm = 80000.f;

	/** Far from water-brush island (~2 km NE of origin). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(200000.f, 150000.f);

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SmoothedWaterZ = 0.f;
	float SmoothedPitch = 0.f;
	float WavePitchSampleTimer = 0.f;
	float CachedWavePitch = 0.f;
	bool bFloatInit = false;
	int32 StartupSkipFrames = 3;

	void LoadLoftMesh();
	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;
	void OnMoveRight(float Value);
	void EnsureOpenWaterSpawn();
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const;
};
