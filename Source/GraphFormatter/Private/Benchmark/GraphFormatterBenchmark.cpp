/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Benchmark/GraphFormatterBenchmark.h"

#include "Benchmark/ElkLayoutAdapter.h"
#include "Benchmark/SGraphFormatterBenchmark.h"
#include "BlueprintAutoLayout.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "FormatterSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "GraphFormatterAdaptagrams.h"
#include "HAL/FileManager.h"
#include "K2/K2GraphFormatter.h"
#include "K2/K2RerouteRouter.h"
#include "K2Node.h"
#include "K2Node_Knot.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SGraphPanel.h"
#include "Widgets/SWindow.h"

namespace GraphFormatter::Benchmark
{
namespace BenchmarkPrivate
{
constexpr double FallbackNodeWidth = 160.0;
constexpr double FallbackNodeHeight = 80.0;
constexpr double FallbackPinTop = 32.0;
constexpr double FallbackPinPitch = 24.0;
constexpr double BenchmarkWindowWidth = 1900.0;
constexpr double BenchmarkWindowHeight = 1200.0;

[[nodiscard]]
const TCHAR* BackendStableId(const EFormatterBackend Backend) noexcept
{
	switch (Backend)
	{
		case EFormatterBackend::GraphFormatter:
			return TEXT("graph_formatter_native");
		case EFormatterBackend::BlueprintAutoLayout:
			return TEXT("blueprint_auto_layout_0_6_9");
		case EFormatterBackend::GraphFormatterLibavoid:
			return TEXT("graph_formatter_libavoid");
		case EFormatterBackend::ElkLayered:
			return TEXT("elk_layered_0_12_0");
	}
	return TEXT("unknown");
}

struct FLibavoidBinding
{
	UEdGraphPin* OutputPin = nullptr;
	UEdGraphPin* InputPin = nullptr;
	FString StableKey;
	bool bExecution = false;
};

[[nodiscard]]
FString PinIdentity(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	const FString NodeId = Node != nullptr && Node->NodeGuid.IsValid()
							 ? Node->NodeGuid.ToString(EGuidFormats::Digits)
							 : (Node != nullptr ? Node->GetName() : TEXT("NoNode"));
	const FString PinId = Pin.PinId.IsValid() ? Pin.PinId.ToString(EGuidFormats::Digits) : Pin.PinName.ToString();
	return FString::Printf(TEXT("%s:%s:%d"), *NodeId, *PinId, static_cast<int32>(Pin.Direction.GetValue()));
}

void ResolveTopologyTargets(const UEdGraphPin& InputPin, TSet<const UEdGraphPin*>& Visited, TArray<const UEdGraphPin*>& OutTargets)
{
	if (Visited.Contains(&InputPin)) { return; }
	Visited.Add(&InputPin);
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(InputPin.GetOwningNodeUnchecked());
	if (Knot == nullptr || &InputPin != Knot->GetInputPin())
	{
		OutTargets.Add(&InputPin);
		return;
	}
	if (const UEdGraphPin* OutputPin = Knot->GetOutputPin())
	{
		for (const UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
		{
			if (LinkedPin != nullptr) { ResolveTopologyTargets(*LinkedPin, Visited, OutTargets); }
		}
	}
}

[[nodiscard]]
TArray<FString> CaptureLogicalTopology(const UEdGraph& Graph)
{
	TArray<FString> Links;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UK2Node_Knot>()) { continue; }
		for (const UEdGraphPin* OutputPin : Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				if (LinkedPin == nullptr) { continue; }
				TSet<const UEdGraphPin*> Visited;
				TArray<const UEdGraphPin*> Targets;
				ResolveTopologyTargets(*LinkedPin, Visited, Targets);
				for (const UEdGraphPin* Target : Targets)
				{
					Links.Add(PinIdentity(*OutputPin) + TEXT("->") + PinIdentity(*Target));
				}
			}
		}
	}
	Links.Sort();
	return Links;
}

[[nodiscard]]
UEdGraph* FindMatchingGraph(UBlueprint& Blueprint, const UEdGraph& SourceGraph)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	if (SourceGraph.GraphGuid.IsValid())
	{
		if (UEdGraph** Match =
				Graphs.FindByPredicate([&SourceGraph](const UEdGraph* Candidate)
									   { return Candidate != nullptr && Candidate->GraphGuid == SourceGraph.GraphGuid; }))
		{
			return *Match;
		}
	}
	if (UEdGraph** Match =
			Graphs.FindByPredicate([&SourceGraph](const UEdGraph* Candidate)
								   { return Candidate != nullptr && Candidate->GetFName() == SourceGraph.GetFName(); }))
	{
		return *Match;
	}
	return nullptr;
}

[[nodiscard]]
bool BuildScope(UEdGraph& Graph, TSet<UEdGraphNode*>& OutScope)
{
	OutScope.Reset();
	if (Cast<UEdGraphSchema_K2>(Graph.GetSchema()) == nullptr) { return false; }
	int32 SemanticNodeCount = 0;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		if (!Node->IsA<UK2Node>() && !Node->IsA<UEdGraphNode_Comment>()) { return false; }
		OutScope.Add(Node);
		SemanticNodeCount += Node->IsA<UEdGraphNode_Comment>() ? 0 : 1;
	}
	return SemanticNodeCount >= 2;
}

[[nodiscard]]
FVector2D RectSize(const FSlateRect& Rect)
{ return FVector2D(Rect.Right - Rect.Left, Rect.Bottom - Rect.Top); }

[[nodiscard]]
FVector2D ResolveNodeSize(const UEdGraphNode& Node, const K2::FGraphGeometrySnapshot& Geometry)
{
	if (const K2::FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(&Node))
	{
		if (NodeGeometry->Bounds.IsSet()) { return RectSize(NodeGeometry->Bounds.GetValue()); }
	}
	if (Node.IsA<UK2Node_Knot>()) { return FVector2D(K2::RerouteKnotWidth, K2::RerouteKnotHeight); }
	return FVector2D(
		Node.NodeWidth > 0 ? static_cast<double>(Node.NodeWidth) : FallbackNodeWidth,
		Node.NodeHeight > 0 ? static_cast<double>(Node.NodeHeight) : FallbackNodeHeight
	);
}

void SeedMeasuredNodeSizes(UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry)
{
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UK2Node_Knot>()) { continue; }
		const FVector2D Size = ResolveNodeSize(*Node, Geometry);
		Node->NodeWidth = FMath::Max(1, FMath::RoundToInt32(Size.X));
		Node->NodeHeight = FMath::Max(1, FMath::RoundToInt32(Size.Y));
	}
}

[[nodiscard]]
int32 DirectionOrdinal(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return 0; }
	int32 Ordinal = 0;
	for (const UEdGraphPin* Candidate : Node->Pins)
	{
		if (Candidate == &Pin) { break; }
		if (Candidate != nullptr && Candidate->Direction == Pin.Direction) { ++Ordinal; }
	}
	return Ordinal;
}

[[nodiscard]]
FVector2D ResolvePinAnchor(const UEdGraphPin& Pin, const K2::FGraphGeometrySnapshot& Geometry)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return FVector2D::ZeroVector; }
	const FVector2D Position(Node->NodePosX, Node->NodePosY);
	if (const K2::FGraphPinGeometrySnapshot* PinGeometry = Geometry.FindPin(&Pin))
	{
		return Position + PinGeometry->NodeOffset;
	}
	const FVector2D Size = ResolveNodeSize(*Node, Geometry);
	if (Node->IsA<UK2Node_Knot>()) { return Position + Size * 0.5; }
	const double MaximumY = FMath::Max(FallbackPinTop, Size.Y - FallbackPinPitch * 0.5);
	const double Y = FMath::Clamp(
		FallbackPinTop + static_cast<double>(DirectionOrdinal(Pin)) * FallbackPinPitch, FallbackPinTop, MaximumY
	);
	return Position + FVector2D(Pin.Direction == EGPD_Input ? 0.0 : Size.X, Y);
}

[[nodiscard]]
FString StableEdgeKey(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin)
{ return FString::Printf(TEXT("libavoid:%s->%s"), *PinIdentity(OutputPin), *PinIdentity(InputPin)); }

