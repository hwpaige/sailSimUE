#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Sailing/Ocean/IOceanHeightSampler.h"
#include "Sailing/Ocean/SailEnvPreset.h"
#include "SailOceanSubsystem.generated.h"

class ADirectionalLight;
class AActor;
class UMaterialInstanceDynamic;

/** BP-friendly sample. */
USTRUCT(BlueprintType)
struct FOceanSampleBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	FVector Surface = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	FVector Normal = FVector::UpVector;

	UPROPERTY(BlueprintReadOnly, Category = Ocean)
	bool bValid = false;
};

/**
 * World subsystem for ocean height + open-ocean visual setup.
 * One flat UE Water plane (no Gerstner, no second fill-plane layer).
 * The Open World template carves a central island hole via the Water Body Ocean
 * spline — we collapse that spline so stock water fills the full zone.
 *
 * Sky: the template often ships BOTH SM_SkySphere (legacy dome mesh) and
 * SkyAtmosphere — those fight and create a light/dark seam when looking around.
 * We hide the dome and keep SkyAtmosphere only.
 */
UCLASS()
class SAILSIMUE_API USailOceanSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	FOceanSampleBP SampleOceanBP(FVector WorldPos) const;

	FOceanSample SampleOcean(const FVector& WorldPos) const;

	void SetSeaParams(const FSeaParams& Params);
	FSeaParams GetSeaParams() const;
	FName GetBackendName() const;

	/**
	 * Open-ocean prep: hide island landscape, collapse ocean island hole,
	 * move Water zone under boat. Call whenever boat snaps (harbor is far from origin).
	 */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void PrepareOpenOcean(const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm);

	void EnsureOceanVisualCoverage(const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm);

	bool IsOpenOceanReady() const { return bOpenOceanPrepared; }

	/** Volumetric cloud intensity 0..1 (0 = fully off). */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void SetVolumetricCloudIntensity(float Intensity01);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetVolumetricCloudIntensity() const { return VolumetricCloudIntensity; }

	/** Exponential height fog intensity 0..1 (0 = fully off for testing). */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void SetFogIntensity(float Intensity01);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetFogIntensity() const { return FogIntensity; }

	/**
	 * Short high-frequency surface chop via water material normal strength (0..1).
	 * Used by wind puffs — not full Gerstner displacement (water stays a flat body).
	 */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void SetSurfaceChopIntensity(float Intensity01);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetSurfaceChopIntensity() const { return SurfaceChopIntensity; }

	/**
	 * Sky / fog / cloud / sun look. Fair Day restores the map's captured baseline
	 * (the polished daytime look you liked). Other presets derive from that capture.
	 * @return Suggested true-wind speed kn, or -1 if wind should be left alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float ApplyEnvPreset(uint8 PresetId);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	uint8 GetActiveEnvPreset() const { return static_cast<uint8>(ActiveEnvPreset); }

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	FString GetEnvPresetName(uint8 PresetId) const;

	/**
	 * Continuous time of day (hours 0..24). Noon (12) = captured Fair Day baseline
	 * (current fog/clouds/lighting). Does not overwrite user fog/cloud intensity sliders.
	 */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void SetTimeOfDayHours(float Hours0To24);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetTimeOfDayHours() const { return TimeOfDayHours; }

	/** 0..1 slider position (0 = midnight, 0.5 = noon Fair Day, 1 = midnight). */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	void SetTimeOfDay01(float Norm01);

	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetTimeOfDay01() const { return FMath::Clamp(TimeOfDayHours / 24.f, 0.f, 1.f); }

	/** e.g. "12:00  Fair Day" / "19:30  Dusk". */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	FString GetTimeOfDayLabel() const;

	/** 0 = full day, 1 = full night — for window/lamp emissives. */
	UFUNCTION(BlueprintCallable, Category = "SailSim|Ocean")
	float GetNightAmount() const;

	/** Capture sun/sky/fog baseline once (current map look = Fair Day). */
	void CaptureEnvBaselineIfNeeded();

