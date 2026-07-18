// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SailSimUE : ModuleRules
{
	public SailSimUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Headers under Source/SailSimUE/... (e.g. "Sailing/BoatDynamics.h")
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Water",
			"ProceduralMeshComponent",
			"Json",
			"JsonUtilities",
			"Landscape"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"UMG",
			"ImageWrapper",
			"RenderCore",
			"RHI",
			"MeshDescription",
			"StaticMeshDescription"
		});
	}
}