void BuildLibavoidProblem(
	UEdGraph& Graph,
	const K2::FGraphGeometrySnapshot& Geometry,
	TArray<FGraphFormatterAdaptagramsObstacle>& OutObstacles,
	TArray<FGraphFormatterAdaptagramsConnection>& OutConnections,
	TArray<FLibavoidBinding>& OutBindings,
	TArray<K2::FRerouteObstacle>& OutNativeObstacles,
	TSet<UEdGraphNode*>& OutScope
)
{
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>()) { continue; }
		FGraphFormatterAdaptagramsObstacle& Obstacle = OutObstacles.AddDefaulted_GetRef();
		Obstacle.Minimum = FVector2D(Node->NodePosX, Node->NodePosY);
		Obstacle.Maximum = Obstacle.Minimum + ResolveNodeSize(*Node, Geometry);
		K2::FRerouteObstacle& NativeObstacle = OutNativeObstacles.AddDefaulted_GetRef();
		NativeObstacle.Node = Node;
		NativeObstacle.Bounds = FBox2D(Obstacle.Minimum, Obstacle.Maximum);
		OutScope.Add(Node);
	}

	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UK2Node_Knot>()) { continue; }
		for (UEdGraphPin* OutputPin : Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (UEdGraphPin* InputPin : OutputPin->LinkedTo)
			{
				if (InputPin == nullptr || InputPin->Direction != EGPD_Input || InputPin->LinkedTo.Num() != 1
					|| InputPin->GetOwningNodeUnchecked() == nullptr
					|| InputPin->GetOwningNodeUnchecked()->IsA<UK2Node_Knot>())
				{
					continue;
				}
				FLibavoidBinding& Binding = OutBindings.AddDefaulted_GetRef();
				Binding.OutputPin = OutputPin;
				Binding.InputPin = InputPin;
				Binding.StableKey = StableEdgeKey(*OutputPin, *InputPin);
				Binding.bExecution = OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
								  && InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;

				FGraphFormatterAdaptagramsConnection& Connection = OutConnections.AddDefaulted_GetRef();
				Connection.StableId = OutConnections.Num() - 1;
				Connection.Source = ResolvePinAnchor(*OutputPin, Geometry);
				Connection.Target = ResolvePinAnchor(*InputPin, Geometry);
				Connection.SourceDirection = EGraphFormatterRouteDirection::Right;
				Connection.TargetDirection = EGraphFormatterRouteDirection::Left;
			}
		}
	}
}

[[nodiscard]]
TArray<FVector2D> SimplifyWaypoints(const TArray<FVector2D>& Points)
{
	TArray<FVector2D> Result;
	for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
	{
		const FVector2D Point = Points[Index];
		if (!Result.IsEmpty() && Result.Last().Equals(Point, 0.5)) { continue; }
		Result.Add(Point);
		while (Result.Num() >= 2)
		{
			const FVector2D Before = Result.Num() == 2 ? Points[0] : Result[Result.Num() - 3];
			const FVector2D Middle = Result[Result.Num() - 2];
			const FVector2D After = Result.Last();
			const bool bCollinearX = FMath::IsNearlyEqual(Before.X, Middle.X, 0.5)
								  && FMath::IsNearlyEqual(Middle.X, After.X, 0.5);
			const bool bCollinearY = FMath::IsNearlyEqual(Before.Y, Middle.Y, 0.5)
								  && FMath::IsNearlyEqual(Middle.Y, After.Y, 0.5);
			if (!bCollinearX && !bCollinearY) { break; }
			Result.RemoveAt(Result.Num() - 2);
		}
	}
	return Result;
}

[[nodiscard]]
K2::FRerouteSettings MakeNativeRoutingSettings(const UFormatterSettings& Settings)
{
	K2::FRerouteSettings Result;
	Result.ObstacleClearance = Settings.K2ObstacleClearance;
	Result.ChannelSpacing = Settings.K2RoutingChannelSpacing;
	Result.MaxKnotsPerWire = Settings.K2MaxGeneratedKnots;
	Result.LongDataWireRankThreshold = Settings.K2LongDataWireRankThreshold;
	Result.PlanningWorkBudget = Settings.K2RoutingPlanningWorkBudget;
	Result.bRouteDataWires = Settings.bRouteDataWires;
	return Result;
}

[[nodiscard]]
TArray<K2::FRerouteEdge> MakeNativeRoutingEdges(
	const TConstArrayView<FLibavoidBinding> Bindings, const TConstArrayView<FGraphFormatterAdaptagramsConnection> Connections
)
{
	TArray<K2::FRerouteEdge> Edges;
	Edges.Reserve(Bindings.Num());
	for (int32 Index = 0; Index < Bindings.Num() && Connections.IsValidIndex(Index); ++Index)
	{
		const FLibavoidBinding& Binding = Bindings[Index];
		K2::FRerouteEdge& Edge = Edges.AddDefaulted_GetRef();
		Edge.OutputPin = Binding.OutputPin;
		Edge.InputPin = Binding.InputPin;
		Edge.OutputAnchor = Connections[Index].Source;
		Edge.InputAnchor = Connections[Index].Target;
		Edge.StableKey = Binding.StableKey;
		Edge.bExecution = Binding.bExecution;
	}
	return Edges;
}

K2::FRerouteResult ApplyLibavoidRouting(FBenchmarkCandidate& Candidate, const UFormatterSettings& FormatterSettings)
{
	if (Candidate.Graph == nullptr)
	{
		K2::FRerouteResult Failed;
		Failed.SkippedWires = 1;
		Failed.Diagnostics.Add(TEXT("The libavoid candidate graph is unavailable."));
		return Failed;
	}
	UEdGraph& Graph = *Candidate.Graph;
	TArray<FGraphFormatterAdaptagramsObstacle> Obstacles;
	TArray<FGraphFormatterAdaptagramsConnection> Connections;
	TArray<FLibavoidBinding> Bindings;
	TArray<K2::FRerouteObstacle> NativeObstacles;
	TSet<UEdGraphNode*> Scope;
	BuildLibavoidProblem(Graph, Candidate.Geometry, Obstacles, Connections, Bindings, NativeObstacles, Scope);
	Candidate.Telemetry.Add(TEXT("libavoid_obstacles"), Obstacles.Num());
	Candidate.Telemetry.Add(TEXT("direct_connections_considered"), Connections.Num());

	const K2::FRerouteSettings NativeSettings = MakeNativeRoutingSettings(FormatterSettings);
	const TArray<K2::FRerouteEdge> BaselineEdges = MakeNativeRoutingEdges(Bindings, Connections);
	const K2::FReroutePlan TriggerPlan = K2::FK2RerouteRouter::Plan(
		BaselineEdges, NativeObstacles, Scope, NativeSettings, FormatterSettings.K2LayoutCellSize
	);
	TSet<FString> RequiredRouteKeys;
	for (const FString& Key : TriggerPlan.RequiredRouteKeys)
	{
		RequiredRouteKeys.Add(Key);
	}
	Candidate.Telemetry.Add(TEXT("connections_requiring_routing"), RequiredRouteKeys.Num());
	if (RequiredRouteKeys.IsEmpty())
	{
		Candidate.Diagnostics.Add(
			FString::Printf(TEXT("libavoid adapter inspected %d direct wires; none had a routing defect."), Connections.Num())
		);
		return {};
	}

	FGraphFormatterAdaptagramsSettings Settings;
	Settings.ShapeBufferDistance = FormatterSettings.K2ObstacleClearance;
	Settings.IdealNudgingDistance = FormatterSettings.K2RoutingChannelSpacing;
	const FGraphFormatterAdaptagramsResult Routed =
		IGraphFormatterAdaptagramsModule::Get().RouteOrthogonal(Obstacles, Connections, Settings);
	Candidate.Diagnostics.Add(Routed.Diagnostic);
	Candidate.Telemetry.Add(TEXT("connections_submitted_to_libavoid"), Connections.Num());
	if (!Routed.bSucceeded)
	{
		K2::FRerouteResult Failed;
		Failed.SkippedWires = RequiredRouteKeys.Num();
		Failed.Diagnostics.Add(Routed.Diagnostic);
		return Failed;
	}

	TMap<FString, TArray<FVector2D>> ProposedWaypoints;
	int32 KnotLimitRejections = 0;
	for (const FGraphFormatterAdaptagramsRoute& Route : Routed.Routes)
	{
		if (!Bindings.IsValidIndex(Route.StableId)) { continue; }
		const FLibavoidBinding& Binding = Bindings[Route.StableId];
		if (!RequiredRouteKeys.Contains(Binding.StableKey)) { continue; }
		TArray<FVector2D> Waypoints = SimplifyWaypoints(Route.Points);
		if (Waypoints.IsEmpty()) { continue; }
		if (Waypoints.Num() > FormatterSettings.K2MaxGeneratedKnots)
		{
			++KnotLimitRejections;
			continue;
		}
		ProposedWaypoints.Add(Binding.StableKey, MoveTemp(Waypoints));
	}
	Candidate.Telemetry.Add(TEXT("libavoid_routes_with_bends"), ProposedWaypoints.Num());
	Candidate.Telemetry.Add(TEXT("libavoid_knot_limit_rejections"), KnotLimitRejections);

	TArray<K2::FRerouteEdge> ValidationEdges = BaselineEdges;
	for (K2::FRerouteEdge& Edge : ValidationEdges)
	{
		if (const TArray<FVector2D>* Waypoints = ProposedWaypoints.Find(Edge.StableKey))
		{
			Edge.PreferredWaypoints = *Waypoints;
			Edge.bValidatePreferredWaypointsOnly = true;
		}
		else
		{
			Edge.bReservationOnly = true;
		}
	}
	K2::FReroutePlan AcceptedPlan = K2::FK2RerouteRouter::Plan(
		ValidationEdges, NativeObstacles, Scope, NativeSettings, FormatterSettings.K2LayoutCellSize
	);
	AcceptedPlan.SkippedWires = FMath::Max(0, RequiredRouteKeys.Num() - AcceptedPlan.Routes.Num());
	K2::FRerouteResult Result = K2::FK2RerouteRouter::ApplyPlan(Graph, AcceptedPlan);
	Candidate.Telemetry.Add(TEXT("libavoid_routes_accepted"), Result.RoutedWires);
	Candidate.Telemetry.Add(TEXT("libavoid_knots_created"), Result.CreatedKnots);
	Candidate.Diagnostics.Add(
		FString::Printf(
			TEXT("Conservative adapter: %d/%d defect wires accepted, %d knots created; clear wires stayed direct."),
			Result.RoutedWires,
			RequiredRouteKeys.Num(),
			Result.CreatedKnots
		)
	);
	return Result;
}

