/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/K2GraphFormatter.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "FormatterSettings.h"
#include "K2/GraphGeometrySnapshot.h"
#include "K2/K2RerouteRouter.h"
#include "K2Node.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

namespace GraphFormatter::K2::Tests
{
namespace
{
constexpr double GeometryEpsilon = 1.0;
constexpr double FallbackNodeWidth = 160.0;
constexpr double FallbackNodeHeight = 80.0;
constexpr double FallbackPinTop = 32.0;
constexpr double FallbackPinPitch = 24.0;
constexpr double LayoutCellSize = 128.0;

struct FBlueprintCorpusEntry
{
	const TCHAR* Label;
	const TCHAR* ObjectPath;
	const TCHAR* SelectionEvidence;
	const TCHAR* RequiredFormattedGraph = nullptr;
};

// This is deliberately a checked-in manifest, not a test-discovery query. Local editor history and
// currently opened Perforce files select candidates, then a fresh .T3D review admits them here. That
// keeps CI deterministic while retaining the authored graphs that matter most to day-to-day work.
const FBlueprintCorpusEntry BlueprintCorpus[] = {
	{
     TEXT("LoomLift.LargeEventAndFunctionGraphs"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomLift.BP_LoomLift"),
     TEXT("most recently opened large Loom Blueprint; currently edited"),
	 },
	{
     TEXT("LoomModuleItem.RecentCoreItem"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomModuleItem.BP_LoomModuleItem"),
     TEXT("recently opened and most recently saved Loom item Blueprint"),
	 },
	{
     TEXT("LoomDroneClamp.MostFrequentlyOpenedModule"),
     TEXT("/Game/Development/LoomCradle/Blueprints/Modules/BP_LoomMod_DroneClamp.BP_LoomMod_DroneClamp"),
     TEXT("highest local Blueprint-open count in the current editing sessions"),
	 },
	{
     TEXT("LoomLightCable.RecentModule"),
     TEXT("/Game/Development/LoomCradle/Blueprints/Modules/BP_LoomMod_LightCable.BP_LoomMod_LightCable"),
     TEXT("recently opened and saved module Blueprint"),
	 },
	{
     TEXT("LoomCradle.CurrentFeatureRoot"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomCradle.BP_LoomCradle"),
     TEXT("current LLM-generated Loom feature root; 112 K2 nodes across 11 graphs"),
     TEXT("EventGraph"),
	 },
	{
     TEXT("LoomCradleOrganic.LargestGeneratedBlueprint"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomCradleOrganic.BP_LoomCradleOrganic"),
     TEXT("largest LLM-generated Loom Blueprint; 289 K2 nodes across 16 graphs"),
     TEXT("EventGraph"),
	 },
	{
     TEXT("LoomCradleHex.CurrentFormattingRepro"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomCradleHex.BP_LoomCradleHex"),
     TEXT("current simple-function grid-alignment and inherited-graph formatting repro"),
	 },
	{
     TEXT("LoomSlotDriver.CurrentFeatureDriver"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomSlotDriver.BP_LoomSlotDriver"),
     TEXT("recently saved LLM-generated Loom driver; 238 K2 nodes across 10 graphs"),
     TEXT("EventGraph"),
	 },
	{
     TEXT("LoomSlotDriverOrganic.GeneratedVariant"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomSlotDriverOrganic.BP_LoomSlotDriverOrganic"),
     TEXT("LLM-generated organic driver variant; 92 K2 nodes across 7 graphs"),
     TEXT("EventGraph"),
	 },
	{
     TEXT("LoomSlotDriverOrganic2.GeneratedVariant"),
     TEXT("/Game/Development/LoomCradle/Blueprints/BP_LoomSlotDriverOrganic2.BP_LoomSlotDriverOrganic2"),
     TEXT("second LLM-generated organic driver variant; 106 K2 nodes across 7 graphs"),
     TEXT("EventGraph"),
	 },
	{
     TEXT("DrinkMeStation.HighRevisionLargeGraph"),
     TEXT("/Game/Blueprints/DrinkMe/BP_DrinkMeStation.BP_DrinkMeStation"),
     TEXT("highest-revision currently edited Blueprint and large hand-formatted graph"),
	 },
	{
     TEXT("DrinkMeIngredientSlot.RecentlyOpened"),
     TEXT("/Game/Blueprints/DrinkMe/BP_DrinkMeIngredientSlot.BP_DrinkMeIngredientSlot"),
     TEXT("recently opened DrinkMe slot Blueprint"),
	 },
	{
     TEXT("DrinkMeOutputSlot.FrequentlyEditedSibling"),
     TEXT("/Game/Blueprints/DrinkMe/BP_DrinkMeOutputSlot.BP_DrinkMeOutputSlot"),
     TEXT("frequently revised sibling of the recent DrinkMe slot graph"),
	 },
	{
     TEXT("WorldItemSlot.RecentLargeGraph"),
     TEXT("/Game/Blueprints/Items/BP_WorldItemSlot.BP_WorldItemSlot"),
     TEXT("recently opened large item-slot Blueprint"),
	 },
	{
     TEXT("WorldItem.FrequentlyEditedFoundation"),
     TEXT("/Game/Blueprints/Items/BP_WorldItem.BP_WorldItem"),
     TEXT("frequently revised foundational item Blueprint"),
	 },
	{
     TEXT("ResourceCarrier.CurrentFormattingRepro"),
     TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier"),
     TEXT("current DropHeldActor no-op and DetachActor spacing/reroute-column repros"),
     TEXT("DropHeldActor"),
	 },
};

struct FEdgeSample
{
	const UEdGraphNode* Source = nullptr;
	const UEdGraphNode* Target = nullptr;
	const UEdGraphPin* OutputPin = nullptr;
	const UEdGraphPin* InputPin = nullptr;
	FVector2D Start = FVector2D::ZeroVector;
	FVector2D End = FVector2D::ZeroVector;
	TArray<FVector2D> RenderedPoints;
	bool bExecution = false;
	bool bPreferredExecution = false;
};

struct FResolvedLogicalPath
{
	const UEdGraphPin* Target = nullptr;
	TArray<FVector2D> Waypoints;
};

struct FGraphQualityMetrics
{
	int32 NodeCount = 0;
	int32 NodeOverlapCount = 0;
	int32 BackwardExecutionEdgeCount = 0;
	int32 BackwardDataEdgeCount = 0;
	int32 NonStraightPreferredExecutionEdgeCount = 0;
	int32 ExecutionCrossingCount = 0;
	int32 DataCrossingCount = 0;
	int32 WireThroughNodeCount = 0;
	int32 InsufficientStraightExecutionGapCount = 0;
	double BackwardDataDistance = 0.0;
	double PreferredExecutionVerticalError = 0.0;
	TMap<const UEdGraphNode*, FVector2D> Positions;
	TArray<const UEdGraphNode*> ExecutionRoots;
};

[[nodiscard]]
bool IsExecutionPin(const UEdGraphPin& Pin)
{ return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; }

[[nodiscard]]
FString NodeIdentity(const UEdGraphNode& Node)
{
	return Node.NodeGuid.IsValid() ? Node.NodeGuid.ToString(EGuidFormats::Digits)
								   : FString::Printf(TEXT("%s:%s"), *Node.GetClass()->GetPathName(), *Node.GetName());
}

[[nodiscard]]
int32 FindPinIndex(const UEdGraphNode& Node, const UEdGraphPin& Pin)
{
	for (int32 Index = 0; Index < Node.Pins.Num(); ++Index)
	{
		if (Node.Pins[Index] == &Pin) { return Index; }
	}
	return INDEX_NONE;
}

[[nodiscard]]
FString PinIdentity(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	const int32 PinIndex = Node != nullptr ? FindPinIndex(*Node, Pin) : INDEX_NONE;
	const FString PinGuid = Pin.PersistentGuid.IsValid()
							  ? Pin.PersistentGuid.ToString(EGuidFormats::Digits)
							  : (Pin.PinId.IsValid() ? Pin.PinId.ToString(EGuidFormats::Digits) : TEXT("NoGuid"));
	return FString::Printf(
		TEXT("%s:%s:%d:%s:%d"),
		Node != nullptr ? *NodeIdentity(*Node) : TEXT("NoNode"),
		*PinGuid,
		PinIndex,
		*Pin.PinName.ToString(),
		static_cast<int32>(Pin.Direction.GetValue())
	);
}

[[nodiscard]]
TArray<FString> CaptureTopology(const UEdGraph& Graph)
{
	TArray<FString> Links;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin != nullptr) { Links.Add(PinIdentity(*Pin) + TEXT("->") + PinIdentity(*LinkedPin)); }
			}
		}
	}
	Links.Sort();
	return Links;
}

void ResolveLogicalTargets(
	const UEdGraphPin& CandidateInput, TSet<const UEdGraphPin*>& VisitedPins, TArray<const UEdGraphPin*>& OutTargets
)
{
	if (VisitedPins.Contains(&CandidateInput)) { return; }
	VisitedPins.Add(&CandidateInput);
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(CandidateInput.GetOwningNodeUnchecked());
	if (Knot == nullptr || !FK2RerouteRouter::IsGeneratedRerouteNode(Knot) || &CandidateInput != Knot->GetInputPin())
	{
		OutTargets.Add(&CandidateInput);
		return;
	}

	const UEdGraphPin* KnotOutput = Knot->GetOutputPin();
	if (KnotOutput == nullptr) { return; }
	for (const UEdGraphPin* LinkedPin : KnotOutput->LinkedTo)
	{
		if (LinkedPin != nullptr) { ResolveLogicalTargets(*LinkedPin, VisitedPins, OutTargets); }
	}
}

[[nodiscard]]
TArray<FString> CaptureLogicalTopology(const UEdGraph& Graph)
{
	TArray<FString> Links;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || FK2RerouteRouter::IsGeneratedRerouteNode(Node)) { continue; }
		for (const UEdGraphPin* OutputPin : Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				if (LinkedPin == nullptr) { continue; }
				TSet<const UEdGraphPin*> VisitedPins;
				TArray<const UEdGraphPin*> Targets;
				ResolveLogicalTargets(*LinkedPin, VisitedPins, Targets);
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
int32 CountAuthoredNodes(const UEdGraph& Graph)
{
	int32 Count = 0;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		if (NodePointer != nullptr && !FK2RerouteRouter::IsGeneratedRerouteNode(NodePointer.Get())) { ++Count; }
	}
	return Count;
}

[[nodiscard]]
FString CaptureGraphState(const UEdGraph& Graph)
{
	TArray<FString> NodeStates;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		NodeStates.Add(
			FString::Printf(
				TEXT("%s|%s|%d|%d|%d|%d"),
				*NodeIdentity(*Node),
				*Node->GetClass()->GetPathName(),
				Node->NodePosX,
				Node->NodePosY,
				Node->NodeWidth,
				Node->NodeHeight
			)
		);
	}
	NodeStates.Sort();
	const TArray<FString> Topology = CaptureTopology(Graph);
	return FString::Join(NodeStates, TEXT("\n")) + TEXT("\n--links--\n") + FString::Join(Topology, TEXT("\n"));
}

[[nodiscard]]
FVector2D ResolveNodeSize(const UEdGraphNode& Node)
{
	if (Node.IsA<UK2Node_Knot>()) { return FVector2D(RerouteKnotWidth, RerouteKnotHeight); }
	return FVector2D(
		Node.NodeWidth >= 1 ? static_cast<double>(Node.NodeWidth) : FallbackNodeWidth,
		Node.NodeHeight >= 1 ? static_cast<double>(Node.NodeHeight) : FallbackNodeHeight
	);
}

[[nodiscard]]
const UEdGraphNode* FindNodeByObjectName(const UEdGraph& Graph, const TCHAR* Name)
{
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node != nullptr && Node->GetName() == Name) { return Node; }
	}
	return nullptr;
}

void VerifyResourceCarrierDetachRerouteColumns(FAutomationTestBase& Test, const FString& Context, const UEdGraph& Graph)
{
	struct FExpectedAlignment
	{
		const TCHAR* Knot;
		const TCHAR* SemanticNode;
		bool bInputColumn = false;
	};
	const FExpectedAlignment ExpectedAlignments[] = {
		{ TEXT("K2Node_Knot_2"), TEXT("K2Node_CallFunction_43"), true },
		{ TEXT("K2Node_Knot_0"), TEXT("K2Node_CallFunction_43") },
		{ TEXT("K2Node_Knot_1"), TEXT("K2Node_CallFunction_0") },
		{ TEXT("K2Node_Knot_3"), TEXT("K2Node_Message_1") },
		{ TEXT("K2Node_Knot_4"), TEXT("K2Node_VariableSet_0") },
		{ TEXT("K2Node_Knot_5"), TEXT("K2Node_IfThenElse_0") },
		{ TEXT("K2Node_Knot_6"), TEXT("K2Node_CallFunction_1"), true },
	};
	for (const FExpectedAlignment& Expected : ExpectedAlignments)
	{
		const UEdGraphNode* Knot = FindNodeByObjectName(Graph, Expected.Knot);
		const UEdGraphNode* SemanticNode = FindNodeByObjectName(Graph, Expected.SemanticNode);
		Test.TestNotNull(*FString::Printf(TEXT("%s: finds reroute '%s'"), *Context, Expected.Knot), Knot);
		Test.TestNotNull(
			*FString::Printf(TEXT("%s: finds semantic column node '%s'"), *Context, Expected.SemanticNode), SemanticNode
		);
		if (Knot == nullptr || SemanticNode == nullptr) { continue; }
		const double KnotCenterX = static_cast<double>(Knot->NodePosX) + RerouteKnotWidth * 0.5;
		const double ExpectedPinX = static_cast<double>(SemanticNode->NodePosX)
								  + (Expected.bInputColumn ? 0.0 : ResolveNodeSize(*SemanticNode).X);
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: reroute '%s' center aligns with the %s pin column of '%s'"),
				*Context,
				Expected.Knot,
				Expected.bInputColumn ? TEXT("input") : TEXT("output"),
				Expected.SemanticNode
			),
			FMath::IsNearlyEqual(KnotCenterX, ExpectedPinX, GeometryEpsilon)
		);
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
bool IsPreferredExecutionPin(const UEdGraphPin& Pin)
{
	if (!IsExecutionPin(Pin)) { return false; }
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return false; }
	for (const UEdGraphPin* Candidate : Node->Pins)
	{
		if (Candidate == nullptr || Candidate->Direction != Pin.Direction || !IsExecutionPin(*Candidate)) { continue; }
		return Candidate == &Pin;
	}
	return false;
}

[[nodiscard]]
FVector2D ResolvePinAnchor(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return FVector2D::ZeroVector; }
	const FVector2D Position(Node->NodePosX, Node->NodePosY);
	const FVector2D Size = ResolveNodeSize(*Node);
	if (Node->IsA<UK2Node_Knot>()) { return Position + Size * 0.5; }

