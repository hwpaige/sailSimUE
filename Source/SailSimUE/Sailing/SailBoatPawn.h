#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "Sailing/BoatMeshFromJson.h"
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
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetHelmInput(float StarboardPositive);

	/** Sheet ease 0..1 (0 hard in, 1 eased). */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetSheetEase(float Ease01);

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetSheetEase() const { return Dynamics.SheetEase; }

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

	/** Apply JSON sailing/dims into FBoatDynamics (BeginPlay only). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Mesh")
	bool bApplyJsonSailingParams = true;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float OriginIslandRadiusCm = 80000.f;

	/** Far from water-brush island (~2 km NE of origin). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(200000.f, 150000.f);

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	/** Mouse X/Y orbit sensitivity (degrees per input unit). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float OrbitYawSpeed = 2.2f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float OrbitPitchSpeed = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MinOrbitPitchDeg = -85.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MaxOrbitPitchDeg = -2.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float ZoomSpeedCm = 120.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MinArmLengthCm = 350.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MaxArmLengthCm = 6000.f;

	/** If true, only orbit while holding RMB (recommended for PIE). If false, always-on mouse look. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	bool bRequireRMBToOrbit = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SheetAxis = 0.f;
	FVector BoomBaseLoc = FVector::ZeroVector;
	FVector BoomEndLoc = FVector::ZeroVector;
	bool bBoomEndpointsValid = false;
	float SmoothedWaterZ = 0.f;
	float SmoothedPitch = 0.f;
	float WavePitchSampleTimer = 0.f;
	float CachedWavePitch = 0.f;
	bool bFloatInit = false;
	bool bLoftMeshLoaded = false;
	FString LoadedLoftPath;
	FBoatJsonSailingParams CachedSailingParams;
	int32 StartupSkipFrames = 3;

	/** Spring-arm orbit (relative to boat). */
	float OrbitYawDeg = -25.f;
	float OrbitPitchDeg = -18.f;
	bool bOrbitRMBHeld = false;

	/** Load procedural loft (+ spars). bApplyDynamics: wire sailing params into VPP. */
	void LoadLoftMesh(bool bApplyDynamics = false);
	void PlaceSparFromEndpoints(UStaticMeshComponent* Comp, const FVector& A, const FVector& B);
	void ApplyCachedSailingToDynamics();
	void UpdateBoomFromSheet();
	void ApplyOrbitToSpringArm();
	void ApplyDynamicsToTransform(float DeltaSeconds);
	void DrawHud() const;
	void OnMoveRight(float Value);
	void OnSheetAxis(float Value);
	void OnLookYaw(float Value);
	void OnLookPitch(float Value);
	void OnCameraZoom(float Value);
	void OnOrbitPressed();
	void OnOrbitReleased();
	void EnsureOpenWaterSpawn();
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const;
};
