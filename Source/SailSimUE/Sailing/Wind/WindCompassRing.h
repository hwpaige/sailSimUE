#pragma once

#include "CoreMinimal.h"

class ASailBoatPawn;
class USceneComponent;
class UProceduralMeshComponent;
class UTextRenderComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UFont;

/**
 * On-water true / apparent wind rose (port of web hBuildWindCompass).
 *
 * Flat horizontal ring, yaw with heading only. Cyan = true (P), amber = apparent (A),
 * red = north (N). Soft-fill materials so dusk doesn't crush them to black and bloom
 * doesn't wash them out. Letter marks are mesh glyphs on the triangle faces.
 */
struct FWindCompassRing
{
	bool bEnabled = true;

	/** Ring radius (cm). Web R = 30 ft. */
	float RadiusCm = 30.f * 30.48f;
	/** Height above actor Z (cm). Web lift = 2.2 ft. */
	float LiftCm = 2.2f * 30.48f;
	/** Hide when true wind is below this (kn). */
	float MinTwsKn = 0.1f;
	/**
	 * Peak night EmissiveBoost for arrows (Emissive = BaseColor × boost). Day = 0.
	 * Ring uses a slightly higher peak so it reads as a soft instrument at night.
	 */
	float SoftFill = 0.06f;

	void Clear();
	void EnsureBuilt(ASailBoatPawn* Boat);
	void Update(ASailBoatPawn* Boat, float DeltaSeconds);

private:
	bool bBuilt = false;
	/** Bump when material recipe changes so hot-reload rebuilds (avoids sticky bright MIDs). */
	static constexpr int32 MatRecipeVersion = 12;
	int32 BuiltMatRecipe = 0;

	TWeakObjectPtr<USceneComponent> RootW;
	TWeakObjectPtr<UProceduralMeshComponent> StaticMeshW; // ring + ticks + lubber
	TWeakObjectPtr<UProceduralMeshComponent> TrueArrowW;  // section0 tri, section1 "P"
	TWeakObjectPtr<UProceduralMeshComponent> AppArrowW;   // section0 tri, section1 "A"
	TWeakObjectPtr<UProceduralMeshComponent> NorthArrowW; // section0 tri, section1 "N"
	/** Outer degree readouts (TWD / AWA). */
	TWeakObjectPtr<UTextRenderComponent> TrueLabelW;
	TWeakObjectPtr<UTextRenderComponent> AppLabelW;

	TWeakObjectPtr<UMaterialInstanceDynamic> RingMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> TrueMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> AppMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> NorthMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> LubberMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> MarkMatW; // black letter strokes

	/** Per-mat peak night boost (multiplied by night factor each frame). Mark stays 0. */
	float RingNightBoost = 0.f;
	float TrueNightBoost = 0.f;
	float AppNightBoost = 0.f;
	float NorthNightBoost = 0.f;
	float LubberNightBoost = 0.f;
	float CachedNightGlow01 = -1.f;

	FString LastTrueTxt;
	FString LastAppTxt;

	UProceduralMeshComponent* MakeMesh(ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name);
	UTextRenderComponent* MakeLabel(ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name, const FColor& Color);
	/**
	 * Overlay MID (unlit translucent preferred): BaseColor + EmissiveBoost + Opacity.
	 * Translucent so water SSR (SceneColor) does not mirror the rose; Lumen/RT flags
	 * are also cleared on the primitives. Night fill via ApplyNightEmissive.
	 */
	UMaterialInstanceDynamic* MakeIndicatorMat(const FLinearColor& Color, float /*unusedFill*/ = -1.f);

	/** 0 = bright day (no glow), 1 = full night. Matches env preset / sun like boat lights. */
	static float SampleNightGlow01(const UWorld* World);
	/**
	 * Day: brighter BaseColor, EmissiveBoost=0.
	 * Night: darker BaseColor + soft EmissiveBoost (avoids exposure-wash white).
	 */
	void ApplyNightEmissive(float NightGlow01);

	void BuildStaticGeometry(UProceduralMeshComponent* Mesh);

	enum class EFaceLetter : uint8 { P, A, N };

	/** Wind-style: tip on rim, base outboard. Also rebuilds face letter (section 1). */
	void PlaceWindArrow(
		UProceduralMeshComponent* Mesh,
		float FlowToRad,
		float LenCm,
		float PlaneZCm,
		EFaceLetter Letter,
		float& OutSeatRad,
		float& OutAltCm) const;

	/** North-style: tip outboard. Also rebuilds face letter (section 1). */
	void PlaceNorthArrow(
		UProceduralMeshComponent* Mesh,
		float NorthRad,
		float PlaneZCm,
		float& OutSeatRad,
		float& OutAltCm) const;

	/** Build white stroke letter at triangle centroid into mesh section 1. */
	void BuildFaceLetter(
		UProceduralMeshComponent* Mesh,
		EFaceLetter Letter,
		float SeatRad,
		float MidR,
		float PlaneZCm,
		float ScaleCm) const;

	static FString FormatSignedAwa(float SignedAwaDeg);
	static void SetMeshVisible(UProceduralMeshComponent* Mesh, bool bVis);
	static void SetLabelVisible(UTextRenderComponent* Lab, bool bVis);
};