void ConfigureBlueprintAutoLayout(
	FBlueprintLayoutConfig& Config, const UFormatterSettings& Settings, const K2::FGraphGeometrySnapshot& Geometry
)
{
	Config.NodePaddingX = FMath::Max(Settings.K2HorizontalSpacing, Settings.K2LayoutCellSize);
	Config.NodePaddingY = FMath::Max(Settings.K2VerticalSpacing, Settings.K2LayoutCellSize);
	Config.BranchExtraPaddingY = FMath::Max(Settings.K2BranchSpacing, Settings.K2LayoutCellSize);
	Config.RootExtraPaddingY = FMath::Max(Settings.K2ComponentSpacing, Settings.K2LayoutCellSize * 2);
	Config.PureNodeGapX = FMath::Max(Settings.K2PureHorizontalSpacing, Settings.K2LayoutCellSize);
	Config.PureNodePaddingY = FMath::Max(Settings.K2PureVerticalSpacing, Settings.K2LayoutCellSize);
	Config.CommentPadding = Settings.K2CommentPadding;
	Config.RerouteObstacleMargin = Settings.K2ObstacleClearance;
	Config.LayeredRankSpacingX = static_cast<float>(Config.NodePaddingX);
	Config.LayeredNodeSpacingY = static_cast<float>(Config.NodePaddingY);
	Config.bUseLayeredEngine = true;
	Config.bMaterializeLongEdges = true;
	Config.PinYOffsetResolver = [&Geometry](const UEdGraphPin& Pin) -> TOptional<float>
	{
		const K2::FGraphPinGeometrySnapshot* PinGeometry = Geometry.FindPin(&Pin);
		return PinGeometry != nullptr ? TOptional<float>{ static_cast<float>(PinGeometry->NodeOffset.Y) }
									  : TOptional<float>{};
	};
}

[[nodiscard]]
FIntPoint FindLayoutAnchor(const UEdGraph& Graph)
{
	FIntPoint Anchor(MAX_int32, MAX_int32);
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>()) { continue; }
		Anchor.X = FMath::Min(Anchor.X, Node->NodePosX);
		Anchor.Y = FMath::Min(Anchor.Y, Node->NodePosY);
	}
	return Anchor.X == MAX_int32 ? FIntPoint::ZeroValue : Anchor;
}

void DescribeCandidateConfiguration(
	FBenchmarkCandidate& Candidate, const UFormatterSettings& Settings, const int32 InitialSemanticNodeCount, const int32 InitialLogicalWireCount
)
{
	const TCHAR* NativePlacementMode = Settings.K2LayoutMode == EGraphFormatterK2LayoutMode::PreserveHumanLayout
										 ? TEXT("preserve authored human layout")
										 : TEXT("full graph reflow");
	Candidate.Configuration.Add(TEXT("scope"), TEXT("whole active K2 graph"));
	Candidate.Configuration.Add(TEXT("geometry"), TEXT("source Slate snapshot remapped by node and pin identity"));
	Candidate.NumericConfiguration.Add(TEXT("layout_cell_size"), Settings.K2LayoutCellSize);
	Candidate.NumericConfiguration.Add(TEXT("horizontal_spacing"), Settings.K2HorizontalSpacing);
	Candidate.NumericConfiguration.Add(TEXT("vertical_spacing"), Settings.K2VerticalSpacing);
	Candidate.NumericConfiguration.Add(TEXT("branch_spacing"), Settings.K2BranchSpacing);
	Candidate.NumericConfiguration.Add(TEXT("component_spacing"), Settings.K2ComponentSpacing);
	Candidate.NumericConfiguration.Add(TEXT("obstacle_clearance"), Settings.K2ObstacleClearance);
	Candidate.NumericConfiguration.Add(TEXT("routing_channel_spacing"), Settings.K2RoutingChannelSpacing);
	Candidate.NumericConfiguration.Add(TEXT("maximum_knots_per_wire"), Settings.K2MaxGeneratedKnots);
	Candidate.Telemetry.Add(TEXT("initial_semantic_nodes"), InitialSemanticNodeCount);
	Candidate.Telemetry.Add(TEXT("initial_logical_wires"), InitialLogicalWireCount);
	Candidate.Telemetry.Add(TEXT("geometry_node_records_remapped"), Candidate.Geometry.Nodes.Num());
	Candidate.Telemetry.Add(TEXT("geometry_pin_records_remapped"), Candidate.Geometry.Pins.Num());
	const FGraphFormatterAdaptagramsSettings LibavoidDefaults;

	switch (Candidate.Backend)
	{
		case EFormatterBackend::GraphFormatter:
			Candidate.Configuration.Add(TEXT("layout"), TEXT("GraphFormatter semantic K2 placement"));
			Candidate.Configuration.Add(TEXT("placement_mode"), NativePlacementMode);
			Candidate.Configuration.Add(TEXT("router"), TEXT("GraphFormatter conservative native router"));
			break;
		case EFormatterBackend::BlueprintAutoLayout:
			Candidate.Configuration.Add(TEXT("layout"), TEXT("Blueprint Auto Layout 0.6.9 layered engine"));
			Candidate.Configuration.Add(TEXT("placement_mode"), TEXT("full graph reflow"));
			Candidate.Configuration.Add(
				TEXT("pin_geometry"), TEXT("captured source pin offsets with upstream ordinal fallback")
			);
			Candidate.Configuration.Add(
				TEXT("router"), TEXT("upstream long-edge materialization plus obstacle reroute pass")
			);
			Candidate.NumericConfiguration.Add(
				TEXT("layered_rank_spacing"), FMath::Max(Settings.K2HorizontalSpacing, Settings.K2LayoutCellSize)
			);
			Candidate.NumericConfiguration.Add(
				TEXT("layered_node_spacing"), FMath::Max(Settings.K2VerticalSpacing, Settings.K2LayoutCellSize)
			);
			Candidate.NumericConfiguration.Add(TEXT("materialize_long_edges"), 1.0);
			Candidate.NumericConfiguration.Add(TEXT("run_obstacle_router"), 1.0);
			break;
		case EFormatterBackend::GraphFormatterLibavoid:
			Candidate.Configuration.Add(TEXT("layout"), TEXT("GraphFormatter semantic K2 placement"));
			Candidate.Configuration.Add(TEXT("placement_mode"), NativePlacementMode);
			Candidate.Configuration.Add(
				TEXT("router"), TEXT("libavoid orthogonal proposals behind GraphFormatter defect and safety gates")
			);
			Candidate.Configuration.Add(
				TEXT("routing_scope"), TEXT("only direct wires with backward, obstruction, or crossing defects")
			);
			Candidate.NumericConfiguration.Add(TEXT("libavoid_segment_penalty"), LibavoidDefaults.SegmentPenalty);
			Candidate.NumericConfiguration.Add(TEXT("libavoid_crossing_penalty"), LibavoidDefaults.CrossingPenalty);
			Candidate.NumericConfiguration.Add(TEXT("libavoid_shared_path_penalty"), LibavoidDefaults.SharedPathPenalty);
			Candidate.NumericConfiguration.Add(TEXT("libavoid_port_direction_penalty"), LibavoidDefaults.PortDirectionPenalty);
			Candidate.NumericConfiguration.Add(
				TEXT("libavoid_reverse_direction_penalty"), LibavoidDefaults.ReverseDirectionPenalty
			);
			break;
		case EFormatterBackend::ElkLayered:
			Candidate.Configuration.Add(TEXT("layout"), TEXT("elkjs 0.12.0 ELK Layered"));
			Candidate.Configuration.Add(TEXT("placement_mode"), TEXT("full directed graph reflow"));
			Candidate.Configuration.Add(TEXT("ports"), TEXT("fixed-position WEST/EAST ports from captured pin offsets"));
			Candidate.Configuration.Add(
				TEXT("router"), TEXT("ELK orthogonal sections materialized as transient K2 reroute nodes")
			);
			Candidate.Configuration.Add(
				TEXT("stability"), TEXT("source Y/X model order used as a crossing-minimization preference")
			);
			Candidate.Configuration.Add(
				TEXT("runtime"), TEXT("unmodified EPL-2.0 elkjs bundle in an external Node.js process")
			);
			Candidate.NumericConfiguration.Add(TEXT("elk_execution_edge_priority"), 1000.0);
			Candidate.NumericConfiguration.Add(TEXT("elk_data_edge_priority"), 1.0);
			Candidate.NumericConfiguration.Add(TEXT("elk_thoroughness"), 20.0);
			Candidate.NumericConfiguration.Add(TEXT("elk_separate_connected_components"), 0.0);
			break;
	}
}

