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
#include "Styling/AppStyle.h"
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
constexpr double ReadabilityEpsilon = 1.0;
constexpr double MaterialReadabilityRatio = 1.25;
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
	TArray<FVector2D> PlannedRouteWaypoints;
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
	bool bPreserveAuthoredRerouteChannel = false;
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

struct FReadabilityMetrics
{
	int32 OverlapPairCount = 0;
	TSet<FString> OverlapPairs;
	int32 BackwardExecutionEdgeCount = 0;
	TSet<FString> BackwardExecutionEdges;
	TMap<FString, double> BackwardExecutionDistances;
	double BackwardExecutionDistance = 0.0;
	int32 NonStraightPreferredExecutionEdgeCount = 0;
	TSet<FString> NonStraightPreferredExecutionEdges;
	double PreferredExecutionVerticalError = 0.0;
	TSet<FString> InsufficientPreferredExecutionGaps;
	int32 BackwardDataEdgeCount = 0;
	double BackwardDataDistance = 0.0;
	TSet<FString> WiresThroughNodes;
	TSet<FString> ExecutionWireCrossings;
	TSet<FString> DataWireCrossings;
	int32 ExecutionWireCrossingCount = 0;
	int32 DataWireCrossingCount = 0;
	double MaximumExecutionRootHorizontalDrift = 0.0;
	double MaximumExecutionRootVerticalDrift = 0.0;
	double ExecutionRootColumnDeviationIncrease = 0.0;
	int32 ExecutionRootOrderInversionCount = 0;
	bool bWorkBudgetExhausted = false;
};

struct FReadabilityWorkBudget
{
	int64 Remaining = 0;

	explicit FReadabilityWorkBudget(const int32 WorkBudget)
		: Remaining(FMath::Max(0, WorkBudget))
	{
	}

	bool TryConsume(const int64 Work)
	{
		if (Work < 0 || Work > Remaining) { return false; }
		Remaining -= Work;
		return true;
	}
};

