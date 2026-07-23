/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/K2LayoutCore.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"

namespace GraphFormatter::K2Layout::Tests
{
namespace
{
constexpr double PositionTolerance = 0.001;

FPortSnapshot MakePort(
	const TCHAR* Key,
	const EPortDirection Direction,
	const EPortKind Kind,
	const double OffsetY,
	const int32 SemanticOrder = 0,
	const bool bPreferredExecutionPort = false
)
{
	FPortSnapshot Port;
	Port.Key = FPortKey{ FString{ Key } };
	Port.Direction = Direction;
	Port.Kind = Kind;
	Port.Offset = FVector2D{ 0.0, OffsetY };
	Port.SemanticOrder = SemanticOrder;
	Port.bPreferredExecutionPort = bPreferredExecutionPort;
	return Port;
}

FNodeSnapshot MakeNode(const TCHAR* Key, const bool bIsPure = false, const FVector2D Size = FVector2D{ 128.0, 80.0 })
{
	FNodeSnapshot Node;
	Node.Key = FNodeKey{ FString{ Key } };
	Node.Size = Size;
	Node.bIsPure = bIsPure;
	return Node;
}

FNodeSnapshot WithOriginalPosition(FNodeSnapshot Node, const FVector2D Position)
{
	Node.OriginalPosition = Position;
	Node.bHasOriginalPosition = true;
	return Node;
}

FNodeSnapshot MakeExecutionNode(
	const TCHAR* Key, const double InputY = 32.0, const double OutputY = 32.0, const FVector2D Size = FVector2D{ 128.0, 80.0 }
)
{
	FNodeSnapshot Node = MakeNode(Key, false, Size);
	Node.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, InputY, 0, true));
	Node.Ports.Add(MakePort(TEXT("ExecOut"), EPortDirection::Output, EPortKind::Execution, OutputY, 0, true));
	return Node;
}

FNodeSnapshot MakePureProvider(const TCHAR* Key, const double OutputY = 24.0, const FVector2D Size = FVector2D{ 96.0, 64.0 })
{
	FNodeSnapshot Node = MakeNode(Key, true, Size);
	Node.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, OutputY));
	return Node;
}

FExecutionEdgeSnapshot MakeExecutionEdge(
	const TCHAR* Key,
	const TCHAR* SourceNode,
	const TCHAR* SourcePort,
	const TCHAR* TargetNode,
	const TCHAR* TargetPort,
	const int32 BranchOrder = INDEX_NONE,
	const bool bPreferredAlignment = true
)
{
	FExecutionEdgeSnapshot Edge;
	Edge.Key = FEdgeKey{ FString{ Key } };
	Edge.Source = FPortReference{ FNodeKey{ FString{ SourceNode } }, FPortKey{ FString{ SourcePort } } };
	Edge.Target = FPortReference{ FNodeKey{ FString{ TargetNode } }, FPortKey{ FString{ TargetPort } } };
	Edge.BranchOrder = BranchOrder;
	Edge.bPreferredAlignment = bPreferredAlignment;
	return Edge;
}

FDataEdgeSnapshot MakeDataEdge(
	const TCHAR* Key, const TCHAR* SourceNode, const TCHAR* SourcePort, const TCHAR* TargetNode, const TCHAR* TargetPort
)
{
	FDataEdgeSnapshot Edge;
	Edge.Key = FEdgeKey{ FString{ Key } };
	Edge.Source = FPortReference{ FNodeKey{ FString{ SourceNode } }, FPortKey{ FString{ SourcePort } } };
	Edge.Target = FPortReference{ FNodeKey{ FString{ TargetNode } }, FPortKey{ FString{ TargetPort } } };
	return Edge;
}

const FPlannedNodePosition* FindPlannedNode(const FLayoutPlan& Plan, const TCHAR* Key)
{
	return Plan.Nodes.FindByPredicate([Key](const FPlannedNodePosition& Node)
									  { return Node.Node.Value.Equals(Key, ESearchCase::CaseSensitive); });
}

const FNodeSnapshot* FindSnapshotNode(const FGraphSnapshot& Snapshot, const TCHAR* Key)
{
	return Snapshot.Nodes.FindByPredicate([Key](const FNodeSnapshot& Node)
										  { return Node.Key.Value.Equals(Key, ESearchCase::CaseSensitive); });
}

const FPlannedEdgeRoute* FindPlannedRoute(const FLayoutPlan& Plan, const FString& Key)
{
	return Plan.ExecutionRoutes.FindByPredicate([&Key](const FPlannedEdgeRoute& Route)
												{ return Route.Edge.Value.Equals(Key, ESearchCase::CaseSensitive); });
}

bool NearlyEqual(const FVector2D& A, const FVector2D& B) { return A.Equals(B, PositionTolerance); }

bool IsFinite(const FVector2D& Value) { return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y); }

bool IsOnGrid(const double Value, const double Grid)
{
	if (Grid <= 0.0) { return true; }
	return FMath::IsNearlyEqual(Value / Grid, FMath::RoundToDouble(Value / Grid), PositionTolerance);
}

struct FTestRect
{
	double Left{ 0.0 };
	double Top{ 0.0 };
	double Right{ 0.0 };
	double Bottom{ 0.0 };
};

FTestRect MakeRect(const FPlannedNodePosition& Node, const FNodeSnapshot& SnapshotNode)
{
	return FTestRect{
		Node.Position.X,
		Node.Position.Y,
		Node.Position.X + SnapshotNode.Size.X,
		Node.Position.Y + SnapshotNode.Size.Y,
	};
}

bool Overlaps(const FTestRect& A, const FTestRect& B)
{
	return A.Left < B.Right - PositionTolerance && A.Right > B.Left + PositionTolerance
		&& A.Top < B.Bottom - PositionTolerance && A.Bottom > B.Top + PositionTolerance;
}

bool TestAllNodePairsDoNotOverlap(
	FAutomationTestBase& Test, const FString& Context, const FGraphSnapshot& Snapshot, const FLayoutPlan& Plan
)
{
	bool bNoOverlaps = true;
	for (int32 LeftIndex = 0; LeftIndex < Plan.Nodes.Num(); ++LeftIndex)
	{
		const FPlannedNodePosition& Left = Plan.Nodes[LeftIndex];
		const FNodeSnapshot* LeftSnapshot = FindSnapshotNode(Snapshot, *Left.Node.Value);
		if (LeftSnapshot == nullptr)
		{
			Test.AddError(*FString::Printf(TEXT("%s: missing snapshot node '%s'"), *Context, *Left.Node.Value));
			bNoOverlaps = false;
			continue;
		}

		for (int32 RightIndex = LeftIndex + 1; RightIndex < Plan.Nodes.Num(); ++RightIndex)
		{
			const FPlannedNodePosition& Right = Plan.Nodes[RightIndex];
			const FNodeSnapshot* RightSnapshot = FindSnapshotNode(Snapshot, *Right.Node.Value);
			if (RightSnapshot == nullptr)
			{
				Test.AddError(*FString::Printf(TEXT("%s: missing snapshot node '%s'"), *Context, *Right.Node.Value));
				bNoOverlaps = false;
				continue;
			}

			const bool bOverlaps = Overlaps(MakeRect(Left, *LeftSnapshot), MakeRect(Right, *RightSnapshot));
			Test.TestFalse(
				*FString::Printf(TEXT("%s: nodes '%s' and '%s' do not overlap"), *Context, *Left.Node.Value, *Right.Node.Value),
				bOverlaps
			);
			bNoOverlaps &= !bOverlaps;
		}
	}
	return bNoOverlaps;
}

bool PlansMatch(
	FAutomationTestBase& Test,
	const FString& Context,
	const FLayoutPlan& Expected,
	const FLayoutPlan& Actual,
	const bool bCompareDisplacement = true
)
{
	bool bMatches = true;
	const auto Check = [&Test, &Context, &bMatches](const FString& Detail, const bool bCondition)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: %s"), *Context, *Detail), bCondition);
		bMatches &= bCondition;
	};

	Check(TEXT("node counts match"), Expected.Nodes.Num() == Actual.Nodes.Num());
	for (const FPlannedNodePosition& ExpectedNode : Expected.Nodes)
	{
		const FPlannedNodePosition* ActualNode = FindPlannedNode(Actual, *ExpectedNode.Node.Value);
		Check(FString::Printf(TEXT("node '%s' exists"), *ExpectedNode.Node.Value), ActualNode != nullptr);
		if (ActualNode == nullptr) { continue; }
		Check(
			FString::Printf(TEXT("node '%s' position matches"), *ExpectedNode.Node.Value),
			NearlyEqual(ExpectedNode.Position, ActualNode->Position)
		);
		Check(
			FString::Printf(TEXT("node '%s' component matches"), *ExpectedNode.Node.Value),
			ExpectedNode.ComponentIndex == ActualNode->ComponentIndex
		);
		Check(
			FString::Printf(TEXT("node '%s' rank matches"), *ExpectedNode.Node.Value),
			ExpectedNode.ExecutionRank == ActualNode->ExecutionRank
		);
		Check(
			FString::Printf(TEXT("node '%s' order matches"), *ExpectedNode.Node.Value),
			ExpectedNode.OrderInRank == ActualNode->OrderInRank
		);
	}

	Check(TEXT("route counts match"), Expected.ExecutionRoutes.Num() == Actual.ExecutionRoutes.Num());
	for (const FPlannedEdgeRoute& ExpectedRoute : Expected.ExecutionRoutes)
	{
		const FPlannedEdgeRoute* ActualRoute = FindPlannedRoute(Actual, ExpectedRoute.Edge.Value);
		Check(FString::Printf(TEXT("route '%s' exists"), *ExpectedRoute.Edge.Value), ActualRoute != nullptr);
		if (ActualRoute == nullptr) { continue; }
		Check(
			FString::Printf(TEXT("route '%s' waypoint count matches"), *ExpectedRoute.Edge.Value),
			ExpectedRoute.Waypoints.Num() == ActualRoute->Waypoints.Num()
		);
		const int32 CommonWaypointCount = FMath::Min(ExpectedRoute.Waypoints.Num(), ActualRoute->Waypoints.Num());
		for (int32 WaypointIndex = 0; WaypointIndex < CommonWaypointCount; ++WaypointIndex)
		{
			Check(
				FString::Printf(TEXT("route '%s' waypoint %d matches"), *ExpectedRoute.Edge.Value, WaypointIndex),
				NearlyEqual(ExpectedRoute.Waypoints[WaypointIndex], ActualRoute->Waypoints[WaypointIndex])
			);
		}
	}

	Check(TEXT("diagnostic counts match"), Expected.Diagnostics.Num() == Actual.Diagnostics.Num());
	const int32 CommonDiagnosticCount = FMath::Min(Expected.Diagnostics.Num(), Actual.Diagnostics.Num());
	for (int32 DiagnosticIndex = 0; DiagnosticIndex < CommonDiagnosticCount; ++DiagnosticIndex)
	{
		const FLayoutDiagnostic& ExpectedDiagnostic = Expected.Diagnostics[DiagnosticIndex];
		const FLayoutDiagnostic& ActualDiagnostic = Actual.Diagnostics[DiagnosticIndex];
		Check(
			FString::Printf(TEXT("diagnostic %d severity matches"), DiagnosticIndex),
			ExpectedDiagnostic.Severity == ActualDiagnostic.Severity
		);
		Check(
			FString::Printf(TEXT("diagnostic %d code matches"), DiagnosticIndex),
			ExpectedDiagnostic.Code == ActualDiagnostic.Code
		);
		Check(
			FString::Printf(TEXT("diagnostic %d subject matches"), DiagnosticIndex),
			ExpectedDiagnostic.SubjectKey == ActualDiagnostic.SubjectKey
		);
		Check(
			FString::Printf(TEXT("diagnostic %d message matches"), DiagnosticIndex),
			ExpectedDiagnostic.Message == ActualDiagnostic.Message
		);
	}

	const FLayoutStatistics& ExpectedStats = Expected.Statistics;
	const FLayoutStatistics& ActualStats = Actual.Statistics;
	Check(TEXT("accepted node count matches"), ExpectedStats.AcceptedNodeCount == ActualStats.AcceptedNodeCount);
	Check(
		TEXT("accepted execution edge count matches"),
		ExpectedStats.AcceptedExecutionEdgeCount == ActualStats.AcceptedExecutionEdgeCount
	);
	Check(TEXT("accepted data edge count matches"), ExpectedStats.AcceptedDataEdgeCount == ActualStats.AcceptedDataEdgeCount);
	Check(TEXT("component count matches"), ExpectedStats.ComponentCount == ActualStats.ComponentCount);
	Check(
		TEXT("condensed cycle count matches"),
		ExpectedStats.CondensedExecutionCycleCount == ActualStats.CondensedExecutionCycleCount
	);
	Check(TEXT("virtual node count matches"), ExpectedStats.VirtualNodeCount == ActualStats.VirtualNodeCount);
	Check(
		TEXT("initial crossing count matches"), ExpectedStats.InitialExecutionCrossings == ActualStats.InitialExecutionCrossings
	);
	Check(TEXT("final crossing count matches"), ExpectedStats.FinalExecutionCrossings == ActualStats.FinalExecutionCrossings);
	Check(TEXT("authored component count matches"), ExpectedStats.AuthoredComponentCount == ActualStats.AuthoredComponentCount);
	if (bCompareDisplacement)
	{
		Check(
			TEXT("total node displacement matches"),
			FMath::IsNearlyEqual(ExpectedStats.TotalNodeDisplacement, ActualStats.TotalNodeDisplacement, PositionTolerance)
		);
	}
	return bMatches;
}

