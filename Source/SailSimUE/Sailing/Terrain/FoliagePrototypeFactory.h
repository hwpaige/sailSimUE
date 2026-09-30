#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
class UMaterialInterface;

/**
 * Builds lightweight tree/grass StaticMeshes for HISM foliage.
 * Prefer Content/Foliage/SM_* when present (drop free Plant packs there);
 * otherwise generate simple trunk+crown / grass-card prototypes at runtime.
 *
 * Species ids match bake: oak, cedar, scrub, lawn, beach, marsh.
 */
struct FFoliagePrototypeFactory
{
	static UStaticMesh* ResolveOrBuild(const FString& Species, UMaterialInterface* Material);
	static UStaticMesh* TryLoadContentOverride(const FString& Species);

	static UStaticMesh* BuildOak(UMaterialInterface* Material);
	static UStaticMesh* BuildCedar(UMaterialInterface* Material);
	static UStaticMesh* BuildScrub(UMaterialInterface* Material);
	static UStaticMesh* BuildGrassClump(UMaterialInterface* Material, float HeightCm, float WidthCm, int32 Blades);
};
