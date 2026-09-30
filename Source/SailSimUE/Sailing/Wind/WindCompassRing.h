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
 * On-water true / apparent wind rose.
 *
 * Flat horizontal instrument: white ring, gold lubber, cyan true (P), amber
 * apparent (A), red north (N). Unlit self-colored material (M_WindCompass_Unlit)
 * so it never depends on scene lighting / yacht PBR.
 */
struct FWindCompassRing
{
	bool bEnabled = true;

	/** Ring radius (cm). Web R = 30 ft. */
	float RadiusCm = 30.f * 30.48f;
	/** Height above actor Z (cm). */
	float LiftCm = 2.2f * 30.48f;
	/** Hide when true wind is below this (kn). */
	float MinTwsKn = 0.1f;
	/**
	 * Peak night EmissiveBoost. Graph: Emissive = BaseColor * (1 + boost * 12).
	 * Day uses boost 0 (still fully visible via BaseColor * 1).
	 */
	float SoftFill = 0.06f;

	void Clear();
	void EnsureBuilt(ASailBoatPawn* Boat);
	void Update(ASailBoatPawn* Boat, float DeltaSeconds);

private:
	bool bBuilt = false;
	/** Bump when geometry/mat recipe changes so PIE rebuilds. */
	static constexpr int32 MatRecipeVersion = 21; // true wind world-stable
	int32 BuiltMatRecipe = 0;

	TWeakObjectPtr<USceneComponent> RootW;
	TWeakObjectPtr<UProceduralMeshComponent> StaticMeshW; // ring + ticks + lubber
	TWeakObjectPtr<UProceduralMeshComponent> TrueArrowW;  // section0 tri, section1 "P"
	TWeakObjectPtr<UProceduralMeshComponent> AppArrowW;   // section0 tri, section1 "A"
	TWeakObjectPtr<UProceduralMeshComponent> NorthArrowW; // section0 tri, section1 "N"
	TWeakObjectPtr<UTextRenderComponent> TrueLabelW;
	TWeakObjectPtr<UTextRenderComponent> AppLabelW;

	TWeakObjectPtr<UMaterialInstanceDynamic> RingMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> TrueMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> AppMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> NorthMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> LubberMatW;
	TWeakObjectPtr<UMaterialInstanceDynamic> MarkMatW;

	float RingNightBoost = 0.f;
	float TrueNightBoost = 0.f;
	float AppNightBoost = 0.f;
	float NorthNightBoost = 0.f;
	float LubberNightBoost = 0.f;
	float CachedNightGlow01 = -1.f;

	FString LastTrueTxt;
	FString LastAppTxt;

	float DispHdgDeg = 0.f;
	/** World true-wind FROM (deg). Never store true as boat-relative — that couples to yaw. */
	float DispTrueWindFromDeg = 0.f;
	/** Apparent is boat-relative (AWA); smooth in local frame. */
	float DispAppToRad = 0.f;
	float DispTrueLenCm = 0.f;
	float DispAppLenCm = 0.f;
	bool bDispInit = false;
	float BuiltTrueToRad = 1.e9f;
	float BuiltAppToRad = 1.e9f;
	float BuiltNorthRad = 1.e9f;
	float BuiltTrueLenCm = -1.f;
	float BuiltAppLenCm = -1.f;

	UProceduralMeshComponent* MakeMesh(ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name);
	UTextRenderComponent* MakeLabel(ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name, const FColor& Color);
	/** Unlit instrument MID: BaseColor + EmissiveBoost only. */
	UMaterialInstanceDynamic* MakeIndicatorMat(const FLinearColor& Color);

	static float SampleNightGlow01(const UWorld* World);
	void ApplyNightEmissive(float NightGlow01);

	void BuildStaticGeometry(UProceduralMeshComponent* Mesh);

	enum class EFaceLetter : uint8 { P, A, N };

	void PlaceWindArrow(
		UProceduralMeshComponent* Mesh,
		float FlowToRad,
		float LenCm,
		float PlaneZCm,
		EFaceLetter Letter,
		float& OutSeatRad,
		float& OutAltCm) const;

	void PlaceNorthArrow(
		UProceduralMeshComponent* Mesh,
		float NorthRad,
		float PlaneZCm,
		float& OutSeatRad,
		float& OutAltCm) const;

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