struct FMeasuredWirePath
{
	FString Key;
	UEdGraphNode* OutputNode = nullptr;
	UEdGraphNode* InputNode = nullptr;
	const UEdGraphPin* OutputPin = nullptr;
	const UEdGraphPin* InputPin = nullptr;
	TArray<FVector2D> RenderedPoints;
	FBox2D RenderedBounds = FBox2D(EForceInit::ForceInit);
	bool bExecution = false;
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
FString MakeLegacyStableNodeKey(const UEdGraph& Graph, const UEdGraphNode& Node)
{
	if (Node.NodeGuid.IsValid()) { return GuidKey(TEXT("N:"), Node.NodeGuid); }

	return FString::Printf(
		TEXT("N:fallback:%s:%08d:%s"), *Graph.GetPathName(), FindGraphNodeIndex(Graph, Node), *Node.GetName()
	);
}

[[nodiscard]]
FString MakeStableNodeKey(const UEdGraph& Graph, const UEdGraphNode& Node)
{
	// DuplicateObject legitimately regenerates Blueprint node GUIDs. The node UObject name remains
	// unique within its graph, so place it before the persistent identity to keep layout/core
	// tie-breaks deterministic across benchmark copies while retaining the GUID for route ownership.
	return FString::Printf(TEXT("N:stable:%s|%s"), *Node.GetName(), *MakeLegacyStableNodeKey(Graph, Node));
}

[[nodiscard]]
FString MakeLegacyStablePinKey(const UEdGraphPin& Pin, const int32 PinOrdinal)
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
FString MakeStablePinKey(const UEdGraphPin& Pin, const int32 PinOrdinal)
{
	// Pin GUIDs can also be reconstructed in transient Blueprint copies. Direction, ordinal, and
	// display name provide a deterministic semantic order; the legacy identity remains as a suffix
	// so generated-route keys still uniquely own a physical pin.
	return FString::Printf(
		TEXT("P:stable:%d:%08d:%s|%s"),
		static_cast<int32>(Pin.Direction.GetValue()),
		PinOrdinal,
		*Pin.PinName.ToString(),
		*MakeLegacyStablePinKey(Pin, PinOrdinal)
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
	if (Node.IsA<UK2Node_Knot>()) { return FVector2D(RerouteKnotWidth, RerouteKnotHeight); }

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
	if (Pin.GetOwningNodeUnchecked() != nullptr && Pin.GetOwningNodeUnchecked()->IsA<UK2Node_Knot>())
	{
		return FVector2D(RerouteKnotWidth * 0.5, RerouteKnotHeight * 0.5);
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
bool HasPositiveAreaIntersection(const FBox2D& First, const FBox2D& Second)
{
	return First.Min.X < Second.Max.X - ReadabilityEpsilon && Second.Min.X < First.Max.X - ReadabilityEpsilon
		&& First.Min.Y < Second.Max.Y - ReadabilityEpsilon && Second.Min.Y < First.Max.Y - ReadabilityEpsilon;
}

[[nodiscard]]
bool TryResolveEdgeAnchors(
	const FAdapterEdgeRecord& Edge,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	FVector2D& OutOutputAnchor,
	FVector2D& OutInputAnchor
)
{
	UEdGraphNode* OutputNode = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
	UEdGraphNode* InputNode = Edge.InputPin != nullptr ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
	const FVector2D* OutputPosition = Positions.Find(OutputNode);
	const FVector2D* InputPosition = Positions.Find(InputNode);
	const FAdapterPinRecord* OutputPin = PinRecords.Find(Edge.OutputPin);
	const FAdapterPinRecord* InputPin = PinRecords.Find(Edge.InputPin);
	if (OutputPosition == nullptr || InputPosition == nullptr || OutputPin == nullptr || InputPin == nullptr)
	{
		return false;
	}

	OutOutputAnchor = *OutputPosition + OutputPin->Offset;
	OutInputAnchor = *InputPosition + InputPin->Offset;
	return true;
}

[[nodiscard]]
bool TryResolvePinAnchor(
	const UEdGraphPin& Pin,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	FVector2D& OutAnchor
)
{
	UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	const FVector2D* Position = Positions.Find(Node);
	const FAdapterPinRecord* PinRecord = PinRecords.Find(&Pin);
	if (Position == nullptr) { return false; }
	if (PinRecord == nullptr)
	{
		// Validated formatter-generated knots are intentionally absent from semantic pin
		// records, but they still participate in a neighboring manual knot's Kismet tangent
		// average. Both knot pins render at the same physical center.
		if (Node != nullptr && Node->IsA<UK2Node_Knot>())
		{
			OutAnchor = *Position + FVector2D(RerouteKnotWidth * 0.5, RerouteKnotHeight * 0.5);
			return true;
		}
		return false;
	}
	OutAnchor = *Position + PinRecord->Offset;
	return true;
}

[[nodiscard]]
bool TryAverageConnectedPinAnchor(
	const UEdGraphPin& Pin,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	FVector2D& OutAverage
)
{
	FVector2D Sum = FVector2D::ZeroVector;
	int32 Count = 0;
	for (const UEdGraphPin* LinkedPin : Pin.LinkedTo)
	{
		FVector2D Anchor;
		if (LinkedPin != nullptr && TryResolvePinAnchor(*LinkedPin, PinRecords, Positions, Anchor))
		{
			Sum += Anchor;
			++Count;
		}
	}
	if (Count == 0) { return false; }
	OutAverage = Sum / static_cast<double>(Count);
	return true;
}

[[nodiscard]]
bool ShouldReverseKnotTangent(
	UEdGraphNode* Node,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions
)
{
	UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
	if (Knot == nullptr) { return false; }
	UEdGraphPin* InputPin = Knot->GetInputPin();
	UEdGraphPin* OutputPin = Knot->GetOutputPin();
	if (InputPin == nullptr || OutputPin == nullptr) { return false; }

	FVector2D AverageLeft;
	FVector2D AverageRight;
	FVector2D Center;
	const bool bLeftValid = TryAverageConnectedPinAnchor(*InputPin, PinRecords, Positions, AverageLeft);
	const bool bRightValid = TryAverageConnectedPinAnchor(*OutputPin, PinRecords, Positions, AverageRight);
	if (bLeftValid && bRightValid) { return AverageRight.X < AverageLeft.X; }
	if (!TryResolvePinAnchor(*OutputPin, PinRecords, Positions, Center)) { return false; }
	if (bLeftValid) { return Center.X < AverageLeft.X; }
	return bRightValid && AverageRight.X < Center.X;
}

[[nodiscard]]
bool PinsShareRenderedTerminal(const UEdGraphPin* First, const UEdGraphPin* Second)
{
	if (First == nullptr || Second == nullptr) { return false; }
	if (First == Second) { return true; }
	UEdGraphNode* FirstNode = First->GetOwningNodeUnchecked();
	return FirstNode != nullptr && FirstNode == Second->GetOwningNodeUnchecked() && FirstNode->IsA<UK2Node_Knot>();
}

[[nodiscard]]
bool IsPinAlignedRerouteTerminalContact(
	const FAdapterNodeRecord& First,
	const FAdapterNodeRecord& Second,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions
)
{
	const FAdapterNodeRecord* KnotRecord = First.Node != nullptr && First.Node->IsA<UK2Node_Knot>() ? &First : nullptr;
	const FAdapterNodeRecord* SemanticRecord = KnotRecord == &First ? &Second : nullptr;
	if (KnotRecord == nullptr && Second.Node != nullptr && Second.Node->IsA<UK2Node_Knot>())
	{
		KnotRecord = &Second;
		SemanticRecord = &First;
	}
	if (KnotRecord == nullptr || SemanticRecord == nullptr || SemanticRecord->Node == nullptr
		|| SemanticRecord->Node->IsA<UK2Node_Knot>())
	{
		return false;
	}

	const FVector2D* KnotPosition = Positions.Find(KnotRecord->Node);
	if (KnotPosition == nullptr) { return false; }
	const double KnotCenterX = KnotPosition->X + RerouteKnotWidth * 0.5;
	for (const UEdGraphPin* KnotPin : KnotRecord->Node->Pins)
	{
		if (KnotPin == nullptr) { continue; }
		for (const UEdGraphPin* LinkedPin : KnotPin->LinkedTo)
		{
			if (LinkedPin == nullptr || LinkedPin->GetOwningNodeUnchecked() != SemanticRecord->Node
				|| LinkedPin->Direction == KnotPin->Direction)
			{
				continue;
			}
			FVector2D SemanticAnchor;
			if (TryResolvePinAnchor(*LinkedPin, PinRecords, Positions, SemanticAnchor)
				&& FMath::IsNearlyEqual(KnotCenterX, SemanticAnchor.X, ReadabilityEpsilon))
			{
				// A reroute centered on a directly connected pin necessarily straddles the node
				// boundary by half of its tiny visual box. This is the intended terminal tangent,
				// not a knot buried under an unrelated node.
				return true;
			}
		}
	}
	return false;
}

void MeasureNodeReadability(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	FReadabilityWorkBudget& WorkBudget,
	FReadabilityMetrics& OutMetrics
)
{
	for (int32 FirstIndex = 0; FirstIndex < Nodes.Num(); ++FirstIndex)
	{
		const FVector2D* FirstPosition = Positions.Find(Nodes[FirstIndex].Node);
		if (FirstPosition == nullptr) { continue; }
		const FBox2D FirstBounds = MakeNodeBox(*FirstPosition, Nodes[FirstIndex].Size);
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Nodes.Num(); ++SecondIndex)
		{
			const FVector2D* SecondPosition = Positions.Find(Nodes[SecondIndex].Node);
			if (SecondPosition == nullptr) { continue; }
			if (!WorkBudget.TryConsume(1))
			{
				OutMetrics.bWorkBudgetExhausted = true;
				return;
			}
			const FBox2D SecondBounds = MakeNodeBox(*SecondPosition, Nodes[SecondIndex].Size);
			if (HasPositiveAreaIntersection(FirstBounds, SecondBounds)
				&& !IsPinAlignedRerouteTerminalContact(Nodes[FirstIndex], Nodes[SecondIndex], PinRecords, Positions))
			{
				++OutMetrics.OverlapPairCount;
				OutMetrics.OverlapPairs.Add(Nodes[FirstIndex].Key + TEXT("|") + Nodes[SecondIndex].Key);
			}
		}
	}
}

[[nodiscard]]
bool SegmentIntersectsBoxInterior(const FVector2D Start, const FVector2D End, const FBox2D& Box)
{
	// Shrink the rectangle so a wire merely touching a node's border or terminating at a pin
	// does not count as travelling underneath that node.
	const FVector2D InteriorMin = Box.Min + FVector2D(ReadabilityEpsilon, ReadabilityEpsilon);
	const FVector2D InteriorMax = Box.Max - FVector2D(ReadabilityEpsilon, ReadabilityEpsilon);
	if (InteriorMin.X >= InteriorMax.X || InteriorMin.Y >= InteriorMax.Y) { return false; }

	double MinimumTime = 0.0;
	double MaximumTime = 1.0;
	const FVector2D Delta = End - Start;
	const auto ClipAxis =
		[&MinimumTime, &MaximumTime](
			const double StartValue, const double DeltaValue, const double MinimumValue, const double MaximumValue
		)
	{
		if (FMath::Abs(DeltaValue) <= UE_DOUBLE_SMALL_NUMBER)
		{
			return StartValue >= MinimumValue && StartValue <= MaximumValue;
		}
		double EntryTime = (MinimumValue - StartValue) / DeltaValue;
		double ExitTime = (MaximumValue - StartValue) / DeltaValue;
		if (EntryTime > ExitTime) { Swap(EntryTime, ExitTime); }
		MinimumTime = FMath::Max(MinimumTime, EntryTime);
		MaximumTime = FMath::Min(MaximumTime, ExitTime);
		return MinimumTime <= MaximumTime;
	};

	return ClipAxis(Start.X, Delta.X, InteriorMin.X, InteriorMax.X)
		&& ClipAxis(Start.Y, Delta.Y, InteriorMin.Y, InteriorMax.Y);
}

[[nodiscard]]
int32 CountRenderedPolylineIntersections(
	TConstArrayView<FVector2D> First, TConstArrayView<FVector2D> Second, TConstArrayView<FVector2D> IgnoredSharedTerminals
)
{
	int32 Count = 0;
	for (int32 FirstSegment = 1; FirstSegment < First.Num(); ++FirstSegment)
	{
		const TConstArrayView<FVector2D> FirstPair(First.GetData() + FirstSegment - 1, 2);
		for (int32 SecondSegment = 1; SecondSegment < Second.Num(); ++SecondSegment)
		{
			const TConstArrayView<FVector2D> SecondPair(Second.GetData() + SecondSegment - 1, 2);
			Count += FK2RerouteRouter::RenderedPolylinesIntersectExceptAtSharedTerminals(
						 FirstPair, SecondPair, IgnoredSharedTerminals
					 )
					   ? 1
					   : 0;
		}
	}
	return Count;
}

void MeasureWireReadability(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	TConstArrayView<FAdapterEdgeRecord> Edges,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	const double MinimumPreferredExecutionGap,
	const bool bUsePlannedRoutes,
	FReadabilityWorkBudget& WorkBudget,
	FReadabilityMetrics& OutMetrics
)
{
	TArray<FMeasuredWirePath> WirePaths;
	WirePaths.Reserve(Edges.Num());
	for (const FAdapterEdgeRecord& Edge : Edges)
	{
		FVector2D OutputAnchor;
		FVector2D InputAnchor;
		if (!TryResolveEdgeAnchors(Edge, PinRecords, Positions, OutputAnchor, InputAnchor)) { continue; }

		TArray<FVector2D> LogicalPoints;
		LogicalPoints.Add(OutputAnchor);
		if (bUsePlannedRoutes && !Edge.PlannedRouteWaypoints.IsEmpty())
		{
			LogicalPoints.Append(Edge.PlannedRouteWaypoints);
		}
		else if (Edge.bExistingGeneratedRoute) { LogicalPoints.Append(Edge.ExistingRouteWaypoints); }
		LogicalPoints.Add(InputAnchor);
		FMeasuredWirePath& Path = WirePaths.AddDefaulted_GetRef();
		Path.Key = Edge.Key;
		Path.OutputNode = Edge.OutputPin->GetOwningNodeUnchecked();
		Path.InputNode = Edge.InputPin->GetOwningNodeUnchecked();
		Path.OutputPin = Edge.OutputPin;
		Path.InputPin = Edge.InputPin;
		Path.bExecution = Edge.bExecution;
		Path.RenderedPoints = FK2RerouteRouter::BuildRenderedPolyline(
			LogicalPoints,
			ShouldReverseKnotTangent(Path.OutputNode, PinRecords, Positions),
			ShouldReverseKnotTangent(Path.InputNode, PinRecords, Positions)
		);
		if (!WorkBudget.TryConsume(Path.RenderedPoints.Num()))
		{
			OutMetrics.bWorkBudgetExhausted = true;
			return;
		}
		for (const FVector2D Point : Path.RenderedPoints)
		{
			Path.RenderedBounds += Point;
		}

		const double BackwardDistance = FMath::Max(0.0, OutputAnchor.X - InputAnchor.X);
		if (Edge.bExecution)
		{
			OutMetrics.BackwardExecutionDistance += BackwardDistance;
			if (BackwardDistance > ReadabilityEpsilon)
			{
				++OutMetrics.BackwardExecutionEdgeCount;
				OutMetrics.BackwardExecutionEdges.Add(Edge.Key);
				OutMetrics.BackwardExecutionDistances.Add(Edge.Key, BackwardDistance);
			}
			if (Edge.bPreferredAlignment)
			{
				const double VerticalError = FMath::Abs(OutputAnchor.Y - InputAnchor.Y);
				OutMetrics.PreferredExecutionVerticalError += VerticalError;
				if (VerticalError > ReadabilityEpsilon)
				{
					++OutMetrics.NonStraightPreferredExecutionEdgeCount;
					OutMetrics.NonStraightPreferredExecutionEdges.Add(Edge.Key);
				}
				const double HorizontalGap = InputAnchor.X - OutputAnchor.X;
				if (VerticalError <= ReadabilityEpsilon && HorizontalGap >= -ReadabilityEpsilon
					&& HorizontalGap < MinimumPreferredExecutionGap - ReadabilityEpsilon)
				{
					OutMetrics.InsufficientPreferredExecutionGaps.Add(Edge.Key);
				}
			}
		}
		else
		{
			OutMetrics.BackwardDataDistance += BackwardDistance;
			OutMetrics.BackwardDataEdgeCount += BackwardDistance > ReadabilityEpsilon ? 1 : 0;
		}
	}

	for (const FMeasuredWirePath& Path : WirePaths)
	{
		for (const FAdapterNodeRecord& Node : Nodes)
		{
			if (Node.Node == Path.OutputNode || Node.Node == Path.InputNode) { continue; }
			const FVector2D* Position = Positions.Find(Node.Node);
			if (Position == nullptr) { continue; }
			const FBox2D NodeBounds = MakeNodeBox(*Position, Node.Size);
			const bool bBoundsSeparated = Path.RenderedBounds.Max.X <= NodeBounds.Min.X
									   || NodeBounds.Max.X <= Path.RenderedBounds.Min.X
									   || Path.RenderedBounds.Max.Y <= NodeBounds.Min.Y
									   || NodeBounds.Max.Y <= Path.RenderedBounds.Min.Y;
			if (bBoundsSeparated) { continue; }
			for (int32 PointIndex = 1; PointIndex < Path.RenderedPoints.Num(); ++PointIndex)
			{
				if (!WorkBudget.TryConsume(1))
				{
					OutMetrics.bWorkBudgetExhausted = true;
					return;
				}
				if (SegmentIntersectsBoxInterior(
						Path.RenderedPoints[PointIndex - 1], Path.RenderedPoints[PointIndex], NodeBounds
					))
				{
					OutMetrics.WiresThroughNodes.Add(Path.Key + TEXT("|") + Node.Key);
					break;
				}
			}
		}
	}

	for (int32 FirstIndex = 0; FirstIndex < WirePaths.Num(); ++FirstIndex)
	{
		const FMeasuredWirePath& First = WirePaths[FirstIndex];
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < WirePaths.Num(); ++SecondIndex)
		{
			const FMeasuredWirePath& Second = WirePaths[SecondIndex];
			if (First.Key == Second.Key) { continue; }
			const bool bBoundsSeparated = First.RenderedBounds.Max.X < Second.RenderedBounds.Min.X
									   || Second.RenderedBounds.Max.X < First.RenderedBounds.Min.X
									   || First.RenderedBounds.Max.Y < Second.RenderedBounds.Min.Y
									   || Second.RenderedBounds.Max.Y < First.RenderedBounds.Min.Y;
			if (bBoundsSeparated) { continue; }
			const int64 FirstSegmentCount = FMath::Max(0, First.RenderedPoints.Num() - 1);
			const int64 SecondSegmentCount = FMath::Max(0, Second.RenderedPoints.Num() - 1);
			if (SecondSegmentCount > 0 && FirstSegmentCount > WorkBudget.Remaining / SecondSegmentCount)
			{
				OutMetrics.bWorkBudgetExhausted = true;
				return;
			}
			if (!WorkBudget.TryConsume(FirstSegmentCount * SecondSegmentCount))
			{
				OutMetrics.bWorkBudgetExhausted = true;
				return;
			}

			TArray<FVector2D, TInlineAllocator<2>> IgnoredSharedTerminals;
			const auto AddIgnoredTerminal = [&IgnoredSharedTerminals](const FVector2D& Terminal)
			{
				if (!IgnoredSharedTerminals.ContainsByPredicate([&Terminal](const FVector2D& Existing)
																{ return Existing.Equals(Terminal); }))
				{
					IgnoredSharedTerminals.Add(Terminal);
				}
			};
			if (!First.RenderedPoints.IsEmpty() && !Second.RenderedPoints.IsEmpty())
			{
				if (PinsShareRenderedTerminal(First.OutputPin, Second.OutputPin))
				{
					AddIgnoredTerminal(First.RenderedPoints[0]);
				}
				if (PinsShareRenderedTerminal(First.OutputPin, Second.InputPin))
				{
					AddIgnoredTerminal(First.RenderedPoints[0]);
				}
				if (PinsShareRenderedTerminal(First.InputPin, Second.OutputPin))
				{
					AddIgnoredTerminal(First.RenderedPoints.Last());
				}
				if (PinsShareRenderedTerminal(First.InputPin, Second.InputPin))
				{
					AddIgnoredTerminal(First.RenderedPoints.Last());
				}
			}
			const int32 IntersectionCount =
				CountRenderedPolylineIntersections(First.RenderedPoints, Second.RenderedPoints, IgnoredSharedTerminals);
			if (IntersectionCount == 0) { continue; }

			const bool bFirstKeyFirst = First.Key.Compare(Second.Key, ESearchCase::CaseSensitive) < 0;
			const FString PairKey = bFirstKeyFirst ? First.Key + TEXT("|") + Second.Key
												   : Second.Key + TEXT("|") + First.Key;
			if (First.bExecution || Second.bExecution)
			{
				OutMetrics.ExecutionWireCrossings.Add(PairKey);
				OutMetrics.ExecutionWireCrossingCount += IntersectionCount;
			}
			else
			{
				OutMetrics.DataWireCrossings.Add(PairKey);
				OutMetrics.DataWireCrossingCount += IntersectionCount;
			}
		}
	}
}

void MeasureExecutionRootMovement(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	const TMap<UEdGraphNode*, FVector2D>& OriginalPositions,
	const TMap<UEdGraphNode*, FVector2D>& CandidatePositions,
	const double LayoutCellSize,
	FReadabilityMetrics& OutMetrics
)
{
	TSet<UEdGraphNode*> ExecutionRoots;
	for (const FAdapterNodeRecord& Node : Nodes)
	{
		bool bHasExecutionOutput = false;
		bool bHasLinkedExecutionInput = false;
		for (const UEdGraphPin* Pin : Node.Node->Pins)
		{
			if (Pin == nullptr || !IsExecutionPin(*Pin)) { continue; }
			bHasExecutionOutput |= Pin->Direction == EGPD_Output;
			bHasLinkedExecutionInput |= Pin->Direction == EGPD_Input && !Pin->LinkedTo.IsEmpty();
		}
		if (bHasExecutionOutput && !bHasLinkedExecutionInput) { ExecutionRoots.Add(Node.Node); }
	}

	TArray<double> OriginalRootXs;
	OriginalRootXs.Reserve(ExecutionRoots.Num());
	for (UEdGraphNode* Root : ExecutionRoots)
	{
		const FVector2D* OriginalPosition = OriginalPositions.Find(Root);
		const FVector2D* CandidatePosition = CandidatePositions.Find(Root);
		if (OriginalPosition == nullptr || CandidatePosition == nullptr) { continue; }
		OriginalRootXs.Add(OriginalPosition->X);
		OutMetrics.MaximumExecutionRootHorizontalDrift = FMath::Max(
			OutMetrics.MaximumExecutionRootHorizontalDrift, FMath::Abs(OriginalPosition->X - CandidatePosition->X)
		);
		OutMetrics.MaximumExecutionRootVerticalDrift = FMath::Max(
			OutMetrics.MaximumExecutionRootVerticalDrift, FMath::Abs(OriginalPosition->Y - CandidatePosition->Y)
		);
	}
	if (!OriginalRootXs.IsEmpty())
	{
		OriginalRootXs.Sort();
		const int32 Middle = OriginalRootXs.Num() / 2;
		const double MedianRootX = OriginalRootXs.Num() % 2 == 0
									 ? (OriginalRootXs[Middle - 1] + OriginalRootXs[Middle]) * 0.5
									 : OriginalRootXs[Middle];
		const double SharedRootColumn = FMath::GridSnap(MedianRootX, LayoutCellSize);
		double OriginalMaximumDeviation = 0.0;
		double CandidateMaximumDeviation = 0.0;
		for (UEdGraphNode* Root : ExecutionRoots)
		{
			const FVector2D* OriginalPosition = OriginalPositions.Find(Root);
			const FVector2D* CandidatePosition = CandidatePositions.Find(Root);
			if (OriginalPosition == nullptr || CandidatePosition == nullptr) { continue; }
			OriginalMaximumDeviation =
				FMath::Max(OriginalMaximumDeviation, FMath::Abs(OriginalPosition->X - SharedRootColumn));
			CandidateMaximumDeviation =
				FMath::Max(CandidateMaximumDeviation, FMath::Abs(CandidatePosition->X - SharedRootColumn));
		}
		OutMetrics.ExecutionRootColumnDeviationIncrease = CandidateMaximumDeviation - OriginalMaximumDeviation;
	}

	const TArray<UEdGraphNode*> OrderedExecutionRoots = ExecutionRoots.Array();
	for (int32 FirstIndex = 0; FirstIndex < OrderedExecutionRoots.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < OrderedExecutionRoots.Num(); ++SecondIndex)
		{
			const double OriginalDelta = OriginalPositions.FindChecked(OrderedExecutionRoots[FirstIndex]).Y
									   - OriginalPositions.FindChecked(OrderedExecutionRoots[SecondIndex]).Y;
			const double CandidateDelta = CandidatePositions.FindChecked(OrderedExecutionRoots[FirstIndex]).Y
										- CandidatePositions.FindChecked(OrderedExecutionRoots[SecondIndex]).Y;
			if (FMath::Abs(OriginalDelta) > ReadabilityEpsilon && OriginalDelta * CandidateDelta < -ReadabilityEpsilon)
			{
				++OutMetrics.ExecutionRootOrderInversionCount;
			}
		}
	}
}

[[nodiscard]]
FReadabilityMetrics MeasureReadability(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	TConstArrayView<FAdapterEdgeRecord> Edges,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
	const int32 WorkBudget,
	const double MinimumPreferredExecutionGap,
	const bool bUsePlannedRoutes = false
)
{
	FReadabilityMetrics Metrics;
	FReadabilityWorkBudget RemainingWork(WorkBudget);
	MeasureNodeReadability(Nodes, PinRecords, Positions, RemainingWork, Metrics);
	if (!Metrics.bWorkBudgetExhausted)
	{
		MeasureWireReadability(
			Nodes, Edges, PinRecords, Positions, MinimumPreferredExecutionGap, bUsePlannedRoutes, RemainingWork, Metrics
		);
	}
	return Metrics;
}

[[nodiscard]]
bool IsMateriallyGreater(const double Candidate, const double Original, const double AbsoluteAllowance, const double Ratio)
{ return Candidate > Original * Ratio + AbsoluteAllowance; }

[[nodiscard]]
bool ContainsNewKey(const TSet<FString>& Candidate, const TSet<FString>& Original)
{
	for (const FString& Key : Candidate)
	{
		if (!Original.Contains(Key)) { return true; }
	}
	return false;
}

[[nodiscard]]
FString FindFirstNewKey(const TSet<FString>& Candidate, const TSet<FString>& Original)
{
	TArray<FString> NewKeys;
	for (const FString& Key : Candidate)
	{
		if (!Original.Contains(Key)) { NewKeys.Add(Key); }
	}
	NewKeys.Sort();
	return NewKeys.IsEmpty() ? FString() : NewKeys[0];
}

[[nodiscard]]
FString DescribeEdge(const FAdapterEdgeRecord& Edge)
{
	const UEdGraphNode* OutputNode = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
	const UEdGraphNode* InputNode = Edge.InputPin != nullptr ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
	return FString::Printf(
		TEXT("%s.%s -> %s.%s"),
		OutputNode != nullptr ? *OutputNode->GetName() : TEXT("<missing>"),
		Edge.OutputPin != nullptr ? *Edge.OutputPin->PinName.ToString() : TEXT("<missing>"),
		InputNode != nullptr ? *InputNode->GetName() : TEXT("<missing>"),
		Edge.InputPin != nullptr ? *Edge.InputPin->PinName.ToString() : TEXT("<missing>")
	);
}

[[nodiscard]]
FString DescribeCrossingPair(const FString& PairKey, TConstArrayView<FAdapterEdgeRecord> Edges)
{
	FString FirstKey;
	FString SecondKey;
	if (!PairKey.Split(TEXT("|"), &FirstKey, &SecondKey)) { return PairKey; }
	const FAdapterEdgeRecord* First =
		Edges.FindByPredicate([&FirstKey](const FAdapterEdgeRecord& Edge) { return Edge.Key == FirstKey; });
	const FAdapterEdgeRecord* Second =
		Edges.FindByPredicate([&SecondKey](const FAdapterEdgeRecord& Edge) { return Edge.Key == SecondKey; });
	if (First == nullptr || Second == nullptr) { return PairKey; }
	return FString::Printf(TEXT("'%s' crosses '%s'"), *DescribeEdge(*First), *DescribeEdge(*Second));
}

[[nodiscard]]
FString FindHardReadabilityRegression(
	const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const double LayoutCellSize
)
{
	// A row-wrapped execution chain can trade one straight incoming gap for a longer backward
	// continuation. Its source can advance by one spacing cell while both endpoints snap by up to
	// half a cell in opposite directions, so the bounded local correction is at most two cells.
	const double LocalSpacingCorrection = LayoutCellSize * 2.0;
	if (ContainsNewKey(Candidate.OverlapPairs, Original.OverlapPairs))
	{
		return FString::Printf(
			TEXT("the candidate creates a new node overlap (%s)"),
			*FindFirstNewKey(Candidate.OverlapPairs, Original.OverlapPairs)
		);
	}
	if (ContainsNewKey(Candidate.BackwardExecutionEdges, Original.BackwardExecutionEdges))
	{
		return TEXT("the candidate makes execution flow run farther backward");
	}
	for (const TPair<FString, double>& CandidateDistance : Candidate.BackwardExecutionDistances)
	{
		const double* OriginalDistance = Original.BackwardExecutionDistances.Find(CandidateDistance.Key);
		if (OriginalDistance != nullptr && CandidateDistance.Value > *OriginalDistance + LocalSpacingCorrection)
		{
			return TEXT("the candidate makes execution flow run farther backward");
		}
	}
	if (ContainsNewKey(Candidate.NonStraightPreferredExecutionEdges, Original.NonStraightPreferredExecutionEdges)
		|| Candidate.PreferredExecutionVerticalError > Original.PreferredExecutionVerticalError + LayoutCellSize)
	{
		return TEXT("the candidate bends a preferred straight execution connection");
	}
	if (ContainsNewKey(Candidate.InsufficientPreferredExecutionGaps, Original.InsufficientPreferredExecutionGaps))
	{
		return TEXT("the candidate creates a sub-cell gap on a straight execution connection");
	}
	if (Candidate.ExecutionWireCrossings.Num() > Original.ExecutionWireCrossings.Num()
		|| Candidate.ExecutionWireCrossingCount > Original.ExecutionWireCrossingCount)
	{
		return TEXT("the candidate increases execution-wire crossing pairs or multiplicity");
	}
	return FString();
}

[[nodiscard]]
FString FindWireThroughNodeRegression(
	const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const bool bAllowRerouteTradeoffs
)
{
	if (ContainsNewKey(Candidate.WiresThroughNodes, Original.WiresThroughNodes))
	{
		const int32 AllowedIncrease = bAllowRerouteTradeoffs ? 1 : 0;
		if (Candidate.WiresThroughNodes.Num() > Original.WiresThroughNodes.Num() + AllowedIncrease)
		{
			return FString::Printf(
				TEXT("the candidate makes a wire pass through a new node (%s)"),
				*FindFirstNewKey(Candidate.WiresThroughNodes, Original.WiresThroughNodes)
			);
		}
	}
	return FString();
}

[[nodiscard]]
FString FindBackwardDataRegression(const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const double LayoutCellSize)
{
	const int32 AllowedBackwardDataIncrease = FMath::Max(1, Original.BackwardDataEdgeCount / 4);
	if (Candidate.BackwardDataEdgeCount > Original.BackwardDataEdgeCount + AllowedBackwardDataIncrease
		|| IsMateriallyGreater(
			Candidate.BackwardDataDistance, Original.BackwardDataDistance, LayoutCellSize * 2.0, MaterialReadabilityRatio
		))
	{
		const int32 OriginalStructuralDefects = Original.OverlapPairCount + Original.ExecutionWireCrossingCount
											  + Original.WiresThroughNodes.Num();
		const int32 CandidateStructuralDefects = Candidate.OverlapPairCount + Candidate.ExecutionWireCrossingCount
											   + Candidate.WiresThroughNodes.Num();
		// Generated/LLM graphs can begin with nearly every node stacked at one coordinate. In that
		// state a canonical left-to-right redraw may lengthen some data returns while eliminating
		// hundreds of overlaps, execution crossings, and wires under nodes. Treat a 50% aggregate
		// structural cleanup as the dominant readability win; ordinary authored graphs keep the
		// conservative backward-data gate.
		if (OriginalStructuralDefects < 8 || CandidateStructuralDefects * 2 > OriginalStructuralDefects)
		{
			return TEXT("the candidate introduces materially worse backward data wiring");
		}
	}
	return FString();
}

[[nodiscard]]
FString FindDataCrossingRegression(const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate)
{
	if (Candidate.DataWireCrossingCount > Original.DataWireCrossingCount)
	{
		return TEXT("the candidate increases total data-wire crossing multiplicity");
	}
	return FString();
}

[[nodiscard]]
FString FindRootPreservationRegression(const FReadabilityMetrics& Candidate)
{
	if (Candidate.ExecutionRootColumnDeviationIncrease > ReadabilityEpsilon)
	{
		return TEXT("the candidate moves execution graph starts farther from their shared major-grid column");
	}
	if (Candidate.ExecutionRootOrderInversionCount > 0)
	{
		return TEXT("the candidate changes the authored top-to-bottom order of execution graph starts");
	}
	return FString();
}

[[nodiscard]]
bool HasMaterialReadabilityImprovement(
	const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const double LayoutCellSize
)
{
	return Candidate.OverlapPairCount < Original.OverlapPairCount
		|| Candidate.BackwardExecutionEdgeCount < Original.BackwardExecutionEdgeCount
		|| Candidate.BackwardExecutionDistance + LayoutCellSize < Original.BackwardExecutionDistance
		|| Candidate.NonStraightPreferredExecutionEdgeCount < Original.NonStraightPreferredExecutionEdgeCount
		|| Candidate.PreferredExecutionVerticalError + LayoutCellSize < Original.PreferredExecutionVerticalError
		|| Candidate.InsufficientPreferredExecutionGaps.Num() < Original.InsufficientPreferredExecutionGaps.Num()
		|| Candidate.BackwardDataEdgeCount < Original.BackwardDataEdgeCount
		|| Candidate.BackwardDataDistance + LayoutCellSize < Original.BackwardDataDistance
		|| Candidate.WiresThroughNodes.Num() < Original.WiresThroughNodes.Num()
		|| Candidate.ExecutionWireCrossingCount < Original.ExecutionWireCrossingCount
		|| Candidate.DataWireCrossingCount < Original.DataWireCrossingCount;
}

[[nodiscard]]
FString FindAuthoredMovementRegression(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	const TMap<UEdGraphNode*, FVector2D>& CandidatePositions,
	const FReadabilityMetrics& OriginalReadability,
	const FReadabilityMetrics& CandidateReadability,
	const double LayoutCellSize
)
{
	TSet<UEdGraphNode*> AuthoredNodeSet;
	TMap<UEdGraphNode*, const FAdapterNodeRecord*> AuthoredRecords;
	TArray<UEdGraphNode*> ExecutionRoots;
	TArray<double> ExecutionRootXs;
	for (const FAdapterNodeRecord& Node : Nodes)
	{
		if (Node.Node == nullptr) { continue; }
		AuthoredNodeSet.Add(Node.Node);
		AuthoredRecords.Add(Node.Node, &Node);
		bool bHasExecutionOutput = false;
		bool bHasLinkedExecutionInput = false;
		for (const UEdGraphPin* Pin : Node.Node->Pins)
		{
			if (Pin == nullptr || !IsExecutionPin(*Pin)) { continue; }
			bHasExecutionOutput |= Pin->Direction == EGPD_Output;
			bHasLinkedExecutionInput |= Pin->Direction == EGPD_Input && !Pin->LinkedTo.IsEmpty();
		}
		if (bHasExecutionOutput && !bHasLinkedExecutionInput)
		{
			ExecutionRoots.Add(Node.Node);
			ExecutionRootXs.Add(Node.OriginalPosition.X);
		}
	}

	TMap<UEdGraphNode*, double> ExpectedHorizontalTranslations;
	if (!ExecutionRootXs.IsEmpty())
	{
		ExecutionRootXs.Sort();
		const int32 Middle = ExecutionRootXs.Num() / 2;
		const double MedianRootX = ExecutionRootXs.Num() % 2 == 0
									 ? (ExecutionRootXs[Middle - 1] + ExecutionRootXs[Middle]) * 0.5
									 : ExecutionRootXs[Middle];
		const double SharedRootColumn = FMath::GridSnap(MedianRootX, LayoutCellSize);
		for (UEdGraphNode* Root : ExecutionRoots)
		{
			const FAdapterNodeRecord* const* RootRecord = AuthoredRecords.Find(Root);
			if (RootRecord == nullptr) { continue; }
			const double Translation = SharedRootColumn - (*RootRecord)->OriginalPosition.X;
			const FVector2D* CandidateRootPosition = CandidatePositions.Find(Root);
			if (CandidateRootPosition == nullptr
				|| !FMath::IsNearlyEqual(
					CandidateRootPosition->X - (*RootRecord)->OriginalPosition.X, Translation, ReadabilityEpsilon
				))
			{
				// A paragraph receives the normalization only when its root actually made the
				// canonical column translation. Otherwise a downstream-only shift could masquerade
				// as harmless whole-paragraph movement and repeat on every invocation.
				continue;
			}
			TArray<UEdGraphNode*> Pending{ Root };
			for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
			{
				UEdGraphNode* Current = Pending[PendingIndex];
				if (Current == nullptr || ExpectedHorizontalTranslations.Contains(Current)) { continue; }
				ExpectedHorizontalTranslations.Add(Current, Translation);
				for (const UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin == nullptr || !IsExecutionPin(*Pin)) { continue; }
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin != nullptr ? LinkedPin->GetOwningNodeUnchecked() : nullptr;
						if (LinkedNode != nullptr && AuthoredNodeSet.Contains(LinkedNode))
						{
							Pending.AddUnique(LinkedNode);
						}
					}
				}
			}
		}

