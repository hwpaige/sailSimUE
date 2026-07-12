#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/** Load sail_geom.boat3d export JSON and fill a ProceduralMeshComponent. */
struct FBoatMeshFromJson
{
	/** Returns true if at least one section was created. Path is absolute or under Content/. */
	static bool LoadIntoProceduralMesh(
		UProceduralMeshComponent* Mesh,
		const FString& JsonPathOrContentRelative,
		UMaterialInterface* DefaultMaterial = nullptr);

	/** Default Content path for the J/105 loft. */
	static FString DefaultJ105Path();
};
