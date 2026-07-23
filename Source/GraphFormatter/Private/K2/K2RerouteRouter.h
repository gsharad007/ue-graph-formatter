/*---------------------------------------------------------------------------------------------
 * Copyright (c) Howaajin. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace GraphFormatter::K2
{
inline constexpr double RerouteKnotWidth = 42.0;
inline constexpr double RerouteKnotHeight = 24.0;

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
	/** Validate the preferred polyline exactly; do not silently replace an unsafe external proposal with a native route. */
	bool bValidatePreferredWaypointsOnly = false;
	int32 RankSpan = 0;
	bool bExecution = false;
	/** Stock Kismet reverses endpoint tangents when a user knot points back toward its input side. */
	bool bReverseOutputTangent = false;
	bool bReverseInputTangent = false;
	/** A previously generated logical chain supplied only to reserve its existing polyline. */
	bool bExistingGeneratedRoute = false;
	/** An ordinary stationary wire supplied as routing context; reserve it without reporting a skip. */
	bool bReservationOnly = false;
};

struct FRerouteObstacle
{
	UEdGraphNode* Node = nullptr;
	FBox2D Bounds = FBox2D(EForceInit::ForceInit);
};

struct FPlannedReroute
{
	FRerouteEdge Edge;
	TArray<FVector2D> Waypoints;
	/** Stable endpoint identities captured while planning, before any route can reconstruct a K2 pin. */
	TWeakObjectPtr<UEdGraphNode> OutputNode;
	TWeakObjectPtr<UEdGraphNode> InputNode;
	FGuid OutputPinId;
	FGuid InputPinId;
};

/** A deterministic, non-mutating routing decision that can be inspected before graph changes are committed. */
struct FReroutePlan
{
	TArray<FPlannedReroute> Routes;
	/** Stable keys whose direct baseline had a routing defect, even when no safe route was found. */
	TArray<FString> RequiredRouteKeys;
	int32 SkippedWires = 0;
	bool bPlanningBudgetExhausted = false;
	TArray<FString> Diagnostics;
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
	/**
	 * Reproduces the stock Kismet spline approximation used by routing from pin anchors and
	 * optional logical knot centers. Readability validation uses the same geometry so accepted
	 * layouts cannot disagree with the router about a wire passing through a node.
	 */
	static TArray<FVector2D> BuildRenderedPolyline(
		TConstArrayView<FVector2D> LogicalPoints, bool bReverseFirstTangent = false, bool bReverseLastTangent = false
	);
	/** Returns true when two already-rendered polyline approximations touch, overlap, or cross. */
	static bool RenderedPolylinesIntersect(TConstArrayView<FVector2D> FirstPolyline, TConstArrayView<FVector2D> SecondPolyline);
	/**
	 * As above, but ignores a non-overlapping touch only when it occurs at an explicitly supplied
	 * point that is also a terminal of both polylines. This mirrors the Kismet router's shared-pin
	 * exception without hiding a later recrossing or a shared initial/final segment.
	 */
	static bool RenderedPolylinesIntersectExceptAtSharedTerminals(
		TConstArrayView<FVector2D> FirstPolyline,
		TConstArrayView<FVector2D> SecondPolyline,
		TConstArrayView<FVector2D> IgnoredSharedTerminals
	);

	/** Returns true only for reroute nodes created and tagged by this formatter. */
	static bool IsGeneratedRerouteNode(const UEdGraphNode* Node);
	/** Reads the non-empty logical-edge identity from a tagged generated reroute node. */
	static bool TryGetGeneratedRouteKey(const UEdGraphNode* Node, FString& OutLogicalEdgeKey);

	/** Returns tagged knot nodes and centers in validated route-ordinal order for one logical edge. */
	static bool FindGeneratedRoute(
		UEdGraph& Graph, FStringView LogicalEdgeKey, TArray<UEdGraphNode*>& OutKnots, TArray<FVector2D>& OutWaypoints
	);

	/** Plans every required route without modifying graph nodes, links, metadata, transactions, or package state. */
	static FReroutePlan Plan(
		TConstArrayView<FRerouteEdge> Edges,
		TConstArrayView<FRerouteObstacle> Obstacles,
		const TSet<UEdGraphNode*>& Scope,
		const FRerouteSettings& Settings,
		double GridSize
	);

	/** Applies a previously inspected plan. Each failed route restores its original direct link before continuing. */
	static FRerouteResult ApplyPlan(UEdGraph& Graph, const FReroutePlan& Plan);

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
