#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class ASailBoatPawn;

/**
 * Local ship-wake as a water-surface deformation (not a bright overlay object).
 *
 * Builds a grid patch around/behind the hull. Each vertex sits on the base ocean
 * sample plus a Kelvin-style height disturbance (trough + divergent arms +
 * transverse ripples). Material is water-like / ocean MID so it reads as the
 * sea itself being transformed.
 */
struct FBoatWakeSim
{
	bool bEnabled = true;

	/** Hull speed reference (kn) — displacement plateaus near this (J/105 ~7.3). */
	float HullSpeedKn = 7.3f;
	/** Min boatspeed (kn) before wake appears. */
	float MinSpeedKn = 0.55f;
	/** Peak trough depth at hull speed (cm, negative below ambient surface). */
	float PeakTroughCm = 28.f;
	/** Peak divergent-arm crest height at hull speed (cm). */
	float PeakArmCrestCm = 18.f;
	/** Peak transverse ripple height (cm). */
	float PeakTransverseCm = 8.f;
	/** How far aft the patch extends (cm). */
	float PatchAftCm = 4200.f;
	/** How far forward of origin the patch starts (cm, negative = aft of bow). */
	float PatchFwdCm = 200.f;
	/** Half-width of patch (cm). */
	float PatchHalfWidthCm = 1600.f;
	/** Grid resolution along track / across. */
	int32 GridAlong = 48;
	int32 GridAcross = 28;
	/** Kelvin half-angle (deg). Deep-water cusp ≈ 19.47°. */
	float KelvinHalfAngleDeg = 19.5f;
	/** Overall amplitude scale 0..1. */
	float Strength = 1.f;
	/** Rebuild interval (sec) — surface deforms smoothly without full every-frame cost. */
	float RebuildIntervalSec = 0.033f;

	void Clear();
	void EnsureMesh(ASailBoatPawn* Boat, USceneComponent* AttachRoot);
	void Update(ASailBoatPawn* Boat, float DeltaSeconds);

	/**
	 * Extra surface height (cm) from this boat's wake at a world XY.
	 * Used so gameplay float / samples can optionally feel the same field.
	 */
	float SampleWakeHeightCm(const ASailBoatPawn* Boat, const FVector& WorldXY) const;

private:
	float TimeSec = 0.f;
	float RebuildAccum = 0.f;
	float CachedAmp = 0.f;

	TWeakObjectPtr<UProceduralMeshComponent> MeshW;
	TWeakObjectPtr<UMaterialInstanceDynamic> WaterMidW;
	bool bMeshReady = false;

	void EnsureMaterial(ASailBoatPawn* Boat, UProceduralMeshComponent* Mesh);
	float SpeedAmp(float Kn) const;
	FVector SampleBaseSurface(const ASailBoatPawn* Boat, const FVector& WorldXY) const;
	/** Boat-local: +X bow, +Y starboard, origin at boat actor. */
	float WakeHeightLocal(float LocalX, float LocalY, float HalfLoa, float HalfBeam, float Amp) const;
	void RebuildMesh(ASailBoatPawn* Boat);
};
