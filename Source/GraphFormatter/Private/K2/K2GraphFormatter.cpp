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
	double BackwardExecutionDistance = 0.0;
	int32 NonStraightPreferredExecutionEdgeCount = 0;
	TSet<FString> NonStraightPreferredExecutionEdges;
	double PreferredExecutionVerticalError = 0.0;
	int32 BackwardDataEdgeCount = 0;
	double BackwardDataDistance = 0.0;
	TSet<FString> WiresThroughNodes;
	TSet<FString> ExecutionWireCrossings;
	TSet<FString> DataWireCrossings;
	double MaximumExecutionRootHorizontalDrift = 0.0;
	double MaximumExecutionRootVerticalDrift = 0.0;
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

void MeasureNodeReadability(
	TConstArrayView<FAdapterNodeRecord> Nodes,
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
			if (HasPositiveAreaIntersection(FirstBounds, SecondBounds))
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

void MeasureWireReadability(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	TConstArrayView<FAdapterEdgeRecord> Edges,
	const TMap<const UEdGraphPin*, FAdapterPinRecord>& PinRecords,
	const TMap<UEdGraphNode*, FVector2D>& Positions,
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
			if (!FK2RerouteRouter::RenderedPolylinesIntersectExceptAtSharedTerminals(
					First.RenderedPoints, Second.RenderedPoints, IgnoredSharedTerminals
				))
			{
				continue;
			}

			const bool bFirstKeyFirst = First.Key.Compare(Second.Key, ESearchCase::CaseSensitive) < 0;
			const FString PairKey = bFirstKeyFirst ? First.Key + TEXT("|") + Second.Key
												   : Second.Key + TEXT("|") + First.Key;
			if (First.bExecution || Second.bExecution) { OutMetrics.ExecutionWireCrossings.Add(PairKey); }
			else
			{
				OutMetrics.DataWireCrossings.Add(PairKey);
			}
		}
	}
}