	const double MaximumY = FMath::Max(FallbackPinTop, Size.Y - FallbackPinPitch * 0.5);
	const double Y = FMath::Clamp(
		FallbackPinTop + static_cast<double>(DirectionOrdinal(Pin)) * FallbackPinPitch, FallbackPinTop, MaximumY
	);
	return Position + FVector2D(Pin.Direction == EGPD_Input ? 0.0 : Size.X, Y);
}

void ResolveLogicalPaths(
	const UEdGraphPin& CandidateInput,
	TSet<const UEdGraphPin*>& Traversal,
	TArray<FVector2D>& Waypoints,
	TArray<FResolvedLogicalPath>& OutPaths
)
{
	if (Traversal.Contains(&CandidateInput)) { return; }
	Traversal.Add(&CandidateInput);
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(CandidateInput.GetOwningNodeUnchecked());
	if (Knot == nullptr || !FK2RerouteRouter::IsGeneratedRerouteNode(Knot) || &CandidateInput != Knot->GetInputPin())
	{
		FResolvedLogicalPath& Path = OutPaths.AddDefaulted_GetRef();
		Path.Target = &CandidateInput;
		Path.Waypoints = Waypoints;
		Traversal.Remove(&CandidateInput);
		return;
	}

	Waypoints.Add(ResolvePinAnchor(CandidateInput));
	if (const UEdGraphPin* KnotOutput = Knot->GetOutputPin())
	{
		for (const UEdGraphPin* LinkedPin : KnotOutput->LinkedTo)
		{
			if (LinkedPin != nullptr) { ResolveLogicalPaths(*LinkedPin, Traversal, Waypoints, OutPaths); }
		}
	}
	Waypoints.Pop(EAllowShrinking::No);
	Traversal.Remove(&CandidateInput);
}

