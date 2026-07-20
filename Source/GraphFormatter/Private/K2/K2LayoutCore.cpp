/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2LayoutCore.h"

#include "Algo/Sort.h"
#include "Containers/BitArray.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/Function.h"

namespace GraphFormatter::K2Layout
{
bool FLayoutPlan::HasErrors() const noexcept
{
	for (const FLayoutDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Severity == EDiagnosticSeverity::Error) { return true; }
	}
	return false;
}

namespace Private
{
[[nodiscard]]
bool StableLess(const FString& A, const FString& B)
{ return A.Compare(B, ESearchCase::CaseSensitive) < 0; }

[[nodiscard]]
bool StableEqual(const FString& A, const FString& B)
{ return A.Equals(B, ESearchCase::CaseSensitive); }

[[nodiscard]]
double SnapNearest(const double Value, const double Grid)
{ return Grid > 0.0 ? FMath::RoundToDouble(Value / Grid) * Grid : Value; }

[[nodiscard]]
double SnapUp(const double Value, const double Grid)
{ return Grid > 0.0 ? FMath::CeilToDouble(Value / Grid) * Grid : Value; }

[[nodiscard]]
double SnapDown(const double Value, const double Grid)
{ return Grid > 0.0 ? FMath::FloorToDouble(Value / Grid) * Grid : Value; }

[[nodiscard]]
double Median(TArray<double> Values)
{
	if (Values.IsEmpty()) { return 0.0; }
	Values.Sort();
	const int32 Middle = Values.Num() / 2;
	return Values.Num() % 2 == 0 ? (Values[Middle - 1] + Values[Middle]) * 0.5 : Values[Middle];
}

struct FPortState
{
	FPortSnapshot Snapshot;
};

struct FNodeState
{
	FNodeSnapshot Snapshot;
	TArray<FPortState> Ports;
	TMap<FPortKey, int32> PortByKey;
	TArray<int32> ExecutionIn;
	TArray<int32> ExecutionOut;
	TArray<int32> DataIn;
	TArray<int32> DataOut;
	FVector2D Position{ FVector2D::ZeroVector };
	int32 Component{ INDEX_NONE };
	int32 Scc{ INDEX_NONE };
	int32 Rank{ INDEX_NONE };
	int32 Order{ INDEX_NONE };
	int32 Vertex{ INDEX_NONE };
	bool bPlaced{ false };
};

struct FExecutionEdgeState
{
	FExecutionEdgeSnapshot Snapshot;
	int32 SourceNode{ INDEX_NONE };
	int32 SourcePort{ INDEX_NONE };
	int32 TargetNode{ INDEX_NONE };
	int32 TargetPort{ INDEX_NONE };
	int32 EffectiveBranchOrder{ 0 };
	TArray<int32> Chain;
};

struct FDataEdgeState
{
	FDataEdgeSnapshot Snapshot;
	int32 SourceNode{ INDEX_NONE };
	int32 SourcePort{ INDEX_NONE };
	int32 TargetNode{ INDEX_NONE };
	int32 TargetPort{ INDEX_NONE };
};

struct FSccState
{
	FString StableKey;
	TArray<int32> Nodes;
	TArray<int32> Out;
	int32 Component{ INDEX_NONE };
	int32 Rank{ 0 };
	int32 InDegree{ 0 };
};

struct FVertexState
{
	FString StableKey;
	FVector2D Size{ FVector2D::ZeroVector };
	FVector2D Position{ FVector2D::ZeroVector };
	TArray<int32> InSegments;
	TArray<int32> OutSegments;
	int32 Node{ INDEX_NONE };
	int32 Component{ INDEX_NONE };
	int32 Rank{ INDEX_NONE };
	int32 Order{ INDEX_NONE };
	int32 AlignmentBlock{ INDEX_NONE };
	double RelativeY{ 0.0 };
	bool bVirtual{ false };
};

struct FSegmentState
{
	int32 ExecutionEdge{ INDEX_NONE };
	int32 SourceVertex{ INDEX_NONE };
	int32 TargetVertex{ INDEX_NONE };
	double SourcePortY{ 0.0 };
	double TargetPortY{ 0.0 };
	int32 SourcePortOrder{ 0 };
	int32 TargetPortOrder{ 0 };
};

struct FOrderConstraint
{
	int32 Before{ INDEX_NONE };
	int32 After{ INDEX_NONE };
	FString SubjectKey;
};

struct FNeighbourMetric
{
	double Barycenter{ 0.0 };
	double Median{ 0.0 };
	int32 PreviousOrder{ INDEX_NONE };
	bool bConnected{ false };
};

struct FAlignmentBlock
{
	FString StableKey;
	TArray<int32> Vertices;
	double DesiredOrder{ 0.0 };
	double BaseY{ 0.0 };
	int32 GlobalOrder{ INDEX_NONE };
};

struct FComponentState
{
	FString StableKey;
	TArray<int32> Nodes;
	TArray<int32> Vertices;
	TArray<TArray<int32>> Layers;
	FVector2D PackedOffset{ FVector2D::ZeroVector };
};

struct FRect
{
	double Left{ 0.0 };
	double Top{ 0.0 };
	double Right{ 0.0 };
	double Bottom{ 0.0 };
};

class FLayoutBuilder
{
public:
	FLayoutBuilder(const FGraphSnapshot& InSnapshot, const FLayoutSettings& InSettings)
		: Snapshot(InSnapshot)
		, Settings(InSettings)
	{
	}

	[[nodiscard]]
	FLayoutPlan Build();

private:
	void SanitizeSettings();
	void BuildNodes();
	void BuildEdges();
	void BuildComponents();
	void BuildExecutionSccs();
	void RankExecutionSccs();
	void BuildExecutionVertices();
	void BuildBranchConstraints();
	void OrderExecutionLayers();
	void PlaceExecutionLayers();
	void PlacePureDataNodes();
	void PackComponents();
	void BuildResult();

	void AddDiagnostic(EDiagnosticSeverity Severity, EDiagnosticCode Code, const FString& Subject, FString Message);
	void BuildPorts(FNodeState& Node, const FNodeSnapshot& SourceNode);
	void BuildExecutionEdges(const TMap<FEdgeKey, int32>& EdgeKeyCounts);
	void BuildDataEdges(const TMap<FEdgeKey, int32>& EdgeKeyCounts);
	[[nodiscard]]
	bool ResolveEndpoint(
		const FPortReference& Reference, EPortDirection Direction, EPortKind Kind, int32& OutNode, int32& OutPort, const FString& EdgeKey
	);
	void SortIncidentEdges();
	void VisitTarjan(
		int32 NodeIndex,
		int32& NextIndex,
		TArray<int32>& Indices,
		TArray<int32>& LowLinks,
		TArray<int32>& Stack,
		TBitArray<>& OnStack,
		TArray<TArray<int32>>& FoundSccs
	);
	void NormalizeSccs(TArray<TArray<int32>> FoundSccs);
	void BuildComponentLayers(FComponentState& Component);
	void ApplyOrderingSweep(FComponentState& Component, bool bDownward, bool bMedianFirst);
	void SortLayerByNeighbours(TArray<int32>& Layer, bool bUseIncoming, bool bMedianFirst);
	void ApplyHardOrder(TArray<int32>& Layer);
	void ImproveByAdjacentSwaps(FComponentState& Component, bool bReverse, int32& RemainingEvaluationBudget);
	[[nodiscard]]
	bool LayerHonorsHardOrder(const TArray<int32>& Layer) const;
	[[nodiscard]]
	int64 CountCrossings(const FComponentState& Component) const;
	[[nodiscard]]
	int64 CountCrossingsBetweenRanks(const FComponentState& Component, int32 LowerRank) const;
	void SelectAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& OutBlocks);
	void OrderAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& Blocks);
	void PositionAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& Blocks);
	void PositionRankColumns(FComponentState& Component);
	void PlacePureWave(FComponentState& Component, TArray<int32>& RemainingPureNodes);
	void PlaceDataOnlyAnchors(FComponentState& Component, TArray<int32>& RemainingPureNodes);
	void PlacePureNode(int32 NodeIndex, const TArray<int32>& PlacedTargets, TArray<FRect>& Obstacles);
	[[nodiscard]]
	double FindCollisionFreeY(const FNodeState& Node, double X, double DesiredY, const TArray<FRect>& Obstacles) const;
	[[nodiscard]]
	FRect CalculateComponentBounds(const FComponentState& Component) const;
	void OffsetComponent(FComponentState& Component, const FVector2D& Offset);

	[[nodiscard]]
	bool IsExecutionParticipant(const FNodeState& Node) const;
	[[nodiscard]]
	const FPortState& GetPort(int32 NodeIndex, int32 PortIndex) const;
	[[nodiscard]]
	int32 FirstDownstreamVertex(const FExecutionEdgeState& Edge) const;
	[[nodiscard]]
	bool IsGridEnabled() const;
	[[nodiscard]]
	bool IsHybridGrid() const;
	[[nodiscard]]
	double GridX() const;
	[[nodiscard]]
	double GridY() const;

	const FGraphSnapshot& Snapshot;
	FLayoutSettings Settings;
	FLayoutPlan Plan;
	TArray<FNodeState> Nodes;
	TArray<FExecutionEdgeState> ExecutionEdges;
	TArray<FDataEdgeState> DataEdges;
	TArray<FSccState> Sccs;
	TArray<FVertexState> Vertices;
	TArray<FSegmentState> Segments;
	TArray<FOrderConstraint> BranchConstraints;
	TArray<FComponentState> Components;
	TMap<FNodeKey, int32> NodeByKey;
	TSet<FEdgeKey> ReportedDuplicateEdgeKeys;
	bool bReportedContradictoryBranchOrder{ false };
};

void FLayoutBuilder::AddDiagnostic(
	const EDiagnosticSeverity Severity, const EDiagnosticCode Code, const FString& Subject, FString Message
)
{ Plan.Diagnostics.Add(FLayoutDiagnostic{ Severity, Code, Subject, MoveTemp(Message) }); }

void FLayoutBuilder::SanitizeSettings()
{
	const auto NonNegativeFinite = [](const double Value)
	{ return FMath::IsFinite(Value) ? FMath::Max(0.0, Value) : 0.0; };
	Settings.HorizontalSpacing = NonNegativeFinite(Settings.HorizontalSpacing);
	Settings.VerticalSpacing = NonNegativeFinite(Settings.VerticalSpacing);
	Settings.BranchSpacing = NonNegativeFinite(Settings.BranchSpacing);
	Settings.PureNodeHorizontalSpacing = NonNegativeFinite(Settings.PureNodeHorizontalSpacing);
	Settings.PureNodeVerticalSpacing = NonNegativeFinite(Settings.PureNodeVerticalSpacing);
	Settings.CollisionClearance = NonNegativeFinite(Settings.CollisionClearance);
	Settings.ComponentSpacing.X = NonNegativeFinite(Settings.ComponentSpacing.X);
	Settings.ComponentSpacing.Y = NonNegativeFinite(Settings.ComponentSpacing.Y);
	Settings.ComponentRowWidth = NonNegativeFinite(Settings.ComponentRowWidth);
	Settings.GridSize.X = NonNegativeFinite(Settings.GridSize.X);
	Settings.GridSize.Y = NonNegativeFinite(Settings.GridSize.Y);
	Settings.OrderingSweeps = FMath::Clamp(Settings.OrderingSweeps, 0, 64);
	Settings.AdjacentSwapPasses = FMath::Clamp(Settings.AdjacentSwapPasses, 0, 32);
	Settings.AdjacentSwapEvaluationBudget = FMath::Clamp(Settings.AdjacentSwapEvaluationBudget, 0, 100000);
}