		// Pure provider trees visually belong to their execution consumer. Extend the component
		// translation only when every already-assigned neighbour agrees, so a shared provider that
		// bridges two event islands is never dragged arbitrarily with one of them.
		for (int32 Pass = 0; Pass < Nodes.Num(); ++Pass)
		{
			bool bAssignedInPass = false;
			for (const FAdapterNodeRecord& Node : Nodes)
			{
				if (Node.Node == nullptr || ExpectedHorizontalTranslations.Contains(Node.Node)) { continue; }
				bool bHasExecutionPort = false;
				TOptional<double> AgreedTranslation;
				bool bAmbiguous = false;
				for (const UEdGraphPin* Pin : Node.Node->Pins)
				{
					if (Pin == nullptr) { continue; }
					bHasExecutionPort |= IsExecutionPin(*Pin);
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = LinkedPin != nullptr ? LinkedPin->GetOwningNodeUnchecked() : nullptr;
						const double* LinkedTranslation = ExpectedHorizontalTranslations.Find(LinkedNode);
						if (LinkedTranslation == nullptr) { continue; }
						if (!AgreedTranslation.IsSet()) { AgreedTranslation = *LinkedTranslation; }
						else if (!FMath::IsNearlyEqual(AgreedTranslation.GetValue(), *LinkedTranslation, ReadabilityEpsilon))
						{
							bAmbiguous = true;
						}
					}
				}
				if (!bHasExecutionPort && AgreedTranslation.IsSet() && !bAmbiguous)
				{
					ExpectedHorizontalTranslations.Add(Node.Node, AgreedTranslation.GetValue());
					bAssignedInPass = true;
				}
			}
			if (!bAssignedInPass) { break; }
		}
	}

	int32 LargeMovementCount = 0;
	double MaximumNormalizedMovement = 0.0;
	const UEdGraphNode* FurthestMovedNode = nullptr;
	// A locally authored node can need one full execution-spacing column plus up to half a
	// coarse-grid cell of snap correction. Treat that combined adjustment as local; otherwise a
	// valid 620 -> 768 correction is postponed until a second formatter invocation.
	const double LocalMovementRadius = LayoutCellSize * 1.5;
	for (const FAdapterNodeRecord& Node : Nodes)
	{
		const FVector2D* CandidatePosition = CandidatePositions.Find(Node.Node);
		FVector2D TranslationNormalizedPosition = CandidatePosition != nullptr ? *CandidatePosition : Node.OriginalPosition;
		if (const double* Translation = ExpectedHorizontalTranslations.Find(Node.Node))
		{
			TranslationNormalizedPosition.X -= *Translation;
		}
		const double NormalizedMovement = CandidatePosition != nullptr
											? FVector2D::Distance(Node.OriginalPosition, TranslationNormalizedPosition)
											: 0.0;
		if (NormalizedMovement > MaximumNormalizedMovement)
		{
			MaximumNormalizedMovement = NormalizedMovement;
			FurthestMovedNode = Node.Node;
		}
		if (NormalizedMovement > LocalMovementRadius + ReadabilityEpsilon) { ++LargeMovementCount; }
	}
	if (LargeMovementCount == 0) { return FString(); }

	const bool bReadabilityImproved =
		HasMaterialReadabilityImprovement(OriginalReadability, CandidateReadability, LayoutCellSize);
	const int32 OriginalStructuralDefectCount = OriginalReadability.OverlapPairCount
											  + OriginalReadability.ExecutionWireCrossingCount
											  + OriginalReadability.WiresThroughNodes.Num();
	const bool bDenseCrossingField = OriginalReadability.ExecutionWireCrossingCount * 4 > Nodes.Num()
								  && OriginalReadability.WiresThroughNodes.Num() * 4 > Nodes.Num()
								  && OriginalStructuralDefectCount * 4 >= Nodes.Num() * 3;
	const bool bSeverelyTangled = OriginalReadability.OverlapPairCount > Nodes.Num() * 2
							   || OriginalReadability.ExecutionWireCrossingCount > Nodes.Num() * 2
							   || OriginalReadability.WiresThroughNodes.Num() > Nodes.Num() * 2 || bDenseCrossingField;
	// Preserve mode must not let a small tail of an otherwise hand-authored graph "walk" several
	// columns on every invocation merely because earlier safe moves made the next rank admissible.
	// Whole-paragraph root normalization is removed above, and severely tangled generated graphs
	// remain eligible for a complete reflow.
	const double MaximumAuthoredMovementRadius = LayoutCellSize * 12.0;
	if (!bSeverelyTangled && MaximumNormalizedMovement > MaximumAuthoredMovementRadius + ReadabilityEpsilon)
	{
		return FString::Printf(
			TEXT("the candidate moves authored node '%s' %.0f units beyond its paragraph translation (maximum %.0f)"),
			FurthestMovedNode != nullptr ? *FurthestMovedNode->GetName() : TEXT("<unknown>"),
			MaximumNormalizedMovement,
			MaximumAuthoredMovementRadius
		);
	}
	// Preserve mode is deliberately conservative. A small graph may move up to three helper nodes
	// when that straightens execution or removes a concrete readability defect. Larger authored
	// paragraphs must retain most of their mental map; a broad canonical redraw belongs to Reflow.
	// A severely tangled generated graph has no useful spatial mental map to preserve, so allow the
	// complete deterministic cleanup when the ordinary readability gates confirm a real gain.
	const int32 AllowedLargeMovementCount =
		bSeverelyTangled
			? Nodes.Num()
			: (bReadabilityImproved ? FMath::Max(3, FMath::CeilToInt(static_cast<double>(Nodes.Num()) * 0.65))
									: FMath::Max(1, FMath::FloorToInt(static_cast<double>(Nodes.Num()) * 0.35)));
	if (LargeMovementCount > AllowedLargeMovementCount)
	{
		return FString::Printf(
			TEXT("the candidate moves %d of %d authored nodes beyond the local %.0f-unit movement radius%s"),
			LargeMovementCount,
			Nodes.Num(),
			LocalMovementRadius,
			bReadabilityImproved ? TEXT(" despite its readability gain") : TEXT(" without a material readability gain")
		);
	}
	return FString();
}

[[nodiscard]]
FString FindReadabilityRegression(
	const FReadabilityMetrics& Original,
	const FReadabilityMetrics& Candidate,
	const double LayoutCellSize,
	const bool bPreserveHumanLayout,
	const bool bAllowRerouteTradeoffs = false
)
{
	if (Original.bWorkBudgetExhausted || Candidate.bWorkBudgetExhausted)
	{
		return TEXT("the graph-wide readability safety check exhausted its deterministic work budget");
	}
	if (FString Reason = FindHardReadabilityRegression(Original, Candidate, LayoutCellSize); !Reason.IsEmpty())
	{
		return Reason;
	}
	// Routing is best-effort and can be skipped when no safe path exists or its work budget is
	// exhausted. Never accept a layout that depends on a later routing attempt for node clearance.
	if (FString Reason = FindWireThroughNodeRegression(Original, Candidate, bAllowRerouteTradeoffs); !Reason.IsEmpty())
	{
		return Reason;
	}
	if (FString Reason = FindBackwardDataRegression(Original, Candidate, LayoutCellSize); !Reason.IsEmpty())
	{
		return Reason;
	}
	if (FString Reason = FindDataCrossingRegression(Original, Candidate); !Reason.IsEmpty()) { return Reason; }
	if (bPreserveHumanLayout)
	{
		if (FString Reason = FindRootPreservationRegression(Candidate); !Reason.IsEmpty()) { return Reason; }
	}
	return FString();
}

struct FReadabilityEvaluationContext
{
	TConstArrayView<FAdapterNodeRecord> ReadabilityNodes;
	TConstArrayView<FAdapterNodeRecord> AuthoredNodes;
	TConstArrayView<FAdapterEdgeRecord> Edges;
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords;
	const TMap<UEdGraphNode*, FVector2D>& OriginalPositions;
	const FReadabilityMetrics& OriginalReadability;
	int32 WorkBudget = 0;
	double LayoutCellSize = 1.0;
	bool bPreserveHumanLayout = false;
};

struct FCandidateReadabilityEvaluation
{
	FReadabilityMetrics Metrics;
	FString Regression;
};

[[nodiscard]]
FCandidateReadabilityEvaluation EvaluateReadabilityCandidate(
	const FReadabilityEvaluationContext& Context,
	const TMap<UEdGraphNode*, FVector2D>& CandidatePositions,
	const bool bUsePlannedRoutes,
	const bool bAllowRerouteTradeoffs = false
)
{
	FCandidateReadabilityEvaluation Evaluation;
	Evaluation.Metrics = MeasureReadability(
		Context.ReadabilityNodes,
		Context.Edges,
		Context.PinRecords,
		CandidatePositions,
		Context.WorkBudget,
		Context.LayoutCellSize,
		bUsePlannedRoutes
	);
	if (!Context.OriginalReadability.bWorkBudgetExhausted && !Evaluation.Metrics.bWorkBudgetExhausted)
	{
		MeasureExecutionRootMovement(
			Context.ReadabilityNodes, Context.OriginalPositions, CandidatePositions, Context.LayoutCellSize, Evaluation.Metrics
		);
	}
	Evaluation.Regression = FindReadabilityRegression(
		Context.OriginalReadability, Evaluation.Metrics, Context.LayoutCellSize, Context.bPreserveHumanLayout, bAllowRerouteTradeoffs
	);
	if (Evaluation.Regression.IsEmpty() && Context.bPreserveHumanLayout)
	{
		Evaluation.Regression = FindAuthoredMovementRegression(
			Context.AuthoredNodes, CandidatePositions, Context.OriginalReadability, Evaluation.Metrics, Context.LayoutCellSize
		);
	}
	return Evaluation;
}

struct FConservativeFallbackResult
{
	TMap<UEdGraphNode*, FVector2D> Positions;
	FReadabilityMetrics Readability;
	TArray<FString> Diagnostics;
	int32 MovedNodeCount = 0;
};

[[nodiscard]]
TMap<UEdGraphNode*, FVector2D> BuildAuthoredRerouteColumnAlignments(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	TConstArrayView<FAdapterEdgeRecord> Edges,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions
)
{
	TMap<UEdGraphNode*, const FAdapterNodeRecord*> NodeRecords;
	TSet<UEdGraphNode*> Knots;
	for (const FAdapterNodeRecord& Node : Nodes)
	{
		NodeRecords.Add(Node.Node, &Node);
		if (Node.Node != nullptr && Node.Node->IsA<UK2Node_Knot>()) { Knots.Add(Node.Node); }
	}

	TMap<UEdGraphNode*, TArray<const FAdapterEdgeRecord*>> IncomingEdges;
	TMap<UEdGraphNode*, TArray<const FAdapterEdgeRecord*>> OutgoingEdges;
	TMap<UEdGraphNode*, TArray<UEdGraphNode*>> KnotPredecessors;
	TMap<UEdGraphNode*, TArray<UEdGraphNode*>> KnotSuccessors;
	for (const FAdapterEdgeRecord& Edge : Edges)
	{
		UEdGraphNode* Source = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
		UEdGraphNode* Target = Edge.InputPin != nullptr ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
		if (Knots.Contains(Source)) { OutgoingEdges.FindOrAdd(Source).Add(&Edge); }
		if (Knots.Contains(Target)) { IncomingEdges.FindOrAdd(Target).Add(&Edge); }
		if (Knots.Contains(Source) && Knots.Contains(Target))
		{
			KnotSuccessors.FindOrAdd(Source).AddUnique(Target);
			KnotPredecessors.FindOrAdd(Target).AddUnique(Source);
		}
	}

	const auto StableNodeLess = [&NodeRecords](const UEdGraphNode& Left, const UEdGraphNode& Right)
	{
		const FAdapterNodeRecord* const* LeftRecord = NodeRecords.Find(&Left);
		const FAdapterNodeRecord* const* RightRecord = NodeRecords.Find(&Right);
		const FString LeftKey = LeftRecord != nullptr ? (*LeftRecord)->Key : Left.GetName();
		const FString RightKey = RightRecord != nullptr ? (*RightRecord)->Key : Right.GetName();
		return LeftKey < RightKey;
	};
	for (TPair<UEdGraphNode*, TArray<UEdGraphNode*>>& Pair : KnotPredecessors)
	{
		Pair.Value.Sort([&StableNodeLess](const UEdGraphNode& Left, const UEdGraphNode& Right)
						{ return StableNodeLess(Left, Right); });
	}
	for (TPair<UEdGraphNode*, TArray<UEdGraphNode*>>& Pair : KnotSuccessors)
	{
		Pair.Value.Sort([&StableNodeLess](const UEdGraphNode& Left, const UEdGraphNode& Right)
						{ return StableNodeLess(Left, Right); });
	}
	const auto StableEdgeLess = [](const FAdapterEdgeRecord& Left, const FAdapterEdgeRecord& Right)
	{ return Left.Key < Right.Key; };
	for (TPair<UEdGraphNode*, TArray<const FAdapterEdgeRecord*>>& Pair : IncomingEdges)
	{
		Pair.Value.Sort([&StableEdgeLess](const FAdapterEdgeRecord& Left, const FAdapterEdgeRecord& Right)
						{ return StableEdgeLess(Left, Right); });
	}
	for (TPair<UEdGraphNode*, TArray<const FAdapterEdgeRecord*>>& Pair : OutgoingEdges)
	{
		Pair.Value.Sort([&StableEdgeLess](const FAdapterEdgeRecord& Left, const FAdapterEdgeRecord& Right)
						{ return StableEdgeLess(Left, Right); });
	}

	const auto ResolvePinX = [&PinRecords, &Positions](const UEdGraphPin* Pin, double& OutX)
	{
		if (Pin == nullptr) { return false; }
		UEdGraphNode* Owner = Pin->GetOwningNodeUnchecked();
		const FVector2D* Position = Positions.Find(Owner);
		const FAdapterPinRecord* Record = PinRecords.Find(Pin);
		if (Position == nullptr || Record == nullptr) { return false; }
		OutX = Position->X + Record->Offset.X;
		return true;
	};
	const auto ResolveOutputColumnX = [&NodeRecords, &PinRecords, &Positions](const UEdGraphNode& Node, double& OutX)
	{
		const FVector2D* Position = Positions.Find(&Node);
		if (Position == nullptr) { return false; }
		bool bFoundOutput = false;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output) { continue; }
			if (const FAdapterPinRecord* Record = PinRecords.Find(Pin))
			{
				OutX = bFoundOutput ? FMath::Max(OutX, Position->X + Record->Offset.X) : Position->X + Record->Offset.X;
				bFoundOutput = true;
			}
		}
		if (bFoundOutput) { return true; }
		const FAdapterNodeRecord* const* Record = NodeRecords.Find(&Node);
		if (Record == nullptr) { return false; }
		OutX = Position->X + (*Record)->Size.X;
		return true;
	};
	const auto FindSemanticTargetInputX = [&OutgoingEdges, &Knots, &ResolvePinX](UEdGraphNode& Knot, double& OutX)
	{
		const TArray<const FAdapterEdgeRecord*>* KnotEdges = OutgoingEdges.Find(&Knot);
		if (KnotEdges == nullptr) { return false; }
		bool bFound = false;
		for (const FAdapterEdgeRecord* Edge : *KnotEdges)
		{
			UEdGraphNode* Target = Edge != nullptr && Edge->InputPin != nullptr ? Edge->InputPin->GetOwningNodeUnchecked()
																				: nullptr;
			if (Target == nullptr || Knots.Contains(Target)) { continue; }
			double Candidate = 0.0;
			if (ResolvePinX(Edge->InputPin, Candidate))
			{
				OutX = bFound ? FMath::Min(OutX, Candidate) : Candidate;
				bFound = true;
			}
		}
		return bFound;
	};
	const auto FindSemanticTargetOutputX = [&OutgoingEdges, &Knots, &ResolveOutputColumnX](UEdGraphNode& Knot, double& OutX)
	{
		const TArray<const FAdapterEdgeRecord*>* KnotEdges = OutgoingEdges.Find(&Knot);
		if (KnotEdges == nullptr) { return false; }
		bool bFound = false;
		for (const FAdapterEdgeRecord* Edge : *KnotEdges)
		{
			UEdGraphNode* Target = Edge != nullptr && Edge->InputPin != nullptr ? Edge->InputPin->GetOwningNodeUnchecked()
																				: nullptr;
			if (Target == nullptr || Knots.Contains(Target)) { continue; }
			double Candidate = 0.0;
			if (ResolveOutputColumnX(*Target, Candidate))
			{
				OutX = bFound ? FMath::Max(OutX, Candidate) : Candidate;
				bFound = true;
			}
		}
		return bFound;
	};

	TArray<UEdGraphNode*> Starts;
	for (UEdGraphNode* Knot : Knots)
	{
		const TArray<UEdGraphNode*>* Predecessors = KnotPredecessors.Find(Knot);
		if (Predecessors == nullptr || Predecessors->IsEmpty()) { Starts.Add(Knot); }
	}
	Starts.Sort([&StableNodeLess](const UEdGraphNode& Left, const UEdGraphNode& Right)
				{ return StableNodeLess(Left, Right); });

	TSet<UEdGraphNode*> Visited;
	TMap<UEdGraphNode*, FVector2D> Result;
	for (UEdGraphNode* Start : Starts)
	{
		TArray<UEdGraphNode*> Chain;
		UEdGraphNode* Current = Start;
		while (Current != nullptr && !Visited.Contains(Current))
		{
			Visited.Add(Current);
			Chain.Add(Current);
			const TArray<UEdGraphNode*>* Successors = KnotSuccessors.Find(Current);
			if (Successors == nullptr || Successors->Num() != 1) { break; }
			UEdGraphNode* Next = (*Successors)[0];
			const TArray<UEdGraphNode*>* NextPredecessors = KnotPredecessors.Find(Next);
			if (NextPredecessors == nullptr || NextPredecessors->Num() != 1) { break; }
			Current = Next;
		}
		if (Chain.IsEmpty()) { continue; }

		bool bBranchingBus = false;
		for (int32 Index = 0; Index + 1 < Chain.Num(); ++Index)
		{
			double Unused = 0.0;
			bBranchingBus |= FindSemanticTargetInputX(*Chain[Index], Unused);
		}

		TMap<UEdGraphNode*, double> DesiredCenters;
		if (bBranchingBus)
		{
			double FirstInputX = 0.0;
			if (FindSemanticTargetInputX(*Chain[0], FirstInputX)) { DesiredCenters.Add(Chain[0], FirstInputX); }
			for (int32 Index = 1; Index < Chain.Num(); ++Index)
			{
				double PreviousOutputX = 0.0;
				if (FindSemanticTargetOutputX(*Chain[Index - 1], PreviousOutputX))
				{
					DesiredCenters.Add(Chain[Index], PreviousOutputX);
				}
			}
			// In a branching bus, every knot after the first is the outgoing continuation of the
			// semantic node in the preceding column. That rule also applies to the terminal knot:
			// snapping it to its eventual consumer would erase the final producer-column alignment.
		}
		else
		{
			const TArray<const FAdapterEdgeRecord*>* FirstIncoming = IncomingEdges.Find(Chain[0]);
			if (FirstIncoming != nullptr && Chain.Num() > 1)
			{
				for (const FAdapterEdgeRecord* Edge : *FirstIncoming)
				{
					UEdGraphNode* Source = Edge != nullptr && Edge->OutputPin != nullptr
											 ? Edge->OutputPin->GetOwningNodeUnchecked()
											 : nullptr;
					if (Source == nullptr || Knots.Contains(Source)) { continue; }
					double OutputX = 0.0;
					if (ResolvePinX(Edge->OutputPin, OutputX))
					{
						DesiredCenters.Add(Chain[0], OutputX);
						break;
					}
				}
			}
			double LastInputX = 0.0;
			if (FindSemanticTargetInputX(*Chain.Last(), LastInputX)) { DesiredCenters.Add(Chain.Last(), LastInputX); }
		}

		for (const TPair<UEdGraphNode*, double>& Alignment : DesiredCenters)
		{
			const FVector2D* Position = Positions.Find(Alignment.Key);
			if (Position == nullptr) { continue; }
			const FVector2D AlignedPosition(
				static_cast<double>(FMath::RoundToInt(Alignment.Value - RerouteKnotWidth * 0.5)), Position->Y
			);
			if (!Position->Equals(AlignedPosition, ReadabilityEpsilon)) { Result.Add(Alignment.Key, AlignedPosition); }
		}
	}
	return Result;
}