[[nodiscard]]
bool TryAverageConnectedPinAnchor(const UEdGraphPin& Pin, FVector2D& OutAverage)
{
	FVector2D Sum = FVector2D::ZeroVector;
	int32 Count = 0;
	for (const UEdGraphPin* LinkedPin : Pin.LinkedTo)
	{
		if (LinkedPin == nullptr) { continue; }
		Sum += ResolvePinAnchor(*LinkedPin);
		++Count;
	}
	if (Count == 0) { return false; }
	OutAverage = Sum / static_cast<double>(Count);
	return true;
}

[[nodiscard]]
bool ShouldReverseKnotTangent(const UEdGraphNode* Node)
{
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
	if (Knot == nullptr || Knot->GetInputPin() == nullptr || Knot->GetOutputPin() == nullptr) { return false; }

	FVector2D AverageLeft;
	FVector2D AverageRight;
	const bool bLeftValid = TryAverageConnectedPinAnchor(*Knot->GetInputPin(), AverageLeft);
	const bool bRightValid = TryAverageConnectedPinAnchor(*Knot->GetOutputPin(), AverageRight);
	if (bLeftValid && bRightValid) { return AverageRight.X < AverageLeft.X; }
	const FVector2D Center = ResolvePinAnchor(*Knot->GetOutputPin());
	if (bLeftValid) { return Center.X < AverageLeft.X; }
	return bRightValid && AverageRight.X < Center.X;
}

[[nodiscard]]
bool PinsShareRenderedTerminal(const UEdGraphPin* First, const UEdGraphPin* Second)
{
	if (First == nullptr || Second == nullptr) { return false; }
	if (First == Second) { return true; }
	const UEdGraphNode* FirstNode = First->GetOwningNodeUnchecked();
	return FirstNode != nullptr && FirstNode == Second->GetOwningNodeUnchecked() && FirstNode->IsA<UK2Node_Knot>();
}

[[nodiscard]]
bool Overlaps(const UEdGraphNode& First, const UEdGraphNode& Second)
{
	const FVector2D FirstMin(First.NodePosX, First.NodePosY);
	const FVector2D SecondMin(Second.NodePosX, Second.NodePosY);
	const FVector2D FirstMax = FirstMin + ResolveNodeSize(First);
	const FVector2D SecondMax = SecondMin + ResolveNodeSize(Second);
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
bool PointInsideNode(const FVector2D Point, const UEdGraphNode& Node)
{
	const FVector2D Minimum(Node.NodePosX, Node.NodePosY);
	const FVector2D Maximum = Minimum + ResolveNodeSize(Node);
	return Point.X > Minimum.X + GeometryEpsilon && Point.X < Maximum.X - GeometryEpsilon
		&& Point.Y > Minimum.Y + GeometryEpsilon && Point.Y < Maximum.Y - GeometryEpsilon;
}

[[nodiscard]]
bool SegmentPassesThroughNode(const FVector2D Start, const FVector2D End, const UEdGraphNode& Node)
{
	const FVector2D Minimum(Node.NodePosX, Node.NodePosY);
	const FVector2D Maximum = Minimum + ResolveNodeSize(Node);
	if (FMath::Max(Start.X, End.X) <= Minimum.X || FMath::Min(Start.X, End.X) >= Maximum.X
		|| FMath::Max(Start.Y, End.Y) <= Minimum.Y || FMath::Min(Start.Y, End.Y) >= Maximum.Y)
	{
		return false;
	}
	if (PointInsideNode((Start + End) * 0.5, Node)) { return true; }

	const FVector2D TopLeft = Minimum;
	const FVector2D TopRight(Maximum.X, Minimum.Y);
	const FVector2D BottomLeft(Minimum.X, Maximum.Y);
	const FVector2D BottomRight = Maximum;
	return SegmentsCrossProperly(Start, End, TopLeft, TopRight)
		|| SegmentsCrossProperly(Start, End, TopRight, BottomRight)
		|| SegmentsCrossProperly(Start, End, BottomRight, BottomLeft)
		|| SegmentsCrossProperly(Start, End, BottomLeft, TopLeft);
}

[[nodiscard]]
TArray<FEdgeSample> CaptureEdges(const UEdGraph& Graph)
{
	TArray<FEdgeSample> Edges;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || FK2RerouteRouter::IsGeneratedRerouteNode(Node)) { continue; }
		for (const UEdGraphPin* OutputPin : Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				if (LinkedPin == nullptr) { continue; }
				TSet<const UEdGraphPin*> Traversal;
				TArray<FVector2D> Waypoints;
				TArray<FResolvedLogicalPath> LogicalPaths;
				ResolveLogicalPaths(*LinkedPin, Traversal, Waypoints, LogicalPaths);
				for (const FResolvedLogicalPath& LogicalPath : LogicalPaths)
				{
					const UEdGraphPin* InputPin = LogicalPath.Target;
					const UEdGraphNode* InputNode = InputPin != nullptr ? InputPin->GetOwningNodeUnchecked() : nullptr;
					if (InputPin == nullptr || InputPin->Direction != EGPD_Input || InputNode == nullptr) { continue; }
					const bool bExecution = IsExecutionPin(*OutputPin) && IsExecutionPin(*InputPin);
					FEdgeSample& Edge = Edges.AddDefaulted_GetRef();
					Edge.Source = Node;
					Edge.Target = InputNode;
					Edge.OutputPin = OutputPin;
					Edge.InputPin = InputPin;
					Edge.Start = ResolvePinAnchor(*OutputPin);
					Edge.End = ResolvePinAnchor(*InputPin);
					TArray<FVector2D> LogicalPoints;
					LogicalPoints.Reserve(LogicalPath.Waypoints.Num() + 2);
					LogicalPoints.Add(Edge.Start);
					LogicalPoints.Append(LogicalPath.Waypoints);
					LogicalPoints.Add(Edge.End);
					Edge.RenderedPoints = FK2RerouteRouter::BuildRenderedPolyline(
						LogicalPoints, ShouldReverseKnotTangent(Node), ShouldReverseKnotTangent(InputNode)
					);
					Edge.bExecution = bExecution;
					Edge.bPreferredExecution = bExecution && IsPreferredExecutionPin(*OutputPin)
											&& IsPreferredExecutionPin(*InputPin);
				}
			}
		}
	}
	return Edges;
}

