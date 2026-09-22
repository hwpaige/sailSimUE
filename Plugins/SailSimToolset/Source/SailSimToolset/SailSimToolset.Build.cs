// Copyright Sail Buddy. All Rights Reserved.

using UnrealBuildTool;

public class SailSimToolset : ModuleRules
{
	public SailSimToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ToolsetRegistry",
		});

		// Game module public include path is Source/SailSimUE (Sailing/SailSimPerf.h).
		PrivateIncludePaths.Add(System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "../../../../Source/SailSimUE")));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"LevelEditor",
			"EditorFramework",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"ImageWrapper",
			"RenderCore",
			"RHI",
			"SailSimUE",
		});
	}
}
