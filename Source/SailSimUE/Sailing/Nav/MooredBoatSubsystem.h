#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Sailing/BoatMeshFromJson.h"
#include "ProceduralMeshComponent.h"
#include "MooredBoatSubsystem.generated.h"

class UProceduralMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UHierarchicalInstancedStaticMeshComponent;

/** One selected mooring that will host a parked J/105. */
USTRUCT()
struct FMooredBoatSlot
{
	GENERATED_BODY()

	FVector MooringWorldCm = FVector::ZeroVector;
	/** Initial/true heading of boat bow (deg, 0 = north / +X) — points toward mooring. */
	float HeadingDeg = 0.f;
	/** Pennant length ball → bow tip (cm). */
	float LineLenCm = 500.f;
	int32 MooringMeshId = 1;
};

/**
 * Per-resident mooring dynamics (inertial, not pure sin oscillators).
 * Real boats on a short pennant: heavy inertia, soft spring on rode, slow
 * sheer about head-to-wind, never snap-stop at sinusoid extrema.
 */
struct FMooredBoatSway
{
	int32 SlotIndex = INDEX_NONE;
	/** Per-boat phase offsets for filtered gust noise (rad). */
	float NoisePhaseA = 0.f;
	float NoisePhaseB = 0.f;
	float NoisePhaseC = 0.f;
	/** Low-passed local wind (kn / FROM deg). */
	float SmoothTwsKn = 12.f;
	float SmoothTwdFromDeg = 225.f;
	/** Inertial yaw state (deg / deg/s). Bow into wind at equilibrium. */
	float HeadingDeg = 0.f;
	float YawRateDegS = 0.f;
	/** Rode stretch (cm beyond base line) and rate. */
	float SurgeCm = 0.f;
	float SurgeRateCmS = 0.f;
	/** Attitude (deg) + rates. */
	float RollDeg = 0.f;
	float RollRateDegS = 0.f;
	float PitchDeg = 0.f;
	float PitchRateDegS = 0.f;
	float HeaveCm = 0.f;
	float HeaveRateCmS = 0.f;
	float TimeSec = 0.f;
	bool bInitialized = false;
	/** Multi-segment pennant (bow → ring). Transform-only updates each frame. */
	TArray<TWeakObjectPtr<UStaticMeshComponent>, TInlineAllocator<4>> PennantSegs;
};

/** Material role for a cached moored-boat mesh section. */
enum class EMooredHullPart : uint8
{
	HullGloss = 0,   // white gelcoat topsides
	BootStripe,      // dark waterline boot
	HullStripe,      // cove / sheer accent stripe
	Antifoul,        // below waterline
	Deck,            // deck / cockpit
	Cabin,           // cabin house
	Windows,         // cabin glass
	Keel,            // keel / rudder
	Other,
};

/** CPU-side hull section cache (loaded once, applied to every moored boat). */
struct FMooredHullSection
{
	TArray<FVector> Positions;
	TArray<int32> Indices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	/** Solid tint for MID BaseColor (stripes already in Colors where needed). */
	FLinearColor Tint = FLinearColor::White;
	bool bTwoSided = false;
	EMooredHullPart Part = EMooredHullPart::Other;
	float Roughness = 0.35f;
	float Metallic = 0.f;
	float Specular = 0.5f;
};

/**
 * Parks J/105 hulls (no sails, boom centered) on a fair subset of harbor
 * mooring balls, each hanging on a short bow pennant like a real mooring.
 */