[[nodiscard]]
FGraphQualityMetrics MeasureQuality(const UEdGraph& Graph)
{
	FGraphQualityMetrics Metrics;
	TArray<const UEdGraphNode*> Nodes;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>() || FK2RerouteRouter::IsGeneratedRerouteNode(Node))
		{
			continue;
		}
		Nodes.Add(Node);
		Metrics.Positions.Add(Node, FVector2D(Node->NodePosX, Node->NodePosY));
		bool bHasExecutionOutput = false;
		bool bHasLinkedExecutionInput = false;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || !IsExecutionPin(*Pin)) { continue; }
			bHasExecutionOutput |= Pin->Direction == EGPD_Output;
			bHasLinkedExecutionInput |= Pin->Direction == EGPD_Input && !Pin->LinkedTo.IsEmpty();
		}
		if (bHasExecutionOutput && !bHasLinkedExecutionInput) { Metrics.ExecutionRoots.Add(Node); }
	}
	Metrics.NodeCount = Nodes.Num();

	for (int32 FirstIndex = 0; FirstIndex < Nodes.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Nodes.Num(); ++SecondIndex)
		{
			if (Overlaps(*Nodes[FirstIndex], *Nodes[SecondIndex])) { ++Metrics.NodeOverlapCount; }
		}
	}

	const TArray<FEdgeSample> Edges = CaptureEdges(Graph);
	for (const FEdgeSample& Edge : Edges)
	{
		if (Edge.bExecution && Edge.End.X < Edge.Start.X - GeometryEpsilon) { ++Metrics.BackwardExecutionEdgeCount; }
		if (!Edge.bExecution)
		{
			const double BackwardDistance = FMath::Max(0.0, Edge.Start.X - Edge.End.X);
			Metrics.BackwardDataDistance += BackwardDistance;
			Metrics.BackwardDataEdgeCount += BackwardDistance > GeometryEpsilon ? 1 : 0;
		}
		if (Edge.bPreferredExecution)
		{
			const double VerticalError = FMath::Abs(Edge.End.Y - Edge.Start.Y);
			Metrics.PreferredExecutionVerticalError += VerticalError;
			if (VerticalError > GeometryEpsilon) { ++Metrics.NonStraightPreferredExecutionEdgeCount; }
			if (VerticalError <= GeometryEpsilon && !Edge.Source->IsA<UK2Node_Knot>() && !Edge.Target->IsA<UK2Node_Knot>()
				&& Edge.End.X >= Edge.Start.X && Edge.End.X - Edge.Start.X < LayoutCellSize - GeometryEpsilon)
			{
				++Metrics.InsufficientStraightExecutionGapCount;
			}
		}

		for (const UEdGraphNode* Node : Nodes)
		{
			if (Node == Edge.Source || Node == Edge.Target) { continue; }
			for (int32 PointIndex = 1; PointIndex < Edge.RenderedPoints.Num(); ++PointIndex)
			{
				if (SegmentPassesThroughNode(Edge.RenderedPoints[PointIndex - 1], Edge.RenderedPoints[PointIndex], *Node))
				{
					++Metrics.WireThroughNodeCount;
					break;
				}
			}
		}
	}

	for (int32 FirstIndex = 0; FirstIndex < Edges.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Edges.Num(); ++SecondIndex)
		{
			const FEdgeSample& First = Edges[FirstIndex];
			const FEdgeSample& Second = Edges[SecondIndex];
			TArray<FVector2D, TInlineAllocator<2>> IgnoredSharedTerminals;
			const auto AddIgnoredTerminal = [&IgnoredSharedTerminals](const FVector2D& Terminal)
			{
				if (!IgnoredSharedTerminals.ContainsByPredicate([&Terminal](const FVector2D& Existing)
																{ return Existing.Equals(Terminal); }))
				{
					IgnoredSharedTerminals.Add(Terminal);
				}
			};
			if (PinsShareRenderedTerminal(First.OutputPin, Second.OutputPin)) { AddIgnoredTerminal(First.Start); }
			if (PinsShareRenderedTerminal(First.OutputPin, Second.InputPin)) { AddIgnoredTerminal(First.Start); }
			if (PinsShareRenderedTerminal(First.InputPin, Second.OutputPin)) { AddIgnoredTerminal(First.End); }
			if (PinsShareRenderedTerminal(First.InputPin, Second.InputPin)) { AddIgnoredTerminal(First.End); }
			if (FK2RerouteRouter::RenderedPolylinesIntersectExceptAtSharedTerminals(
					First.RenderedPoints, Second.RenderedPoints, IgnoredSharedTerminals
				))
			{
				if (First.bExecution || Second.bExecution) { ++Metrics.ExecutionCrossingCount; }
				else
				{
					++Metrics.DataCrossingCount;
				}
			}
		}
	}
	return Metrics;
}

[[nodiscard]]
bool BuildFormattingScope(UEdGraph& Graph, TSet<UEdGraphNode*>& OutScope, int32& OutRealNodeCount)
{
	OutScope.Reset();
	OutRealNodeCount = 0;
	if (Cast<UEdGraphSchema_K2>(Graph.GetSchema()) == nullptr) { return false; }
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		if (Node->IsA<UEdGraphNode_Comment>())
		{
			OutScope.Add(Node);
			continue;
		}
		if (!Node->IsA<UK2Node>()) { return false; }
		OutScope.Add(Node);
		++OutRealNodeCount;
	}
	return OutRealNodeCount >= 2;
}

[[nodiscard]]
UEdGraph* FindDuplicatedGraph(UBlueprint& DuplicateBlueprint, const UEdGraph& SourceGraph)
{
	TArray<UEdGraph*> DuplicateGraphs;
	DuplicateBlueprint.GetAllGraphs(DuplicateGraphs);
	if (SourceGraph.GraphGuid.IsValid())
	{
		if (UEdGraph** Match = DuplicateGraphs.FindByPredicate(
				[&SourceGraph](const UEdGraph* Candidate)
				{ return Candidate != nullptr && Candidate->GraphGuid == SourceGraph.GraphGuid; }
			))
		{
			return *Match;
		}
	}

	if (UEdGraph** Match = DuplicateGraphs.FindByPredicate(
			[&SourceGraph](const UEdGraph* Candidate)
			{
				return Candidate != nullptr && Candidate->GetFName() == SourceGraph.GetFName()
					&& Candidate->GetClass() == SourceGraph.GetClass();
			}
		))
	{
		return *Match;
	}
	return nullptr;
}

