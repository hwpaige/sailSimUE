#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WindFieldSubsystem.generated.h"

/** Local true-wind sample (met FROM convention). */
USTRUCT(BlueprintType)
struct FWindSample
{
	GENERATED_BODY()

	/** True wind speed (kn). */
	UPROPERTY(BlueprintReadOnly, Category = "Wind")
	float SpeedKn = 12.f;

	/** True wind FROM direction (deg, 0 = north). */
	UPROPERTY(BlueprintReadOnly, Category = "Wind")
	float DirFromDeg = 225.f;

	/** Gust factor above base (0 = calm base, 0.3 = +30% puff). */
	UPROPERTY(BlueprintReadOnly, Category = "Wind")
	float GustFrac = 0.f;

	/**
	 * Signed header/lift relative to base wind FROM (deg).
	 * + = wind shifts clockwise (right-hand) on the compass.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Wind")
	float DirShiftDeg = 0.f;
};

/** One traveling wind puff / cat's paw (logic only — no mesh footprint). */
USTRUCT()
struct FWindPuff
{
	GENERATED_BODY()

	FVector2D CenterCm = FVector2D::ZeroVector;
	/** Ellipse semi-axes (cm): major along wind, minor across. */
	float MajorCm = 8000.f;
	float MinorCm = 4000.f;
	/** Peak speed boost (kn) at core. */
	float SpeedBoostKn = 4.f;
	/** Peak direction shift (deg) at core (random lift/header). */
	float DirShiftDeg = 0.f;
	float AgeSec = 0.f;
	float LifeSec = 35.f;
};

/**
 * Spatially varying true wind: base + traveling puffs (gusts with lifts/headers).
 * Samples feed the boat VPP and minimap; local gust strength drives water surface chop.
 */
UCLASS()
class SAILSIMUE_API UWindFieldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	UPROPERTY(EditAnywhere, Category = "Wind|Base")
	float BaseSpeedKn = 12.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Base")
	float BaseDirFromDeg = 225.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	bool bEnablePuffs = true;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs", meta = (ClampMin = "2", ClampMax = "40"))
	int32 TargetPuffCount = 10;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float SpawnRadiusCm = 160000.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float CullRadiusCm = 240000.f;

	/** Cat's-paw life (s). Real puffs often persist ~1–3 min, not 15 s. */
	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float MinLifeSec = 55.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float MaxLifeSec = 140.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float MinGustKn = 1.5f;

	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float MaxGustKn = 5.5f;

	/** Peak header/lift inside a puff (deg). Milder = less twitchy helm. */
	UPROPERTY(EditAnywhere, Category = "Wind|Puffs")
	float MaxDirShiftDeg = 8.f;

	/**
	 * Low-pass time constants for the boat-local wind (seconds to ~63% of a step).
	 * Matches how true wind *feels* on the water: shifts build over tens of seconds,
	 * not frame-to-frame. Applied in SampleWindAt when bSmoothLocal is true (default
	 * path used by the boat each tick).
	 */
	UPROPERTY(EditAnywhere, Category = "Wind|Feel", meta = (ClampMin = "2", ClampMax = "120"))
	float LocalSpeedSmoothTauSec = 14.f;

	UPROPERTY(EditAnywhere, Category = "Wind|Feel", meta = (ClampMin = "2", ClampMax = "180"))
	float LocalDirSmoothTauSec = 28.f;

	/**
	 * Drive water near-normal strength from local gust (short high-frequency chop look).
	 * 0 = off, 1 = full response.
	 */
	UPROPERTY(EditAnywhere, Category = "Wind|Water", meta = (ClampMin = "0", ClampMax = "1"))
	float WaterChopResponse = 1.f;

	UFUNCTION(BlueprintCallable, Category = "Wind")
	void SetBaseWind(float SpeedKn, float DirFromDeg);

	/**
	 * Instantaneous field sample (no temporal smoothing). Used for maps / debug.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wind")
	FWindSample SampleWindAt(FVector WorldPos) const;

	/**
	 * Boat-local sample: field value low-passed in time so direction/speed don't jump.
	 * Call once per frame from the boat; advances the smoother with DeltaSeconds.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wind")
	FWindSample SampleSmoothedWindAt(FVector WorldPos, float DeltaSeconds);

	UFUNCTION(BlueprintCallable, Category = "Wind")
	FWindSample SampleWindLatLon(double Lat, double Lon) const;

	/** Update sample focus only (cheap; call every frame from the boat). */
	UFUNCTION(BlueprintCallable, Category = "Wind")
	void SetFocus(FVector FocusWorld);

	/**
	 * Set focus and immediately rebuild puff population (possess / teleport).
	 * Do not call every frame — use SetFocus instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wind")
	void ForceFocus(FVector FocusWorld);

	UFUNCTION(BlueprintCallable, Category = "Wind")
	int32 GetPuffCount() const { return Puffs.Num(); }

	UFUNCTION(BlueprintCallable, Category = "Wind")
	FString GetStatusLine() const;

	const TArray<FWindPuff>& GetPuffs() const { return Puffs; }

private:
	TArray<FWindPuff> Puffs;
	FVector LastFocus = FVector::ZeroVector;
	float SpawnAccum = 0.f;
	float TimeSec = 0.f;
	bool bBootstrapped = false;
	float LastChopApplied = -1.f;

	/** Temporal smoother state for SampleSmoothedWindAt (boat feel). */
	bool bLocalSmoothInit = false;
	float SmoothSpeedKn = 12.f;
	float SmoothDirFromDeg = 225.f;

	void EvolvePuffs(float Dt);
	void MaintainPopulation(bool bForce);
	void SpawnPuffNear(const FVector2D& Focus, bool bAimAtBoat);
	void ApplyWaterChopFromLocalGust();
	float PuffWeight(const FWindPuff& P, const FVector2D& Pos) const;
	FVector2D Focus2D() const;
};