void RunGraphFormatterCandidate(FBenchmarkCandidate& Candidate, const UFormatterSettings& Settings, const bool bLibavoid)
{
	TSet<UEdGraphNode*> Scope;
	if (Candidate.Graph == nullptr || !BuildScope(*Candidate.Graph, Scope))
	{
		Candidate.Diagnostics.Add(TEXT("The graph is not a supported K2 graph with at least two semantic nodes."));
		return;
	}
	const K2::FK2FormatResult FormatResult =
		K2::FK2GraphFormatter::Format(*Candidate.Graph, Candidate.Geometry, Scope, !bLibavoid, Settings);
	Candidate.Telemetry.Add(TEXT("nodes_moved"), FormatResult.MovedNodeCount);
	Candidate.Telemetry.Add(TEXT("comments_resized"), FormatResult.ResizedCommentCount);
	Candidate.Telemetry.Add(TEXT("native_routes_created"), FormatResult.RoutedWireCount);
	Candidate.Telemetry.Add(TEXT("native_knots_created"), FormatResult.CreatedKnotCount);
	Candidate.Telemetry.Add(TEXT("native_routes_skipped"), FormatResult.SkippedRerouteWireCount);
	Candidate.Diagnostics.Append(FormatResult.Diagnostics);
	if (!FormatResult.Message.IsEmpty()) { Candidate.Diagnostics.Add(FormatResult.Message); }
	const bool bLayoutSucceeded = FormatResult.Status == K2::EK2FormatStatus::Formatted
							   || FormatResult.Status == K2::EK2FormatStatus::NoChanges;
	if (!bLayoutSucceeded) { return; }
	if (bLibavoid)
	{
		const K2::FRerouteResult RouteResult = ApplyLibavoidRouting(Candidate, Settings);
		Candidate.Diagnostics.Append(RouteResult.Diagnostics);
		if (RouteResult.HasFatalError()) { return; }
	}
	Candidate.bSucceeded = true;
}

void RunBlueprintAutoLayoutCandidate(FBenchmarkCandidate& Candidate, const UFormatterSettings& Settings)
{
	if (Candidate.Graph == nullptr) { return; }
	FBlueprintLayoutConfig Config;
	ConfigureBlueprintAutoLayout(Config, Settings, Candidate.Geometry);
	const FIntPoint Anchor = FindLayoutAnchor(*Candidate.Graph);
	FBlueprintAutoLayout Layout(Config);
	const int32 PositionedNodes = Layout.LayoutAndRouteGraph(Candidate.Graph, Anchor.X, Anchor.Y);
	Candidate.bSucceeded = PositionedNodes > 0;
	Candidate.Telemetry.Add(TEXT("nodes_positioned"), PositionedNodes);
	Candidate.Telemetry.Add(TEXT("measured_pin_offsets_available"), Candidate.Geometry.Pins.Num());
	Candidate.Diagnostics.Add(FString::Printf(TEXT("Blueprint Auto Layout positioned %d nodes."), PositionedNodes));
}

void RunElkLayeredCandidate(FBenchmarkCandidate& Candidate, const UFormatterSettings& Settings, const FString& ArtifactDirectory)
{
	if (Candidate.Graph == nullptr) { return; }
	const FElkLayoutResult Layout = RunElkLayeredLayout(*Candidate.Graph, Candidate.Geometry, Settings, ArtifactDirectory);
	Candidate.bSucceeded = Layout.bSucceeded;
	Candidate.Configuration.Add(TEXT("node_executable"), Layout.NodeExecutable);
	Candidate.Telemetry.Add(TEXT("nodes_positioned"), Layout.PositionedNodes);
	Candidate.Telemetry.Add(TEXT("ports_submitted"), Layout.SubmittedPorts);
	Candidate.Telemetry.Add(TEXT("measured_port_offsets_used"), Layout.MeasuredPorts);
	Candidate.Telemetry.Add(TEXT("edges_submitted"), Layout.SubmittedEdges);
	Candidate.Telemetry.Add(TEXT("elk_edges_with_routes"), Layout.EdgesWithRoutes);
	Candidate.Telemetry.Add(TEXT("elk_routes_materialized"), Layout.MaterializedRoutes);
	Candidate.Telemetry.Add(TEXT("elk_knots_created"), Layout.CreatedKnots);
	Candidate.Telemetry.Add(TEXT("elk_knot_limit_rejections"), Layout.KnotLimitRejections);
	Candidate.Diagnostics.Append(Layout.Diagnostics);
}

[[nodiscard]]
TSharedPtr<FJsonObject> MetricsToJson(const FGraphQualityMetrics& Metrics)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("semantic_nodes"), Metrics.SemanticNodeCount);
	Json->SetNumberField(TEXT("reroute_nodes"), Metrics.RerouteNodeCount);
	Json->SetNumberField(TEXT("added_reroute_nodes"), Metrics.AddedRerouteNodeCount);
	Json->SetNumberField(TEXT("moved_semantic_nodes"), Metrics.MovedSemanticNodeCount);
	Json->SetNumberField(TEXT("moved_reroute_nodes"), Metrics.MovedRerouteNodeCount);
	Json->SetNumberField(TEXT("node_overlaps"), Metrics.NodeOverlapCount);
	Json->SetNumberField(TEXT("backward_exec_edges"), Metrics.BackwardExecutionEdgeCount);
	Json->SetNumberField(TEXT("backward_data_edges"), Metrics.BackwardDataEdgeCount);
	Json->SetNumberField(TEXT("non_straight_primary_exec_edges"), Metrics.NonStraightPrimaryExecutionEdgeCount);
	Json->SetNumberField(TEXT("exec_crossings"), Metrics.ExecutionCrossingCount);
	Json->SetNumberField(TEXT("data_crossings"), Metrics.DataCrossingCount);
	Json->SetNumberField(TEXT("wires_through_nodes"), Metrics.WireThroughNodeCount);
	Json->SetNumberField(TEXT("insufficient_exec_gaps"), Metrics.InsufficientExecutionGapCount);
	Json->SetNumberField(TEXT("off_grid_x"), Metrics.OffGridXCount);
	Json->SetNumberField(TEXT("off_grid_y"), Metrics.OffGridYCount);
	Json->SetNumberField(TEXT("logical_wires"), Metrics.LogicalWireCount);
	Json->SetNumberField(TEXT("bends"), Metrics.BendCount);
	Json->SetNumberField(TEXT("backward_data_distance"), Metrics.BackwardDataDistance);
	Json->SetNumberField(TEXT("primary_exec_vertical_error"), Metrics.PrimaryExecutionVerticalError);
	Json->SetNumberField(TEXT("rendered_wire_length"), Metrics.TotalRenderedWireLength);
	Json->SetNumberField(TEXT("drawing_area"), Metrics.DrawingArea);
	Json->SetNumberField(TEXT("total_node_movement"), Metrics.TotalNodeMovement);
	Json->SetNumberField(TEXT("maximum_node_movement"), Metrics.MaximumNodeMovement);
	Json->SetNumberField(TEXT("total_reroute_node_movement"), Metrics.TotalRerouteNodeMovement);
	Json->SetNumberField(TEXT("maximum_reroute_node_movement"), Metrics.MaximumRerouteNodeMovement);
	Json->SetNumberField(TEXT("composite_penalty"), Metrics.CompositePenalty);
	return Json;
}

