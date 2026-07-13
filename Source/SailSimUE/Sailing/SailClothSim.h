#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;

/**
 * Lightweight Verlet cloth on a single procedural mesh section (Phase 4 scaffold).
 * Pins luff near mast (local X≈0 in sail-pivot space), drives clew toward boom tip.
 * Not a full port of the web solver — enough to make sheet/trim read as cloth.
 */
struct FSailClothSim
{
	bool bEnabled = true;
	bool bInitialized = false;
	int32 SectionIndex = 0;

	/** Verlet particles in component-local space (sail pivot = mast base). */
	TArray<FVector> Pos;
	TArray<FVector> Prev;
	TArray<FVector> Rest;
	TArray<uint8> bPinned; // 1 = fixed

	struct FSpring
	{
		int32 A = 0;
		int32 B = 0;
		float RestLen = 0.f;
	};
	TArray<FSpring> Springs;

	/** Index of farthest |Y| or aft-most free particle for sheet (clew). */
	int32 ClewIndex = INDEX_NONE;

	float GravityCm = -980.f;
	float Damping = 0.994f;
	float StructuralStiffness = 0.72f; // 0..1 constraint blend per iter
	int32 ConstraintIters = 5;
	float WindForceScale = 16.f;
	float SheetPullScale = 0.42f; // how hard clew tracks boom tip

	/** 0..1 cloth fill / sheet-shape quality (updated each Step). */
	float LastFillQuality = 0.75f;
	float LastCamberCm = 0.f;

	bool BuildFromMesh(UProceduralMeshComponent* Mesh, int32 Section = 0);
	void Clear();
	void Step(
		float Dt,
		const FVector& WindLocalDir, // unit, component space
		float WindSpeedKn,
		const FVector& ClewTargetLocal, // boom tip in sail component space
		float SheetEase01);
	void PushToMesh(UProceduralMeshComponent* Mesh) const;

	/** Refresh LastFillQuality / LastCamberCm from current particle state. */
	void MeasureShape();
};