[[nodiscard]]
FConservativeFallbackResult BuildConservativeFallback(
	const FReadabilityEvaluationContext& ReadabilityContext,
	TConstArrayView<FAdapterEdgeRecord> LayoutEdges,
	const TMap<UEdGraphNode*, FVector2D>& PreferredPositions
)
{
	FConservativeFallbackResult Result;
	Result.Positions = ReadabilityContext.OriginalPositions;
	FCandidateReadabilityEvaluation Current = EvaluateReadabilityCandidate(ReadabilityContext, Result.Positions, false);
	Result.Readability = Current.Metrics;
	if (!Current.Regression.IsEmpty()) { return Result; }

	TMap<const UEdGraphNode*, FString> LastRejectedMoves;
	TMap<const UEdGraphNode*, FString> LastForwardExecutionShiftRejections;
	const auto TryPositionWithPolicy = [&ReadabilityContext, &Result, &Current, &LastRejectedMoves](
										   UEdGraphNode& Node, const FVector2D Position, const bool bAllowRerouteTradeoffs
									   )
	{
		FVector2D* Existing = Result.Positions.Find(&Node);
		if (Existing == nullptr || Existing->Equals(Position, ReadabilityEpsilon)) { return false; }
		const FVector2D Previous = *Existing;
		*Existing = Position;
		FCandidateReadabilityEvaluation Trial =
			EvaluateReadabilityCandidate(ReadabilityContext, Result.Positions, false, bAllowRerouteTradeoffs);
		if (!Trial.Regression.IsEmpty())
		{
			LastRejectedMoves.Add(
				&Node,
				FString::Printf(
					TEXT("moving '%s' to (%.0f, %.0f) would cause %s"), *Node.GetName(), Position.X, Position.Y, *Trial.Regression
				)
			);
			*Existing = Previous;
			return false;
		}
		LastRejectedMoves.Remove(&Node);
		Current = MoveTemp(Trial);
		Result.Readability = Current.Metrics;
		return true;
	};
	const auto TryPosition = [&TryPositionWithPolicy](UEdGraphNode& Node, const FVector2D Position)
	{ return TryPositionWithPolicy(Node, Position, false); };
	const auto TryReroutePosition = [&TryPositionWithPolicy](UEdGraphNode& Node, const FVector2D Position)
	{ return TryPositionWithPolicy(Node, Position, true); };
	const auto TryPositionsWithPolicy = [&ReadabilityContext, &Result, &Current, &LastRejectedMoves](
											TConstArrayView<UEdGraphNode*> Nodes,
											const TMap<UEdGraphNode*, FVector2D>& Positions,
											const bool bAllowRerouteTradeoffs,
											const bool bRequireReadabilityImprovement
										)
	{
		TMap<UEdGraphNode*, FVector2D> PreviousPositions;
		for (UEdGraphNode* Node : Nodes)
		{
			const FVector2D* DesiredPosition = Positions.Find(Node);
			FVector2D* ExistingPosition = Result.Positions.Find(Node);
			if (Node == nullptr || DesiredPosition == nullptr || ExistingPosition == nullptr
				|| ExistingPosition->Equals(*DesiredPosition, ReadabilityEpsilon))
			{
				continue;
			}
			PreviousPositions.Add(Node, *ExistingPosition);
			*ExistingPosition = *DesiredPosition;
		}
		if (PreviousPositions.IsEmpty()) { return false; }

		FCandidateReadabilityEvaluation Trial =
			EvaluateReadabilityCandidate(ReadabilityContext, Result.Positions, false, bAllowRerouteTradeoffs);
		const bool bImprovesReadability =
			HasMaterialReadabilityImprovement(Current.Metrics, Trial.Metrics, ReadabilityContext.LayoutCellSize);
		if (!Trial.Regression.IsEmpty() || (bRequireReadabilityImprovement && !bImprovesReadability))
		{
			for (const TPair<UEdGraphNode*, FVector2D>& Previous : PreviousPositions)
			{
				Result.Positions.FindChecked(Previous.Key) = Previous.Value;
				LastRejectedMoves.Add(
					Previous.Key,
					Trial.Regression.IsEmpty()
						? TEXT("moving its preferred layout group would not materially improve readability")
						: FString::Printf(TEXT("moving its preferred layout group would cause %s"), *Trial.Regression)
				);
			}
			return false;
		}

		for (const TPair<UEdGraphNode*, FVector2D>& Previous : PreviousPositions)
		{
			LastRejectedMoves.Remove(Previous.Key);
		}
		Current = MoveTemp(Trial);
		Result.Readability = Current.Metrics;
		return true;
	};
	const auto TryPositions =
		[&TryPositionsWithPolicy](TConstArrayView<UEdGraphNode*> Nodes, const TMap<UEdGraphNode*, FVector2D>& Positions)
	{ return TryPositionsWithPolicy(Nodes, Positions, false, false); };
	const auto TryImprovingPositions =
		[&TryPositionsWithPolicy](TConstArrayView<UEdGraphNode*> Nodes, const TMap<UEdGraphNode*, FVector2D>& Positions)
	{ return TryPositionsWithPolicy(Nodes, Positions, false, true); };
	const auto TryReroutePositions =
		[&TryPositionsWithPolicy](TConstArrayView<UEdGraphNode*> Nodes, const TMap<UEdGraphNode*, FVector2D>& Positions)
	{ return TryPositionsWithPolicy(Nodes, Positions, true, false); };

	TMap<const UEdGraphNode*, const FAdapterNodeRecord*> NodesByObject;
	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		NodesByObject.Add(Node.Node, &Node);
	}
	TMap<const UEdGraphNode*, TArray<const UEdGraphNode*>> ConnectedNodes;
	TMap<const UEdGraphNode*, TArray<const UEdGraphNode*>> ExecutionConnectedNodes;
	TMap<const UEdGraphNode*, TArray<const UEdGraphNode*>> ExecutionSuccessors;
	for (const FAdapterEdgeRecord& Edge : LayoutEdges)
	{
		const UEdGraphNode* Source = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
		const UEdGraphNode* Target = Edge.InputPin != nullptr ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
		if (Source == nullptr || Target == nullptr || !NodesByObject.Contains(Source) || !NodesByObject.Contains(Target))
		{
			continue;
		}
		ConnectedNodes.FindOrAdd(Source).AddUnique(Target);
		ConnectedNodes.FindOrAdd(Target).AddUnique(Source);
		if (Edge.bExecution)
		{
			ExecutionConnectedNodes.FindOrAdd(Source).AddUnique(Target);
			ExecutionConnectedNodes.FindOrAdd(Target).AddUnique(Source);
			ExecutionSuccessors.FindOrAdd(Source).AddUnique(Target);
		}
	}
	const auto TryForwardExecutionShift =
		[&ReadabilityContext,
		 &Result,
		 &ExecutionSuccessors,
		 &ConnectedNodes,
		 &LayoutEdges,
		 &NodesByObject,
		 &LastForwardExecutionShiftRejections,
		 &LastRejectedMoves,
		 &TryPositionsWithPolicy](const UEdGraphNode& Source, UEdGraphNode& Target, const double RequiredTargetX)
	{
		const FVector2D* TargetPosition = Result.Positions.Find(&Target);
		if (TargetPosition == nullptr || TargetPosition->X + ReadabilityEpsilon >= RequiredTargetX) { return false; }

		TArray<const UEdGraphNode*> Pending{ &Target };
		TSet<const UEdGraphNode*> Visited;
		for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
		{
			const UEdGraphNode* Node = Pending[PendingIndex];
			if (Node == nullptr || Visited.Contains(Node)) { continue; }
			if (Node == &Source)
			{
				// Do not translate an execution cycle through its own source; the individual and
				// right-side fallbacks below can still admit a smaller safe correction.
				return false;
			}
			Visited.Add(Node);
			if (const TArray<const UEdGraphNode*>* Successors = ExecutionSuccessors.Find(Node))
			{
				Pending.Append(*Successors);
			}
		}
		if (Visited.IsEmpty()) { return false; }

		// Solve every downstream preferred-execution gutter in the same atomic candidate. A
		// uniform suffix translation only fixes the first short gap; each later formatter click
		// would otherwise advance the next suffix by one more cell.
		TMap<UEdGraphNode*, FVector2D> ShiftedPositions;
		for (const UEdGraphNode* Node : Visited)
		{
			if (const FVector2D* Position = Result.Positions.Find(Node))
			{
				ShiftedPositions.Add(const_cast<UEdGraphNode*>(Node), *Position);
			}
		}
		FVector2D* ShiftedTargetPosition = ShiftedPositions.Find(&Target);
		if (ShiftedTargetPosition == nullptr) { return false; }
		ShiftedTargetPosition->X = RequiredTargetX;

		const auto ResolveCandidatePosition = [&Result, &ShiftedPositions](const UEdGraphNode* Node) -> const FVector2D*
		{
			if (const FVector2D* ShiftedPosition = ShiftedPositions.Find(Node)) { return ShiftedPosition; }
			return Result.Positions.Find(Node);
		};
		const int32 MaximumPropagationPasses = FMath::Max(1, Visited.Num() * 2);
		for (int32 Pass = 0; Pass < MaximumPropagationPasses; ++Pass)
		{
			bool bChangedInPass = false;
			for (const FAdapterEdgeRecord& Edge : LayoutEdges)
			{
				if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr)
				{
					continue;
				}
				const UEdGraphNode* EdgeSource = Edge.OutputPin->GetOwningNodeUnchecked();
				UEdGraphNode* EdgeTarget = Edge.InputPin->GetOwningNodeUnchecked();
				if (EdgeSource == nullptr || EdgeTarget == nullptr || EdgeSource->IsA<UK2Node_Knot>()
					|| EdgeTarget->IsA<UK2Node_Knot>() || !Visited.Contains(EdgeTarget))
				{
					continue;
				}
				const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(EdgeSource);
				const FAdapterNodeRecord* const* TargetRecord = NodesByObject.Find(EdgeTarget);
				const FVector2D* OriginalSourcePosition = ReadabilityContext.OriginalPositions.Find(EdgeSource);
				const FVector2D* OriginalTargetPosition = ReadabilityContext.OriginalPositions.Find(EdgeTarget);
				const FVector2D* SourcePosition = ResolveCandidatePosition(EdgeSource);
				FVector2D* CandidateTargetPosition = ShiftedPositions.Find(EdgeTarget);
				if (SourceRecord == nullptr || TargetRecord == nullptr || OriginalSourcePosition == nullptr
					|| OriginalTargetPosition == nullptr || SourcePosition == nullptr
					|| CandidateTargetPosition == nullptr || OriginalTargetPosition->X <= OriginalSourcePosition->X)
				{
					continue;
				}

				double SourceColumnRight = SourcePosition->X + (*SourceRecord)->Size.X;
				for (const FAdapterNodeRecord& ColumnNode : ReadabilityContext.AuthoredNodes)
				{
					const FVector2D* ColumnPosition = ResolveCandidatePosition(ColumnNode.Node);
					if (ColumnPosition != nullptr
						&& FMath::IsNearlyEqual(ColumnPosition->X, SourcePosition->X, ReadabilityEpsilon))
					{
						SourceColumnRight = FMath::Max(SourceColumnRight, ColumnPosition->X + ColumnNode.Size.X);
					}
				}

				const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
				const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
				double UnsnappedTargetX = SourceColumnRight + ReadabilityContext.LayoutCellSize;
				if (OutputPin != nullptr && InputPin != nullptr)
				{
					UnsnappedTargetX = FMath::Max(
						UnsnappedTargetX,
						SourcePosition->X + OutputPin->Offset.X + ReadabilityContext.LayoutCellSize - InputPin->Offset.X
					);
				}
				const double PropagatedTargetX = FMath::CeilToDouble(UnsnappedTargetX / ReadabilityContext.LayoutCellSize)
											   * ReadabilityContext.LayoutCellSize;
				if (CandidateTargetPosition->X + ReadabilityEpsilon < PropagatedTargetX)
				{
					CandidateTargetPosition->X = PropagatedTargetX;
					bChangedInPass = true;
				}
			}
			if (!bChangedInPass) { break; }
		}

		// Carry a pure provider tree only when every one of its consumers already belongs to this
		// suffix and those consumers agree on one translation. This keeps local getter/operator
		// wiring attached without dragging shared state or a neighbouring execution paragraph.
		TMap<const UEdGraphNode*, double> TranslationByIncludedNode;
		for (const TPair<UEdGraphNode*, FVector2D>& ShiftedPosition : ShiftedPositions)
		{
			if (const FVector2D* CurrentPosition = Result.Positions.Find(ShiftedPosition.Key))
			{
				TranslationByIncludedNode.Add(ShiftedPosition.Key, ShiftedPosition.Value.X - CurrentPosition->X);
			}
		}
		for (int32 Pass = 0; Pass < ReadabilityContext.AuthoredNodes.Num(); ++Pass)
		{
			bool bAttachedInPass = false;
			for (const FAdapterNodeRecord& NodeRecord : ReadabilityContext.AuthoredNodes)
			{
				UEdGraphNode* Node = NodeRecord.Node;
				if (Node == nullptr || Node->IsA<UK2Node_Knot>() || TranslationByIncludedNode.Contains(Node))
				{
					continue;
				}

				bool bHasExecutionPin = false;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					bHasExecutionPin |= Pin != nullptr && IsExecutionPin(*Pin);
				}
				if (bHasExecutionPin) { continue; }

				bool bHasConsumer = false;
				bool bSharedOrConflicting = false;
				TOptional<double> AgreedTranslation;
				for (const FAdapterEdgeRecord& Edge : LayoutEdges)
				{
					const UEdGraphNode* Provider = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked()
																			 : nullptr;
					if (Edge.bExecution || Provider != Node || Edge.InputPin == nullptr) { continue; }
					bHasConsumer = true;
					const UEdGraphNode* Consumer = Edge.InputPin->GetOwningNodeUnchecked();
					const double* ConsumerTranslation = TranslationByIncludedNode.Find(Consumer);
					if (ConsumerTranslation == nullptr)
					{
						bSharedOrConflicting = true;
						break;
					}
					if (!AgreedTranslation.IsSet()) { AgreedTranslation = *ConsumerTranslation; }
					else if (!FMath::IsNearlyEqual(AgreedTranslation.GetValue(), *ConsumerTranslation, ReadabilityEpsilon))
					{
						bSharedOrConflicting = true;
						break;
					}
				}
				if (!bHasConsumer || bSharedOrConflicting || !AgreedTranslation.IsSet()
					|| FMath::IsNearlyZero(AgreedTranslation.GetValue(), ReadabilityEpsilon))
				{
					continue;
				}
				const FVector2D* CurrentPosition = Result.Positions.Find(Node);
				if (CurrentPosition == nullptr) { continue; }
				FVector2D ProviderPosition = *CurrentPosition;
				ProviderPosition.X =
					FMath::GridSnap(ProviderPosition.X + AgreedTranslation.GetValue(), ReadabilityContext.LayoutCellSize);
				const double SnappedTranslation = ProviderPosition.X - CurrentPosition->X;
				if (FMath::IsNearlyZero(SnappedTranslation, ReadabilityEpsilon)) { continue; }
				ShiftedPositions.Add(Node, ProviderPosition);
				// Propagate the provider's actual snapped displacement, rather than the consumer's
				// displacement. An upstream provider chain then converges on visible major-grid
				// columns as one atomic candidate without independently snapping into itself.
				TranslationByIncludedNode.Add(Node, SnappedTranslation);
				bAttachedInPass = true;
			}
			if (!bAttachedInPass) { break; }
		}

		// Let affected authored reroute chains follow their semantic terminals inside the same
		// candidate. Unrelated reroutes remain exactly where the author placed them.
		TMap<UEdGraphNode*, FVector2D> CandidatePositions = Result.Positions;
		for (const TPair<UEdGraphNode*, FVector2D>& ShiftedPosition : ShiftedPositions)
		{
			CandidatePositions.FindOrAdd(ShiftedPosition.Key) = ShiftedPosition.Value;
		}
		const TMap<UEdGraphNode*, FVector2D> RerouteAlignments = BuildAuthoredRerouteColumnAlignments(
			ReadabilityContext.AuthoredNodes, LayoutEdges, ReadabilityContext.PinRecords, CandidatePositions
		);
		TSet<const UEdGraphNode*> AffectedKnots;
		Pending.Reset();
		for (const TPair<UEdGraphNode*, FVector2D>& ShiftedPosition : ShiftedPositions)
		{
			Pending.Add(ShiftedPosition.Key);
		}
		for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
		{
			const UEdGraphNode* Node = Pending[PendingIndex];
			if (const TArray<const UEdGraphNode*>* Neighbours = ConnectedNodes.Find(Node))
			{
				for (const UEdGraphNode* Neighbour : *Neighbours)
				{
					if (Neighbour != nullptr && Neighbour->IsA<UK2Node_Knot>() && !AffectedKnots.Contains(Neighbour))
					{
						AffectedKnots.Add(Neighbour);
						Pending.Add(Neighbour);
					}
				}
			}
		}
		for (const UEdGraphNode* Knot : AffectedKnots)
		{
			if (const FVector2D* Alignment = RerouteAlignments.Find(Knot))
			{
				ShiftedPositions.FindOrAdd(const_cast<UEdGraphNode*>(Knot)) = *Alignment;
			}
		}

		TArray<UEdGraphNode*> ShiftedNodes;
		ShiftedPositions.GetKeys(ShiftedNodes);
		ShiftedNodes.Sort(
			[&NodesByObject](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				const FAdapterNodeRecord* const* LeftRecord = NodesByObject.Find(&Left);
				const FAdapterNodeRecord* const* RightRecord = NodesByObject.Find(&Right);
				return LeftRecord != nullptr && RightRecord != nullptr ? (*LeftRecord)->Key < (*RightRecord)->Key
																	   : Left.GetName() < Right.GetName();
			}
		);
		// Execution gutters are the primary visual spine. Admit this complete, converged suffix
		// only when it trades at most one additional data wire-under-node defect; because every
		// downstream gutter, owned provider, and affected reroute is included atomically, that
		// bounded allowance cannot accumulate through partial suffix shifts or repeat clicks.
		if (TryPositionsWithPolicy(ShiftedNodes, ShiftedPositions, true, false))
		{
			LastForwardExecutionShiftRejections.Remove(&Target);
			return true;
		}
		const FString AllReroutesRejection = LastRejectedMoves.FindRef(&Target);

		// A data-reroute adjustment may be the only unsafe part of an otherwise complete semantic
		// fixed point. Keep execution knots attached to their shifted branch/merge pins, but retry
		// with data knots authored; the bounded reconciliation phase below will independently admit
		// every safe data-reroute endpoint.
		for (const UEdGraphNode* Knot : AffectedKnots)
		{
			const UK2Node_Knot* TypedKnot = Cast<UK2Node_Knot>(Knot);
			if (TypedKnot != nullptr && TypedKnot->GetInputPin() != nullptr && IsExecutionPin(*TypedKnot->GetInputPin()))
			{
				continue;
			}
			if (const FVector2D* CurrentPosition = Result.Positions.Find(Knot))
			{
				ShiftedPositions.FindOrAdd(const_cast<UEdGraphNode*>(Knot)) = *CurrentPosition;
			}
		}
		if (TryPositionsWithPolicy(ShiftedNodes, ShiftedPositions, false, false))
		{
			LastForwardExecutionShiftRejections.Remove(&Target);
			return true;
		}
		const FString ExecutionReroutesRejection = LastRejectedMoves.FindRef(&Target);

		// If even an execution-knot alignment is unsafe, leave every reroute authored and retain
		// the semantic move only when it independently clears all strict readability gates.
		for (const UEdGraphNode* Knot : AffectedKnots)
		{
			if (const FVector2D* CurrentPosition = Result.Positions.Find(Knot))
			{
				ShiftedPositions.FindOrAdd(const_cast<UEdGraphNode*>(Knot)) = *CurrentPosition;
			}
		}
		if (TryPositionsWithPolicy(ShiftedNodes, ShiftedPositions, false, false))
		{
			LastForwardExecutionShiftRejections.Remove(&Target);
			return true;
		}
		const FString SemanticOnlyRejection = LastRejectedMoves.FindRef(&Target);
		LastForwardExecutionShiftRejections.Add(
			&Target,
			SemanticOnlyRejection.IsEmpty()
				? TEXT("solving the complete downstream execution suffix was not readability-safe")
				: FString::Printf(
					  TEXT(
						  "solving the complete downstream execution suffix was rejected with all reroutes (%s), "
						  "execution reroutes only (%s), and authored reroutes (%s)"
					  ),
					  AllReroutesRejection.IsEmpty() ? TEXT("unknown") : *AllReroutesRejection,
					  ExecutionReroutesRejection.IsEmpty() ? TEXT("unknown") : *ExecutionReroutesRejection,
					  *SemanticOnlyRejection
				  )
		);
		return false;
	};
	const auto TryRightSideColumnShift = [&ReadabilityContext, &Result, &Current, &LastRejectedMoves, &ConnectedNodes](
											 UEdGraphNode& Target, const double RequiredTargetX
										 )
	{
		const FVector2D* TargetPosition = Result.Positions.Find(&Target);
		if (TargetPosition == nullptr || TargetPosition->X + ReadabilityEpsilon >= RequiredTargetX) { return false; }
		const double ThresholdColumnX = FMath::GridSnap(TargetPosition->X, ReadabilityContext.LayoutCellSize);
		TSet<const UEdGraphNode*> Component;
		TArray<const UEdGraphNode*> Pending{ &Target };
		for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
		{
			const UEdGraphNode* Node = Pending[PendingIndex];
			if (Node == nullptr || Component.Contains(Node)) { continue; }
			Component.Add(Node);
			if (const TArray<const UEdGraphNode*>* Neighbours = ConnectedNodes.Find(Node))
			{
				Pending.Append(*Neighbours);
			}
		}

		const double ColumnDeltaX = RequiredTargetX - ThresholdColumnX;
		TMap<UEdGraphNode*, FVector2D> PreviousPositions;
		for (const UEdGraphNode* ComponentNode : Component)
		{
			UEdGraphNode* Node = const_cast<UEdGraphNode*>(ComponentNode);
			FVector2D* Position = Result.Positions.Find(Node);
			if (Position == nullptr) { continue; }
			const double NodeColumnX = Node->IsA<UK2Node_Knot>()
										 ? Position->X
										 : FMath::GridSnap(Position->X, ReadabilityContext.LayoutCellSize);
			if (NodeColumnX + ReadabilityEpsilon < ThresholdColumnX) { continue; }
			PreviousPositions.Add(Node, *Position);
			Position->X = Node->IsA<UK2Node_Knot>() ? Position->X + ColumnDeltaX : NodeColumnX + ColumnDeltaX;
		}
		if (PreviousPositions.IsEmpty()) { return false; }

		FCandidateReadabilityEvaluation Trial = EvaluateReadabilityCandidate(ReadabilityContext, Result.Positions, false);
		if (!Trial.Regression.IsEmpty())
		{
			for (const TPair<UEdGraphNode*, FVector2D>& Previous : PreviousPositions)
			{
				Result.Positions.FindChecked(Previous.Key) = Previous.Value;
				LastRejectedMoves.Add(
					Previous.Key,
					FString::Printf(TEXT("shifting its right-side column group would cause %s"), *Trial.Regression)
				);
			}
			return false;
		}
		for (const TPair<UEdGraphNode*, FVector2D>& Previous : PreviousPositions)
		{
			LastRejectedMoves.Remove(Previous.Key);
		}
		Current = MoveTemp(Trial);
		Result.Readability = Current.Metrics;
		return true;
	};

	// A move accepted later in a pass can make a previously unsafe grid snap viable. Iterate the
	// grid and preferred-execution constraints together so a single format command reaches the same
	// fixed point that previously required multiple clicks. Once a node has reached the major grid,
	// do not pull it back toward its authored column after execution spacing advances it farther right.
	TSet<const UEdGraphNode*> GridAlignedNodes;
	const int32 MaximumFallbackPasses = FMath::Max(1, ReadabilityContext.AuthoredNodes.Num() * 2);
	for (int32 Pass = 0; Pass < MaximumFallbackPasses; ++Pass)
	{
		bool bMovedInPass = false;
		for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
		{
			if (Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>() || GridAlignedNodes.Contains(Node.Node))
			{
				continue;
			}
			const FVector2D GridPosition(
				FMath::GridSnap(Node.OriginalPosition.X, ReadabilityContext.LayoutCellSize), Node.OriginalPosition.Y
			);
			const FVector2D* CurrentPosition = Result.Positions.Find(Node.Node);
			if (CurrentPosition != nullptr && CurrentPosition->Equals(GridPosition, ReadabilityEpsilon))
			{
				GridAlignedNodes.Add(Node.Node);
				continue;
			}
			if (TryPosition(*Node.Node, GridPosition))
			{
				GridAlignedNodes.Add(Node.Node);
				bMovedInPass = true;
			}
		}

		for (const FAdapterEdgeRecord& Edge : LayoutEdges)
		{
			if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr)
			{
				continue;
			}
			UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
			UEdGraphNode* Target = Edge.InputPin->GetOwningNodeUnchecked();
			if (Source == nullptr || Target == nullptr || Source->IsA<UK2Node_Knot>() || Target->IsA<UK2Node_Knot>())
			{
				continue;
			}
			const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(Source);
			const FAdapterNodeRecord* const* TargetRecord = NodesByObject.Find(Target);
			const FVector2D* SourcePosition = Result.Positions.Find(Source);
			const FVector2D* TargetPosition = Result.Positions.Find(Target);
			if (SourceRecord == nullptr || TargetRecord == nullptr || SourcePosition == nullptr || TargetPosition == nullptr
				|| (*TargetRecord)->OriginalPosition.X <= (*SourceRecord)->OriginalPosition.X)
			{
				continue;
			}
			double SourceColumnRight = SourcePosition->X + (*SourceRecord)->Size.X;
			for (const FAdapterNodeRecord& ColumnNode : ReadabilityContext.AuthoredNodes)
			{
				const FVector2D* ColumnPosition = Result.Positions.Find(ColumnNode.Node);
				if (ColumnPosition != nullptr
					&& FMath::IsNearlyEqual(ColumnPosition->X, SourcePosition->X, ReadabilityEpsilon))
				{
					SourceColumnRight = FMath::Max(SourceColumnRight, ColumnPosition->X + ColumnNode.Size.X);
				}
			}
			const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
			const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
			double UnsnappedTargetX = SourceColumnRight + ReadabilityContext.LayoutCellSize;
			if (OutputPin != nullptr && InputPin != nullptr)
			{
				// The visible gutter is measured between execution pins, not only between the
				// cached node bounds. Some Slate node styles place their output pin slightly past
				// the measured body, so the body-only rule could still leave a 96-unit wire.
				UnsnappedTargetX = FMath::Max(
					UnsnappedTargetX,
					SourcePosition->X + OutputPin->Offset.X + ReadabilityContext.LayoutCellSize - InputPin->Offset.X
				);
			}
			const double RequiredTargetX = FMath::CeilToDouble(UnsnappedTargetX / ReadabilityContext.LayoutCellSize)
										 * ReadabilityContext.LayoutCellSize;
			const double FinalTargetX = FMath::Max(TargetPosition->X, RequiredTargetX);
			bool bMovedTarget = false;
			if (OutputPin != nullptr && InputPin != nullptr)
			{
				const double StraightExecutionY = SourcePosition->Y + OutputPin->Offset.Y - InputPin->Offset.Y;
				bMovedTarget = TryPosition(*Target, FVector2D(FinalTargetX, StraightExecutionY));
			}
			if (!bMovedTarget && TargetPosition->X + ReadabilityEpsilon < RequiredTargetX)
			{
				bMovedTarget = TryPosition(*Target, FVector2D(RequiredTargetX, TargetPosition->Y))
							|| TryForwardExecutionShift(*Source, *Target, RequiredTargetX)
							|| TryRightSideColumnShift(*Target, RequiredTargetX);
			}
			if (bMovedTarget)
			{
				for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
				{
					const FVector2D* Position = Result.Positions.Find(Node.Node);
					if (Node.Node != nullptr && !Node.Node->IsA<UK2Node_Knot>() && Position != nullptr
						&& FMath::IsNearlyZero(FMath::Fmod(Position->X, ReadabilityContext.LayoutCellSize), ReadabilityEpsilon))
					{
						GridAlignedNodes.Add(Node.Node);
					}
				}
				bMovedInPass = true;
			}
		}
		if (!bMovedInPass) { break; }
	}

	// Never let a later semantic-group admission undo a gutter already established by the
	// execution-first fallback. Anchor execution roots at their current authored row, then
	// propagate exact pin alignment through each preferred execution chain. This makes the target
	// independent of which neighbouring paragraph happened to become safe first.
	TMap<UEdGraphNode*, FVector2D> MonotonicPreferredPositions = PreferredPositions;
	const auto HasFullPreferredExecutionGutter = [&ReadabilityContext, &Result, &LayoutEdges, &NodesByObject](
													 const UEdGraphNode& Target, const FVector2D& TargetPosition
												 )
	{
		bool bHasPreferredIncomingEdge = false;
		for (const FAdapterEdgeRecord& Edge : LayoutEdges)
		{
			if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr
				|| Edge.InputPin->GetOwningNodeUnchecked() != &Target)
			{
				continue;
			}

			const UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
			const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(Source);
			const FVector2D* SourcePosition = Result.Positions.Find(Source);
			if (Source == nullptr || Source->IsA<UK2Node_Knot>() || SourceRecord == nullptr || SourcePosition == nullptr)
			{
				continue;
			}
			bHasPreferredIncomingEdge = true;

			double SourceColumnRight = SourcePosition->X + (*SourceRecord)->Size.X;
			for (const FAdapterNodeRecord& ColumnNode : ReadabilityContext.AuthoredNodes)
			{
				const FVector2D* ColumnPosition = Result.Positions.Find(ColumnNode.Node);
				if (ColumnPosition != nullptr
					&& FMath::IsNearlyEqual(ColumnPosition->X, SourcePosition->X, ReadabilityEpsilon))
				{
					SourceColumnRight = FMath::Max(SourceColumnRight, ColumnPosition->X + ColumnNode.Size.X);
				}
			}

			const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
			const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
			double RequiredTargetX = SourceColumnRight + ReadabilityContext.LayoutCellSize;
			if (OutputPin != nullptr && InputPin != nullptr)
			{
				RequiredTargetX = FMath::Max(
					RequiredTargetX,
					SourcePosition->X + OutputPin->Offset.X + ReadabilityContext.LayoutCellSize - InputPin->Offset.X
				);
			}
			RequiredTargetX = FMath::CeilToDouble(RequiredTargetX / ReadabilityContext.LayoutCellSize)
							* ReadabilityContext.LayoutCellSize;
			if (TargetPosition.X + ReadabilityEpsilon < RequiredTargetX) { return false; }
		}
		return bHasPreferredIncomingEdge;
	};
	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		FVector2D* PreferredPosition = MonotonicPreferredPositions.Find(Node.Node);
		const FVector2D* CurrentPosition = Result.Positions.Find(Node.Node);
		if (Node.Node == nullptr || PreferredPosition == nullptr || CurrentPosition == nullptr) { continue; }

		bool bHasExecutionOutput = false;
		bool bHasLinkedExecutionInput = false;
		for (const UEdGraphPin* Pin : Node.Node->Pins)
		{
			if (Pin == nullptr || !IsExecutionPin(*Pin)) { continue; }
			bHasExecutionOutput |= Pin->Direction == EGPD_Output;
			bHasLinkedExecutionInput |= Pin->Direction == EGPD_Input && !Pin->LinkedTo.IsEmpty();
		}
		const bool bExecutionRoot = bHasExecutionOutput && !bHasLinkedExecutionInput;
		PreferredPosition->X = bExecutionRoot || HasFullPreferredExecutionGutter(*Node.Node, *CurrentPosition)
								 ? CurrentPosition->X
								 : FMath::Max(PreferredPosition->X, CurrentPosition->X);
		if (bExecutionRoot) { PreferredPosition->Y = CurrentPosition->Y; }
	}
	for (int32 Pass = 0; Pass < ReadabilityContext.AuthoredNodes.Num(); ++Pass)
	{
		bool bAlignedInPass = false;
		for (const FAdapterEdgeRecord& Edge : LayoutEdges)
		{
			if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr)
			{
				continue;
			}
			UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
			UEdGraphNode* Target = Edge.InputPin->GetOwningNodeUnchecked();
			FVector2D* SourcePosition = MonotonicPreferredPositions.Find(Source);
			FVector2D* TargetPosition = MonotonicPreferredPositions.Find(Target);
			const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
			const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
			if (SourcePosition == nullptr || TargetPosition == nullptr || OutputPin == nullptr || InputPin == nullptr)
			{
				continue;
			}
			const double AlignedTargetY = SourcePosition->Y + OutputPin->Offset.Y - InputPin->Offset.Y;
			if (!FMath::IsNearlyEqual(TargetPosition->Y, AlignedTargetY, ReadabilityEpsilon))
			{
				TargetPosition->Y = AlignedTargetY;
				bAlignedInPass = true;
			}
		}
		if (!bAlignedInPass) { break; }
	}

	// When the complete semantic layout is unsafe, retain coherent pieces that are safe in the
	// original graph-wide wire field. Execution-connected paragraphs move atomically, including
	// pure provider trees that belong unambiguously to one paragraph. Isolated semantic moves are
	// deliberately not admitted here: accepting only one rank changes the authored rank medians on
	// the next invocation and produces the rightward "stair-step" that used to require repeated
	// formatter clicks.
	TArray<TArray<UEdGraphNode*>> PreferredSemanticGroups;
	TMap<const UEdGraphNode*, int32> SemanticGroupByNode;
	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		if (Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>() || SemanticGroupByNode.Contains(Node.Node)
			|| !ExecutionConnectedNodes.Contains(Node.Node))
		{
			continue;
		}

		const int32 GroupIndex = PreferredSemanticGroups.AddDefaulted();
		TArray<const UEdGraphNode*> Pending{ Node.Node };
		for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
		{
			const UEdGraphNode* CurrentNode = Pending[PendingIndex];
			if (CurrentNode == nullptr || SemanticGroupByNode.Contains(CurrentNode)) { continue; }
			SemanticGroupByNode.Add(CurrentNode, GroupIndex);
			PreferredSemanticGroups[GroupIndex].Add(const_cast<UEdGraphNode*>(CurrentNode));
			if (const TArray<const UEdGraphNode*>* Neighbours = ExecutionConnectedNodes.Find(CurrentNode))
			{
				Pending.Append(*Neighbours);
			}
		}
	}

	// Attach a pure provider only when all of its already classified neighbours agree on the same
	// execution paragraph. Shared providers that bridge paragraphs stay authored.
	for (int32 Pass = 0; Pass < ReadabilityContext.AuthoredNodes.Num(); ++Pass)
	{
		bool bAssignedInPass = false;
		for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
		{
			if (Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>() || SemanticGroupByNode.Contains(Node.Node))
			{
				continue;
			}
			bool bHasExecutionPin = false;
			for (const UEdGraphPin* Pin : Node.Node->Pins)
			{
				bHasExecutionPin |= Pin != nullptr && IsExecutionPin(*Pin);
			}
			if (bHasExecutionPin) { continue; }

			TOptional<int32> AgreedGroup;
			bool bAmbiguous = false;
			if (const TArray<const UEdGraphNode*>* Neighbours = ConnectedNodes.Find(Node.Node))
			{
				for (const UEdGraphNode* Neighbour : *Neighbours)
				{
					const int32* NeighbourGroup = SemanticGroupByNode.Find(Neighbour);
					if (NeighbourGroup == nullptr) { continue; }
					if (!AgreedGroup.IsSet()) { AgreedGroup = *NeighbourGroup; }
					else if (AgreedGroup.GetValue() != *NeighbourGroup) { bAmbiguous = true; }
				}
			}
			if (AgreedGroup.IsSet() && !bAmbiguous)
			{
				SemanticGroupByNode.Add(Node.Node, AgreedGroup.GetValue());
				PreferredSemanticGroups[AgreedGroup.GetValue()].Add(Node.Node);
				bAssignedInPass = true;
			}
		}
		if (!bAssignedInPass) { break; }
	}
	for (TArray<UEdGraphNode*>& Group : PreferredSemanticGroups)
	{
		Group.Sort(
			[&NodesByObject](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				const FAdapterNodeRecord* const* LeftRecord = NodesByObject.Find(&Left);
				const FAdapterNodeRecord* const* RightRecord = NodesByObject.Find(&Right);
				return LeftRecord != nullptr && RightRecord != nullptr ? (*LeftRecord)->Key < (*RightRecord)->Key
																	   : Left.GetName() < Right.GetName();
			}
		);
	}

	// Equal translations are a second safe atomic unit for data-only islands and for a paragraph
	// whose full semantic target is rejected by a data-wire constraint.
	TMap<FString, TArray<UEdGraphNode*>> PreferredTranslationGroups;
	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		const FVector2D* PreferredPosition = MonotonicPreferredPositions.Find(Node.Node);
		if (Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>() || PreferredPosition == nullptr) { continue; }
		const FVector2D Delta = *PreferredPosition - Node.OriginalPosition;
		const FString TranslationKey =
			FString::Printf(TEXT("%d:%d"), FMath::RoundToInt(Delta.X), FMath::RoundToInt(Delta.Y));
		PreferredTranslationGroups.FindOrAdd(TranslationKey).Add(Node.Node);
	}
	TArray<FString> PreferredTranslationKeys;
	PreferredTranslationGroups.GetKeys(PreferredTranslationKeys);
	PreferredTranslationKeys.Sort();
	for (int32 Pass = 0; Pass < MaximumFallbackPasses; ++Pass)
	{
		bool bMovedInPass = false;
		for (const TArray<UEdGraphNode*>& Group : PreferredSemanticGroups)
		{
			if (Group.Num() > 1) { bMovedInPass |= TryImprovingPositions(Group, MonotonicPreferredPositions); }
		}
		for (const FString& TranslationKey : PreferredTranslationKeys)
		{
			const TArray<UEdGraphNode*>& Group = PreferredTranslationGroups.FindChecked(TranslationKey);
			if (Group.Num() > 1) { bMovedInPass |= TryImprovingPositions(Group, MonotonicPreferredPositions); }
		}
		if (!bMovedInPass) { break; }
	}

	// Solve each accepted execution paragraph to its transitive fixed point and admit that result
	// atomically. Moving A after A->B was checked can otherwise make B->C safe only on the next
	// formatter click, and moving one node at a time can temporarily create a wire-under-node defect
	// that the complete paragraph position does not contain.
	for (const TArray<UEdGraphNode*>& Group : PreferredSemanticGroups)
	{
		if (Group.Num() <= 1) { continue; }
		TSet<const UEdGraphNode*> GroupNodes;
		TMap<UEdGraphNode*, FVector2D> FixedPointPositions;
		for (UEdGraphNode* Node : Group)
		{
			const FVector2D* Position = Result.Positions.Find(Node);
			if (Node != nullptr && Position != nullptr)
			{
				GroupNodes.Add(Node);
				FixedPointPositions.Add(Node, *Position);
			}
		}

		bool bChanged = false;
		for (int32 Pass = 0; Pass < Group.Num(); ++Pass)
		{
			bool bChangedInPass = false;
			for (const FAdapterEdgeRecord& Edge : LayoutEdges)
			{
				if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr)
				{
					continue;
				}
				UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
				UEdGraphNode* Target = Edge.InputPin->GetOwningNodeUnchecked();
				if (!GroupNodes.Contains(Source) || !GroupNodes.Contains(Target) || Source->IsA<UK2Node_Knot>()
					|| Target->IsA<UK2Node_Knot>())
				{
					continue;
				}
				const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(Source);
				const FAdapterNodeRecord* const* TargetRecord = NodesByObject.Find(Target);
				FVector2D* SourcePosition = FixedPointPositions.Find(Source);
				FVector2D* TargetPosition = FixedPointPositions.Find(Target);
				const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
				const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
				if (SourceRecord == nullptr || TargetRecord == nullptr || SourcePosition == nullptr
					|| TargetPosition == nullptr || OutputPin == nullptr || InputPin == nullptr
					|| (*TargetRecord)->OriginalPosition.X <= (*SourceRecord)->OriginalPosition.X)
				{
					continue;
				}

				double SourceColumnRight = SourcePosition->X + (*SourceRecord)->Size.X;
				for (const FAdapterNodeRecord& ColumnNode : ReadabilityContext.AuthoredNodes)
				{
					const FVector2D* ColumnPosition = FixedPointPositions.Find(ColumnNode.Node);
					if (ColumnPosition == nullptr) { ColumnPosition = Result.Positions.Find(ColumnNode.Node); }
					if (ColumnPosition != nullptr
						&& FMath::IsNearlyEqual(ColumnPosition->X, SourcePosition->X, ReadabilityEpsilon))
					{
						SourceColumnRight = FMath::Max(SourceColumnRight, ColumnPosition->X + ColumnNode.Size.X);
					}
				}
				const double UnsnappedTargetX = FMath::Max(
					SourceColumnRight + ReadabilityContext.LayoutCellSize,
					SourcePosition->X + OutputPin->Offset.X + ReadabilityContext.LayoutCellSize - InputPin->Offset.X
				);
				const double RequiredTargetX = FMath::CeilToDouble(UnsnappedTargetX / ReadabilityContext.LayoutCellSize)
											 * ReadabilityContext.LayoutCellSize;
				const double AlignedTargetY = SourcePosition->Y + OutputPin->Offset.Y - InputPin->Offset.Y;
				if (TargetPosition->X + ReadabilityEpsilon < RequiredTargetX
					|| !FMath::IsNearlyEqual(TargetPosition->Y, AlignedTargetY, ReadabilityEpsilon))
				{
					TargetPosition->X = FMath::Max(TargetPosition->X, RequiredTargetX);
					TargetPosition->Y = AlignedTargetY;
					bChangedInPass = true;
					bChanged = true;
				}
			}
			if (!bChangedInPass) { break; }
		}
		if (bChanged) { TryImprovingPositions(Group, FixedPointPositions); }
	}

	// Semantic snapping/gutter changes can make a reroute alignment safe, while anchoring a reroute
	// can in turn make a previously rejected semantic snap or gutter safe. Solve both layers as one
	// bounded fixed point so those dependent corrections do not leak into a second formatter click.
	FString DeferredRerouteRejectionDiagnostic;
	constexpr int32 MaximumRerouteReconciliationPasses = 4;
	for (int32 ReconciliationPass = 0; ReconciliationPass < MaximumRerouteReconciliationPasses; ++ReconciliationPass)
	{
		DeferredRerouteRejectionDiagnostic.Reset();
		const TMap<UEdGraphNode*, FVector2D> PositionsBeforeReconciliation = Result.Positions;
		for (int32 Pass = 0; Pass < MaximumFallbackPasses; ++Pass)
		{
			bool bMovedInPass = false;
			for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
			{
				if (Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>()) { continue; }
				const FVector2D* Position = Result.Positions.Find(Node.Node);
				if (Position == nullptr) { continue; }
				const FVector2D GridPosition(FMath::GridSnap(Position->X, ReadabilityContext.LayoutCellSize), Position->Y);
				bMovedInPass |= TryPosition(*Node.Node, GridPosition);
			}

			for (const FAdapterEdgeRecord& Edge : LayoutEdges)
			{
				if (!Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr || Edge.InputPin == nullptr)
				{
					continue;
				}
				UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
				UEdGraphNode* Target = Edge.InputPin->GetOwningNodeUnchecked();
				if (Source == nullptr || Target == nullptr || Source->IsA<UK2Node_Knot>() || Target->IsA<UK2Node_Knot>())
				{
					continue;
				}
				const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(Source);
				const FAdapterNodeRecord* const* TargetRecord = NodesByObject.Find(Target);
				const FVector2D* SourcePosition = Result.Positions.Find(Source);
				const FVector2D* TargetPosition = Result.Positions.Find(Target);
				if (SourceRecord == nullptr || TargetRecord == nullptr || SourcePosition == nullptr || TargetPosition == nullptr
					|| (*TargetRecord)->OriginalPosition.X <= (*SourceRecord)->OriginalPosition.X)
				{
					continue;
				}

				double SourceColumnRight = SourcePosition->X + (*SourceRecord)->Size.X;
				for (const FAdapterNodeRecord& ColumnNode : ReadabilityContext.AuthoredNodes)
				{
					const FVector2D* ColumnPosition = Result.Positions.Find(ColumnNode.Node);
					if (ColumnPosition != nullptr
						&& FMath::IsNearlyEqual(ColumnPosition->X, SourcePosition->X, ReadabilityEpsilon))
					{
						SourceColumnRight = FMath::Max(SourceColumnRight, ColumnPosition->X + ColumnNode.Size.X);
					}
				}

				const FAdapterPinRecord* OutputPin = ReadabilityContext.PinRecords.Find(Edge.OutputPin);
				const FAdapterPinRecord* InputPin = ReadabilityContext.PinRecords.Find(Edge.InputPin);
				double UnsnappedTargetX = SourceColumnRight + ReadabilityContext.LayoutCellSize;
				if (OutputPin != nullptr && InputPin != nullptr)
				{
					UnsnappedTargetX = FMath::Max(
						UnsnappedTargetX,
						SourcePosition->X + OutputPin->Offset.X + ReadabilityContext.LayoutCellSize - InputPin->Offset.X
					);
				}
				const double RequiredTargetX = FMath::CeilToDouble(UnsnappedTargetX / ReadabilityContext.LayoutCellSize)
											 * ReadabilityContext.LayoutCellSize;
				const double FinalTargetX = FMath::Max(TargetPosition->X, RequiredTargetX);
				bool bMovedTarget = false;
				if (OutputPin != nullptr && InputPin != nullptr)
				{
					const double StraightExecutionY = SourcePosition->Y + OutputPin->Offset.Y - InputPin->Offset.Y;
					bMovedTarget = TryPosition(*Target, FVector2D(FinalTargetX, StraightExecutionY));
				}
				if (!bMovedTarget && TargetPosition->X + ReadabilityEpsilon < RequiredTargetX)
				{
					bMovedTarget = TryPosition(*Target, FVector2D(RequiredTargetX, TargetPosition->Y))
								|| TryForwardExecutionShift(*Source, *Target, RequiredTargetX)
								|| TryRightSideColumnShift(*Target, RequiredTargetX);
				}
				bMovedInPass |= bMovedTarget;
			}
			if (!bMovedInPass) { break; }
		}

		const TMap<UEdGraphNode*, FVector2D> RerouteAlignments = BuildAuthoredRerouteColumnAlignments(
			ReadabilityContext.AuthoredNodes, LayoutEdges, ReadabilityContext.PinRecords, Result.Positions
		);
		if (!RerouteAlignments.IsEmpty())
		{
			TMap<const UEdGraphNode*, TArray<const UEdGraphNode*>> AlignedKnotNeighbours;
			for (const FAdapterEdgeRecord& Edge : LayoutEdges)
			{
				const UEdGraphNode* Source = Edge.OutputPin != nullptr ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
				const UEdGraphNode* Target = Edge.InputPin != nullptr ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
				if (RerouteAlignments.Contains(Source) && RerouteAlignments.Contains(Target))
				{
					AlignedKnotNeighbours.FindOrAdd(Source).AddUnique(Target);
					AlignedKnotNeighbours.FindOrAdd(Target).AddUnique(Source);
				}
			}
			TArray<UEdGraphNode*> OrderedAlignedKnots;
			RerouteAlignments.GetKeys(OrderedAlignedKnots);
			OrderedAlignedKnots.Sort(
				[&NodesByObject](const UEdGraphNode& Left, const UEdGraphNode& Right)
				{
					const FAdapterNodeRecord* const* LeftRecord = NodesByObject.Find(&Left);
					const FAdapterNodeRecord* const* RightRecord = NodesByObject.Find(&Right);
					return LeftRecord != nullptr && RightRecord != nullptr ? (*LeftRecord)->Key < (*RightRecord)->Key
																		   : Left.GetName() < Right.GetName();
				}
			);

			TSet<const UEdGraphNode*> VisitedAlignedKnots;
			for (UEdGraphNode* Start : OrderedAlignedKnots)
			{
				if (VisitedAlignedKnots.Contains(Start)) { continue; }
				TArray<UEdGraphNode*> ChainGroup;
				TArray<const UEdGraphNode*> Pending{ Start };
				for (int32 PendingIndex = 0; PendingIndex < Pending.Num(); ++PendingIndex)
				{
					const UEdGraphNode* Knot = Pending[PendingIndex];
					if (Knot == nullptr || VisitedAlignedKnots.Contains(Knot)) { continue; }
					VisitedAlignedKnots.Add(Knot);
					ChainGroup.Add(const_cast<UEdGraphNode*>(Knot));
					if (const TArray<const UEdGraphNode*>* Neighbours = AlignedKnotNeighbours.Find(Knot))
					{
						Pending.Append(*Neighbours);
					}
				}
				TryReroutePositions(ChainGroup, RerouteAlignments);
			}

			// If one member makes a chain-level alignment unsafe, retain every independently safe
			// endpoint/column instead of rolling back all authored reroutes in the graph.
			for (int32 Pass = 0; Pass < MaximumFallbackPasses; ++Pass)
			{
				bool bMovedInPass = false;
				for (UEdGraphNode* Knot : OrderedAlignedKnots)
				{
					bMovedInPass |= TryReroutePosition(*Knot, RerouteAlignments.FindChecked(Knot));
				}
				if (!bMovedInPass) { break; }
			}

			int32 RejectedAlignmentCount = 0;
			FString FirstRejection;
			for (UEdGraphNode* Knot : OrderedAlignedKnots)
			{
				const FVector2D* Position = Result.Positions.Find(Knot);
				if (Position == nullptr || Position->Equals(RerouteAlignments.FindChecked(Knot), ReadabilityEpsilon))
				{
					continue;
				}
				++RejectedAlignmentCount;
				if (FirstRejection.IsEmpty()) { FirstRejection = LastRejectedMoves.FindRef(Knot); }
			}
			if (RejectedAlignmentCount > 0)
			{
				DeferredRerouteRejectionDiagnostic = FString::Printf(
					TEXT("Kept %d authored reroute column(s) because %s."),
					RejectedAlignmentCount,
					FirstRejection.IsEmpty() ? TEXT("their pin alignment was not readability-safe") : *FirstRejection
				);
			}
		}
		bool bMovedInReconciliation = PositionsBeforeReconciliation.Num() != Result.Positions.Num();
		if (!bMovedInReconciliation)
		{
			for (const TPair<UEdGraphNode*, FVector2D>& Position : Result.Positions)
			{
				const FVector2D* PreviousPosition = PositionsBeforeReconciliation.Find(Position.Key);
				if (PreviousPosition == nullptr || !PreviousPosition->Equals(Position.Value, ReadabilityEpsilon))
				{
					bMovedInReconciliation = true;
					break;
				}
			}
		}
		if (!bMovedInReconciliation) { break; }
	}
	if (!DeferredRerouteRejectionDiagnostic.IsEmpty())
	{
		Result.Diagnostics.Add(MoveTemp(DeferredRerouteRejectionDiagnostic));
	}

	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		if (Result.Diagnostics.Num() >= 8 || Node.Node == nullptr || Node.Node->IsA<UK2Node_Knot>()) { continue; }
		const FVector2D* Position = Result.Positions.Find(Node.Node);
		const FString* Rejection = LastRejectedMoves.Find(Node.Node);
		if (Position == nullptr || Rejection == nullptr) { continue; }
		const double GridX = FMath::GridSnap(Position->X, ReadabilityContext.LayoutCellSize);
		if (!FMath::IsNearlyEqual(Position->X, GridX, ReadabilityEpsilon))
		{
			Result.Diagnostics.Add(
				FString::Printf(
					TEXT("Could not align '%s' from X %.0f to major-grid X %.0f because %s."),
					*Node.Node->GetName(),
					Position->X,
					GridX,
					**Rejection
				)
			);
		}
	}

	for (const FAdapterNodeRecord& Node : ReadabilityContext.AuthoredNodes)
	{
		const FVector2D* Position = Result.Positions.Find(Node.Node);
		Result.MovedNodeCount += Position != nullptr && !Position->Equals(Node.OriginalPosition, ReadabilityEpsilon) ? 1
																													 : 0;
	}
	for (const FAdapterEdgeRecord& Edge : LayoutEdges)
	{
		if (Result.Diagnostics.Num() >= 8 || !Edge.bExecution || !Edge.bPreferredAlignment || Edge.OutputPin == nullptr
			|| Edge.InputPin == nullptr)
		{
			continue;
		}
		const UEdGraphNode* Source = Edge.OutputPin->GetOwningNodeUnchecked();
		const UEdGraphNode* Target = Edge.InputPin->GetOwningNodeUnchecked();
		const FAdapterNodeRecord* const* SourceRecord = NodesByObject.Find(Source);
		const FVector2D* SourcePosition = Result.Positions.Find(Source);
		const FVector2D* TargetPosition = Result.Positions.Find(Target);
		const FString* Rejection = LastForwardExecutionShiftRejections.Find(Target);
		if (Rejection == nullptr) { Rejection = LastRejectedMoves.Find(Target); }
		if (SourceRecord == nullptr || SourcePosition == nullptr || TargetPosition == nullptr || Rejection == nullptr)
		{
			continue;
		}
		const double Gap = TargetPosition->X - (SourcePosition->X + (*SourceRecord)->Size.X);
		if (Gap + ReadabilityEpsilon < ReadabilityContext.LayoutCellSize)
		{
			Result.Diagnostics.Add(
				FString::Printf(TEXT("Could not enforce the full gap for %s because %s."), *DescribeEdge(Edge), **Rejection)
			);
		}
	}
	return Result;
}