bool FLayoutBuilder::IsGridEnabled() const { return Settings.GridPolicy != EGridPolicy::None; }

bool FLayoutBuilder::IsHybridGrid() const { return Settings.GridPolicy == EGridPolicy::HybridExecution; }

double FLayoutBuilder::GridX() const { return IsGridEnabled() ? Settings.GridSize.X : 0.0; }

double FLayoutBuilder::GridY() const { return IsGridEnabled() ? Settings.GridSize.Y : 0.0; }

const FPortState& FLayoutBuilder::GetPort(const int32 NodeIndex, const int32 PortIndex) const
{ return Nodes[NodeIndex].Ports[PortIndex]; }

bool FLayoutBuilder::IsExecutionParticipant(const FNodeState& Node) const
{ return !Node.Snapshot.bIsPure || !Node.ExecutionIn.IsEmpty() || !Node.ExecutionOut.IsEmpty(); }

void FLayoutBuilder::BuildPorts(FNodeState& Node, const FNodeSnapshot& SourceNode)
{
	TArray<int32> PortIndices;
	PortIndices.Reserve(SourceNode.Ports.Num());
	for (int32 Index = 0; Index < SourceNode.Ports.Num(); ++Index)
	{
		PortIndices.Add(Index);
	}
	PortIndices.Sort(
		[&SourceNode](const int32 A, const int32 B)
		{
			const FPortSnapshot& Left = SourceNode.Ports[A];
			const FPortSnapshot& Right = SourceNode.Ports[B];
			if (!StableEqual(Left.Key.Value, Right.Key.Value)) { return StableLess(Left.Key.Value, Right.Key.Value); }
			return A < B;
		}
	);

	for (int32 Cursor = 0; Cursor < PortIndices.Num();)
	{
		const FPortSnapshot& Port = SourceNode.Ports[PortIndices[Cursor]];
		int32 End = Cursor + 1;
		while (End < PortIndices.Num() && StableEqual(Port.Key.Value, SourceNode.Ports[PortIndices[End]].Key.Value))
		{
			++End;
		}
		if (!Port.Key.IsValid())
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::EmptyStableKey,
				SourceNode.Key.Value,
				TEXT("A port has an empty stable key and was omitted.")
			);
		}
		else if (End - Cursor > 1)
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::DuplicateStableKey,
				Port.Key.Value,
				FString::Printf(
					TEXT("Node '%s' contains duplicate port keys; all duplicates were omitted."), *SourceNode.Key.Value
				)
			);
		}
		else
		{
			FPortSnapshot AcceptedPort = Port;
			const bool bInvalidX = !FMath::IsFinite(AcceptedPort.Offset.X);
			const bool bInvalidY = !FMath::IsFinite(AcceptedPort.Offset.Y);
			if (bInvalidX || bInvalidY)
			{
				AcceptedPort.Offset.X = bInvalidX ? 0.0 : AcceptedPort.Offset.X;
				AcceptedPort.Offset.Y = bInvalidY ? 0.0 : AcceptedPort.Offset.Y;
				AddDiagnostic(
					EDiagnosticSeverity::Warning,
					EDiagnosticCode::InvalidNodeGeometry,
					Port.Key.Value,
					FString::Printf(
						TEXT("Port '%s/%s' has a non-finite offset; invalid axes were clamped to zero."),
						*SourceNode.Key.Value,
						*Port.Key.Value
					)
				);
			}
			const int32 PortIndex = Node.Ports.Add(FPortState{ MoveTemp(AcceptedPort) });
			Node.PortByKey.Add(Port.Key, PortIndex);
		}
		Cursor = End;
	}
}

void FLayoutBuilder::BuildNodes()
{
	TArray<int32> NodeIndices;
	NodeIndices.Reserve(Snapshot.Nodes.Num());
	for (int32 Index = 0; Index < Snapshot.Nodes.Num(); ++Index)
	{
		NodeIndices.Add(Index);
	}
	NodeIndices.Sort(
		[this](const int32 A, const int32 B)
		{
			const FString& Left = Snapshot.Nodes[A].Key.Value;
			const FString& Right = Snapshot.Nodes[B].Key.Value;
			return StableEqual(Left, Right) ? A < B : StableLess(Left, Right);
		}
	);

	for (int32 Cursor = 0; Cursor < NodeIndices.Num();)
	{
		const FNodeSnapshot& SourceNode = Snapshot.Nodes[NodeIndices[Cursor]];
		int32 End = Cursor + 1;
		while (End < NodeIndices.Num() && StableEqual(SourceNode.Key.Value, Snapshot.Nodes[NodeIndices[End]].Key.Value))
		{
			++End;
		}
		if (!SourceNode.Key.IsValid())
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::EmptyStableKey,
				TEXT("<node>"),
				TEXT("A node has an empty stable key and was omitted.")
			);
		}
		else if (End - Cursor > 1)
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::DuplicateStableKey,
				SourceNode.Key.Value,
				TEXT("Duplicate node keys are ambiguous; all nodes with this key were omitted.")
			);
		}
		else
		{
			FNodeState Node;
			Node.Snapshot = SourceNode;
			Node.Snapshot.Ports.Reset();
			const bool bInvalidWidth = !FMath::IsFinite(SourceNode.Size.X) || SourceNode.Size.X <= 0.0;
			const bool bInvalidHeight = !FMath::IsFinite(SourceNode.Size.Y) || SourceNode.Size.Y <= 0.0;
			if (bInvalidWidth || bInvalidHeight)
			{
				Node.Snapshot.Size.X = bInvalidWidth ? 1.0 : SourceNode.Size.X;
				Node.Snapshot.Size.Y = bInvalidHeight ? 1.0 : SourceNode.Size.Y;
				AddDiagnostic(
					EDiagnosticSeverity::Warning,
					EDiagnosticCode::InvalidNodeGeometry,
					SourceNode.Key.Value,
					TEXT("Non-finite or non-positive node dimensions were clamped to one layout unit.")
				);
			}
			BuildPorts(Node, SourceNode);
			const int32 NodeIndex = Nodes.Add(MoveTemp(Node));
			NodeByKey.Add(SourceNode.Key, NodeIndex);
		}
		Cursor = End;
	}
}

bool FLayoutBuilder::ResolveEndpoint(
	const FPortReference& Reference,
	const EPortDirection Direction,
	const EPortKind Kind,
	int32& OutNode,
	int32& OutPort,
	const FString& EdgeKey
)
{
	const int32* FoundNode = NodeByKey.Find(Reference.Node);
	if (FoundNode == nullptr)
	{
		AddDiagnostic(
			EDiagnosticSeverity::Error,
			EDiagnosticCode::MissingEndpointNode,
			EdgeKey,
			FString::Printf(TEXT("Edge endpoint node '%s' is absent or invalid."), *Reference.Node.Value)
		);
		return false;
	}
	const int32* FoundPort = Nodes[*FoundNode].PortByKey.Find(Reference.Port);
	if (FoundPort == nullptr)
	{
		AddDiagnostic(
			EDiagnosticSeverity::Error,
			EDiagnosticCode::MissingEndpointPort,
			EdgeKey,
			FString::Printf(
				TEXT("Edge endpoint port '%s/%s' is absent or invalid."), *Reference.Node.Value, *Reference.Port.Value
			)
		);
		return false;
	}
	const FPortSnapshot& Port = Nodes[*FoundNode].Ports[*FoundPort].Snapshot;
	if (Port.Direction != Direction || Port.Kind != Kind)
	{
		AddDiagnostic(
			EDiagnosticSeverity::Error,
			EDiagnosticCode::EndpointPortMismatch,
			EdgeKey,
			FString::Printf(
				TEXT("Edge endpoint '%s/%s' has the wrong direction or kind."), *Reference.Node.Value, *Reference.Port.Value
			)
		);
		return false;
	}
	OutNode = *FoundNode;
	OutPort = *FoundPort;
	return true;
}

void FLayoutBuilder::BuildExecutionEdges(const TMap<FEdgeKey, int32>& EdgeKeyCounts)
{
	TArray<int32> EdgeIndices;
	for (int32 Index = 0; Index < Snapshot.ExecutionEdges.Num(); ++Index)
	{
		EdgeIndices.Add(Index);
	}
	EdgeIndices.Sort(
		[this](const int32 A, const int32 B)
		{
			const FString& Left = Snapshot.ExecutionEdges[A].Key.Value;
			const FString& Right = Snapshot.ExecutionEdges[B].Key.Value;
			return StableEqual(Left, Right) ? A < B : StableLess(Left, Right);
		}
	);

	for (const int32 SourceIndex : EdgeIndices)
	{
		const FExecutionEdgeSnapshot& SourceEdge = Snapshot.ExecutionEdges[SourceIndex];
		const int32* Count = EdgeKeyCounts.Find(SourceEdge.Key);
		if (!SourceEdge.Key.IsValid())
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::EmptyStableKey,
				TEXT("<execution-edge>"),
				TEXT("An execution edge has an empty stable key and was omitted.")
			);
			continue;
		}
		if (Count != nullptr && *Count > 1)
		{
			if (!ReportedDuplicateEdgeKeys.Contains(SourceEdge.Key))
			{
				ReportedDuplicateEdgeKeys.Add(SourceEdge.Key);
				AddDiagnostic(
					EDiagnosticSeverity::Error,
					EDiagnosticCode::DuplicateStableKey,
					SourceEdge.Key.Value,
					TEXT("Duplicate edge keys are ambiguous; all edges with this key were omitted.")
				);
			}
			continue;
		}

		FExecutionEdgeState Edge;
		Edge.Snapshot = SourceEdge;
		const bool bSourceValid = ResolveEndpoint(
			SourceEdge.Source,
			EPortDirection::Output,
			EPortKind::Execution,
			Edge.SourceNode,
			Edge.SourcePort,
			SourceEdge.Key.Value
		);
		const bool bTargetValid = ResolveEndpoint(
			SourceEdge.Target,
			EPortDirection::Input,
			EPortKind::Execution,
			Edge.TargetNode,
			Edge.TargetPort,
			SourceEdge.Key.Value
		);
		if (!bSourceValid || !bTargetValid) { continue; }
		const int32 PortOrder = GetPort(Edge.SourceNode, Edge.SourcePort).Snapshot.SemanticOrder;
		Edge.EffectiveBranchOrder = SourceEdge.BranchOrder == INDEX_NONE ? PortOrder : SourceEdge.BranchOrder;
		const int32 EdgeIndex = ExecutionEdges.Add(MoveTemp(Edge));
		Nodes[ExecutionEdges[EdgeIndex].SourceNode].ExecutionOut.Add(EdgeIndex);
		Nodes[ExecutionEdges[EdgeIndex].TargetNode].ExecutionIn.Add(EdgeIndex);
	}
}

