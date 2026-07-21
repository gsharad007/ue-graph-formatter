/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"
#include "FormatterGraph.h"
#include "FormatterSettings.generated.h"

UENUM()
enum class EGraphFormatterK2LayoutMode : uint8
{
	/** Improve an authored layout in place while preserving event-island position and reading order. */
	PreserveHumanLayout UMETA(DisplayName = "Preserve Human Layout"),
	/** Rebuild the selected graph as a fresh deterministic layered drawing. */
	FullReflow UMETA(DisplayName = "Full Reflow"),
};

UCLASS(config = EditorPerProjectUserSettings)
class GRAPHFORMATTER_API UFormatterSettings : public UObject
{
public:
	GENERATED_BODY()

	UFormatterSettings();

	/** Enable auto detect Graph Editor */
	UPROPERTY(config, EditAnywhere, Category = "Options")
	bool AutoDetectGraphEditor;

	/** All Asset types supported */
	UPROPERTY(config, EditAnywhere, Category = "Options")
	TMap<FString, bool> SupportedAssetTypes;

	/** Toolbar toggle */
	UPROPERTY(config, EditAnywhere, Category = "Options")
	bool DisableToolbar;

	/** Positioning algorithm*/
	UPROPERTY(config, EditAnywhere, Category = "Options")
	EGraphFormatterPositioningAlgorithm PositioningAlgorithm;

	/** Border thickness */
	UPROPERTY(config, EditAnywhere, Category = "Options", meta = (ClampMin = 1))
	int32 CommentBorder;

	/** Spacing between two layers */
	UPROPERTY(config, EditAnywhere, Category = "Options", meta = (ClampMin = 0))
	int32 HorizontalSpacing;

	/** Spacing between two nodes */
	UPROPERTY(config, EditAnywhere, Category = "Options", meta = (ClampMin = 0))
	int32 VerticalSpacing;

	/** Legacy compatibility value retained for config serialization. Deterministic layout deliberately ignores it. */
	UPROPERTY(config, meta = (DeprecatedProperty, DeprecationMessage = "MaxLayerNodes is ignored by deterministic layout."))
	int32 MaxLayerNodes;

	/** Legacy generic-formatter parameter grouping. Used only when the K2 formatter is disabled. */
	UPROPERTY(
		config,
		EditAnywhere,
		Category = "Legacy Generic Layout",
		meta = (DisplayName = "Enable Parameter Group (K2 Formatter Disabled)", EditCondition = "!bEnableK2Formatter")
	)
	bool bEnableBlueprintParameterGroup;

	/** Legacy parameter-group spacing factor. Used only when the K2 formatter is disabled. */
	UPROPERTY(
		config,
		EditAnywhere,
		Category = "Legacy Generic Layout",
		meta =
			(DisplayName = "Parameter Group Spacing (K2 Formatter Disabled)",
			 EditCondition = "!bEnableK2Formatter && bEnableBlueprintParameterGroup")
	)
	FVector2D SpacingFactorOfParameterGroup;

	/** Vertex ordering max iterations */
	UPROPERTY(config, EditAnywhere, Category = "Performance", meta = (ClampMin = 0, ClampMax = 100))
	int32 MaxOrderingIterations;

	/** Use the Blueprint-semantic execution-first formatter for supported K2 graphs. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout")
	bool bEnableK2Formatter;

	/** Snap columns and free lane roots to the graph grid while preserving exact primary exec alignment. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout")
	bool bEnableHybridGridSnap;

	/** Preserve authored event islands and local node groupings, or explicitly rebuild the layout. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout")
	EGraphFormatterK2LayoutMode K2LayoutMode;

	/** Legacy config value. Per-island preservation is selected through K2LayoutMode. */
	UPROPERTY(
		config,
		meta = (DeprecatedProperty, DeprecationMessage = "Use K2LayoutMode. Whole-scope anchor translation has been replaced by per-island preservation.")
	)
	bool bPreserveComponentAnchor;

	/** Coarse visual grid unit used for execution columns and event-island gutters. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 1, ClampMax = 1024))
	int32 K2LayoutCellSize;

	/** Number of deterministic alternating crossing-reduction sweeps. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Performance", meta = (ClampMin = 1, ClampMax = 32))
	int32 K2OrderingSweeps;

	/** Deterministic upper bound for each graph-wide readability and routing geometry pass. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Performance", meta = (ClampMin = 1000, ClampMax = 10000000))
	int32 K2RoutingPlanningWorkBudget;

	/** Horizontal clearance between execution columns. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2HorizontalSpacing;

	/** Vertical clearance between nodes in the same execution column. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2VerticalSpacing;

	/** Additional clearance reserved between sibling branch lanes. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2BranchSpacing;

	/** Vertical clearance between disconnected execution components. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2ComponentSpacing;

	/** Horizontal clearance between pure-data provider columns. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2PureHorizontalSpacing;

	/** Vertical clearance between pure-data providers feeding the same consumer. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2PureVerticalSpacing;

	/** Padding applied when resizing comments around formatted contents. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Spacing", meta = (ClampMin = 0))
	int32 K2CommentPadding;

	/** Clearance around unrelated nodes used by collision resolution and wire routing. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Routing", meta = (ClampMin = 0))
	int32 K2ObstacleClearance;

	/** Spacing between deterministic rectilinear routing channels. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Routing", meta = (ClampMin = 1))
	int32 K2RoutingChannelSpacing;

	/** Maximum generated reroute nodes on one logical wire. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Routing", meta = (ClampMin = 1, ClampMax = 16))
	int32 K2MaxGeneratedKnots;

	/** Route data wires that span at least this many execution ranks. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Routing", meta = (ClampMin = 1))
	int32 K2LongDataWireRankThreshold;

	/** Include obstructed, backward, and unusually long data wires in the opt-in routing command. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout|Routing")
	bool bRouteDataWires;

	/** Show actionable notifications when a graph cannot be measured or formatted safely. */
	UPROPERTY(config, EditAnywhere, Category = "K2 Layout")
	bool bShowLayoutNotifications;

	/** Straight connections old settings */
	UPROPERTY(config, Category = "Graph Formatter", BlueprintReadWrite)
	FVector2D ForwardSplineTangentFromHorizontalDelta;
	UPROPERTY(config, Category = "Graph Formatter", BlueprintReadWrite)
	FVector2D ForwardSplineTangentFromVerticalDelta;
	UPROPERTY(config, Category = "Graph Formatter", BlueprintReadWrite)
	FVector2D BackwardSplineTangentFromHorizontalDelta;
	UPROPERTY(config, Category = "Graph Formatter", BlueprintReadWrite)
	FVector2D BackwardSplineTangentFromVerticalDelta;
};