[[nodiscard]]
UFormatterSettings* MakeCorpusSettings()
{
	UFormatterSettings* Settings = NewObject<UFormatterSettings>(GetTransientPackage());
	Settings->bEnableK2Formatter = true;
	Settings->bEnableHybridGridSnap = true;
	Settings->K2LayoutMode = EGraphFormatterK2LayoutMode::PreserveHumanLayout;
	Settings->K2LayoutCellSize = static_cast<int32>(LayoutCellSize);
	Settings->K2OrderingSweeps = 8;
	Settings->K2RoutingPlanningWorkBudget = 1000000;
	Settings->K2HorizontalSpacing = 160;
	Settings->K2VerticalSpacing = 96;
	Settings->K2BranchSpacing = 96;
	Settings->K2ComponentSpacing = 256;
	Settings->K2PureHorizontalSpacing = 80;
	Settings->K2PureVerticalSpacing = 48;
	Settings->K2CommentPadding = 64;
	Settings->K2ObstacleClearance = 48;
	Settings->K2RoutingChannelSpacing = 32;
	Settings->K2MaxGeneratedKnots = 6;
	Settings->K2LongDataWireRankThreshold = 3;
	Settings->bRouteDataWires = true;
	return Settings;
}

[[nodiscard]]
bool IsSuccessfulFormatStatus(const EK2FormatStatus Status)
{ return Status == EK2FormatStatus::Formatted || Status == EK2FormatStatus::NoChanges; }

FString DescribeResult(const FK2FormatResult& Result)
{
	FString Description = Result.Message;
	if (!Result.Diagnostics.IsEmpty())
	{
		Description += TEXT(" Diagnostics: ") + FString::Join(Result.Diagnostics, TEXT(" | "));
	}
	return Description;
}

void VerifyRootPreservation(
	FAutomationTestBase& Test, const FString& Context, const FGraphQualityMetrics& Before, const FGraphQualityMetrics& After
)
{
	double MaximumHorizontalDrift = 0.0;
	double MaximumVerticalDrift = 0.0;
	for (const UEdGraphNode* Root : Before.ExecutionRoots)
	{
		const FVector2D* BeforePosition = Before.Positions.Find(Root);
		const FVector2D* AfterPosition = After.Positions.Find(Root);
		if (BeforePosition == nullptr || AfterPosition == nullptr) { continue; }
		MaximumHorizontalDrift = FMath::Max(MaximumHorizontalDrift, FMath::Abs(AfterPosition->X - BeforePosition->X));
		MaximumVerticalDrift = FMath::Max(MaximumVerticalDrift, FMath::Abs(BeforePosition->Y - AfterPosition->Y));
	}

	Test.TestTrue(
		*FString::Printf(
			TEXT("%s: execution roots move horizontally by no more than half a %.0f-unit cell"), *Context, LayoutCellSize
		),
		MaximumHorizontalDrift <= LayoutCellSize * 0.5 + GeometryEpsilon
	);
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s: execution roots move vertically by no more than half a %.0f-unit cell"), *Context, LayoutCellSize
		),
		MaximumVerticalDrift <= LayoutCellSize * 0.5 + GeometryEpsilon
	);

	int32 RootOrderInversions = 0;
	for (int32 FirstIndex = 0; FirstIndex < Before.ExecutionRoots.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Before.ExecutionRoots.Num(); ++SecondIndex)
		{
			const UEdGraphNode* First = Before.ExecutionRoots[FirstIndex];
			const UEdGraphNode* Second = Before.ExecutionRoots[SecondIndex];
			const double BeforeDelta = Before.Positions.FindChecked(First).Y - Before.Positions.FindChecked(Second).Y;
			const double AfterDelta = After.Positions.FindChecked(First).Y - After.Positions.FindChecked(Second).Y;
			if (FMath::Abs(BeforeDelta) > GeometryEpsilon && BeforeDelta * AfterDelta < -GeometryEpsilon)
			{
				++RootOrderInversions;
			}
		}
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s: execution-root top-to-bottom order is preserved"), *Context), RootOrderInversions, 0
	);
}

void VerifyReadabilityDoesNotRegress(
	FAutomationTestBase& Test, const FString& Context, const FGraphQualityMetrics& Before, const FGraphQualityMetrics& After
)
{
	Test.TestTrue(
		*FString::Printf(TEXT("%s: node overlaps do not increase"), *Context), After.NodeOverlapCount <= Before.NodeOverlapCount
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: backward execution edges do not increase"), *Context),
		After.BackwardExecutionEdgeCount <= Before.BackwardExecutionEdgeCount
	);
	const int32 AllowedBackwardDataIncrease = FMath::Max(1, Before.BackwardDataEdgeCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: backward data edges do not materially increase"), *Context),
		After.BackwardDataEdgeCount <= Before.BackwardDataEdgeCount + AllowedBackwardDataIncrease
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: backward data distance does not materially increase"), *Context),
		After.BackwardDataDistance <= Before.BackwardDataDistance * 1.25 + LayoutCellSize * 2.0 + GeometryEpsilon
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: preferred execution bends do not increase"), *Context),
		After.NonStraightPreferredExecutionEdgeCount <= Before.NonStraightPreferredExecutionEdgeCount
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: preferred execution vertical error does not materially increase"), *Context),
		After.PreferredExecutionVerticalError <= Before.PreferredExecutionVerticalError + LayoutCellSize + GeometryEpsilon
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: execution-wire crossings do not increase"), *Context),
		After.ExecutionCrossingCount <= Before.ExecutionCrossingCount
	);
	const int32 AllowedDataCrossingIncrease = FMath::Max(1, Before.DataCrossingCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: data-wire crossings do not materially increase"), *Context),
		After.DataCrossingCount <= Before.DataCrossingCount + AllowedDataCrossingIncrease
	);
	const int32 AllowedWireThroughIncrease = FMath::Max(1, Before.WireThroughNodeCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: wires passing under nodes do not materially increase"), *Context),
		After.WireThroughNodeCount <= Before.WireThroughNodeCount + AllowedWireThroughIncrease
	);
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s: formatting does not introduce another sub-cell gap on a straight primary execution edge"), *Context
		),
		After.InsufficientStraightExecutionGapCount <= Before.InsufficientStraightExecutionGapCount
	);
}

