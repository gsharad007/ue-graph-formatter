/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/K2GraphFormatter.h"

#include "Algo/Sort.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "FormatterSettings.h"
#include "GraphEditor.h"
#include "K2/GraphGeometrySnapshot.h"
#include "K2/K2LayoutCore.h"
#include "K2/K2RerouteRouter.h"
#include "K2Node.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "SNodePanel.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GraphFormatterK2"

namespace GraphFormatter::K2
{
namespace
{
constexpr double MinimumNodeExtent = 1.0;
constexpr double FallbackNodeWidth = 160.0;
constexpr double FallbackNodeHeight = 80.0;
constexpr double FallbackPinTop = 32.0;
constexpr double FallbackPinPitch = 24.0;
constexpr int32 MaximumObstacleResolutionPasses = 4096;

struct FAdapterPinRecord
{
	FString Key;
	FVector2D Offset = FVector2D::ZeroVector;
	int32 SemanticOrder = 0;
	int32 KindOrder = 0;
	bool bExecution = false;
	bool bPreferredExecutionPort = false;
};

struct FAdapterNodeRecord
{
	UEdGraphNode* Node = nullptr;
	FString Key;
	FVector2D OriginalPosition = FVector2D::ZeroVector;
	FVector2D Size = FVector2D(FallbackNodeWidth, FallbackNodeHeight);
};

struct FAdapterEdgeRecord
{
	UEdGraphPin* OutputPin = nullptr;
	UEdGraphPin* InputPin = nullptr;
	FString Key;
	TArray<UEdGraphNode*> ExistingGeneratedKnots;
	TArray<FVector2D> ExistingRouteWaypoints;
	bool bExecution = false;
	bool bExistingGeneratedRoute = false;
	int32 BranchOrder = INDEX_NONE;
	bool bPreferredAlignment = false;
};

struct FValidatedGeneratedRoute
{
	UEdGraphPin* OutputPin = nullptr;
	UEdGraphPin* InputPin = nullptr;
	TArray<UEdGraphNode*> Knots;
	TArray<FVector2D> Waypoints;
};

struct FPlannedAdapterNode
{
	UEdGraphNode* Node = nullptr;
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D Size = FVector2D(FallbackNodeWidth, FallbackNodeHeight);
	int32 ComponentIndex = INDEX_NONE;
	int32 ExecutionRank = INDEX_NONE;
};

struct FNodePositionChange
{
	UEdGraphNode* Node = nullptr;
	FVector2D Position = FVector2D::ZeroVector;
};

struct FCommentRecord
{
	UEdGraphNode_Comment* Comment = nullptr;
	FString Key;
	FBox2D OriginalBounds = FBox2D(EForceInit::ForceInit);
	FBox2D PlannedBounds = FBox2D(EForceInit::ForceInit);
	TArray<UEdGraphNode*> Members;
	uint8 VisitState = 0;
};

struct FCommentBoundsChange
{
	UEdGraphNode_Comment* Comment = nullptr;
	FBox2D Bounds = FBox2D(EForceInit::ForceInit);
};

[[nodiscard]]
FString GuidKey(const TCHAR* Prefix, const FGuid& Guid)
{ return FString::Printf(TEXT("%s%s"), Prefix, *Guid.ToString(EGuidFormats::Digits)); }

[[nodiscard]]
int32 FindGraphNodeIndex(const UEdGraph& Graph, const UEdGraphNode& Node)
{
	for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
	{
		if (Graph.Nodes[Index].Get() == &Node) { return Index; }
	}
	return INDEX_NONE;
}

[[nodiscard]]
int32 FindNodePinIndex(const UEdGraphNode& Node, const UEdGraphPin& Pin)
{
	for (int32 Index = 0; Index < Node.Pins.Num(); ++Index)
	{
		if (Node.Pins[Index] == &Pin) { return Index; }
	}
	return INDEX_NONE;
}

[[nodiscard]]
FString MakeStableNodeKey(const UEdGraph& Graph, const UEdGraphNode& Node)
{
	if (Node.NodeGuid.IsValid()) { return GuidKey(TEXT("N:"), Node.NodeGuid); }

	return FString::Printf(
		TEXT("N:fallback:%s:%08d:%s"), *Graph.GetPathName(), FindGraphNodeIndex(Graph, Node), *Node.GetName()
	);
}

[[nodiscard]]
FString MakeStablePinKey(const UEdGraphPin& Pin, const int32 PinOrdinal)
{
#if WITH_EDITORONLY_DATA
	if (Pin.PersistentGuid.IsValid()) { return GuidKey(TEXT("P:persistent:"), Pin.PersistentGuid); }
#endif
	if (Pin.PinId.IsValid()) { return GuidKey(TEXT("P:"), Pin.PinId); }

	return FString::Printf(
		TEXT("P:fallback:%d:%08d:%s"), static_cast<int32>(Pin.Direction.GetValue()), PinOrdinal, *Pin.PinName.ToString()
	);
}

[[nodiscard]]
bool IsExecutionPin(const UEdGraphPin& Pin)
{ return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; }

[[nodiscard]]
FVector2D RectTopLeft(const FSlateRect& Rect)
{
	const FVector2f Value = Rect.GetTopLeft2f();
	return FVector2D(static_cast<double>(Value.X), static_cast<double>(Value.Y));
}

[[nodiscard]]
FVector2D RectBottomRight(const FSlateRect& Rect)
{
	const FVector2f Value = Rect.GetBottomRight2f();
	return FVector2D(static_cast<double>(Value.X), static_cast<double>(Value.Y));
}

[[nodiscard]]
FBox2D RectToBox(const FSlateRect& Rect)
{ return FBox2D(RectTopLeft(Rect), RectBottomRight(Rect)); }

[[nodiscard]]
FVector2D ResolveNodeSize(const UEdGraphNode& Node, const FGraphGeometrySnapshot& Geometry)
{
	if (const FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(&Node))
	{
		if (NodeGeometry->Bounds.IsSet())
		{
			const FVector2D Size = RectBottomRight(NodeGeometry->Bounds.GetValue())
								 - RectTopLeft(NodeGeometry->Bounds.GetValue());
			if (FMath::IsFinite(Size.X) && FMath::IsFinite(Size.Y) && Size.X >= MinimumNodeExtent
				&& Size.Y >= MinimumNodeExtent)
			{
				return Size;
			}
		}
	}

	const double PersistedWidth = static_cast<double>(Node.NodeWidth);
	const double PersistedHeight = static_cast<double>(Node.NodeHeight);
	return FVector2D(
		PersistedWidth >= MinimumNodeExtent ? PersistedWidth : FallbackNodeWidth,
		PersistedHeight >= MinimumNodeExtent ? PersistedHeight : FallbackNodeHeight
	);
}

[[nodiscard]]
FVector2D ResolvePinOffset(
	const UEdGraphPin& Pin, const FGraphGeometrySnapshot& Geometry, const FVector2D NodeSize, const int32 DirectionOrdinal
)
{
	if (const FGraphPinGeometrySnapshot* PinGeometry = Geometry.FindPin(&Pin))
	{
		if (FMath::IsFinite(PinGeometry->NodeOffset.X) && FMath::IsFinite(PinGeometry->NodeOffset.Y))
		{
			return PinGeometry->NodeOffset;
		}
	}

	const double X = Pin.Direction == EGPD_Input ? 0.0 : NodeSize.X;
	const double UnclampedY = FallbackPinTop + static_cast<double>(DirectionOrdinal) * FallbackPinPitch;
	const double MaximumY = FMath::Max(FallbackPinTop, NodeSize.Y - FallbackPinPitch * 0.5);
	return FVector2D(X, FMath::Clamp(UnclampedY, FallbackPinTop, MaximumY));
}

[[nodiscard]]
FBox2D MakeNodeBox(const FVector2D Position, const FVector2D Size)
{ return FBox2D(Position, Position + Size); }

[[nodiscard]]
FBox2D ResolveCurrentNodeBounds(const UEdGraphNode& Node, const FGraphGeometrySnapshot& Geometry)
{
	if (const FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(&Node))
	{
		if (NodeGeometry->Bounds.IsSet()) { return RectToBox(NodeGeometry->Bounds.GetValue()); }
	}

	return MakeNodeBox(
		FVector2D(static_cast<double>(Node.NodePosX), static_cast<double>(Node.NodePosY)), ResolveNodeSize(Node, Geometry)
	);
}

[[nodiscard]]
bool StrictlyContains(const FBox2D& Outer, const FBox2D& Inner)
{
	if (!Outer.bIsValid || !Inner.bIsValid) { return false; }
	const bool bContains = Inner.Min.X >= Outer.Min.X && Inner.Max.X <= Outer.Max.X && Inner.Min.Y >= Outer.Min.Y
						&& Inner.Max.Y <= Outer.Max.Y;
	const FVector2D OuterSize = Outer.GetSize();
	const FVector2D InnerSize = Inner.GetSize();
	return bContains && (OuterSize.X > InnerSize.X || OuterSize.Y > InnerSize.Y);
}

[[nodiscard]]
bool IsOriginalCommentMember(
	const UEdGraph& Graph, const FGraphGeometrySnapshot& Geometry, const UEdGraphNode_Comment& Comment, const UEdGraphNode& Candidate
)
{
	if (&Comment == &Candidate || Candidate.GetGraph() != &Graph) { return false; }

	const FBox2D CommentBounds = ResolveCurrentNodeBounds(Comment, Geometry);
	const FBox2D CandidateBounds = ResolveCurrentNodeBounds(Candidate, Geometry);
	if (Candidate.IsA<UEdGraphNode_Comment>())
	{
		// Comment membership must form a strict spatial hierarchy. This prevents overlapping
		// comments (or stale engine membership) from manufacturing a recursive cycle.
		return StrictlyContains(CommentBounds, CandidateBounds);
	}

	for (UObject* MemberObject : Comment.GetNodesUnderComment())
	{
		if (MemberObject == &Candidate) { return true; }
	}
	return StrictlyContains(CommentBounds, CandidateBounds);
}

[[nodiscard]]
bool ArePinsReciprocallyLinked(const UEdGraphPin* First, const UEdGraphPin* Second)
{
	return First != nullptr && Second != nullptr && First->LinkedTo.Contains(Second) && Second->LinkedTo.Contains(First);
}

[[nodiscard]]
bool IsGeneratedRouteTopologyValid(const FAdapterEdgeRecord& Edge, TConstArrayView<UEdGraphNode*> OrderedKnots)
{
	if (Edge.OutputPin == nullptr || Edge.InputPin == nullptr || OrderedKnots.IsEmpty()) { return false; }

	const UEdGraphPin* PreviousOutput = Edge.OutputPin;
	for (int32 KnotIndex = 0; KnotIndex < OrderedKnots.Num(); ++KnotIndex)
	{
		UEdGraphNode* Node = OrderedKnots[KnotIndex];
		const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
		const UEdGraphPin* KnotInput = Knot != nullptr ? Knot->GetInputPin() : nullptr;
		const UEdGraphPin* KnotOutput = Knot != nullptr ? Knot->GetOutputPin() : nullptr;
		if (Knot == nullptr || KnotInput == nullptr || KnotOutput == nullptr || PreviousOutput == nullptr
			|| (KnotIndex > 0 && PreviousOutput->LinkedTo.Num() != 1) || KnotInput->LinkedTo.Num() != 1
			|| !ArePinsReciprocallyLinked(PreviousOutput, KnotInput))
		{
			return false;
		}
		PreviousOutput = KnotOutput;
	}
	return PreviousOutput != nullptr && PreviousOutput->LinkedTo.Num() == 1 && Edge.InputPin->LinkedTo.Num() == 1
		&& ArePinsReciprocallyLinked(PreviousOutput, Edge.InputPin);
}

[[nodiscard]]
FBox2D InflateBox(const FBox2D& Bounds, const double Amount)
{ return Bounds.ExpandBy(FMath::Max(0.0, Amount)); }

[[nodiscard]]
double SnapUp(const double Value, const double GridSize)
{
	if (GridSize <= UE_DOUBLE_SMALL_NUMBER) { return Value; }
	return FMath::CeilToDouble(Value / GridSize) * GridSize;
}

[[nodiscard]]
bool SelectionMatches(const SGraphEditor& GraphEditor, const TSet<UEdGraphNode*>& ExpectedSelection)
{
	const TSet<UObject*>& CurrentSelection = GraphEditor.GetSelectedNodes();
	if (CurrentSelection.Num() != ExpectedSelection.Num()) { return false; }
	for (UEdGraphNode* ExpectedNode : ExpectedSelection)
	{
		if (!CurrentSelection.Contains(ExpectedNode)) { return false; }
	}
	return true;
}

void RestoreSelection(SGraphEditor& GraphEditor, const TSet<UEdGraphNode*>& ExpectedSelection)
{
	if (SelectionMatches(GraphEditor, ExpectedSelection)) { return; }

	TArray<UEdGraphNode*> OrderedSelection = ExpectedSelection.Array();
	Algo::Sort(
		OrderedSelection,
		[](const UEdGraphNode* Left, const UEdGraphNode* Right)
		{
			if (Left == nullptr || Right == nullptr) { return Left != nullptr; }
			return Left->NodeGuid.ToString(EGuidFormats::Digits) < Right->NodeGuid.ToString(EGuidFormats::Digits);
		}
	);

	GraphEditor.ClearSelectionSet();
	for (UEdGraphNode* Node : OrderedSelection)
	{
		if (IsValid(Node)) { GraphEditor.SetNodeSelection(Node, true); }
	}
}
} // namespace

bool FK2GraphFormatter::CanFormat(const UEdGraph& Graph, const TSet<UEdGraphNode*>& Scope)
{
	if (Cast<UEdGraphSchema_K2>(Graph.GetSchema()) == nullptr) { return false; }

	int32 RealNodeCount = 0;
	for (UEdGraphNode* Node : Scope)
	{
		if (Node == nullptr || Node->GetGraph() != &Graph) { return false; }
		if (Node->IsA<UEdGraphNode_Comment>()) { continue; }
		if (!Node->IsA<UK2Node>()) { return false; }
		++RealNodeCount;
	}

	return RealNodeCount > 0;
}

FK2FormatResult FK2GraphFormatter::Format(
	SGraphEditor& GraphEditor,
	UEdGraph& Graph,
	const FGraphGeometrySnapshot& Geometry,
	const TSet<UEdGraphNode*>& Scope,
	const bool bRouteWires,
	const UFormatterSettings& Settings
)
{
	FK2FormatResult Result;
	if (!CanFormat(Graph, Scope))
	{
		Result.Status = EK2FormatStatus::UnsupportedGraph;
		Result.Message = TEXT("The semantic formatter supports K2 graphs whose selected non-comment nodes are UK2Nodes.");
		return Result;
	}

	if (!Geometry.IsReady())
	{
		Result.Status = EK2FormatStatus::InvalidGeometry;
		Result.Message = Geometry.ShouldRetry()
						   ? TEXT("Graph geometry is not ready; retry after the graph panel receives a Slate tick.")
						   : TEXT("Graph geometry could not be captured safely.");
		for (const FGraphGeometryDiagnostic& Diagnostic : Geometry.Diagnostics)
		{
			Result.Diagnostics.Add(Diagnostic.ToString());
		}
		return Result;
	}

	TSet<UEdGraphNode*> OriginalSelection;
	for (UObject* SelectedObject : GraphEditor.GetSelectedNodes())
	{
		if (UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject)) { OriginalSelection.Add(SelectedNode); }
	}