template<typename ElementType>
void ReverseInsertionOrder(TArray<ElementType>& Values)
{
	for (int32 Left = 0, Right = Values.Num() - 1; Left < Right; ++Left, --Right)
	{
		Values.Swap(Left, Right);
	}
}

template<typename ElementType>
void DeterministicShuffle(TArray<ElementType>& Values, uint32& State)
{
	for (int32 Index = Values.Num() - 1; Index > 0; --Index)
	{
		State = State * 1664525U + 1013904223U;
		Values.Swap(Index, static_cast<int32>(State % static_cast<uint32>(Index + 1)));
	}
}

void ReverseSnapshotInsertion(FGraphSnapshot& Snapshot)
{
	for (FNodeSnapshot& Node : Snapshot.Nodes)
	{
		ReverseInsertionOrder(Node.Ports);
	}
	ReverseInsertionOrder(Snapshot.Nodes);
	ReverseInsertionOrder(Snapshot.ExecutionEdges);
	ReverseInsertionOrder(Snapshot.DataEdges);
}

void ShuffleSnapshotInsertion(FGraphSnapshot& Snapshot, const uint32 Seed)
{
	uint32 State = Seed;
	for (FNodeSnapshot& Node : Snapshot.Nodes)
	{
		DeterministicShuffle(Node.Ports, State);
	}
	DeterministicShuffle(Snapshot.Nodes, State);
	DeterministicShuffle(Snapshot.ExecutionEdges, State);
	DeterministicShuffle(Snapshot.DataEdges, State);
}

