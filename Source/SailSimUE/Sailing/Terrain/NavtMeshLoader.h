#pragma once

#include "CoreMinimal.h"

/** Parsed NAVT (Nantucket terrain) mesh — web bake format. */
struct FNavtMeshData
{
	TArray<FVector> Positions; // UE cm
	TArray<FVector> Normals;
	TArray<FLinearColor> Colors;
	TArray<int32> Indices;
	FBox Bounds = FBox(ForceInit);

	bool IsValid() const { return Positions.Num() >= 3 && Indices.Num() >= 3; }
};

/**
 * Load sail-sim NAVT binary (bake-terrain-tiles / writeNavtMesh):
 *   header 16: 'NAVT' u16 ver, pad2, u32 nV, u32 nI
 *   float32 positions xyz (x=north ft, y=elev m, z=east ft)
 *   rgb8 colors (3 bytes/vert) + pad to 4-byte boundary
 *   uint32 indices
 */
struct FNavtMeshLoader
{
	/** Convert NAVT feet/m → UE world cm (+X north, +Y east, +Z up). */
	static FVector NavtToUeCm(float XFt, float ElevM, float ZFtEast)
	{
		static constexpr float CmPerFt = 30.48f;
		static constexpr float CmPerM = 100.f;
		return FVector(XFt * CmPerFt, ZFtEast * CmPerFt, ElevM * CmPerM);
	}

	static bool LoadFile(const FString& AbsPath, FNavtMeshData& Out);
};