[[nodiscard]]
const TCHAR* GeometryStatusName(const K2::EGraphGeometrySnapshotStatus Status) noexcept
{
	switch (Status)
	{
		case K2::EGraphGeometrySnapshotStatus::Ready:
			return TEXT("ready");
		case K2::EGraphGeometrySnapshotStatus::NeedsRetry:
			return TEXT("needs_retry");
		case K2::EGraphGeometrySnapshotStatus::InvalidInput:
			return TEXT("invalid_input");
	}
	return TEXT("unknown");
}

[[nodiscard]]
TSharedPtr<FJsonObject> GeometryToJson(const K2::FGraphGeometrySnapshot& Geometry)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("status"), GeometryStatusName(Geometry.Status));
	Json->SetNumberField(TEXT("requested_nodes"), Geometry.RequestedNodeCount);
	Json->SetNumberField(TEXT("node_records"), Geometry.Nodes.Num());
	Json->SetNumberField(TEXT("captured_node_bounds"), Geometry.CapturedNodeBoundsCount);
	Json->SetNumberField(
		TEXT("node_bounds_coverage"),
		Geometry.RequestedNodeCount > 0 ? static_cast<double>(Geometry.CapturedNodeBoundsCount) / Geometry.RequestedNodeCount
										: 1.0
	);
	Json->SetNumberField(TEXT("requested_visible_pins"), Geometry.RequestedVisiblePinCount);
	Json->SetNumberField(TEXT("captured_pin_anchors"), Geometry.CapturedPinCount);
	Json->SetNumberField(TEXT("pin_records"), Geometry.Pins.Num());
	Json->SetNumberField(
		TEXT("visible_pin_anchor_coverage"),
		Geometry.RequestedVisiblePinCount > 0
			? static_cast<double>(Geometry.CapturedPinCount) / Geometry.RequestedVisiblePinCount
			: 0.0
	);
	Json->SetStringField(
		TEXT("pin_anchor_coverage_status"), Geometry.RequestedVisiblePinCount > 0 ? TEXT("measured") : TEXT("not_captured")
	);

	int32 SlateSizedNodes = 0;
	int32 DesiredSizedNodes = 0;
	int32 PersistedSizedNodes = 0;
	int32 UnavailableSizedNodes = 0;
	for (const TPair<const UEdGraphNode*, K2::FGraphNodeGeometrySnapshot>& Pair : Geometry.Nodes)
	{
		switch (Pair.Value.SizeSource)
		{
			case K2::EGraphNodeSizeSource::SlateGeometry:
				++SlateSizedNodes;
				break;
			case K2::EGraphNodeSizeSource::DesiredSize:
				++DesiredSizedNodes;
				break;
			case K2::EGraphNodeSizeSource::PersistedNode:
				++PersistedSizedNodes;
				break;
			case K2::EGraphNodeSizeSource::Unavailable:
				++UnavailableSizedNodes;
				break;
		}
	}
	Json->SetNumberField(TEXT("slate_sized_nodes"), SlateSizedNodes);
	Json->SetNumberField(TEXT("desired_sized_nodes"), DesiredSizedNodes);
	Json->SetNumberField(TEXT("persisted_sized_nodes"), PersistedSizedNodes);
	Json->SetNumberField(TEXT("unavailable_sized_nodes"), UnavailableSizedNodes);

	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	for (const K2::FGraphGeometryDiagnostic& Diagnostic : Geometry.Diagnostics)
	{
		DiagnosticValues.Add(MakeShared<FJsonValueString>(Diagnostic.ToString()));
	}
	Json->SetArrayField(TEXT("diagnostics"), DiagnosticValues);
	return Json;
}

[[nodiscard]]
TSharedPtr<FJsonObject> StringMapToJson(const TMap<FString, FString>& Values)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Values.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		Json->SetStringField(Key, Values.FindChecked(Key));
	}
	return Json;
}

[[nodiscard]]
TSharedPtr<FJsonObject> NumberMapToJson(const TMap<FString, double>& Values)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Values.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		Json->SetNumberField(Key, Values.FindChecked(Key));
	}
	return Json;
}

[[nodiscard]]
TSharedPtr<FJsonObject> ScreenshotToJson(const FString& Filename, const FIntPoint Size)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	const bool bCaptured = !Filename.IsEmpty() && Size.X > 0 && Size.Y > 0;
	Json->SetBoolField(TEXT("captured"), bCaptured);
	Json->SetStringField(TEXT("file"), Filename);
	Json->SetNumberField(TEXT("width"), Size.X);
	Json->SetNumberField(TEXT("height"), Size.Y);
	return Json;
}

[[nodiscard]]
bool SaveJson(const FString& Filename, const TSharedRef<FJsonObject>& Json, FString& OutError)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Json, Writer))
	{
		OutError = FString::Printf(TEXT("Could not serialize '%s'."), *Filename);
		return false;
	}
	if (!FFileHelper::SaveStringToFile(Serialized, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write '%s'."), *Filename);
		return false;
	}
	return true;
}

[[nodiscard]]
bool InitializeCandidateCopy(
	FBenchmarkCandidate& Candidate,
	UBlueprint& SourceBlueprint,
	const UEdGraph& SourceGraph,
	const K2::FGraphGeometrySnapshot& SourceGeometry,
	const FString& ObjectSuffix,
	FString& OutError
)
{
	const FName DuplicateName = MakeUniqueObjectName(
		GetTransientPackage(), SourceBlueprint.GetClass(), *FString::Printf(TEXT("GF_Bakeoff_%s"), *ObjectSuffix)
	);
	{
		// UE's Blueprint merge tool uses this guard for read-only transient copies. It suppresses
		// compilation while preserving a valid Blueprint context for self-typed nodes.
		FGuardValue_Bitfield(SourceBlueprint.bDuplicatingReadOnly, true);
		Candidate.Blueprint = DuplicateObject<UBlueprint>(&SourceBlueprint, GetTransientPackage(), DuplicateName);
	}
	if (Candidate.Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("Could not duplicate the Blueprint for %s."), *ObjectSuffix);
		return false;
	}
	Candidate.Blueprint->ClearFlags(RF_Public | RF_Standalone);
	Candidate.Blueprint->SetFlags(RF_Transient | RF_Transactional);
	Candidate.Graph = FindMatchingGraph(*Candidate.Blueprint, SourceGraph);
	if (Candidate.Graph == nullptr)
	{
		OutError =
			FString::Printf(TEXT("Could not find graph '%s' in the %s copy."), *SourceGraph.GetName(), *ObjectSuffix);
		return false;
	}
	Candidate.Graph->SetFlags(RF_Transient);
	Candidate.Geometry = SourceGeometry.RemapToGraph(SourceGraph, *Candidate.Graph);
	SeedMeasuredNodeSizes(*Candidate.Graph, Candidate.Geometry);
	return true;
}

void RandomizeBlindLabels(TArray<FBenchmarkCandidate>& Candidates, const FGuid& GraphGuid)
{
	const uint32 Seed = GetTypeHash(GraphGuid) ^ static_cast<uint32>(FDateTime::UtcNow().GetTicks());
	FRandomStream Random(static_cast<int32>(Seed));
	for (int32 Index = Candidates.Num() - 1; Index > 0; --Index)
	{
		const int32 SwapIndex = Random.RandRange(0, Index);
		Candidates.Swap(Index, SwapIndex);
	}
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		Candidates[Index].BlindLabel = FString::Chr(TEXT('A') + Index);
	}
}

