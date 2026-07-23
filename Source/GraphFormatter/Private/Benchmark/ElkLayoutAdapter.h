/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UFormatterSettings;

namespace GraphFormatter::K2
{
struct FGraphGeometrySnapshot;
}

namespace GraphFormatter::Benchmark
{
inline constexpr const TCHAR* ElkJsVersion = TEXT("0.12.0");

struct FElkLayoutResult
{
	bool bSucceeded = false;
	int32 PositionedNodes = 0;
	int32 SubmittedPorts = 0;
	int32 MeasuredPorts = 0;
	int32 SubmittedEdges = 0;
	int32 EdgesWithRoutes = 0;
	int32 MaterializedRoutes = 0;
	int32 CreatedKnots = 0;
	int32 KnotLimitRejections = 0;
	FString NodeExecutable;
	TArray<FString> Diagnostics;
};

/**
 * Runs the unmodified elkjs bundle out of process and applies its node positions and orthogonal
 * edge sections to a transient comparison graph. An empty artifact directory uses disposable
 * Intermediate files; a non-empty directory preserves elk_input.json/output/process artifacts.
 */
[[nodiscard]]
FElkLayoutResult RunElkLayeredLayout(
	UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, const UFormatterSettings& Settings, const FString& ArtifactDirectory
);
} // namespace GraphFormatter::Benchmark