private:
	struct FEnvBaseline
	{
		bool bValid = false;
		FRotator SunRotation = FRotator(0.f, 0.f, 0.f);
		float SunIntensity = 10.f;
		FLinearColor SunColor = FLinearColor(1.f, 0.98f, 0.94f, 1.f);
		float SunSourceAngle = 0.5357f;
		float SkyLightIntensity = 1.f;
		FLinearColor SkyLightColor = FLinearColor::White;
		float FogDensity = 0.02f;
		float SecondFogDensity = 0.f;
		FLinearColor FogInscattering = FLinearColor(0.f, 0.f, 0.f, 1.f);
		float FogHeightFalloff = 0.2f;
		float FogStartDistance = 0.f;
		float FogMaxOpacity = 1.f;
		float CloudIntensity = 1.f;
		float FogIntensity = 1.f;
		float CloudLayerBottomKm = 5.f;
		float AtmMulti = 1.f;
		float AtmRayleigh = 0.0331f;
		float AtmMie = 0.004f;
		FLinearColor AtmSkyLuminance = FLinearColor::White;
		float AtmHeightFogContribution = 1.f;
		bool bAtmCaptured = false;
	};
	FEnvBaseline EnvBaseline;
	ESailEnvPreset ActiveEnvPreset = ESailEnvPreset::FairDay;
	/** When true, sun/sky come from TimeOfDayHours (not a discrete env button). */
	bool bTimeOfDayDriven = true;
	/** Local solar time hours; 12 = Fair Day baseline. */
	float TimeOfDayHours = 12.f;

	void ApplySunAndSky(const FSailEnvPresetDesc& Desc);
	void ApplyFogLook(const FSailEnvPresetDesc& Desc);
	void ApplyAtmosphereLook(const FSailEnvPresetDesc& Desc);
	void ApplyCloudLayerLook(const FSailEnvPresetDesc& Desc);
	/** Build lighting desc for TimeOfDayHours (Fair Day at noon from capture). */
	FSailEnvPresetDesc BuildTimeOfDayDesc() const;
	void ApplyTimeOfDayInternal();
	static FSailEnvPresetDesc LerpEnvDesc(const FSailEnvPresetDesc& A, const FSailEnvPresetDesc& B, float T);
	/** Moon directional (atm light 1) + star dome; only visible on Night. */
	void EnsureNightSkyActors();
	void ApplyNightSky(bool bNight);
	void DestroyNightSkyActors();
	void UpdateNightSkyFollow(const FVector& BoatWorldPos);

	/** Runtime moon (AtmosphereSunLight index 1). */
	TWeakObjectPtr<ADirectionalLight> NightMoonLight;
	/** Large inverted star sphere (follows boat). */
	TWeakObjectPtr<AActor> NightStarDome;
	/** Distant emissive moon disc for a clear lunar disk. */
	TWeakObjectPtr<AActor> NightMoonDisc;
	TWeakObjectPtr<UMaterialInstanceDynamic> NightStarMid;
	TWeakObjectPtr<UMaterialInstanceDynamic> NightMoonDiscMid;
	bool bNightSkyActive = false;

	TUniquePtr<IOceanHeightSampler> Sampler;
	bool bOpenOceanPrepared = false;
	bool bWaterZonesConfigured = false;
	bool bTerrainHidden = false;
	bool bIslandHoleCollapsed = false;
	bool bFlatWavesCleared = false;
	bool bMaterialsPolished = false;
	bool bSkySeamsFixed = false;
	bool bLegacySkyDomeHidden = false;
	float FollowAccum = 0.f;
	float TerrainHideAccum = 0.f;
	FVector LastZoneBoatXY = FVector::ZeroVector;
	/** Last values applied to WaterZone (skip SetZoneExtent/rebuild if unchanged). */
	float LastAppliedZoneExtentCm = 0.f;
	float LastAppliedLocalTessCm = 0.f;

	/** 0 = clouds off, 1 = full template clouds. */
	float VolumetricCloudIntensity = 1.f;
	/** Captured once so intermediate intensity can restore layer height. */
	float CachedCloudLayerHeightKm = -1.f;

	/** 0 = fog off, 1 = template fog density. */
	float FogIntensity = 1.f;
	/** Baseline fog density captured once from the level. */
	float CachedFogDensity = -1.f;
	float CachedSecondFogDensity = -1.f;

	/** 0 = calm normals, 1 = short high-frequency chop (puff response). */
	float SurfaceChopIntensity = 0.f;

	void EnsureSampler();
	void HideIslandTerrain();
	/** Hide SM_SkySphere / M_SimpleSkyDome so only SkyAtmosphere draws the sky. */
	void HideLegacySkyDome();
	/** Collapse Water Body Ocean spline island hole so water fills the zone solidly. */
	void CollapseOceanIslandHole();
	/**
	 * Full water setup (extent, local tess, materials). Only MarkForRebuild when
	 * extents/tess actually change or on first configure — not on every boat move.
	 */
	void ConfigureWaterZones(const FVector& BoatWorldPos, float ZoneExtentCm, float LocalTessDiameterCm);
	/**
	 * Cheap follow: teleport WaterZone + ocean body under the boat WITHOUT
	 * MarkForRebuild / SetZoneExtent / UpdateAll. Local-only tessellation already
	 * slides the mesh around the view; we only re-center the zone when the boat
	 * nears the zone edge (was: PrepareOpenOcean → full mesh rebuild = hitch).
	 */
	void FollowWaterZone(const FVector& BoatWorldPos);
	/**
	 * Fix light/dark sky seam: kill legacy sky dome, re-center atmosphere/fog,
	 * extend cloud ray march, clear fog cutoff.
	 */
	void FixSkyAndAtmosphereSeams(const FVector& BoatWorldPos);
	void ApplyVolumetricCloudIntensity();
	void ApplyFogIntensity();
	/** Strip Gerstner / any WaterWaves asset so the body is a flat plane. */
	void ClearWaterWaves();
	void PolishWaterMaterials();
	void SoftenHorizonFog();
};