[[nodiscard]]
const TCHAR* BackendObjectSuffix(const EFormatterBackend Backend) noexcept
{
	switch (Backend)
	{
		case EFormatterBackend::GraphFormatter:
			return TEXT("GraphFormatter");
		case EFormatterBackend::BlueprintAutoLayout:
			return TEXT("BlueprintAutoLayout");
		case EFormatterBackend::GraphFormatterLibavoid:
			return TEXT("Libavoid");
		case EFormatterBackend::ElkLayered:
			return TEXT("ElkLayered");
	}
	return TEXT("Unknown");
}

constexpr const TCHAR* ReadabilitySafetyDiagnosticPrefix = TEXT("[readability safety]");

void EvaluateReadabilitySafety(FBenchmarkCandidate& Candidate, const FGraphQualityMetrics& OriginalMetrics)
{
	Candidate.Diagnostics.RemoveAll([](const FString& Diagnostic)
									{ return Diagnostic.StartsWith(ReadabilitySafetyDiagnosticPrefix); });
	Candidate.bReadabilitySafetyPassed = true;
	if (!Candidate.bSucceeded || !Candidate.bTopologyPreserved) { return; }

	TArray<FString> Regressions;
	const FGraphQualityMetrics& Metrics = Candidate.Metrics;
	if (Metrics.NodeOverlapCount > OriginalMetrics.NodeOverlapCount)
	{
		Regressions.Add(
			FString::Printf(TEXT("node overlaps %d -> %d"), OriginalMetrics.NodeOverlapCount, Metrics.NodeOverlapCount)
		);
	}
	if (Metrics.WireThroughNodeCount > OriginalMetrics.WireThroughNodeCount)
	{
		Regressions.Add(
			FString::Printf(TEXT("wire/node intersections %d -> %d"), OriginalMetrics.WireThroughNodeCount, Metrics.WireThroughNodeCount)
		);
	}
	if (Metrics.BackwardExecutionEdgeCount > OriginalMetrics.BackwardExecutionEdgeCount)
	{
		Regressions.Add(
			FString::Printf(
				TEXT("backward execution edges %d -> %d"), OriginalMetrics.BackwardExecutionEdgeCount, Metrics.BackwardExecutionEdgeCount
			)
		);
	}
	if (Metrics.ExecutionCrossingCount > OriginalMetrics.ExecutionCrossingCount)
	{
		Regressions.Add(
			FString::Printf(TEXT("execution crossings %d -> %d"), OriginalMetrics.ExecutionCrossingCount, Metrics.ExecutionCrossingCount)
		);
	}
	if (Metrics.InsufficientExecutionGapCount > OriginalMetrics.InsufficientExecutionGapCount)
	{
		Regressions.Add(
			FString::Printf(
				TEXT("sub-cell straight execution gaps %d -> %d"),
				OriginalMetrics.InsufficientExecutionGapCount,
				Metrics.InsufficientExecutionGapCount
			)
		);
	}
	if (Regressions.IsEmpty()) { return; }

	Candidate.bReadabilitySafetyPassed = false;
	Candidate.Diagnostics.Add(
		FString::Printf(TEXT("%s INVALID: %s."), ReadabilitySafetyDiagnosticPrefix, *FString::Join(Regressions, TEXT(", ")))
	);
}

[[nodiscard]]
TSharedPtr<FGraphFormatterBenchmarkRun> BuildRun(
	UBlueprint& SourceBlueprint,
	UEdGraph& SourceGraph,
	const K2::FGraphGeometrySnapshot& SourceGeometry,
	const bool bWriteManifest,
	FString& OutError
)
{
	TSet<UEdGraphNode*> SourceScope;
	if (!BuildScope(SourceGraph, SourceScope))
	{
		OutError = TEXT("The active graph is not a supported K2 graph with at least two semantic nodes.");
		return nullptr;
	}
	if (!SourceGeometry.IsReady())
	{
		OutError = TEXT("The source geometry snapshot is not ready.");
		return nullptr;
	}

	TSharedPtr<FGraphFormatterBenchmarkRun> Run = MakeShared<FGraphFormatterBenchmarkRun>();
	Run->RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-")
			   + FGuid::NewGuid().ToString(EGuidFormats::Short);
	Run->SourceAssetPath = SourceBlueprint.GetPathName();
	Run->SourceGraphName = SourceGraph.GetName();
	Run->ReportDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GraphFormatter"), TEXT("Bakeoff"), Run->RunId);
	const FGraphBenchmarkBaseline Baseline = CaptureBaseline(SourceGraph);
	const TArray<FString> SourceTopology = CaptureLogicalTopology(SourceGraph);
	const UFormatterSettings& Settings = *GetDefault<UFormatterSettings>();

	FBenchmarkCandidate Original;
	if (!InitializeCandidateCopy(Original, SourceBlueprint, SourceGraph, SourceGeometry, TEXT("Original"), OutError))
	{
		return nullptr;
	}
	Run->OriginalBlueprint = Original.Blueprint;
	Run->OriginalGraph = Original.Graph;
	Run->OriginalGeometry = MoveTemp(Original.Geometry);
	Run->OriginalMetrics =
		MeasureGraphQuality(*Run->OriginalGraph, Run->OriginalGeometry, Baseline, Settings.K2LayoutCellSize);

	const EFormatterBackend Backends[] = {
		EFormatterBackend::GraphFormatter,
		EFormatterBackend::BlueprintAutoLayout,
		EFormatterBackend::GraphFormatterLibavoid,
		EFormatterBackend::ElkLayered,
	};
	for (const EFormatterBackend Backend : Backends)
	{
		FBenchmarkCandidate& Candidate = Run->Candidates.AddDefaulted_GetRef();
		Candidate.Backend = Backend;
		Candidate.DisplayName = DescribeBackend(Backend);
		switch (Backend)
		{
			case EFormatterBackend::GraphFormatter:
				Candidate.Description = TEXT("Current GraphFormatter semantic placement and native reroute pass.");
				break;
			case EFormatterBackend::BlueprintAutoLayout:
				Candidate.Description = TEXT("Independent MIT layered Blueprint formatter and its own reroute pass.");
				break;
			case EFormatterBackend::GraphFormatterLibavoid:
				Candidate.Description = TEXT("Our semantic placement with external object-avoiding orthogonal routing.");
				break;
			case EFormatterBackend::ElkLayered:
				Candidate.Description =
					TEXT("Independent ELK Layered placement with fixed ports and orthogonal edge sections.");
				break;
		}
		if (!InitializeCandidateCopy(
				Candidate, SourceBlueprint, SourceGraph, SourceGeometry, BackendObjectSuffix(Backend), OutError
			))
		{
			return nullptr;
		}
		DescribeCandidateConfiguration(Candidate, Settings, Run->OriginalMetrics.SemanticNodeCount, SourceTopology.Num());
		const double StartTime = FPlatformTime::Seconds();
		if (Backend == EFormatterBackend::BlueprintAutoLayout) { RunBlueprintAutoLayoutCandidate(Candidate, Settings); }
		else if (Backend == EFormatterBackend::ElkLayered)
		{
			RunElkLayeredCandidate(Candidate, Settings, bWriteManifest ? Run->ReportDirectory : FString());
		}
		else
		{
			RunGraphFormatterCandidate(Candidate, Settings, Backend == EFormatterBackend::GraphFormatterLibavoid);
		}
		Candidate.DurationMilliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		Candidate.bTopologyPreserved = CaptureLogicalTopology(*Candidate.Graph) == SourceTopology;
		if (!Candidate.bTopologyPreserved)
		{
			Candidate.bSucceeded = false;
			Candidate.Diagnostics.Add(TEXT("Logical pin topology changed; this candidate is invalid."));
		}
		Candidate.Metrics = MeasureGraphQuality(*Candidate.Graph, Candidate.Geometry, Baseline, Settings.K2LayoutCellSize);
		EvaluateReadabilitySafety(Candidate, Run->OriginalMetrics);
	}
	RandomizeBlindLabels(Run->Candidates, SourceGraph.GraphGuid);
	if (bWriteManifest && !Run->SaveManifest(OutError)) { return nullptr; }
	return Run;
}

