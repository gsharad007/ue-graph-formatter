/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

namespace UnrealBuildTool.Rules
{
	using System.IO;

	public class GraphFormatter : ModuleRules
	{
		public GraphFormatter(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PublicIncludePaths.Add(ModuleDirectory);
			PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "ThirdParty", "BlueprintAutoLayout"));
			PrivateDefinitions.Add("BLUEPRINTAUTOLAYOUT_API=");
			PrivateIncludePathModuleNames.Add("GraphFormatterAdaptagrams");
			DynamicallyLoadedModuleNames.Add("GraphFormatterAdaptagrams");

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"ApplicationCore",
					"InputCore",
					"Engine",
					"Kismet",
					"UnrealEd",
					"SlateCore",
					"Slate",
					"EditorStyle",
					"GraphEditor",
					"Json",
					"Projects",
					"BlueprintGraph",
					"MaterialEditor",
					"AIModule",
					"AIGraph",
					"BehaviorTreeEditor",
				}
			);
		}
	}
}