	// Treat generated reroutes as presentation artifacts only after validating the complete maximal
	// chain, its ordinal metadata, exact one-to-one topology, real endpoint directions, and stable
	// logical-edge identity. A stale/copied/malformed marker remains an ordinary K2 knot.
	TSet<FString> GeneratedRouteKeySet;
	for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
	{
		UEdGraphNode* GraphNode = GraphNodePointer.Get();
		if (!FK2RerouteRouter::IsGeneratedRerouteNode(GraphNode)) { continue; }
		FString LogicalEdgeKey;
		if (FK2RerouteRouter::TryGetGeneratedRouteKey(GraphNode, LogicalEdgeKey))
		{
			GeneratedRouteKeySet.Add(MoveTemp(LogicalEdgeKey));
		}
		else
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Ignored malformed generated reroute marker on '%s'."), *GraphNode->GetName())
			);
		}
	}
	TArray<FString> GeneratedRouteKeys = GeneratedRouteKeySet.Array();
	GeneratedRouteKeys.Sort();
	TMap<FString, FValidatedGeneratedRoute> ValidatedGeneratedRoutes;
	TSet<UEdGraphNode*> ValidatedGeneratedKnots;
	for (const FString& LogicalEdgeKey : GeneratedRouteKeys)
	{
		FValidatedGeneratedRoute CandidateRoute;
		if (!FK2RerouteRouter::FindGeneratedRoute(Graph, LogicalEdgeKey, CandidateRoute.Knots, CandidateRoute.Waypoints))
		{
			Result.Diagnostics.Add(FString::Printf(TEXT("Ignored malformed generated reroute chain '%s'."), *LogicalEdgeKey));
			continue;
		}

		const UK2Node_Knot* FirstKnot = Cast<UK2Node_Knot>(CandidateRoute.Knots[0]);
		const UK2Node_Knot* LastKnot = Cast<UK2Node_Knot>(CandidateRoute.Knots.Last());
		if (FirstKnot == nullptr || LastKnot == nullptr)
		{
			Result.Diagnostics.Add(FString::Printf(TEXT("Ignored non-knot generated reroute chain '%s'."), *LogicalEdgeKey));
			continue;
		}
		const UEdGraphPin* FirstInput = FirstKnot->GetInputPin();
		const UEdGraphPin* LastOutput = LastKnot->GetOutputPin();
		CandidateRoute.OutputPin = FirstInput != nullptr && FirstInput->LinkedTo.Num() == 1 ? FirstInput->LinkedTo[0]
																							: nullptr;
		CandidateRoute.InputPin = LastOutput != nullptr && LastOutput->LinkedTo.Num() == 1 ? LastOutput->LinkedTo[0]
																						   : nullptr;
		UEdGraphNode* OutputNode =
			CandidateRoute.OutputPin != nullptr ? CandidateRoute.OutputPin->GetOwningNodeUnchecked() : nullptr;
		UEdGraphNode* InputNode = CandidateRoute.InputPin != nullptr ? CandidateRoute.InputPin->GetOwningNodeUnchecked()
																	 : nullptr;
		FAdapterEdgeRecord CandidateEdge;
		CandidateEdge.OutputPin = CandidateRoute.OutputPin;
		CandidateEdge.InputPin = CandidateRoute.InputPin;
		const bool bValidEndpoints = OutputNode != nullptr && InputNode != nullptr
								  && CandidateRoute.OutputPin->Direction == EGPD_Output
								  && CandidateRoute.InputPin->Direction == EGPD_Input
								  && !FK2RerouteRouter::IsGeneratedRerouteNode(OutputNode)
								  && !FK2RerouteRouter::IsGeneratedRerouteNode(InputNode);
		if (!bValidEndpoints || !IsGeneratedRouteTopologyValid(CandidateEdge, CandidateRoute.Knots))
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Ignored topologically invalid generated reroute chain '%s'."), *LogicalEdgeKey)
			);
			continue;
		}

		const int32 OutputPinIndex = FindNodePinIndex(*OutputNode, *CandidateRoute.OutputPin);
		const int32 InputPinIndex = FindNodePinIndex(*InputNode, *CandidateRoute.InputPin);
		const bool bExecution = IsExecutionPin(*CandidateRoute.OutputPin);
		const bool bMatchingKinds = bExecution == IsExecutionPin(*CandidateRoute.InputPin);
		if (OutputPinIndex == INDEX_NONE || InputPinIndex == INDEX_NONE || !bMatchingKinds)
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Ignored generated reroute chain with invalid endpoints '%s'."), *LogicalEdgeKey)
			);
			continue;
		}
		const FString ExpectedKey = FString::Printf(
			TEXT("%s:%s/%s->%s/%s"),
			bExecution ? TEXT("E") : TEXT("D"),
			*MakeStableNodeKey(Graph, *OutputNode),
			*MakeStablePinKey(*CandidateRoute.OutputPin, OutputPinIndex),
			*MakeStableNodeKey(Graph, *InputNode),
			*MakeStablePinKey(*CandidateRoute.InputPin, InputPinIndex)
		);
		if (ExpectedKey != LogicalEdgeKey)
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Ignored generated reroute chain with stale identity '%s'."), *LogicalEdgeKey)
			);
			continue;
		}

		for (UEdGraphNode* Knot : CandidateRoute.Knots)
		{
			ValidatedGeneratedKnots.Add(Knot);
		}
		ValidatedGeneratedRoutes.Add(LogicalEdgeKey, MoveTemp(CandidateRoute));
	}

	TArray<FAdapterNodeRecord> NodeRecords;
	NodeRecords.Reserve(Scope.Num());
	for (UEdGraphNode* Node : Scope)
	{
		if (Node != nullptr && !Node->IsA<UEdGraphNode_Comment>() && !ValidatedGeneratedKnots.Contains(Node))
		{
			FAdapterNodeRecord& Record = NodeRecords.AddDefaulted_GetRef();
			Record.Node = Node;
			Record.Key = MakeStableNodeKey(Graph, *Node);
			Record.OriginalPosition = FVector2D(static_cast<double>(Node->NodePosX), static_cast<double>(Node->NodePosY));
			Record.Size = ResolveNodeSize(*Node, Geometry);
		}
	}
	NodeRecords.Sort([](const FAdapterNodeRecord& Left, const FAdapterNodeRecord& Right) { return Left.Key < Right.Key; });
	if (NodeRecords.IsEmpty())
	{
		Result.Status = EK2FormatStatus::NoChanges;
		Result.Message = TEXT("The selection contains no semantic K2 nodes to format.");
		return Result;
	}

	K2Layout::FGraphSnapshot LayoutSnapshot;
	LayoutSnapshot.Nodes.Reserve(NodeRecords.Num());
	TMap<const UEdGraphNode*, int32> NodeRecordIndices;
	TMap<const UEdGraphPin*, FAdapterPinRecord> PinRecords;
	TMap<FString, UEdGraphNode*> NodesByKey;
	for (int32 NodeIndex = 0; NodeIndex < NodeRecords.Num(); ++NodeIndex)
	{
		const FAdapterNodeRecord& AdapterNode = NodeRecords[NodeIndex];
		NodeRecordIndices.Add(AdapterNode.Node, NodeIndex);
		NodesByKey.Add(AdapterNode.Key, AdapterNode.Node);

		K2Layout::FNodeSnapshot& LayoutNode = LayoutSnapshot.Nodes.AddDefaulted_GetRef();
		LayoutNode.Key = K2Layout::FNodeKey(AdapterNode.Key);
		LayoutNode.Size = AdapterNode.Size;
		const UK2Node* K2Node = CastChecked<UK2Node>(AdapterNode.Node);
		bool bHasExecutionPin = false;
		for (const UEdGraphPin* Pin : AdapterNode.Node->Pins)
		{
			bHasExecutionPin |= Pin != nullptr && IsExecutionPin(*Pin);
		}
		// UK2Node_Knot reports itself pure even after wildcard propagation turns it into an exec reroute.
		// Treating that case as a pure provider makes the layout core reject a graph it routed itself.
		LayoutNode.bIsPure = K2Node->IsNodePure() && !bHasExecutionPin;
		LayoutNode.Ports.Reserve(AdapterNode.Node->Pins.Num());

		int32 InputOrdinal = 0;
		int32 OutputOrdinal = 0;
		int32 ExecutionInputOrdinal = 0;
		int32 ExecutionOutputOrdinal = 0;
		for (int32 PinOrdinal = 0; PinOrdinal < AdapterNode.Node->Pins.Num(); ++PinOrdinal)
		{
			UEdGraphPin* Pin = AdapterNode.Node->Pins[PinOrdinal];
			if (Pin == nullptr || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output)) { continue; }

			const bool bInput = Pin->Direction == EGPD_Input;
			const bool bExecution = IsExecutionPin(*Pin);
			const int32 DirectionOrdinal = bInput ? InputOrdinal++ : OutputOrdinal++;
			int32 KindOrdinal = DirectionOrdinal;
			if (bExecution) { KindOrdinal = bInput ? ExecutionInputOrdinal++ : ExecutionOutputOrdinal++; }

			FAdapterPinRecord PinRecord;
			PinRecord.Key = MakeStablePinKey(*Pin, PinOrdinal);
			PinRecord.Offset = ResolvePinOffset(*Pin, Geometry, AdapterNode.Size, DirectionOrdinal);
			PinRecord.SemanticOrder = DirectionOrdinal;
			PinRecord.KindOrder = KindOrdinal;
			PinRecord.bExecution = bExecution;
			PinRecord.bPreferredExecutionPort = bExecution && KindOrdinal == 0;
			PinRecords.Add(Pin, PinRecord);

			K2Layout::FPortSnapshot& LayoutPort = LayoutNode.Ports.AddDefaulted_GetRef();
			LayoutPort.Key = K2Layout::FPortKey(PinRecord.Key);
			LayoutPort.Direction = bInput ? K2Layout::EPortDirection::Input : K2Layout::EPortDirection::Output;
			LayoutPort.Kind = bExecution ? K2Layout::EPortKind::Execution : K2Layout::EPortKind::Data;
			LayoutPort.Offset = PinRecord.Offset;
			LayoutPort.SemanticOrder = PinRecord.SemanticOrder;
			LayoutPort.bPreferredExecutionPort = PinRecord.bPreferredExecutionPort;
		}
	}

	struct FPendingLogicalLink
	{
		UEdGraphPin* Pin = nullptr;
		bool bThroughGeneratedRoute = false;
	};

	// Generated reroutes are presentation artifacts. Collapse every tagged knot chain back to its
	// real endpoint pair for semantic layout, so a second Format + Route pass sees the same graph as
	// the first pass rather than treating its own knots as new ranks/components.
	const auto ResolveLogicalInputs = [&ValidatedGeneratedKnots](UEdGraphPin& OutputPin)
	{
		TArray<FPendingLogicalLink> Pending;
		Pending.Reserve(OutputPin.LinkedTo.Num());
		for (UEdGraphPin* LinkedPin : OutputPin.LinkedTo)
		{
			Pending.Add({ LinkedPin, false });
		}

		TSet<const UEdGraphPin*> VisitedPins;
		TMap<UEdGraphPin*, bool> ResolvedInputs;
		for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
		{
			const FPendingLogicalLink Current = Pending[PendingIndex];
			if (Current.Pin == nullptr || VisitedPins.Contains(Current.Pin)) { continue; }
			VisitedPins.Add(Current.Pin);

			UEdGraphNode* OwningNode = Current.Pin->GetOwningNodeUnchecked();
			if (ValidatedGeneratedKnots.Contains(OwningNode))
			{
				const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(OwningNode);
				if (Knot != nullptr)
				{
					if (UEdGraphPin* KnotOutput = Knot->GetOutputPin())
					{
						for (UEdGraphPin* LinkedPin : KnotOutput->LinkedTo)
						{
							Pending.Add({ LinkedPin, true });
						}
					}
				}
				continue;
			}

			if (Current.Pin->Direction != EGPD_Input) { continue; }
			if (bool* ExistingPath = ResolvedInputs.Find(Current.Pin))
			{
				// Prefer a real direct link if malformed topology exposes both a direct and routed path.
				*ExistingPath = *ExistingPath && Current.bThroughGeneratedRoute;
			}
			else
			{
				ResolvedInputs.Add(Current.Pin, Current.bThroughGeneratedRoute);
			}
		}
		return ResolvedInputs;
	};

	TArray<FAdapterEdgeRecord> EdgeRecords;
	for (const FAdapterNodeRecord& AdapterNode : NodeRecords)
	{
		for (UEdGraphPin* OutputPin : AdapterNode.Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			const FAdapterPinRecord* OutputPinRecord = PinRecords.Find(OutputPin);
			if (OutputPinRecord == nullptr) { continue; }

			const TMap<UEdGraphPin*, bool> LogicalInputs = ResolveLogicalInputs(*OutputPin);
			for (const TPair<UEdGraphPin*, bool>& LogicalInput : LogicalInputs)
			{
				UEdGraphPin* InputPin = LogicalInput.Key;
				if (InputPin == nullptr || InputPin->Direction != EGPD_Input) { continue; }
				UEdGraphNode* InputNode = InputPin->GetOwningNodeUnchecked();
				const int32* InputNodeIndex = NodeRecordIndices.Find(InputNode);
				const FAdapterPinRecord* InputPinRecord = PinRecords.Find(InputPin);
				if (InputNodeIndex == nullptr || InputPinRecord == nullptr) { continue; }

				if (OutputPinRecord->bExecution != InputPinRecord->bExecution)
				{
					Result.Diagnostics.Add(
						FString::Printf(
							TEXT("Skipped invalid exec/data link from '%s' to '%s'."),
							*AdapterNode.Node->GetName(),
							*InputNode->GetName()
						)
					);
					continue;
				}

				const FAdapterNodeRecord& InputAdapterNode = NodeRecords[*InputNodeIndex];
				FAdapterEdgeRecord& Edge = EdgeRecords.AddDefaulted_GetRef();
				Edge.OutputPin = OutputPin;
				Edge.InputPin = InputPin;
				Edge.bExecution = OutputPinRecord->bExecution;
				Edge.bExistingGeneratedRoute = LogicalInput.Value;
				Edge.BranchOrder = Edge.bExecution ? OutputPinRecord->KindOrder : INDEX_NONE;
				Edge.bPreferredAlignment = Edge.bExecution && OutputPinRecord->bPreferredExecutionPort
										&& InputPinRecord->bPreferredExecutionPort;
				Edge.Key = FString::Printf(
					TEXT("%s:%s/%s->%s/%s"),
					Edge.bExecution ? TEXT("E") : TEXT("D"),
					*AdapterNode.Key,
					*OutputPinRecord->Key,
					*InputAdapterNode.Key,
					*InputPinRecord->Key
				);
			}
		}
	}
	EdgeRecords.Sort([](const FAdapterEdgeRecord& Left, const FAdapterEdgeRecord& Right) { return Left.Key < Right.Key; });
	for (FAdapterEdgeRecord& Edge : EdgeRecords)
	{
		if (!Edge.bExistingGeneratedRoute) { continue; }
		const FValidatedGeneratedRoute* ExistingRoute = ValidatedGeneratedRoutes.Find(Edge.Key);
		if (ExistingRoute == nullptr || ExistingRoute->OutputPin != Edge.OutputPin
			|| ExistingRoute->InputPin != Edge.InputPin)
		{
			Edge.ExistingGeneratedKnots.Reset();
			Edge.ExistingRouteWaypoints.Reset();
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Could not validate the generated reroute chain for '%s'."), *Edge.Key)
			);
		}
		else
		{
			Edge.ExistingGeneratedKnots = ExistingRoute->Knots;
			Edge.ExistingRouteWaypoints = ExistingRoute->Waypoints;
		}
	}

	for (const FAdapterEdgeRecord& Edge : EdgeRecords)
	{
		const UEdGraphNode* OutputNode = Edge.OutputPin->GetOwningNodeUnchecked();
		const UEdGraphNode* InputNode = Edge.InputPin->GetOwningNodeUnchecked();
		const FAdapterNodeRecord& OutputAdapterNode = NodeRecords[*NodeRecordIndices.Find(OutputNode)];
		const FAdapterNodeRecord& InputAdapterNode = NodeRecords[*NodeRecordIndices.Find(InputNode)];
		const FAdapterPinRecord& OutputPinRecord = *PinRecords.Find(Edge.OutputPin);
		const FAdapterPinRecord& InputPinRecord = *PinRecords.Find(Edge.InputPin);

		if (Edge.bExecution)
		{
			K2Layout::FExecutionEdgeSnapshot& LayoutEdge = LayoutSnapshot.ExecutionEdges.AddDefaulted_GetRef();
			LayoutEdge.Key = K2Layout::FEdgeKey(Edge.Key);
			LayoutEdge.Source.Node = K2Layout::FNodeKey(OutputAdapterNode.Key);
			LayoutEdge.Source.Port = K2Layout::FPortKey(OutputPinRecord.Key);
			LayoutEdge.Target.Node = K2Layout::FNodeKey(InputAdapterNode.Key);
			LayoutEdge.Target.Port = K2Layout::FPortKey(InputPinRecord.Key);
			LayoutEdge.BranchOrder = Edge.BranchOrder;
			LayoutEdge.bPreferredAlignment = Edge.bPreferredAlignment;
		}
		else
		{
			K2Layout::FDataEdgeSnapshot& LayoutEdge = LayoutSnapshot.DataEdges.AddDefaulted_GetRef();
			LayoutEdge.Key = K2Layout::FEdgeKey(Edge.Key);
			LayoutEdge.Source.Node = K2Layout::FNodeKey(OutputAdapterNode.Key);
			LayoutEdge.Source.Port = K2Layout::FPortKey(OutputPinRecord.Key);
			LayoutEdge.Target.Node = K2Layout::FNodeKey(InputAdapterNode.Key);
			LayoutEdge.Target.Port = K2Layout::FPortKey(InputPinRecord.Key);
		}
	}

	const double GridSize = FMath::Max(1.0, static_cast<double>(SNodePanel::GetSnapGridSize()));
	K2Layout::FLayoutSettings LayoutSettings;
	LayoutSettings.HorizontalSpacing = static_cast<double>(Settings.K2HorizontalSpacing);
	LayoutSettings.VerticalSpacing = static_cast<double>(Settings.K2VerticalSpacing);
	LayoutSettings.BranchSpacing = static_cast<double>(Settings.K2BranchSpacing);
	LayoutSettings.PureNodeHorizontalSpacing = static_cast<double>(Settings.K2PureHorizontalSpacing);
	LayoutSettings.PureNodeVerticalSpacing = static_cast<double>(Settings.K2PureVerticalSpacing);
	LayoutSettings.CollisionClearance = static_cast<double>(Settings.K2ObstacleClearance);
	LayoutSettings.ComponentSpacing =
		FVector2D(static_cast<double>(Settings.K2ComponentSpacing), static_cast<double>(Settings.K2ComponentSpacing));
	LayoutSettings.GridSize = FVector2D(GridSize, GridSize);
	LayoutSettings.OrderingSweeps = Settings.K2OrderingSweeps;
	LayoutSettings.AdjacentSwapPasses = FMath::Clamp(Settings.K2OrderingSweeps / 3, 1, 12);
	LayoutSettings.GridPolicy = Settings.bEnableHybridGridSnap ? K2Layout::EGridPolicy::HybridExecution
															   : K2Layout::EGridPolicy::NodeGrid;

	K2Layout::FLayoutPlan LayoutPlan = K2Layout::BuildLayout(LayoutSnapshot, LayoutSettings);
	for (const K2Layout::FLayoutDiagnostic& Diagnostic : LayoutPlan.Diagnostics)
	{
		Result.Diagnostics.Add(Diagnostic.Message);
	}
	if (LayoutPlan.HasErrors() || LayoutPlan.Nodes.Num() != NodeRecords.Num())
	{
		Result.Status = EK2FormatStatus::LayoutFailed;
		Result.Message = TEXT("The deterministic K2 layout core rejected part of the graph; no graph state was changed.");
		return Result;
	}
	TMap<FString, TArray<FVector2D>> ExecutionRouteWaypoints;
	for (const K2Layout::FPlannedEdgeRoute& Route : LayoutPlan.ExecutionRoutes)
	{
		ExecutionRouteWaypoints.Add(Route.Edge.Value, Route.Waypoints);
	}

	FVector2D OriginalTopLeft(DBL_MAX, DBL_MAX);
	for (const FAdapterNodeRecord& Node : NodeRecords)
	{
		OriginalTopLeft.X = FMath::Min(OriginalTopLeft.X, Node.OriginalPosition.X);
		OriginalTopLeft.Y = FMath::Min(OriginalTopLeft.Y, Node.OriginalPosition.Y);
	}
	if (Settings.bPreserveComponentAnchor)
	{
		OriginalTopLeft.X = FMath::GridSnap(OriginalTopLeft.X, GridSize);
		OriginalTopLeft.Y = FMath::GridSnap(OriginalTopLeft.Y, GridSize);
	}

	FVector2D PlannedTopLeft(DBL_MAX, DBL_MAX);
	for (const K2Layout::FPlannedNodePosition& PlannedNode : LayoutPlan.Nodes)
	{
		PlannedTopLeft.X = FMath::Min(PlannedTopLeft.X, PlannedNode.Position.X);
		PlannedTopLeft.Y = FMath::Min(PlannedTopLeft.Y, PlannedNode.Position.Y);
	}
	const FVector2D AnchorOffset = Settings.bPreserveComponentAnchor ? OriginalTopLeft - PlannedTopLeft
																	 : FVector2D::ZeroVector;

	TArray<FPlannedAdapterNode> PlannedNodes;
	PlannedNodes.Reserve(LayoutPlan.Nodes.Num());
	TMap<UEdGraphNode*, int32> PlannedNodeIndices;
	TMap<int32, FBox2D> ComponentBaseBounds;
	for (int32 PlannedIndex = 0; PlannedIndex < LayoutPlan.Nodes.Num(); ++PlannedIndex)
	{
		const K2Layout::FPlannedNodePosition& LayoutNode = LayoutPlan.Nodes[PlannedIndex];
		UEdGraphNode* const* NodePointer = NodesByKey.Find(LayoutNode.Node.Value);
		if (NodePointer == nullptr)
		{
			Result.Status = EK2FormatStatus::LayoutFailed;
			Result.Message = TEXT("The K2 layout core returned an unknown stable node key.");
			return Result;
		}
		UEdGraphNode* Node = *NodePointer;
		const FAdapterNodeRecord& AdapterNode = NodeRecords[*NodeRecordIndices.Find(Node)];

		FPlannedAdapterNode& PlannedNode = PlannedNodes.AddDefaulted_GetRef();
		PlannedNode.Node = Node;
		PlannedNode.Position = LayoutNode.Position + AnchorOffset;
		PlannedNode.Position.X = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.X));
		PlannedNode.Position.Y = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.Y));
		PlannedNode.Size = AdapterNode.Size;
		PlannedNode.ComponentIndex = LayoutNode.ComponentIndex >= 0 ? LayoutNode.ComponentIndex : 1000000 + PlannedIndex;
		PlannedNode.ExecutionRank = LayoutNode.ExecutionRank;
		PlannedNodeIndices.Add(Node, PlannedIndex);
		const FBox2D NodeBounds = MakeNodeBox(PlannedNode.Position, PlannedNode.Size);
		if (FBox2D* ComponentBounds = ComponentBaseBounds.Find(PlannedNode.ComponentIndex))
		{
			*ComponentBounds += NodeBounds;
		}
		else
		{
			ComponentBaseBounds.Add(PlannedNode.ComponentIndex, NodeBounds);
		}
	}

	TSet<UEdGraphNode*> MovableGeneratedKnots;
	for (const TPair<FString, FValidatedGeneratedRoute>& RoutePair : ValidatedGeneratedRoutes)
	{
		const FValidatedGeneratedRoute& Route = RoutePair.Value;
		const UEdGraphNode* OutputNode = Route.OutputPin != nullptr ? Route.OutputPin->GetOwningNodeUnchecked() : nullptr;
		const UEdGraphNode* InputNode = Route.InputPin != nullptr ? Route.InputPin->GetOwningNodeUnchecked() : nullptr;
		if (NodeRecordIndices.Contains(OutputNode) && NodeRecordIndices.Contains(InputNode))
		{
			for (UEdGraphNode* Knot : Route.Knots)
			{
				MovableGeneratedKnots.Add(Knot);
			}
		}
	}

	TArray<FBox2D> FixedObstacles;
	for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
	{
		UEdGraphNode* GraphNode = GraphNodePointer.Get();
		if (GraphNode == nullptr) { continue; }
		if (ValidatedGeneratedKnots.Contains(GraphNode))
		{
			if (MovableGeneratedKnots.Contains(GraphNode)) { continue; }
		}
		else if (Scope.Contains(GraphNode)) { continue; }
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(GraphNode))
		{
			bool bEnclosesScope = false;
			for (UEdGraphNode* ScopeNode : Scope)
			{
				if (ScopeNode != nullptr && IsOriginalCommentMember(Graph, Geometry, *Comment, *ScopeNode))
				{
					bEnclosesScope = true;
					break;
				}
			}
			// Enclosing comments are resized after layout. Every unrelated comment remains a fixed
			// obstacle so a partial selection cannot accidentally move into a foreign group.
			if (bEnclosesScope) { continue; }
		}
		FixedObstacles.Add(
			InflateBox(ResolveCurrentNodeBounds(*GraphNode, Geometry), static_cast<double>(Settings.K2ObstacleClearance))
		);
	}

	TArray<int32> ComponentIndices;
	ComponentBaseBounds.GetKeys(ComponentIndices);
	ComponentIndices.Sort();
	TMap<int32, double> ComponentVerticalShifts;
	TMap<int32, FBox2D> ResolvedComponentBounds;
	for (const int32 ComponentIndex : ComponentIndices)
	{
		const FBox2D BaseBounds = ComponentBaseBounds.FindChecked(ComponentIndex);
		double VerticalShift = 0.0;
		bool bResolved = false;
		for (int32 Pass = 0; Pass < MaximumObstacleResolutionPasses; ++Pass)
		{
			const FBox2D Candidate = BaseBounds.ShiftBy(FVector2D(0.0, VerticalShift));
			double RequiredShift = VerticalShift;
			bool bIntersects = false;
			for (const FBox2D& Obstacle : FixedObstacles)
			{
				if (Candidate.Intersect(Obstacle))
				{
					bIntersects = true;
					RequiredShift = FMath::Max(RequiredShift, Obstacle.Max.Y + GridSize - BaseBounds.Min.Y);
				}
			}
			for (const TPair<int32, FBox2D>& OtherComponent : ComponentBaseBounds)
			{
				if (OtherComponent.Key == ComponentIndex) { continue; }
				const FBox2D* ResolvedBounds = ResolvedComponentBounds.Find(OtherComponent.Key);
				const FBox2D Obstacle = InflateBox(
					ResolvedBounds != nullptr ? *ResolvedBounds : OtherComponent.Value,
					static_cast<double>(Settings.K2ObstacleClearance)
				);
				if (Candidate.Intersect(Obstacle))
				{
					bIntersects = true;
					RequiredShift = FMath::Max(RequiredShift, Obstacle.Max.Y + GridSize - BaseBounds.Min.Y);
				}
			}

			if (!bIntersects)
			{
				bResolved = true;
				break;
			}
			VerticalShift = SnapUp(FMath::Max(VerticalShift + GridSize, RequiredShift), GridSize);
		}

		if (!bResolved)
		{
			Result.Status = EK2FormatStatus::LayoutFailed;
			Result.Message = TEXT("A formatted component could not be placed clear of fixed graph nodes.");
			return Result;
		}
		ComponentVerticalShifts.Add(ComponentIndex, VerticalShift);
		ResolvedComponentBounds.Add(ComponentIndex, BaseBounds.ShiftBy(FVector2D(0.0, VerticalShift)));
		if (VerticalShift > 0.0)
		{
			Result.Diagnostics.Add(
				FString::Printf(
					TEXT("Shifted layout component %d downward by %.0f units to avoid fixed nodes."), ComponentIndex, VerticalShift
				)
			);
		}
	}

	TMap<UEdGraphNode*, FVector2D> FinalNodePositions;
	TMap<UEdGraphNode*, FVector2D> PlannedNodeSizes;
	TMap<UEdGraphNode*, int32> ExecutionRanks;
	TArray<FNodePositionChange> NodeChanges;
	for (FPlannedAdapterNode& PlannedNode : PlannedNodes)
	{
		PlannedNode.Position.Y += ComponentVerticalShifts.FindChecked(PlannedNode.ComponentIndex);
		PlannedNode.Position.X = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.X));
		PlannedNode.Position.Y = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.Y));
		FinalNodePositions.Add(PlannedNode.Node, PlannedNode.Position);
		PlannedNodeSizes.Add(PlannedNode.Node, PlannedNode.Size);
		ExecutionRanks.Add(PlannedNode.Node, PlannedNode.ExecutionRank);
		if (PlannedNode.Node->NodePosX != FMath::RoundToInt(PlannedNode.Position.X)
			|| PlannedNode.Node->NodePosY != FMath::RoundToInt(PlannedNode.Position.Y))
		{
			NodeChanges.Add({ PlannedNode.Node, PlannedNode.Position });
		}
	}

	// Generated knots are presentation artifacts and do not participate in semantic ranking. Keep
	// a validated existing chain attached to its newly formatted endpoints by interpolating the two
	// endpoint deltas across its ordered knots. This changes geometry only; pin topology is untouched.
	TMap<UEdGraphNode*, FVector2D> PlannedGeneratedKnotPositions;
	for (FAdapterEdgeRecord& Edge : EdgeRecords)
	{
		if (!Edge.bExistingGeneratedRoute || Edge.ExistingGeneratedKnots.IsEmpty()
			|| Edge.ExistingGeneratedKnots.Num() != Edge.ExistingRouteWaypoints.Num())
		{
			continue;
		}

		UEdGraphNode* OutputNode = Edge.OutputPin->GetOwningNodeUnchecked();
		UEdGraphNode* InputNode = Edge.InputPin->GetOwningNodeUnchecked();
		const int32* OutputNodeIndex = NodeRecordIndices.Find(OutputNode);
		const int32* InputNodeIndex = NodeRecordIndices.Find(InputNode);
		const FAdapterPinRecord* OutputPinRecord = PinRecords.Find(Edge.OutputPin);
		const FAdapterPinRecord* InputPinRecord = PinRecords.Find(Edge.InputPin);
		const FVector2D* FinalOutputPosition = FinalNodePositions.Find(OutputNode);
		const FVector2D* FinalInputPosition = FinalNodePositions.Find(InputNode);
		if (OutputNodeIndex == nullptr || InputNodeIndex == nullptr || OutputPinRecord == nullptr
			|| InputPinRecord == nullptr || FinalOutputPosition == nullptr || FinalInputPosition == nullptr)
		{
			continue;
		}

		const FVector2D OriginalOutputAnchor = NodeRecords[*OutputNodeIndex].OriginalPosition + OutputPinRecord->Offset;
		const FVector2D OriginalInputAnchor = NodeRecords[*InputNodeIndex].OriginalPosition + InputPinRecord->Offset;
		const FVector2D OutputDelta = *FinalOutputPosition + OutputPinRecord->Offset - OriginalOutputAnchor;
		const FVector2D InputDelta = *FinalInputPosition + InputPinRecord->Offset - OriginalInputAnchor;
		for (int32 KnotIndex = 0; KnotIndex < Edge.ExistingGeneratedKnots.Num(); ++KnotIndex)
		{
			UEdGraphNode* Knot = Edge.ExistingGeneratedKnots[KnotIndex];
			const FVector2D OriginalKnotTopLeft(static_cast<double>(Knot->NodePosX), static_cast<double>(Knot->NodePosY));
			const FVector2D CenterOffset = Edge.ExistingRouteWaypoints[KnotIndex] - OriginalKnotTopLeft;
			const double Alpha = static_cast<double>(KnotIndex + 1)
							   / static_cast<double>(Edge.ExistingGeneratedKnots.Num() + 1);
			FVector2D PlannedCenter = Edge.ExistingRouteWaypoints[KnotIndex] + FMath::Lerp(OutputDelta, InputDelta, Alpha);
			PlannedCenter.X = FMath::GridSnap(PlannedCenter.X, GridSize);
			PlannedCenter.Y = FMath::GridSnap(PlannedCenter.Y, GridSize);
			const FVector2D PlannedKnotTopLeft = PlannedCenter - CenterOffset;

			if (const FVector2D* ExistingPosition = PlannedGeneratedKnotPositions.Find(Knot))
			{
				if (!ExistingPosition->Equals(PlannedKnotTopLeft))
				{
					Result.Status = EK2FormatStatus::LayoutFailed;
					Result.Message =
						TEXT("A generated reroute node belonged to conflicting logical routes; no graph state was changed.");
					return Result;
				}
			}
			else
			{
				PlannedGeneratedKnotPositions.Add(Knot, PlannedKnotTopLeft);
				FinalNodePositions.Add(Knot, PlannedKnotTopLeft);
				PlannedNodeSizes.Add(Knot, ResolveNodeSize(*Knot, Geometry));
				if (Knot->NodePosX != FMath::RoundToInt(PlannedKnotTopLeft.X)
					|| Knot->NodePosY != FMath::RoundToInt(PlannedKnotTopLeft.Y))
				{
					NodeChanges.Add({ Knot, PlannedKnotTopLeft });
				}
			}
			Edge.ExistingRouteWaypoints[KnotIndex] = PlannedCenter;
		}
	}

	// Formatting a selection can move nodes out of an unselected parent comment. Discover every
	// affected enclosing comment, including nested ancestors, before any graph state is changed.
	TSet<UEdGraphNode*> AffectedNodes = Scope;
	TSet<UEdGraphNode_Comment*> AffectedComments;
	bool bDiscoveredComment = false;
	do
	{
		bDiscoveredComment = false;
		for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
		{
			UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(GraphNodePointer.Get());
			if (Comment == nullptr || AffectedComments.Contains(Comment)) { continue; }

			bool bAffected = Scope.Contains(Comment);
			if (!bAffected)
			{
				for (UEdGraphNode* AffectedNode : AffectedNodes)
				{
					if (AffectedNode != nullptr && IsOriginalCommentMember(Graph, Geometry, *Comment, *AffectedNode))
					{
						bAffected = true;
						break;
					}
				}
			}
			if (!bAffected) { continue; }

			AffectedComments.Add(Comment);
			AffectedNodes.Add(Comment);
			bDiscoveredComment = true;
		}
	}
	while (bDiscoveredComment);

	TArray<FCommentRecord> CommentRecords;
	CommentRecords.Reserve(AffectedComments.Num());
	for (UEdGraphNode_Comment* Comment : AffectedComments)
	{
		FCommentRecord& Record = CommentRecords.AddDefaulted_GetRef();
		Record.Comment = Comment;
		Record.Key = MakeStableNodeKey(Graph, *Comment);
		Record.OriginalBounds = ResolveCurrentNodeBounds(*Comment, Geometry);
	}
	CommentRecords.Sort([](const FCommentRecord& Left, const FCommentRecord& Right) { return Left.Key < Right.Key; });
	TMap<UEdGraphNode_Comment*, int32> CommentRecordIndices;
	for (int32 CommentIndex = 0; CommentIndex < CommentRecords.Num(); ++CommentIndex)
	{
		CommentRecordIndices.Add(CommentRecords[CommentIndex].Comment, CommentIndex);
	}

	for (FCommentRecord& CommentRecord : CommentRecords)
	{
		TSet<UEdGraphNode*> MemberSet;
		for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
		{
			UEdGraphNode* Candidate = GraphNodePointer.Get();
			if (Candidate != nullptr && !ValidatedGeneratedKnots.Contains(Candidate)
				&& IsOriginalCommentMember(Graph, Geometry, *CommentRecord.Comment, *Candidate))
			{
				// Stationary members matter too: an affected parent comment must never shrink across
				// an unselected node while it expands around the nodes that were formatted.
				MemberSet.Add(Candidate);
			}
		}
		CommentRecord.Members = MemberSet.Array();
		CommentRecord.Members.Sort([&Graph](const UEdGraphNode& Left, const UEdGraphNode& Right)
								   { return MakeStableNodeKey(Graph, Left) < MakeStableNodeKey(Graph, Right); });
	}

	TArray<int32> CommentApplicationOrder;
	TFunction<bool(int32)> ComputeCommentBounds = [&](const int32 CommentIndex)
	{
		FCommentRecord& Record = CommentRecords[CommentIndex];
		if (Record.VisitState == 2) { return true; }
		if (Record.VisitState == 1)
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Ignored cyclic comment membership at '%s'."), *Record.Comment->GetName())
			);
			return false;
		}

		Record.VisitState = 1;
		FBox2D ContentBounds(EForceInit::ForceInit);
		for (UEdGraphNode* Member : Record.Members)
		{
			if (UEdGraphNode_Comment* MemberComment = Cast<UEdGraphNode_Comment>(Member))
			{
				if (const int32* MemberCommentIndex = CommentRecordIndices.Find(MemberComment))
				{
					if (ComputeCommentBounds(*MemberCommentIndex))
					{
						ContentBounds += CommentRecords[*MemberCommentIndex].PlannedBounds;
					}
					continue;
				}
			}

			if (const FVector2D* Position = FinalNodePositions.Find(Member))
			{
				const FVector2D* Size = PlannedNodeSizes.Find(Member);
				if (Size != nullptr)
				{
					ContentBounds += MakeNodeBox(*Position, *Size);
					continue;
				}
			}
			ContentBounds += ResolveCurrentNodeBounds(*Member, Geometry);
		}

		Record.PlannedBounds = ContentBounds.bIsValid
								 ? InflateBox(ContentBounds, static_cast<double>(Settings.K2CommentPadding))
								 : Record.OriginalBounds;
		Record.VisitState = 2;
		CommentApplicationOrder.Add(CommentIndex);
		return true;
	};
	for (int32 CommentIndex = 0; CommentIndex < CommentRecords.Num(); ++CommentIndex)
	{
		ComputeCommentBounds(CommentIndex);
	}

	TArray<FCommentBoundsChange> CommentChanges;
	for (const int32 CommentIndex : CommentApplicationOrder)
	{
		FCommentRecord& Record = CommentRecords[CommentIndex];
		Record.PlannedBounds.Min.X = FMath::FloorToDouble(Record.PlannedBounds.Min.X / GridSize) * GridSize;
		Record.PlannedBounds.Min.Y = FMath::FloorToDouble(Record.PlannedBounds.Min.Y / GridSize) * GridSize;
		Record.PlannedBounds.Max.X = FMath::CeilToDouble(Record.PlannedBounds.Max.X / GridSize) * GridSize;
		Record.PlannedBounds.Max.Y = FMath::CeilToDouble(Record.PlannedBounds.Max.Y / GridSize) * GridSize;
		const bool bPositionChanged = Record.Comment->NodePosX != FMath::RoundToInt(Record.PlannedBounds.Min.X)
								   || Record.Comment->NodePosY != FMath::RoundToInt(Record.PlannedBounds.Min.Y);
		const bool bSizeChanged = Record.Comment->NodeWidth != FMath::RoundToInt(Record.PlannedBounds.GetSize().X)
							   || Record.Comment->NodeHeight != FMath::RoundToInt(Record.PlannedBounds.GetSize().Y);
		if (bPositionChanged || bSizeChanged) { CommentChanges.Add({ Record.Comment, Record.PlannedBounds }); }
	}

	// A group comment's membership is geometric and Unreal refreshes it from containment. Reject a
	// plan that would silently capture unrelated nodes or manufacture a new non-nested comment
	// overlap. This is evaluated before the transaction, so unsafe comment layouts are atomic no-ops.
	for (const FCommentRecord& Record : CommentRecords)
	{
		TSet<const UEdGraphNode*> OriginalMembers;
		for (UEdGraphNode* Member : Record.Members)
		{
			OriginalMembers.Add(Member);
		}
		for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
		{
			UEdGraphNode* Candidate = GraphNodePointer.Get();
			if (Candidate == nullptr || Candidate == Record.Comment || OriginalMembers.Contains(Candidate)
				|| ValidatedGeneratedKnots.Contains(Candidate))
			{
				continue;
			}

			FBox2D CandidateBounds = ResolveCurrentNodeBounds(*Candidate, Geometry);
			if (const FVector2D* Position = FinalNodePositions.Find(Candidate))
			{
				if (const FVector2D* Size = PlannedNodeSizes.Find(Candidate))
				{
					CandidateBounds = MakeNodeBox(*Position, *Size);
				}
			}
			if (const UEdGraphNode_Comment* CandidateComment = Cast<UEdGraphNode_Comment>(Candidate))
			{
				if (const int32* CandidateIndex = CommentRecordIndices.Find(CandidateComment))
				{
					CandidateBounds = CommentRecords[*CandidateIndex].PlannedBounds;
				}
			}

			const FBox2D OriginalCandidateBounds = ResolveCurrentNodeBounds(*Candidate, Geometry);
			const bool bNewCapture = StrictlyContains(Record.PlannedBounds, CandidateBounds)
								  && !StrictlyContains(Record.OriginalBounds, OriginalCandidateBounds);
			if (bNewCapture)
			{
				Result.Status = EK2FormatStatus::LayoutFailed;
				Result.Message = FString::Printf(
					TEXT("Formatting '%s' would capture unrelated node '%s'; no graph state was changed."),
					*Record.Comment->GetName(),
					*Candidate->GetName()
				);
				return Result;
			}

			const UEdGraphNode_Comment* OtherComment = Cast<UEdGraphNode_Comment>(Candidate);
			if (OtherComment == nullptr) { continue; }
			const bool bPlannedNested = StrictlyContains(Record.PlannedBounds, CandidateBounds)
									 || StrictlyContains(CandidateBounds, Record.PlannedBounds);
			const bool bOriginallyOverlapped = Record.OriginalBounds.Intersect(OriginalCandidateBounds);
			if (Record.PlannedBounds.Intersect(CandidateBounds) && !bPlannedNested && !bOriginallyOverlapped)
			{
				Result.Status = EK2FormatStatus::LayoutFailed;
				Result.Message = FString::Printf(
					TEXT("Formatting '%s' would create an ambiguous overlap with comment '%s'; no graph state was changed."),
					*Record.Comment->GetName(),
					*OtherComment->GetName()
				);
				return Result;
			}
		}
	}

	TArray<FRerouteEdge> RerouteEdges;
	TArray<FRerouteObstacle> RerouteObstacles;
	if (bRouteWires)
	{
		RerouteEdges.Reserve(EdgeRecords.Num() + ValidatedGeneratedRoutes.Num());
		TSet<FString> AddedRerouteKeys;
		for (const FAdapterEdgeRecord& Edge : EdgeRecords)
		{
			UEdGraphNode* OutputNode = Edge.OutputPin->GetOwningNodeUnchecked();
			UEdGraphNode* InputNode = Edge.InputPin->GetOwningNodeUnchecked();
			const FVector2D* OutputPosition = FinalNodePositions.Find(OutputNode);
			const FVector2D* InputPosition = FinalNodePositions.Find(InputNode);
			const FAdapterPinRecord* OutputPinRecord = PinRecords.Find(Edge.OutputPin);
			const FAdapterPinRecord* InputPinRecord = PinRecords.Find(Edge.InputPin);
			if (OutputPosition == nullptr || InputPosition == nullptr || OutputPinRecord == nullptr
				|| InputPinRecord == nullptr)
			{
				continue;
			}

			FRerouteEdge& RerouteEdge = RerouteEdges.AddDefaulted_GetRef();
			RerouteEdge.OutputPin = Edge.OutputPin;
			RerouteEdge.InputPin = Edge.InputPin;
			RerouteEdge.OutputAnchor = *OutputPosition + OutputPinRecord->Offset;
			RerouteEdge.InputAnchor = *InputPosition + InputPinRecord->Offset;
			RerouteEdge.StableKey = Edge.Key;
			AddedRerouteKeys.Add(Edge.Key);
			RerouteEdge.bExecution = Edge.bExecution;
			RerouteEdge.bExistingGeneratedRoute = Edge.bExistingGeneratedRoute;
			if (Edge.bExistingGeneratedRoute)
			{
				RerouteEdge.PreferredWaypoints = Edge.ExistingRouteWaypoints;
				if (RerouteEdge.PreferredWaypoints.IsEmpty())
				{
					Result.Diagnostics.Add(
						FString::Printf(TEXT("Could not validate the generated reroute chain for '%s'."), *Edge.Key)
					);
				}
			}
			else if (Edge.bExecution)
			{
				if (const TArray<FVector2D>* CoreWaypoints = ExecutionRouteWaypoints.Find(Edge.Key))
				{
					const int32* PlannedOutputIndex = PlannedNodeIndices.Find(OutputNode);
					if (PlannedOutputIndex != nullptr)
					{
						const int32 ComponentIndex = PlannedNodes[*PlannedOutputIndex].ComponentIndex;
						const FVector2D RouteOffset = AnchorOffset
													+ FVector2D(0.0, ComponentVerticalShifts.FindRef(ComponentIndex));
						RerouteEdge.PreferredWaypoints.Reserve(CoreWaypoints->Num());
						for (const FVector2D& Waypoint : *CoreWaypoints)
						{
							RerouteEdge.PreferredWaypoints.Add(Waypoint + RouteOffset);
						}
					}
				}
			}
			const int32 OutputRank = ExecutionRanks.FindRef(OutputNode);
			const int32 InputRank = ExecutionRanks.FindRef(InputNode);
			if (OutputRank != INDEX_NONE && InputRank != INDEX_NONE)
			{
				RerouteEdge.RankSpan = FMath::Abs(InputRank - OutputRank);
			}
			else
			{
				const double RankWidth = FMath::Max(1.0, static_cast<double>(Settings.K2HorizontalSpacing));
				RerouteEdge.RankSpan =
					FMath::RoundToInt(FMath::Abs(RerouteEdge.InputAnchor.X - RerouteEdge.OutputAnchor.X) / RankWidth);
			}
		}

		const auto ResolveRoutePinOffset = [&Geometry, &PinRecords](UEdGraphPin& Pin)
		{
			if (const FAdapterPinRecord* Existing = PinRecords.Find(&Pin)) { return Existing->Offset; }
			UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
			const FVector2D NodeSize = ResolveNodeSize(*Node, Geometry);
			int32 DirectionOrdinal = 0;
			for (UEdGraphPin* CandidatePin : Node->Pins)
			{
				if (CandidatePin == &Pin) { break; }
				if (CandidatePin != nullptr && CandidatePin->Direction == Pin.Direction) { ++DirectionOrdinal; }
			}
			return ResolvePinOffset(Pin, Geometry, NodeSize, DirectionOrdinal);
		};
		TArray<FString> ValidatedRouteKeys;
		ValidatedGeneratedRoutes.GetKeys(ValidatedRouteKeys);
		ValidatedRouteKeys.Sort();
		for (const FString& StableKey : ValidatedRouteKeys)
		{
			if (AddedRerouteKeys.Contains(StableKey)) { continue; }
			const FValidatedGeneratedRoute& ExistingRoute = ValidatedGeneratedRoutes.FindChecked(StableKey);
			UEdGraphNode* OutputNode = ExistingRoute.OutputPin->GetOwningNodeUnchecked();
			UEdGraphNode* InputNode = ExistingRoute.InputPin->GetOwningNodeUnchecked();
			const FVector2D OutputPosition =
				FinalNodePositions.Contains(OutputNode)
					? FinalNodePositions.FindChecked(OutputNode)
					: FVector2D(static_cast<double>(OutputNode->NodePosX), static_cast<double>(OutputNode->NodePosY));
			const FVector2D InputPosition =
				FinalNodePositions.Contains(InputNode)
					? FinalNodePositions.FindChecked(InputNode)
					: FVector2D(static_cast<double>(InputNode->NodePosX), static_cast<double>(InputNode->NodePosY));
			FRerouteEdge& Reservation = RerouteEdges.AddDefaulted_GetRef();
			Reservation.OutputPin = ExistingRoute.OutputPin;
			Reservation.InputPin = ExistingRoute.InputPin;
			Reservation.OutputAnchor = OutputPosition + ResolveRoutePinOffset(*ExistingRoute.OutputPin);
			Reservation.InputAnchor = InputPosition + ResolveRoutePinOffset(*ExistingRoute.InputPin);
			Reservation.StableKey = StableKey;
			Reservation.PreferredWaypoints = ExistingRoute.Waypoints;
			Reservation.bExecution = IsExecutionPin(*ExistingRoute.OutputPin);
			Reservation.bExistingGeneratedRoute = true;
		}

		RerouteObstacles.Reserve(Graph.Nodes.Num());
		for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
		{
			UEdGraphNode* GraphNode = GraphNodePointer.Get();
			if (GraphNode == nullptr || GraphNode->IsA<UEdGraphNode_Comment>()) { continue; }
			FRerouteObstacle& Obstacle = RerouteObstacles.AddDefaulted_GetRef();
			Obstacle.Node = GraphNode;
			if (const FVector2D* Position = FinalNodePositions.Find(GraphNode))
			{
				Obstacle.Bounds = MakeNodeBox(*Position, PlannedNodeSizes.FindChecked(GraphNode));
			}
			else
			{
				Obstacle.Bounds = ResolveCurrentNodeBounds(*GraphNode, Geometry);
			}
		}
	}

	if (NodeChanges.IsEmpty() && CommentChanges.IsEmpty() && !bRouteWires)
	{
		Result.Status = EK2FormatStatus::NoChanges;
		Result.Message = TEXT("The graph is already formatted.");
		return Result;
	}

	const FText TransactionDescription =
		bRouteWires ? LOCTEXT("FormatAndRouteK2GraphTransaction", "Format and Route Blueprint Graph")
					: LOCTEXT("FormatK2GraphTransaction", "Format Blueprint Graph");
	UPackage* const GraphPackage = Graph.GetOutermost();
	const bool bPackageWasDirty = GraphPackage != nullptr && GraphPackage->IsDirty();
	FScopedTransaction Transaction(TransactionDescription);
	Graph.Modify(false);
	for (const FNodePositionChange& Change : NodeChanges)
	{
		Change.Node->Modify();
		Change.Node->NodePosX = FMath::RoundToInt(Change.Position.X);
		Change.Node->NodePosY = FMath::RoundToInt(Change.Position.Y);
	}
	for (const FCommentBoundsChange& Change : CommentChanges)
	{
		Change.Comment->Modify();
		Change.Comment->SetBounds(FSlateRect(
			static_cast<float>(Change.Bounds.Min.X),
			static_cast<float>(Change.Bounds.Min.Y),
			static_cast<float>(Change.Bounds.Max.X),
			static_cast<float>(Change.Bounds.Max.Y)
		));
	}

	FRerouteResult RerouteResult;
	if (bRouteWires)
	{
		FRerouteSettings RerouteSettings;
		RerouteSettings.ObstacleClearance = static_cast<double>(Settings.K2ObstacleClearance);
		RerouteSettings.ChannelSpacing = static_cast<double>(Settings.K2RoutingChannelSpacing);
		RerouteSettings.MaxKnotsPerWire = Settings.K2MaxGeneratedKnots;
		RerouteSettings.LongDataWireRankThreshold = Settings.K2LongDataWireRankThreshold;
		RerouteSettings.PlanningWorkBudget = Settings.K2RoutingPlanningWorkBudget;
		RerouteSettings.bRouteDataWires = Settings.bRouteDataWires;
		RerouteResult = FK2RerouteRouter::Route(Graph, RerouteEdges, RerouteObstacles, Scope, RerouteSettings, GridSize);
		Result.RoutedWireCount = RerouteResult.RoutedWires;
		Result.CreatedKnotCount = RerouteResult.CreatedKnots;
		Result.SkippedRerouteWireCount = RerouteResult.SkippedWires;
		Result.Diagnostics.Append(RerouteResult.Diagnostics);
	}

	Result.MovedNodeCount = NodeChanges.Num();
	Result.ResizedCommentCount = CommentChanges.Num();
	if (!Result.WasModified() && !RerouteResult.HasFatalError())
	{
		Transaction.Cancel();
		if (GraphPackage != nullptr && !bPackageWasDirty) { GraphPackage->SetDirtyFlag(false); }
		Result.Status = EK2FormatStatus::NoChanges;
		Result.Message = bRouteWires ? TEXT("The graph is already formatted and no wires required safe rerouting.")
									 : TEXT("The graph is already formatted.");
		RestoreSelection(GraphEditor, OriginalSelection);
		return Result;
	}

	if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	else
	{
		Graph.MarkPackageDirty();
	}
	Graph.NotifyGraphChanged();
	RestoreSelection(GraphEditor, OriginalSelection);
	if (RerouteResult.HasFatalError())
	{
		// Keep the outer transaction: if Unreal ever rejects both schema restoration and the
		// verified low-level restoration fallback, the user still has one atomic Undo path.
		Result.Status = EK2FormatStatus::LayoutFailed;
		Result.Message = TEXT(
			"A reroute boundary failed and its original direct link could not be restored. "
			"The operation was retained as one transaction; undo it immediately."
		);
		return Result;
	}

	Result.Status = EK2FormatStatus::Formatted;
	Result.Message = FString::Printf(
		TEXT("Formatted %d node(s), resized %d comment(s), and created %d reroute node(s)."),
		Result.MovedNodeCount,
		Result.ResizedCommentCount,
		Result.CreatedKnotCount
	);
	return Result;
}
} // namespace GraphFormatter::K2

#undef LOCTEXT_NAMESPACE