void VerifyHumanMovementBudget(
	FAutomationTestBase& Test, const FString& Context, const FGraphQualityMetrics& Before, const FGraphQualityMetrics& After
)
{
	int32 LargeMovementCount = 0;
	TArray<FString> LargeMovements;
	const double LocalMovementRadius = LayoutCellSize * 1.5;
	for (const TPair<const UEdGraphNode*, FVector2D>& Pair : Before.Positions)
	{
		const FVector2D* AfterPosition = After.Positions.Find(Pair.Key);
		if (AfterPosition != nullptr
			&& FVector2D::Distance(Pair.Value, *AfterPosition) > LocalMovementRadius + GeometryEpsilon)
		{
			++LargeMovementCount;
			const FVector2D Delta = *AfterPosition - Pair.Value;
			LargeMovements.Add(
				FString::Printf(
					TEXT("%s [%s]: (%.0f, %.0f) -> (%.0f, %.0f), delta=(%.0f, %.0f), distance=%.1f"),
					*Pair.Key->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Pair.Key->GetName(),
					Pair.Value.X,
					Pair.Value.Y,
					AfterPosition->X,
					AfterPosition->Y,
					Delta.X,
					Delta.Y,
					Delta.Size()
				)
			);
		}
	}
	const bool bHasMeasuredImprovement =
		After.NodeOverlapCount < Before.NodeOverlapCount
		|| After.BackwardExecutionEdgeCount < Before.BackwardExecutionEdgeCount
		|| After.BackwardDataEdgeCount < Before.BackwardDataEdgeCount
		|| After.BackwardDataDistance + LayoutCellSize < Before.BackwardDataDistance
		|| After.NonStraightPreferredExecutionEdgeCount < Before.NonStraightPreferredExecutionEdgeCount
		|| After.ExecutionCrossingCount < Before.ExecutionCrossingCount
		|| After.DataCrossingCount < Before.DataCrossingCount || After.WireThroughNodeCount < Before.WireThroughNodeCount
		|| After.InsufficientStraightExecutionGapCount < Before.InsufficientStraightExecutionGapCount;
	const double AllowedLargeMovementFraction = bHasMeasuredImprovement ? 0.65 : 0.35;
	const int32 AllowedLargeMovementCount =
		bHasMeasuredImprovement ? FMath::Max(3, FMath::FloorToInt(Before.NodeCount * AllowedLargeMovementFraction))
								: FMath::Max(1, FMath::FloorToInt(Before.NodeCount * AllowedLargeMovementFraction));
	const bool bWithinMovementBudget = LargeMovementCount <= AllowedLargeMovementCount;
	Test.TestTrue(
		*FString::Printf(
			TEXT("%s: authored movement stays local (%d of %d nodes may move beyond the %.0f-unit local radius%s)"),
			*Context,
			AllowedLargeMovementCount,
			Before.NodeCount,
			LocalMovementRadius,
			bHasMeasuredImprovement ? TEXT(" because readability improved") : TEXT("")
		),
		bWithinMovementBudget
	);
	if (!bWithinMovementBudget)
	{
		LargeMovements.Sort();
		for (const FString& Movement : LargeMovements)
		{
			Test.AddInfo(FString::Printf(TEXT("%s movement: %s"), *Context, *Movement));
		}
	}
}

void VerifyFormattedSemanticNodeXGrid(
	FAutomationTestBase& Test,
	const FString& Context,
	const UEdGraph& Graph,
	const FGraphQualityMetrics& Before,
	const FK2FormatResult& Result
)
{
	if (Result.Status != EK2FormatStatus::Formatted) { return; }

	const int32 MajorGridSize = FMath::RoundToInt(LayoutCellSize);
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>()) { continue; }
		const FVector2D* OriginalPosition = Before.Positions.Find(Node);
		if (OriginalPosition != nullptr && Node->NodePosX == FMath::RoundToInt(OriginalPosition->X)
			&& Node->NodePosY == FMath::RoundToInt(OriginalPosition->Y))
		{
			continue;
		}
		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: every moved node '%s' starts on a visible major-grid X rule"), *Context, *Node->GetName()
			),
			Node->NodePosX % MajorGridSize,
			0
		);
	}
}

void VerifyRoutedReadabilityDoesNotRegress(
	FAutomationTestBase& Test, const FString& Context, const FGraphQualityMetrics& Before, const FGraphQualityMetrics& After
)
{
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not increase node overlaps"), *Context),
		After.NodeOverlapCount <= Before.NodeOverlapCount
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not increase execution-wire crossings"), *Context),
		After.ExecutionCrossingCount <= Before.ExecutionCrossingCount
	);
	const int32 AllowedBackwardDataIncrease = FMath::Max(1, Before.BackwardDataEdgeCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not materially increase backward data edges"), *Context),
		After.BackwardDataEdgeCount <= Before.BackwardDataEdgeCount + AllowedBackwardDataIncrease
	);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not materially increase backward data distance"), *Context),
		After.BackwardDataDistance <= Before.BackwardDataDistance * 1.25 + LayoutCellSize * 2.0 + GeometryEpsilon
	);
	const int32 AllowedDataCrossingIncrease = FMath::Max(1, Before.DataCrossingCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not materially increase data-wire crossings"), *Context),
		After.DataCrossingCount <= Before.DataCrossingCount + AllowedDataCrossingIncrease
	);
	const int32 AllowedWireThroughIncrease = FMath::Max(1, Before.WireThroughNodeCount / 4);
	Test.TestTrue(
		*FString::Printf(TEXT("%s: routed layout does not materially increase wires under nodes"), *Context),
		After.WireThroughNodeCount <= Before.WireThroughNodeCount + AllowedWireThroughIncrease
	);
}
} // namespace

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FK2BlueprintCorpusPreservationTest,
	"Project.Unit Tests.GraphFormatter.K2Corpus.PreserveAuthoredBlueprints",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

void FK2BlueprintCorpusPreservationTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const FBlueprintCorpusEntry& Entry : BlueprintCorpus)
	{
		OutBeautifiedNames.Add(Entry.Label);
		OutTestCommands.Add(Entry.ObjectPath);
	}
}

