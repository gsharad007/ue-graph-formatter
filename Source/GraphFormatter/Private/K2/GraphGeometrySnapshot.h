/*---------------------------------------------------------------------------------------------
 * Copyright (c) Howaajin. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Layout/SlateRect.h"

class SGraphPanel;
class UEdGraphNode;
class UEdGraphPin;

namespace GraphFormatter::K2
{
enum class EGraphGeometrySnapshotStatus : uint8
{
	Ready,
	NeedsRetry,
	InvalidInput,
};

enum class EGraphGeometryDiagnosticSeverity : uint8
{
	Info,
	Retryable,
	Error,
};

enum class EGraphGeometryDiagnosticCode : uint8
{
	MissingPanelGraph,
	PanelGeometryUnavailable,
	NullNode,
	NodeOutsidePanelGraph,
	InvalidNodeGuid,
	MissingNodeWidget,
	NodeWidgetMismatch,
	NodePositionFallback,
	NodeSizeFallback,
	NodeGeometryUnavailable,
	NullPin,
	MissingPinWidget,
	PinNotVisible,
	PinGeometryUnavailable,
	PinAnchorOutsideNode,
};

[[nodiscard]]
const TCHAR* DescribeGraphGeometryDiagnostic(EGraphGeometryDiagnosticCode Code) noexcept;

struct FGraphGeometryDiagnostic
{
	EGraphGeometryDiagnosticCode Code = EGraphGeometryDiagnosticCode::PanelGeometryUnavailable;
	EGraphGeometryDiagnosticSeverity Severity = EGraphGeometryDiagnosticSeverity::Info;
	FGuid NodeGuid;
	FGuid PinId;
	FString Detail;

	[[nodiscard]]
	FString ToString() const;
};

enum class EGraphNodePositionSource : uint8
{
	SlateGeometry,
	PersistedNode,
};

enum class EGraphNodeSizeSource : uint8
{
	SlateGeometry,
	DesiredSize,
	PersistedNode,
	Unavailable,
};

struct FGraphNodeGeometrySnapshot
{
	FGuid NodeGuid;
	FVector2D PersistedPosition = FVector2D::ZeroVector;
	FVector2D Position = FVector2D::ZeroVector;
	TOptional<FSlateRect> Bounds;
	EGraphNodePositionSource PositionSource = EGraphNodePositionSource::PersistedNode;
	EGraphNodeSizeSource SizeSource = EGraphNodeSizeSource::Unavailable;

	[[nodiscard]]
	bool HasBounds() const noexcept
	{ return Bounds.IsSet(); }
};

struct FGraphPinGeometrySnapshot
{
	FGuid PinId;
	FGuid NodeGuid;
	FVector2D NodeOffset = FVector2D::ZeroVector;
	FVector2D Anchor = FVector2D::ZeroVector;
};

/**
 * Read-only graph-space geometry captured from the last Slate tick.
 *
 * Capture never runs a prepass, ticks widgets, changes zoom, or changes the panel view. Persisted node positions,
 * desired/persisted sizes, and deterministic adapter pin offsets are valid fallbacks for off-screen nodes. Missing
 * panel geometry or a node with no measurable/persisted size requests one normal Slate-tick retry and then causes an
 * atomic no-op instead of risking overlap from a guessed node rectangle.
 */
struct FGraphGeometrySnapshot
{
	EGraphGeometrySnapshotStatus Status = EGraphGeometrySnapshotStatus::Ready;
	TMap<const UEdGraphNode*, FGraphNodeGeometrySnapshot> Nodes;
	TMap<const UEdGraphPin*, FGraphPinGeometrySnapshot> Pins;
	TArray<FGraphGeometryDiagnostic> Diagnostics;
	int32 RequestedNodeCount = 0;
	int32 CapturedNodeBoundsCount = 0;
	int32 RequestedVisiblePinCount = 0;
	int32 CapturedPinCount = 0;

	[[nodiscard]]
	static FGraphGeometrySnapshot Capture(const SGraphPanel& Panel);

	[[nodiscard]]
	static FGraphGeometrySnapshot Capture(const SGraphPanel& Panel, TConstArrayView<UEdGraphNode*> NodesToCapture);

	[[nodiscard]]
	bool IsReady() const noexcept
	{ return Status == EGraphGeometrySnapshotStatus::Ready; }

	[[nodiscard]]
	bool ShouldRetry() const noexcept
	{ return Status == EGraphGeometrySnapshotStatus::NeedsRetry; }

	[[nodiscard]]
	const FGraphNodeGeometrySnapshot* FindNode(const UEdGraphNode* Node) const noexcept;

	[[nodiscard]]
	const FGraphPinGeometrySnapshot* FindPin(const UEdGraphPin* Pin) const noexcept;
};
} // namespace GraphFormatter::K2