void MeasureExecutionRootMovement(
	TConstArrayView<FAdapterNodeRecord> Nodes,
	const TMap<UEdGraphNode*, FVector2D>& OriginalPositions,
	const TMap<UEdGraphNode*, FVector2D>& CandidatePositions,
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

	for (UEdGraphNode* Root : ExecutionRoots)
	{
		const FVector2D* OriginalPosition = OriginalPositions.Find(Root);
		const FVector2D* CandidatePosition = CandidatePositions.Find(Root);
		if (OriginalPosition == nullptr || CandidatePosition == nullptr) { continue; }
		OutMetrics.MaximumExecutionRootHorizontalDrift = FMath::Max(
			OutMetrics.MaximumExecutionRootHorizontalDrift, FMath::Abs(OriginalPosition->X - CandidatePosition->X)
		);
		OutMetrics.MaximumExecutionRootVerticalDrift = FMath::Max(
			OutMetrics.MaximumExecutionRootVerticalDrift, FMath::Abs(OriginalPosition->Y - CandidatePosition->Y)
		);
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
	const bool bUsePlannedRoutes = false
)
{
	FReadabilityMetrics Metrics;
	FReadabilityWorkBudget RemainingWork(WorkBudget);
	MeasureNodeReadability(Nodes, Positions, RemainingWork, Metrics);
	if (!Metrics.bWorkBudgetExhausted)
	{
		MeasureWireReadability(Nodes, Edges, PinRecords, Positions, bUsePlannedRoutes, RemainingWork, Metrics);
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
FString FindHardReadabilityRegression(
	const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const double LayoutCellSize
)
{
	if (ContainsNewKey(Candidate.OverlapPairs, Original.OverlapPairs))
	{
		return TEXT("the candidate creates a new node overlap");
	}
	if (ContainsNewKey(Candidate.BackwardExecutionEdges, Original.BackwardExecutionEdges)
		|| Candidate.BackwardExecutionDistance > Original.BackwardExecutionDistance + LayoutCellSize)
	{
		return TEXT("the candidate makes execution flow run farther backward");
	}
	if (ContainsNewKey(Candidate.NonStraightPreferredExecutionEdges, Original.NonStraightPreferredExecutionEdges)
		|| Candidate.PreferredExecutionVerticalError > Original.PreferredExecutionVerticalError + LayoutCellSize)
	{
		return TEXT("the candidate bends a preferred straight execution connection");
	}
	if (ContainsNewKey(Candidate.ExecutionWireCrossings, Original.ExecutionWireCrossings))
	{
		return TEXT("the candidate introduces a new execution-wire crossing pair");
	}
	return FString();
}

[[nodiscard]]
FString FindWireThroughNodeRegression(const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate)
{
	const int32 AllowedIncrease = FMath::Max(1, Original.WiresThroughNodes.Num() / 4);
	if (Candidate.WiresThroughNodes.Num() > Original.WiresThroughNodes.Num() + AllowedIncrease)
	{
		return TEXT("the candidate makes materially more wires pass through nodes");
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
		return TEXT("the candidate introduces materially worse backward data wiring");
	}
	return FString();
}

[[nodiscard]]
FString FindDataCrossingRegression(const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate)
{
	const int32 AllowedIncrease = FMath::Max(1, Original.DataWireCrossings.Num() / 4);
	if (Candidate.DataWireCrossings.Num() > Original.DataWireCrossings.Num() + AllowedIncrease)
	{
		return TEXT("the candidate introduces materially more data-wire crossings");
	}
	return FString();
}

[[nodiscard]]
FString FindRootPreservationRegression(const FReadabilityMetrics& Candidate, const double LayoutCellSize)
{
	const double SnapTolerance = LayoutCellSize * 0.5 + ReadabilityEpsilon;
	if (Candidate.MaximumExecutionRootHorizontalDrift > SnapTolerance)
	{
		return TEXT("the candidate moves an execution graph start horizontally beyond coarse-grid snapping");
	}
	if (Candidate.MaximumExecutionRootVerticalDrift > SnapTolerance)
	{
		return TEXT("the candidate moves an execution graph start vertically beyond coarse-grid snapping");
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
		|| Candidate.BackwardDataEdgeCount < Original.BackwardDataEdgeCount
		|| Candidate.BackwardDataDistance + LayoutCellSize < Original.BackwardDataDistance
		|| Candidate.WiresThroughNodes.Num() < Original.WiresThroughNodes.Num()
		|| Candidate.ExecutionWireCrossings.Num() < Original.ExecutionWireCrossings.Num()
		|| Candidate.DataWireCrossings.Num() < Original.DataWireCrossings.Num();
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
	int32 LargeMovementCount = 0;
	for (const FAdapterNodeRecord& Node : Nodes)
	{
		const FVector2D* CandidatePosition = CandidatePositions.Find(Node.Node);
		if (CandidatePosition != nullptr
			&& FVector2D::Distance(Node.OriginalPosition, *CandidatePosition) > LayoutCellSize + ReadabilityEpsilon)
		{
			++LargeMovementCount;
		}
	}
	if (LargeMovementCount == 0) { return FString(); }

	const bool bReadabilityImproved =
		HasMaterialReadabilityImprovement(OriginalReadability, CandidateReadability, LayoutCellSize);
	// Preserve mode is deliberately conservative. A small graph may move up to three helper nodes
	// when that straightens execution or removes a concrete readability defect. Larger authored
	// paragraphs must retain most of their mental map; a broad canonical redraw belongs to Reflow.
	const int32 AllowedLargeMovementCount = bReadabilityImproved
											  ? FMath::Max(3, FMath::FloorToInt(static_cast<double>(Nodes.Num()) * 0.65))
											  : FMath::Max(1, FMath::FloorToInt(static_cast<double>(Nodes.Num()) * 0.35));
	if (LargeMovementCount > AllowedLargeMovementCount)
	{
		return FString::Printf(
			TEXT("the candidate moves %d of %d authored nodes beyond one %.0f-unit layout cell%s"),
			LargeMovementCount,
			Nodes.Num(),
			LayoutCellSize,
			bReadabilityImproved ? TEXT(" despite its readability gain") : TEXT(" without a material readability gain")
		);
	}
	return FString();
}

[[nodiscard]]
FString FindReadabilityRegression(
	const FReadabilityMetrics& Original, const FReadabilityMetrics& Candidate, const double LayoutCellSize, const bool bPreserveHumanLayout
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
	if (FString Reason = FindWireThroughNodeRegression(Original, Candidate); !Reason.IsEmpty()) { return Reason; }
	if (FString Reason = FindBackwardDataRegression(Original, Candidate, LayoutCellSize); !Reason.IsEmpty())
	{
		return Reason;
	}
	if (FString Reason = FindDataCrossingRegression(Original, Candidate); !Reason.IsEmpty()) { return Reason; }
	if (bPreserveHumanLayout)
	{
		if (FString Reason = FindRootPreservationRegression(Candidate, LayoutCellSize); !Reason.IsEmpty())
		{
			return Reason;
		}
	}
	return FString();
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
	const double LayoutCellSize = FMath::Max(1.0, static_cast<double>(Settings.K2LayoutCellSize));
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
		PlannedNode.Position = LayoutNode.Position + AnchorOffset;
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
		if (PlannedNode.Node->NodePosX != FMath::RoundToInt(PlannedNode.Position.X)
			|| PlannedNode.Node->NodePosY != FMath::RoundToInt(PlannedNode.Position.Y))
		{
			NodeChanges.Add({ PlannedNode.Node, PlannedNode.Position });
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
				if (Knot->NodePosX != FMath::RoundToInt(PlannedKnotTopLeft.X)
					|| Knot->NodePosY != FMath::RoundToInt(PlannedKnotTopLeft.Y))
				{
					NodeChanges.Add({ Knot, PlannedKnotTopLeft });
				}
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
		ReadabilityNodeRecords, ReadabilityEdgeRecords, PinRecords, OriginalNodePositions, Settings.K2RoutingPlanningWorkBudget
	);
	FReadabilityMetrics CandidateReadability = MeasureReadability(
		ReadabilityNodeRecords,
		ReadabilityEdgeRecords,
		PinRecords,
		CandidateReadabilityPositions,
		Settings.K2RoutingPlanningWorkBudget,
		true
	);
	if (!OriginalReadability.bWorkBudgetExhausted && !CandidateReadability.bWorkBudgetExhausted)
	{
		MeasureExecutionRootMovement(
			ReadabilityNodeRecords, OriginalNodePositions, CandidateReadabilityPositions, CandidateReadability
		);
	}
	FString ReadabilityRegression =
		FindReadabilityRegression(OriginalReadability, CandidateReadability, LayoutCellSize, bPreserveHumanLayout);
	const FString AuthoredMovementRegression =
		bPreserveHumanLayout
			? FindAuthoredMovementRegression(
				  NodeRecords, CandidateReadabilityPositions, OriginalReadability, CandidateReadability, LayoutCellSize
			  )
			: FString();
	if (ReadabilityRegression.IsEmpty()) { ReadabilityRegression = AuthoredMovementRegression; }
	if (!ReadabilityRegression.IsEmpty() && !bRouteWires)
	{
		Result.Status = EK2FormatStatus::NoChanges;
		Result.Message = TEXT("The readability safety gate kept the authored graph unchanged.");
		Result.Diagnostics.Add(FString::Printf(TEXT("Rejected layout because %s."), *ReadabilityRegression));
		return Result;
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
	FReroutePlan ReroutePlan;
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

		CandidateReadability = MeasureReadability(
			ReadabilityNodeRecords,
			ReadabilityEdgeRecords,
			PinRecords,
			CandidateReadabilityPositions,
			Settings.K2RoutingPlanningWorkBudget,
			true
		);
		if (!OriginalReadability.bWorkBudgetExhausted && !CandidateReadability.bWorkBudgetExhausted)
		{
			MeasureExecutionRootMovement(
				ReadabilityNodeRecords, OriginalNodePositions, CandidateReadabilityPositions, CandidateReadability
			);
		}
		ReadabilityRegression =
			FindReadabilityRegression(OriginalReadability, CandidateReadability, LayoutCellSize, bPreserveHumanLayout);
		// Routing may improve a wire without justifying a broad redraw of the authored node layout.
		// Keep the movement decision made against the node-layout candidate independent of the later
		// best-effort reroute plan.
		if (ReadabilityRegression.IsEmpty()) { ReadabilityRegression = AuthoredMovementRegression; }
		if (!ReadabilityRegression.IsEmpty())
		{
			Result.Status = EK2FormatStatus::NoChanges;
			Result.Message =
				TEXT("The readability safety gate kept the authored graph unchanged after routing was planned.");
			Result.Diagnostics.Append(ReroutePlan.Diagnostics);
			Result.Diagnostics.Add(FString::Printf(TEXT("Rejected routed layout because %s."), *ReadabilityRegression));
			return Result;
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
