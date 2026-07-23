/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Benchmark/GraphFormatterBenchmarkMetrics.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2/GraphGeometrySnapshot.h"
#include "K2/K2RerouteRouter.h"
#include "K2Node_Knot.h"

namespace GraphFormatter::Benchmark
{
namespace MetricsPrivate
{
constexpr double GeometryEpsilon = 1.0;
constexpr double FallbackNodeWidth = 160.0;
constexpr double FallbackNodeHeight = 80.0;
constexpr double FallbackPinTop = 32.0;
constexpr double FallbackPinPitch = 24.0;

struct FLogicalPath
{
	const UEdGraphPin* Target = nullptr;
	TArray<FVector2D> Waypoints;
};

struct FWireSample
{
	const UEdGraphNode* Source = nullptr;
	const UEdGraphNode* Target = nullptr;
	const UEdGraphPin* OutputPin = nullptr;
	const UEdGraphPin* InputPin = nullptr;
	FVector2D Start = FVector2D::ZeroVector;
	FVector2D End = FVector2D::ZeroVector;
	TArray<FVector2D> RenderedPoints;
	int32 BendCount = 0;
	bool bExecution = false;
	bool bPrimaryExecution = false;
};

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
bool IsExecutionPin(const UEdGraphPin& Pin)
{ return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; }

[[nodiscard]]
bool IsPrimaryExecutionPin(const UEdGraphPin& Pin)
{
	if (!IsExecutionPin(Pin)) { return false; }
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return false; }
	for (const UEdGraphPin* Candidate : Node->Pins)
	{
		if (Candidate != nullptr && Candidate->Direction == Pin.Direction && IsExecutionPin(*Candidate))
		{
			return Candidate == &Pin;
		}
	}
	return false;
}

void ResolveLogicalPaths(
	const UEdGraphPin& CandidateInput,
	const K2::FGraphGeometrySnapshot& Geometry,
	TSet<const UEdGraphPin*>& Traversal,
	TArray<FVector2D>& Waypoints,
	TArray<FLogicalPath>& OutPaths
)
{
	if (Traversal.Contains(&CandidateInput)) { return; }
	Traversal.Add(&CandidateInput);
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(CandidateInput.GetOwningNodeUnchecked());
	if (Knot == nullptr || &CandidateInput != Knot->GetInputPin())
	{
		FLogicalPath& Path = OutPaths.AddDefaulted_GetRef();
		Path.Target = &CandidateInput;
		Path.Waypoints = Waypoints;
		Traversal.Remove(&CandidateInput);
		return;
	}

	Waypoints.Add(ResolvePinAnchor(CandidateInput, Geometry));
	if (const UEdGraphPin* KnotOutput = Knot->GetOutputPin())
	{
		for (const UEdGraphPin* LinkedPin : KnotOutput->LinkedTo)
		{
			if (LinkedPin != nullptr) { ResolveLogicalPaths(*LinkedPin, Geometry, Traversal, Waypoints, OutPaths); }
		}
	}
	Waypoints.Pop(EAllowShrinking::No);
	Traversal.Remove(&CandidateInput);
}

[[nodiscard]]
TArray<FWireSample> CaptureWires(const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry)
{
	TArray<FWireSample> Wires;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>()) { continue; }
		for (const UEdGraphPin* OutputPin : Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				if (LinkedPin == nullptr) { continue; }
				TSet<const UEdGraphPin*> Traversal;
				TArray<FVector2D> Waypoints;
				TArray<FLogicalPath> Paths;
				ResolveLogicalPaths(*LinkedPin, Geometry, Traversal, Waypoints, Paths);
				for (const FLogicalPath& Path : Paths)
				{
					const UEdGraphPin* InputPin = Path.Target;
					const UEdGraphNode* Target = InputPin != nullptr ? InputPin->GetOwningNodeUnchecked() : nullptr;
					if (InputPin == nullptr || Target == nullptr || InputPin->Direction != EGPD_Input) { continue; }
					FWireSample& Wire = Wires.AddDefaulted_GetRef();
					Wire.Source = Node;
					Wire.Target = Target;
					Wire.OutputPin = OutputPin;
					Wire.InputPin = InputPin;
					Wire.Start = ResolvePinAnchor(*OutputPin, Geometry);
					Wire.End = ResolvePinAnchor(*InputPin, Geometry);
					Wire.BendCount = Path.Waypoints.Num();
					Wire.bExecution = IsExecutionPin(*OutputPin) && IsExecutionPin(*InputPin);
					Wire.bPrimaryExecution = Wire.bExecution && IsPrimaryExecutionPin(*OutputPin)
										  && IsPrimaryExecutionPin(*InputPin);
					TArray<FVector2D> LogicalPoints;
					LogicalPoints.Add(Wire.Start);
					LogicalPoints.Append(Path.Waypoints);
					LogicalPoints.Add(Wire.End);
					Wire.RenderedPoints = K2::FK2RerouteRouter::BuildRenderedPolyline(LogicalPoints);
				}
			}
		}
	}
	return Wires;
}

