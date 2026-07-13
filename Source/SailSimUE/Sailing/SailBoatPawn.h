#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "Sailing/BoatMeshFromJson.h"
#include "Sailing/BoatSpec.h"
#include "Sailing/SailClothSim.h"
#include "SailBoatPawn.generated.h"

class UStaticMeshComponent;
class USpringArmComponent;
class UCameraComponent;
class USceneComponent;
class UProceduralMeshComponent;
class UBoxComponent;

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
	virtual void UnPossessed() override;

	/** True once GameMode has spawned/possessed this as the player boat. */
	UPROPERTY(BlueprintReadOnly, Category = "Sailing|Spawn")
	bool bPlayerSessionBoat = false;

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
	float GetRudderStarboardDeg() const { return -Dynamics.Rudder; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetApparentWindAngleDeg() const { return Dynamics.GetApparentWindAngleDeg(); }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetApparentWindSpeedKn() const { return Dynamics.GetApparentWindSpeedKn(); }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetTrueWindSpeedKn() const { return Dynamics.TrueWindSpeedKn; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetTrueWindDirDeg() const { return Dynamics.TrueWindDirDeg; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	bool IsAutoHeading() const { return Dynamics.bAutoHeading; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	bool IsSailing() const { return Dynamics.bSailing; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetSailing(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void SetTrueWind(float SpeedKn, float DirDeg);

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void AdjustTrueWindSpeed(float DeltaKn);

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	void AdjustTrueWindDir(float DeltaDeg);

	/** Load catalog preset by id (j105, endeavour, melges24, cruiser36). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Mesh")
	bool SetBoatPreset(const FString& PresetId);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Mesh")
	FString GetBoatPresetId() const { return ActivePresetId; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Mesh")
	FString GetBoatDisplayName() const;

	/** Phase 3.2: live LOA scale (1 = catalog). Rebuilds mesh scale + dynamics. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spec")
	void SetLiveLoaScale(float Scale);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spec")
	void AdjustLiveLoaScale(float Delta);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spec")
	float GetLiveLoaScale() const { return ActiveSpec.ScaleLoa; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spec")
	FString GetSpecSummary() const;

	/**
	 * Phase 3.3: re-loft via Python sail_geom oracle → Content/Data/live_boat3d.json,
	 * then reload mesh (true geometry change, not just scale).
	 */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spec")
	bool RebuildLoftFromOracle();

	/** If true, -/= LOA scale also triggers oracle re-loft (slower; needs python3). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spec")
	bool bAutoOracleOnScale = false;

	/**
	 * Open-water spawn (XY cm). Must be outside the water-brush island
	 * (hundreds of meters) but inside WaterZone half-extent (~1 km with default ensure).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(70000.f, 55000.f);

	/** Snap hull waterline to ocean surface; optionally force open-water XY. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spawn")
	void SnapToWaterSurface(bool bForceXY);

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BoatRoot;

	/** Lofted hull/deck/cabin (sails are separate so they can sheet). Visual only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> LoftMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> MainSailMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> JibSailMesh;

	/** Simple hull collider (avoids Chaos bad-tri complex mesh from loft). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> HullCollision;

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
	float MaxWaterZSpeedCm = 140.f;

	/** Soft buoyancy vertical spring (higher = snappier ride on waves). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float BuoyancyStiffness = 12.0f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float BuoyancyDamping = 5.5f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float WavePitchSmoothRate = 4.0f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float WaveRollSmoothRate = 3.5f;

	/** How much wave slope adds to VPP heel (0 = pure VPP, 1 = full wave roll). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float WaveRollGain = 0.55f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float MaxWavePitchDeg = 12.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float MaxWaveRollDeg = 10.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	float WaveSampleInterval = 0.05f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullLengthCm = 1050.f;

	UPROPERTY(EditAnywhere, Category = "Sailing")
	float HullBeamCm = 335.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Buoyancy")
	bool bMultiPointBuoyancy = true;

	UPROPERTY(EditAnywhere, Category = "Sailing|Cloth")
	bool bEnableSailCloth = true;

	/** Content-relative path to boat3d export. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Mesh")
	FString BoatJsonRelativePath = TEXT("Data/j105_boat3d.json");

	UPROPERTY(EditAnywhere, Category = "Sailing|Mesh")
	FString ActivePresetId = TEXT("j105");

	/** Apply JSON sailing/dims into FBoatDynamics (BeginPlay only). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Mesh")
	bool bApplyJsonSailingParams = true;

	/** Soft radius around origin for the water-brush island (force offshore if closer). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float OriginIslandRadiusCm = 40000.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	/** Resize WaterZone extent (not location) so the water mesh covers the boat. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bEnsureOceanCoverage = true;

	/** Full ZoneExtent (cm) after ensure — must cover OpenWaterSpawnXY; Mac-safe if ≤~400k. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float WaterZoneExtentCm = 250000.f;

	/** Mouse X/Y orbit sensitivity (degrees per input unit). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float OrbitYawSpeed = 2.2f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float OrbitPitchSpeed = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MinOrbitPitchDeg = -70.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MaxOrbitPitchDeg = -8.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float ZoomSpeedCm = 120.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MinArmLengthCm = 1200.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float MaxArmLengthCm = 8000.f;

	/** Chase arm length as multiple of LOA (cm). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	float CameraArmLengthLoaScale = 2.4f;

	/** If true, only orbit while holding RMB (recommended for PIE). If false, always-on mouse look. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Camera")
	bool bRequireRMBToOrbit = true;

	FBoatDynamics Dynamics;
	float HelmAxis = 0.f;
	float SheetAxis = 0.f;
	FVector BoomBaseLoc = FVector::ZeroVector;
	FVector BoomEndLoc = FVector::ZeroVector;
	FVector MastBaseLoc = FVector::ZeroVector;
	bool bBoomEndpointsValid = false;
	bool bMastPivotValid = false;
	float SmoothedWaterZ = 0.f;
	float VerticalVelZ = 0.f;
	float SmoothedPitch = 0.f;
	float SmoothedWaveRoll = 0.f;
	float WaveSampleTimer = 0.f;
	float CachedWavePitch = 0.f;
	float CachedWaveRoll = 0.f;
	bool bFloatInit = false;
	bool bLoftMeshLoaded = false;
	FString LoadedLoftPath;
	FBoatJsonSailingParams CachedSailingParams;
	FBoatSpec ActiveSpec;
	int32 StartupSkipFrames = 3;
	/** Frames to wait before destroying unpossessed level/WP boats. */
	int32 OrphanGraceFrames = 8;
	int32 WaterSnapFrames = 0;
	FSailClothSim MainCloth;
	FSailClothSim JibCloth;

	/** Spring-arm orbit (relative to boat). Yaw≈0 = from astern; pitch negative = elevated. */
	float OrbitYawDeg = 25.f;
	float OrbitPitchDeg = -20.f;
	bool bOrbitRMBHeld = false;
	int32 CameraLagEnableFrames = 0;

	/** Load procedural loft (+ spars). bApplyDynamics: wire sailing params into VPP. */
	void LoadLoftMesh(bool bApplyDynamics = false);
	void UpdateHullCollisionFromMesh();
	void PlaceSparFromEndpoints(UStaticMeshComponent* Comp, const FVector& A, const FVector& B);
	void ApplyCachedSailingToDynamics();
	/** Apply ActiveSpec scales to mesh + FBoatDynamics (Phase 3.2). */
	void ApplyActiveSpecToBoat();
	void UpdateBoomFromSheet();
	void UpdateSailCloth(float DeltaSeconds);
	void ApplyClothForceToDynamics();
	void EnsureOceanCoverage();
	void ApplyOrbitToSpringArm();
	/** Reset arm length / orbit / lag so PIE never starts inside the hull. */
	void RefreshChaseCamera();
	void ApplyDynamicsToTransform(float DeltaSeconds);
	void SampleMultiPointBuoyancy(const FVector& Loc, float CosH, float SinH,
		float& OutTargetZ, float& OutWavePitchDeg, float& OutWaveRollDeg) const;
	void OnMoveRight(float Value);
	void OnSheetAxis(float Value);
	void OnLookYaw(float Value);
	void OnLookPitch(float Value);
	void OnCameraZoom(float Value);
	void OnOrbitPressed();
	void OnOrbitReleased();
	void OnToggleSailing();
	void OnWindSpeedUp();
	void OnWindSpeedDown();
	void OnWindDirLeft();
	void OnWindDirRight();
	void OnPreset1();
	void OnPreset2();
	void OnPreset3();
	void OnPreset4();
	void OnLoaScaleUp();
	void OnLoaScaleDown();
	void OnRebuildLoft();
	void EnsureOpenWaterSpawn();
	bool SampleWaterSurface(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const;
};
