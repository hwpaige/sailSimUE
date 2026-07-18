#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sailing/BoatDynamics.h"
#include "Sailing/BoatMeshFromJson.h"
#include "Sailing/BoatSpec.h"
#include "Sailing/SailClothSim.h"
#include "Sailing/SailVisualExtras.h"
#include "Sailing/Ocean/BoatWakeSim.h"
#include "Sailing/Wind/WindCompassRing.h"
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

	/** Sticky tiller position in degrees (+ = starboard), −35..+35. */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetHeldHelmStarboardDeg() const { return HeldHelmStarboardDeg; }

	/** Main outhaul 0..1 (full = foot taut on boom at class E). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	void SetOuthaul(float V01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	float GetOuthaul() const { return Dynamics.Outhaul01; }

	/** Boom vang 0 = hard on, 1 = off. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	void SetVang(float V01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	float GetVang() const { return Dynamics.Vang01; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	void SetJibCar(float V01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	float GetJibCar() const { return Dynamics.JibCar01; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	void SetJibLuffTension(float V01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	float GetJibLuffTension() const { return Dynamics.JibLuffTension01; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	void SetJibLeechTension(float V01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Trim")
	float GetJibLeechTension() const { return Dynamics.JibLeechTension01; }

	/** Asym spin deploy 0..1 (manual KITE set; smooth hoist/douse for HUD). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	float GetSpinDeploy01() const { return SpinDeploy01; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	bool IsSpinFlying() const { return SpinDeploy01 > 0.15f; }

	/** Spin sheet 0 hard in … 1 fully eased. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	void SetSpinSheetEase(float Ease01);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	float GetSpinSheetEase() const { return SpinSheetEase01; }

	/**
	 * Manual jib set / douse. Never auto-opens on TWA / kite set — button (or prefs) only.
	 * 1 = fully set, 0 = doused (mesh + sheets hidden).
	 */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Jib")
	void SetJibSet(bool bSet);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Jib")
	void ToggleJibSet();

	UFUNCTION(BlueprintCallable, Category = "Sailing|Jib")
	bool IsJibSet() const { return JibSetTarget01 > 0.5f; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Jib")
	float GetJibSet01() const { return JibSet01; }

	/**
	 * Manual kite set / douse. SET → hoist and stay flying at any TWA;
	 * DOUSED → drop. No auto deploy/douse by wind angle. Independent of the jib.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	void SetKiteSet(bool bSet);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	void ToggleKiteSet();

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	bool IsKiteSet() const { return KiteSetTarget01 > 0.5f; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Spin")
	float GetKiteSetTarget01() const { return KiteSetTarget01; }

	/** Live cloth edge lengths (cm) for debug HUD. */
	float GetMainLiveFootCm() const { return MainCloth.LiveFootChordCm; }
	float GetMainLiveLeechCm() const { return MainCloth.LiveLeechPathCm; }
	float GetJibLiveFootCm() const { return JibCloth.LiveFootChordCm; }
	float GetJibLiveLeechCm() const { return JibCloth.LiveLeechPathCm; }
	float GetMainLoftFootCm() const { return MainCloth.LoftFootLen; }
	float GetMainLoftLeechCm() const { return MainCloth.LoftLeechPath; }
	float GetJibLoftFootCm() const { return JibCloth.LoftFootLen; }
	float GetJibLoftLeechCm() const { return JibCloth.LoftLeechPath; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetSpeedKnots() const { return Dynamics.GetSpeedKnots(); }

	/** World-space water surface sample (for wake / FX). */
	bool SampleWaterSurfacePublic(const FVector& WorldXY, FVector& OutSurface, FVector& OutNormal, float* OutDepth = nullptr) const
	{
		return SampleWaterSurface(WorldXY, OutSurface, OutNormal, OutDepth);
	}

	float GetHullLengthCm() const { return HullLengthCm; }
	float GetHullBeamCm() const { return HullBeamCm; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetHeelDeg() const { return Dynamics.Phi; }

	/** Wave / hull pitch (deg, + bow up). */
	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetPitchDeg() const { return SmoothedPitch; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetHeadingDeg() const { return Dynamics.Heading; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetRudderStarboardDeg() const { return -Dynamics.Rudder; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetApparentWindAngleDeg() const { return Dynamics.GetApparentWindAngleDeg(); }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	/** Apparent wind speed in DISPLAY knots (HUD). */
	float GetApparentWindSpeedKn() const
	{
		return FBoatDynamics::WindDisplayFromPhysics(Dynamics.GetApparentWindSpeedKn());
	}
	/** Raw physics AWS for cloth / aero (not for HUD labels). */
	float GetApparentWindSpeedPhysicsKn() const { return Dynamics.GetApparentWindSpeedKn(); }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	/**
	 * True wind speed in DISPLAY knots (HUD / sliders / prefs).
	 * Calibrated so the number matches sail fill (phys 25 ≈ feel 8).
	 */
	float GetTrueWindSpeedKn() const
	{
		return FBoatDynamics::WindDisplayFromPhysics(Dynamics.TrueWindSpeedKn);
	}
	/** Raw physics knots used by the VPP / wind field (not for HUD labels). */
	float GetTrueWindSpeedPhysicsKn() const { return Dynamics.TrueWindSpeedKn; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetTrueWindDirDeg() const { return Dynamics.TrueWindDirDeg; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	bool IsAutoHeading() const { return Dynamics.bAutoHeading; }

	UFUNCTION(BlueprintCallable, Category = "Sailing")
	float GetAutoTargetHeading() const { return Dynamics.AutoTarget; }

	float GetAutoAwaTarget() const { return Dynamics.AutoAwaTarget; }
	FBoatDynamics::EAutoMode GetAutoMode() const { return Dynamics.AutoMode; }
	bool IsAutoTrim() const { return Dynamics.bAutoTrim; }
	bool IsTacking() const { return Dynamics.bTacking; }

	// --- DC panel / COLREGS lights (breaker states) ---
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetBreakerNav(bool bOn);
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetBreakerSteaming(bool bOn);
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetBreakerAnchor(bool bOn);
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetBreakerDeck(bool bOn);
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetBreakerCabin(bool bOn);
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	void SetAllBreakersOff();

	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	bool IsBreakerNav() const { return bBreakerNav; }
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	bool IsBreakerSteaming() const { return bBreakerSteaming; }
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	bool IsBreakerAnchor() const { return bBreakerAnchor; }
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	bool IsBreakerDeck() const { return bBreakerDeck; }
	UFUNCTION(BlueprintCallable, Category = "Sailing|Lights")
	bool IsBreakerCabin() const { return bBreakerCabin; }

	/** Enable/disable autopilot (web setAutoHeading). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void SetAutoHeading(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void ToggleAutoHeading();

	/** Set or nudge autopilot target heading (deg, 0–360) — switches to HDG mode. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void SetAutoTargetHeading(float Deg);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void NudgeAutoTarget(float DeltaDeg);

	/** Select mode; clicking the active engaged mode stands by (web hSelectAutoMode). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void SelectAutoMode(uint8 ModeIndex);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void SetAutoAwaTarget(float SignedDeg);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void StartTack();

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void SetAutoTrim(bool bOn);

	UFUNCTION(BlueprintCallable, Category = "Sailing|Autopilot")
	void ToggleAutoTrim();

	/** Display string for TGT readout (web hAutopilotTargetText). */
	FString GetAutoTargetDisplayText() const;

	/** Acquire chart route into NAV mode (web hNavAcquireRoute). */
	bool AcquireNavRoute();
	/** Refresh bearing to active WP / advance on arrival (web hNavUpdateTarget). */
	void UpdateNavTarget();

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

	/** Phase 4: cloth fill quality 0..~1.2 (main+jib weighted). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Cloth")
	float GetClothForceScale() const { return Dynamics.ClothForceScale; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Cloth")
	float GetMainCamberCm() const { return MainCloth.LastCamberCm; }

	/** 0 = fully powered / filled, 1 = fully luffing (web-style threshold). */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Cloth")
	float GetMainLuffAmount() const { return MainCloth.GetLuffAmount(); }

	/** Peak wind displacement applied to cloth this frame (cm) — should be >0 with wind. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Cloth")
	float GetMainWindPushCm() const { return MainCloth.LastWindPushCm; }

	UFUNCTION(BlueprintCallable, Category = "Sailing|Cloth")
	float GetClothStretchRatio() const
	{
		if (MainCloth.bInitialized) return MainCloth.LastStretchRatio;
		if (JibCloth.bInitialized) return JibCloth.LastStretchRatio;
		return 1.f;
	}

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
	 * Spawn XY in world cm (FNavGeo: +X north, +Y east from NAV_ORIGIN).
	 * Default = Nantucket Harbor inner basin (41.2850°N, 70.0900°W) — web NAV_BOAT_START.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sailing|Spawn")
	FVector2D OpenWaterSpawnXY = FVector2D(-2163470.4f, 1080562.6f);

	/** Snap hull waterline to ocean surface; optionally force open-water XY. */
	UFUNCTION(BlueprintCallable, Category = "Sailing|Spawn")
	void SnapToWaterSurface(bool bForceXY);

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> BoatRoot;

	/** Lofted hull/deck/cabin (sails are separate so they can sheet). Visual only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> LoftMesh;

	/** Keel + rudder — separate so they can disable shadow cast (underwater). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> AppendagesMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> MainSailMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> JibSailMesh;

	/** Class asymmetric spinnaker (sprit-launched). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProceduralMeshComponent> SpinSailMesh;

	/** Retractable J/Sprit (extends with kite). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> BowspritMesh;

	/** Forestay extrusion (foil) + furler drum at the tack (roller-furling jib). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> ForestayFoilMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> FurlerDrumMesh;
	/** Spin sheet rope: clew → stern quarter block. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> SpinSheetMesh;
	/** Spin sheet: quarter block → cabin-top winch. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> SpinSheetToWinchMesh;
	/** Windward (lazy) spin sheet segments. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> SpinLazySheetSegA;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> SpinLazySheetSegB;

	/** Simple hull collider (avoids Chaos bad-tri complex mesh from loft). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UBoxComponent> HullCollision;

	/** Mast cylinder (JSON only has endpoints). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MastMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> BoomMesh;

	// --- Running rigging (jib tracks, traveler, mainsheet) ---
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibTrackPortMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibTrackStbdMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibCarPortMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibCarStbdMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> TravelerTrackMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> TravelerCarMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> MidboomBlockMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> MainsheetRopeMesh;
	/** Boom vang: mast base block → boom block + tackle line (J/105 mid-boom vang). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> VangMastBlockMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> VangBoomBlockMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> VangRopeMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibSheetPortMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibSheetStbdMesh;
	/** Jib sheet: track car → primary winch (both sides). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibSheetPortToWinchMesh;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibSheetStbdToWinchMesh;
	/** Extra lazy-sheet polyline segments (clew→wrap→car is multi-part). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibLazySheetSegA;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TObjectPtr<UStaticMeshComponent> JibLazySheetSegB;

	/**
	 * Standing rigging (web hBuildSpreadersAndStanding):
	 * 0-1 spreaders, 2 collar, 3-4 lowers, 5-6 uppers, 7-8 caps, 9 forestay, 10 backstay.
	 */
	static constexpr int32 StandingRigCount = 11;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Rigging")
	TArray<TObjectPtr<UStaticMeshComponent>> StandingRigMeshes;

	/**
	 * COLREGS / yacht lighting:
	 * 0 Port, 1 Stbd, 2 Stern, 3 Steaming, 4 Anchor, 5 DeckP, 6 DeckS, 7 Cabin.
	 * Sector lights use spots; anchor + cabin use omni points.
	 */
	enum class EBoatLightId : uint8
	{
		Port = 0, Stbd, Stern, Steaming, Anchor, DeckP, DeckS, Cabin, Count
	};
	static constexpr int32 BoatLightCount = static_cast<int32>(EBoatLightId::Count);

	/** Dark fixture housing (always visible). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TArray<TObjectPtr<UStaticMeshComponent>> LightHousingMeshes;
	/** Colored lens cap (emissive when on). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TArray<TObjectPtr<UStaticMeshComponent>> LightLensMeshes;
	/** Spot lights for sector / deck (indices match lights that use spots). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TArray<TObjectPtr<class USpotLightComponent>> LightSpots;
	/**
	 * Small colored omni at each nav fixture so red/green/white read from any
	 * camera angle (sector spots only light water in a cone).
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TObjectPtr<class UPointLightComponent> LightPortGlow = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TObjectPtr<class UPointLightComponent> LightStbdGlow = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TObjectPtr<class UPointLightComponent> LightSternGlow = nullptr;
	/** Omni for all-round anchor + cabin. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TObjectPtr<class UPointLightComponent> LightAnchorPoint = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components|Lights")
	TObjectPtr<class UPointLightComponent> LightCabinPoint = nullptr;

	/** DC breaker states (manual panel). */
	bool bBreakerNav = false;
	bool bBreakerSteaming = false;
	bool bBreakerAnchor = false;
	bool bBreakerDeck = false;
	bool bBreakerCabin = false;
	bool bBoatLightsBuilt = false;

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

	/** Leech + jib-body telltales and main-sail race numbers/logo (web sail visuals). */
	UPROPERTY(EditAnywhere, Category = "Sailing|Cloth")
	bool bEnableSailVisualExtras = true;

	/** Wake surface patch — off until a proper water-coupled solution. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Wake")
	bool bEnableWake = false;

	/** RRS G1 sail identity (e.g. "USA 52735"). Baked into markings atlas at load. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Cloth")
	FString SailNumber = TEXT("USA 52735");

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
	float OriginIslandRadiusCm = 15000.f;

	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bForceOpenWaterSpawn = true;

	/** Resize WaterZone + enable local tessellation so ocean draws under the boat. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	bool bEnsureOceanCoverage = true;

	/** Full ZoneExtent (cm). With local tessellation this can stay moderate. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float WaterZoneExtentCm = 240000.f;

	/** Sliding-window water mesh diameter (cm). ~1.2 km — local tess rebuilds every frame. */
	UPROPERTY(EditAnywhere, Category = "Sailing|Spawn")
	float LocalWaterTessellationDiameterCm = 120000.f;

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
	/** Closest approach (nm) to the active NAV mark — used for pass-abeam advance. */
	float NavTrackMinDistNm = 1.0e9f;
	/** Distance (nm) when this mark first became active — approach arming. */
	float NavTrackInitialDistNm = 1.0e9f;
	int32 NavTrackIndex = INDEX_NONE;
	/**
	 * True once we have closed range toward the active mark.
	 * Abeam / closest-point advance only after this, so engaging NAV never
	 * skips WP1 just because the bow is not yet pointed at it.
	 */
	bool bNavApproachedActive = false;
	/**
	 * Sticky tiller angle in degrees (+ starboard). A/D rates this while held;
	 * release keeps the last value (no spring-to-zero, no snap-to-rail).
	 */
	float HeldHelmStarboardDeg = 0.f;
	float HelmAxis = 0.f;
	float SheetAxis = 0.f;
	float OuthaulAxis = 0.f;
	float VangAxis = 0.f;
	float JibCarAxis = 0.f;
	float JibLeechAxis = 0.f;
	FVector BoomBaseLoc = FVector::ZeroVector;
	/** Rest boom tip from loft (may be off-center). */
	FVector BoomEndLoc = FVector::ZeroVector;
	/** Hard-in boom tip on centerline (Y = base Y) — sheet ease 0. */
	FVector BoomEndCenterlineLoc = FVector::ZeroVector;
	FVector MastBaseLoc = FVector::ZeroVector;
	bool bBoomEndpointsValid = false;
	bool bMastPivotValid = false;

	FBoatJsonRunningRigging CachedRigging;
	/** Traveler half-width (cm) — J/105 cockpit traveler ~1.2–1.4 m span. */
	float TravelerHalfWidthCm = 70.f;
	/** Midboom mainsheet attach fraction along boom (web MIDBOOM_FRAC = 0.5). */
	float MidboomSheetFrac = 0.50f;
	/** Boom vang attach fraction along boom (web MAINVANG_FRAC = 0.25). */
	float VangBoomFrac = 0.25f;
	/** Mast vang attach drop as fraction of boom length below gooseneck. */
	float VangMastDropFrac = 0.06f;
	/**
	 * Free boom angle from centerline (deg). + = starboard (UE +Y).
	 * Wind blows boom to leeward; sheet sets max |angle|. Not kinematic 1:1 with slider.
	 */
	float BoomYawDeg = 0.f;
	/**
	 * Boom tip rise above gooseneck height (cm, unscaled boat space).
	 * Driven by vang: 0 = hard on (boom flat/low), MaxBoomRise = fully eased.
	 */
	float BoomRiseCm = 0.f;
	/** Max tip rise when vang is fully eased (fraction of boom length). */
	float MaxBoomRiseFrac = 0.14f;
	/** How fast boom swings out under wind (deg/s). */
	float BoomSwingOutRateDeg = 55.f;
	/** How fast sheet hauls boom in (deg/s) — faster than free swing. */
	float BoomSheetInRateDeg = 120.f;
	float MaxBoomSwingDeg = 72.f;
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
	FSailClothSim SpinCloth;
	FSailVisualExtras SailVisuals;

	/** 0..1 kite hoist progress (follows KiteSetTarget01; not gated by TWA). */
	float SpinDeploy01 = 0.f;
	float SpritExtend01 = 0.f;
	bool bSpinMeshBuilt = false;
	/**
	 * Spin sheet ease 0 = hard on (clew close, flat kite), 1 = eased way out.
	 * Default tight — previous auto ease left the sheet too deep.
	 */
	float SpinSheetEase01 = 0.18f;
	/**
	 * Manual jib set 0..1 (smoothed). Target is 0 or 1 via SetJibSet / ToggleJibSet.
	 * Default set (up) — user hits JIB to douse if needed.
	 */
	float JibSet01 = 1.f;
	float JibSetTarget01 = 1.f;
	/** Visual furler drum spin (radians), driven by roller-furl amount. */
	float FurlerSpinRad = 0.f;
	/**
	 * True while geometric roller-furl owns the jib mesh (Set01 below full-open).
	 * Used to seed loft verts once when handing control back to cloth Step.
	 */
	bool bJibFurlDriveActive = false;
	/**
	 * Manual kite set 0 or 1. When 1, hoist and stay up; when 0, douse.
	 * Default doused — user hits KITE · SET when they want it.
	 */
	float KiteSetTarget01 = 0.f;
	/**
	 * Legacy TWA band (sheet deep-bias only; no longer gates deploy/douse).
	 */
	float SpinDeployTwaStartDeg = 72.f;
	float SpinDeployTwaFullDeg = 100.f;
	/**
	 * Extended sprit tip past stem (cm).
	 * J/105 retractable carbon sprit ≈ 6.5 ft (2.0 m) past the stem when fully out
	 * (class hardware; Code Zero often flies on the same sprit tip).
	 */
	float SpritExtendCm = 6.5f * 30.48f;
	FBoatWakeSim BoatWake;
	/** On-water true/apparent wind rose (web hWindCompass). */
	FWindCompassRing WindCompass;

	/** Spring-arm orbit (relative to boat). Yaw≈0 = from astern; pitch negative = elevated. */
	float OrbitYawDeg = 25.f;
	/** Default ~−16°: low exterior chase (less top-down than the old −28°). */
	float OrbitPitchDeg = -16.f;
	bool bOrbitRMBHeld = false;
	int32 CameraLagEnableFrames = 0;

	/** Load procedural loft (+ spars). bApplyDynamics: wire sailing params into VPP. */
	void LoadLoftMesh(bool bApplyDynamics = false);
	/** Navy topsides + red cove stripe on the player hull (HullPaint MIDs). */
	void ApplyPlayerHullPaint(UProceduralMeshComponent* HullMesh);
	void UpdateHullCollisionFromMesh();
	void PlaceSparFromEndpoints(UStaticMeshComponent* Comp, const FVector& A, const FVector& B);
	/** Place forestay foil + spin furler drum from jib stay endpoints (MeshS-scaled boat cm). */
	void UpdateJibFurlerVisuals(float MeshS);
	void PlaceBoxAt(UStaticMeshComponent* Comp, const FVector& Center, const FVector& Scale, const FRotator& Rot);
	void EnsureRunningRiggingBuilt();
	void UpdateRunningRigging();
	/** J/105 spreaders + forestay/backstay/shrouds (web standing rig). */
	void UpdateStandingRigging();
	void EnsureBoatLightsBuilt();
	void UpdateBoatLights();
	void ApplyCachedSailingToDynamics();
	/** Apply ActiveSpec scales to mesh + FBoatDynamics (Phase 3.2). */
	void ApplyActiveSpecToBoat();
	/** Dt<=0: snap boom to current sheet limit (load / settle). */
	void UpdateBoomFromSheet(float DeltaSeconds = -1.f);
	/**
	 * Gooseneck + tip in boat cm (scaled by MeshS), with current yaw + vang rise.
	 * Vang01: 0 hard on → tip low; 1 eased → tip rises up to MaxBoomRiseFrac·L.
	 */
	void GetBoomEndpointsBoat(float MeshS, FVector& OutBase, FVector& OutTip) const;
	void UpdateSailCloth(float DeltaSeconds);
	/** Build or rebuild main/jib cloth from current sail meshes if enabled. */
	void EnsureSailClothBuilt(bool bForceRebuild = false);
	/** Code Zero + retractable sprit (auto deploy on a reach). */
	void EnsureSpinAndSpritBuilt(bool bForceRebuild = false);
	void UpdateSpinAndSprit(float DeltaSeconds);
	/** |TWA| deg: 0 head-to-wind, 90 beam, 180 run. */
	float GetTrueWindAngleAbsDeg() const;
	/** Snap cloth rig + push meshes (after build / before first play frame). */
	void SettleSailClothImmediate();
	void ApplyClothForceToDynamics();
	/**
	 * Same as FBoatDynamics::LeeSign: +1 wind FROM starboard, −1 FROM port.
	 * Leeward (sails/boom) is the opposite side of the boat.
	 */
	float GetWindFromSign() const;
	/** Boom yaw sign toward leeward: +1 → port (wind from stbd), −1 → stbd. Matches VPP LeeSign. */
	float GetLeeSideSign() const;
	/** Wind FROM unit in boat frame (+X bow, +Y starboard). */
	FVector GetTrueWindBoatVector() const;
	FVector GetApparentWindBoatVector() const;
	/** Air velocity (downwind) unit for sail fill pressure. */
	FVector GetApparentWindAirVelBoat() const;
	/** Update on-water true/apparent wind rose (replaces old debug arrow). */
	void UpdateWindCompass(float DeltaSeconds);
	void UpdateWake(float DeltaSeconds);
	void EnsureOceanCoverage();
	void ApplyOrbitToSpringArm();
	/** Reset arm length / orbit / lag so PIE never starts inside the hull. */
	void RefreshChaseCamera();
	void ApplyDynamicsToTransform(float DeltaSeconds);
	void SampleMultiPointBuoyancy(const FVector& Loc, float CosH, float SinH,
		float& OutTargetZ, float& OutWavePitchDeg, float& OutWaveRollDeg) const;
	void OnMoveRight(float Value);
	void OnSheetAxis(float Value);
	void OnOuthaulAxis(float Value);
	void OnVangAxis(float Value);
	void OnJibCarAxis(float Value);
	void OnJibLeechAxis(float Value);
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