FGraphSnapshot MakeDeterminismGraph()
{
	FGraphSnapshot Snapshot;

	FNodeSnapshot Entry = MakeNode(TEXT("Entry"));
	Entry.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 32.0));
	Entry.Ports.Add(MakePort(TEXT("Then0"), EPortDirection::Output, EPortKind::Execution, 30.0, 0, true));
	Entry.Ports.Add(MakePort(TEXT("Then1"), EPortDirection::Output, EPortKind::Execution, 58.0, 1));
	Snapshot.Nodes.Add(MoveTemp(Entry));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("Alpha"), 28.0, 42.0));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("Beta"), 28.0, 48.0));

	FNodeSnapshot Merge = MakeExecutionNode(TEXT("Merge"), 36.0, 44.0, FVector2D{ 144.0, 96.0 });
	Merge.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 72.0, 0));
	Snapshot.Nodes.Add(MoveTemp(Merge));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("Exit"), 32.0, 32.0));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("Provider")));

	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("EntryToAlpha"), TEXT("Entry"), TEXT("Then0"), TEXT("Alpha"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("EntryToBeta"), TEXT("Entry"), TEXT("Then1"), TEXT("Beta"), TEXT("ExecIn"), 1)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("AlphaToMerge"), TEXT("Alpha"), TEXT("ExecOut"), TEXT("Merge"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("BetaToMerge"), TEXT("Beta"), TEXT("ExecOut"), TEXT("Merge"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("MergeToExit"), TEXT("Merge"), TEXT("ExecOut"), TEXT("Exit"), TEXT("ExecIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("ProviderToMerge"), TEXT("Provider"), TEXT("ValueOut"), TEXT("Merge"), TEXT("ValueIn"))
	);
	return Snapshot;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutInsertionOrderDeterminismTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Determinism.InsertionOrder",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutInsertionOrderDeterminismTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot CanonicalSnapshot = MakeDeterminismGraph();
	const FLayoutPlan CanonicalPlan = BuildLayout(CanonicalSnapshot);
	TestFalse(TEXT("canonical graph is valid"), CanonicalPlan.HasErrors());

	FGraphSnapshot ReversedSnapshot = CanonicalSnapshot;
	ReverseSnapshotInsertion(ReversedSnapshot);
	const FLayoutPlan ReversedPlan = BuildLayout(ReversedSnapshot);
	PlansMatch(*this, TEXT("reversed insertion"), CanonicalPlan, ReversedPlan);

	for (uint32 Seed = 1; Seed <= 8; ++Seed)
	{
		FGraphSnapshot ShuffledSnapshot = CanonicalSnapshot;
		ShuffleSnapshotInsertion(ShuffledSnapshot, Seed * 0x9e3779b9U);
		const FLayoutPlan ShuffledPlan = BuildLayout(ShuffledSnapshot);
		PlansMatch(*this, FString::Printf(TEXT("shuffle seed %u"), Seed), CanonicalPlan, ShuffledPlan);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutCrossingInvariantTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Crossings.ExactCountAndMonotonicFinalOrder",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutCrossingInvariantTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("S0")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("S1")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("T0")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("T1")));
	// K2,2 has exactly one inversion in the stable S0,S1 / T0,T1 order. Pairs sharing
	// either endpoint must not be counted as crossings.
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("S0-T0"), TEXT("S0"), TEXT("ExecOut"), TEXT("T0"), TEXT("ExecIn")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("S0-T1"), TEXT("S0"), TEXT("ExecOut"), TEXT("T1"), TEXT("ExecIn")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("S1-T0"), TEXT("S1"), TEXT("ExecOut"), TEXT("T0"), TEXT("ExecIn")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("S1-T1"), TEXT("S1"), TEXT("ExecOut"), TEXT("T1"), TEXT("ExecIn")));

	const FLayoutPlan Plan = BuildLayout(Snapshot);
	TestFalse(TEXT("the crossing fixture is valid"), Plan.HasErrors());
	TestEqual(
		TEXT("Fenwick inversion counting matches the exact pairwise result"),
		Plan.Statistics.InitialExecutionCrossings,
		int64{ 1 }
	);
	TestTrue(
		TEXT("alignment and positioning never worsen the accepted crossing objective"),
		Plan.Statistics.FinalExecutionCrossings <= Plan.Statistics.InitialExecutionCrossings
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutDenseOrderingBudgetTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Crossings.DenseGraphUsesDeterministicWorkBudget",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutDenseOrderingBudgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	constexpr int32 NodesPerRank = 12;
	for (int32 Index = 0; Index < NodesPerRank; ++Index)
	{
		Snapshot.Nodes.Add(MakeExecutionNode(*FString::Printf(TEXT("S%02d"), Index)));
		Snapshot.Nodes.Add(MakeExecutionNode(*FString::Printf(TEXT("T%02d"), Index)));
	}
	for (int32 SourceIndex = 0; SourceIndex < NodesPerRank; ++SourceIndex)
	{
		for (int32 TargetIndex = 0; TargetIndex < NodesPerRank; ++TargetIndex)
		{
			const FString Source = FString::Printf(TEXT("S%02d"), SourceIndex);
			const FString Target = FString::Printf(TEXT("T%02d"), TargetIndex);
			const FString Edge = FString::Printf(TEXT("E%02d-%02d"), SourceIndex, TargetIndex);
			Snapshot.ExecutionEdges.Add(
				MakeExecutionEdge(*Edge, *Source, TEXT("ExecOut"), *Target, TEXT("ExecIn"), INDEX_NONE, false)
			);
		}
	}

	FLayoutSettings Settings;
	Settings.OrderingSweeps = 2;
	Settings.AdjacentSwapPasses = 8;
	Settings.AdjacentSwapEvaluationBudget = 3;
	const FLayoutPlan CanonicalPlan = BuildLayout(Snapshot, Settings);
	FGraphSnapshot ReversedSnapshot = Snapshot;
	ReverseSnapshotInsertion(ReversedSnapshot);
	const FLayoutPlan ReversedPlan = BuildLayout(ReversedSnapshot, Settings);
	TestFalse(TEXT("the dense fixture is valid"), CanonicalPlan.HasErrors());
	TestTrue(
		TEXT("dense ordering reports the deterministic interactive work cap"),
		CanonicalPlan.Diagnostics.ContainsByPredicate(
			[](const FLayoutDiagnostic& Diagnostic)
			{ return Diagnostic.Code == EDiagnosticCode::OrderingBudgetExhausted; }
		)
	);
	TestTrue(
		TEXT("the dense final order is crossing-monotonic"),
		CanonicalPlan.Statistics.FinalExecutionCrossings <= CanonicalPlan.Statistics.InitialExecutionCrossings
	);
	PlansMatch(*this, TEXT("dense reversed insertion"), CanonicalPlan, ReversedPlan);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutLinearExecutionTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Execution.LinearChainAlignment",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutLinearExecutionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("A"), 20.0, 52.0));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("B"), 20.0, 56.0));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("C"), 24.0, 40.0));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("A-B"), TEXT("A"), TEXT("ExecOut"), TEXT("B"), TEXT("ExecIn")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("B-C"), TEXT("B"), TEXT("ExecOut"), TEXT("C"), TEXT("ExecIn")));

	const FLayoutPlan Plan = BuildLayout(Snapshot);
	TestFalse(TEXT("linear graph is valid"), Plan.HasErrors());
	const FPlannedNodePosition* A = FindPlannedNode(Plan, TEXT("A"));
	const FPlannedNodePosition* B = FindPlannedNode(Plan, TEXT("B"));
	const FPlannedNodePosition* C = FindPlannedNode(Plan, TEXT("C"));
	TestNotNull(TEXT("A was planned"), A);
	TestNotNull(TEXT("B was planned"), B);
	TestNotNull(TEXT("C was planned"), C);
	if (A == nullptr || B == nullptr || C == nullptr) { return false; }

	TestEqual(TEXT("A is rank zero"), A->ExecutionRank, 0);
	TestEqual(TEXT("B is rank one"), B->ExecutionRank, 1);
	TestEqual(TEXT("C is rank two"), C->ExecutionRank, 2);
	TestTrue(TEXT("execution ranks progress left-to-right from A to B"), A->Position.X < B->Position.X);
	TestTrue(TEXT("execution ranks progress left-to-right from B to C"), B->Position.X < C->Position.X);
	TestTrue(
		TEXT("A output aligns exactly with B input"),
		FMath::IsNearlyEqual(A->Position.Y + 52.0, B->Position.Y + 20.0, PositionTolerance)
	);
	TestTrue(
		TEXT("B output aligns exactly with C input"),
		FMath::IsNearlyEqual(B->Position.Y + 56.0, C->Position.Y + 24.0, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutSemanticBranchOrderTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Execution.SemanticBranchOrder",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutSemanticBranchOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Root = MakeNode(TEXT("Root"), false, FVector2D{ 144.0, 96.0 });
	Root.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 30.0));
	Root.Ports.Add(MakePort(TEXT("First"), EPortDirection::Output, EPortKind::Execution, 38.0, 0, true));
	Root.Ports.Add(MakePort(TEXT("Second"), EPortDirection::Output, EPortKind::Execution, 68.0, 1));
	Snapshot.Nodes.Add(MoveTemp(Root));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("FirstBranch"), 30.0, 42.0));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("SecondBranch"), 30.0, 42.0));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Root-First"), TEXT("Root"), TEXT("First"), TEXT("FirstBranch"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Root-Second"), TEXT("Root"), TEXT("Second"), TEXT("SecondBranch"), TEXT("ExecIn"), 1)
	);

	FLayoutSettings Settings;
	Settings.VerticalSpacing = 96.0;
	Settings.BranchSpacing = 80.0;
	Settings.CollisionClearance = 32.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("branch graph is valid"), Plan.HasErrors());
	const FPlannedNodePosition* First = FindPlannedNode(Plan, TEXT("FirstBranch"));
	const FPlannedNodePosition* Second = FindPlannedNode(Plan, TEXT("SecondBranch"));
	const FNodeSnapshot* FirstSnapshot = FindSnapshotNode(Snapshot, TEXT("FirstBranch"));
	const FNodeSnapshot* SecondSnapshot = FindSnapshotNode(Snapshot, TEXT("SecondBranch"));
	if (First == nullptr || Second == nullptr || FirstSnapshot == nullptr || SecondSnapshot == nullptr)
	{
		AddError(TEXT("branch result omitted a required node"));
		return false;
	}

	TestEqual(TEXT("branches share an execution rank"), First->ExecutionRank, Second->ExecutionRank);
	TestTrue(TEXT("semantic branch zero precedes branch one"), First->OrderInRank < Second->OrderInRank);
	TestTrue(TEXT("semantic branch zero is visually above branch one"), First->Position.Y < Second->Position.Y);
	TestFalse(
		TEXT("branch nodes do not overlap"), Overlaps(MakeRect(*First, *FirstSnapshot), MakeRect(*Second, *SecondSnapshot))
	);
	TestTrue(
		TEXT("branch nodes retain ordinary plus branch-specific vertical spacing"),
		First->Position.Y + FirstSnapshot->Size.Y + Settings.VerticalSpacing + Settings.BranchSpacing
			<= Second->Position.Y + PositionTolerance
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutScopedBranchSpacingTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Execution.ScopedBranchSpacing",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutScopedBranchSpacingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot BranchRoot = MakeNode(TEXT("BranchRoot"), false, FVector2D{ 100.0, 60.0 });
	BranchRoot.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 20.0));
	BranchRoot.Ports.Add(MakePort(TEXT("First"), EPortDirection::Output, EPortKind::Execution, 20.0, 0, true));
	BranchRoot.Ports.Add(MakePort(TEXT("Second"), EPortDirection::Output, EPortKind::Execution, 40.0, 1));
	Snapshot.Nodes.Add(MoveTemp(BranchRoot));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("BranchFirst"), 20.0, 20.0, FVector2D{ 100.0, 60.0 }));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("BranchSecond"), 20.0, 20.0, FVector2D{ 100.0, 60.0 }));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Branch-First"), TEXT("BranchRoot"), TEXT("First"), TEXT("BranchFirst"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Branch-Second"), TEXT("BranchRoot"), TEXT("Second"), TEXT("BranchSecond"), TEXT("ExecIn"), 1
	));

	FNodeSnapshot OrdinarySourceA = MakeExecutionNode(TEXT("OrdinarySourceA"), 20.0, 20.0, FVector2D{ 100.0, 60.0 });
	OrdinarySourceA.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 44.0));
	Snapshot.Nodes.Add(MoveTemp(OrdinarySourceA));
	FNodeSnapshot OrdinarySourceB = MakeExecutionNode(TEXT("OrdinarySourceB"), 20.0, 20.0, FVector2D{ 100.0, 60.0 });
	OrdinarySourceB.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 44.0));
	Snapshot.Nodes.Add(MoveTemp(OrdinarySourceB));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("OrdinaryTargetA"), 20.0, 20.0, FVector2D{ 100.0, 60.0 }));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("OrdinaryTargetB"), 20.0, 20.0, FVector2D{ 100.0, 60.0 }));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Ordinary-A"), TEXT("OrdinarySourceA"), TEXT("ExecOut"), TEXT("OrdinaryTargetA"), TEXT("ExecIn")
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Ordinary-B"), TEXT("OrdinarySourceB"), TEXT("ExecOut"), TEXT("OrdinaryTargetB"), TEXT("ExecIn")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Ordinary-Component-Link"), TEXT("OrdinarySourceA"), TEXT("ValueOut"), TEXT("OrdinarySourceB"), TEXT("ValueIn")
	));

	FLayoutSettings Settings;
	Settings.GridPolicy = EGridPolicy::None;
	Settings.VerticalSpacing = 30.0;
	Settings.BranchSpacing = 70.0;
	Settings.CollisionClearance = 0.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("scoped branch-spacing graph is valid"), Plan.HasErrors());
	const FPlannedNodePosition* BranchFirst = FindPlannedNode(Plan, TEXT("BranchFirst"));
	const FPlannedNodePosition* BranchSecond = FindPlannedNode(Plan, TEXT("BranchSecond"));
	const FPlannedNodePosition* OrdinaryTargetA = FindPlannedNode(Plan, TEXT("OrdinaryTargetA"));
	const FPlannedNodePosition* OrdinaryTargetB = FindPlannedNode(Plan, TEXT("OrdinaryTargetB"));
	if (BranchFirst == nullptr || BranchSecond == nullptr || OrdinaryTargetA == nullptr || OrdinaryTargetB == nullptr)
	{
		AddError(TEXT("scoped branch-spacing result omitted a required node"));
		return false;
	}

	const double BranchGap = BranchSecond->Position.Y - (BranchFirst->Position.Y + 60.0);
	const double OrdinaryGap = OrdinaryTargetB->Position.Y - (OrdinaryTargetA->Position.Y + 60.0);
	TestTrue(
		TEXT("sibling branches receive ordinary plus branch-specific spacing"),
		FMath::IsNearlyEqual(BranchGap, Settings.VerticalSpacing + Settings.BranchSpacing, PositionTolerance)
	);
	TestTrue(
		TEXT("unrelated same-rank nodes receive only ordinary vertical spacing"),
		FMath::IsNearlyEqual(OrdinaryGap, Settings.VerticalSpacing, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutExecutionCycleTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Execution.CycleCondensation",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutExecutionCycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("CycleA")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("CycleB")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("CycleC")));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("CycleA-B"), TEXT("CycleA"), TEXT("ExecOut"), TEXT("CycleB"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("CycleB-C"), TEXT("CycleB"), TEXT("ExecOut"), TEXT("CycleC"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("CycleC-A"), TEXT("CycleC"), TEXT("ExecOut"), TEXT("CycleA"), TEXT("ExecIn"))
	);

	const FLayoutPlan Plan = BuildLayout(Snapshot);
	TestFalse(TEXT("a valid cycle is not an error"), Plan.HasErrors());
	TestEqual(TEXT("all cycle nodes were returned"), Plan.Nodes.Num(), 3);
	TestEqual(TEXT("one execution SCC was condensed"), Plan.Statistics.CondensedExecutionCycleCount, 1);
	TestTrue(
		TEXT("cycle condensation is diagnosed"),
		Plan.Diagnostics.ContainsByPredicate([](const FLayoutDiagnostic& Diagnostic)
											 { return Diagnostic.Code == EDiagnosticCode::ExecutionCycleCondensed; })
	);
	for (const FPlannedNodePosition& Node : Plan.Nodes)
	{
		TestTrue(*FString::Printf(TEXT("cycle node '%s' is finite"), *Node.Node.Value), IsFinite(Node.Position));
		TestEqual(*FString::Printf(TEXT("cycle node '%s' shares SCC rank"), *Node.Node.Value), Node.ExecutionRank, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPureProviderGroupingTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Data.PureProviderGrouping",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPureProviderGroupingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Consumer = MakeExecutionNode(TEXT("Consumer"), 24.0, 24.0, FVector2D{ 160.0, 112.0 });
	Consumer.Ports.Add(MakePort(TEXT("Value0"), EPortDirection::Input, EPortKind::Data, 24.0, 0));
	Consumer.Ports.Add(MakePort(TEXT("Value1"), EPortDirection::Input, EPortKind::Data, 72.0, 1));
	Snapshot.Nodes.Add(MoveTemp(Consumer));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("Provider0"), 24.0));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("Provider1"), 24.0));
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Provider0-Value0"), TEXT("Provider0"), TEXT("ValueOut"), TEXT("Consumer"), TEXT("Value0"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Provider1-Value1"), TEXT("Provider1"), TEXT("ValueOut"), TEXT("Consumer"), TEXT("Value1"))
	);

	FLayoutSettings Settings;
	Settings.PureNodeHorizontalSpacing = 96.0;
	Settings.PureNodeVerticalSpacing = 48.0;
	Settings.CollisionClearance = 32.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("pure-provider graph is valid"), Plan.HasErrors());
	const FPlannedNodePosition* Provider0 = FindPlannedNode(Plan, TEXT("Provider0"));
	const FPlannedNodePosition* Provider1 = FindPlannedNode(Plan, TEXT("Provider1"));
	const FPlannedNodePosition* ConsumerPlan = FindPlannedNode(Plan, TEXT("Consumer"));
	const FNodeSnapshot* Provider0Snapshot = FindSnapshotNode(Snapshot, TEXT("Provider0"));
	const FNodeSnapshot* Provider1Snapshot = FindSnapshotNode(Snapshot, TEXT("Provider1"));
	if (Provider0 == nullptr || Provider1 == nullptr || ConsumerPlan == nullptr || Provider0Snapshot == nullptr
		|| Provider1Snapshot == nullptr)
	{
		AddError(TEXT("pure-provider result omitted a required node"));
		return false;
	}

	TestTrue(TEXT("first provider is left of its consumer"), Provider0->Position.X < ConsumerPlan->Position.X);
	TestTrue(TEXT("second provider is left of its consumer"), Provider1->Position.X < ConsumerPlan->Position.X);
	TestTrue(
		TEXT("providers form one readable input column"),
		FMath::IsNearlyEqual(Provider0->Position.X, Provider1->Position.X)
	);
	TestTrue(TEXT("provider order follows consumer input order"), Provider0->Position.Y < Provider1->Position.Y);
	TestFalse(
		TEXT("provider nodes do not overlap"),
		Overlaps(MakeRect(*Provider0, *Provider0Snapshot), MakeRect(*Provider1, *Provider1Snapshot))
	);
	TestTrue(
		TEXT("providers retain the configured pure-node vertical spacing"),
		Provider0->Position.Y + Provider0Snapshot->Size.Y + Settings.PureNodeVerticalSpacing
			<= Provider1->Position.Y + PositionTolerance
	);
	TestEqual(TEXT("providers share the consumer component"), Provider0->ComponentIndex, ConsumerPlan->ComponentIndex);
	TestEqual(TEXT("both providers share the consumer component"), Provider1->ComponentIndex, ConsumerPlan->ComponentIndex);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPureFallbackCollisionTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Data.PureFallbackAvoidsPlacedNode",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPureFallbackCollisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Impure = MakeExecutionNode(TEXT("Impure"), 24.0, 48.0, FVector2D{ 176.0, 112.0 });
	Impure.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 80.0));
	Snapshot.Nodes.Add(MoveTemp(Impure));

	FNodeSnapshot CycleA = MakeNode(TEXT("CycleA"), true, FVector2D{ 144.0, 96.0 });
	CycleA.Ports.Add(MakePort(TEXT("ExternalIn"), EPortDirection::Input, EPortKind::Data, 28.0));
	CycleA.Ports.Add(MakePort(TEXT("CycleIn"), EPortDirection::Input, EPortKind::Data, 58.0));
	CycleA.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 72.0));
	Snapshot.Nodes.Add(MoveTemp(CycleA));

	FNodeSnapshot CycleB = MakeNode(TEXT("CycleB"), true, FVector2D{ 128.0, 88.0 });
	CycleB.Ports.Add(MakePort(TEXT("CycleIn"), EPortDirection::Input, EPortKind::Data, 32.0));
	CycleB.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 64.0));
	Snapshot.Nodes.Add(MoveTemp(CycleB));

	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Impure-CycleA"), TEXT("Impure"), TEXT("ValueOut"), TEXT("CycleA"), TEXT("ExternalIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("CycleA-CycleB"), TEXT("CycleA"), TEXT("ValueOut"), TEXT("CycleB"), TEXT("CycleIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("CycleB-CycleA"), TEXT("CycleB"), TEXT("ValueOut"), TEXT("CycleA"), TEXT("CycleIn"))
	);

	FLayoutSettings Settings;
	Settings.CollisionClearance = 32.0;
	Settings.PureNodeVerticalSpacing = 64.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("fallback graph is valid"), Plan.HasErrors());
	TestEqual(TEXT("fallback graph retains every node"), Plan.Nodes.Num(), Snapshot.Nodes.Num());
	TestTrue(
		TEXT("cycle fallback is diagnosed"),
		Plan.Diagnostics.ContainsByPredicate([](const FLayoutDiagnostic& Diagnostic)
											 { return Diagnostic.Code == EDiagnosticCode::PureDataCycleFallback; })
	);
	TestAllNodePairsDoNotOverlap(*this, TEXT("pure-cycle fallback"), Snapshot, Plan);

	FGraphSnapshot ReversedSnapshot = Snapshot;
	ReverseSnapshotInsertion(ReversedSnapshot);
	const FLayoutPlan ReversedPlan = BuildLayout(ReversedSnapshot, Settings);
	PlansMatch(*this, TEXT("pure-cycle fallback reversed insertion"), Plan, ReversedPlan);
	return true;
}

