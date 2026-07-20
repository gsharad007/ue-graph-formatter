/*---------------------------------------------------------------------------------------------
 * Copyright (c) Howaajin. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace GraphFormatter::K2
{
struct FRerouteSettings
{
	double ObstacleClearance = 48.0;
	double ChannelSpacing = 32.0;
	int32 MaxKnotsPerWire = 6;
	int32 LongDataWireRankThreshold = 3;
	/** Maximum primitive geometry comparisons performed by one deterministic routing pass. */
	int32 PlanningWorkBudget = 1000000;
	bool bRouteDataWires = true;
};

struct FRerouteEdge
{
	UEdGraphPin* OutputPin = nullptr;
	UEdGraphPin* InputPin = nullptr;
	FVector2D OutputAnchor = FVector2D::ZeroVector;
	FVector2D InputAnchor = FVector2D::ZeroVector;
	FString StableKey;
	/** Optional engine-independent layout-core route, excluding the two real pin anchors. */
	TArray<FVector2D> PreferredWaypoints;
	int32 RankSpan = 0;
	bool bExecution = false;
	/** A previously generated logical chain supplied only to reserve its existing polyline. */
	bool bExistingGeneratedRoute = false;
};

struct FRerouteObstacle
{
	UEdGraphNode* Node = nullptr;
	FBox2D Bounds = FBox2D(EForceInit::ForceInit);
};

struct FRerouteResult
{
	int32 RoutedWires = 0;
	int32 CreatedKnots = 0;
	int32 SkippedWires = 0;
	/** True only if a failed replacement could not restore the exact original direct pin link. */
	bool bTopologyRestorationFailed = false;
	/** True when the deterministic geometry-comparison budget left the remaining wires unchanged. */
	bool bPlanningBudgetExhausted = false;
	TArray<FString> Diagnostics;

	bool WasModified() const { return CreatedKnots > 0; }
	bool HasFatalError() const { return bTopologyRestorationFailed; }
};

/**
 * Adds standard K2 knot nodes only after a node layout has been accepted. The router is
 * deliberately topology-conservative: it operates on direct links whose real endpoints are
 * both in scope, stages every knot chain before replacing the boundary link, and restores the
 * original link if any schema operation fails.
 */
class FK2RerouteRouter
{
public:
	/** Returns true only for reroute nodes created and tagged by this formatter. */
	static bool IsGeneratedRerouteNode(const UEdGraphNode* Node);
	/** Reads the non-empty logical-edge identity from a tagged generated reroute node. */
	static bool TryGetGeneratedRouteKey(const UEdGraphNode* Node, FString& OutLogicalEdgeKey);

	/** Returns tagged knot nodes and centers in validated route-ordinal order for one logical edge. */
	static bool FindGeneratedRoute(
		UEdGraph& Graph, FStringView LogicalEdgeKey, TArray<UEdGraphNode*>& OutKnots, TArray<FVector2D>& OutWaypoints
	);

	static FRerouteResult Route(
		UEdGraph& Graph,
		TConstArrayView<FRerouteEdge> Edges,
		TConstArrayView<FRerouteObstacle> Obstacles,
		const TSet<UEdGraphNode*>& Scope,
		const FRerouteSettings& Settings,
		double GridSize
	);
};
} // namespace GraphFormatter::K2
