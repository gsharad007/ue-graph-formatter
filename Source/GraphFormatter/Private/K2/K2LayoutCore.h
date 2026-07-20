/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Math/Vector2D.h"
#include "Misc/Crc.h"
#include "Templates/UnrealTemplate.h"

namespace GraphFormatter::K2Layout
{
/**
 * Stable keys are the only identity and tie-break information consumed by the layout core.
 * Callers should derive them from persistent graph and pin identifiers, never pointer values.
 */
struct FNodeKey
{
	FString Value;

	FNodeKey() = default;
	explicit FNodeKey(FString InValue)
		: Value(MoveTemp(InValue))
	{
	}

	[[nodiscard]]
	bool IsValid() const noexcept
	{ return !Value.IsEmpty(); }
	[[nodiscard]]
	friend bool operator==(const FNodeKey& A, const FNodeKey& B) noexcept
	{ return A.Value == B.Value; }
	[[nodiscard]]
	friend uint32 GetTypeHash(const FNodeKey& Key) noexcept
	{ return FCrc::StrCrc32(*Key.Value); }
};

struct FPortKey
{
	FString Value;

	FPortKey() = default;
	explicit FPortKey(FString InValue)
		: Value(MoveTemp(InValue))
	{
	}

	[[nodiscard]]
	bool IsValid() const noexcept
	{ return !Value.IsEmpty(); }
	[[nodiscard]]
	friend bool operator==(const FPortKey& A, const FPortKey& B) noexcept
	{ return A.Value == B.Value; }
	[[nodiscard]]
	friend uint32 GetTypeHash(const FPortKey& Key) noexcept
	{ return FCrc::StrCrc32(*Key.Value); }
};

struct FEdgeKey
{
	FString Value;

	FEdgeKey() = default;
	explicit FEdgeKey(FString InValue)
		: Value(MoveTemp(InValue))
	{
	}

	[[nodiscard]]
	bool IsValid() const noexcept
	{ return !Value.IsEmpty(); }
	[[nodiscard]]
	friend bool operator==(const FEdgeKey& A, const FEdgeKey& B) noexcept
	{ return A.Value == B.Value; }
	[[nodiscard]]
	friend uint32 GetTypeHash(const FEdgeKey& Key) noexcept
	{ return FCrc::StrCrc32(*Key.Value); }
};

enum class EPortDirection : uint8
{
	Input,
	Output,
};

enum class EPortKind : uint8
{
	Execution,
	Data,
};

struct FPortSnapshot
{
	FPortKey Key;
	EPortDirection Direction{ EPortDirection::Input };
	EPortKind Kind{ EPortKind::Data };
	FVector2D Offset{ FVector2D::ZeroVector };
	int32 SemanticOrder{ 0 };
	bool bPreferredExecutionPort{ false };
};

struct FNodeSnapshot
{
	FNodeKey Key;
	FVector2D Size{
		FVector2D{ 1.0, 1.0 }
	};
	TArray<FPortSnapshot> Ports;
	bool bIsPure{ false };
};

struct FPortReference
{
	FNodeKey Node;
	FPortKey Port;
};

struct FExecutionEdgeSnapshot
{
	FEdgeKey Key;
	FPortReference Source;
	FPortReference Target;
	int32 BranchOrder{ INDEX_NONE };
	bool bPreferredAlignment{ false };
};

struct FDataEdgeSnapshot
{
	FEdgeKey Key;
	FPortReference Source;
	FPortReference Target;
};

struct FGraphSnapshot
{
	TArray<FNodeSnapshot> Nodes;
	TArray<FExecutionEdgeSnapshot> ExecutionEdges;
	TArray<FDataEdgeSnapshot> DataEdges;
};

enum class EGridPolicy : uint8
{
	/** Do not quantize positions. */
	None,
	/** Snap every node top-left. Exact execution-pin alignment may be lost. */
	NodeGrid,
	/** Snap rank columns and alignment-block anchors, preserving exact execution-pin offsets within a block. */
	HybridExecution,
};

struct FLayoutSettings
{
	double HorizontalSpacing{ 192.0 };
	double VerticalSpacing{ 96.0 };
	/** Additional clearance between ordered sibling execution branches emitted by the same source node. */
	double BranchSpacing{ 96.0 };
	double PureNodeHorizontalSpacing{ 112.0 };
	double PureNodeVerticalSpacing{ 48.0 };
	double CollisionClearance{ 32.0 };
	FVector2D ComponentSpacing{
		FVector2D{ 320.0, 224.0 }
	};
	double ComponentRowWidth{ 4096.0 };
	FVector2D GridSize{
		FVector2D{ 16.0, 16.0 }
	};
	int32 OrderingSweeps{ 12 };
	int32 AdjacentSwapPasses{ 4 };
	/** Deterministic cap on expensive local crossing evaluations for interactive editor latency. */
	int32 AdjacentSwapEvaluationBudget{ 2048 };
	EGridPolicy GridPolicy{ EGridPolicy::HybridExecution };
};

enum class EDiagnosticSeverity : uint8
{
	Information,
	Warning,
	Error,
};

enum class EDiagnosticCode : uint8
{
	EmptyStableKey,
	DuplicateStableKey,
	InvalidNodeGeometry,
	MissingEndpointNode,
	MissingEndpointPort,
	EndpointPortMismatch,
	PureNodeHasExecutionEdge,
	ExecutionCycleCondensed,
	ContradictoryBranchOrder,
	OrderingBudgetExhausted,
	AlignmentRejectedCrossings,
	PureDataCycleFallback,
};

struct FLayoutDiagnostic
{
	EDiagnosticSeverity Severity{ EDiagnosticSeverity::Information };
	EDiagnosticCode Code{ EDiagnosticCode::EmptyStableKey };
	FString SubjectKey;
	FString Message;
};

struct FPlannedNodePosition
{
	FNodeKey Node;
	FVector2D Position{ FVector2D::ZeroVector };
	int32 ComponentIndex{ INDEX_NONE };
	int32 ExecutionRank{ INDEX_NONE };
	int32 OrderInRank{ INDEX_NONE };
};

/** Intermediate bend locations corresponding to virtual nodes introduced for a long execution edge. */
struct FPlannedEdgeRoute
{
	FEdgeKey Edge;
	TArray<FVector2D> Waypoints;
};

struct FLayoutStatistics
{
	int32 AcceptedNodeCount{ 0 };
	int32 AcceptedExecutionEdgeCount{ 0 };
	int32 AcceptedDataEdgeCount{ 0 };
	int32 ComponentCount{ 0 };
	int32 CondensedExecutionCycleCount{ 0 };
	int32 VirtualNodeCount{ 0 };
	int64 InitialExecutionCrossings{ 0 };
	int64 FinalExecutionCrossings{ 0 };
};

struct FLayoutPlan
{
	TArray<FPlannedNodePosition> Nodes;
	TArray<FPlannedEdgeRoute> ExecutionRoutes;
	TArray<FLayoutDiagnostic> Diagnostics;
	FLayoutStatistics Statistics;

	[[nodiscard]]
	bool HasErrors() const noexcept;
};

/**
 * Produces a deterministic plan without reading or mutating UObject/Slate state.
 * Invalid snapshot records are omitted and reported; valid disconnected records are still laid out.
 */
[[nodiscard]]
FLayoutPlan BuildLayout(const FGraphSnapshot& Snapshot, const FLayoutSettings& Settings = {});
} // namespace GraphFormatter::K2Layout