UCLASS()
class SAILSIMUE_API UMooredBoatSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	bool bEnabled = true;

	/**
	 * Cap on selected moorings that get a boat (near full + mid HISM combined).
	 * Full multi-component actors only within NearFullRadiusCm; rest are HISM hulls.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats", meta = (ClampMin = "4", ClampMax = "120"))
	int32 MaxBoats = 16; // keep 16 (Prefer-ON density PASS); deepen via static/HISM, not count cut

	/** Fraction of floating moorings that get a boat (before MaxBoats). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats", meta = (ClampMin = "0.05", ClampMax = "0.8"))
	float OccupancyFraction = 0.28f;

	/** Min spacing between moored boat origins (cm). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float MinBoatSpacingCm = 1800.f;

	/** Prefer moorings within this radius of the harbor start (cm). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float PreferNearHarborCm = 90000.f;

	/** Stream mid HISM hulls within this radius of focus (cm). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float LoadRadiusCm = 90000.f;

	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float UnloadRadiusCm = 120000.f;

	/**
	 * Full actor (hull+mast+pennant) within this radius (cm). Field boats are
	 * Static HISM. Default ~25 m + MaxNearFullBoats → ~0–1 hero (not a ring of
	 * full actors). ClampMin 0 allows HISM-only.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Tiers", meta = (ClampMin = "0.0"))
	float NearFullRadiusCm = 2500.f;

	/** Cap on simultaneous NearFull actors (closest to focus). Field = Static HISM. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Tiers", meta = (ClampMin = "0", ClampMax = "8"))
	int32 MaxNearFullBoats = 1;

	/** HISM hull-only band: NearFull .. MidHism (cm). ~250 m. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Tiers", meta = (ClampMin = "5000.0"))
	float MidHismRadiusCm = 25000.f;

	/**
	 * True wind FROM direction (deg, 0 = north). Boats hang downwind of their
	 * ball with bow into the wind (toward the mooring).
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float WindFromDeg = 225.f;

	/** Base pennant length ball→bow (cm). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float BaseLineLenCm = 520.f;

	/** Waterline Z for boat origin (cm), matches player WaterlineOffsetCm. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float WaterlineOffsetCm = 4.f;

	/** How often to stream load/unload moored boats (sec). Sway runs every frame. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float UpdateIntervalSec = 0.75f;

	/** Ignore duplicate ForceStream calls within this window (sec). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats")
	float StreamDebounceSec = 1.25f;

	/**
	 * Wind-driven pendulum sway on NearFull residents. Default OFF — field boats
	 * are static harbor props. Optional ON for ≤1 hero close-up.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway")
	bool bEnableSway = false;

	/** Masthead all-round white anchor light on every resident (COLREGS at anchor). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights")
	bool bShowAnchorLights = true;

	/**
	 * Real UPointLightComponent on moored boats. Default OFF: emissive globe/halo
	 * already sells the lantern; N dynamic point lights (was 48 × 50 m) thrash
	 * deferred lighting on Mac. Enable only for hero close-ups / night photography.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights")
	bool bAnchorPointLights = false;

	/** Peak candelas at full night (scaled down in daylight). Only if bAnchorPointLights. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights", meta = (ClampMin = "5", ClampMax = "200"))
	float AnchorLightCandelas = 40.f;

	/** Point-light attenuation radius (cm). Keep small — not a stadium flood. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights", meta = (ClampMin = "500", ClampMax = "10000"))
	float AnchorLightAttenuationCm = 1800.f;

	/**
	 * Fraction of moored boats with cabin lights on (warm glow through glass).
	 * Deterministic per slot — stable across reloads.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights", meta = (ClampMin = "0", ClampMax = "1"))
	float CabinLitFraction = 0.5f;

	/** Peak cabin interior candelas at full night. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Lights", meta = (ClampMin = "5", ClampMax = "120"))
	float CabinLightCandelas = 40.f;

	/** Peak yaw sheer from head-to-wind at ~12 kn (deg). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "1", ClampMax = "25"))
	float SwayYawDegAt12kn = 8.f;

	/** Max roll at high wind (deg). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.5", ClampMax = "12"))
	float SwayRollDegMax = 2.8f;

	/** Peak rode stretch as fraction of pennant length. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.005", ClampMax = "0.08"))
	float SwaySurgeFrac = 0.025f;

	/**
	 * Cylinder segments for each bow pennant (2–4). More = smoother catenary,
	 * still cheap (transform-only updates, no PMC / cloth).
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "2", ClampMax = "4"))
	int32 PennantSegmentCount = 3;

	/** Must match EncAid MooringScale (SM_Buoy_1/2 instance scale). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.5", ClampMax = "8"))
	float MooringBuoyScale = 2.5f;

	/** Must match EncAid MooringZOffsetCm (ball actor Z). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway")
	float MooringBuoyZOffsetCm = 30.f;

	/** How fast local wind samples are low-passed (1/s). Lower = calmer field response. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.05", ClampMax = "2"))
	float WindSmoothRate = 0.35f;

	/** Yaw spring stiffness toward head-to-wind (higher = snappier return). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.05", ClampMax = "2"))
	float YawSpring = 0.22f;

	/** Yaw rate damping (critically-ish damped with spring). */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.1", ClampMax = "4"))
	float YawDamping = 0.85f;

	/** Max yaw rate (deg/s) — real moored boats rarely spin faster. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Sway", meta = (ClampMin = "0.5", ClampMax = "20"))
	float MaxYawRateDegS = 4.5f;

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	void ForceStreamAround(FVector FocusWorld);

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	bool ReloadSlots();

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	int32 GetSlotCount() const { return Slots.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	int32 GetResidentCount() const { return Resident.Num() + MidHismSlotToInstance.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	int32 GetNearFullCount() const { return Resident.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	int32 GetMidHismCount() const { return MidHismSlotToInstance.Num(); }

	UFUNCTION(BlueprintCallable, Category = "MooredBoats")
	FString GetStatusLine() const;

private:
	TArray<FMooredBoatSlot> Slots;
	bool bSlotsReady = false;
	float Accum = 0.f;
	float StreamDebounceLeft = 0.f;
	FVector LastFocus = FVector::ZeroVector;

	/** Slot index → near-band full multi-component actor. */
	UPROPERTY()
	TMap<int32, TObjectPtr<AActor>> Resident;

	/** Slot index → mid-band HISM instance id. */
	TMap<int32, int32> MidHismSlotToInstance;

	/** Holder actor for mid-field HISM hulls. */
	UPROPERTY()
	TObjectPtr<AActor> MidHismOwner = nullptr;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> MidHullHism = nullptr;

	/** Slot index → sway runtime (near full only). */
	TMap<int32, FMooredBoatSway> SwayState;

	/** Hull sections baked once from j105_boat3d.json (no sails, no keel/rudder). */
	TArray<FMooredHullSection> HullSections;
	/** Keel + rudder — separate mesh so they never cast onto topsides. */
	TArray<FMooredHullSection> KeelSections;
	FBoatJsonSparEndpoints TemplateSpars;
	float BowOffsetCm = 520.f;
	bool bTemplateReady = false;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SparMaterial = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LineMaterial = nullptr;

	/** Two-sided lit material for deck/cabin shells (owned by subsystem). */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> TwoSidedBaseMat = nullptr;

	/** Cached /Game/Materials/Yacht/MI_* (or fallbacks). Index = EMooredHullPart. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> YachtMats;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> YachtSparMat = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> YachtRopeMat = nullptr;

	/**
	 * Shared static mesh for all moored J/105 hulls (PMC is only a bake intermediate).
	 * Nanite is OFF by default: small hulls + thin paint stripes look blocky under Nanite
	 * position quantization. Single SM still collapses 14 PMC sections → far fewer draws/VSM.
	 */
	UPROPERTY()
	TObjectPtr<UStaticMesh> HullNaniteMesh = nullptr;

	bool bHullNaniteReady = false;

	/** If true, bake Nanite on the shared hull (looks soft on thin stripe bands). Prefer false. */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Rendering")
	bool bHullUseNanite = false;

	/**
	 * Mooring-field paint: hull topsides + cove stripe pick from common yacht colors
	 * (navy / dark green / red, plus classic white hull). Deck is always white.
	 */
	UPROPERTY(EditAnywhere, Category = "MooredBoats|Rendering")
	bool bVaryBoatPaint = true;

	bool ResolveAidsPath(FString& OutPath) const;
	void EnsureYachtMaterials();
	UMaterialInterface* MaterialForPart(EMooredHullPart Part) const;
	bool EnsureTemplate();
	/** Bake HullSections → Nanite UStaticMesh (editor full build; runtime fast fallback). */
	bool BuildHullNaniteMesh(UProceduralMeshComponent* SourcePmc);
	void ApplyHullTo(UProceduralMeshComponent* Hull) const;
	/** Keel/rudder only — always no cast shadow. */
	void ApplyKeelTo(UProceduralMeshComponent* KeelMesh) const;
	void ApplyHullMaterialsToStaticMesh(UStaticMeshComponent* HullSmc) const;
	/** Per-boat hull/stripe palette + white deck (MID overrides on the mesh). */
	void ApplyBoatPaintScheme(UMeshComponent* HullMesh, int32 SlotIndex) const;
	static void DisableAllCastShadows(UPrimitiveComponent* Prim);
	/** Drop Lumen/reflection/DF contribution only — keep full mesh LODs readable mid-harbor. */
	static void StripMooredReflectionCost(UPrimitiveComponent* Prim);
	void ClearAll();
	void RebuildAround(const FVector& Focus);
	void EnsureMidHism();
	void ClearMidHism();
	void AddOrUpdateMidHism(int32 SlotIndex, const FMooredBoatSlot& Slot);
	void RemoveMidHism(int32 SlotIndex);
	FTransform MakeMooredHullTransform(const FMooredBoatSlot& Slot) const;
	AActor* SpawnMooredBoat(const FMooredBoatSlot& Slot, int32 SlotIndex);
	void InitSwayState(int32 SlotIndex, TArrayView<UStaticMeshComponent* const> PennantSegs);
	void UpdateAllSway(float DeltaTime);
	void ApplySwayToBoat(int32 SlotIndex, AActor* Boat, FMooredBoatSway& State, float DeltaTime);
	/** Full setup: mesh, collision, material, transform (spawn / rigging only). */
	void PlaceCylinder(UStaticMeshComponent* Comp, const FVector& LocalA, const FVector& LocalB,
		float RadiusScale, UMaterialInterface* Mat) const;
	/** Hot path: relative transform only (pennant sway). Mesh/mat already set. */
	void UpdateCylinderTransform(UStaticMeshComponent* Comp, const FVector& LocalA,
		const FVector& LocalB, float RadiusScale) const;
	/** Cache SM_Buoy top-of-mesh (ring) height × scale; matches EncAid placement. */
	void CacheMooringRingHeight();
	/** World position of the mooring-ball top ring (not the buoy centre). */
	FVector GetMooringRingWorld(const FMooredBoatSlot& Slot) const;
	/** Cheap multi-segment catenary pennant (bow → ring). */
	void UpdatePennantRope(
		const FTransform& BoatXf,
		const FMooredBoatSlot& Slot,
		FMooredBoatSway& State,
		float WindFactor,
		float Gust) const;
	/** J/105 spreaders + forestay/backstay/shrouds (web standing rig). */
	void AddStandingRigging(AActor* Boat, USceneComponent* Root) const;
	/** Masthead white housing + lens + point light (always on; intensity follows day/night). */
	void AddAnchorLight(AActor* Boat, USceneComponent* Root) const;
	/** Interior cabin point light + warm glass emissive (only if slot is "lit"). */
	void AddCabinLight(AActor* Boat, USceneComponent* Root, UMeshComponent* HullMesh, int32 SlotIndex) const;
	/** True if this mooring slot has cabin lights on (~CabinLitFraction of fleet). */
	bool ShouldCabinBeLit(int32 SlotIndex) const;
	/** Warm tungsten emissive on window glass only (not coachroof / cabin shell). */
	void ApplyCabinGlassGlow(UMeshComponent* HullMesh, bool bLit, float VisScale) const;
	/** Refresh intensity / visibility for all resident anchor + cabin lights. */
	void UpdateAllAnchorLights();
	/** 0.15 (bright day) … 1.0 (night) — matches player boat VisScale. */
	float ComputeNightVisScale() const;
	FVector GetFocusLocation() const;

	float LightAccum = 0.f;
	/** Last applied day/night scale — skip updates when unchanged (avoids flicker). */
	float LastAnchorVisScale = -1.f;

	/** Local-Z of buoy mesh top ring (unscaled); × MooringBuoyScale → world offset above buoy actor. */
	float MooringRingLocalZCm = 48.f;
	bool bMooringRingCached = false;
};