[[nodiscard]]
bool Overlaps(const UEdGraphNode& First, const UEdGraphNode& Second, const K2::FGraphGeometrySnapshot& Geometry)
{
	const FVector2D FirstMin(First.NodePosX, First.NodePosY);
	const FVector2D SecondMin(Second.NodePosX, Second.NodePosY);
	const FVector2D FirstMax = FirstMin + ResolveNodeSize(First, Geometry);
	const FVector2D SecondMax = SecondMin + ResolveNodeSize(Second, Geometry);
	return FirstMin.X < SecondMax.X - GeometryEpsilon && SecondMin.X < FirstMax.X - GeometryEpsilon
		&& FirstMin.Y < SecondMax.Y - GeometryEpsilon && SecondMin.Y < FirstMax.Y - GeometryEpsilon;
}

[[nodiscard]]
double Cross(const FVector2D A, const FVector2D B, const FVector2D C)
{ return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X); }

[[nodiscard]]
bool SegmentsCrossProperly(const FVector2D A, const FVector2D B, const FVector2D C, const FVector2D D)
{
	const double First = Cross(A, B, C);
	const double Second = Cross(A, B, D);
	const double Third = Cross(C, D, A);
	const double Fourth = Cross(C, D, B);
	return ((First > GeometryEpsilon && Second < -GeometryEpsilon)
			|| (First < -GeometryEpsilon && Second > GeometryEpsilon))
		&& ((Third > GeometryEpsilon && Fourth < -GeometryEpsilon)
			|| (Third < -GeometryEpsilon && Fourth > GeometryEpsilon));
}

[[nodiscard]]
bool PointInsideNode(const FVector2D Point, const UEdGraphNode& Node, const K2::FGraphGeometrySnapshot& Geometry)
{
	const FVector2D Minimum(Node.NodePosX, Node.NodePosY);
	const FVector2D Maximum = Minimum + ResolveNodeSize(Node, Geometry);
	return Point.X > Minimum.X + GeometryEpsilon && Point.X < Maximum.X - GeometryEpsilon
		&& Point.Y > Minimum.Y + GeometryEpsilon && Point.Y < Maximum.Y - GeometryEpsilon;
}

[[nodiscard]]
bool SegmentPassesThroughNode(
	const FVector2D Start, const FVector2D End, const UEdGraphNode& Node, const K2::FGraphGeometrySnapshot& Geometry
)
{
	const FVector2D Minimum(Node.NodePosX, Node.NodePosY);
	const FVector2D Maximum = Minimum + ResolveNodeSize(Node, Geometry);
	if (FMath::Max(Start.X, End.X) <= Minimum.X || FMath::Min(Start.X, End.X) >= Maximum.X
		|| FMath::Max(Start.Y, End.Y) <= Minimum.Y || FMath::Min(Start.Y, End.Y) >= Maximum.Y)
	{
		return false;
	}
	if (PointInsideNode((Start + End) * 0.5, Node, Geometry)) { return true; }
	const FVector2D TopRight(Maximum.X, Minimum.Y);
	const FVector2D BottomLeft(Minimum.X, Maximum.Y);
	return SegmentsCrossProperly(Start, End, Minimum, TopRight) || SegmentsCrossProperly(Start, End, TopRight, Maximum)
		|| SegmentsCrossProperly(Start, End, Maximum, BottomLeft)
		|| SegmentsCrossProperly(Start, End, BottomLeft, Minimum);
}

[[nodiscard]]
bool PinsShareTerminal(const UEdGraphPin* First, const UEdGraphPin* Second)
{ return First != nullptr && First == Second; }

