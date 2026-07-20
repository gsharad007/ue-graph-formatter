/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/K2RerouteRouter.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_Knot.h"
#include "Misc/AutomationTest.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace GraphFormatter::K2::Tests
{
namespace
{
constexpr TCHAR GeneratedRerouteMetadataKey[] = TEXT("GraphFormatter.GeneratedReroute");
constexpr TCHAR LogicalEdgeMetadataKey[] = TEXT("GraphFormatter.LogicalEdge");
constexpr TCHAR RouteOrdinalMetadataKey[] = TEXT("GraphFormatter.RouteOrdinal");

struct FRouteFixture
{
	UEdGraph* Graph = nullptr;
	UK2Node_Knot* Source = nullptr;
	UK2Node_Knot* Destination = nullptr;
};

UEdGraph* MakeGraph()
{
	const FString PackageName =
		FString::Printf(TEXT("/Temp/GraphFormatterRoutingTests/P_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	Package->SetFlags(RF_Transient);
	UBlueprint* Blueprint = NewObject<UBlueprint>(Package, NAME_None, RF_Transient | RF_Transactional);
	UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, NAME_None, RF_Transient | RF_Transactional);
	Graph->Schema = UEdGraphSchema_K2::StaticClass();
	return Graph;
}

UK2Node_Knot* AddKnot(UEdGraph& Graph, const FVector2D Position)
{
	FGraphNodeCreator<UK2Node_Knot> NodeCreator(Graph);
	UK2Node_Knot* Knot = NodeCreator.CreateNode(false);
	Knot->NodePosX = FMath::RoundToInt(Position.X);
	Knot->NodePosY = FMath::RoundToInt(Position.Y);
	NodeCreator.Finalize();
	return Knot;
}

bool Connect(const UEdGraph& Graph, UK2Node_Knot& Source, UK2Node_Knot& Destination)
{
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
	return Schema && Schema->UEdGraphSchema::TryCreateConnection(Source.GetOutputPin(), Destination.GetInputPin());
}

bool HasDirectLink(const UK2Node_Knot& Source, const UK2Node_Knot& Destination)
{ return Source.GetOutputPin()->LinkedTo.Contains(Destination.GetInputPin()); }

bool IsGenerated(const UEdGraphNode* Node) { return FK2RerouteRouter::IsGeneratedRerouteNode(Node); }

TArray<UK2Node_Knot*> GetGeneratedKnots(const UEdGraph& Graph)
{
	TArray<UK2Node_Knot*> Result;
	for (const TObjectPtr<UEdGraphNode>& Node : Graph.Nodes)
	{
		UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node.Get());
		if (Knot && IsGenerated(Knot)) { Result.Add(Knot); }
	}

	Result.Sort(
		[](const UK2Node_Knot& Left, const UK2Node_Knot& Right)
		{
			FMetaData& LeftMetaData = Left.GetOutermost()->GetMetaData();
			FMetaData& RightMetaData = Right.GetOutermost()->GetMetaData();
			const int32 LeftOrdinal = FCString::Atoi(*LeftMetaData.GetValue(&Left, RouteOrdinalMetadataKey));
			const int32 RightOrdinal = FCString::Atoi(*RightMetaData.GetValue(&Right, RouteOrdinalMetadataKey));
			return LeftOrdinal < RightOrdinal;
		}
	);
	return Result;
}

TArray<UK2Node_Knot*> GetGeneratedKnotsForLogicalEdge(const UEdGraph& Graph, const FString& StableKey)
{
	TArray<UK2Node_Knot*> Result;
	for (UK2Node_Knot* Knot : GetGeneratedKnots(Graph))
	{
		FMetaData& MetaData = Knot->GetOutermost()->GetMetaData();
		if (MetaData.GetValue(Knot, LogicalEdgeMetadataKey) == StableKey) { Result.Add(Knot); }
	}
	return Result;
}

FVector2D GetKnotCenter(const UK2Node_Knot& Knot) { return FVector2D(Knot.NodePosX + 21.0, Knot.NodePosY + 12.0); }

int32 CountGeneratedKnotsOnPath(const UK2Node_Knot& Source, const UK2Node_Knot& Destination, TArray<UK2Node_Knot*>* OutPath = nullptr)
{
	UEdGraphPin* CurrentOutput = Source.GetOutputPin();
	TSet<const UEdGraphNode*> Visited;
	int32 KnotCount = 0;

	while (CurrentOutput)
	{
		if (CurrentOutput->LinkedTo.Num() != 1) { return INDEX_NONE; }

		UEdGraphPin* NextInput = CurrentOutput->LinkedTo[0];
		if (NextInput == Destination.GetInputPin()) { return KnotCount; }

		UK2Node_Knot* NextKnot = NextInput ? Cast<UK2Node_Knot>(NextInput->GetOwningNodeUnchecked()) : nullptr;
		if (!NextKnot || NextInput != NextKnot->GetInputPin() || !IsGenerated(NextKnot) || Visited.Contains(NextKnot))
		{
			return INDEX_NONE;
		}

		Visited.Add(NextKnot);
		if (OutPath) { OutPath->Add(NextKnot); }
		++KnotCount;
		CurrentOutput = NextKnot->GetOutputPin();
	}

	return INDEX_NONE;
}

FRouteFixture MakeBackwardFixture()
{
	FRouteFixture Fixture;
	Fixture.Graph = MakeGraph();
	Fixture.Source = AddKnot(*Fixture.Graph, FVector2D(400.0, 100.0));
	Fixture.Destination = AddKnot(*Fixture.Graph, FVector2D(100.0, 100.0));
	Connect(*Fixture.Graph, *Fixture.Source, *Fixture.Destination);
	return Fixture;
}

FRerouteEdge MakeEdge(
	FRouteFixture& Fixture,
	const FString& StableKey,
	const FVector2D OutputAnchor = FVector2D(400.0, 100.0),
	const FVector2D InputAnchor = FVector2D(100.0, 100.0)
)
{
	FRerouteEdge Edge;
	Edge.OutputPin = Fixture.Source->GetOutputPin();
	Edge.InputPin = Fixture.Destination->GetInputPin();
	Edge.OutputAnchor = OutputAnchor;
	Edge.InputAnchor = InputAnchor;
	Edge.StableKey = StableKey;
	Edge.bExecution = true;
	return Edge;
}

FRerouteResult RouteEdge(
	FRouteFixture& Fixture,
	const FRerouteEdge& Edge,
	TConstArrayView<FRerouteObstacle> Obstacles = {},
	const FRerouteSettings& Settings = FRerouteSettings()
)
{
	TSet<UEdGraphNode*> Scope;
	Scope.Add(Fixture.Source);
	Scope.Add(Fixture.Destination);
	return FK2RerouteRouter::Route(*Fixture.Graph, MakeArrayView(&Edge, 1), Obstacles, Scope, Settings, 16.0);
}

TArray<FRerouteEdge> MakePathSegmentEdges(const UK2Node_Knot& Source, const UK2Node_Knot& Destination, const FString& StableKey)
{
	TArray<FRerouteEdge> Edges;
	UEdGraphPin* CurrentOutput = Source.GetOutputPin();
	int32 SegmentIndex = 0;
	while (CurrentOutput && CurrentOutput->LinkedTo.Num() == 1)
	{
		UEdGraphPin* NextInput = CurrentOutput->LinkedTo[0];
		FRerouteEdge& Segment = Edges.AddDefaulted_GetRef();
		Segment.OutputPin = CurrentOutput;
		Segment.InputPin = NextInput;
		Segment.OutputAnchor = FVector2D(400.0 - SegmentIndex * 32.0, 100.0);
		Segment.InputAnchor = FVector2D(368.0 - SegmentIndex * 32.0, 100.0);
		Segment.StableKey = FString::Printf(TEXT("%s.Segment.%d"), *StableKey, SegmentIndex);
		Segment.bExecution = true;
		++SegmentIndex;

		if (NextInput == Destination.GetInputPin()) { break; }

		UK2Node_Knot* NextKnot = NextInput ? Cast<UK2Node_Knot>(NextInput->GetOwningNodeUnchecked()) : nullptr;
		CurrentOutput = NextKnot ? NextKnot->GetOutputPin() : nullptr;
	}
	return Edges;
}

struct FCompetingRouteFixture
{
	UEdGraph* Graph = nullptr;
	TArray<FRerouteEdge> Edges;
	TSet<UEdGraphNode*> Scope;
};

FCompetingRouteFixture MakeCompetingRouteFixture(const bool bReverseInputOrder)
{
	FCompetingRouteFixture Fixture;
	Fixture.Graph = MakeGraph();
	UK2Node_Knot* FirstSource = AddKnot(*Fixture.Graph, FVector2D(400.0, 100.0));
	UK2Node_Knot* FirstDestination = AddKnot(*Fixture.Graph, FVector2D(100.0, 100.0));
	UK2Node_Knot* SecondSource = AddKnot(*Fixture.Graph, FVector2D(400.0, 100.0));
	UK2Node_Knot* SecondDestination = AddKnot(*Fixture.Graph, FVector2D(100.0, 100.0));
	Connect(*Fixture.Graph, *FirstSource, *FirstDestination);
	Connect(*Fixture.Graph, *SecondSource, *SecondDestination);

	FRerouteEdge FirstEdge;
	FirstEdge.OutputPin = FirstSource->GetOutputPin();
	FirstEdge.InputPin = FirstDestination->GetInputPin();
	FirstEdge.OutputAnchor = FVector2D(400.0, 100.0);
	FirstEdge.InputAnchor = FVector2D(100.0, 100.0);
	FirstEdge.StableKey = TEXT("Competing.A");
	FirstEdge.bExecution = true;
	FRerouteEdge SecondEdge;
	SecondEdge.OutputPin = SecondSource->GetOutputPin();
	SecondEdge.InputPin = SecondDestination->GetInputPin();
	SecondEdge.OutputAnchor = FVector2D(400.0, 100.0);
	SecondEdge.InputAnchor = FVector2D(100.0, 100.0);
	SecondEdge.StableKey = TEXT("Competing.B");
	SecondEdge.bExecution = true;
	if (bReverseInputOrder) { Fixture.Edges = { SecondEdge, FirstEdge }; }
	else
	{
		Fixture.Edges = { FirstEdge, SecondEdge };
	}

	Fixture.Scope = { FirstSource, FirstDestination, SecondSource, SecondDestination };
	return Fixture;
}

struct FCrossingRouteFixture
{
	UEdGraph* Graph = nullptr;
	UK2Node_Knot* FirstSource = nullptr;
	UK2Node_Knot* FirstDestination = nullptr;
	UK2Node_Knot* SecondSource = nullptr;
	UK2Node_Knot* SecondDestination = nullptr;
	TArray<FRerouteEdge> Edges;
	TSet<UEdGraphNode*> Scope;
};

FCrossingRouteFixture MakeCrossingRouteFixture(const bool bSecondIsExecution, const bool bReverseInputOrder = false)
{
	FCrossingRouteFixture Fixture;
	Fixture.Graph = MakeGraph();
	Fixture.FirstSource = AddKnot(*Fixture.Graph, FVector2D(0.0, 0.0));
	Fixture.FirstDestination = AddKnot(*Fixture.Graph, FVector2D(400.0, 200.0));
	Fixture.SecondSource = AddKnot(*Fixture.Graph, FVector2D(0.0, 200.0));
	Fixture.SecondDestination = AddKnot(*Fixture.Graph, FVector2D(400.0, 0.0));
	Connect(*Fixture.Graph, *Fixture.FirstSource, *Fixture.FirstDestination);
	Connect(*Fixture.Graph, *Fixture.SecondSource, *Fixture.SecondDestination);

	FRerouteEdge FirstEdge;
	FirstEdge.OutputPin = Fixture.FirstSource->GetOutputPin();
	FirstEdge.InputPin = Fixture.FirstDestination->GetInputPin();
	FirstEdge.OutputAnchor = FVector2D(0.0, 0.0);
	FirstEdge.InputAnchor = FVector2D(400.0, 200.0);
	FirstEdge.StableKey = TEXT("Crossing.Execution");
	FirstEdge.bExecution = true;

	FRerouteEdge SecondEdge;
	SecondEdge.OutputPin = Fixture.SecondSource->GetOutputPin();
	SecondEdge.InputPin = Fixture.SecondDestination->GetInputPin();
	SecondEdge.OutputAnchor = FVector2D(0.0, 200.0);
	SecondEdge.InputAnchor = FVector2D(400.0, 0.0);
	SecondEdge.StableKey = bSecondIsExecution ? TEXT("Crossing.OtherExecution") : TEXT("Crossing.Data");
	SecondEdge.bExecution = bSecondIsExecution;
	if (bReverseInputOrder) { Fixture.Edges = { SecondEdge, FirstEdge }; }
	else
	{
		Fixture.Edges = { FirstEdge, SecondEdge };
	}

	Fixture.Scope = { Fixture.FirstSource, Fixture.FirstDestination, Fixture.SecondSource, Fixture.SecondDestination };
	return Fixture;
}

struct FDenseRouteFixture
{
	UEdGraph* Graph = nullptr;
	TArray<UK2Node_Knot*> Sources;
	TArray<UK2Node_Knot*> Destinations;
	TArray<FRerouteEdge> Edges;
	TSet<UEdGraphNode*> Scope;
};

FDenseRouteFixture MakeDenseRouteFixture(const bool bReverseInputOrder)
{
	FDenseRouteFixture Fixture;
	Fixture.Graph = MakeGraph();
	constexpr int32 EdgeCount = 12;
	for (int32 Index = 0; Index < EdgeCount; ++Index)
	{
		const double Y = Index * 96.0;
		UK2Node_Knot* Source = AddKnot(*Fixture.Graph, FVector2D(0.0, Y));
		UK2Node_Knot* Destination = AddKnot(*Fixture.Graph, FVector2D(400.0, Y));
		Connect(*Fixture.Graph, *Source, *Destination);
		Fixture.Sources.Add(Source);
		Fixture.Destinations.Add(Destination);
		Fixture.Scope.Add(Source);
		Fixture.Scope.Add(Destination);

		FRerouteEdge Edge;
		Edge.OutputPin = Source->GetOutputPin();
		Edge.InputPin = Destination->GetInputPin();
		Edge.OutputAnchor = FVector2D(0.0, Y);
		Edge.InputAnchor = FVector2D(400.0, Y);
		Edge.StableKey = FString::Printf(TEXT("Dense.%02d"), Index);
		Edge.bExecution = true;
		if (bReverseInputOrder) { Fixture.Edges.Insert(MoveTemp(Edge), 0); }
		else
		{
			Fixture.Edges.Add(MoveTemp(Edge));
		}
	}
	return Fixture;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteBackwardObstacleTopologyTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.BackwardObstaclePreservesOtherLinks",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteBackwardObstacleTopologyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	TestNotNull(TEXT("transient K2 graph was created"), Fixture.Graph);
	TestNotNull(TEXT("source knot was created"), Fixture.Source);
	TestNotNull(TEXT("destination knot was created"), Fixture.Destination);
	TestTrue(TEXT("fixture starts with its direct link"), HasDirectLink(*Fixture.Source, *Fixture.Destination));

	UK2Node_Knot* OtherSource = AddKnot(*Fixture.Graph, FVector2D(0.0, 400.0));
	UK2Node_Knot* OtherDestination = AddKnot(*Fixture.Graph, FVector2D(300.0, 400.0));
	TestTrue(TEXT("unrelated link was created"), Connect(*Fixture.Graph, *OtherSource, *OtherDestination));
	UK2Node_Knot* ObstacleNode = AddKnot(*Fixture.Graph, FVector2D(250.0, 100.0));

	FRerouteObstacle Obstacle;
	Obstacle.Node = ObstacleNode;
	Obstacle.Bounds = FBox2D(FVector2D(200.0, 50.0), FVector2D(300.0, 150.0));
	const FRerouteEdge Edge = MakeEdge(Fixture, TEXT("Backward.Obstructed"));
	const FRerouteResult Result = RouteEdge(Fixture, Edge, MakeArrayView(&Obstacle, 1));

	TestEqual(TEXT("one logical wire was routed"), Result.RoutedWires, 1);
	TestTrue(TEXT("the route contains knots"), Result.CreatedKnots > 0);
	TestEqual(TEXT("successful route has no skipped wires"), Result.SkippedWires, 0);
	TestFalse(TEXT("the original direct edge was removed"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	TestTrue(TEXT("the unrelated edge remains direct"), HasDirectLink(*OtherSource, *OtherDestination));
	TestEqual(
		TEXT("the replacement chain reaches the original destination"),
		CountGeneratedKnotsOnPath(*Fixture.Source, *Fixture.Destination),
		Result.CreatedKnots
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteSourceFanOutTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.SourceFanOutPreservesUnrelatedBranch",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteSourceFanOutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	UK2Node_Knot* OtherDestination = AddKnot(*Fixture.Graph, FVector2D(100.0, 260.0));
	TestTrue(
		TEXT("the source output accepts an unrelated fan-out branch"),
		Connect(*Fixture.Graph, *Fixture.Source, *OtherDestination)
	);
	const FString StableKey = TEXT("FanOut.LogicalEdge");
	const FRerouteResult Result = RouteEdge(Fixture, MakeEdge(Fixture, StableKey));
	const TArray<UK2Node_Knot*> Generated = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, StableKey);

	TestEqual(TEXT("one fan-out branch is routed"), Result.RoutedWires, 1);
	TestTrue(TEXT("the unrelated source branch remains direct"), HasDirectLink(*Fixture.Source, *OtherDestination));
	TestFalse(TEXT("the replaced branch is no longer direct"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	if (!Generated.IsEmpty())
	{
		TestTrue(
			TEXT("the generated chain begins at the shared source output"),
			Generated[0]->GetInputPin()->LinkedTo.Contains(Fixture.Source->GetOutputPin())
		);
		TestTrue(
			TEXT("the generated chain ends at the intended destination input"),
			Generated.Last()->GetOutputPin()->LinkedTo.Contains(Fixture.Destination->GetInputPin())
		);
	}
	else
	{
		AddError(TEXT("the routed fan-out branch created no generated knots"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteRepeatedPassDoesNotGrowTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.RepeatedPassDoesNotGrowGeneratedChain",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteRepeatedPassDoesNotGrowTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	const FString StableKey = TEXT("Repeated.LogicalEdge");
	const FRerouteEdge OriginalEdge = MakeEdge(Fixture, StableKey);
	const FRerouteResult FirstResult = RouteEdge(Fixture, OriginalEdge);
	TestTrue(TEXT("first pass creates a route"), FirstResult.WasModified());

	TArray<FRerouteEdge> SegmentEdges = MakePathSegmentEdges(*Fixture.Source, *Fixture.Destination, StableKey);
	TSet<UEdGraphNode*> Scope;
	for (const TObjectPtr<UEdGraphNode>& Node : Fixture.Graph->Nodes)
	{
		if (Node) { Scope.Add(Node.Get()); }
	}

	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	const int32 GeneratedCountBefore = GetGeneratedKnots(*Fixture.Graph).Num();
	const FRerouteSettings Settings;
	const FRerouteResult SecondResult =
		FK2RerouteRouter::Route(*Fixture.Graph, SegmentEdges, TConstArrayView<FRerouteObstacle>(), Scope, Settings, 16.0);

	TestFalse(TEXT("a repeated pass creates no knots"), SecondResult.WasModified());
	TestEqual(TEXT("a repeated pass does not grow the graph"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestEqual(TEXT("the generated chain length is stable"), GetGeneratedKnots(*Fixture.Graph).Num(), GeneratedCountBefore);
	TestEqual(
		TEXT("the original chain remains connected"),
		CountGeneratedKnotsOnPath(*Fixture.Source, *Fixture.Destination),
		GeneratedCountBefore
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteMaximumKnotLimitTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.MaximumKnotLimitPreservesOriginalLink",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteMaximumKnotLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	const FRerouteEdge Edge = MakeEdge(Fixture, TEXT("KnotLimit.LogicalEdge"));
	FRerouteSettings Settings;
	Settings.MaxKnotsPerWire = 1;
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	const FRerouteResult Result = RouteEdge(Fixture, Edge, {}, Settings);

	TestFalse(TEXT("over-budget route makes no mutation"), Result.WasModified());
	TestEqual(TEXT("over-budget required route is reported as skipped"), Result.SkippedWires, 1);
	TestEqual(TEXT("over-budget required route emits one diagnostic"), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		TestTrue(
			TEXT("over-budget diagnostic identifies the logical edge"),
			Result.Diagnostics[0].Contains(TEXT("KnotLimit.LogicalEdge"))
		);
		TestTrue(
			TEXT("over-budget diagnostic explains the configured limit"),
			Result.Diagnostics[0].Contains(TEXT("configured limit"))
		);
	}
	TestEqual(TEXT("over-budget route spawns no transient knots"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestTrue(TEXT("over-budget route preserves the original link"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	TestEqual(TEXT("over-budget route leaves source topology unchanged"), Fixture.Source->GetOutputPin()->LinkedTo.Num(), 1);
	TestEqual(
		TEXT("over-budget route leaves destination topology unchanged"), Fixture.Destination->GetInputPin()->LinkedTo.Num(), 1
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteUnneededRouteTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.UnneededForwardRouteIsNoOp",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteUnneededRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	const FRerouteEdge Edge =
		MakeEdge(Fixture, TEXT("Forward.Clear.LogicalEdge"), FVector2D(0.0, 100.0), FVector2D(400.0, 100.0));
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	Fixture.Graph->GetOutermost()->SetDirtyFlag(false);
	const FRerouteResult Result = RouteEdge(Fixture, Edge);

	TestFalse(TEXT("clear forward route makes no mutation"), Result.WasModified());
	TestFalse(TEXT("clear forward no-op leaves its package clean"), Fixture.Graph->GetOutermost()->IsDirty());
	TestEqual(TEXT("clear forward route does not grow the graph"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestTrue(TEXT("clear forward route remains direct"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteDisabledDataRoutingTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.DisabledDataRoutingNeverMutatesDataWire",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteDisabledDataRoutingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	FRerouteEdge Edge = MakeEdge(Fixture, TEXT("Data.Disabled.LogicalEdge"));
	Edge.bExecution = false;
	FRerouteSettings Settings;
	Settings.bRouteDataWires = false;
	Fixture.Graph->GetOutermost()->SetDirtyFlag(false);
	const FRerouteResult Result = RouteEdge(Fixture, Edge, {}, Settings);

	TestFalse(TEXT("disabled data routing creates no knots"), Result.WasModified());
	TestTrue(TEXT("the backward data wire remains direct"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	TestFalse(TEXT("the disabled data no-op leaves the package clean"), Fixture.Graph->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteNarrowForwardObstacleTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.NarrowForwardGapFindsNonSelfIntersectingChannel",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteNarrowForwardObstacleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	UK2Node_Knot* MiddleObstacle = AddKnot(*Fixture.Graph, FVector2D(20.0, 90.0));
	FRerouteEdge Edge =
		MakeEdge(Fixture, TEXT("Forward.Narrow.LogicalEdge"), FVector2D(0.0, 100.0), FVector2D(48.0, 100.0));
	TArray<FRerouteObstacle> Obstacles = {
		FRerouteObstacle{ Fixture.Source,      FBox2D(FVector2D(-100.0, 60.0), FVector2D(0.0,   140.0)) },
		FRerouteObstacle{ Fixture.Destination, FBox2D(FVector2D(48.0,   60.0), FVector2D(148.0, 140.0)) },
		FRerouteObstacle{ MiddleObstacle,      FBox2D(FVector2D(20.0,   88.0), FVector2D(28.0,  112.0)) },
	};
	FRerouteSettings Settings;
	Settings.ObstacleClearance = 8.0;
	Settings.ChannelSpacing = 32.0;
	const FRerouteResult Result = RouteEdge(Fixture, Edge, Obstacles, Settings);

	TestEqual(TEXT("the obstructed narrow forward wire is routed"), Result.RoutedWires, 1);
	TestFalse(TEXT("the narrow route preserves topology safety"), Result.HasFatalError());
	TestTrue(TEXT("the narrow route reaches its destination"), GetGeneratedKnots(*Fixture.Graph).Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteStableGuidAndMetadataTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.GeneratedIdentityIsStableAndTagged",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteStableGuidAndMetadataTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString StableKey = TEXT("Stable.LogicalEdge");
	FRouteFixture FirstFixture = MakeBackwardFixture();
	FRouteFixture SecondFixture = MakeBackwardFixture();
	const FRerouteEdge FirstEdge = MakeEdge(FirstFixture, StableKey);
	const FRerouteEdge SecondEdge = MakeEdge(SecondFixture, StableKey);
	const FRerouteResult FirstResult = RouteEdge(FirstFixture, FirstEdge);
	const FRerouteResult SecondResult = RouteEdge(SecondFixture, SecondEdge);
	TArray<UK2Node_Knot*> FirstKnots = GetGeneratedKnots(*FirstFixture.Graph);
	TArray<UK2Node_Knot*> SecondKnots = GetGeneratedKnots(*SecondFixture.Graph);

	TestTrue(TEXT("first deterministic route succeeds"), FirstResult.WasModified());
	TestTrue(TEXT("second deterministic route succeeds"), SecondResult.WasModified());
	TestEqual(TEXT("equivalent routes have the same knot count"), FirstKnots.Num(), SecondKnots.Num());
	for (int32 Index = 0; Index < FMath::Min(FirstKnots.Num(), SecondKnots.Num()); ++Index)
	{
		UK2Node_Knot* FirstKnot = FirstKnots[Index];
		UK2Node_Knot* SecondKnot = SecondKnots[Index];
		FMetaData& FirstMetaData = FirstKnot->GetOutermost()->GetMetaData();
		TestEqual(FString::Printf(TEXT("knot %d has a stable GUID"), Index), FirstKnot->NodeGuid, SecondKnot->NodeGuid);
		TestEqual(
			FString::Printf(TEXT("knot %d records its logical edge"), Index),
			FirstMetaData.GetValue(FirstKnot, LogicalEdgeMetadataKey),
			StableKey
		);
		TestEqual(
			FString::Printf(TEXT("knot %d records its route ordinal"), Index),
			FCString::Atoi(*FirstMetaData.GetValue(FirstKnot, RouteOrdinalMetadataKey)),
			Index
		);
		TestEqual(
			FString::Printf(TEXT("knot %d carries its canonical GUID as the generated marker"), Index),
			FirstMetaData.GetValue(FirstKnot, GeneratedRerouteMetadataKey),
			FirstKnot->NodeGuid.ToString(EGuidFormats::Digits)
		);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteGeneratedMarkerAuthenticationTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.GeneratedMarkerRequiresExactLiveNodeGuid",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteGeneratedMarkerAuthenticationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString StableKey = TEXT("Authenticated.LogicalEdge");
	FRouteFixture Fixture = MakeBackwardFixture();
	const FRerouteResult Result = RouteEdge(Fixture, MakeEdge(Fixture, StableKey));
	TArray<UK2Node_Knot*> GeneratedKnots = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, StableKey);
	TestTrue(TEXT("the authentication fixture creates a route"), Result.WasModified());
	if (GeneratedKnots.IsEmpty())
	{
		AddError(TEXT("the authentication fixture produced no generated knots"));
		return true;
	}

	UK2Node_Knot* CopiedMarkerKnot = AddKnot(*Fixture.Graph, FVector2D(800.0, 400.0));
	FMetaData& MetaData = Fixture.Graph->GetOutermost()->GetMetaData();
	MetaData.SetValue(
		CopiedMarkerKnot, GeneratedRerouteMetadataKey, *MetaData.GetValue(GeneratedKnots[0], GeneratedRerouteMetadataKey)
	);
	MetaData.SetValue(CopiedMarkerKnot, LogicalEdgeMetadataKey, *StableKey);
	MetaData.SetValue(CopiedMarkerKnot, RouteOrdinalMetadataKey, TEXT("0"));
	TestFalse(
		TEXT("a stale or copied marker cannot authenticate a knot with a different live GUID"),
		FK2RerouteRouter::IsGeneratedRerouteNode(CopiedMarkerKnot)
	);
	FString CopiedKey;
	TestFalse(
		TEXT("a stale or copied marker cannot expose formatter route ownership"),
		FK2RerouteRouter::TryGetGeneratedRouteKey(CopiedMarkerKnot, CopiedKey)
	);

	TArray<UEdGraphNode*> RecoveredKnots;
	TArray<FVector2D> RecoveredWaypoints;
	TestTrue(
		TEXT("the unauthenticated copied marker is ignored when recovering the authentic chain"),
		FK2RerouteRouter::FindGeneratedRoute(*Fixture.Graph, StableKey, RecoveredKnots, RecoveredWaypoints)
	);
	TestEqual(TEXT("only the authentic knots are recovered"), RecoveredKnots.Num(), GeneratedKnots.Num());

	const FString OriginalOrdinal = MetaData.GetValue(GeneratedKnots[0], RouteOrdinalMetadataKey);
	MetaData.SetValue(GeneratedKnots[0], RouteOrdinalMetadataKey, TEXT("00"));
	TestFalse(
		TEXT("a parseable but non-canonical route ordinal is rejected"),
		FK2RerouteRouter::FindGeneratedRoute(*Fixture.Graph, StableKey, RecoveredKnots, RecoveredWaypoints)
	);
	MetaData.SetValue(GeneratedKnots[0], RouteOrdinalMetadataKey, *OriginalOrdinal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteCompetingRoutesTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.CompetingRoutesReserveDistinctDeterministicChannels",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteCompetingRoutesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRerouteSettings Settings;
	Settings.ChannelSpacing = 48.0;
	FCompetingRouteFixture CanonicalFixture = MakeCompetingRouteFixture(false);
	const FRerouteResult CanonicalResult = FK2RerouteRouter::Route(
		*CanonicalFixture.Graph, CanonicalFixture.Edges, TConstArrayView<FRerouteObstacle>(), CanonicalFixture.Scope, Settings, 16.0
	);
	TArray<UK2Node_Knot*> FirstRoute = GetGeneratedKnotsForLogicalEdge(*CanonicalFixture.Graph, TEXT("Competing.A"));
	TArray<UK2Node_Knot*> SecondRoute = GetGeneratedKnotsForLogicalEdge(*CanonicalFixture.Graph, TEXT("Competing.B"));

	TestEqual(TEXT("both competing logical wires are routed"), CanonicalResult.RoutedWires, 2);
	TestEqual(TEXT("no competing logical wire is skipped"), CanonicalResult.SkippedWires, 0);
	TestFalse(TEXT("competing routes preserve topology safety"), CanonicalResult.HasFatalError());
	TestEqual(TEXT("first competing route has four bends"), FirstRoute.Num(), 4);
	TestEqual(TEXT("second competing route has four bends"), SecondRoute.Num(), 4);
	if (FirstRoute.Num() == 4 && SecondRoute.Num() == 4)
	{
		const double RequiredXSeparation = 42.0 + Settings.ChannelSpacing;
		const double RequiredYSeparation = 24.0 + Settings.ChannelSpacing;
		for (UK2Node_Knot* FirstKnot : FirstRoute)
		{
			for (UK2Node_Knot* SecondKnot : SecondRoute)
			{
				const FVector2D Delta = (GetKnotCenter(*FirstKnot) - GetKnotCenter(*SecondKnot)).GetAbs();
				TestTrue(
					TEXT("reserved routes keep every generated knot box separated"),
					Delta.X + UE_DOUBLE_SMALL_NUMBER >= RequiredXSeparation
						|| Delta.Y + UE_DOUBLE_SMALL_NUMBER >= RequiredYSeparation
				);
			}
		}
		TestTrue(
			TEXT("competing wires occupy distinct horizontal channels"),
			FMath::Abs(GetKnotCenter(*FirstRoute[1]).Y - GetKnotCenter(*SecondRoute[1]).Y) >= Settings.ChannelSpacing
		);
	}

	FCompetingRouteFixture ReversedFixture = MakeCompetingRouteFixture(true);
	const FRerouteResult ReversedResult = FK2RerouteRouter::Route(
		*ReversedFixture.Graph, ReversedFixture.Edges, TConstArrayView<FRerouteObstacle>(), ReversedFixture.Scope, Settings, 16.0
	);
	TestEqual(TEXT("reversed input still routes both wires"), ReversedResult.RoutedWires, 2);
	for (const FString StableKey : { FString(TEXT("Competing.A")), FString(TEXT("Competing.B")) })
	{
		const TArray<UK2Node_Knot*> CanonicalKnots = GetGeneratedKnotsForLogicalEdge(*CanonicalFixture.Graph, StableKey);
		const TArray<UK2Node_Knot*> ReversedKnots = GetGeneratedKnotsForLogicalEdge(*ReversedFixture.Graph, StableKey);
		TestEqual(
			FString::Printf(TEXT("%s knot count is insertion-order invariant"), *StableKey),
			CanonicalKnots.Num(),
			ReversedKnots.Num()
		);
		for (int32 Index = 0; Index < FMath::Min(CanonicalKnots.Num(), ReversedKnots.Num()); ++Index)
		{
			TestEqual(
				FString::Printf(TEXT("%s knot %d position is insertion-order invariant"), *StableKey, Index),
				GetKnotCenter(*CanonicalKnots[Index]),
				GetKnotCenter(*ReversedKnots[Index])
			);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteDirectExecutionCrossingTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.DirectExecutionCrossingRoutesDeterministically",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteDirectExecutionCrossingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRerouteSettings Settings;
	FCrossingRouteFixture CanonicalFixture = MakeCrossingRouteFixture(true, false);
	const FRerouteResult CanonicalResult = FK2RerouteRouter::Route(
		*CanonicalFixture.Graph, CanonicalFixture.Edges, TConstArrayView<FRerouteObstacle>(), CanonicalFixture.Scope, Settings, 16.0
	);
	const int32 RemainingCanonicalDirectLinks =
		(HasDirectLink(*CanonicalFixture.FirstSource, *CanonicalFixture.FirstDestination) ? 1 : 0)
		+ (HasDirectLink(*CanonicalFixture.SecondSource, *CanonicalFixture.SecondDestination) ? 1 : 0);
	TestTrue(TEXT("at least one crossing execution baseline is routed"), CanonicalResult.RoutedWires >= 1);
	TestTrue(TEXT("a crossing direct link is replaced"), RemainingCanonicalDirectLinks < 2);
	TestTrue(
		TEXT("the lexically earlier equal-priority execution baseline remains direct"),
		HasDirectLink(*CanonicalFixture.FirstSource, *CanonicalFixture.FirstDestination)
	);
	TestFalse(
		TEXT("the lexically later equal-priority execution wire yields"),
		HasDirectLink(*CanonicalFixture.SecondSource, *CanonicalFixture.SecondDestination)
	);
	TestTrue(
		TEXT("crossing execution routing creates authenticated knots"),
		!GetGeneratedKnots(*CanonicalFixture.Graph).IsEmpty()
	);
	TestFalse(TEXT("crossing execution routing preserves topology safety"), CanonicalResult.HasFatalError());

	FCrossingRouteFixture ReversedFixture = MakeCrossingRouteFixture(true, true);
	const FRerouteResult ReversedResult = FK2RerouteRouter::Route(
		*ReversedFixture.Graph, ReversedFixture.Edges, TConstArrayView<FRerouteObstacle>(), ReversedFixture.Scope, Settings, 16.0
	);
	TestEqual(
		TEXT("crossing execution routing count is insertion-order invariant"),
		ReversedResult.RoutedWires,
		CanonicalResult.RoutedWires
	);
	for (const FString StableKey : { FString(TEXT("Crossing.Execution")), FString(TEXT("Crossing.OtherExecution")) })
	{
		const TArray<UK2Node_Knot*> CanonicalKnots = GetGeneratedKnotsForLogicalEdge(*CanonicalFixture.Graph, StableKey);
		const TArray<UK2Node_Knot*> ReversedKnots = GetGeneratedKnotsForLogicalEdge(*ReversedFixture.Graph, StableKey);
		TestEqual(
			FString::Printf(TEXT("%s generated knot count is insertion-order invariant"), *StableKey),
			ReversedKnots.Num(),
			CanonicalKnots.Num()
		);
		for (int32 Index = 0; Index < FMath::Min(CanonicalKnots.Num(), ReversedKnots.Num()); ++Index)
		{
			TestEqual(
				FString::Printf(TEXT("%s knot %d position is insertion-order invariant"), *StableKey, Index),
				GetKnotCenter(*ReversedKnots[Index]),
				GetKnotCenter(*CanonicalKnots[Index])
			);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteMixedCrossingPolicyTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.DataCrossingYieldsToExecutionSpine",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteMixedCrossingPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCrossingRouteFixture Fixture = MakeCrossingRouteFixture(false);
	const FRerouteSettings Settings;
	const FRerouteResult Result = FK2RerouteRouter::Route(
		*Fixture.Graph, Fixture.Edges, TConstArrayView<FRerouteObstacle>(), Fixture.Scope, Settings, 16.0
	);

	TestEqual(TEXT("only the crossing data wire is routed"), Result.RoutedWires, 1);
	TestTrue(
		TEXT("the primary execution spine remains direct"), HasDirectLink(*Fixture.FirstSource, *Fixture.FirstDestination)
	);
	TestFalse(
		TEXT("the data wire yields to the execution spine"), HasDirectLink(*Fixture.SecondSource, *Fixture.SecondDestination)
	);
	TestTrue(
		TEXT("the replacement knots belong to the data wire"),
		!GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("Crossing.Data")).IsEmpty()
	);
	TestTrue(
		TEXT("the execution spine receives no generated knots"),
		GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("Crossing.Execution")).IsEmpty()
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteSharedTerminalCrossingExemptionTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.SharedTerminalFanOutDoesNotTriggerRouting",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteSharedTerminalCrossingExemptionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UEdGraph* Graph = MakeGraph();
	UK2Node_Knot* Source = AddKnot(*Graph, FVector2D(0.0, 100.0));
	UK2Node_Knot* TopDestination = AddKnot(*Graph, FVector2D(400.0, 0.0));
	UK2Node_Knot* BottomDestination = AddKnot(*Graph, FVector2D(400.0, 200.0));
	Connect(*Graph, *Source, *TopDestination);
	Connect(*Graph, *Source, *BottomDestination);

	FRerouteEdge TopEdge;
	TopEdge.OutputPin = Source->GetOutputPin();
	TopEdge.InputPin = TopDestination->GetInputPin();
	TopEdge.OutputAnchor = FVector2D(0.0, 100.0);
	TopEdge.InputAnchor = FVector2D(400.0, 0.0);
	TopEdge.StableKey = TEXT("SharedTerminal.Top");
	TopEdge.bExecution = true;
	FRerouteEdge BottomEdge;
	BottomEdge.OutputPin = Source->GetOutputPin();
	BottomEdge.InputPin = BottomDestination->GetInputPin();
	BottomEdge.OutputAnchor = FVector2D(0.0, 100.0);
	BottomEdge.InputAnchor = FVector2D(400.0, 200.0);
	BottomEdge.StableKey = TEXT("SharedTerminal.Bottom");
	BottomEdge.bExecution = true;
	const TArray<FRerouteEdge> Edges = { TopEdge, BottomEdge };
	const TSet<UEdGraphNode*> Scope = { Source, TopDestination, BottomDestination };
	const FRerouteSettings Settings;
	const FRerouteResult Result =
		FK2RerouteRouter::Route(*Graph, Edges, TConstArrayView<FRerouteObstacle>(), Scope, Settings, 16.0);

	TestEqual(TEXT("a legitimate shared-terminal fan-out is not routed"), Result.RoutedWires, 0);
	TestTrue(TEXT("the top fan-out branch stays direct"), HasDirectLink(*Source, *TopDestination));
	TestTrue(TEXT("the bottom fan-out branch stays direct"), HasDirectLink(*Source, *BottomDestination));
	TestTrue(TEXT("the terminal exemption creates no knots"), GetGeneratedKnots(*Graph).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2ReroutePlanningBudgetTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.DensePlanningBudgetIsBoundedAndDeterministic",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2ReroutePlanningBudgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRerouteSettings Settings;
	Settings.PlanningWorkBudget = 5;
	FDenseRouteFixture CanonicalFixture = MakeDenseRouteFixture(false);
	const FRerouteResult CanonicalResult = FK2RerouteRouter::Route(
		*CanonicalFixture.Graph, CanonicalFixture.Edges, TConstArrayView<FRerouteObstacle>(), CanonicalFixture.Scope, Settings, 16.0
	);
	TestTrue(TEXT("the dense pass reports planning-budget exhaustion"), CanonicalResult.bPlanningBudgetExhausted);
	TestEqual(TEXT("budget exhaustion installs no routes"), CanonicalResult.RoutedWires, 0);
	TestEqual(TEXT("budget exhaustion creates no knots"), CanonicalResult.CreatedKnots, 0);
	TestEqual(TEXT("the wire where bounded planning stops is reported as skipped"), CanonicalResult.SkippedWires, 1);
	TestEqual(TEXT("budget exhaustion emits one deterministic diagnostic"), CanonicalResult.Diagnostics.Num(), 1);
	if (CanonicalResult.Diagnostics.Num() == 1)
	{
		TestTrue(
			TEXT("the diagnostic identifies the deterministic dense edge where work stopped"),
			CanonicalResult.Diagnostics[0].Contains(TEXT("Dense."))
		);
		TestTrue(
			TEXT("the diagnostic identifies work-budget exhaustion"),
			CanonicalResult.Diagnostics[0].Contains(TEXT("work budget was exhausted"))
		);
	}
	for (int32 Index = 0; Index < CanonicalFixture.Sources.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("dense direct wire %d remains unchanged"), Index),
			HasDirectLink(*CanonicalFixture.Sources[Index], *CanonicalFixture.Destinations[Index])
		);
	}

	FDenseRouteFixture ReversedFixture = MakeDenseRouteFixture(true);
	const FRerouteResult ReversedResult = FK2RerouteRouter::Route(
		*ReversedFixture.Graph, ReversedFixture.Edges, TConstArrayView<FRerouteObstacle>(), ReversedFixture.Scope, Settings, 16.0
	);
	TestTrue(TEXT("reversed dense input exhausts the same bounded budget"), ReversedResult.bPlanningBudgetExhausted);
	TestEqual(
		TEXT("reversed dense input emits the same diagnostic count"),
		ReversedResult.Diagnostics.Num(),
		CanonicalResult.Diagnostics.Num()
	);
	if (ReversedResult.Diagnostics.Num() == 1 && CanonicalResult.Diagnostics.Num() == 1)
	{
		TestEqual(
			TEXT("budget diagnostic is insertion-order invariant"),
			ReversedResult.Diagnostics[0],
			CanonicalResult.Diagnostics[0]
		);
	}
	TestEqual(TEXT("reversed dense input also creates no knots"), ReversedResult.CreatedKnots, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteFutureBaselineReservationTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.LaterDirectBaselineConstrainsEarlierRoute",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteFutureBaselineReservationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCompetingRouteFixture Fixture = MakeCompetingRouteFixture(false);
	FRerouteEdge& RoutedEdge = Fixture.Edges[0];
	RoutedEdge.StableKey = TEXT("A.Route");
	RoutedEdge.OutputAnchor = FVector2D(400.0, 100.0);
	RoutedEdge.InputAnchor = FVector2D(100.0, 100.0);
	FRerouteEdge& LaterBaseline = Fixture.Edges[1];
	LaterBaseline.StableKey = TEXT("Z.Baseline");
	// This shallow angle stays outside the generated-knot boxes but within one channel of the
	// upper candidate. It exercises distance/angle crowding on flattened spline microsegments,
	// rather than the old exact-horizontal special case.
	LaterBaseline.OutputAnchor = FVector2D(0.0, 0.0);
	LaterBaseline.InputAnchor = FVector2D(400.0, 8.0);
	FRerouteSettings Settings;
	Settings.ChannelSpacing = 48.0;
	const FRerouteResult Result = FK2RerouteRouter::Route(
		*Fixture.Graph, Fixture.Edges, TConstArrayView<FRerouteObstacle>(), Fixture.Scope, Settings, 16.0
	);
	const TArray<UK2Node_Knot*> RoutedKnots = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("A.Route"));

	TestEqual(TEXT("only the obstructed/backward logical edge is routed"), Result.RoutedWires, 1);
	TestTrue(
		TEXT("the lexically later angled baseline remains direct"),
		LaterBaseline.OutputPin->LinkedTo.Contains(LaterBaseline.InputPin)
	);
	if (RoutedKnots.Num() >= 3)
	{
		TestTrue(
			TEXT("the earlier route avoids crowding the angled baseline's top channel"),
			GetKnotCenter(*RoutedKnots[1]).Y > RoutedEdge.OutputAnchor.Y
		);
	}
	else
	{
		AddError(TEXT("the constrained route did not create its expected channel bends"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteExistingRouteReservationTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.ExistingGeneratedRouteReservesItsChannel",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteExistingRouteReservationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRerouteSettings Settings;
	Settings.ChannelSpacing = 48.0;
	FCompetingRouteFixture Fixture = MakeCompetingRouteFixture(false);
	const FRerouteResult FirstResult = FK2RerouteRouter::Route(
		*Fixture.Graph, MakeArrayView(&Fixture.Edges[0], 1), TConstArrayView<FRerouteObstacle>(), Fixture.Scope, Settings, 16.0
	);
	TestEqual(TEXT("the first pass installs one route"), FirstResult.RoutedWires, 1);

	FRerouteEdge ExistingRoute = Fixture.Edges[0];
	TArray<UEdGraphNode*> ExistingKnots;
	ExistingRoute.bExistingGeneratedRoute = true;
	TestTrue(
		TEXT("the first route can be recovered in stable ordinal order"),
		FK2RerouteRouter::FindGeneratedRoute(*Fixture.Graph, ExistingRoute.StableKey, ExistingKnots, ExistingRoute.PreferredWaypoints)
	);
	TArray<FRerouteEdge> SecondPassEdges = { Fixture.Edges[1], ExistingRoute };
	const FRerouteResult SecondResult = FK2RerouteRouter::Route(
		*Fixture.Graph, SecondPassEdges, TConstArrayView<FRerouteObstacle>(), Fixture.Scope, Settings, 16.0
	);
	const TArray<UK2Node_Knot*> FirstRoute = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("Competing.A"));
	const TArray<UK2Node_Knot*> SecondRoute = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("Competing.B"));

	TestEqual(TEXT("the second pass installs only the new logical route"), SecondResult.RoutedWires, 1);
	TestEqual(TEXT("the recovered route is a reservation, not a skipped wire"), SecondResult.SkippedWires, 0);
	TestEqual(TEXT("the existing route is not extended"), FirstRoute.Num(), ExistingKnots.Num());
	if (FirstRoute.Num() >= 2 && SecondRoute.Num() >= 2)
	{
		TestTrue(
			TEXT("the new route avoids the recovered route's occupied horizontal channel"),
			FMath::Abs(GetKnotCenter(*FirstRoute[1]).Y - GetKnotCenter(*SecondRoute[1]).Y) >= Settings.ChannelSpacing
		);
	}
	else
	{
		AddError(TEXT("both logical routes must retain enough knots to compare their occupied channels"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2ReroutePreferredWaypointsTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.PreferredCoreWaypointsAreHonored",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2ReroutePreferredWaypointsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	FRerouteEdge Edge = MakeEdge(Fixture, TEXT("Preferred.LogicalEdge"), FVector2D(0.0, 100.0), FVector2D(400.0, 100.0));
	Edge.PreferredWaypoints = {
		FVector2D(64.0, 100.0),
		FVector2D(64.0, 200.0),
		FVector2D(336.0, 200.0),
		FVector2D(336.0, 100.0),
	};
	const FRerouteResult Result = RouteEdge(Fixture, Edge);
	const TArray<UK2Node_Knot*> Knots = GetGeneratedKnotsForLogicalEdge(*Fixture.Graph, TEXT("Preferred.LogicalEdge"));

	TestEqual(TEXT("preferred route is installed"), Result.RoutedWires, 1);
	TestEqual(TEXT("preferred route retains all four meaningful bends"), Knots.Num(), Edge.PreferredWaypoints.Num());
	for (int32 Index = 0; Index < FMath::Min(Knots.Num(), Edge.PreferredWaypoints.Num()); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("preferred waypoint %d is retained"), Index),
			GetKnotCenter(*Knots[Index]),
			Edge.PreferredWaypoints[Index]
		);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteStraightPreferredRouteTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.StraightCoreHintDoesNotCreateDogleg",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteStraightPreferredRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	FRerouteEdge Edge =
		MakeEdge(Fixture, TEXT("Preferred.Straight.LogicalEdge"), FVector2D(0.0, 100.0), FVector2D(400.0, 100.0));
	Edge.PreferredWaypoints = { FVector2D(128.0, 100.0), FVector2D(256.0, 100.0) };
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	Fixture.Graph->GetOutermost()->SetDirtyFlag(false);
	const FRerouteResult Result = RouteEdge(Fixture, Edge);

	TestFalse(TEXT("a collinear core hint leaves a clear execution wire direct"), Result.WasModified());
	TestEqual(TEXT("a collinear core hint creates no reroute nodes"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestTrue(
		TEXT("the clear execution edge remains directly connected"), HasDirectLink(*Fixture.Source, *Fixture.Destination)
	);
	TestFalse(TEXT("the no-op does not dirty the package"), Fixture.Graph->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteMalformedDestinationFanInTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.MalformedDestinationFanInIsNeverRewritten",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteMalformedDestinationFanInTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteFixture Fixture = MakeBackwardFixture();
	UK2Node_Knot* OtherSource = AddKnot(*Fixture.Graph, FVector2D(400.0, 260.0));
	OtherSource->GetOutputPin()->MakeLinkTo(Fixture.Destination->GetInputPin(), false);
	const int32 NodeCountBefore = Fixture.Graph->Nodes.Num();
	Fixture.Graph->GetOutermost()->SetDirtyFlag(false);
	const FRerouteResult Result = RouteEdge(Fixture, MakeEdge(Fixture, TEXT("Malformed.FanIn")));

	TestFalse(TEXT("a malformed destination fan-in is not rewritten"), Result.WasModified());
	TestEqual(TEXT("no staging nodes survive the rejected rewrite"), Fixture.Graph->Nodes.Num(), NodeCountBefore);
	TestTrue(TEXT("the requested direct relationship is preserved"), HasDirectLink(*Fixture.Source, *Fixture.Destination));
	TestTrue(TEXT("the unrelated malformed relationship is preserved"), HasDirectLink(*OtherSource, *Fixture.Destination));
	TestEqual(TEXT("both original destination references remain"), Fixture.Destination->GetInputPin()->LinkedTo.Num(), 2);
	TestFalse(TEXT("the preflight rejection leaves a clean package clean"), Fixture.Graph->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2RerouteBoundaryFailureRollbackTest,
	"Project.Unit Tests.GraphFormatter.K2Routing.BoundaryFailureRestoresExactOriginalTopology",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2RerouteBoundaryFailureRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UEdGraph* Graph = MakeGraph();
	UK2Node_Knot* First = AddKnot(*Graph, FVector2D(400.0, 100.0));
	UK2Node_Knot* Second = AddKnot(*Graph, FVector2D(100.0, 100.0));
	UEdGraphPin* FirstInput = First->GetInputPin();
	UEdGraphPin* SecondInput = Second->GetInputPin();
	FirstInput->MakeLinkTo(SecondInput, false);
	const int32 NodeCountBefore = Graph->Nodes.Num();
	Graph->GetOutermost()->SetDirtyFlag(false);

	FRerouteEdge Edge;
	Edge.OutputPin = FirstInput;
	Edge.InputPin = SecondInput;
	Edge.OutputAnchor = FVector2D(400.0, 100.0);
	Edge.InputAnchor = FVector2D(100.0, 100.0);
	Edge.StableKey = TEXT("Rollback.InvalidBoundary");
	Edge.bExecution = true;
	TSet<UEdGraphNode*> Scope = { First, Second };
	const FRerouteSettings Settings;
	const FRerouteResult Result = FK2RerouteRouter::Route(
		*Graph, MakeArrayView(&Edge, 1), TConstArrayView<FRerouteObstacle>(), Scope, Settings, 16.0
	);

	TestEqual(TEXT("schema-rejected boundary route is skipped"), Result.SkippedWires, 1);
	TestEqual(TEXT("failed staging leaves no generated knots"), Result.CreatedKnots, 0);
	TestEqual(TEXT("failed staging removes every staged graph node"), Graph->Nodes.Num(), NodeCountBefore);
	TestFalse(TEXT("last-resort normalization restores topology without a fatal error"), Result.HasFatalError());
	TestFalse(TEXT("fully restored failure leaves a previously clean package clean"), Graph->GetOutermost()->IsDirty());
	TestEqual(TEXT("first pin has exactly one restored reference"), FirstInput->LinkedTo.Num(), 1);
	TestEqual(TEXT("second pin has exactly one restored reference"), SecondInput->LinkedTo.Num(), 1);
	TestTrue(TEXT("first pin references the exact original peer"), FirstInput->LinkedTo.Contains(SecondInput));
	TestTrue(TEXT("second pin reciprocally references the exact original peer"), SecondInput->LinkedTo.Contains(FirstInput));
	TestTrue(
		TEXT("rollback diagnostic claims restoration only after verification"),
		Result.Diagnostics.Num() == 1 && Result.Diagnostics[0].Contains(TEXT("restored and verified"))
	);
	return true;
}
} // namespace GraphFormatter::K2::Tests

#endif