FGraphSnapshot MakeNoOverlapStressGraph()
{
	FGraphSnapshot Snapshot;
	FNodeSnapshot Entry = MakeNode(TEXT("Entry"), false, FVector2D{ 176.0, 128.0 });
	Entry.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 30.0));
	Entry.Ports.Add(MakePort(TEXT("Then0"), EPortDirection::Output, EPortKind::Execution, 36.0, 0, true));
	Entry.Ports.Add(MakePort(TEXT("Then1"), EPortDirection::Output, EPortKind::Execution, 72.0, 1));
	Entry.Ports.Add(MakePort(TEXT("CycleSeed"), EPortDirection::Output, EPortKind::Data, 104.0));
	Snapshot.Nodes.Add(MoveTemp(Entry));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("BranchA"), 28.0, 48.0, FVector2D{ 152.0, 88.0 }));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("BranchB"), 28.0, 48.0, FVector2D{ 136.0, 104.0 }));

	FNodeSnapshot Merge = MakeExecutionNode(TEXT("Merge"), 34.0, 42.0, FVector2D{ 184.0, 128.0 });
	Merge.Ports.Add(MakePort(TEXT("Value0"), EPortDirection::Input, EPortKind::Data, 70.0, 0));
	Merge.Ports.Add(MakePort(TEXT("Value1"), EPortDirection::Input, EPortKind::Data, 102.0, 1));
	Snapshot.Nodes.Add(MoveTemp(Merge));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("Exit"), 30.0, 44.0, FVector2D{ 144.0, 80.0 }));

	FNodeSnapshot Provider0 = MakePureProvider(TEXT("Provider0"), 24.0, FVector2D{ 112.0, 72.0 });
	Provider0.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 48.0));
	Snapshot.Nodes.Add(MoveTemp(Provider0));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("Provider1"), 32.0, FVector2D{ 104.0, 80.0 }));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("NestedProvider"), 24.0, FVector2D{ 96.0, 64.0 }));

	FNodeSnapshot CycleA = MakeNode(TEXT("CycleA"), true, FVector2D{ 144.0, 96.0 });
	CycleA.Ports.Add(MakePort(TEXT("SeedIn"), EPortDirection::Input, EPortKind::Data, 28.0));
	CycleA.Ports.Add(MakePort(TEXT("CycleIn"), EPortDirection::Input, EPortKind::Data, 58.0));
	CycleA.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 72.0));
	Snapshot.Nodes.Add(MoveTemp(CycleA));
	FNodeSnapshot CycleB = MakeNode(TEXT("CycleB"), true, FVector2D{ 128.0, 88.0 });
	CycleB.Ports.Add(MakePort(TEXT("CycleIn"), EPortDirection::Input, EPortKind::Data, 32.0));
	CycleB.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 64.0));
	Snapshot.Nodes.Add(MoveTemp(CycleB));

	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Entry-BranchA"), TEXT("Entry"), TEXT("Then0"), TEXT("BranchA"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Entry-BranchB"), TEXT("Entry"), TEXT("Then1"), TEXT("BranchB"), TEXT("ExecIn"), 1)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("BranchA-Merge"), TEXT("BranchA"), TEXT("ExecOut"), TEXT("Merge"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("BranchB-Merge"), TEXT("BranchB"), TEXT("ExecOut"), TEXT("Merge"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Merge-Exit"), TEXT("Merge"), TEXT("ExecOut"), TEXT("Exit"), TEXT("ExecIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Provider0-Merge"), TEXT("Provider0"), TEXT("ValueOut"), TEXT("Merge"), TEXT("Value0"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Provider1-Merge"), TEXT("Provider1"), TEXT("ValueOut"), TEXT("Merge"), TEXT("Value1"))
	);
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Nested-Provider0"), TEXT("NestedProvider"), TEXT("ValueOut"), TEXT("Provider0"), TEXT("ValueIn")
	));
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Entry-CycleA"), TEXT("Entry"), TEXT("CycleSeed"), TEXT("CycleA"), TEXT("SeedIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("CycleA-CycleB"), TEXT("CycleA"), TEXT("ValueOut"), TEXT("CycleB"), TEXT("CycleIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("CycleB-CycleA"), TEXT("CycleB"), TEXT("ValueOut"), TEXT("CycleA"), TEXT("CycleIn"))
	);

	FNodeSnapshot Detached1 = MakeExecutionNode(TEXT("Detached1"), 24.0, 48.0, FVector2D{ 152.0, 96.0 });
	Detached1.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 72.0));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("Detached0"), 24.0, 48.0, FVector2D{ 136.0, 80.0 }));
	Snapshot.Nodes.Add(MoveTemp(Detached1));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("DetachedProvider"), 24.0, FVector2D{ 104.0, 72.0 }));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Detached0-Detached1"), TEXT("Detached0"), TEXT("ExecOut"), TEXT("Detached1"), TEXT("ExecIn")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("DetachedProvider-Detached1"), TEXT("DetachedProvider"), TEXT("ValueOut"), TEXT("Detached1"), TEXT("ValueIn")
	));

	FNodeSnapshot PureSink = MakeNode(TEXT("PureSink"), true, FVector2D{ 136.0, 88.0 });
	PureSink.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 44.0));
	Snapshot.Nodes.Add(MoveTemp(PureSink));
	Snapshot.Nodes.Add(MakePureProvider(TEXT("PureSource"), 24.0, FVector2D{ 112.0, 72.0 }));
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("PureSource-PureSink"), TEXT("PureSource"), TEXT("ValueOut"), TEXT("PureSink"), TEXT("ValueIn"))
	);
	return Snapshot;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutAllPairsNoOverlapTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Invariants.AllPairsNoOverlap",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutAllPairsNoOverlapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot Snapshot = MakeNoOverlapStressGraph();
	FLayoutSettings Settings;
	Settings.VerticalSpacing = 112.0;
	Settings.PureNodeHorizontalSpacing = 96.0;
	Settings.PureNodeVerticalSpacing = 64.0;
	Settings.CollisionClearance = 40.0;
	Settings.ComponentSpacing = FVector2D{ 384.0, 256.0 };
	Settings.ComponentRowWidth = 1536.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("stress graph is valid"), Plan.HasErrors());
	TestEqual(TEXT("stress graph retains every node"), Plan.Nodes.Num(), Snapshot.Nodes.Num());
	TestAllNodePairsDoNotOverlap(*this, TEXT("canonical stress graph"), Snapshot, Plan);

	for (uint32 Seed = 1; Seed <= 4; ++Seed)
	{
		FGraphSnapshot ShuffledSnapshot = Snapshot;
		ShuffleSnapshotInsertion(ShuffledSnapshot, Seed * 0x85ebca6bU);
		const FLayoutPlan ShuffledPlan = BuildLayout(ShuffledSnapshot, Settings);
		const FString Context = FString::Printf(TEXT("stress shuffle seed %u"), Seed);
		PlansMatch(*this, Context, Plan, ShuffledPlan);
		TestAllNodePairsDoNotOverlap(*this, Context, ShuffledSnapshot, ShuffledPlan);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutDisconnectedComponentPackingTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Components.DisconnectedPacking",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutDisconnectedComponentPackingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("A0")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("A1")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("B0")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("B1")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("A0-A1"), TEXT("A0"), TEXT("ExecOut"), TEXT("A1"), TEXT("ExecIn")));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(TEXT("B0-B1"), TEXT("B0"), TEXT("ExecOut"), TEXT("B1"), TEXT("ExecIn")));

	FLayoutSettings Settings;
	Settings.ComponentRowWidth = 16384.0;
	Settings.ComponentSpacing = FVector2D{ 320.0, 224.0 };
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	TestEqual(TEXT("two components were found"), Plan.Statistics.ComponentCount, 2);
	const FPlannedNodePosition* A0 = FindPlannedNode(Plan, TEXT("A0"));
	const FPlannedNodePosition* A1 = FindPlannedNode(Plan, TEXT("A1"));
	const FPlannedNodePosition* B0 = FindPlannedNode(Plan, TEXT("B0"));
	const FPlannedNodePosition* B1 = FindPlannedNode(Plan, TEXT("B1"));
	if (A0 == nullptr || A1 == nullptr || B0 == nullptr || B1 == nullptr)
	{
		AddError(TEXT("component packing result omitted a required node"));
		return false;
	}

	TestEqual(TEXT("A nodes share a component"), A0->ComponentIndex, A1->ComponentIndex);
	TestEqual(TEXT("B nodes share a component"), B0->ComponentIndex, B1->ComponentIndex);
	TestNotEqual(TEXT("disconnected chains have distinct components"), A0->ComponentIndex, B0->ComponentIndex);
	const double ARight = FMath::Max(A0->Position.X, A1->Position.X) + 128.0;
	const double BLeft = FMath::Min(B0->Position.X, B1->Position.X);
	TestTrue(
		TEXT("components are packed without overlap and with configured spacing"),
		ARight + Settings.ComponentSpacing.X <= BLeft + PositionTolerance
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutHybridGridTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Grid.HybridExecution",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutHybridGridTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("GridA"), 13.0, 37.0, FVector2D{ 123.0, 80.0 }));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("GridB"), 13.0, 37.0, FVector2D{ 123.0, 80.0 }));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("GridA-B"), TEXT("GridA"), TEXT("ExecOut"), TEXT("GridB"), TEXT("ExecIn"), INDEX_NONE, true)
	);

	FLayoutSettings Settings;
	Settings.HorizontalSpacing = 150.0;
	Settings.GridPolicy = EGridPolicy::HybridExecution;
	Settings.GridSize = FVector2D{ 16.0, 16.0 };
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* A = FindPlannedNode(Plan, TEXT("GridA"));
	const FPlannedNodePosition* B = FindPlannedNode(Plan, TEXT("GridB"));
	if (A == nullptr || B == nullptr)
	{
		AddError(TEXT("hybrid-grid result omitted a required node"));
		return false;
	}

	TestTrue(TEXT("first rank column is grid snapped"), IsOnGrid(A->Position.X, Settings.GridSize.X));
	TestTrue(TEXT("second rank column is grid snapped"), IsOnGrid(B->Position.X, Settings.GridSize.X));
	TestTrue(TEXT("alignment block anchor is grid snapped"), IsOnGrid(A->Position.Y, Settings.GridSize.Y));
	TestTrue(
		TEXT("horizontal clearance is respected before snapping"),
		B->Position.X + PositionTolerance >= A->Position.X + 123.0 + Settings.HorizontalSpacing
	);
	TestTrue(
		TEXT("hybrid mode preserves exact execution-pin alignment"),
		FMath::IsNearlyEqual(A->Position.Y + 37.0, B->Position.Y + 13.0, PositionTolerance)
	);
	TestFalse(
		TEXT("hybrid mode may keep a node off-grid to preserve its pin offset"),
		IsOnGrid(B->Position.Y, Settings.GridSize.Y)
	);
	return true;
}

