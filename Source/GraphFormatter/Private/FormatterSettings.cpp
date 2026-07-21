/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "FormatterSettings.h"

UFormatterSettings::UFormatterSettings()
	: AutoDetectGraphEditor(false)
	, SupportedAssetTypes(
		  {
			  { "Blueprint",                        true },
			  { "AnimBlueprint",                    true },
			  { "WidgetBlueprint",                  true },
			  { "BehaviorTree",                     true },
			  { "Material",                         true },
			  { "SoundCue",                         true },
			  { "NiagaraScript",                    true },
			  { "NiagaraSystem",                    true },
			  { "MetaSoundSource",                  true },
			  { "LevelScriptBlueprint",             true },
			  { "EditorUtilityBlueprint",           true },
			  { "EditorUtilityWidgetBlueprint",     true },
			  { "PCGGraph",                         true },
			  { "InterchangeBlueprintPipelineBase", true },
			  { "MetaSoundPatch",                   true },
}
	  ),
	DisableToolbar(false), PositioningAlgorithm(EGraphFormatterPositioningAlgorithm::EFastAndSimpleMethodMedian),
	CommentBorder(45), HorizontalSpacing(100), VerticalSpacing(80), MaxLayerNodes(0),
	bEnableBlueprintParameterGroup(true), SpacingFactorOfParameterGroup(0.314), MaxOrderingIterations(10),
	bEnableK2Formatter(true), bEnableHybridGridSnap(true),
	K2LayoutMode(EGraphFormatterK2LayoutMode::PreserveHumanLayout), bPreserveComponentAnchor(true),
	K2LayoutCellSize(50), K2OrderingSweeps(8), K2RoutingPlanningWorkBudget(1000000), K2HorizontalSpacing(160),
	K2VerticalSpacing(96), K2BranchSpacing(96), K2ComponentSpacing(256), K2PureHorizontalSpacing(80),
	K2PureVerticalSpacing(48), K2CommentPadding(64), K2ObstacleClearance(48), K2RoutingChannelSpacing(32),
	K2MaxGeneratedKnots(6), K2LongDataWireRankThreshold(3), bRouteDataWires(true), bShowLayoutNotifications(true)
{
}