[[nodiscard]]
FConservativeFallbackResult BuildConvergedConservativeFallback(
	const FReadabilityEvaluationContext& OriginalContext,
	TConstArrayView<FAdapterEdgeRecord> LayoutEdges,
	const TMap<UEdGraphNode*, FVector2D>& PreferredPositions
)
{
	FConservativeFallbackResult Combined = BuildConservativeFallback(OriginalContext, LayoutEdges, PreferredPositions);
	if (Combined.Readability.bWorkBudgetExhausted) { return Combined; }

	// The safe subset admitted by one fallback pass can make another grid or execution-gutter
	// correction safe. Model those subsequent formatter clicks in memory, but only retain a
	// combined result that still passes the original graph-wide safety baseline. This reaches a
	// fixed point in one transaction without allowing per-pass regressions to accumulate.
	constexpr int32 MaximumRefinementPasses = 4;
	for (int32 Pass = 0; Pass < MaximumRefinementPasses; ++Pass)
	{
		TArray<FAdapterNodeRecord> RefinementReadabilityNodes;
		RefinementReadabilityNodes.Reserve(OriginalContext.ReadabilityNodes.Num());
		for (const FAdapterNodeRecord& Node : OriginalContext.ReadabilityNodes)
		{
			FAdapterNodeRecord& Copy = RefinementReadabilityNodes.Add_GetRef(Node);
			if (const FVector2D* Position = Combined.Positions.Find(Copy.Node)) { Copy.OriginalPosition = *Position; }
		}

		TArray<FAdapterNodeRecord> RefinementAuthoredNodes;
		RefinementAuthoredNodes.Reserve(OriginalContext.AuthoredNodes.Num());
		for (const FAdapterNodeRecord& Node : OriginalContext.AuthoredNodes)
		{
			FAdapterNodeRecord& Copy = RefinementAuthoredNodes.Add_GetRef(Node);
			if (const FVector2D* Position = Combined.Positions.Find(Copy.Node)) { Copy.OriginalPosition = *Position; }
		}

		const FReadabilityEvaluationContext RefinementContext{
			RefinementReadabilityNodes,
			RefinementAuthoredNodes,
			OriginalContext.Edges,
			OriginalContext.PinRecords,
			Combined.Positions,
			Combined.Readability,
			OriginalContext.WorkBudget,
			OriginalContext.LayoutCellSize,
			OriginalContext.bPreserveHumanLayout,
		};
		FConservativeFallbackResult Refined = BuildConservativeFallback(RefinementContext, LayoutEdges, PreferredPositions);
		if (Refined.MovedNodeCount == 0 || Refined.Readability.bWorkBudgetExhausted) { break; }

		FCandidateReadabilityEvaluation CombinedEvaluation =
			EvaluateReadabilityCandidate(OriginalContext, Refined.Positions, false, true);
		if (!CombinedEvaluation.Regression.IsEmpty() || CombinedEvaluation.Metrics.bWorkBudgetExhausted) { break; }

		Combined.Positions = MoveTemp(Refined.Positions);
		Combined.Readability = MoveTemp(CombinedEvaluation.Metrics);
	}

	Combined.MovedNodeCount = 0;
	for (const FAdapterNodeRecord& Node : OriginalContext.AuthoredNodes)
	{
		const FVector2D* Position = Combined.Positions.Find(Node.Node);
		Combined.MovedNodeCount +=
			Position != nullptr && !Position->Equals(Node.OriginalPosition, ReadabilityEpsilon) ? 1 : 0;
	}
	return Combined;
}

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
double ResolveLayoutCellSize(const UFormatterSettings& Settings, const double FineGridSize)
{
	return K2Layout::ResolveMajorGridAlignedCellSize(
		static_cast<double>(Settings.K2LayoutCellSize),
		FineGridSize,
		static_cast<double>(FAppStyle::GetFloat(TEXT("Graph.Panel.GridRulePeriod")))
	);
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
{ return FormatInternal(&GraphEditor, Graph, Geometry, Scope, bRouteWires, Settings); }

FK2FormatResult FK2GraphFormatter::Format(
	UEdGraph& Graph,
	const FGraphGeometrySnapshot& Geometry,
	const TSet<UEdGraphNode*>& Scope,
	const bool bRouteWires,
	const UFormatterSettings& Settings
)
{ return FormatInternal(nullptr, Graph, Geometry, Scope, bRouteWires, Settings); }

FK2FormatResult FK2GraphFormatter::FormatInternal(
	SGraphEditor* GraphEditor,
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
	if (GraphEditor != nullptr)
	{
		for (UObject* SelectedObject : GraphEditor->GetSelectedNodes())
		{
			if (UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject))
			{
				OriginalSelection.Add(SelectedNode);
			}
		}
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
		const FString LegacyExpectedKey = FString::Printf(
			TEXT("%s:%s/%s->%s/%s"),
			bExecution ? TEXT("E") : TEXT("D"),
			*MakeLegacyStableNodeKey(Graph, *OutputNode),
			*MakeLegacyStablePinKey(*CandidateRoute.OutputPin, OutputPinIndex),
			*MakeLegacyStableNodeKey(Graph, *InputNode),
			*MakeLegacyStablePinKey(*CandidateRoute.InputPin, InputPinIndex)
		);
		if (ExpectedKey != LogicalEdgeKey && LegacyExpectedKey != LogicalEdgeKey)
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
		// Normalize legacy metadata to the deterministic key in memory. Existing knots remain
		// untouched; if the route is ever rebuilt, the router writes the new identity.
		ValidatedGeneratedRoutes.Add(ExpectedKey, MoveTemp(CandidateRoute));
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
		LayoutNode.OriginalPosition = AdapterNode.OriginalPosition;
		LayoutNode.bHasOriginalPosition = true;
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

	// The layout core intentionally operates only on the requested scope, but links that cross
	// that boundary still constrain readability. Record their stationary outside endpoints for
	// the acceptance gate so formatting a middle node cannot silently bend a straight exec wire.
	TArray<FAdapterEdgeRecord> ReadabilityEdgeRecords = EdgeRecords;
	TSet<FString> ReadabilityEdgeKeys;
	for (const FAdapterEdgeRecord& Edge : ReadabilityEdgeRecords)
	{
		ReadabilityEdgeKeys.Add(Edge.Key);
	}
	TMap<UEdGraphNode*, FVector2D> StationaryReadabilityPositions;
	const auto EnsurePinRecord = [&Geometry, &PinRecords](UEdGraphPin& RequiredPin) -> const FAdapterPinRecord*
	{
		if (const FAdapterPinRecord* Existing = PinRecords.Find(&RequiredPin)) { return Existing; }
		UEdGraphNode* Node = RequiredPin.GetOwningNodeUnchecked();
		if (Node == nullptr) { return nullptr; }

		const FVector2D NodeSize = ResolveNodeSize(*Node, Geometry);
		int32 InputOrdinal = 0;
		int32 OutputOrdinal = 0;
		int32 ExecutionInputOrdinal = 0;
		int32 ExecutionOutputOrdinal = 0;
		for (int32 PinOrdinal = 0; PinOrdinal < Node->Pins.Num(); ++PinOrdinal)
		{
			UEdGraphPin* Pin = Node->Pins[PinOrdinal];
			if (Pin == nullptr || (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output)) { continue; }
			const bool bInput = Pin->Direction == EGPD_Input;
			const bool bExecution = IsExecutionPin(*Pin);
			const int32 DirectionOrdinal = bInput ? InputOrdinal++ : OutputOrdinal++;
			int32 KindOrdinal = DirectionOrdinal;
			if (bExecution) { KindOrdinal = bInput ? ExecutionInputOrdinal++ : ExecutionOutputOrdinal++; }
			if (Pin != &RequiredPin) { continue; }

			FAdapterPinRecord Record;
			Record.Key = MakeStablePinKey(*Pin, PinOrdinal);
			Record.Offset = ResolvePinOffset(*Pin, Geometry, NodeSize, DirectionOrdinal);
			Record.SemanticOrder = DirectionOrdinal;
			Record.KindOrder = KindOrdinal;
			Record.bExecution = bExecution;
			Record.bPreferredExecutionPort = bExecution && KindOrdinal == 0;
			PinRecords.Add(Pin, MoveTemp(Record));
			return PinRecords.Find(Pin);
		}
		return nullptr;
	};

	for (const FAdapterNodeRecord& ScopeNode : NodeRecords)
	{
		for (UEdGraphPin* ScopePin : ScopeNode.Node->Pins)
		{
			if (ScopePin == nullptr || (ScopePin->Direction != EGPD_Input && ScopePin->Direction != EGPD_Output))
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : ScopePin->LinkedTo)
			{
				UEdGraphNode* OutsideNode = LinkedPin != nullptr ? LinkedPin->GetOwningNodeUnchecked() : nullptr;
				if (OutsideNode == nullptr || OutsideNode->GetGraph() != &Graph
					|| NodeRecordIndices.Contains(OutsideNode) || ValidatedGeneratedKnots.Contains(OutsideNode))
				{
					continue;
				}
				UEdGraphPin* OutputPin = ScopePin->Direction == EGPD_Output ? ScopePin : LinkedPin;
				UEdGraphPin* InputPin = ScopePin->Direction == EGPD_Input ? ScopePin : LinkedPin;
				if (OutputPin == nullptr || InputPin == nullptr || OutputPin->Direction != EGPD_Output
					|| InputPin->Direction != EGPD_Input)
				{
					continue;
				}
				if (EnsurePinRecord(*OutputPin) == nullptr || EnsurePinRecord(*InputPin) == nullptr) { continue; }
				// Reacquire both pointers after insertion because TMap growth may relocate values.
				const FAdapterPinRecord* OutputRecord = PinRecords.Find(OutputPin);
				const FAdapterPinRecord* InputRecord = PinRecords.Find(InputPin);
				if (OutputRecord == nullptr || InputRecord == nullptr || OutputRecord->bExecution != InputRecord->bExecution)
				{
					continue;
				}

				FAdapterEdgeRecord BoundaryEdge;
				BoundaryEdge.OutputPin = OutputPin;
				BoundaryEdge.InputPin = InputPin;
				BoundaryEdge.bExecution = OutputRecord->bExecution;
				BoundaryEdge.BranchOrder = BoundaryEdge.bExecution ? OutputRecord->KindOrder : INDEX_NONE;
				// Every execution link crossing an explicit-selection boundary is fixed context.
				// Preserve an authored straight boundary segment even when it is not a primary branch pin.
				BoundaryEdge.bPreferredAlignment = BoundaryEdge.bExecution;
				BoundaryEdge.Key = FString::Printf(
					TEXT("%s:%s/%s->%s/%s"),
					BoundaryEdge.bExecution ? TEXT("E") : TEXT("D"),
					*MakeStableNodeKey(Graph, *OutputPin->GetOwningNodeUnchecked()),
					*OutputRecord->Key,
					*MakeStableNodeKey(Graph, *InputPin->GetOwningNodeUnchecked()),
					*InputRecord->Key
				);
				if (ReadabilityEdgeKeys.Contains(BoundaryEdge.Key)) { continue; }
				ReadabilityEdgeKeys.Add(BoundaryEdge.Key);
				ReadabilityEdgeRecords.Add(MoveTemp(BoundaryEdge));
				StationaryReadabilityPositions.Add(
					OutsideNode,
					FVector2D(static_cast<double>(OutsideNode->NodePosX), static_cast<double>(OutsideNode->NodePosY))
				);
			}
		}
	}
	// A partial selection can also move beneath a completely stationary outside-to-outside wire.
	// Include every remaining direct logical edge in the graph-wide readability field. Validated
	// formatter knots are collapsed separately below; user-authored knots remain ordinary nodes.
	for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
	{
		UEdGraphNode* OutputNode = GraphNodePointer.Get();
		if (OutputNode == nullptr || OutputNode->IsA<UEdGraphNode_Comment>()
			|| ValidatedGeneratedKnots.Contains(OutputNode))
		{
			continue;
		}
		for (UEdGraphPin* OutputPin : OutputNode->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (UEdGraphPin* InputPin : OutputPin->LinkedTo)
			{
				UEdGraphNode* InputNode = InputPin != nullptr ? InputPin->GetOwningNodeUnchecked() : nullptr;
				if (InputPin == nullptr || InputPin->Direction != EGPD_Input || InputNode == nullptr
					|| InputNode->GetGraph() != &Graph || InputNode->IsA<UEdGraphNode_Comment>()
					|| ValidatedGeneratedKnots.Contains(InputNode))
				{
					continue;
				}
				if (EnsurePinRecord(*OutputPin) == nullptr || EnsurePinRecord(*InputPin) == nullptr) { continue; }
				const FAdapterPinRecord* OutputRecord = PinRecords.Find(OutputPin);
				const FAdapterPinRecord* InputRecord = PinRecords.Find(InputPin);
				if (OutputRecord == nullptr || InputRecord == nullptr || OutputRecord->bExecution != InputRecord->bExecution)
				{
					continue;
				}

				const FString EdgeKey = FString::Printf(
					TEXT("%s:%s/%s->%s/%s"),
					OutputRecord->bExecution ? TEXT("E") : TEXT("D"),
					*MakeStableNodeKey(Graph, *OutputNode),
					*OutputRecord->Key,
					*MakeStableNodeKey(Graph, *InputNode),
					*InputRecord->Key
				);
				if (ReadabilityEdgeKeys.Contains(EdgeKey)) { continue; }

				FAdapterEdgeRecord& ReadabilityEdge = ReadabilityEdgeRecords.AddDefaulted_GetRef();
				ReadabilityEdge.OutputPin = OutputPin;
				ReadabilityEdge.InputPin = InputPin;
				ReadabilityEdge.Key = EdgeKey;
				ReadabilityEdge.bExecution = OutputRecord->bExecution;
				ReadabilityEdge.BranchOrder = ReadabilityEdge.bExecution ? OutputRecord->KindOrder : INDEX_NONE;
				ReadabilityEdge.bPreferredAlignment = ReadabilityEdge.bExecution && OutputRecord->bPreferredExecutionPort
												   && InputRecord->bPreferredExecutionPort;
				ReadabilityEdgeKeys.Add(EdgeKey);
			}
		}
	}
	for (const TPair<FString, FValidatedGeneratedRoute>& RoutePair : ValidatedGeneratedRoutes)
	{
		const FValidatedGeneratedRoute& Route = RoutePair.Value;
		UEdGraphNode* OutputNode = Route.OutputPin != nullptr ? Route.OutputPin->GetOwningNodeUnchecked() : nullptr;
		UEdGraphNode* InputNode = Route.InputPin != nullptr ? Route.InputPin->GetOwningNodeUnchecked() : nullptr;
		if (OutputNode == nullptr || InputNode == nullptr) { continue; }
		const bool bOutputInScope = NodeRecordIndices.Contains(OutputNode);
		const bool bInputInScope = NodeRecordIndices.Contains(InputNode);
		if (ReadabilityEdgeKeys.Contains(RoutePair.Key)) { continue; }
		if (EnsurePinRecord(*Route.OutputPin) == nullptr || EnsurePinRecord(*Route.InputPin) == nullptr) { continue; }
		const FAdapterPinRecord* OutputRecord = PinRecords.Find(Route.OutputPin);
		const FAdapterPinRecord* InputRecord = PinRecords.Find(Route.InputPin);
		if (OutputRecord == nullptr || InputRecord == nullptr || OutputRecord->bExecution != InputRecord->bExecution)
		{
			continue;
		}

		FAdapterEdgeRecord BoundaryEdge;
		BoundaryEdge.OutputPin = Route.OutputPin;
		BoundaryEdge.InputPin = Route.InputPin;
		BoundaryEdge.Key = RoutePair.Key;
		BoundaryEdge.ExistingGeneratedKnots = Route.Knots;
		BoundaryEdge.ExistingRouteWaypoints = Route.Waypoints;
		BoundaryEdge.bExecution = OutputRecord->bExecution;
		BoundaryEdge.bExistingGeneratedRoute = true;
		BoundaryEdge.BranchOrder = BoundaryEdge.bExecution ? OutputRecord->KindOrder : INDEX_NONE;
		BoundaryEdge.bPreferredAlignment = BoundaryEdge.bExecution;
		ReadabilityEdgeKeys.Add(BoundaryEdge.Key);
		ReadabilityEdgeRecords.Add(MoveTemp(BoundaryEdge));
		if (bOutputInScope != bInputInScope)
		{
			UEdGraphNode* OutsideNode = bOutputInScope ? InputNode : OutputNode;
			StationaryReadabilityPositions.Add(
				OutsideNode,
				FVector2D(static_cast<double>(OutsideNode->NodePosX), static_cast<double>(OutsideNode->NodePosY))
			);
		}
	}

	// Stationary graph nodes still matter to readability: a moved selected wire must not
	// start travelling beneath an unselected node, and selected roots must not cross an
	// unselected event root. Keep this graph-wide view separate from the layout scope.
	TArray<FAdapterNodeRecord> ReadabilityNodeRecords = NodeRecords;
	TSet<UEdGraphNode*> ReadabilityNodes;
	for (const FAdapterNodeRecord& Node : NodeRecords)
	{
		ReadabilityNodes.Add(Node.Node);
	}
	for (const TObjectPtr<UEdGraphNode>& GraphNodePointer : Graph.Nodes)
	{
		UEdGraphNode* GraphNode = GraphNodePointer.Get();
		if (GraphNode == nullptr || GraphNode->IsA<UEdGraphNode_Comment>() || ReadabilityNodes.Contains(GraphNode))
		{
			continue;
		}
		FAdapterNodeRecord& Record = ReadabilityNodeRecords.AddDefaulted_GetRef();
		Record.Node = GraphNode;
		Record.Key = MakeStableNodeKey(Graph, *GraphNode);
		Record.OriginalPosition =
			FVector2D(static_cast<double>(GraphNode->NodePosX), static_cast<double>(GraphNode->NodePosY));
		Record.Size = ResolveNodeSize(*GraphNode, Geometry);
		StationaryReadabilityPositions.Add(GraphNode, Record.OriginalPosition);
	}
	ReadabilityNodeRecords.Sort([](const FAdapterNodeRecord& Left, const FAdapterNodeRecord& Right)
								{ return Left.Key < Right.Key; });
	ReadabilityEdgeRecords.Sort([](const FAdapterEdgeRecord& Left, const FAdapterEdgeRecord& Right)
								{ return Left.Key < Right.Key; });

	const double GridSize = FMath::Max(1.0, static_cast<double>(SNodePanel::GetSnapGridSize()));
	const double LayoutCellSize = ResolveLayoutCellSize(Settings, GridSize);
	const bool bPreserveHumanLayout = Settings.K2LayoutMode == EGraphFormatterK2LayoutMode::PreserveHumanLayout;
	K2Layout::FLayoutSettings LayoutSettings;
	LayoutSettings.HorizontalSpacing = FMath::Max(static_cast<double>(Settings.K2HorizontalSpacing), LayoutCellSize);
	LayoutSettings.VerticalSpacing = FMath::Max(static_cast<double>(Settings.K2VerticalSpacing), LayoutCellSize);
	LayoutSettings.BranchSpacing = FMath::Max(static_cast<double>(Settings.K2BranchSpacing), LayoutCellSize);
	LayoutSettings.PureNodeHorizontalSpacing =
		FMath::Max(static_cast<double>(Settings.K2PureHorizontalSpacing), LayoutCellSize);
	LayoutSettings.PureNodeVerticalSpacing =
		FMath::Max(static_cast<double>(Settings.K2PureVerticalSpacing), LayoutCellSize);
	LayoutSettings.CollisionClearance = FMath::Max(static_cast<double>(Settings.K2ObstacleClearance), LayoutCellSize);
	const double ComponentSpacing = FMath::Max(static_cast<double>(Settings.K2ComponentSpacing), LayoutCellSize);
	LayoutSettings.ComponentSpacing = FVector2D(ComponentSpacing, ComponentSpacing);
	LayoutSettings.GridSize = FVector2D(GridSize, GridSize);
	LayoutSettings.LayoutCellSize = LayoutCellSize;
	LayoutSettings.OrderingSweeps = Settings.K2OrderingSweeps;
	LayoutSettings.AdjacentSwapPasses = FMath::Clamp(Settings.K2OrderingSweeps / 3, 1, 12);
	LayoutSettings.GridPolicy = Settings.bEnableHybridGridSnap ? K2Layout::EGridPolicy::HybridExecution
															   : K2Layout::EGridPolicy::NodeGrid;
	LayoutSettings.LayoutMode = bPreserveHumanLayout ? K2Layout::ELayoutMode::PreserveAuthored
													 : K2Layout::ELayoutMode::Reflow;

	K2Layout::FLayoutPlan LayoutPlan = K2Layout::BuildLayout(LayoutSnapshot, LayoutSettings);
	if (bPreserveHumanLayout && !LayoutPlan.HasErrors() && LayoutPlan.Nodes.Num() == LayoutSnapshot.Nodes.Num())
	{
		// BuildLayout is pure, so refine the authored snapshot in memory until it reaches the same
		// fixed point a user would otherwise get by clicking Format Graph repeatedly. This is
		// especially important for provider placement: moving its consumer can change which
		// collision-free shelf is preferred on the next invocation.
		constexpr int32 MaximumAuthoredRefinementPasses = 8;
		for (int32 Pass = 0; Pass < MaximumAuthoredRefinementPasses; ++Pass)
		{
			TMap<FString, FVector2D> CurrentPositionsByKey;
			for (const K2Layout::FPlannedNodePosition& PlannedNode : LayoutPlan.Nodes)
			{
				CurrentPositionsByKey.Add(PlannedNode.Node.Value, PlannedNode.Position);
			}

			K2Layout::FGraphSnapshot RefinedSnapshot = LayoutSnapshot;
			bool bCompleteSnapshot = true;
			for (K2Layout::FNodeSnapshot& Node : RefinedSnapshot.Nodes)
			{
				const FVector2D* Position = CurrentPositionsByKey.Find(Node.Key.Value);
				if (Position == nullptr)
				{
					bCompleteSnapshot = false;
					break;
				}
				Node.OriginalPosition = *Position;
				Node.bHasOriginalPosition = true;
			}
			if (!bCompleteSnapshot) { break; }

			K2Layout::FLayoutPlan RefinedPlan = K2Layout::BuildLayout(RefinedSnapshot, LayoutSettings);
			if (RefinedPlan.HasErrors() || RefinedPlan.Nodes.Num() != LayoutPlan.Nodes.Num()) { break; }

			bool bPositionsChanged = false;
			for (const K2Layout::FPlannedNodePosition& PlannedNode : RefinedPlan.Nodes)
			{
				const FVector2D* PreviousPosition = CurrentPositionsByKey.Find(PlannedNode.Node.Value);
				if (PreviousPosition == nullptr || !PreviousPosition->Equals(PlannedNode.Position, ReadabilityEpsilon))
				{
					bPositionsChanged = true;
					break;
				}
			}
			LayoutSnapshot = MoveTemp(RefinedSnapshot);
			LayoutPlan = MoveTemp(RefinedPlan);
			if (!bPositionsChanged) { break; }
		}
	}
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
	if (!bPreserveHumanLayout)
	{
		OriginalTopLeft.X = FMath::GridSnap(OriginalTopLeft.X, LayoutCellSize);
		OriginalTopLeft.Y = FMath::GridSnap(OriginalTopLeft.Y, LayoutCellSize);
	}

	FVector2D PlannedTopLeft(DBL_MAX, DBL_MAX);
	for (const K2Layout::FPlannedNodePosition& PlannedNode : LayoutPlan.Nodes)
	{
		PlannedTopLeft.X = FMath::Min(PlannedTopLeft.X, PlannedNode.Position.X);
		PlannedTopLeft.Y = FMath::Min(PlannedTopLeft.Y, PlannedNode.Position.Y);
	}
	// Preserve mode is already expressed in authored graph coordinates by the core. A second global
	// translation would destroy its per-island anchors. Full Reflow gets one scope-level anchor so
	// an explicitly aggressive redraw still does not teleport the selected graph to the origin.
	const FVector2D AnchorOffset = bPreserveHumanLayout ? FVector2D::ZeroVector : OriginalTopLeft - PlannedTopLeft;

	TArray<FPlannedAdapterNode> PlannedNodes;
	PlannedNodes.Reserve(LayoutPlan.Nodes.Num());
	TMap<UEdGraphNode*, int32> PlannedNodeIndices;
	TMap<int32, FBox2D> ComponentBaseBounds;
	TMap<int32, TArray<FBox2D>> ComponentNodeBaseBounds;
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
		// User-authored reroutes are routing controls, not semantic providers. Preserve their selected
		// horizontal channel here; after semantic columns settle, a separate deterministic pass aligns
		// only their X coordinate to the relevant input/output pin column. Reflow remains explicitly
		// free to redraw both axes.
		PlannedNode.bPreserveAuthoredRerouteChannel = bPreserveHumanLayout && Node->IsA<UK2Node_Knot>();
		PlannedNode.Position = PlannedNode.bPreserveAuthoredRerouteChannel ? AdapterNode.OriginalPosition
																		   : LayoutNode.Position + AnchorOffset;
		PlannedNode.Position.X = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.X));
		PlannedNode.Position.Y = static_cast<double>(FMath::RoundToInt(PlannedNode.Position.Y));
		PlannedNode.Size = AdapterNode.Size;
		PlannedNode.ComponentIndex = LayoutNode.ComponentIndex >= 0 ? LayoutNode.ComponentIndex : 1000000 + PlannedIndex;
		PlannedNode.ExecutionRank = LayoutNode.ExecutionRank;
		PlannedNodeIndices.Add(Node, PlannedIndex);
		const FBox2D NodeBounds = MakeNodeBox(PlannedNode.Position, PlannedNode.Size);
		ComponentNodeBaseBounds.FindOrAdd(PlannedNode.ComponentIndex).Add(NodeBounds);
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
	const double PlacementClearance = FMath::Max(static_cast<double>(Settings.K2ObstacleClearance), LayoutCellSize);
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
		FixedObstacles.Add(InflateBox(ResolveCurrentNodeBounds(*GraphNode, Geometry), PlacementClearance));
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
			for (const FBox2D& NodeBaseBounds : ComponentNodeBaseBounds.FindChecked(ComponentIndex))
			{
				const FBox2D CandidateNode = NodeBaseBounds.ShiftBy(FVector2D(0.0, VerticalShift));
				for (const FBox2D& Obstacle : FixedObstacles)
				{
					if (HasPositiveAreaIntersection(CandidateNode, Obstacle))
					{
						bIntersects = true;
						RequiredShift = FMath::Max(RequiredShift, Obstacle.Max.Y + LayoutCellSize - NodeBaseBounds.Min.Y);
					}
				}
			}
			// Components are processed in authored top-to-bottom order. Only paragraphs that
			// have already been placed are obstacles here; treating a later paragraph's
			// unresolved authored bounds as fixed can push an earlier event graph below it
			// and invert the layout the user authored.
			for (const TPair<int32, FBox2D>& ResolvedComponent : ResolvedComponentBounds)
			{
				const FBox2D Obstacle = InflateBox(ResolvedComponent.Value, PlacementClearance);
				if (HasPositiveAreaIntersection(Candidate, Obstacle))
				{
					bIntersects = true;
					RequiredShift = FMath::Max(RequiredShift, Obstacle.Max.Y + LayoutCellSize - BaseBounds.Min.Y);
				}
			}

			if (!bIntersects)
			{
				bResolved = true;
				break;
			}
			VerticalShift = SnapUp(FMath::Max(VerticalShift + LayoutCellSize, RequiredShift), LayoutCellSize);
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
	}
	if (bPreserveHumanLayout)
	{
		const TMap<UEdGraphNode*, FVector2D> RerouteAlignments =
			BuildAuthoredRerouteColumnAlignments(NodeRecords, EdgeRecords, PinRecords, FinalNodePositions);
		for (const TPair<UEdGraphNode*, FVector2D>& Alignment : RerouteAlignments)
		{
			FinalNodePositions.FindChecked(Alignment.Key) = Alignment.Value;
		}
	}

	// Generated knots are presentation artifacts and do not participate in semantic ranking. Plan
	// their movement before the readability gate so both the gate and the router inspect the exact
	// endpoint-to-waypoint polyline that will be committed. Interpolating the endpoint deltas keeps
	// a validated authored chain attached without changing pin topology.
	TMap<UEdGraphNode*, FVector2D> PlannedGeneratedKnotPositions;
	TMap<FString, TArray<FVector2D>> PlannedGeneratedRouteWaypoints;
	for (FAdapterEdgeRecord& Edge : EdgeRecords)
	{
		Edge.PlannedRouteWaypoints.Reset();
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
		Edge.PlannedRouteWaypoints.Reserve(Edge.ExistingGeneratedKnots.Num());
		for (int32 KnotIndex = 0; KnotIndex < Edge.ExistingGeneratedKnots.Num(); ++KnotIndex)
		{
			UEdGraphNode* Knot = Edge.ExistingGeneratedKnots[KnotIndex];
			const FVector2D OriginalKnotTopLeft(static_cast<double>(Knot->NodePosX), static_cast<double>(Knot->NodePosY));
			const FVector2D CenterOffset = Edge.ExistingRouteWaypoints[KnotIndex] - OriginalKnotTopLeft;
			const double Alpha = static_cast<double>(KnotIndex + 1)
							   / static_cast<double>(Edge.ExistingGeneratedKnots.Num() + 1);
			const FVector2D InterpolatedDelta = FMath::Lerp(OutputDelta, InputDelta, Alpha);
			// Preserve the router's exact first-pass waypoint. Snapping the absolute center here can
			// shift an already valid knot on pass two when an endpoint-aligned channel is intentionally
			// off the editor's fine grid. Snap only the displacement caused by endpoint movement.
			const FVector2D SnappedDelta(
				FMath::GridSnap(InterpolatedDelta.X, GridSize), FMath::GridSnap(InterpolatedDelta.Y, GridSize)
			);
			const FVector2D PlannedCenter = Edge.ExistingRouteWaypoints[KnotIndex] + SnappedDelta;
			const FVector2D UnroundedKnotTopLeft = PlannedCenter - CenterOffset;
			const FVector2D PlannedKnotTopLeft{
				static_cast<double>(FMath::RoundToInt(UnroundedKnotTopLeft.X)),
				static_cast<double>(FMath::RoundToInt(UnroundedKnotTopLeft.Y)),
			};
			// NodePos is integer-valued. Derive the measured waypoint from that exact applied
			// top-left so the acceptance gate and the committed graph cannot disagree by a pixel.
			const FVector2D AppliedCenter = PlannedKnotTopLeft + CenterOffset;

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
			}
			Edge.PlannedRouteWaypoints.Add(AppliedCenter);
		}
		PlannedGeneratedRouteWaypoints.Add(Edge.Key, Edge.PlannedRouteWaypoints);
	}
	for (FAdapterEdgeRecord& ReadabilityEdge : ReadabilityEdgeRecords)
	{
		if (const TArray<FVector2D>* PlannedWaypoints = PlannedGeneratedRouteWaypoints.Find(ReadabilityEdge.Key))
		{
			ReadabilityEdge.PlannedRouteWaypoints = *PlannedWaypoints;
		}
	}

	TMap<UEdGraphNode*, FVector2D> OriginalNodePositions;
	OriginalNodePositions.Reserve(NodeRecords.Num() + StationaryReadabilityPositions.Num());
	for (const FAdapterNodeRecord& Node : NodeRecords)
	{
		OriginalNodePositions.Add(Node.Node, Node.OriginalPosition);
	}
	TMap<UEdGraphNode*, FVector2D> CandidateReadabilityPositions = FinalNodePositions;
	for (const TPair<UEdGraphNode*, FVector2D>& StationaryPosition : StationaryReadabilityPositions)
	{
		OriginalNodePositions.Add(StationaryPosition.Key, StationaryPosition.Value);
		if (!CandidateReadabilityPositions.Contains(StationaryPosition.Key))
		{
			CandidateReadabilityPositions.Add(StationaryPosition.Key, StationaryPosition.Value);
		}
	}
	const FReadabilityMetrics OriginalReadability = MeasureReadability(
		ReadabilityNodeRecords, ReadabilityEdgeRecords, PinRecords, OriginalNodePositions, Settings.K2RoutingPlanningWorkBudget, LayoutCellSize
	);
	const FReadabilityEvaluationContext ReadabilityContext{
		ReadabilityNodeRecords,
		NodeRecords,
		ReadabilityEdgeRecords,
		PinRecords,
		OriginalNodePositions,
		OriginalReadability,
		Settings.K2RoutingPlanningWorkBudget,
		LayoutCellSize,
		bPreserveHumanLayout,
	};
	FCandidateReadabilityEvaluation CandidateEvaluation =
		EvaluateReadabilityCandidate(ReadabilityContext, CandidateReadabilityPositions, true);
	FReadabilityMetrics CandidateReadability = CandidateEvaluation.Metrics;
	FString ReadabilityRegression = CandidateEvaluation.Regression;
	const FString PrimaryReadabilityRegression = ReadabilityRegression;
	const bool bPrimaryLayoutRejected = !PrimaryReadabilityRegression.IsEmpty();
	bool bUsedConservativeFallback = false;
	if (bPrimaryLayoutRejected && bPreserveHumanLayout)
	{
		const FConservativeFallbackResult Fallback =
			BuildConvergedConservativeFallback(ReadabilityContext, EdgeRecords, CandidateReadabilityPositions);
		if ((Fallback.MovedNodeCount > 0 || bRouteWires) && !Fallback.Readability.bWorkBudgetExhausted)
		{
			bUsedConservativeFallback = true;
			for (TPair<UEdGraphNode*, FVector2D>& PlannedPosition : FinalNodePositions)
			{
				if (const FVector2D* FallbackPosition = Fallback.Positions.Find(PlannedPosition.Key))
				{
					PlannedPosition.Value = *FallbackPosition;
				}
			}
			for (FAdapterEdgeRecord& Edge : EdgeRecords)
			{
				Edge.PlannedRouteWaypoints.Reset();
			}
			for (FAdapterEdgeRecord& Edge : ReadabilityEdgeRecords)
			{
				Edge.PlannedRouteWaypoints.Reset();
			}
			CandidateReadabilityPositions = Fallback.Positions;
			CandidateReadability = Fallback.Readability;
			ReadabilityRegression.Reset();
			Result.Diagnostics.Add(
				FString::Printf(
					TEXT("Primary layout was unsafe because %s; retained %d safe authored-grid move(s)."),
					*PrimaryReadabilityRegression,
					Fallback.MovedNodeCount
				)
			);
			Result.Diagnostics.Append(Fallback.Diagnostics);
		}
	}
	if (!ReadabilityRegression.IsEmpty() && !bRouteWires)
	{
		Result.Status = EK2FormatStatus::NoChanges;
		Result.bSafetyRejected = true;
		Result.Message = TEXT("The readability safety gate kept the authored graph unchanged.");
		Result.Diagnostics.Add(FString::Printf(TEXT("Rejected layout because %s."), *ReadabilityRegression));
		const FString NewCrossingPair =
			FindFirstNewKey(CandidateReadability.ExecutionWireCrossings, OriginalReadability.ExecutionWireCrossings);
		if (!NewCrossingPair.IsEmpty())
		{
			Result.Diagnostics.Add(
				FString::Printf(
					TEXT("First new execution-wire crossing: %s."),
					*DescribeCrossingPair(NewCrossingPair, ReadabilityEdgeRecords)
				)
			);
		}
		if (GraphEditor != nullptr) { RestoreSelection(*GraphEditor, OriginalSelection); }
		return Result;
	}

	NodeChanges.Reset();
	for (const TPair<UEdGraphNode*, FVector2D>& PlannedPosition : FinalNodePositions)
	{
		UEdGraphNode* Node = PlannedPosition.Key;
		if (Node != nullptr
			&& (Node->NodePosX != FMath::RoundToInt(PlannedPosition.Value.X)
				|| Node->NodePosY != FMath::RoundToInt(PlannedPosition.Value.Y)))
		{
			NodeChanges.Add({ Node, PlannedPosition.Value });
		}
	}
	NodeChanges.Sort([&Graph](const FNodePositionChange& Left, const FNodePositionChange& Right)
					 { return MakeStableNodeKey(Graph, *Left.Node) < MakeStableNodeKey(Graph, *Right.Node); });

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
	FReroutePlan ReroutePlan;
	int32 SafetySkippedRerouteWireCount = 0;
	if (bRouteWires)
	{
		// Seed routing with the complete graph-wide wire field used by the readability gate.
		// Out-of-scope edges are skipped for mutation by the router, but their rendered paths
		// remain reservations so a newly routed selected wire cannot cross them after acceptance.
		RerouteEdges.Reserve(ReadabilityEdgeRecords.Num() + ValidatedGeneratedRoutes.Num());
		TSet<FString> AddedRerouteKeys;
		for (const FAdapterEdgeRecord& Edge : ReadabilityEdgeRecords)
		{
			UEdGraphNode* OutputNode = Edge.OutputPin->GetOwningNodeUnchecked();
			UEdGraphNode* InputNode = Edge.InputPin->GetOwningNodeUnchecked();
			const FVector2D* OutputPosition = CandidateReadabilityPositions.Find(OutputNode);
			const FVector2D* InputPosition = CandidateReadabilityPositions.Find(InputNode);
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
			RerouteEdge.bReverseOutputTangent =
				ShouldReverseKnotTangent(OutputNode, PinRecords, CandidateReadabilityPositions);
			RerouteEdge.bReverseInputTangent =
				ShouldReverseKnotTangent(InputNode, PinRecords, CandidateReadabilityPositions);
			RerouteEdge.bExistingGeneratedRoute = Edge.bExistingGeneratedRoute;
			RerouteEdge.bReservationOnly = !Scope.Contains(OutputNode) || !Scope.Contains(InputNode);
			if (Edge.bExistingGeneratedRoute)
			{
				RerouteEdge.PreferredWaypoints = !Edge.PlannedRouteWaypoints.IsEmpty() ? Edge.PlannedRouteWaypoints
																					   : Edge.ExistingRouteWaypoints;
				if (RerouteEdge.PreferredWaypoints.IsEmpty())
				{
					Result.Diagnostics.Add(
						FString::Printf(TEXT("Could not validate the generated reroute chain for '%s'."), *Edge.Key)
					);
				}
			}
			else if (Edge.bExecution && !bUsedConservativeFallback)
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
			const int32* OutputRank = ExecutionRanks.Find(OutputNode);
			const int32* InputRank = ExecutionRanks.Find(InputNode);
			if (OutputRank != nullptr && InputRank != nullptr && *OutputRank != INDEX_NONE && *InputRank != INDEX_NONE)
			{
				RerouteEdge.RankSpan = FMath::Abs(*InputRank - *OutputRank);
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
			Reservation.bReverseOutputTangent =
				ShouldReverseKnotTangent(OutputNode, PinRecords, CandidateReadabilityPositions);
			Reservation.bReverseInputTangent =
				ShouldReverseKnotTangent(InputNode, PinRecords, CandidateReadabilityPositions);
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

	if (bRouteWires)
	{
		const FReadabilityMetrics PreRoutingReadability = CandidateReadability;
		FRerouteSettings RerouteSettings;
		RerouteSettings.ObstacleClearance = static_cast<double>(Settings.K2ObstacleClearance);
		RerouteSettings.ChannelSpacing = static_cast<double>(Settings.K2RoutingChannelSpacing);
		RerouteSettings.MaxKnotsPerWire = Settings.K2MaxGeneratedKnots;
		RerouteSettings.LongDataWireRankThreshold = Settings.K2LongDataWireRankThreshold;
		RerouteSettings.PlanningWorkBudget = Settings.K2RoutingPlanningWorkBudget;
		RerouteSettings.bRouteDataWires = Settings.bRouteDataWires;
		ReroutePlan = FK2RerouteRouter::Plan(RerouteEdges, RerouteObstacles, Scope, RerouteSettings, GridSize);
		for (const FPlannedReroute& PlannedRoute : ReroutePlan.Routes)
		{
			FAdapterEdgeRecord* ReadabilityEdge =
				ReadabilityEdgeRecords.FindByPredicate([&PlannedRoute](const FAdapterEdgeRecord& Edge)
													   { return Edge.Key == PlannedRoute.Edge.StableKey; });
			if (ReadabilityEdge != nullptr) { ReadabilityEdge->PlannedRouteWaypoints = PlannedRoute.Waypoints; }
		}

		CandidateEvaluation = EvaluateReadabilityCandidate(ReadabilityContext, CandidateReadabilityPositions, true, true);
		CandidateReadability = CandidateEvaluation.Metrics;
		ReadabilityRegression = CandidateEvaluation.Regression;
		if (!ReadabilityRegression.IsEmpty())
		{
			Result.Diagnostics.Append(ReroutePlan.Diagnostics);
			Result.Diagnostics.Add(FString::Printf(TEXT("Skipped routed layout because %s."), *ReadabilityRegression));
			const FString NewCrossingPair =
				FindFirstNewKey(CandidateReadability.ExecutionWireCrossings, OriginalReadability.ExecutionWireCrossings);
			if (!NewCrossingPair.IsEmpty())
			{
				Result.Diagnostics.Add(
					FString::Printf(
						TEXT("First new routed execution-wire crossing: %s."),
						*DescribeCrossingPair(NewCrossingPair, ReadabilityEdgeRecords)
					)
				);
			}
			if (!NodeChanges.IsEmpty() || !CommentChanges.IsEmpty())
			{
				SafetySkippedRerouteWireCount = ReroutePlan.Routes.Num() + ReroutePlan.SkippedWires;
				ReroutePlan = FReroutePlan();
				CandidateReadability = PreRoutingReadability;
				ReadabilityRegression.Reset();
				Result.Diagnostics.Add(TEXT("Kept the accepted node layout and discarded only the unsafe reroute plan."));
			}
			else
			{
				Result.Status = EK2FormatStatus::NoChanges;
				Result.bSafetyRejected = true;
				Result.Message =
					TEXT("No safe node-layout or reroute improvement was available; the authored graph was unchanged.");
				if (GraphEditor != nullptr) { RestoreSelection(*GraphEditor, OriginalSelection); }
				return Result;
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
		RerouteResult = FK2RerouteRouter::ApplyPlan(Graph, ReroutePlan);
		Result.RoutedWireCount = RerouteResult.RoutedWires;
		Result.CreatedKnotCount = RerouteResult.CreatedKnots;
		Result.SkippedRerouteWireCount = SafetySkippedRerouteWireCount + RerouteResult.SkippedWires;
		Result.Diagnostics.Append(RerouteResult.Diagnostics);
	}

	Result.MovedNodeCount = NodeChanges.Num();
	Result.ResizedCommentCount = CommentChanges.Num();
	if (!Result.WasModified() && !RerouteResult.HasFatalError())
	{
		Transaction.Cancel();
		if (GraphPackage != nullptr && !bPackageWasDirty) { GraphPackage->SetDirtyFlag(false); }
		Result.Status = EK2FormatStatus::NoChanges;
		Result.bSafetyRejected = bPrimaryLayoutRejected;
		if (bPrimaryLayoutRejected)
		{
			Result.Message = TEXT("No safe formatting improvement was available; the authored graph was unchanged.");
		}
		else
		{
			Result.Message = bRouteWires ? TEXT("The graph is already formatted and no wires required safe rerouting.")
										 : TEXT("The graph is already formatted.");
		}
		if (GraphEditor != nullptr) { RestoreSelection(*GraphEditor, OriginalSelection); }
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
	if (GraphEditor != nullptr) { RestoreSelection(*GraphEditor, OriginalSelection); }
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
