/*---------------------------------------------------------------------------------------------
 * Copyright (c) Howaajin. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/GraphGeometrySnapshot.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"

namespace GraphFormatter::K2
{
namespace
{
constexpr double MinimumGeometryExtent = 0.01;
constexpr double PositionAgreementTolerance = 0.5;
constexpr double MaximumPinOverflow = 128.0;

[[nodiscard]]
FVector2D ToVector2D(const FVector2f Value) noexcept
{ return FVector2D{ static_cast<double>(Value.X), static_cast<double>(Value.Y) }; }

[[nodiscard]]
bool IsFinite(const FVector2D Value) noexcept
{ return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y); }

[[nodiscard]]
bool IsUsableSize(const FVector2D Size) noexcept
{ return IsFinite(Size) && Size.X > MinimumGeometryExtent && Size.Y > MinimumGeometryExtent; }

[[nodiscard]]
bool IsInsideNodeAllowance(const FVector2D Offset, const FVector2D NodeSize) noexcept
{
	return Offset.X >= -MaximumPinOverflow && Offset.Y >= -MaximumPinOverflow
		&& Offset.X <= NodeSize.X + MaximumPinOverflow && Offset.Y <= NodeSize.Y + MaximumPinOverflow;
}

void AddDiagnostic(
	FGraphGeometrySnapshot& Snapshot,
	const EGraphGeometryDiagnosticCode Code,
	const EGraphGeometryDiagnosticSeverity Severity,
	const UEdGraphNode* Node,
	const UEdGraphPin* Pin,
	FString Detail
)
{
	FGraphGeometryDiagnostic Diagnostic;
	Diagnostic.Code = Code;
	Diagnostic.Severity = Severity;
	Diagnostic.NodeGuid = Node != nullptr ? Node->NodeGuid : FGuid{};
	Diagnostic.PinId = Pin != nullptr ? Pin->PinId : FGuid{};
	Diagnostic.Detail = MoveTemp(Detail);
	Snapshot.Diagnostics.Add(MoveTemp(Diagnostic));

	if (Severity == EGraphGeometryDiagnosticSeverity::Error)
	{
		Snapshot.Status = EGraphGeometrySnapshotStatus::InvalidInput;
	}
	else if (Severity == EGraphGeometryDiagnosticSeverity::Retryable && Snapshot.Status == EGraphGeometrySnapshotStatus::Ready)
	{
		Snapshot.Status = EGraphGeometrySnapshotStatus::NeedsRetry;
	}
}

[[nodiscard]]
TOptional<FVector2D> ResolveSlateGraphPosition(
	const SGraphPanel& Panel, const FGeometry& PanelGeometry, const FGeometry& NodeGeometry, const bool bPanelGeometryUsable
)
{
	if (!bPanelGeometryUsable) { return {}; }

	const FVector2f NodeAbsolutePosition = NodeGeometry.GetAbsolutePosition();
	const FVector2f NodePanelPosition = PanelGeometry.AbsoluteToLocal(NodeAbsolutePosition);
	const FVector2D NodeGraphPosition = ToVector2D(Panel.PanelCoordToGraphCoord(NodePanelPosition));
	return IsFinite(NodeGraphPosition) ? TOptional<FVector2D>{ NodeGraphPosition } : TOptional<FVector2D>{};
}

[[nodiscard]]
TOptional<FVector2D> ResolveNodeSize(
	const UEdGraphNode& Node, const SGraphNode& NodeWidget, const FVector2D SlateSize, EGraphNodeSizeSource& OutSource
)
{
	if (IsUsableSize(SlateSize))
	{
		OutSource = EGraphNodeSizeSource::SlateGeometry;
		return SlateSize;
	}

	const FVector2D DesiredSize = ToVector2D(NodeWidget.GetDesiredSize());
	if (IsUsableSize(DesiredSize))
	{
		OutSource = EGraphNodeSizeSource::DesiredSize;
		return DesiredSize;
	}

	const FVector2D PersistedSize{ static_cast<double>(Node.NodeWidth), static_cast<double>(Node.NodeHeight) };
	if (IsUsableSize(PersistedSize))
	{
		OutSource = EGraphNodeSizeSource::PersistedNode;
		return PersistedSize;
	}

	OutSource = EGraphNodeSizeSource::Unavailable;
	return {};
}

[[nodiscard]]
TOptional<FVector2D> ResolvePersistedNodeSize(const UEdGraphNode& Node) noexcept
{
	const FVector2D Size{ static_cast<double>(Node.NodeWidth), static_cast<double>(Node.NodeHeight) };
	return IsUsableSize(Size) ? TOptional<FVector2D>{ Size } : TOptional<FVector2D>{};
}

void CapturePin(
	FGraphGeometrySnapshot& Snapshot,
	const UEdGraphNode& Node,
	const SGraphNode& NodeWidget,
	const FGeometry& NodeGeometry,
	const FVector2D NodePosition,
	const FVector2D NodeSize,
	UEdGraphPin* Pin
)
{
	if (Pin == nullptr)
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::NullPin,
			EGraphGeometryDiagnosticSeverity::Error,
			&Node,
			nullptr,
			FString::Printf(TEXT("Node '%s' contains a null pin."), *Node.GetName())
		);
		return;
	}

	const TSharedPtr<SGraphPin> PinWidget = NodeWidget.FindWidgetForPin(Pin);
	if (!PinWidget.IsValid())
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::MissingPinWidget,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			Pin,
			FString::Printf(TEXT("No Slate widget is available for pin '%s'."), *Pin->PinName.ToString())
		);
		return;
	}

	if (!PinWidget->GetVisibility().IsVisible())
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::PinNotVisible,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			Pin,
			FString::Printf(TEXT("Pin '%s' is not visible and has no visual anchor."), *Pin->PinName.ToString())
		);
		return;
	}

	++Snapshot.RequestedVisiblePinCount;
	const FGeometry& PinGeometry = PinWidget->GetTickSpaceGeometry();
	const FVector2D PinSize = ToVector2D(PinGeometry.GetLocalSize());
	if (!IsUsableSize(PinSize))
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::PinGeometryUnavailable,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			Pin,
			FString::Printf(TEXT("Pin '%s' has missing or zero tick geometry."), *Pin->PinName.ToString())
		);
		return;
	}

	const float HorizontalCoordinate = Pin->Direction == EGPD_Input ? 0.0f : 1.0f;
	const FVector2f PinAnchorAbsolute =
		PinGeometry.GetAbsolutePositionAtCoordinates(FVector2f{ HorizontalCoordinate, 0.5f });
	const FVector2D NodeOffset = ToVector2D(NodeGeometry.AbsoluteToLocal(PinAnchorAbsolute));
	if (!IsFinite(NodeOffset) || !IsInsideNodeAllowance(NodeOffset, NodeSize))
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::PinAnchorOutsideNode,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			Pin,
			FString::Printf(TEXT("Pin '%s' tick geometry is stale or outside its node."), *Pin->PinName.ToString())
		);
		return;
	}

	FGraphPinGeometrySnapshot PinSnapshot;
	PinSnapshot.PinId = Pin->PinId;
	PinSnapshot.NodeGuid = Node.NodeGuid;
	PinSnapshot.NodeOffset = NodeOffset;
	PinSnapshot.Anchor = NodePosition + NodeOffset;
	Snapshot.Pins.Add(Pin, MoveTemp(PinSnapshot));
	++Snapshot.CapturedPinCount;
}

void CaptureNode(
	FGraphGeometrySnapshot& Snapshot,
	const SGraphPanel& Panel,
	const FGeometry& PanelGeometry,
	const bool bPanelGeometryUsable,
	UEdGraphNode& Node
)
{
	FGraphNodeGeometrySnapshot NodeSnapshot;
	NodeSnapshot.NodeGuid = Node.NodeGuid;
	NodeSnapshot.PersistedPosition = FVector2D{ static_cast<double>(Node.NodePosX), static_cast<double>(Node.NodePosY) };
	NodeSnapshot.Position = NodeSnapshot.PersistedPosition;

	if (!Node.NodeGuid.IsValid())
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::InvalidNodeGuid,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			nullptr,
			FString::Printf(TEXT("Node '%s' has no valid GUID."), *Node.GetName())
		);
	}

	const TSharedPtr<SGraphNode> NodeWidget = Panel.GetNodeWidgetFromGuid(Node.NodeGuid);
	if (!NodeWidget.IsValid())
	{
		const TOptional<FVector2D> PersistedSize = ResolvePersistedNodeSize(Node);
		if (PersistedSize.IsSet())
		{
			NodeSnapshot.Bounds = FSlateRect::FromPointAndExtent(NodeSnapshot.Position, PersistedSize.GetValue());
			NodeSnapshot.SizeSource = EGraphNodeSizeSource::PersistedNode;
			++Snapshot.CapturedNodeBoundsCount;
		}
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::MissingNodeWidget,
			PersistedSize.IsSet() ? EGraphGeometryDiagnosticSeverity::Info : EGraphGeometryDiagnosticSeverity::Retryable,
			&Node,
			nullptr,
			FString::Printf(TEXT("No Slate widget is available for node '%s'."), *Node.GetName())
		);
		Snapshot.Nodes.Add(&Node, MoveTemp(NodeSnapshot));
		return;
	}

	if (NodeWidget->GetNodeObj() != &Node)
	{
		const TOptional<FVector2D> PersistedSize = ResolvePersistedNodeSize(Node);
		if (PersistedSize.IsSet())
		{
			NodeSnapshot.Bounds = FSlateRect::FromPointAndExtent(NodeSnapshot.Position, PersistedSize.GetValue());
			NodeSnapshot.SizeSource = EGraphNodeSizeSource::PersistedNode;
			++Snapshot.CapturedNodeBoundsCount;
		}
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::NodeWidgetMismatch,
			PersistedSize.IsSet() ? EGraphGeometryDiagnosticSeverity::Info : EGraphGeometryDiagnosticSeverity::Retryable,
			&Node,
			nullptr,
			FString::Printf(TEXT("The Slate widget for node '%s' references another graph node."), *Node.GetName())
		);
		Snapshot.Nodes.Add(&Node, MoveTemp(NodeSnapshot));
		return;
	}

	const FGeometry& NodeGeometry = NodeWidget->GetTickSpaceGeometry();
	const FVector2D SlateSize = ToVector2D(NodeGeometry.GetLocalSize());
	const TOptional<FVector2D> SlatePosition =
		ResolveSlateGraphPosition(Panel, PanelGeometry, NodeGeometry, bPanelGeometryUsable);
	if (SlatePosition.IsSet() && SlatePosition.GetValue().Equals(NodeSnapshot.PersistedPosition, PositionAgreementTolerance))
	{
		NodeSnapshot.Position = SlatePosition.GetValue();
		NodeSnapshot.PositionSource = EGraphNodePositionSource::SlateGeometry;
	}
	else
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::NodePositionFallback,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			nullptr,
			FString::Printf(TEXT("Node '%s' is using its persisted NodePosX/Y position."), *Node.GetName())
		);
	}

	const TOptional<FVector2D> NodeSize = ResolveNodeSize(Node, *NodeWidget, SlateSize, NodeSnapshot.SizeSource);
	if (!NodeSize.IsSet())
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::NodeGeometryUnavailable,
			EGraphGeometryDiagnosticSeverity::Retryable,
			&Node,
			nullptr,
			FString::Printf(TEXT("Node '%s' has no usable Slate, desired, or persisted size."), *Node.GetName())
		);
		Snapshot.Nodes.Add(&Node, MoveTemp(NodeSnapshot));
		return;
	}

	if (NodeSnapshot.SizeSource != EGraphNodeSizeSource::SlateGeometry)
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::NodeSizeFallback,
			EGraphGeometryDiagnosticSeverity::Info,
			&Node,
			nullptr,
			FString::Printf(TEXT("Node '%s' is using a fallback size."), *Node.GetName())
		);
	}

	NodeSnapshot.Bounds = FSlateRect::FromPointAndExtent(NodeSnapshot.Position, NodeSize.GetValue());
	++Snapshot.CapturedNodeBoundsCount;

	const bool bNodeTickGeometryUsable = IsUsableSize(SlateSize);
	if (bNodeTickGeometryUsable)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			CapturePin(Snapshot, Node, *NodeWidget, NodeGeometry, NodeSnapshot.Position, NodeSize.GetValue(), Pin);
		}
	}

	Snapshot.Nodes.Add(&Node, MoveTemp(NodeSnapshot));
}
} // namespace

const TCHAR* DescribeGraphGeometryDiagnostic(const EGraphGeometryDiagnosticCode Code) noexcept
{
	switch (Code)
	{
		case EGraphGeometryDiagnosticCode::MissingPanelGraph:
			return TEXT("The graph panel has no graph object");
		case EGraphGeometryDiagnosticCode::PanelGeometryUnavailable:
			return TEXT("The graph panel has missing or zero tick geometry");
		case EGraphGeometryDiagnosticCode::NullNode:
			return TEXT("The capture request contains a null node");
		case EGraphGeometryDiagnosticCode::NodeOutsidePanelGraph:
			return TEXT("The requested node does not belong to the panel graph");
		case EGraphGeometryDiagnosticCode::InvalidNodeGuid:
			return TEXT("The requested node has no valid GUID");
		case EGraphGeometryDiagnosticCode::MissingNodeWidget:
			return TEXT("The requested node has no Slate widget");
		case EGraphGeometryDiagnosticCode::NodeWidgetMismatch:
			return TEXT("The node GUID resolved to a widget for another node");
		case EGraphGeometryDiagnosticCode::NodePositionFallback:
			return TEXT("The node is using its persisted position");
		case EGraphGeometryDiagnosticCode::NodeSizeFallback:
			return TEXT("The node is using a fallback size");
		case EGraphGeometryDiagnosticCode::NodeGeometryUnavailable:
			return TEXT("The node has no usable size");
		case EGraphGeometryDiagnosticCode::NullPin:
			return TEXT("The node contains a null pin");
		case EGraphGeometryDiagnosticCode::MissingPinWidget:
			return TEXT("The pin has no Slate widget");
		case EGraphGeometryDiagnosticCode::PinNotVisible:
			return TEXT("The pin is not visible");
		case EGraphGeometryDiagnosticCode::PinGeometryUnavailable:
			return TEXT("The pin has missing or zero tick geometry");
		case EGraphGeometryDiagnosticCode::PinAnchorOutsideNode:
			return TEXT("The pin anchor is stale or outside its node");
	}
	return TEXT("Unknown graph geometry diagnostic");
}

FString FGraphGeometryDiagnostic::ToString() const
{
	const FString Description{ DescribeGraphGeometryDiagnostic(Code) };
	return Detail.IsEmpty() ? Description : FString::Printf(TEXT("%s: %s"), *Description, *Detail);
}

FGraphGeometrySnapshot FGraphGeometrySnapshot::Capture(const SGraphPanel& Panel)
{
	const UEdGraph* const Graph = Panel.GetGraphObj();
	if (Graph == nullptr)
	{
		FGraphGeometrySnapshot Snapshot;
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::MissingPanelGraph,
			EGraphGeometryDiagnosticSeverity::Error,
			nullptr,
			nullptr,
			TEXT("Cannot capture graph geometry without a graph object.")
		);
		return Snapshot;
	}

	TArray<UEdGraphNode*> GraphNodes;
	GraphNodes.Reserve(Graph->Nodes.Num());
	for (const TObjectPtr<UEdGraphNode>& Node : Graph->Nodes)
	{
		GraphNodes.Add(Node.Get());
	}
	return Capture(Panel, GraphNodes);
}

FGraphGeometrySnapshot FGraphGeometrySnapshot::Capture(const SGraphPanel& Panel, const TConstArrayView<UEdGraphNode*> NodesToCapture)
{
	FGraphGeometrySnapshot Snapshot;
	Snapshot.RequestedNodeCount = NodesToCapture.Num();

	const UEdGraph* const PanelGraph = Panel.GetGraphObj();
	if (PanelGraph == nullptr)
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::MissingPanelGraph,
			EGraphGeometryDiagnosticSeverity::Error,
			nullptr,
			nullptr,
			TEXT("Cannot capture graph geometry without a graph object.")
		);
		return Snapshot;
	}

	const FGeometry& PanelGeometry = Panel.GetTickSpaceGeometry();
	const bool bPanelGeometryUsable = IsUsableSize(ToVector2D(PanelGeometry.GetLocalSize()));
	if (!bPanelGeometryUsable)
	{
		AddDiagnostic(
			Snapshot,
			EGraphGeometryDiagnosticCode::PanelGeometryUnavailable,
			EGraphGeometryDiagnosticSeverity::Retryable,
			nullptr,
			nullptr,
			TEXT("Capture may be retried after the graph panel receives a Slate tick.")
		);
	}

	TSet<const UEdGraphNode*> VisitedNodes;
	for (UEdGraphNode* Node : NodesToCapture)
	{
		if (Node == nullptr)
		{
			AddDiagnostic(
				Snapshot,
				EGraphGeometryDiagnosticCode::NullNode,
				EGraphGeometryDiagnosticSeverity::Error,
				nullptr,
				nullptr,
				TEXT("The node list passed to Capture contains a null entry.")
			);
			continue;
		}
		if (VisitedNodes.Contains(Node)) { continue; }
		VisitedNodes.Add(Node);
		if (Node->GetGraph() != PanelGraph)
		{
			AddDiagnostic(
				Snapshot,
				EGraphGeometryDiagnosticCode::NodeOutsidePanelGraph,
				EGraphGeometryDiagnosticSeverity::Error,
				Node,
				nullptr,
				FString::Printf(TEXT("Node '%s' belongs to another graph."), *Node->GetName())
			);
			continue;
		}

		CaptureNode(Snapshot, Panel, PanelGeometry, bPanelGeometryUsable, *Node);
	}
	return Snapshot;
}

const FGraphNodeGeometrySnapshot* FGraphGeometrySnapshot::FindNode(const UEdGraphNode* Node) const noexcept
{ return Nodes.Find(Node); }

const FGraphPinGeometrySnapshot* FGraphGeometrySnapshot::FindPin(const UEdGraphPin* Pin) const noexcept
{ return Pins.Find(Pin); }
} // namespace GraphFormatter::K2