FLayoutSettings MakePreserveAuthoredSettings()
{
	FLayoutSettings Settings;
	Settings.LayoutMode = ELayoutMode::PreserveAuthored;
	Settings.LayoutCellSize = 50.0;
	Settings.GridPolicy = EGridPolicy::HybridExecution;
	Settings.GridSize = FVector2D{ 16.0, 16.0 };
	return Settings;
}

FGraphSnapshot MakePreservedEventIslandsGraph(const bool bAddCrossIslandDataEdge)
{
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("ZUpperRoot")), FVector2D{ 124.0, 113.0 }));
	FNodeSnapshot UpperTail = WithOriginalPosition(MakeExecutionNode(TEXT("ZUpperTail")), FVector2D{ 405.0, 117.0 });
	UpperTail.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 60.0));
	Snapshot.Nodes.Add(MoveTemp(UpperTail));

	FNodeSnapshot LowerRoot = WithOriginalPosition(MakeExecutionNode(TEXT("ALowerRoot")), FVector2D{ 126.0, 691.0 });
	LowerRoot.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 60.0));
	Snapshot.Nodes.Add(MoveTemp(LowerRoot));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("ALowerTail")), FVector2D{ 410.0, 695.0 }));

	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Upper.Execution"), TEXT("ZUpperRoot"), TEXT("ExecOut"), TEXT("ZUpperTail"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Lower.Execution"), TEXT("ALowerRoot"), TEXT("ExecOut"), TEXT("ALowerTail"), TEXT("ExecIn"))
	);
	if (bAddCrossIslandDataEdge)
	{
		Snapshot.DataEdges.Add(MakeDataEdge(
			TEXT("CrossIsland.Data"), TEXT("ZUpperTail"), TEXT("ValueOut"), TEXT("ALowerRoot"), TEXT("ValueIn")
		));
	}
	return Snapshot;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedEventIslandAnchorsTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.EventIslandAnchorsAndVerticalOrder",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedEventIslandAnchorsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot Snapshot = MakePreservedEventIslandsGraph(false);
	const FLayoutSettings Settings = MakePreserveAuthoredSettings();
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* UpperRoot = FindPlannedNode(Plan, TEXT("ZUpperRoot"));
	const FPlannedNodePosition* UpperTail = FindPlannedNode(Plan, TEXT("ZUpperTail"));
	const FPlannedNodePosition* LowerRoot = FindPlannedNode(Plan, TEXT("ALowerRoot"));
	const FPlannedNodePosition* LowerTail = FindPlannedNode(Plan, TEXT("ALowerTail"));
	if (UpperRoot == nullptr || UpperTail == nullptr || LowerRoot == nullptr || LowerTail == nullptr)
	{
		AddError(TEXT("preserved event-island plan omitted a required node"));
		return false;
	}

	TestFalse(TEXT("the preserved event-island graph is valid"), Plan.HasErrors());
	TestEqual(TEXT("the two event islands remain distinct components"), Plan.Statistics.ComponentCount, 2);
	TestTrue(
		TEXT("the upper event root retains its authored cell anchor"),
		NearlyEqual(UpperRoot->Position, FVector2D{ 150.0, 100.0 })
	);
	TestTrue(
		TEXT("the lower event root retains its authored cell anchor"),
		NearlyEqual(LowerRoot->Position, FVector2D{ 150.0, 700.0 })
	);
	TestTrue(
		TEXT("reverse-lexical keys do not invert authored island order"), UpperRoot->Position.Y < LowerRoot->Position.Y
	);
	TestTrue(
		TEXT("event islands sharing an authored start column are not shelf-packed"),
		FMath::IsNearlyEqual(UpperRoot->Position.X, LowerRoot->Position.X)
	);
	TestEqual(TEXT("upper execution nodes share an island"), UpperRoot->ComponentIndex, UpperTail->ComponentIndex);
	TestEqual(TEXT("lower execution nodes share an island"), LowerRoot->ComponentIndex, LowerTail->ComponentIndex);
	TestNotEqual(TEXT("the authored islands keep distinct identities"), UpperRoot->ComponentIndex, LowerRoot->ComponentIndex);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedStartColumnScopeTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.ExecutionRootsShareScopeWideStartColumn",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedStartColumnScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("FirstEventRoot")), FVector2D{ 100.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("SecondEventRoot")), FVector2D{ 150.0, 500.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("FirstDataIsland"), 24.0, FVector2D{ 100.0, 60.0 }), FVector2D{ 124.0, 900.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("SecondDataIsland"), 24.0, FVector2D{ 100.0, 60.0 }), FVector2D{ 126.0, 1300.0 }
	));

	const FLayoutPlan Plan = BuildLayout(Snapshot, MakePreserveAuthoredSettings());
	const FPlannedNodePosition* FirstEventRoot = FindPlannedNode(Plan, TEXT("FirstEventRoot"));
	const FPlannedNodePosition* SecondEventRoot = FindPlannedNode(Plan, TEXT("SecondEventRoot"));
	const FPlannedNodePosition* FirstDataIsland = FindPlannedNode(Plan, TEXT("FirstDataIsland"));
	const FPlannedNodePosition* SecondDataIsland = FindPlannedNode(Plan, TEXT("SecondDataIsland"));
	if (FirstEventRoot == nullptr || SecondEventRoot == nullptr || FirstDataIsland == nullptr || SecondDataIsland == nullptr)
	{
		AddError(TEXT("start-column scope plan omitted a required node"));
		return false;
	}

	TestFalse(TEXT("the start-column scope graph is valid"), Plan.HasErrors());
	TestTrue(
		TEXT("all execution roots share the scope-wide median major-grid column"),
		FMath::IsNearlyEqual(FirstEventRoot->Position.X, 150.0, PositionTolerance)
			&& FMath::IsNearlyEqual(SecondEventRoot->Position.X, 150.0, PositionTolerance)
	);
	TestTrue(
		TEXT("nearby data-only islands are not coerced into an event-start column"),
		FMath::IsNearlyEqual(FirstDataIsland->Position.X, 100.0, PositionTolerance)
			&& FMath::IsNearlyEqual(SecondDataIsland->Position.X, 150.0, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedEventIslandGutterTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.EventIslandsHonorConfiguredGutter",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedEventIslandGutterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("UpperRoot")), FVector2D{ 100.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("UpperTail")), FVector2D{ 350.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("LowerRoot")), FVector2D{ 100.0, 150.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("LowerTail")), FVector2D{ 350.0, 150.0 }));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Upper.Execution"), TEXT("UpperRoot"), TEXT("ExecOut"), TEXT("UpperTail"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Lower.Execution"), TEXT("LowerRoot"), TEXT("ExecOut"), TEXT("LowerTail"), TEXT("ExecIn"))
	);

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.ComponentSpacing.Y = 275.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* UpperRoot = FindPlannedNode(Plan, TEXT("UpperRoot"));
	const FPlannedNodePosition* LowerRoot = FindPlannedNode(Plan, TEXT("LowerRoot"));
	if (UpperRoot == nullptr || LowerRoot == nullptr)
	{
		AddError(TEXT("event-island gutter plan omitted a root"));
		return false;
	}

	const double SnappedGutter = 300.0;
	TestFalse(TEXT("the event-island gutter graph is valid"), Plan.HasErrors());
	TestTrue(
		TEXT("the later event island clears the earlier island by the configured snapped gutter"),
		LowerRoot->Position.Y >= UpperRoot->Position.Y + 80.0 + SnappedGutter - PositionTolerance
	);
	TestTrue(TEXT("the shifted event root stays on the coarse cell"), IsOnGrid(LowerRoot->Position.Y, 50.0));
	TestTrue(
		TEXT("vertical packing retains the authored start column"),
		FMath::IsNearlyEqual(UpperRoot->Position.X, LowerRoot->Position.X, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedAuthoredColumnExpansionTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.AuthoredExecutionColumnsAreExpandOnly",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedAuthoredColumnExpansionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("SpaciousRoot"), 24.0, 24.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 125.0, 125.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("ExpandedMiddle"), 24.0, 24.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 325.0, 125.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("SpaciousTail"), 24.0, 24.0, FVector2D{ 140.0, 80.0 }), FVector2D{ 1125.0, 125.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("ClusterPeer"), 24.0, 24.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 126.0, 825.0 }
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Spacious.First"), TEXT("SpaciousRoot"), TEXT("ExecOut"), TEXT("ExpandedMiddle"), TEXT("ExecIn")
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Spacious.Second"), TEXT("ExpandedMiddle"), TEXT("ExecOut"), TEXT("SpaciousTail"), TEXT("ExecIn")
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.HorizontalSpacing = 160.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* Root = FindPlannedNode(Plan, TEXT("SpaciousRoot"));
	const FPlannedNodePosition* Middle = FindPlannedNode(Plan, TEXT("ExpandedMiddle"));
	const FPlannedNodePosition* Tail = FindPlannedNode(Plan, TEXT("SpaciousTail"));
	const FPlannedNodePosition* ClusterPeer = FindPlannedNode(Plan, TEXT("ClusterPeer"));
	if (Root == nullptr || Middle == nullptr || Tail == nullptr || ClusterPeer == nullptr)
	{
		AddError(TEXT("authored-column expansion plan omitted a required node"));
		return false;
	}

	TestFalse(TEXT("the authored-column expansion graph is valid"), Plan.HasErrors());
	TestTrue(
		TEXT("a cramped authored column expands to the required clearance"),
		Middle->Position.X - Root->Position.X + PositionTolerance >= 300.0
	);
	TestTrue(
		TEXT("expanding an earlier rank does not consume the next generous authored gap"),
		FMath::IsNearlyEqual(Tail->Position.X - Middle->Position.X, 800.0, PositionTolerance)
	);
	TestTrue(
		TEXT("nearby event starts share one snapped start column without changing internal gaps"),
		FMath::IsNearlyEqual(Root->Position.X, ClusterPeer->Position.X, PositionTolerance)
	);
	TestTrue(TEXT("the preserved root column uses the coarse cell"), IsOnGrid(Root->Position.X, 50.0));
	TestTrue(TEXT("the expanded middle column uses the coarse cell"), IsOnGrid(Middle->Position.X, 50.0));
	TestTrue(TEXT("the preserved downstream column uses the coarse cell"), IsOnGrid(Tail->Position.X, 50.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedVirtualRankSpacingTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.LongEdgeVirtualRanksRetainAuthoredSpacingEvidence",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedVirtualRankSpacingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("VirtualRoot")), FVector2D{ 100.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("CompactA")), FVector2D{ 250.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("CompactB")), FVector2D{ 400.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(MakeExecutionNode(TEXT("DistantTarget")), FVector2D{ 1900.0, 100.0 }));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Virtual.RootA"), TEXT("VirtualRoot"), TEXT("ExecOut"), TEXT("CompactA"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Virtual.AB"), TEXT("CompactA"), TEXT("ExecOut"), TEXT("CompactB"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Virtual.BTarget"), TEXT("CompactB"), TEXT("ExecOut"), TEXT("DistantTarget"), TEXT("ExecIn")
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Virtual.Long"), TEXT("VirtualRoot"), TEXT("ExecOut"), TEXT("DistantTarget"), TEXT("ExecIn"), 1, false
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.HorizontalSpacing = 160.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* Root = FindPlannedNode(Plan, TEXT("VirtualRoot"));
	const FPlannedEdgeRoute* LongRoute = FindPlannedRoute(Plan, TEXT("Virtual.Long"));
	if (Root == nullptr || LongRoute == nullptr || LongRoute->Waypoints.Num() != 2)
	{
		AddError(TEXT("preserved long-edge plan omitted its root or two virtual-rank waypoints"));
		return false;
	}

	TestFalse(TEXT("the preserved long-edge graph is valid"), Plan.HasErrors());
	TestEqual(TEXT("the long edge contributes two virtual ranks"), Plan.Statistics.VirtualNodeCount, 2);
	TestTrue(
		TEXT("the first virtual rank retains interpolated authored X evidence"),
		FMath::IsNearlyEqual(LongRoute->Waypoints[0].X - Root->Position.X, 400.0, PositionTolerance)
	);
	TestTrue(
		TEXT("the second virtual rank propagates that preserved column spacing"),
		FMath::IsNearlyEqual(LongRoute->Waypoints[1].X - Root->Position.X, 800.0, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutCrossIslandDataBandTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.DataOnlyConnectionDoesNotFlattenEventBands",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutCrossIslandDataBandTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot Snapshot = MakePreservedEventIslandsGraph(true);
	const FLayoutSettings Settings = MakePreserveAuthoredSettings();
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* UpperRoot = FindPlannedNode(Plan, TEXT("ZUpperRoot"));
	const FPlannedNodePosition* LowerRoot = FindPlannedNode(Plan, TEXT("ALowerRoot"));
	if (UpperRoot == nullptr || LowerRoot == nullptr)
	{
		AddError(TEXT("cross-island data plan omitted an event root"));
		return false;
	}

	TestFalse(TEXT("the cross-island data graph is valid"), Plan.HasErrors());
	TestEqual(TEXT("the data-only cross-link is accepted"), Plan.Statistics.AcceptedDataEdgeCount, 1);
	TestEqual(TEXT("data-only connectivity does not merge execution islands"), Plan.Statistics.ComponentCount, 2);
	TestNotEqual(
		TEXT("data-only connectivity preserves distinct components"), UpperRoot->ComponentIndex, LowerRoot->ComponentIndex
	);
	TestTrue(TEXT("the upper authored band remains anchored"), NearlyEqual(UpperRoot->Position, { 150.0, 100.0 }));
	TestTrue(TEXT("the lower authored band remains anchored"), NearlyEqual(LowerRoot->Position, { 150.0, 700.0 }));
	TestTrue(
		TEXT("the cross-island data wire does not flatten the bands"),
		LowerRoot->Position.Y - UpperRoot->Position.Y >= 600.0 - PositionTolerance
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedCoarseColumnsTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.CoarseColumnsUseWidestNodeAndFullCellGap",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedCoarseColumnsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Root = MakeNode(TEXT("ColumnRoot"), false, FVector2D{ 125.0, 100.0 });
	Root.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 24.0));
	Root.Ports.Add(MakePort(TEXT("ToNarrow"), EPortDirection::Output, EPortKind::Execution, 32.0, 0, true));
	Root.Ports.Add(MakePort(TEXT("ToWide"), EPortDirection::Output, EPortKind::Execution, 72.0, 1));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Root), FVector2D{ 100.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("NarrowRankOne"), 24.0, 32.0, FVector2D{ 90.0, 72.0 }), FVector2D{ 350.0, 100.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("WideRankOne"), 24.0, 56.0, FVector2D{ 260.0, 112.0 }), FVector2D{ 350.0, 350.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("ColumnTail"), 24.0, 32.0, FVector2D{ 110.0, 80.0 }), FVector2D{ 800.0, 350.0 }
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Root-Narrow"), TEXT("ColumnRoot"), TEXT("ToNarrow"), TEXT("NarrowRankOne"), TEXT("ExecIn"), 0, false
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Root-Wide"), TEXT("ColumnRoot"), TEXT("ToWide"), TEXT("WideRankOne"), TEXT("ExecIn"), 1, false
	));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("Wide-Tail"), TEXT("WideRankOne"), TEXT("ExecOut"), TEXT("ColumnTail"), TEXT("ExecIn"))
	);

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.HorizontalSpacing = 1.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* RootPlan = FindPlannedNode(Plan, TEXT("ColumnRoot"));
	const FPlannedNodePosition* Narrow = FindPlannedNode(Plan, TEXT("NarrowRankOne"));
	const FPlannedNodePosition* Wide = FindPlannedNode(Plan, TEXT("WideRankOne"));
	const FPlannedNodePosition* Tail = FindPlannedNode(Plan, TEXT("ColumnTail"));
	if (RootPlan == nullptr || Narrow == nullptr || Wide == nullptr || Tail == nullptr)
	{
		AddError(TEXT("coarse-column plan omitted a required node"));
		return false;
	}

	TestFalse(TEXT("the coarse-column graph is valid"), Plan.HasErrors());
	for (const FPlannedNodePosition* Node : { RootPlan, Narrow, Wide, Tail })
	{
		TestTrue(
			*FString::Printf(TEXT("node '%s' X uses the layout cell"), *Node->Node.Value),
			IsOnGrid(Node->Position.X, Settings.LayoutCellSize)
		);
	}
	TestTrue(
		TEXT("same-rank nodes share one coarse column"),
		FMath::IsNearlyEqual(Narrow->Position.X, Wide->Position.X, PositionTolerance)
	);
	TestTrue(
		TEXT("rank one leaves a full cell after the root"),
		Narrow->Position.X + PositionTolerance >= RootPlan->Position.X + 125.0 + Settings.LayoutCellSize
	);
	TestTrue(
		TEXT("rank two clears the widest rank-one node plus a full cell"),
		Tail->Position.X + PositionTolerance >= Wide->Position.X + 260.0 + Settings.LayoutCellSize
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutVisibleMajorGridColumnsTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.VisibleMajorGridColumns",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutVisibleMajorGridColumnsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestEqual(
		TEXT("the former 50-unit request expands to one visible 16-by-8 major square"),
		ResolveMajorGridAlignedCellSize(50.0, 16.0, 8.0),
		128.0
	);
	TestEqual(
		TEXT("an exact visible major-grid request remains unchanged"), ResolveMajorGridAlignedCellSize(128.0, 16.0, 8.0), 128.0
	);

	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("FunctionEntry"), 32.0, 37.0, FVector2D{ 224.0, 80.0 }), FVector2D::ZeroVector
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("FunctionResult"), 13.0, 32.0, FVector2D{ 174.0, 96.0 }), FVector2D{ 256.0, 24.0 }
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Entry-Result"), TEXT("FunctionEntry"), TEXT("ExecOut"), TEXT("FunctionResult"), TEXT("ExecIn")
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.LayoutCellSize = 128.0;
	Settings.HorizontalSpacing = 160.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* Entry = FindPlannedNode(Plan, TEXT("FunctionEntry"));
	const FPlannedNodePosition* Result = FindPlannedNode(Plan, TEXT("FunctionResult"));
	if (Entry == nullptr || Result == nullptr)
	{
		AddError(TEXT("visible-major-grid plan omitted a function boundary node"));
		return false;
	}

	TestFalse(TEXT("the two-node function plan is valid"), Plan.HasErrors());
	TestTrue(TEXT("the function entry starts on a visible major-grid rule"), IsOnGrid(Entry->Position.X, 128.0));
	TestTrue(TEXT("the return node starts on a visible major-grid rule"), IsOnGrid(Result->Position.X, 128.0));
	TestTrue(
		TEXT("the return leaves the configured clearance after the entry"),
		Result->Position.X + PositionTolerance >= Entry->Position.X + 224.0 + Settings.HorizontalSpacing
	);
	TestTrue(
		TEXT("the execution pins remain exactly straight"),
		FMath::IsNearlyEqual(Entry->Position.Y + 37.0, Result->Position.Y + 13.0, PositionTolerance)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedExecutionPinAlignmentTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.ExecutionPinAlignmentBeatsNodeYGrid",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedExecutionPinAlignmentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("AlignSource"), 13.0, 37.0, FVector2D{ 125.0, 80.0 }), FVector2D{ 100.0, 100.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("AlignTarget"), 13.0, 37.0, FVector2D{ 125.0, 80.0 }), FVector2D{ 400.0, 150.0 }
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Align.Execution"), TEXT("AlignSource"), TEXT("ExecOut"), TEXT("AlignTarget"), TEXT("ExecIn")
	));
	const FLayoutSettings Settings = MakePreserveAuthoredSettings();
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* Source = FindPlannedNode(Plan, TEXT("AlignSource"));
	const FPlannedNodePosition* Target = FindPlannedNode(Plan, TEXT("AlignTarget"));
	if (Source == nullptr || Target == nullptr)
	{
		AddError(TEXT("execution-alignment plan omitted a required node"));
		return false;
	}

	TestTrue(
		TEXT("the alignment block anchor remains on the 50-unit layout cell"),
		IsOnGrid(Source->Position.Y, Settings.LayoutCellSize)
	);
	TestTrue(
		TEXT("preferred execution pins align exactly"),
		FMath::IsNearlyEqual(Source->Position.Y + 37.0, Target->Position.Y + 13.0, PositionTolerance)
	);
	TestFalse(
		TEXT("the target top may leave the coarse layout cell for exact pin alignment"),
		IsOnGrid(Target->Position.Y, Settings.LayoutCellSize)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedProviderLocalityTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.ProviderUpperLowerGroupingAndLocality",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedProviderLocalityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Consumer = MakeExecutionNode(TEXT("ProviderConsumer"), 40.0, 40.0, FVector2D{ 160.0, 160.0 });
	Consumer.Ports.Add(MakePort(TEXT("UpperValue"), EPortDirection::Input, EPortKind::Data, 30.0, 0));
	Consumer.Ports.Add(MakePort(TEXT("LowerValue"), EPortDirection::Input, EPortKind::Data, 130.0, 1));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Consumer), FVector2D{ 600.0, 400.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("ZUpperProvider"), 24.0, FVector2D{ 100.0, 60.0 }), FVector2D{ 400.0, 300.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("ALowerProvider"), 24.0, FVector2D{ 100.0, 60.0 }), FVector2D{ 400.0, 650.0 }
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Upper.Provider"), TEXT("ZUpperProvider"), TEXT("ValueOut"), TEXT("ProviderConsumer"), TEXT("UpperValue")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Lower.Provider"), TEXT("ALowerProvider"), TEXT("ValueOut"), TEXT("ProviderConsumer"), TEXT("LowerValue")
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.PureNodeHorizontalSpacing = 90.0;
	Settings.PureNodeVerticalSpacing = 50.0;
	Settings.CollisionClearance = 0.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* ConsumerPlan = FindPlannedNode(Plan, TEXT("ProviderConsumer"));
	const FPlannedNodePosition* Upper = FindPlannedNode(Plan, TEXT("ZUpperProvider"));
	const FPlannedNodePosition* Lower = FindPlannedNode(Plan, TEXT("ALowerProvider"));
	if (ConsumerPlan == nullptr || Upper == nullptr || Lower == nullptr)
	{
		AddError(TEXT("provider-locality plan omitted a required node"));
		return false;
	}

	const double UpperGap = ConsumerPlan->Position.X - (Upper->Position.X + 100.0);
	const double LowerGap = ConsumerPlan->Position.X - (Lower->Position.X + 100.0);
	TestTrue(
		TEXT("the authored upper provider remains above its consumer"),
		Upper->Position.Y + 60.0 <= ConsumerPlan->Position.Y + PositionTolerance
	);
	TestTrue(
		TEXT("the authored lower provider remains below its consumer"),
		Lower->Position.Y + PositionTolerance >= ConsumerPlan->Position.Y + 160.0
	);
	TestTrue(TEXT("semantic input order wins over reverse lexical provider keys"), Upper->Position.Y < Lower->Position.Y);
	TestTrue(TEXT("the upper provider Y uses the 50-unit layout cell"), IsOnGrid(Upper->Position.Y, Settings.LayoutCellSize));
	TestTrue(TEXT("the lower provider Y uses the 50-unit layout cell"), IsOnGrid(Lower->Position.Y, Settings.LayoutCellSize));
	TestTrue(
		TEXT("providers remain grouped in one local input column"),
		FMath::IsNearlyEqual(Upper->Position.X, Lower->Position.X, PositionTolerance)
	);
	TestTrue(TEXT("the upper provider honors configured input-side clearance"), UpperGap + PositionTolerance >= 90.0);
	TestTrue(TEXT("the lower provider honors configured input-side clearance"), LowerGap + PositionTolerance >= 90.0);
	TestTrue(TEXT("the upper provider remains local to its consumer"), UpperGap <= 100.0 + PositionTolerance);
	TestTrue(TEXT("the lower provider remains local to its consumer"), LowerGap <= 100.0 + PositionTolerance);
	TestEqual(TEXT("the upper provider stays in the consumer island"), Upper->ComponentIndex, ConsumerPlan->ComponentIndex);
	TestEqual(TEXT("the lower provider stays in the consumer island"), Lower->ComponentIndex, ConsumerPlan->ComponentIndex);
	TestAllNodePairsDoNotOverlap(*this, TEXT("preserved provider locality"), Snapshot, Plan);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedProviderColumnWidthTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.ProviderColumnUsesWidestNode",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedProviderColumnWidthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Root = MakeNode(TEXT("ProviderColumnRoot"), false, FVector2D{ 120.0, 100.0 });
	Root.Ports.Add(MakePort(TEXT("ExecIn"), EPortDirection::Input, EPortKind::Execution, 24.0));
	Root.Ports.Add(MakePort(TEXT("ToUpper"), EPortDirection::Output, EPortKind::Execution, 32.0, 0, true));
	Root.Ports.Add(MakePort(TEXT("ToLower"), EPortDirection::Output, EPortKind::Execution, 72.0, 1));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Root), FVector2D{ 100.0, 500.0 }));

	FNodeSnapshot UpperConsumer = MakeExecutionNode(TEXT("UpperColumnConsumer"), 24.0, 24.0, FVector2D{ 160.0, 140.0 });
	UpperConsumer.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 70.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(UpperConsumer), FVector2D{ 600.0, 350.0 }));
	FNodeSnapshot LowerConsumer = MakeExecutionNode(TEXT("LowerColumnConsumer"), 24.0, 24.0, FVector2D{ 160.0, 140.0 });
	LowerConsumer.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 70.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(LowerConsumer), FVector2D{ 600.0, 750.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("NarrowColumnProvider"), 24.0, FVector2D{ 100.0, 60.0 }), FVector2D{ 350.0, 250.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("WideColumnProvider"), 24.0, FVector2D{ 200.0, 60.0 }), FVector2D{ 300.0, 850.0 }
	));

	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Root-Upper"), TEXT("ProviderColumnRoot"), TEXT("ToUpper"), TEXT("UpperColumnConsumer"), TEXT("ExecIn"), 0, false
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Root-Lower"), TEXT("ProviderColumnRoot"), TEXT("ToLower"), TEXT("LowerColumnConsumer"), TEXT("ExecIn"), 1, false
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Narrow-Upper"), TEXT("NarrowColumnProvider"), TEXT("ValueOut"), TEXT("UpperColumnConsumer"), TEXT("ValueIn")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Wide-Lower"), TEXT("WideColumnProvider"), TEXT("ValueOut"), TEXT("LowerColumnConsumer"), TEXT("ValueIn")
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.PureNodeHorizontalSpacing = 90.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* Narrow = FindPlannedNode(Plan, TEXT("NarrowColumnProvider"));
	const FPlannedNodePosition* Wide = FindPlannedNode(Plan, TEXT("WideColumnProvider"));
	const FPlannedNodePosition* UpperConsumerPlan = FindPlannedNode(Plan, TEXT("UpperColumnConsumer"));
	if (Narrow == nullptr || Wide == nullptr || UpperConsumerPlan == nullptr)
	{
		AddError(TEXT("provider-column plan omitted a provider"));
		return false;
	}

	TestFalse(TEXT("the provider-column graph is valid"), Plan.HasErrors());
	TestTrue(
		TEXT("same-rank providers share the X column defined by the widest provider"),
		FMath::IsNearlyEqual(Narrow->Position.X, Wide->Position.X, PositionTolerance)
	);
	TestTrue(TEXT("the shared provider column uses the coarse cell"), IsOnGrid(Narrow->Position.X, 50.0));
	TestTrue(
		TEXT("the widest provider leaves the configured clearance before the consumer column"),
		UpperConsumerPlan->Position.X - (Wide->Position.X + 200.0) + PositionTolerance >= 90.0
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPureTreeUpstreamDirectionTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.PureTreeNeverMovesBehindImpureUpstreamSource",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPureTreeUpstreamDirectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Source = MakeExecutionNode(TEXT("ImpureSource"), 20.0, 20.0, FVector2D{ 100.0, 80.0 });
	Source.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 56.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Source), FVector2D{ 100.0, 100.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("ExecSpacerOne"), 20.0, 20.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 250.0, 100.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakeExecutionNode(TEXT("ExecSpacerTwo"), 20.0, 20.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 400.0, 100.0 }
	));
	FNodeSnapshot Consumer = MakeExecutionNode(TEXT("ImpureConsumer"), 20.0, 20.0, FVector2D{ 140.0, 96.0 });
	Consumer.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 64.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Consumer), FVector2D{ 550.0, 100.0 }));

	FNodeSnapshot PureA = MakeNode(TEXT("PureA"), true, FVector2D{ 80.0, 60.0 });
	PureA.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 20.0));
	PureA.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 40.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(PureA), FVector2D{ -200.0, 300.0 }));
	FNodeSnapshot PureB = MakeNode(TEXT("PureB"), true, FVector2D{ 80.0, 60.0 });
	PureB.Ports.Add(MakePort(TEXT("ValueIn"), EPortDirection::Input, EPortKind::Data, 20.0));
	PureB.Ports.Add(MakePort(TEXT("ValueOut"), EPortDirection::Output, EPortKind::Data, 40.0));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(PureB), FVector2D{ -50.0, 400.0 }));

	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("Source-SpacerOne"), TEXT("ImpureSource"), TEXT("ExecOut"), TEXT("ExecSpacerOne"), TEXT("ExecIn")
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("SpacerOne-SpacerTwo"), TEXT("ExecSpacerOne"), TEXT("ExecOut"), TEXT("ExecSpacerTwo"), TEXT("ExecIn")
	));
	Snapshot.ExecutionEdges.Add(MakeExecutionEdge(
		TEXT("SpacerTwo-Consumer"), TEXT("ExecSpacerTwo"), TEXT("ExecOut"), TEXT("ImpureConsumer"), TEXT("ExecIn")
	));
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("Source-PureA"), TEXT("ImpureSource"), TEXT("ValueOut"), TEXT("PureA"), TEXT("ValueIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("PureA-PureB"), TEXT("PureA"), TEXT("ValueOut"), TEXT("PureB"), TEXT("ValueIn"))
	);
	Snapshot.DataEdges.Add(
		MakeDataEdge(TEXT("PureB-Consumer"), TEXT("PureB"), TEXT("ValueOut"), TEXT("ImpureConsumer"), TEXT("ValueIn"))
	);

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.PureNodeHorizontalSpacing = 50.0;
	Settings.CollisionClearance = 0.0;
	const FLayoutPlan Plan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* SourcePlan = FindPlannedNode(Plan, TEXT("ImpureSource"));
	const FPlannedNodePosition* PureAPlan = FindPlannedNode(Plan, TEXT("PureA"));
	const FPlannedNodePosition* PureBPlan = FindPlannedNode(Plan, TEXT("PureB"));
	const FPlannedNodePosition* ConsumerPlan = FindPlannedNode(Plan, TEXT("ImpureConsumer"));
	if (SourcePlan == nullptr || PureAPlan == nullptr || PureBPlan == nullptr || ConsumerPlan == nullptr)
	{
		AddError(TEXT("upstream pure-tree plan omitted a required node"));
		return false;
	}

	TestTrue(
		TEXT("the first pure node moves ahead of its impure source by one full cell"),
		PureAPlan->Position.X + PositionTolerance >= SourcePlan->Position.X + 100.0 + 50.0
	);
	TestTrue(
		TEXT("the second pure node follows the first by one full cell"),
		PureBPlan->Position.X + PositionTolerance >= PureAPlan->Position.X + 80.0 + 50.0
	);
	TestTrue(
		TEXT("the pure tree remains on the input side of its consumer when space permits"),
		PureBPlan->Position.X + 80.0 + 50.0 <= ConsumerPlan->Position.X + PositionTolerance
	);
	TestAllNodePairsDoNotOverlap(*this, TEXT("upstream pure-tree direction"), Snapshot, Plan);
	return true;
}