double MeasureWireLength(const FWireSample& Wire)
{
	double Length = 0.0;
	for (int32 Index = 1; Index < Wire.RenderedPoints.Num(); ++Index)
	{
		Length += FVector2D::Distance(Wire.RenderedPoints[Index - 1], Wire.RenderedPoints[Index]);
	}
	return Length;
}

void MeasureNodeMetrics(
	const TArray<const UEdGraphNode*>& Nodes,
	const K2::FGraphGeometrySnapshot& Geometry,
	const FGraphBenchmarkBaseline& Baseline,
	const double GridSize,
	FGraphQualityMetrics& Metrics
)
{
	FBox2D DrawingBounds(EForceInit::ForceInit);
	for (const UEdGraphNode* Node : Nodes)
	{
		const FVector2D Position(Node->NodePosX, Node->NodePosY);
		DrawingBounds += Position;
		DrawingBounds += Position + ResolveNodeSize(*Node, Geometry);
		if (GridSize > GeometryEpsilon)
		{
			Metrics.OffGridXCount += FMath::Abs(FMath::Fmod(Position.X, GridSize)) > GeometryEpsilon ? 1 : 0;
			Metrics.OffGridYCount += FMath::Abs(FMath::Fmod(Position.Y, GridSize)) > GeometryEpsilon ? 1 : 0;
		}
		if (const FVector2D* Before = Baseline.NodePositions.Find(Node->NodeGuid))
		{
			const double Movement = FVector2D::Distance(*Before, Position);
			Metrics.TotalNodeMovement += Movement;
			Metrics.MaximumNodeMovement = FMath::Max(Metrics.MaximumNodeMovement, Movement);
			Metrics.MovedSemanticNodeCount += Movement > GeometryEpsilon ? 1 : 0;
		}
	}
	if (DrawingBounds.bIsValid) { Metrics.DrawingArea = DrawingBounds.GetSize().X * DrawingBounds.GetSize().Y; }
	for (int32 FirstIndex = 0; FirstIndex < Nodes.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Nodes.Num(); ++SecondIndex)
		{
			Metrics.NodeOverlapCount += Overlaps(*Nodes[FirstIndex], *Nodes[SecondIndex], Geometry) ? 1 : 0;
		}
	}
}

void MeasureWireMetrics(
	const TArray<FWireSample>& Wires,
	const TArray<const UEdGraphNode*>& Nodes,
	const K2::FGraphGeometrySnapshot& Geometry,
	const double GridSize,
	FGraphQualityMetrics& Metrics
)
{
	for (const FWireSample& Wire : Wires)
	{
		Metrics.BendCount += Wire.BendCount;
		Metrics.TotalRenderedWireLength += MeasureWireLength(Wire);
		if (Wire.bExecution && Wire.End.X < Wire.Start.X - GeometryEpsilon) { ++Metrics.BackwardExecutionEdgeCount; }
		if (!Wire.bExecution)
		{
			const double BackwardDistance = FMath::Max(0.0, Wire.Start.X - Wire.End.X);
			Metrics.BackwardDataDistance += BackwardDistance;
			Metrics.BackwardDataEdgeCount += BackwardDistance > GeometryEpsilon ? 1 : 0;
		}
		if (Wire.bPrimaryExecution)
		{
			const double VerticalError = FMath::Abs(Wire.End.Y - Wire.Start.Y);
			Metrics.PrimaryExecutionVerticalError += VerticalError;
			Metrics.NonStraightPrimaryExecutionEdgeCount += VerticalError > GeometryEpsilon ? 1 : 0;
			const double SourceRight = Wire.Source->NodePosX + ResolveNodeSize(*Wire.Source, Geometry).X;
			const double Gap = static_cast<double>(Wire.Target->NodePosX) - SourceRight;
			if (VerticalError <= GeometryEpsilon && Gap >= 0.0 && Gap < GridSize - GeometryEpsilon)
			{
				++Metrics.InsufficientExecutionGapCount;
			}
		}
		for (const UEdGraphNode* Node : Nodes)
		{
			if (Node == Wire.Source || Node == Wire.Target) { continue; }
			for (int32 Index = 1; Index < Wire.RenderedPoints.Num(); ++Index)
			{
				if (SegmentPassesThroughNode(Wire.RenderedPoints[Index - 1], Wire.RenderedPoints[Index], *Node, Geometry))
				{
					++Metrics.WireThroughNodeCount;
					break;
				}
			}
		}
	}
	for (int32 FirstIndex = 0; FirstIndex < Wires.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Wires.Num(); ++SecondIndex)
		{
			const FWireSample& First = Wires[FirstIndex];
			const FWireSample& Second = Wires[SecondIndex];
			TArray<FVector2D, TInlineAllocator<2>> IgnoredTerminals;
			if (PinsShareTerminal(First.OutputPin, Second.OutputPin)) { IgnoredTerminals.Add(First.Start); }
			if (PinsShareTerminal(First.InputPin, Second.InputPin)) { IgnoredTerminals.Add(First.End); }
			if (!K2::FK2RerouteRouter::RenderedPolylinesIntersectExceptAtSharedTerminals(
					First.RenderedPoints, Second.RenderedPoints, IgnoredTerminals
				))
			{
				continue;
			}
			if (First.bExecution || Second.bExecution) { ++Metrics.ExecutionCrossingCount; }
			else
			{
				++Metrics.DataCrossingCount;
			}
		}
	}
}

