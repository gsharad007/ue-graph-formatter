// Adaptagrams is isolated in its own dynamically loaded editor-module DLL under LGPL-2.1-or-later.

using UnrealBuildTool;
using System.IO;

public class GraphFormatterAdaptagrams : ModuleRules
{
	public GraphFormatterAdaptagrams(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.None;
		CppStandard = CppStandardVersion.Cpp20;
		bUseUnity = false;
		bEnableExceptions = true;
		bUseRTTI = true;
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UnsafeTypeCastWarningLevel = WarningLevel.Off;

		PublicDependencyModuleNames.Add("Core");
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "ThirdParty", "Adaptagrams"));
		PrivateDefinitions.Add("LIBAVOID_NO_DLL=1");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDefinitions.AddRange(
				new string[]
				{
					"_USE_MATH_DEFINES=1",
					"M_PI=3.14159265358979323846",
					"_CRT_SECURE_NO_WARNINGS=1"
				}
			);
		}
	}
}