FGraphSnapshot UsePlanAsAuthoredSnapshot(const FGraphSnapshot& Snapshot, const FLayoutPlan& Plan)
{
	FGraphSnapshot Result = Snapshot;
	for (FNodeSnapshot& Node : Result.Nodes)
	{
		const FPlannedNodePosition* Planned = FindPlannedNode(Plan, *Node.Key.Value);
		if (Planned == nullptr) { continue; }
		Node.OriginalPosition = Planned->Position;
		Node.bHasOriginalPosition = true;
	}
	return Result;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservedProviderStackFixedPointTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.ProviderStackOrderAndFixedPoint",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservedProviderStackFixedPointTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FGraphSnapshot Snapshot;
	FNodeSnapshot Consumer = MakeExecutionNode(TEXT("StackConsumer"), 40.0, 40.0, FVector2D{ 160.0, 150.0 });
	Consumer.Ports.Add(MakePort(TEXT("FirstValue"), EPortDirection::Input, EPortKind::Data, 30.0, 0));
	Consumer.Ports.Add(MakePort(TEXT("SecondValue"), EPortDirection::Input, EPortKind::Data, 70.0, 1));
	Consumer.Ports.Add(MakePort(TEXT("ThirdValue"), EPortDirection::Input, EPortKind::Data, 100.0, 2));
	Consumer.Ports.Add(MakePort(TEXT("FourthValue"), EPortDirection::Input, EPortKind::Data, 125.0, 3));
	Snapshot.Nodes.Add(WithOriginalPosition(MoveTemp(Consumer), FVector2D{ 600.0, 400.0 }));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("FirstProvider"), 24.0, FVector2D{ 100.0, 80.0 }), FVector2D{ 400.0, 250.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("SecondProvider"), 24.0, FVector2D{ 100.0, 120.0 }), FVector2D{ 400.0, 330.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("ThirdProvider"), 24.0, FVector2D{ 100.0, 100.0 }), FVector2D{ 400.0, 580.0 }
	));
	Snapshot.Nodes.Add(WithOriginalPosition(
		MakePureProvider(TEXT("FourthProvider"), 24.0, FVector2D{ 100.0, 150.0 }), FVector2D{ 400.0, 620.0 }
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("First.Provider"), TEXT("FirstProvider"), TEXT("ValueOut"), TEXT("StackConsumer"), TEXT("FirstValue")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Second.Provider"), TEXT("SecondProvider"), TEXT("ValueOut"), TEXT("StackConsumer"), TEXT("SecondValue")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Third.Provider"), TEXT("ThirdProvider"), TEXT("ValueOut"), TEXT("StackConsumer"), TEXT("ThirdValue")
	));
	Snapshot.DataEdges.Add(MakeDataEdge(
		TEXT("Fourth.Provider"), TEXT("FourthProvider"), TEXT("ValueOut"), TEXT("StackConsumer"), TEXT("FourthValue")
	));

	FLayoutSettings Settings = MakePreserveAuthoredSettings();
	Settings.PureNodeHorizontalSpacing = 90.0;
	Settings.PureNodeVerticalSpacing = 50.0;
	Settings.CollisionClearance = 0.0;
	const FLayoutPlan FirstPlan = BuildLayout(Snapshot, Settings);
	const FPlannedNodePosition* FirstProvider = FindPlannedNode(FirstPlan, TEXT("FirstProvider"));
	const FPlannedNodePosition* SecondProvider = FindPlannedNode(FirstPlan, TEXT("SecondProvider"));
	const FPlannedNodePosition* ThirdProvider = FindPlannedNode(FirstPlan, TEXT("ThirdProvider"));
	const FPlannedNodePosition* FourthProvider = FindPlannedNode(FirstPlan, TEXT("FourthProvider"));
	const FPlannedNodePosition* StackConsumer = FindPlannedNode(FirstPlan, TEXT("StackConsumer"));
	if (FirstProvider == nullptr || SecondProvider == nullptr || ThirdProvider == nullptr || FourthProvider == nullptr
		|| StackConsumer == nullptr)
	{
		AddError(TEXT("provider-stack plan omitted a provider"));
		return false;
	}

	TestFalse(TEXT("the provider-stack graph is valid"), FirstPlan.HasErrors());
	TestTrue(
		TEXT("same-side provider order follows consumer input order"),
		FirstProvider->Position.Y < SecondProvider->Position.Y
	);
	TestTrue(
		TEXT("lower provider order follows consumer input order"), ThirdProvider->Position.Y < FourthProvider->Position.Y
	);
	TestTrue(
		TEXT("upper providers stay above the consumer after collision resolution"),
		SecondProvider->Position.Y + 120.0 + 50.0 <= StackConsumer->Position.Y + PositionTolerance
	);
	TestTrue(
		TEXT("lower providers stay below the consumer after collision resolution"),
		ThirdProvider->Position.Y + PositionTolerance >= StackConsumer->Position.Y + 150.0 + 50.0
	);
	TestAllNodePairsDoNotOverlap(*this, TEXT("stable provider stack"), Snapshot, FirstPlan);
	const FGraphSnapshot FixedPointSnapshot = UsePlanAsAuthoredSnapshot(Snapshot, FirstPlan);
	PlansMatch(*this, TEXT("provider-stack fixed point"), FirstPlan, BuildLayout(FixedPointSnapshot, Settings), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutPreservationDeterminismIdempotenceTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Preservation.DeterministicAndIdempotentFixedPoint",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutPreservationDeterminismIdempotenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot Snapshot = MakePreservedEventIslandsGraph(true);
	const FLayoutSettings Settings = MakePreserveAuthoredSettings();
	const FLayoutPlan CanonicalPlan = BuildLayout(Snapshot, Settings);
	TestFalse(TEXT("the canonical preservation graph is valid"), CanonicalPlan.HasErrors());

	FGraphSnapshot ReversedSnapshot = Snapshot;
	ReverseSnapshotInsertion(ReversedSnapshot);
	PlansMatch(*this, TEXT("preservation reversed insertion"), CanonicalPlan, BuildLayout(ReversedSnapshot, Settings));
	for (uint32 Seed = 1; Seed <= 4; ++Seed)
	{
		FGraphSnapshot ShuffledSnapshot = Snapshot;
		ShuffleSnapshotInsertion(ShuffledSnapshot, Seed * 0x27d4eb2dU);
		PlansMatch(
			*this,
			FString::Printf(TEXT("preservation shuffle seed %u"), Seed),
			CanonicalPlan,
			BuildLayout(ShuffledSnapshot, Settings)
		);
	}

	const FGraphSnapshot FixedPointSnapshot = UsePlanAsAuthoredSnapshot(Snapshot, CanonicalPlan);
	// Geometry/routes must be a fixed point. Displacement is intentionally relative to the
	// supplied authored snapshot, so it becomes zero on this second pass.
	PlansMatch(*this, TEXT("preservation fixed point"), CanonicalPlan, BuildLayout(FixedPointSnapshot, Settings), false);
	return true;
}

