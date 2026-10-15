// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LoomaXRViewerUE58 : ModuleRules
{
	public LoomaXRViewerUE58(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput" });

		// The Live Link proof (HAM-316): a JSON-over-UDP source and a pose applier, in LiveLink/.
		PrivateDependencyModuleNames.AddRange(new string[] { "LiveLinkInterface", "Sockets", "Networking", "Json", "LoomaSceneSync" });

		// VR mode (VR-9), in VR/: the headset pawn, its motion controllers, and the
		// OpenXR session it starts with its own input bound.
		PrivateDependencyModuleNames.AddRange(new string[] { "HeadMountedDisplay", "XRBase", "OpenXRInput" });

		// Looma.LiveLink.Assign reads the editor's selection, so a character can be picked in the outliner.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