void FLayoutBuilder::BuildDataEdges(const TMap<FEdgeKey, int32>& EdgeKeyCounts)
{
	TArray<int32> EdgeIndices;
	for (int32 Index = 0; Index < Snapshot.DataEdges.Num(); ++Index)
	{
		EdgeIndices.Add(Index);
	}
	EdgeIndices.Sort(
		[this](const int32 A, const int32 B)
		{
			const FString& Left = Snapshot.DataEdges[A].Key.Value;
			const FString& Right = Snapshot.DataEdges[B].Key.Value;
			return StableEqual(Left, Right) ? A < B : StableLess(Left, Right);
		}
	);

	for (const int32 SourceIndex : EdgeIndices)
	{
		const FDataEdgeSnapshot& SourceEdge = Snapshot.DataEdges[SourceIndex];
		const int32* Count = EdgeKeyCounts.Find(SourceEdge.Key);
		if (!SourceEdge.Key.IsValid())
		{
			AddDiagnostic(
				EDiagnosticSeverity::Error,
				EDiagnosticCode::EmptyStableKey,
				TEXT("<data-edge>"),
				TEXT("A data edge has an empty stable key and was omitted.")
			);
			continue;
		}
		if (Count != nullptr && *Count > 1)
		{
			if (!ReportedDuplicateEdgeKeys.Contains(SourceEdge.Key))
			{
				ReportedDuplicateEdgeKeys.Add(SourceEdge.Key);
				AddDiagnostic(
					EDiagnosticSeverity::Error,
					EDiagnosticCode::DuplicateStableKey,
					SourceEdge.Key.Value,
					TEXT("Duplicate edge keys are ambiguous; all edges with this key were omitted.")
				);
			}
			continue;
		}

		FDataEdgeState Edge;
		Edge.Snapshot = SourceEdge;
		const bool bSourceValid = ResolveEndpoint(
			SourceEdge.Source, EPortDirection::Output, EPortKind::Data, Edge.SourceNode, Edge.SourcePort, SourceEdge.Key.Value
		);
		const bool bTargetValid = ResolveEndpoint(
			SourceEdge.Target, EPortDirection::Input, EPortKind::Data, Edge.TargetNode, Edge.TargetPort, SourceEdge.Key.Value
		);
		if (!bSourceValid || !bTargetValid) { continue; }
		const int32 EdgeIndex = DataEdges.Add(MoveTemp(Edge));
		Nodes[DataEdges[EdgeIndex].SourceNode].DataOut.Add(EdgeIndex);
		Nodes[DataEdges[EdgeIndex].TargetNode].DataIn.Add(EdgeIndex);
	}
}

void FLayoutBuilder::SortIncidentEdges()
{
	for (FNodeState& Node : Nodes)
	{
		Node.ExecutionOut.Sort(
			[this](const int32 A, const int32 B)
			{
				const FExecutionEdgeState& Left = ExecutionEdges[A];
				const FExecutionEdgeState& Right = ExecutionEdges[B];
				if (Left.EffectiveBranchOrder != Right.EffectiveBranchOrder)
				{
					return Left.EffectiveBranchOrder < Right.EffectiveBranchOrder;
				}
				return StableLess(Left.Snapshot.Key.Value, Right.Snapshot.Key.Value);
			}
		);
		Node.ExecutionIn.Sort(
			[this](const int32 A, const int32 B)
			{
				const FExecutionEdgeState& Left = ExecutionEdges[A];
				const FExecutionEdgeState& Right = ExecutionEdges[B];
				const FString& LeftNode = Nodes[Left.SourceNode].Snapshot.Key.Value;
				const FString& RightNode = Nodes[Right.SourceNode].Snapshot.Key.Value;
				return StableEqual(LeftNode, RightNode) ? StableLess(Left.Snapshot.Key.Value, Right.Snapshot.Key.Value)
														: StableLess(LeftNode, RightNode);
			}
		);
		Node.DataOut.Sort([this](const int32 A, const int32 B)
						  { return StableLess(DataEdges[A].Snapshot.Key.Value, DataEdges[B].Snapshot.Key.Value); });
		Node.DataIn.Sort(
			[this](const int32 A, const int32 B)
			{
				const FDataEdgeState& Left = DataEdges[A];
				const FDataEdgeState& Right = DataEdges[B];
				const int32 LeftOrder = GetPort(Left.TargetNode, Left.TargetPort).Snapshot.SemanticOrder;
				const int32 RightOrder = GetPort(Right.TargetNode, Right.TargetPort).Snapshot.SemanticOrder;
				return LeftOrder == RightOrder ? StableLess(Left.Snapshot.Key.Value, Right.Snapshot.Key.Value)
											   : LeftOrder < RightOrder;
			}
		);
	}
}

void FLayoutBuilder::BuildEdges()
{
	TMap<FEdgeKey, int32> EdgeKeyCounts;
	for (const FExecutionEdgeSnapshot& Edge : Snapshot.ExecutionEdges)
	{
		if (Edge.Key.IsValid()) { ++EdgeKeyCounts.FindOrAdd(Edge.Key); }
	}
	for (const FDataEdgeSnapshot& Edge : Snapshot.DataEdges)
	{
		if (Edge.Key.IsValid()) { ++EdgeKeyCounts.FindOrAdd(Edge.Key); }
	}
	BuildExecutionEdges(EdgeKeyCounts);
	BuildDataEdges(EdgeKeyCounts);
	SortIncidentEdges();

	for (const FNodeState& Node : Nodes)
	{
		if (Node.Snapshot.bIsPure && (!Node.ExecutionIn.IsEmpty() || !Node.ExecutionOut.IsEmpty()))
		{
			AddDiagnostic(
				EDiagnosticSeverity::Warning,
				EDiagnosticCode::PureNodeHasExecutionEdge,
				Node.Snapshot.Key.Value,
				TEXT("A node marked pure has execution edges and will be treated as impure.")
			);
		}
	}
}