FGraphSnapshot MakeLongEdgeGraph()
{
	FGraphSnapshot Snapshot;
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("LongA")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("LongB")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("LongC")));
	Snapshot.Nodes.Add(MakeExecutionNode(TEXT("LongD")));
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("A-B"), TEXT("LongA"), TEXT("ExecOut"), TEXT("LongB"), TEXT("ExecIn"), 0)
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("B-C"), TEXT("LongB"), TEXT("ExecOut"), TEXT("LongC"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("C-D"), TEXT("LongC"), TEXT("ExecOut"), TEXT("LongD"), TEXT("ExecIn"))
	);
	Snapshot.ExecutionEdges.Add(
		MakeExecutionEdge(TEXT("A-D-Long"), TEXT("LongA"), TEXT("ExecOut"), TEXT("LongD"), TEXT("ExecIn"), 1, false)
	);
	return Snapshot;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FK2LayoutRepeatedBuildTest,
	"Project.Unit Tests.GraphFormatter.K2Layout.Idempotence.RepeatedBuild",
	EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext
)

bool FK2LayoutRepeatedBuildTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FGraphSnapshot Snapshot = MakeLongEdgeGraph();
	const FLayoutPlan First = BuildLayout(Snapshot);
	const FLayoutPlan Second = BuildLayout(Snapshot);
	TestFalse(TEXT("long-edge graph is valid"), First.HasErrors());
	TestTrue(TEXT("long edge introduces virtual layout vertices"), First.Statistics.VirtualNodeCount > 0);
	const FPlannedEdgeRoute* LongRoute = FindPlannedRoute(First, TEXT("A-D-Long"));
	TestNotNull(TEXT("long edge receives a deterministic route"), LongRoute);
	if (LongRoute != nullptr)
	{
		TestEqual(TEXT("rank-three long edge has two intermediate waypoints"), LongRoute->Waypoints.Num(), 2);
	}
	PlansMatch(*this, TEXT("repeated build"), First, Second);
	return true;
}
} // namespace
} // namespace GraphFormatter::K2Layout::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