void OpenRunWindow(const TSharedPtr<FGraphFormatterBenchmarkRun>& Run)
{
	const TSharedRef<SWindow> Window =
		SNew(SWindow)
			.Title(FText::FromString(FString::Printf(TEXT("Graph Formatter Bakeoff — %s"), *Run->SourceGraphName)))
			.ClientSize(FVector2D(BenchmarkWindowWidth, BenchmarkWindowHeight))
			.SizingRule(ESizingRule::UserSized)
			.SupportsMaximize(true)
			.SupportsMinimize(true);
	Window->SetContent(SNew(SGraphFormatterBenchmark).Run(Run));
	FSlateApplication::Get().AddWindow(Window);
}
} // namespace BenchmarkPrivate

using namespace BenchmarkPrivate;

const TCHAR* DescribeBackend(const EFormatterBackend Backend) noexcept
{
	switch (Backend)
	{
		case EFormatterBackend::GraphFormatter:
			return TEXT("GraphFormatter semantic layout + native router");
		case EFormatterBackend::BlueprintAutoLayout:
			return TEXT("Blueprint Auto Layout 0.6.9 layered layout + router");
		case EFormatterBackend::GraphFormatterLibavoid:
			return TEXT("GraphFormatter semantic layout + Adaptagrams/libavoid router");
		case EFormatterBackend::ElkLayered:
			return TEXT("elkjs 0.12.0 ELK Layered + orthogonal router");
	}
	return TEXT("Unknown formatter");
}

bool FGraphFormatterBenchmark::ValidateGeometryForComparison(
	const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, FString& OutError
)
{
	int32 MeasurableNodeCount = 0;
	int32 MeasuredNodeCount = 0;
	int32 SlateSizedNodeCount = 0;
	int32 LinkedPinCount = 0;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>()) { continue; }
		++MeasurableNodeCount;
		if (const K2::FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(Node))
		{
			MeasuredNodeCount += NodeGeometry->HasBounds() ? 1 : 0;
			SlateSizedNodeCount += NodeGeometry->SizeSource == K2::EGraphNodeSizeSource::SlateGeometry ? 1 : 0;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			LinkedPinCount += Pin != nullptr && !Pin->LinkedTo.IsEmpty() ? 1 : 0;
		}
	}

	const bool bPinsRequired = LinkedPinCount > 0;
	const bool bVisiblePinsComplete = Geometry.RequestedVisiblePinCount == Geometry.CapturedPinCount;
	const bool bHasMeasuredPins =
		!bPinsRequired || (Geometry.RequestedVisiblePinCount > 0 && Geometry.CapturedPinCount > 0 && bVisiblePinsComplete);
	if (Geometry.IsReady() && MeasuredNodeCount == MeasurableNodeCount && SlateSizedNodeCount > 0 && bHasMeasuredPins)
	{
		OutError.Reset();
		return true;
	}

	OutError = FString::Printf(
		TEXT("Benchmark geometry is not ready: %d/%d graph nodes have bounds, %d use current Slate tick geometry, and %d/%d visible pin anchors were captured (%d linked pins in the graph). Wait for one editor frame and retry."),
		MeasuredNodeCount,
		MeasurableNodeCount,
		SlateSizedNodeCount,
		Geometry.CapturedPinCount,
		Geometry.RequestedVisiblePinCount,
		LinkedPinCount
	);
	return false;
}

void FGraphFormatterBenchmarkRun::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(OriginalBlueprint);
	for (FBenchmarkCandidate& Candidate : Candidates)
	{
		Collector.AddReferencedObject(Candidate.Blueprint);
	}
}

FString FGraphFormatterBenchmarkRun::GetReferencerName() const { return TEXT("FGraphFormatterBenchmarkRun"); }

bool FGraphFormatterBenchmarkRun::RefreshRenderedResults(
	const TConstArrayView<TSharedPtr<SGraphEditor>> GraphEditors, FString& OutError
)
{
	const int32 ExpectedPaneCount = Candidates.Num() + 1;
	if (OriginalGraph == nullptr || Candidates.IsEmpty() || GraphEditors.Num() != ExpectedPaneCount)
	{
		OutError = FString::Printf(
			TEXT("The benchmark needs one original and %d rendered candidate graph panes."), Candidates.Num()
		);
		return false;
	}

	TArray<K2::FGraphGeometrySnapshot> RenderedGeometry;
	RenderedGeometry.Reserve(GraphEditors.Num());
	for (int32 Index = 0; Index < GraphEditors.Num(); ++Index)
	{
		const TSharedPtr<SGraphEditor>& Editor = GraphEditors[Index];
		SGraphPanel* Panel = Editor.IsValid() ? Editor->GetGraphPanel() : nullptr;
		UEdGraph* Graph = Index == 0 ? OriginalGraph : Candidates[Index - 1].Graph;
		if (Panel == nullptr || Graph == nullptr)
		{
			OutError = FString::Printf(TEXT("Benchmark pane %d is unavailable."), Index + 1);
			return false;
		}
		K2::FGraphGeometrySnapshot Geometry = K2::FGraphGeometrySnapshot::Capture(*Panel);
		FString GeometryError;
		if (!FGraphFormatterBenchmark::ValidateGeometryForComparison(*Graph, Geometry, GeometryError))
		{
			OutError = FString::Printf(TEXT("Benchmark pane %d: %s"), Index + 1, *GeometryError);
			return false;
		}
		RenderedGeometry.Add(MoveTemp(Geometry));
	}

	OriginalGeometry = MoveTemp(RenderedGeometry[0]);
	const FGraphBenchmarkBaseline Baseline = CaptureBaseline(*OriginalGraph);
	const UFormatterSettings& Settings = *GetDefault<UFormatterSettings>();
	OriginalMetrics = MeasureGraphQuality(*OriginalGraph, OriginalGeometry, Baseline, Settings.K2LayoutCellSize);
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		FBenchmarkCandidate& Candidate = Candidates[Index];
		Candidate.Geometry = MoveTemp(RenderedGeometry[Index + 1]);
		Candidate.Metrics = MeasureGraphQuality(*Candidate.Graph, Candidate.Geometry, Baseline, Settings.K2LayoutCellSize);
		EvaluateReadabilitySafety(Candidate, OriginalMetrics);
	}
	OutError.Reset();
	return true;
}

bool FGraphFormatterBenchmarkRun::SaveManifest(FString& OutError) const
{
	IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("graph-formatter-bakeoff-v4"));
	Json->SetStringField(TEXT("metric_model"), TEXT("blueprint-readability-v3"));
	Json->SetStringField(TEXT("run_id"), RunId);
	Json->SetStringField(TEXT("source_asset"), SourceAssetPath);
	Json->SetStringField(TEXT("source_graph"), SourceGraphName);
	Json->SetBoolField(TEXT("source_modified"), false);
	Json->SetArrayField(
		TEXT("metric_notes"),
		{
			MakeShared<FJsonValueString>(TEXT("wires_through_nodes counts wire/node intersection pairs, not unique wires")),
			MakeShared<FJsonValueString>(TEXT("added reroute nodes are penalized relative to the original graph")),
			MakeShared<FJsonValueString>(TEXT("the composite penalty is diagnostic; blinded human judgment is primary")),
		}
	);
	Json->SetObjectField(TEXT("original_metrics"), MetricsToJson(OriginalMetrics));
	Json->SetObjectField(TEXT("source_geometry"), GeometryToJson(OriginalGeometry));
	Json->SetObjectField(TEXT("original_screenshot"), ScreenshotToJson(OriginalScreenshotFilename, OriginalScreenshotSize));
	TArray<TSharedPtr<FJsonValue>> RunDiagnosticValues;
	for (const FString& Diagnostic : Diagnostics)
	{
		RunDiagnosticValues.Add(MakeShared<FJsonValueString>(Diagnostic));
	}
	Json->SetArrayField(TEXT("diagnostics"), RunDiagnosticValues);
	TArray<TSharedPtr<FJsonValue>> CandidateValues;
	for (const FBenchmarkCandidate& Candidate : Candidates)
	{
		TSharedPtr<FJsonObject> CandidateJson = MakeShared<FJsonObject>();
		CandidateJson->SetStringField(TEXT("blind_label"), Candidate.BlindLabel);
		CandidateJson->SetStringField(TEXT("backend_id"), BackendStableId(Candidate.Backend));
		CandidateJson->SetStringField(TEXT("backend"), DescribeBackend(Candidate.Backend));
		CandidateJson->SetBoolField(TEXT("succeeded"), Candidate.bSucceeded);
		CandidateJson->SetBoolField(TEXT("topology_preserved"), Candidate.bTopologyPreserved);
		CandidateJson->SetBoolField(TEXT("readability_safety_passed"), Candidate.bReadabilitySafetyPassed);
		CandidateJson->SetBoolField(TEXT("valid"), Candidate.IsValid());
		CandidateJson->SetNumberField(TEXT("duration_ms"), Candidate.DurationMilliseconds);
		CandidateJson->SetObjectField(TEXT("metrics"), MetricsToJson(Candidate.Metrics));
		CandidateJson->SetObjectField(TEXT("geometry"), GeometryToJson(Candidate.Geometry));
		CandidateJson->SetObjectField(TEXT("configuration"), StringMapToJson(Candidate.Configuration));
		CandidateJson->SetObjectField(TEXT("numeric_configuration"), NumberMapToJson(Candidate.NumericConfiguration));
		CandidateJson->SetObjectField(TEXT("telemetry"), NumberMapToJson(Candidate.Telemetry));
		CandidateJson->SetObjectField(
			TEXT("screenshot"), ScreenshotToJson(Candidate.ScreenshotFilename, Candidate.ScreenshotSize)
		);
		TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
		for (const FString& Diagnostic : Candidate.Diagnostics)
		{
			DiagnosticValues.Add(MakeShared<FJsonValueString>(Diagnostic));
		}
		CandidateJson->SetArrayField(TEXT("diagnostics"), DiagnosticValues);
		CandidateValues.Add(MakeShared<FJsonValueObject>(CandidateJson));
	}
	Json->SetArrayField(TEXT("candidates"), CandidateValues);
	return SaveJson(FPaths::Combine(ReportDirectory, TEXT("run.json")), Json, OutError);
}