void FLayoutBuilder::BuildComponents()
{
	TArray<TArray<int32>> Neighbours;
	Neighbours.SetNum(Nodes.Num());
	const auto Connect = [&Neighbours](const int32 A, const int32 B)
	{
		Neighbours[A].Add(B);
		Neighbours[B].Add(A);
	};
	for (const FExecutionEdgeState& Edge : ExecutionEdges)
	{
		Connect(Edge.SourceNode, Edge.TargetNode);
	}
	for (const FDataEdgeState& Edge : DataEdges)
	{
		Connect(Edge.SourceNode, Edge.TargetNode);
	}
	for (TArray<int32>& NodeNeighbours : Neighbours)
	{
		NodeNeighbours.Sort([this](const int32 A, const int32 B)
							{ return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
		TArray<int32> Unique;
		for (const int32 Neighbour : NodeNeighbours)
		{
			if (Unique.IsEmpty() || Unique.Last() != Neighbour) { Unique.Add(Neighbour); }
		}
		NodeNeighbours = MoveTemp(Unique);
	}

	for (int32 StartNode = 0; StartNode < Nodes.Num(); ++StartNode)
	{
		if (Nodes[StartNode].Component != INDEX_NONE) { continue; }
		const int32 ComponentIndex = Components.AddDefaulted();
		FComponentState& Component = Components[ComponentIndex];
		Component.StableKey = Nodes[StartNode].Snapshot.Key.Value;
		TArray<int32> Queue{ StartNode };
		Nodes[StartNode].Component = ComponentIndex;
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const int32 NodeIndex = Queue[Head];
			Component.Nodes.Add(NodeIndex);
			for (const int32 Neighbour : Neighbours[NodeIndex])
			{
				if (Nodes[Neighbour].Component == INDEX_NONE)
				{
					Nodes[Neighbour].Component = ComponentIndex;
					Queue.Add(Neighbour);
				}
			}
		}
		Component.Nodes.Sort([this](const int32 A, const int32 B)
							 { return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
	}
}

void FLayoutBuilder::VisitTarjan(
	const int32 NodeIndex,
	int32& NextIndex,
	TArray<int32>& Indices,
	TArray<int32>& LowLinks,
	TArray<int32>& Stack,
	TBitArray<>& OnStack,
	TArray<TArray<int32>>& FoundSccs
)
{
	Indices[NodeIndex] = NextIndex;
	LowLinks[NodeIndex] = NextIndex;
	++NextIndex;
	Stack.Add(NodeIndex);
	OnStack[NodeIndex] = true;

	TArray<int32> Successors;
	for (const int32 EdgeIndex : Nodes[NodeIndex].ExecutionOut)
	{
		Successors.Add(ExecutionEdges[EdgeIndex].TargetNode);
	}
	Successors.Sort([this](const int32 A, const int32 B)
					{ return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
	int32 Previous = INDEX_NONE;
	for (const int32 Successor : Successors)
	{
		if (Successor == Previous) { continue; }
		Previous = Successor;
		if (Indices[Successor] == INDEX_NONE)
		{
			VisitTarjan(Successor, NextIndex, Indices, LowLinks, Stack, OnStack, FoundSccs);
			LowLinks[NodeIndex] = FMath::Min(LowLinks[NodeIndex], LowLinks[Successor]);
		}
		else if (OnStack[Successor]) { LowLinks[NodeIndex] = FMath::Min(LowLinks[NodeIndex], Indices[Successor]); }
	}

	if (LowLinks[NodeIndex] != Indices[NodeIndex]) { return; }
	TArray<int32>& SccNodes = FoundSccs.AddDefaulted_GetRef();
	while (!Stack.IsEmpty())
	{
		const int32 Popped = Stack.Pop(EAllowShrinking::No);
		OnStack[Popped] = false;
		SccNodes.Add(Popped);
		if (Popped == NodeIndex) { break; }
	}
}

void FLayoutBuilder::NormalizeSccs(TArray<TArray<int32>> FoundSccs)
{
	for (TArray<int32>& Found : FoundSccs)
	{
		Found.Sort([this](const int32 A, const int32 B)
				   { return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
	}
	FoundSccs.Sort([this](const TArray<int32>& A, const TArray<int32>& B)
				   { return StableLess(Nodes[A[0]].Snapshot.Key.Value, Nodes[B[0]].Snapshot.Key.Value); });

	for (TArray<int32>& Found : FoundSccs)
	{
		FSccState Scc;
		Scc.StableKey = Nodes[Found[0]].Snapshot.Key.Value;
		Scc.Component = Nodes[Found[0]].Component;
		Scc.Nodes = MoveTemp(Found);
		const int32 SccIndex = Sccs.Add(MoveTemp(Scc));
		for (const int32 NodeIndex : Sccs[SccIndex].Nodes)
		{
			Nodes[NodeIndex].Scc = SccIndex;
		}
	}
}

void FLayoutBuilder::BuildExecutionSccs()
{
	TArray<int32> Indices;
	TArray<int32> LowLinks;
	Indices.Init(INDEX_NONE, Nodes.Num());
	LowLinks.Init(INDEX_NONE, Nodes.Num());
	TArray<int32> Stack;
	TBitArray<> OnStack(false, Nodes.Num());
	TArray<TArray<int32>> FoundSccs;
	int32 NextIndex = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (IsExecutionParticipant(Nodes[NodeIndex]) && Indices[NodeIndex] == INDEX_NONE)
		{
			VisitTarjan(NodeIndex, NextIndex, Indices, LowLinks, Stack, OnStack, FoundSccs);
		}
	}
	NormalizeSccs(MoveTemp(FoundSccs));

	for (const FSccState& Scc : Sccs)
	{
		bool bCycle = Scc.Nodes.Num() > 1;
		if (!bCycle)
		{
			const int32 NodeIndex = Scc.Nodes[0];
			for (const int32 EdgeIndex : Nodes[NodeIndex].ExecutionOut)
			{
				bCycle |= ExecutionEdges[EdgeIndex].TargetNode == NodeIndex;
			}
		}
		if (bCycle)
		{
			++Plan.Statistics.CondensedExecutionCycleCount;
			AddDiagnostic(
				EDiagnosticSeverity::Information,
				EDiagnosticCode::ExecutionCycleCondensed,
				Scc.StableKey,
				FString::Printf(TEXT("Condensed an execution cycle containing %d node(s)."), Scc.Nodes.Num())
			);
		}
	}
}

void FLayoutBuilder::RankExecutionSccs()
{
	for (const FExecutionEdgeState& Edge : ExecutionEdges)
	{
		const int32 SourceScc = Nodes[Edge.SourceNode].Scc;
		const int32 TargetScc = Nodes[Edge.TargetNode].Scc;
		if (SourceScc != TargetScc) { Sccs[SourceScc].Out.Add(TargetScc); }
	}
	for (FSccState& Scc : Sccs)
	{
		Scc.Out.Sort([this](const int32 A, const int32 B) { return StableLess(Sccs[A].StableKey, Sccs[B].StableKey); });
		TArray<int32> Unique;
		for (const int32 Target : Scc.Out)
		{
			if (Unique.IsEmpty() || Unique.Last() != Target) { Unique.Add(Target); }
		}
		Scc.Out = MoveTemp(Unique);
		for (const int32 Target : Scc.Out)
		{
			++Sccs[Target].InDegree;
		}
	}

	TArray<int32> Ready;
	for (int32 SccIndex = 0; SccIndex < Sccs.Num(); ++SccIndex)
	{
		if (Sccs[SccIndex].InDegree == 0) { Ready.Add(SccIndex); }
	}
	while (!Ready.IsEmpty())
	{
		Ready.Sort([this](const int32 A, const int32 B) { return StableLess(Sccs[A].StableKey, Sccs[B].StableKey); });
		const int32 Source = Ready[0];
		Ready.RemoveAt(0, 1, EAllowShrinking::No);
		for (const int32 Target : Sccs[Source].Out)
		{
			Sccs[Target].Rank = FMath::Max(Sccs[Target].Rank, Sccs[Source].Rank + 1);
			--Sccs[Target].InDegree;
			if (Sccs[Target].InDegree == 0) { Ready.Add(Target); }
		}
	}
	for (const FSccState& Scc : Sccs)
	{
		for (const int32 NodeIndex : Scc.Nodes)
		{
			Nodes[NodeIndex].Rank = Scc.Rank;
		}
	}
}

void FLayoutBuilder::BuildExecutionVertices()
{
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		FNodeState& Node = Nodes[NodeIndex];
		if (!IsExecutionParticipant(Node)) { continue; }
		FVertexState Vertex;
		Vertex.StableKey = FString::Printf(TEXT("N|%s"), *Node.Snapshot.Key.Value);
		Vertex.Size = Node.Snapshot.Size;
		Vertex.Node = NodeIndex;
		Vertex.Component = Node.Component;
		Vertex.Rank = Node.Rank;
		Node.Vertex = Vertices.Add(MoveTemp(Vertex));
	}

	for (int32 EdgeIndex = 0; EdgeIndex < ExecutionEdges.Num(); ++EdgeIndex)
	{
		FExecutionEdgeState& Edge = ExecutionEdges[EdgeIndex];
		const int32 SourceVertex = Nodes[Edge.SourceNode].Vertex;
		const int32 TargetVertex = Nodes[Edge.TargetNode].Vertex;
		Edge.Chain.Add(SourceVertex);
		const int32 SourceRank = Vertices[SourceVertex].Rank;
		const int32 TargetRank = Vertices[TargetVertex].Rank;
		for (int32 Rank = SourceRank + 1; Rank < TargetRank; ++Rank)
		{
			FVertexState Virtual;
			Virtual.StableKey = FString::Printf(TEXT("V|%s|%08d"), *Edge.Snapshot.Key.Value, Rank);
			Virtual.bVirtual = true;
			Virtual.Component = Nodes[Edge.SourceNode].Component;
			Virtual.Rank = Rank;
			Edge.Chain.Add(Vertices.Add(MoveTemp(Virtual)));
			++Plan.Statistics.VirtualNodeCount;
		}
		Edge.Chain.Add(TargetVertex);

		const FPortSnapshot& SourcePort = GetPort(Edge.SourceNode, Edge.SourcePort).Snapshot;
		const FPortSnapshot& TargetPort = GetPort(Edge.TargetNode, Edge.TargetPort).Snapshot;
		for (int32 ChainIndex = 0; ChainIndex + 1 < Edge.Chain.Num(); ++ChainIndex)
		{
			FSegmentState Segment;
			Segment.ExecutionEdge = EdgeIndex;
			Segment.SourceVertex = Edge.Chain[ChainIndex];
			Segment.TargetVertex = Edge.Chain[ChainIndex + 1];
			Segment.SourcePortY = ChainIndex == 0 ? SourcePort.Offset.Y : 0.0;
			Segment.TargetPortY = ChainIndex + 2 == Edge.Chain.Num() ? TargetPort.Offset.Y : 0.0;
			Segment.SourcePortOrder = ChainIndex == 0 ? SourcePort.SemanticOrder : 0;
			Segment.TargetPortOrder = ChainIndex + 2 == Edge.Chain.Num() ? TargetPort.SemanticOrder : 0;
			const int32 SegmentIndex = Segments.Add(MoveTemp(Segment));
			Vertices[Segments[SegmentIndex].SourceVertex].OutSegments.Add(SegmentIndex);
			Vertices[Segments[SegmentIndex].TargetVertex].InSegments.Add(SegmentIndex);
		}
	}

	for (int32 VertexIndex = 0; VertexIndex < Vertices.Num(); ++VertexIndex)
	{
		Components[Vertices[VertexIndex].Component].Vertices.Add(VertexIndex);
	}
	for (FComponentState& Component : Components)
	{
		Component.Vertices.Sort([this](const int32 A, const int32 B)
								{ return StableLess(Vertices[A].StableKey, Vertices[B].StableKey); });
		BuildComponentLayers(Component);
	}
}

void FLayoutBuilder::BuildComponentLayers(FComponentState& Component)
{
	int32 MaximumRank = INDEX_NONE;
	for (const int32 VertexIndex : Component.Vertices)
	{
		MaximumRank = FMath::Max(MaximumRank, Vertices[VertexIndex].Rank);
	}
	Component.Layers.SetNum(MaximumRank + 1);
	for (const int32 VertexIndex : Component.Vertices)
	{
		Component.Layers[Vertices[VertexIndex].Rank].Add(VertexIndex);
	}
	for (TArray<int32>& Layer : Component.Layers)
	{
		Layer.Sort([this](const int32 A, const int32 B)
				   { return StableLess(Vertices[A].StableKey, Vertices[B].StableKey); });
		for (int32 Order = 0; Order < Layer.Num(); ++Order)
		{
			Vertices[Layer[Order]].Order = Order;
		}
	}
}

int32 FLayoutBuilder::FirstDownstreamVertex(const FExecutionEdgeState& Edge) const
{ return Edge.Chain.Num() > 1 ? Edge.Chain[1] : INDEX_NONE; }

void FLayoutBuilder::BuildBranchConstraints()
{
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		TArray<int32> Children;
		TSet<int32> SeenChildren;
		for (const int32 EdgeIndex : Nodes[NodeIndex].ExecutionOut)
		{
			const int32 Child = FirstDownstreamVertex(ExecutionEdges[EdgeIndex]);
			if (Child == INDEX_NONE || Vertices[Child].Rank != Nodes[NodeIndex].Rank + 1) { continue; }
			if (!SeenChildren.Contains(Child))
			{
				SeenChildren.Add(Child);
				Children.Add(Child);
			}
		}
		for (int32 Index = 0; Index + 1 < Children.Num(); ++Index)
		{
			if (Children[Index] != Children[Index + 1])
			{
				BranchConstraints.Add(
					FOrderConstraint{ Children[Index], Children[Index + 1], Nodes[NodeIndex].Snapshot.Key.Value }
				);
			}
		}
	}
}

void FLayoutBuilder::SortLayerByNeighbours(TArray<int32>& Layer, const bool bUseIncoming, const bool bMedianFirst)
{
	TMap<int32, FNeighbourMetric> Metrics;
	for (const int32 VertexIndex : Layer)
	{
		const FVertexState& Vertex = Vertices[VertexIndex];
		const TArray<int32>& SegmentIndices = bUseIncoming ? Vertex.InSegments : Vertex.OutSegments;
		TArray<double> Values;
		for (const int32 SegmentIndex : SegmentIndices)
		{
			const FSegmentState& Segment = Segments[SegmentIndex];
			const int32 Neighbour = bUseIncoming ? Segment.SourceVertex : Segment.TargetVertex;
			const int32 ExpectedRank = bUseIncoming ? Vertex.Rank - 1 : Vertex.Rank + 1;
			if (Vertices[Neighbour].Rank != ExpectedRank) { continue; }
			const int32 PortOrder = bUseIncoming ? Segment.TargetPortOrder : Segment.SourcePortOrder;
			Values.Add(static_cast<double>(Vertices[Neighbour].Order) + static_cast<double>(PortOrder) * 0.000001);
		}
		FNeighbourMetric Metric;
		Metric.PreviousOrder = Vertex.Order;
		Metric.bConnected = !Values.IsEmpty();
		if (Metric.bConnected)
		{
			double Sum = 0.0;
			for (const double Value : Values)
			{
				Sum += Value;
			}
			Metric.Barycenter = Sum / static_cast<double>(Values.Num());
			Metric.Median = Private::Median(MoveTemp(Values));
		}
		Metrics.Add(VertexIndex, Metric);
	}

	Layer.Sort(
		[this, &Metrics, bMedianFirst](const int32 A, const int32 B)
		{
			const FNeighbourMetric& Left = Metrics.FindChecked(A);
			const FNeighbourMetric& Right = Metrics.FindChecked(B);
			if (Left.bConnected != Right.bConnected) { return Left.bConnected; }
			const double LeftPrimary = bMedianFirst ? Left.Median : Left.Barycenter;
			const double RightPrimary = bMedianFirst ? Right.Median : Right.Barycenter;
			if (LeftPrimary != RightPrimary) { return LeftPrimary < RightPrimary; }
			const double LeftSecondary = bMedianFirst ? Left.Barycenter : Left.Median;
			const double RightSecondary = bMedianFirst ? Right.Barycenter : Right.Median;
			if (LeftSecondary != RightSecondary) { return LeftSecondary < RightSecondary; }
			if (Left.PreviousOrder != Right.PreviousOrder) { return Left.PreviousOrder < Right.PreviousOrder; }
			return StableLess(Vertices[A].StableKey, Vertices[B].StableKey);
		}
	);
	ApplyHardOrder(Layer);
}

void FLayoutBuilder::ApplyHardOrder(TArray<int32>& Layer)
{
	TMap<int32, int32> DesiredOrder;
	TMap<int32, int32> InDegree;
	TMap<int32, TArray<int32>> Out;
	for (int32 Index = 0; Index < Layer.Num(); ++Index)
	{
		DesiredOrder.Add(Layer[Index], Index);
		InDegree.Add(Layer[Index], 0);
	}
	for (const FOrderConstraint& Constraint : BranchConstraints)
	{
		if (DesiredOrder.Contains(Constraint.Before) && DesiredOrder.Contains(Constraint.After))
		{
			Out.FindOrAdd(Constraint.Before).Add(Constraint.After);
			++InDegree.FindChecked(Constraint.After);
		}
	}

	TArray<int32> Ready;
	for (const int32 VertexIndex : Layer)
	{
		if (InDegree.FindChecked(VertexIndex) == 0) { Ready.Add(VertexIndex); }
	}
	const auto SortReady = [this, &DesiredOrder](TArray<int32>& Values)
	{
		Values.Sort(
			[this, &DesiredOrder](const int32 A, const int32 B)
			{
				const int32 Left = DesiredOrder.FindChecked(A);
				const int32 Right = DesiredOrder.FindChecked(B);
				return Left == Right ? StableLess(Vertices[A].StableKey, Vertices[B].StableKey) : Left < Right;
			}
		);
	};

	TArray<int32> Ordered;
	while (!Ready.IsEmpty())
	{
		SortReady(Ready);
		const int32 VertexIndex = Ready[0];
		Ready.RemoveAt(0, 1, EAllowShrinking::No);
		Ordered.Add(VertexIndex);
		if (const TArray<int32>* Targets = Out.Find(VertexIndex))
		{
			for (const int32 Target : *Targets)
			{
				int32& TargetDegree = InDegree.FindChecked(Target);
				--TargetDegree;
				if (TargetDegree == 0) { Ready.Add(Target); }
			}
		}
	}

	if (Ordered.Num() != Layer.Num())
	{
		TArray<int32> Remaining;
		for (const int32 VertexIndex : Layer)
		{
			if (!Ordered.Contains(VertexIndex)) { Remaining.Add(VertexIndex); }
		}
		SortReady(Remaining);
		Ordered.Append(Remaining);
		if (!bReportedContradictoryBranchOrder)
		{
			bReportedContradictoryBranchOrder = true;
			AddDiagnostic(
				EDiagnosticSeverity::Warning,
				EDiagnosticCode::ContradictoryBranchOrder,
				Vertices[Remaining[0]].StableKey,
				TEXT("Branch-output order constraints conflict; the stable desired order broke the cycle.")
			);
		}
	}
	Layer = MoveTemp(Ordered);
	for (int32 Order = 0; Order < Layer.Num(); ++Order)
	{
		Vertices[Layer[Order]].Order = Order;
	}
}

void FLayoutBuilder::ApplyOrderingSweep(FComponentState& Component, const bool bDownward, const bool bMedianFirst)
{
	if (bDownward)
	{
		for (int32 Rank = 1; Rank < Component.Layers.Num(); ++Rank)
		{
			SortLayerByNeighbours(Component.Layers[Rank], true, bMedianFirst);
		}
		return;
	}
	for (int32 Rank = Component.Layers.Num() - 2; Rank >= 0; --Rank)
	{
		SortLayerByNeighbours(Component.Layers[Rank], false, bMedianFirst);
	}
}

int64 FLayoutBuilder::CountCrossingsBetweenRanks(const FComponentState& Component, const int32 LowerRank) const
{
	struct FCrossingSegment
	{
		uint64 SourceKey = 0;
		uint64 TargetKey = 0;
		int32 SegmentIndex = INDEX_NONE;
	};
	const auto EndpointKey = [](const int32 Order, const int32 PortOrder)
	{
		const uint32 BiasedOrder = static_cast<uint32>(Order) ^ 0x80000000U;
		const uint32 BiasedPort = static_cast<uint32>(PortOrder) ^ 0x80000000U;
		return static_cast<uint64>(BiasedOrder) << 32 | static_cast<uint64>(BiasedPort);
	};

	TArray<FCrossingSegment> RankSegments;
	for (const int32 VertexIndex : Component.Layers[LowerRank])
	{
		for (const int32 SegmentIndex : Vertices[VertexIndex].OutSegments)
		{
			const FSegmentState& Segment = Segments[SegmentIndex];
			if (Vertices[Segment.TargetVertex].Rank == LowerRank + 1)
			{
				RankSegments.Add(
					FCrossingSegment{
						EndpointKey(Vertices[Segment.SourceVertex].Order, Segment.SourcePortOrder),
						EndpointKey(Vertices[Segment.TargetVertex].Order, Segment.TargetPortOrder),
						SegmentIndex,
					}
				);
			}
		}
	}
	RankSegments.Sort(
		[](const FCrossingSegment& Left, const FCrossingSegment& Right)
		{
			if (Left.SourceKey != Right.SourceKey) { return Left.SourceKey < Right.SourceKey; }
			if (Left.TargetKey != Right.TargetKey) { return Left.TargetKey < Right.TargetKey; }
			return Left.SegmentIndex < Right.SegmentIndex;
		}
	);
	TArray<uint64> TargetKeys;
	TargetKeys.Reserve(RankSegments.Num());
	for (const FCrossingSegment& Segment : RankSegments)
	{
		TargetKeys.Add(Segment.TargetKey);
	}
	TargetKeys.Sort();
	for (int32 Index = TargetKeys.Num() - 1; Index > 0; --Index)
	{
		if (TargetKeys[Index] == TargetKeys[Index - 1]) { TargetKeys.RemoveAt(Index, 1, EAllowShrinking::No); }
	}
	TMap<uint64, int32> TargetIndices;
	for (int32 Index = 0; Index < TargetKeys.Num(); ++Index)
	{
		TargetIndices.Add(TargetKeys[Index], Index);
	}
	TArray<int64> FenwickTree;
	FenwickTree.Init(0, TargetKeys.Num() + 1);
	const auto PrefixCount = [&FenwickTree](const int32 TargetIndex)
	{
		int64 Count = 0;
		for (int32 TreeIndex = TargetIndex + 1; TreeIndex > 0; TreeIndex -= TreeIndex & -TreeIndex)
		{
			Count += FenwickTree[TreeIndex];
		}
		return Count;
	};
	const auto AddTarget = [&FenwickTree](const int32 TargetIndex)
	{
		for (int32 TreeIndex = TargetIndex + 1; TreeIndex < FenwickTree.Num(); TreeIndex += TreeIndex & -TreeIndex)
		{
			++FenwickTree[TreeIndex];
		}
	};

	int64 Crossings = 0;
	int64 PreviousSegmentCount = 0;
	for (int32 GroupBegin = 0; GroupBegin < RankSegments.Num();)
	{
		int32 GroupEnd = GroupBegin + 1;
		while (GroupEnd < RankSegments.Num() && RankSegments[GroupEnd].SourceKey == RankSegments[GroupBegin].SourceKey)
		{
			++GroupEnd;
		}
		for (int32 Index = GroupBegin; Index < GroupEnd; ++Index)
		{
			const int32 TargetIndex = TargetIndices.FindChecked(RankSegments[Index].TargetKey);
			Crossings += PreviousSegmentCount - PrefixCount(TargetIndex);
		}
		for (int32 Index = GroupBegin; Index < GroupEnd; ++Index)
		{
			AddTarget(TargetIndices.FindChecked(RankSegments[Index].TargetKey));
			++PreviousSegmentCount;
		}
		GroupBegin = GroupEnd;
	}
	return Crossings;
}

int64 FLayoutBuilder::CountCrossings(const FComponentState& Component) const
{
	int64 Crossings = 0;
	for (int32 Rank = 0; Rank + 1 < Component.Layers.Num(); ++Rank)
	{
		Crossings += CountCrossingsBetweenRanks(Component, Rank);
	}
	return Crossings;
}

bool FLayoutBuilder::LayerHonorsHardOrder(const TArray<int32>& Layer) const
{
	TMap<int32, int32> Position;
	for (int32 Index = 0; Index < Layer.Num(); ++Index)
	{
		Position.Add(Layer[Index], Index);
	}
	for (const FOrderConstraint& Constraint : BranchConstraints)
	{
		const int32* Before = Position.Find(Constraint.Before);
		const int32* After = Position.Find(Constraint.After);
		if (Before != nullptr && After != nullptr && *Before >= *After) { return false; }
	}
	return true;
}

void FLayoutBuilder::ImproveByAdjacentSwaps(FComponentState& Component, const bool bReverse, int32& RemainingEvaluationBudget)
{
	for (int32 LayerOffset = 0; LayerOffset < Component.Layers.Num(); ++LayerOffset)
	{
		const int32 Rank = bReverse ? Component.Layers.Num() - 1 - LayerOffset : LayerOffset;
		TArray<int32>& Layer = Component.Layers[Rank];
		if (Layer.Num() < 2) { continue; }
		for (int32 Offset = 0; Offset + 1 < Layer.Num(); ++Offset)
		{
			if (RemainingEvaluationBudget <= 0) { return; }
			--RemainingEvaluationBudget;
			const int32 Index = bReverse ? Layer.Num() - 2 - Offset : Offset;
			int64 Before = 0;
			if (Rank > 0) { Before += CountCrossingsBetweenRanks(Component, Rank - 1); }
			if (Rank + 1 < Component.Layers.Num()) { Before += CountCrossingsBetweenRanks(Component, Rank); }

			Swap(Layer[Index], Layer[Index + 1]);
			Vertices[Layer[Index]].Order = Index;
			Vertices[Layer[Index + 1]].Order = Index + 1;
			int64 After = Before;
			if (LayerHonorsHardOrder(Layer))
			{
				After = 0;
				if (Rank > 0) { After += CountCrossingsBetweenRanks(Component, Rank - 1); }
				if (Rank + 1 < Component.Layers.Num()) { After += CountCrossingsBetweenRanks(Component, Rank); }
			}
			if (After >= Before)
			{
				Swap(Layer[Index], Layer[Index + 1]);
				Vertices[Layer[Index]].Order = Index;
				Vertices[Layer[Index + 1]].Order = Index + 1;
			}
		}
	}
}

void FLayoutBuilder::OrderExecutionLayers()
{
	for (FComponentState& Component : Components)
	{
		if (Component.Layers.IsEmpty()) { continue; }
		for (TArray<int32>& Layer : Component.Layers)
		{
			ApplyHardOrder(Layer);
		}
		const int64 InitialCrossings = CountCrossings(Component);
		Plan.Statistics.InitialExecutionCrossings += InitialCrossings;
		int64 BestCrossings = InitialCrossings;
		TArray<TArray<int32>> BestLayers = Component.Layers;

		for (int32 Sweep = 0; Sweep < Settings.OrderingSweeps; ++Sweep)
		{
			const bool bMedianFirst = Sweep % 2 != 0;
			ApplyOrderingSweep(Component, true, bMedianFirst);
			ApplyOrderingSweep(Component, false, !bMedianFirst);
			const int64 Crossings = CountCrossings(Component);
			if (Crossings < BestCrossings)
			{
				BestCrossings = Crossings;
				BestLayers = Component.Layers;
			}
		}
		Component.Layers = MoveTemp(BestLayers);
		for (int32 Rank = 0; Rank < Component.Layers.Num(); ++Rank)
		{
			for (int32 Order = 0; Order < Component.Layers[Rank].Num(); ++Order)
			{
				Vertices[Component.Layers[Rank][Order]].Order = Order;
			}
		}
		int32 RemainingEvaluationBudget = Settings.AdjacentSwapEvaluationBudget;
		for (int32 Pass = 0; Pass < Settings.AdjacentSwapPasses && RemainingEvaluationBudget > 0; ++Pass)
		{
			ImproveByAdjacentSwaps(Component, Pass % 2 != 0, RemainingEvaluationBudget);
		}
		if (Settings.AdjacentSwapPasses > 0 && RemainingEvaluationBudget == 0)
		{
			AddDiagnostic(
				EDiagnosticSeverity::Information,
				EDiagnosticCode::OrderingBudgetExhausted,
				Vertices[Component.Layers[0][0]].StableKey,
				TEXT("The deterministic adjacent-swap work budget was exhausted; the best completed ordering was retained.")
			);
		}
	}
}

void FLayoutBuilder::SelectAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& OutBlocks)
{
	TArray<int32> CandidateEdges;
	for (int32 EdgeIndex = 0; EdgeIndex < ExecutionEdges.Num(); ++EdgeIndex)
	{
		const FExecutionEdgeState& Edge = ExecutionEdges[EdgeIndex];
		if (Nodes[Edge.SourceNode].Component == Nodes[Component.Nodes[0]].Component
			&& Vertices[Edge.Chain.Last()].Rank > Vertices[Edge.Chain[0]].Rank)
		{
			CandidateEdges.Add(EdgeIndex);
		}
	}
	CandidateEdges.Sort(
		[this](const int32 A, const int32 B)
		{
			const FExecutionEdgeState& Left = ExecutionEdges[A];
			const FExecutionEdgeState& Right = ExecutionEdges[B];
			const bool bLeftPreferred = Left.Snapshot.bPreferredAlignment
									 || GetPort(Left.SourceNode, Left.SourcePort).Snapshot.bPreferredExecutionPort
									 || GetPort(Left.TargetNode, Left.TargetPort).Snapshot.bPreferredExecutionPort;
			const bool bRightPreferred = Right.Snapshot.bPreferredAlignment
									  || GetPort(Right.SourceNode, Right.SourcePort).Snapshot.bPreferredExecutionPort
									  || GetPort(Right.TargetNode, Right.TargetPort).Snapshot.bPreferredExecutionPort;
			if (bLeftPreferred != bRightPreferred) { return bLeftPreferred; }
			if (Left.EffectiveBranchOrder != Right.EffectiveBranchOrder)
			{
				return Left.EffectiveBranchOrder < Right.EffectiveBranchOrder;
			}
			return StableLess(Left.Snapshot.Key.Value, Right.Snapshot.Key.Value);
		}
	);

	TArray<int32> SelectedIncoming;
	TArray<int32> SelectedOutgoing;
	SelectedIncoming.Init(INDEX_NONE, Vertices.Num());
	SelectedOutgoing.Init(INDEX_NONE, Vertices.Num());
	for (const int32 EdgeIndex : CandidateEdges)
	{
		const FExecutionEdgeState& Edge = ExecutionEdges[EdgeIndex];
		bool bAvailable = true;
		for (int32 ChainIndex = 0; ChainIndex + 1 < Edge.Chain.Num(); ++ChainIndex)
		{
			bAvailable &= SelectedOutgoing[Edge.Chain[ChainIndex]] == INDEX_NONE;
			bAvailable &= SelectedIncoming[Edge.Chain[ChainIndex + 1]] == INDEX_NONE;
		}
		if (!bAvailable) { continue; }
		for (int32 ChainIndex = 0; ChainIndex + 1 < Edge.Chain.Num(); ++ChainIndex)
		{
			const int32 Source = Edge.Chain[ChainIndex];
			const int32 Target = Edge.Chain[ChainIndex + 1];
			for (const int32 SegmentIndex : Vertices[Source].OutSegments)
			{
				const FSegmentState& Segment = Segments[SegmentIndex];
				if (Segment.ExecutionEdge == EdgeIndex && Segment.TargetVertex == Target)
				{
					SelectedOutgoing[Source] = SegmentIndex;
					SelectedIncoming[Target] = SegmentIndex;
					break;
				}
			}
		}
	}

	TSet<int32> ComponentVertices;
	for (const int32 VertexIndex : Component.Vertices)
	{
		ComponentVertices.Add(VertexIndex);
	}
	TSet<int32> Visited;
	for (const int32 StartVertex : Component.Vertices)
	{
		if (SelectedIncoming[StartVertex] != INDEX_NONE || Visited.Contains(StartVertex)) { continue; }
		FAlignmentBlock Block;
		Block.StableKey = Vertices[StartVertex].StableKey;
		int32 VertexIndex = StartVertex;
		double RelativeY = 0.0;
		while (ComponentVertices.Contains(VertexIndex) && !Visited.Contains(VertexIndex))
		{
			Visited.Add(VertexIndex);
			Vertices[VertexIndex].RelativeY = RelativeY;
			Block.Vertices.Add(VertexIndex);
			const int32 SegmentIndex = SelectedOutgoing[VertexIndex];
			if (SegmentIndex == INDEX_NONE) { break; }
			const FSegmentState& Segment = Segments[SegmentIndex];
			RelativeY += Segment.SourcePortY - Segment.TargetPortY;
			VertexIndex = Segment.TargetVertex;
		}
		OutBlocks.Add(MoveTemp(Block));
	}
	for (const int32 VertexIndex : Component.Vertices)
	{
		if (!Visited.Contains(VertexIndex))
		{
			FAlignmentBlock Block;
			Block.StableKey = Vertices[VertexIndex].StableKey;
			Block.Vertices.Add(VertexIndex);
			Vertices[VertexIndex].RelativeY = 0.0;
			OutBlocks.Add(MoveTemp(Block));
		}
	}
	OutBlocks.Sort([](const FAlignmentBlock& A, const FAlignmentBlock& B)
				   { return StableLess(A.StableKey, B.StableKey); });
	for (int32 BlockIndex = 0; BlockIndex < OutBlocks.Num(); ++BlockIndex)
	{
		FAlignmentBlock& Block = OutBlocks[BlockIndex];
		double Desired = 0.0;
		for (const int32 VertexIndex : Block.Vertices)
		{
			Vertices[VertexIndex].AlignmentBlock = BlockIndex;
			const int32 Rank = Vertices[VertexIndex].Rank;
			const int32 Denominator = FMath::Max(1, Component.Layers[Rank].Num() - 1);
			Desired += static_cast<double>(Vertices[VertexIndex].Order) / static_cast<double>(Denominator);
		}
		Block.DesiredOrder = Desired / static_cast<double>(Block.Vertices.Num());
	}
}

void FLayoutBuilder::OrderAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& Blocks)
{
	TArray<TArray<int32>> Out;
	TArray<int32> InDegree;
	Out.SetNum(Blocks.Num());
	InDegree.Init(0, Blocks.Num());
	TSet<uint64> SeenPairs;
	for (const FOrderConstraint& Constraint : BranchConstraints)
	{
		if (Vertices[Constraint.Before].Component != Nodes[Component.Nodes[0]].Component) { continue; }
		const int32 BeforeBlock = Vertices[Constraint.Before].AlignmentBlock;
		const int32 AfterBlock = Vertices[Constraint.After].AlignmentBlock;
		if (BeforeBlock == AfterBlock) { continue; }
		const uint64 Pair = static_cast<uint64>(static_cast<uint32>(BeforeBlock)) << 32 | static_cast<uint32>(AfterBlock);
		if (!SeenPairs.Contains(Pair))
		{
			SeenPairs.Add(Pair);
			Out[BeforeBlock].Add(AfterBlock);
			++InDegree[AfterBlock];
		}
	}

	TArray<int32> Ready;
	for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
	{
		if (InDegree[BlockIndex] == 0) { Ready.Add(BlockIndex); }
	}
	const auto SortBlocks = [&Blocks](TArray<int32>& Values)
	{
		Values.Sort(
			[&Blocks](const int32 A, const int32 B)
			{
				if (Blocks[A].DesiredOrder != Blocks[B].DesiredOrder)
				{
					return Blocks[A].DesiredOrder < Blocks[B].DesiredOrder;
				}
				return StableLess(Blocks[A].StableKey, Blocks[B].StableKey);
			}
		);
	};

	TArray<int32> Ordered;
	while (!Ready.IsEmpty())
	{
		SortBlocks(Ready);
		const int32 BlockIndex = Ready[0];
		Ready.RemoveAt(0, 1, EAllowShrinking::No);
		Ordered.Add(BlockIndex);
		for (const int32 Target : Out[BlockIndex])
		{
			--InDegree[Target];
			if (InDegree[Target] == 0) { Ready.Add(Target); }
		}
	}
	if (Ordered.Num() != Blocks.Num())
	{
		TArray<int32> Remaining;
		for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
		{
			if (!Ordered.Contains(BlockIndex)) { Remaining.Add(BlockIndex); }
		}
		SortBlocks(Remaining);
		Ordered.Append(Remaining);
		if (!bReportedContradictoryBranchOrder)
		{
			bReportedContradictoryBranchOrder = true;
			AddDiagnostic(
				EDiagnosticSeverity::Warning,
				EDiagnosticCode::ContradictoryBranchOrder,
				Blocks[Remaining[0]].StableKey,
				TEXT("Alignment blocks make branch-output constraints contradictory; stable order broke the cycle.")
			);
		}
	}
	for (int32 GlobalOrder = 0; GlobalOrder < Ordered.Num(); ++GlobalOrder)
	{
		Blocks[Ordered[GlobalOrder]].GlobalOrder = GlobalOrder;
	}
	for (TArray<int32>& Layer : Component.Layers)
	{
		Layer.Sort(
			[this, &Blocks](const int32 A, const int32 B)
			{
				const int32 Left = Blocks[Vertices[A].AlignmentBlock].GlobalOrder;
				const int32 Right = Blocks[Vertices[B].AlignmentBlock].GlobalOrder;
				return Left == Right ? StableLess(Vertices[A].StableKey, Vertices[B].StableKey) : Left < Right;
			}
		);
		for (int32 Order = 0; Order < Layer.Num(); ++Order)
		{
			Vertices[Layer[Order]].Order = Order;
		}
	}
}

void FLayoutBuilder::PositionRankColumns(FComponentState& Component)
{
	TArray<double> RankWidths;
	RankWidths.Init(0.0, Component.Layers.Num());
	for (int32 Rank = 0; Rank < Component.Layers.Num(); ++Rank)
	{
		for (const int32 VertexIndex : Component.Layers[Rank])
		{
			RankWidths[Rank] = FMath::Max(RankWidths[Rank], Vertices[VertexIndex].Size.X);
		}
	}
	double X = 0.0;
	for (int32 Rank = 0; Rank < Component.Layers.Num(); ++Rank)
	{
		if (Rank > 0)
		{
			X += RankWidths[Rank - 1] + Settings.HorizontalSpacing;
			X = SnapUp(X, GridX());
		}
		for (const int32 VertexIndex : Component.Layers[Rank])
		{
			Vertices[VertexIndex].Position.X = X;
		}
	}
}

void FLayoutBuilder::PositionAlignmentBlocks(FComponentState& Component, TArray<FAlignmentBlock>& Blocks)
{
	TArray<int32> BlocksByOrder;
	for (int32 BlockIndex = 0; BlockIndex < Blocks.Num(); ++BlockIndex)
	{
		BlocksByOrder.Add(BlockIndex);
	}
	BlocksByOrder.Sort([&Blocks](const int32 A, const int32 B) { return Blocks[A].GlobalOrder < Blocks[B].GlobalOrder; });
	const double Clearance = FMath::Max(Settings.VerticalSpacing, Settings.CollisionClearance);
	for (const int32 BlockIndex : BlocksByOrder)
	{
		FAlignmentBlock& Block = Blocks[BlockIndex];
		double BaseY = 0.0;
		for (const int32 VertexIndex : Block.Vertices)
		{
			BaseY = FMath::Max(BaseY, -Vertices[VertexIndex].RelativeY);
			const int32 Rank = Vertices[VertexIndex].Rank;
			const int32 Order = Vertices[VertexIndex].Order;
			if (Order == 0) { continue; }
			const int32 PreviousVertex = Component.Layers[Rank][Order - 1];
			const int32 PreviousBlockIndex = Vertices[PreviousVertex].AlignmentBlock;
			if (PreviousBlockIndex == BlockIndex) { continue; }
			const FAlignmentBlock& PreviousBlock = Blocks[PreviousBlockIndex];
			const double PreviousBottom = PreviousBlock.BaseY + Vertices[PreviousVertex].RelativeY
										+ Vertices[PreviousVertex].Size.Y;
			BaseY = FMath::Max(BaseY, PreviousBottom + Clearance - Vertices[VertexIndex].RelativeY);

			for (const FOrderConstraint& Constraint : BranchConstraints)
			{
				if (Constraint.After != VertexIndex) { continue; }
				const int32 BeforeBlockIndex = Vertices[Constraint.Before].AlignmentBlock;
				if (BeforeBlockIndex == BlockIndex || Blocks[BeforeBlockIndex].GlobalOrder >= Block.GlobalOrder)
				{
					continue;
				}
				const FAlignmentBlock& BeforeBlock = Blocks[BeforeBlockIndex];
				const double BeforeBottom = BeforeBlock.BaseY + Vertices[Constraint.Before].RelativeY
										  + Vertices[Constraint.Before].Size.Y;
				BaseY =
					FMath::Max(BaseY, BeforeBottom + Clearance + Settings.BranchSpacing - Vertices[VertexIndex].RelativeY);
			}
		}
		Block.BaseY = IsGridEnabled() ? SnapUp(BaseY, GridY()) : BaseY;
		for (const int32 VertexIndex : Block.Vertices)
		{
			double Y = Block.BaseY + Vertices[VertexIndex].RelativeY;
			if (!IsHybridGrid() && IsGridEnabled()) { Y = SnapUp(Y, GridY()); }
			Vertices[VertexIndex].Position.Y = Y;
			if (!Vertices[VertexIndex].bVirtual)
			{
				FNodeState& Node = Nodes[Vertices[VertexIndex].Node];
				Node.Position = Vertices[VertexIndex].Position;
				Node.Order = Vertices[VertexIndex].Order;
				Node.bPlaced = true;
			}
		}
	}
}

void FLayoutBuilder::PlaceExecutionLayers()
{
	Plan.Statistics.FinalExecutionCrossings = 0;
	for (FComponentState& Component : Components)
	{
		if (Component.Layers.IsEmpty()) { continue; }
		const TArray<TArray<int32>> CrossingOptimizedLayers = Component.Layers;
		const int64 CrossingOptimizedCount = CountCrossings(Component);
		TArray<FAlignmentBlock> Blocks;
		SelectAlignmentBlocks(Component, Blocks);
		OrderAlignmentBlocks(Component, Blocks);
		if (CountCrossings(Component) > CrossingOptimizedCount)
		{
			// Alignment is a secondary objective. If block coalescing damages the saved crossing
			// optimum, fall back to singleton blocks while preserving the exact layer order.
			Component.Layers = CrossingOptimizedLayers;
			Blocks.Reset();
			for (int32 Rank = 0; Rank < Component.Layers.Num(); ++Rank)
			{
				for (int32 Order = 0; Order < Component.Layers[Rank].Num(); ++Order)
				{
					const int32 VertexIndex = Component.Layers[Rank][Order];
					Vertices[VertexIndex].Order = Order;
					Vertices[VertexIndex].RelativeY = 0.0;
					Vertices[VertexIndex].AlignmentBlock = Blocks.Num();
					FAlignmentBlock& Block = Blocks.AddDefaulted_GetRef();
					Block.Vertices.Add(VertexIndex);
					Block.StableKey = Vertices[VertexIndex].StableKey;
					Block.DesiredOrder = static_cast<double>(Order);
					Block.GlobalOrder = Order * Component.Layers.Num() + Rank;
				}
			}
			AddDiagnostic(
				EDiagnosticSeverity::Information,
				EDiagnosticCode::AlignmentRejectedCrossings,
				Vertices[Component.Layers[0][0]].StableKey,
				TEXT("Execution alignment blocks were relaxed because they would increase wire crossings.")
			);
		}
		PositionRankColumns(Component);
		PositionAlignmentBlocks(Component, Blocks);
		Plan.Statistics.FinalExecutionCrossings += CountCrossings(Component);
	}
}

[[nodiscard]]
bool RectsOverlap(const FRect& A, const FRect& B)
{ return A.Left < B.Right && A.Right > B.Left && A.Top < B.Bottom && A.Bottom > B.Top; }

[[nodiscard]]
FRect MakeNodeObstacle(const FNodeState& Node, const double HorizontalClearance, const double VerticalClearance)
{
	return FRect{
		Node.Position.X - HorizontalClearance,
		Node.Position.Y - VerticalClearance,
		Node.Position.X + Node.Snapshot.Size.X + HorizontalClearance,
		Node.Position.Y + Node.Snapshot.Size.Y + VerticalClearance,
	};
}

double FLayoutBuilder::FindCollisionFreeY(
	const FNodeState& Node, const double X, const double DesiredY, const TArray<FRect>& Obstacles
) const
{
	TArray<double> Candidates{ IsGridEnabled() ? SnapNearest(DesiredY, GridY()) : DesiredY };
	for (const FRect& Obstacle : Obstacles)
	{
		const bool bHorizontalOverlap = X < Obstacle.Right && X + Node.Snapshot.Size.X > Obstacle.Left;
		if (bHorizontalOverlap)
		{
			Candidates.Add(IsGridEnabled() ? SnapUp(Obstacle.Bottom, GridY()) : Obstacle.Bottom);
			Candidates.Add(
				IsGridEnabled() ? SnapDown(Obstacle.Top - Node.Snapshot.Size.Y, GridY())
								: Obstacle.Top - Node.Snapshot.Size.Y
			);
		}
	}
	Candidates.Sort(
		[DesiredY](const double A, const double B)
		{
			const double LeftDistance = FMath::Abs(A - DesiredY);
			const double RightDistance = FMath::Abs(B - DesiredY);
			return LeftDistance == RightDistance ? A < B : LeftDistance < RightDistance;
		}
	);

	for (const double Candidate : Candidates)
	{
		const FRect CandidateRect{ X, Candidate, X + Node.Snapshot.Size.X, Candidate + Node.Snapshot.Size.Y };
		bool bCollision = false;
		for (const FRect& Obstacle : Obstacles)
		{
			bCollision |= RectsOverlap(CandidateRect, Obstacle);
		}
		if (!bCollision) { return Candidate; }
	}
	return Candidates.IsEmpty() ? DesiredY : Candidates.Last();
}

void FLayoutBuilder::PlacePureNode(const int32 NodeIndex, const TArray<int32>& PlacedTargets, TArray<FRect>& Obstacles)
{
	FNodeState& Node = Nodes[NodeIndex];
	double DesiredX = 0.0;
	bool bHasX = false;
	TArray<double> DesiredPinYs;
	for (const int32 EdgeIndex : Node.DataOut)
	{
		const FDataEdgeState& Edge = DataEdges[EdgeIndex];
		if (!PlacedTargets.Contains(Edge.TargetNode)) { continue; }
		const FNodeState& Target = Nodes[Edge.TargetNode];
		const double CandidateX = Target.Position.X - Settings.PureNodeHorizontalSpacing - Node.Snapshot.Size.X;
		DesiredX = bHasX ? FMath::Min(DesiredX, CandidateX) : CandidateX;
		bHasX = true;
		const double TargetPinY = Target.Position.Y + GetPort(Edge.TargetNode, Edge.TargetPort).Snapshot.Offset.Y;
		DesiredPinYs.Add(TargetPinY - GetPort(Edge.SourceNode, Edge.SourcePort).Snapshot.Offset.Y);
	}
	DesiredX = IsGridEnabled() ? SnapDown(DesiredX, GridX()) : DesiredX;
	const double DesiredY = Median(MoveTemp(DesiredPinYs));
	const double PositionY = FindCollisionFreeY(Node, DesiredX, DesiredY, Obstacles);
	Node.Position = FVector2D{ DesiredX, PositionY };
	Node.bPlaced = true;
	const double VerticalClearance = FMath::Max(Settings.CollisionClearance, Settings.PureNodeVerticalSpacing);
	Obstacles.Add(MakeNodeObstacle(Node, Settings.CollisionClearance, VerticalClearance));
}

void FLayoutBuilder::PlacePureWave(FComponentState& Component, TArray<int32>& RemainingPureNodes)
{
	struct FCandidate
	{
		int32 Node{ INDEX_NONE };
		TArray<int32> Targets;
		FString PrimaryTargetKey;
		int32 TargetPortOrder{ 0 };
	};
	TArray<FCandidate> Candidates;
	for (const int32 NodeIndex : RemainingPureNodes)
	{
		FCandidate Candidate;
		Candidate.Node = NodeIndex;
		Candidate.TargetPortOrder = MAX_int32;
		for (const int32 EdgeIndex : Nodes[NodeIndex].DataOut)
		{
			const FDataEdgeState& Edge = DataEdges[EdgeIndex];
			if (Nodes[Edge.TargetNode].bPlaced && Nodes[Edge.TargetNode].Component == Nodes[NodeIndex].Component)
			{
				Candidate.Targets.AddUnique(Edge.TargetNode);
				const FString& TargetKey = Nodes[Edge.TargetNode].Snapshot.Key.Value;
				if (Candidate.PrimaryTargetKey.IsEmpty() || StableLess(TargetKey, Candidate.PrimaryTargetKey))
				{
					Candidate.PrimaryTargetKey = TargetKey;
				}
				Candidate.TargetPortOrder =
					FMath::Min(Candidate.TargetPortOrder, GetPort(Edge.TargetNode, Edge.TargetPort).Snapshot.SemanticOrder);
			}
		}
		if (!Candidate.Targets.IsEmpty())
		{
			Candidate.Targets.Sort([this](const int32 A, const int32 B)
								   { return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
			Candidates.Add(MoveTemp(Candidate));
		}
	}
	Candidates.Sort(
		[this](const FCandidate& A, const FCandidate& B)
		{
			const bool bLeftShared = A.Targets.Num() > 1;
			const bool bRightShared = B.Targets.Num() > 1;
			if (bLeftShared != bRightShared) { return !bLeftShared; }
			if (!StableEqual(A.PrimaryTargetKey, B.PrimaryTargetKey))
			{
				return StableLess(A.PrimaryTargetKey, B.PrimaryTargetKey);
			}
			if (A.TargetPortOrder != B.TargetPortOrder) { return A.TargetPortOrder < B.TargetPortOrder; }
			return StableLess(Nodes[A.Node].Snapshot.Key.Value, Nodes[B.Node].Snapshot.Key.Value);
		}
	);

	TArray<FRect> Obstacles;
	const double VerticalClearance = FMath::Max(Settings.CollisionClearance, Settings.PureNodeVerticalSpacing);
	for (const int32 NodeIndex : Component.Nodes)
	{
		const FNodeState& Node = Nodes[NodeIndex];
		if (Node.bPlaced) { Obstacles.Add(MakeNodeObstacle(Node, Settings.CollisionClearance, VerticalClearance)); }
	}
	TSet<int32> PlacedThisWave;
	for (const FCandidate& Candidate : Candidates)
	{
		PlacePureNode(Candidate.Node, Candidate.Targets, Obstacles);
		PlacedThisWave.Add(Candidate.Node);
	}
	RemainingPureNodes.RemoveAll([&PlacedThisWave](const int32 NodeIndex) { return PlacedThisWave.Contains(NodeIndex); });
}

void FLayoutBuilder::PlaceDataOnlyAnchors(FComponentState& Component, TArray<int32>& RemainingPureNodes)
{
	TArray<int32> Sinks;
	for (const int32 NodeIndex : RemainingPureNodes)
	{
		bool bHasRemainingTarget = false;
		for (const int32 EdgeIndex : Nodes[NodeIndex].DataOut)
		{
			bHasRemainingTarget |= RemainingPureNodes.Contains(DataEdges[EdgeIndex].TargetNode);
		}
		if (!bHasRemainingTarget) { Sinks.Add(NodeIndex); }
	}
	if (Sinks.IsEmpty() && !RemainingPureNodes.IsEmpty())
	{
		Sinks.Add(RemainingPureNodes[0]);
		AddDiagnostic(
			EDiagnosticSeverity::Warning,
			EDiagnosticCode::PureDataCycleFallback,
			Nodes[RemainingPureNodes[0]].Snapshot.Key.Value,
			TEXT("A pure-data cycle has no downstream anchor; its stable first node was used as the anchor.")
		);
	}
	const double VerticalClearance = FMath::Max(Settings.CollisionClearance, Settings.PureNodeVerticalSpacing);
	TArray<FRect> Obstacles;
	for (const int32 NodeIndex : Component.Nodes)
	{
		const FNodeState& Node = Nodes[NodeIndex];
		if (Node.bPlaced) { Obstacles.Add(MakeNodeObstacle(Node, Settings.CollisionClearance, VerticalClearance)); }
	}

	double DesiredY = 0.0;
	for (const int32 NodeIndex : Sinks)
	{
		FNodeState& Node = Nodes[NodeIndex];
		const double PositionY = FindCollisionFreeY(Node, 0.0, DesiredY, Obstacles);
		Node.Position = FVector2D{ 0.0, PositionY };
		Node.bPlaced = true;
		Obstacles.Add(MakeNodeObstacle(Node, Settings.CollisionClearance, VerticalClearance));
		DesiredY = Node.Position.Y + Node.Snapshot.Size.Y + Settings.PureNodeVerticalSpacing;
	}
	TSet<int32> Anchors;
	for (const int32 NodeIndex : Sinks)
	{
		Anchors.Add(NodeIndex);
	}
	RemainingPureNodes.RemoveAll([&Anchors](const int32 NodeIndex) { return Anchors.Contains(NodeIndex); });
}

void FLayoutBuilder::PlacePureDataNodes()
{
	for (FComponentState& Component : Components)
	{
		TArray<int32> RemainingPureNodes;
		bool bHasPlacedNode = false;
		for (const int32 NodeIndex : Component.Nodes)
		{
			bHasPlacedNode |= Nodes[NodeIndex].bPlaced;
			if (Nodes[NodeIndex].Snapshot.bIsPure && !IsExecutionParticipant(Nodes[NodeIndex]))
			{
				RemainingPureNodes.Add(NodeIndex);
			}
		}
		RemainingPureNodes.Sort([this](const int32 A, const int32 B)
								{ return StableLess(Nodes[A].Snapshot.Key.Value, Nodes[B].Snapshot.Key.Value); });
		if (!bHasPlacedNode) { PlaceDataOnlyAnchors(Component, RemainingPureNodes); }
		while (!RemainingPureNodes.IsEmpty())
		{
			const int32 Before = RemainingPureNodes.Num();
			PlacePureWave(Component, RemainingPureNodes);
			if (RemainingPureNodes.Num() == Before)
			{
				const int32 FallbackNode = RemainingPureNodes[0];
				TArray<int32> Fallback{ FallbackNode };
				PlaceDataOnlyAnchors(Component, Fallback);
				RemainingPureNodes.RemoveAt(0, 1, EAllowShrinking::No);
				AddDiagnostic(
					EDiagnosticSeverity::Warning,
					EDiagnosticCode::PureDataCycleFallback,
					Nodes[FallbackNode].Snapshot.Key.Value,
					TEXT("A pure-data cycle could not reach a placed consumer; a stable fallback anchor was used.")
				);
			}
		}
	}
}

FRect FLayoutBuilder::CalculateComponentBounds(const FComponentState& Component) const
{
	FRect Bounds;
	bool bInitialized = false;
	const auto Expand = [&Bounds, &bInitialized](const FRect& Rect)
	{
		if (!bInitialized)
		{
			Bounds = Rect;
			bInitialized = true;
			return;
		}
		Bounds.Left = FMath::Min(Bounds.Left, Rect.Left);
		Bounds.Top = FMath::Min(Bounds.Top, Rect.Top);
		Bounds.Right = FMath::Max(Bounds.Right, Rect.Right);
		Bounds.Bottom = FMath::Max(Bounds.Bottom, Rect.Bottom);
	};
	for (const int32 NodeIndex : Component.Nodes)
	{
		const FNodeState& Node = Nodes[NodeIndex];
		Expand(
			FRect{ Node.Position.X,
				   Node.Position.Y,
				   Node.Position.X + Node.Snapshot.Size.X,
				   Node.Position.Y + Node.Snapshot.Size.Y }
		);
	}
	for (const int32 VertexIndex : Component.Vertices)
	{
		const FVertexState& Vertex = Vertices[VertexIndex];
		if (Vertex.bVirtual)
		{
			Expand(
				FRect{ Vertex.Position.X - Settings.CollisionClearance,
					   Vertex.Position.Y - Settings.CollisionClearance,
					   Vertex.Position.X + Settings.CollisionClearance,
					   Vertex.Position.Y + Settings.CollisionClearance }
			);
		}
	}
	return Bounds;
}

void FLayoutBuilder::OffsetComponent(FComponentState& Component, const FVector2D& Offset)
{
	Component.PackedOffset = Offset;
	for (const int32 NodeIndex : Component.Nodes)
	{
		Nodes[NodeIndex].Position += Offset;
	}
	for (const int32 VertexIndex : Component.Vertices)
	{
		Vertices[VertexIndex].Position += Offset;
	}
}

void FLayoutBuilder::PackComponents()
{
	double CursorX = 0.0;
	double CursorY = 0.0;
	double RowHeight = 0.0;
	for (FComponentState& Component : Components)
	{
		const FRect Bounds = CalculateComponentBounds(Component);
		const double Width = Bounds.Right - Bounds.Left;
		const bool bWrap = CursorX > 0.0 && Settings.ComponentRowWidth > 0.0
						&& CursorX + Width > Settings.ComponentRowWidth;
		if (bWrap)
		{
			CursorX = 0.0;
			CursorY = SnapUp(CursorY + RowHeight + Settings.ComponentSpacing.Y, GridY());
			RowHeight = 0.0;
		}
		double OffsetX = CursorX - Bounds.Left;
		double OffsetY = CursorY - Bounds.Top;
		if (IsGridEnabled())
		{
			OffsetX = SnapUp(OffsetX, GridX());
			OffsetY = SnapUp(OffsetY, GridY());
		}
		const FVector2D Offset{ OffsetX, OffsetY };
		OffsetComponent(Component, Offset);
		const double ActualRight = Bounds.Right + OffsetX;
		const double ActualBottom = Bounds.Bottom + OffsetY;
		CursorX = SnapUp(ActualRight + Settings.ComponentSpacing.X, GridX());
		RowHeight = FMath::Max(RowHeight, ActualBottom - CursorY);
	}
}

void FLayoutBuilder::BuildResult()
{
	for (const FNodeState& Node : Nodes)
	{
		Plan.Nodes.Add(FPlannedNodePosition{ Node.Snapshot.Key, Node.Position, Node.Component, Node.Rank, Node.Order });
	}
	for (const FExecutionEdgeState& Edge : ExecutionEdges)
	{
		FPlannedEdgeRoute Route;
		Route.Edge = Edge.Snapshot.Key;
		for (int32 ChainIndex = 1; ChainIndex + 1 < Edge.Chain.Num(); ++ChainIndex)
		{
			Route.Waypoints.Add(Vertices[Edge.Chain[ChainIndex]].Position);
		}
		if (!Route.Waypoints.IsEmpty()) { Plan.ExecutionRoutes.Add(MoveTemp(Route)); }
	}
	Plan.Diagnostics.Sort(
		[](const FLayoutDiagnostic& A, const FLayoutDiagnostic& B)
		{
			if (A.Severity != B.Severity) { return A.Severity < B.Severity; }
			if (A.Code != B.Code) { return A.Code < B.Code; }
			if (!StableEqual(A.SubjectKey, B.SubjectKey)) { return StableLess(A.SubjectKey, B.SubjectKey); }
			return StableLess(A.Message, B.Message);
		}
	);
	Plan.Statistics.AcceptedNodeCount = Nodes.Num();
	Plan.Statistics.AcceptedExecutionEdgeCount = ExecutionEdges.Num();
	Plan.Statistics.AcceptedDataEdgeCount = DataEdges.Num();
	Plan.Statistics.ComponentCount = Components.Num();
}

FLayoutPlan FLayoutBuilder::Build()
{
	SanitizeSettings();
	BuildNodes();
	BuildEdges();
	BuildComponents();
	BuildExecutionSccs();
	RankExecutionSccs();
	BuildExecutionVertices();
	BuildBranchConstraints();
	OrderExecutionLayers();
	PlaceExecutionLayers();
	PlacePureDataNodes();
	PackComponents();
	BuildResult();
	return MoveTemp(Plan);
}
} // namespace Private

FLayoutPlan BuildLayout(const FGraphSnapshot& Snapshot, const FLayoutSettings& Settings)
{ return Private::FLayoutBuilder{ Snapshot, Settings }.Build(); }
} // namespace GraphFormatter::K2Layout
