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

bool PlansMatch(FAutomationTestBase& Test, const FString& Context, const FLayoutPlan& Expected, const FLayoutPlan& Actual)
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