double CalculateCompositePenalty(const FGraphQualityMetrics& Metrics)
{
	return Metrics.NodeOverlapCount * 5000.0 + Metrics.BackwardExecutionEdgeCount * 1500.0
		 + Metrics.WireThroughNodeCount * 1000.0 + Metrics.ExecutionCrossingCount * 750.0 + Metrics.DataCrossingCount * 250.0
		 + Metrics.NonStraightPrimaryExecutionEdgeCount * 175.0 + Metrics.InsufficientExecutionGapCount * 125.0
		 + Metrics.BackwardDataEdgeCount * 100.0 + Metrics.OffGridXCount * 25.0 + Metrics.OffGridYCount * 10.0
		 + Metrics.PrimaryExecutionVerticalError * 0.5 + Metrics.BackwardDataDistance * 0.1 + Metrics.BendCount * 5.0
		 + Metrics.AddedRerouteNodeCount * 750.0 + Metrics.TotalRenderedWireLength * 0.001
		 + Metrics.TotalNodeMovement * 0.05 + Metrics.TotalRerouteNodeMovement * 0.025;
}
} // namespace MetricsPrivate

FGraphBenchmarkBaseline CaptureBaseline(const UEdGraph& Graph)
{
	FGraphBenchmarkBaseline Baseline;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node != nullptr && Node->IsA<UK2Node_Knot>()) { ++Baseline.RerouteNodeCount; }
		if (Node != nullptr && Node->NodeGuid.IsValid())
		{
			Baseline.NodePositions.Add(Node->NodeGuid, FVector2D(Node->NodePosX, Node->NodePosY));
		}
	}
	return Baseline;
}

FGraphQualityMetrics MeasureGraphQuality(
	const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, const FGraphBenchmarkBaseline& Baseline, const double GridSize
)
{
	FGraphQualityMetrics Metrics;
	TArray<const UEdGraphNode*> Nodes;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>()) { continue; }
		if (Node->IsA<UK2Node_Knot>())
		{
			++Metrics.RerouteNodeCount;
			if (const FVector2D* Before = Baseline.NodePositions.Find(Node->NodeGuid))
			{
				const double Movement = FVector2D::Distance(*Before, FVector2D(Node->NodePosX, Node->NodePosY));
				Metrics.TotalRerouteNodeMovement += Movement;
				Metrics.MaximumRerouteNodeMovement = FMath::Max(Metrics.MaximumRerouteNodeMovement, Movement);
				Metrics.MovedRerouteNodeCount += Movement > MetricsPrivate::GeometryEpsilon ? 1 : 0;
			}
			continue;
		}
		Nodes.Add(Node);
	}
	Metrics.SemanticNodeCount = Nodes.Num();
	Metrics.AddedRerouteNodeCount = FMath::Max(0, Metrics.RerouteNodeCount - Baseline.RerouteNodeCount);
	const TArray<MetricsPrivate::FWireSample> Wires = MetricsPrivate::CaptureWires(Graph, Geometry);
	Metrics.LogicalWireCount = Wires.Num();
	MetricsPrivate::MeasureNodeMetrics(Nodes, Geometry, Baseline, GridSize, Metrics);
	MetricsPrivate::MeasureWireMetrics(Wires, Nodes, Geometry, GridSize, Metrics);
	Metrics.CompositePenalty = MetricsPrivate::CalculateCompositePenalty(Metrics);
	return Metrics;
}
} // namespace GraphFormatter::Benchmark
