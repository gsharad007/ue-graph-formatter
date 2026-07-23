/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"

class UEdGraph;

namespace GraphFormatter::K2
{
struct FGraphGeometrySnapshot;
}

namespace GraphFormatter::Benchmark
{
struct FGraphBenchmarkBaseline
{
	TMap<FGuid, FVector2D> NodePositions;
	int32 RerouteNodeCount = 0;
};

/** Lower is better for every penalty field and for CompositePenalty. */
struct FGraphQualityMetrics
{
	int32 SemanticNodeCount = 0;
	int32 RerouteNodeCount = 0;
	int32 AddedRerouteNodeCount = 0;
	int32 MovedSemanticNodeCount = 0;
	int32 MovedRerouteNodeCount = 0;
	int32 NodeOverlapCount = 0;
	int32 BackwardExecutionEdgeCount = 0;
	int32 BackwardDataEdgeCount = 0;
	int32 NonStraightPrimaryExecutionEdgeCount = 0;
	int32 ExecutionCrossingCount = 0;
	int32 DataCrossingCount = 0;
	int32 WireThroughNodeCount = 0;
	int32 InsufficientExecutionGapCount = 0;
	int32 OffGridXCount = 0;
	int32 OffGridYCount = 0;
	int32 LogicalWireCount = 0;
	int32 BendCount = 0;
	double BackwardDataDistance = 0.0;
	double PrimaryExecutionVerticalError = 0.0;
	double TotalRenderedWireLength = 0.0;
	double DrawingArea = 0.0;
	double TotalNodeMovement = 0.0;
	double MaximumNodeMovement = 0.0;
	double TotalRerouteNodeMovement = 0.0;
	double MaximumRerouteNodeMovement = 0.0;
	double CompositePenalty = 0.0;
};

[[nodiscard]]
FGraphBenchmarkBaseline CaptureBaseline(const UEdGraph& Graph);

[[nodiscard]]
FGraphQualityMetrics MeasureGraphQuality(
	const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, const FGraphBenchmarkBaseline& Baseline, double GridSize
);
} // namespace GraphFormatter::Benchmark