bool FGraphFormatterBenchmarkRun::SaveBallot(
	const TMap<FString, TSet<FString>>& Choices, const bool bMappingRevealed, FString& OutError
) const
{
	IFileManager::Get().MakeDirectory(*ReportDirectory, true);
	const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("graph-formatter-ballot-v2"));
	Json->SetStringField(TEXT("run_id"), RunId);
	Json->SetStringField(TEXT("source_asset"), SourceAssetPath);
	Json->SetStringField(TEXT("source_graph"), SourceGraphName);
	Json->SetBoolField(TEXT("mapping_revealed_before_save"), bMappingRevealed);
	Json->SetStringField(TEXT("saved_utc"), FDateTime::UtcNow().ToIso8601());
	const TSharedPtr<FJsonObject> ChoiceJson = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> ResolvedChoiceJson = MakeShared<FJsonObject>();
	TArray<FString> SortedCriteria;
	Choices.GenerateKeyArray(SortedCriteria);
	SortedCriteria.Sort();
	for (const FString& Criterion : SortedCriteria)
	{
		const TSet<FString>* Selected = Choices.Find(Criterion);
		if (Selected == nullptr) { continue; }
		TArray<FString> SortedChoices = Selected->Array();
		SortedChoices.Sort();
		TArray<TSharedPtr<FJsonValue>> ChoiceValues;
		TArray<TSharedPtr<FJsonValue>> ResolvedChoiceValues;
		for (const FString& Choice : SortedChoices)
		{
			ChoiceValues.Add(MakeShared<FJsonValueString>(Choice));
			const FBenchmarkCandidate* Candidate = Candidates.FindByPredicate([&Choice](const FBenchmarkCandidate& Value)
																			  { return Value.BlindLabel == Choice; });
			ResolvedChoiceValues.Add(
				MakeShared<FJsonValueString>(Candidate != nullptr ? BackendStableId(Candidate->Backend) : TEXT("unknown"))
			);
		}
		ChoiceJson->SetArrayField(Criterion, ChoiceValues);
		ResolvedChoiceJson->SetArrayField(Criterion, ResolvedChoiceValues);
	}
	Json->SetObjectField(TEXT("choices"), ChoiceJson);
	Json->SetObjectField(TEXT("resolved_choices"), ResolvedChoiceJson);
	return SaveJson(FPaths::Combine(ReportDirectory, TEXT("ballot.json")), Json, OutError);
}

TSharedPtr<FGraphFormatterBenchmarkRun> FGraphFormatterBenchmark::CreateRun(
	SGraphEditor& SourceEditor, UObject& ContextObject, FString& OutError
)
{
	UEdGraph* SourceGraph = SourceEditor.GetCurrentGraph();
	UBlueprint* SourceBlueprint = Cast<UBlueprint>(&ContextObject);
	if (SourceBlueprint == nullptr && SourceGraph != nullptr)
	{
		SourceBlueprint = SourceGraph->GetTypedOuter<UBlueprint>();
	}
	if (SourceGraph == nullptr || SourceBlueprint == nullptr)
	{
		OutError = TEXT("The active editor is not showing a Blueprint-owned graph.");
		return nullptr;
	}
	TSet<UEdGraphNode*> SourceScope;
	if (!BuildScope(*SourceGraph, SourceScope))
	{
		OutError = TEXT("The active graph is not a supported K2 graph with at least two semantic nodes.");
		return nullptr;
	}

	SGraphPanel* SourcePanel = SourceEditor.GetGraphPanel();
	if (SourcePanel == nullptr)
	{
		OutError = TEXT("The active graph panel is unavailable.");
		return nullptr;
	}
	const K2::FGraphGeometrySnapshot SourceGeometry = K2::FGraphGeometrySnapshot::Capture(*SourcePanel);
	if (!ValidateGeometryForComparison(*SourceGraph, SourceGeometry, OutError)) { return nullptr; }

	return BuildRun(*SourceBlueprint, *SourceGraph, SourceGeometry, true, OutError);
}

TSharedPtr<FGraphFormatterBenchmarkRun> FGraphFormatterBenchmark::CreateHeadlessRun(
	UBlueprint& SourceBlueprint, UEdGraph& SourceGraph, const K2::FGraphGeometrySnapshot& SourceGeometry, FString& OutError
)
{ return BuildRun(SourceBlueprint, SourceGraph, SourceGeometry, false, OutError); }

bool FGraphFormatterBenchmark::Open(SGraphEditor& SourceEditor, UObject& ContextObject, FString& OutError)
{
	const TSharedPtr<FGraphFormatterBenchmarkRun> Run = CreateRun(SourceEditor, ContextObject, OutError);
	if (Run.IsValid())
	{
		OpenRunWindow(Run);
		return true;
	}
	if (!OutError.StartsWith(TEXT("Benchmark geometry")) || !FSlateApplication::IsInitialized()) { return false; }

	SGraphPanel* SourcePanel = SourceEditor.GetGraphPanel();
	if (SourcePanel == nullptr) { return false; }
	SourcePanel->Update();
	const TWeakPtr<SGraphEditor> WeakEditor = StaticCastSharedRef<SGraphEditor>(SourceEditor.AsShared());
	const TWeakObjectPtr<UObject> WeakContext(&ContextObject);
	const TSharedRef<FDelegateHandle> RetryHandle = MakeShared<FDelegateHandle>();
	const TSharedRef<int32> RetryAttempts = MakeShared<int32>(0);
	*RetryHandle = FSlateApplication::Get().OnPostTick().AddLambda(
		[WeakEditor, WeakContext, RetryHandle, RetryAttempts](float)
		{
			const TSharedPtr<SGraphEditor> Editor = WeakEditor.Pin();
			UObject* Context = WeakContext.Get();
			if (!Editor.IsValid() || Context == nullptr)
			{
				FSlateApplication::Get().OnPostTick().Remove(*RetryHandle);
				return;
			}
			++*RetryAttempts;
			FString RetryError;
			const TSharedPtr<FGraphFormatterBenchmarkRun> RetryRun = CreateRun(*Editor, *Context, RetryError);
			if (RetryRun.IsValid())
			{
				FSlateApplication::Get().OnPostTick().Remove(*RetryHandle);
				OpenRunWindow(RetryRun);
				return;
			}
			if (RetryError.StartsWith(TEXT("Benchmark geometry")) && *RetryAttempts < 2)
			{
				if (SGraphPanel* Panel = Editor->GetGraphPanel()) { Panel->Update(); }
				return;
			}
			FSlateApplication::Get().OnPostTick().Remove(*RetryHandle);
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(FString::Printf(TEXT("Graph Formatter comparison could not start.\n\n%s"), *RetryError))
			);
		}
	);
	OutError.Reset();
	return true;
}
} // namespace GraphFormatter::Benchmark