bool FK2BlueprintCorpusPreservationTest::RunTest(const FString& Parameters)
{
	const FBlueprintCorpusEntry* Entry = nullptr;
	for (const FBlueprintCorpusEntry& Candidate : BlueprintCorpus)
	{
		if (Parameters == Candidate.ObjectPath)
		{
			Entry = &Candidate;
			break;
		}
	}
	if (Entry == nullptr)
	{
		AddError(FString::Printf(TEXT("Unknown Blueprint corpus command '%s'."), *Parameters));
		return false;
	}
	if (FCString::Strcmp(Entry->ObjectPath, TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier")) == 0)
	{
		// Duplicating this component Blueprint under /Engine/Transient causes two existing
		// viewmodel/self links to be reconstructed against the transient generated class. The source
		// asset compiles normally; these eight messages are a DuplicateObject test-fixture artifact.
		AddExpectedError(TEXT("Can't connect pins  Viewmodel  and  Self"), EAutomationExpectedErrorFlags::Contains, 4);
		AddExpectedError(TEXT("Can't connect pins  Self  and  Viewmodel"), EAutomationExpectedErrorFlags::Contains, 4);
	}

	UBlueprint* SourceBlueprint = LoadObject<UBlueprint>(nullptr, Entry->ObjectPath);
	if (SourceBlueprint == nullptr)
	{
		AddError(FString::Printf(TEXT("Could not load corpus Blueprint '%s'."), Entry->ObjectPath));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Corpus selection evidence: %s"), Entry->SelectionEvidence));

	TArray<UEdGraph*> SourceGraphs;
	SourceBlueprint->GetAllGraphs(SourceGraphs);
	SourceGraphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
					  { return Left.GetPathName() < Right.GetPathName(); });
	TMap<const UEdGraph*, FString> SourceStateBefore;
	for (const UEdGraph* SourceGraph : SourceGraphs)
	{
		if (SourceGraph != nullptr) { SourceStateBefore.Add(SourceGraph, CaptureGraphState(*SourceGraph)); }
	}
	UPackage* SourcePackage = SourceBlueprint->GetOutermost();
	const bool bSourcePackageWasDirty = SourcePackage != nullptr && SourcePackage->IsDirty();
	const FName WorkingBlueprintName = MakeUniqueObjectName(
		GetTransientPackage(), UBlueprint::StaticClass(), *FString::Printf(TEXT("GFCorpus_%s"), *SourceBlueprint->GetName())
	);
	UBlueprint* WorkingBlueprint =
		DuplicateObject<UBlueprint>(SourceBlueprint, GetTransientPackage(), WorkingBlueprintName);
	const FName RoutedBlueprintName = MakeUniqueObjectName(
		GetTransientPackage(),
		UBlueprint::StaticClass(),
		*FString::Printf(TEXT("GFCorpusRouted_%s"), *SourceBlueprint->GetName())
	);
	UBlueprint* RoutedBlueprint = DuplicateObject<UBlueprint>(SourceBlueprint, GetTransientPackage(), RoutedBlueprintName);
	if (WorkingBlueprint == nullptr || RoutedBlueprint == nullptr)
	{
		AddError(FString::Printf(TEXT("Could not create transient Blueprint copies for '%s'."), Entry->ObjectPath));
		return false;
	}
	WorkingBlueprint->ClearFlags(RF_Public | RF_Standalone);
	WorkingBlueprint->SetFlags(RF_Transient | RF_Transactional);
	RoutedBlueprint->ClearFlags(RF_Public | RF_Standalone);
	RoutedBlueprint->SetFlags(RF_Transient | RF_Transactional);

	UFormatterSettings* Settings = MakeCorpusSettings();
	FGraphGeometrySnapshot HeadlessGeometry;
	HeadlessGeometry.Status = EGraphGeometrySnapshotStatus::Ready;
	int32 TestedGraphCount = 0;
	int32 TestedNodeCount = 0;

	for (UEdGraph* SourceGraph : SourceGraphs)
	{
		if (SourceGraph == nullptr || FBlueprintEditorUtils::IsGraphIntermediate(SourceGraph)
			|| SourceGraph->GetName().EndsWith(TEXT("_MERGED")))
		{
			continue;
		}

		UEdGraph* WorkingGraph = FindDuplicatedGraph(*WorkingBlueprint, *SourceGraph);
		if (WorkingGraph == nullptr)
		{
			AddError(
				FString::Printf(
					TEXT("%s: failed to find the graph in its transient Blueprint copy."), *SourceGraph->GetPathName()
				)
			);
			continue;
		}

		TSet<UEdGraphNode*> Scope;
		int32 RealNodeCount = 0;
		if (!BuildFormattingScope(*WorkingGraph, Scope, RealNodeCount)) { continue; }
		++TestedGraphCount;
		TestedNodeCount += RealNodeCount;
		const FString Context = SourceBlueprint->GetName() + TEXT(".") + SourceGraph->GetName();

		const TArray<FString> TopologyBefore = CaptureTopology(*WorkingGraph);
		const int32 GraphNodeCountBefore = WorkingGraph->Nodes.Num();
		const FGraphQualityMetrics QualityBefore = MeasureQuality(*WorkingGraph);
		const FK2FormatResult FirstResult =
			FK2GraphFormatter::Format(*WorkingGraph, HeadlessGeometry, Scope, false, *Settings);
		const bool bResourceCarrierDetachGraph =
			FCString::Strcmp(Entry->ObjectPath, TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier")) == 0
			&& SourceGraph->GetName() == TEXT("DetachActor");
		const bool bRequiredFormattedGraph =
			(Entry->RequiredFormattedGraph != nullptr && SourceGraph->GetName() == Entry->RequiredFormattedGraph)
			|| bResourceCarrierDetachGraph;
		if (bRequiredFormattedGraph)
		{
			if (FirstResult.bSafetyRejected)
			{
				AddInfo(FString::Printf(TEXT("%s targeted rejection: %s"), *Context, *DescribeResult(FirstResult)));
			}
			TestFalse(
				*FString::Printf(TEXT("%s: the targeted repro is not discarded by the readability safety gate"), *Context),
				FirstResult.bSafetyRejected
			);
			TestEqual(
				*FString::Printf(TEXT("%s: the targeted repro produces a formatted layout"), *Context),
				FirstResult.Status,
				EK2FormatStatus::Formatted
			);
			TestTrue(
				*FString::Printf(TEXT("%s: the targeted repro performs useful node movement"), *Context),
				FirstResult.MovedNodeCount > 0
			);
		}
		TestTrue(
			*FString::Printf(TEXT("%s: formatter completes or safely keeps the authored layout"), *Context),
			IsSuccessfulFormatStatus(FirstResult.Status)
		);
		if (!IsSuccessfulFormatStatus(FirstResult.Status))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Context, *DescribeResult(FirstResult)));
			continue;
		}

		const FGraphQualityMetrics QualityAfter = MeasureQuality(*WorkingGraph);
		TestEqual(
			*FString::Printf(TEXT("%s: formatting without routing preserves the physical node count"), *Context),
			WorkingGraph->Nodes.Num(),
			GraphNodeCountBefore
		);
		TestTrue(
			*FString::Printf(TEXT("%s: formatting without routing preserves every logical pin link"), *Context),
			CaptureTopology(*WorkingGraph) == TopologyBefore
		);
		VerifyRootPreservation(*this, Context, QualityBefore, QualityAfter);
		VerifyReadabilityDoesNotRegress(*this, Context, QualityBefore, QualityAfter);
		VerifyHumanMovementBudget(*this, Context, QualityBefore, QualityAfter);
		VerifyFormattedSemanticNodeXGrid(*this, Context, *WorkingGraph, QualityBefore, FirstResult);
		if (bResourceCarrierDetachGraph)
		{
			TestEqual(
				*FString::Printf(TEXT("%s: every straight execution edge retains one full major-grid gutter"), *Context),
				QualityAfter.InsufficientStraightExecutionGapCount,
				0
			);
			VerifyResourceCarrierDetachRerouteColumns(*this, Context, *WorkingGraph);
		}

		const FK2FormatResult SecondResult =
			FK2GraphFormatter::Format(*WorkingGraph, HeadlessGeometry, Scope, false, *Settings);
		if (SecondResult.MovedNodeCount > 0)
		{
			TArray<FString> SecondPassMoves;
			for (const TPair<const UEdGraphNode*, FVector2D>& BeforePosition : QualityAfter.Positions)
			{
				const UEdGraphNode* Node = BeforePosition.Key;
				const FVector2D AfterPosition(Node->NodePosX, Node->NodePosY);
				if (!AfterPosition.Equals(BeforePosition.Value, GeometryEpsilon))
				{
					SecondPassMoves.Add(
						FString::Printf(
							TEXT("%s (%.0f,%.0f)->(%.0f,%.0f)"),
							*Node->GetName(),
							BeforePosition.Value.X,
							BeforePosition.Value.Y,
							AfterPosition.X,
							AfterPosition.Y
						)
					);
				}
			}
			SecondPassMoves.Sort();
			AddInfo(
				FString::Printf(
					TEXT("%s non-idempotent second pass: %s; first pass: %s; second pass: %s"),
					*Context,
					*FString::Join(SecondPassMoves, TEXT(", ")),
					*DescribeResult(FirstResult),
					*DescribeResult(SecondResult)
				)
			);
		}
		TestTrue(
			*FString::Printf(TEXT("%s: the idempotence pass completes safely"), *Context),
			IsSuccessfulFormatStatus(SecondResult.Status)
		);
		TestEqual(*FString::Printf(TEXT("%s: the second pass moves no nodes"), *Context), SecondResult.MovedNodeCount, 0);
		TestEqual(
			*FString::Printf(TEXT("%s: the second pass resizes no comments"), *Context), SecondResult.ResizedCommentCount, 0
		);
		TestTrue(
			*FString::Printf(TEXT("%s: the idempotence pass preserves every logical pin link"), *Context),
			CaptureTopology(*WorkingGraph) == TopologyBefore
		);

		AddInfo(
			FString::Printf(
				TEXT(
					"%s: nodes=%d moved=%d overlaps=%d->%d preferred-bends=%d->%d exec-crossings=%d->%d "
					"wire-under-node=%d->%d result='%s'"
				),
				*Context,
				RealNodeCount,
				FirstResult.MovedNodeCount,
				QualityBefore.NodeOverlapCount,
				QualityAfter.NodeOverlapCount,
				QualityBefore.NonStraightPreferredExecutionEdgeCount,
				QualityAfter.NonStraightPreferredExecutionEdgeCount,
				QualityBefore.ExecutionCrossingCount,
				QualityAfter.ExecutionCrossingCount,
				QualityBefore.WireThroughNodeCount,
				QualityAfter.WireThroughNodeCount,
				*FirstResult.Message
			)
		);

		UEdGraph* RoutedGraph = FindDuplicatedGraph(*RoutedBlueprint, *SourceGraph);
		if (RoutedGraph == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: failed to find the graph in its routed Blueprint copy."), *Context));
			continue;
		}
		TSet<UEdGraphNode*> RoutedScope;
		int32 RoutedRealNodeCount = 0;
		if (!BuildFormattingScope(*RoutedGraph, RoutedScope, RoutedRealNodeCount))
		{
			AddError(FString::Printf(TEXT("%s: routed duplicate unexpectedly became unsupported."), *Context));
			continue;
		}
		TestEqual(
			*FString::Printf(TEXT("%s: routed duplicate starts with every real K2 node"), *Context), RoutedRealNodeCount, RealNodeCount
		);

		const TArray<FString> LogicalTopologyBefore = CaptureLogicalTopology(*RoutedGraph);
		const int32 AuthoredNodeCountBefore = CountAuthoredNodes(*RoutedGraph);
		const FGraphQualityMetrics RoutedQualityBefore = MeasureQuality(*RoutedGraph);
		const FK2FormatResult RoutedResult =
			FK2GraphFormatter::Format(*RoutedGraph, HeadlessGeometry, RoutedScope, true, *Settings);
		TestTrue(
			*FString::Printf(TEXT("%s: format-and-route completes or safely keeps the authored layout"), *Context),
			IsSuccessfulFormatStatus(RoutedResult.Status)
		);
		if (!IsSuccessfulFormatStatus(RoutedResult.Status))
		{
			AddError(FString::Printf(TEXT("%s routed: %s"), *Context, *DescribeResult(RoutedResult)));
			continue;
		}

		const FGraphQualityMetrics RoutedQualityAfter = MeasureQuality(*RoutedGraph);
		TestEqual(
			*FString::Printf(TEXT("%s: routing preserves every authored node"), *Context),
			CountAuthoredNodes(*RoutedGraph),
			AuthoredNodeCountBefore
		);
		TestTrue(
			*FString::Printf(TEXT("%s: routing preserves logical endpoint topology"), *Context),
			CaptureLogicalTopology(*RoutedGraph) == LogicalTopologyBefore
		);
		VerifyRootPreservation(*this, Context + TEXT(" routed"), RoutedQualityBefore, RoutedQualityAfter);
		VerifyRoutedReadabilityDoesNotRegress(*this, Context, RoutedQualityBefore, RoutedQualityAfter);
		VerifyHumanMovementBudget(*this, Context + TEXT(" routed"), RoutedQualityBefore, RoutedQualityAfter);
		if (bResourceCarrierDetachGraph)
		{
			TestEqual(
				*FString::Printf(TEXT("%s routed: every straight execution edge retains one full major-grid gutter"), *Context),
				RoutedQualityAfter.InsufficientStraightExecutionGapCount,
				0
			);
			VerifyResourceCarrierDetachRerouteColumns(*this, Context + TEXT(" routed"), *RoutedGraph);
		}

		const int32 RoutedNodeCountAfterFirstPass = RoutedGraph->Nodes.Num();
		const FGraphQualityMetrics RoutedQualityBeforeSecondPass = MeasureQuality(*RoutedGraph);
		TSet<UEdGraphNode*> RoutedSecondPassScope;
		int32 RoutedSecondPassNodeCount = 0;
		if (!BuildFormattingScope(*RoutedGraph, RoutedSecondPassScope, RoutedSecondPassNodeCount))
		{
			AddError(FString::Printf(TEXT("%s: routed graph became unsupported before its idempotence pass."), *Context));
			continue;
		}
		const FK2FormatResult RoutedSecondResult =
			FK2GraphFormatter::Format(*RoutedGraph, HeadlessGeometry, RoutedSecondPassScope, true, *Settings);
		const FGraphQualityMetrics RoutedQualityAfterSecondPass = MeasureQuality(*RoutedGraph);
		TestTrue(
			*FString::Printf(TEXT("%s: routed idempotence pass completes safely"), *Context),
			IsSuccessfulFormatStatus(RoutedSecondResult.Status)
		);
		TestEqual(
			*FString::Printf(TEXT("%s: routed second pass moves no nodes"), *Context), RoutedSecondResult.MovedNodeCount, 0
		);
		TestEqual(
			*FString::Printf(TEXT("%s: routed second pass creates no reroute nodes"), *Context),
			RoutedSecondResult.CreatedKnotCount,
			0
		);
		TestEqual(
			*FString::Printf(TEXT("%s: routed second pass keeps the node count stable"), *Context),
			RoutedGraph->Nodes.Num(),
			RoutedNodeCountAfterFirstPass
		);
		TestTrue(
			*FString::Printf(TEXT("%s: routed idempotence preserves logical endpoint topology"), *Context),
			CaptureLogicalTopology(*RoutedGraph) == LogicalTopologyBefore
		);
		VerifyHumanMovementBudget(
			*this, Context + TEXT(" routed second pass"), RoutedQualityBeforeSecondPass, RoutedQualityAfterSecondPass
		);
		AddInfo(
			FString::Printf(
				TEXT(
					"%s routed: moved=%d routed-wires=%d knots=%d overlaps=%d->%d exec-crossings=%d->%d "
					"wire-under-node=%d->%d result='%s'"
				),
				*Context,
				RoutedResult.MovedNodeCount,
				RoutedResult.RoutedWireCount,
				RoutedResult.CreatedKnotCount,
				RoutedQualityBefore.NodeOverlapCount,
				RoutedQualityAfter.NodeOverlapCount,
				RoutedQualityBefore.ExecutionCrossingCount,
				RoutedQualityAfter.ExecutionCrossingCount,
				RoutedQualityBefore.WireThroughNodeCount,
				RoutedQualityAfter.WireThroughNodeCount,
				*RoutedResult.Message
			)
		);
	}

	TestTrue(TEXT("the corpus asset contains at least one eligible authored K2 graph"), TestedGraphCount > 0);
	TestTrue(TEXT("the corpus asset exercises at least two real K2 nodes"), TestedNodeCount >= 2);
	for (const TPair<const UEdGraph*, FString>& Pair : SourceStateBefore)
	{
		TestTrue(
			*FString::Printf(
				TEXT("%s: the loaded source graph's layout and topology remain unchanged"), *Pair.Key->GetPathName()
			),
			CaptureGraphState(*Pair.Key) == Pair.Value
		);
	}
	if (SourcePackage != nullptr)
	{
		TestEqual(
			TEXT("the source Blueprint package dirty flag is unchanged"), SourcePackage->IsDirty(), bSourcePackageWasDirty
		);
	}
	return true;
}
} // namespace GraphFormatter::K2::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
