/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "Benchmark/GraphFormatterBenchmarkMetrics.h"
#include "CoreMinimal.h"
#include "K2/GraphGeometrySnapshot.h"
#include "UObject/GCObject.h"

class SGraphEditor;
class UBlueprint;
class UEdGraph;

namespace GraphFormatter::Benchmark
{
enum class EFormatterBackend : uint8
{
	GraphFormatter,
	BlueprintAutoLayout,
	GraphFormatterLibavoid,
	ElkLayered,
};

[[nodiscard]]
const TCHAR* DescribeBackend(EFormatterBackend Backend) noexcept;

struct FBenchmarkCandidate
{
	EFormatterBackend Backend = EFormatterBackend::GraphFormatter;
	FString BlindLabel;
	FString DisplayName;
	FString Description;
	TObjectPtr<UBlueprint> Blueprint;
	UEdGraph* Graph = nullptr;
	K2::FGraphGeometrySnapshot Geometry;
	FGraphQualityMetrics Metrics;
	double DurationMilliseconds = 0.0;
	bool bSucceeded = false;
	bool bTopologyPreserved = false;
	/** False when rendered metrics show a hard readability regression relative to the original. */
	bool bReadabilitySafetyPassed = true;
	FString ScreenshotFilename;
	FIntPoint ScreenshotSize = FIntPoint::ZeroValue;
	TMap<FString, FString> Configuration;
	TMap<FString, double> NumericConfiguration;
	TMap<FString, double> Telemetry;
	TArray<FString> Diagnostics;

	[[nodiscard]]
	bool IsValid() const noexcept
	{ return bSucceeded && bTopologyPreserved && bReadabilitySafetyPassed; }
};

/** Owns read-only transient Blueprint copies for one source-preserving, blinded comparison run. */
class FGraphFormatterBenchmarkRun final
	: public FGCObject
	, public TSharedFromThis<FGraphFormatterBenchmarkRun>
{
public:
	FString RunId;
	FString SourceAssetPath;
	FString SourceGraphName;
	FString ReportDirectory;
	TObjectPtr<UBlueprint> OriginalBlueprint;
	UEdGraph* OriginalGraph = nullptr;
	K2::FGraphGeometrySnapshot OriginalGeometry;
	FGraphQualityMetrics OriginalMetrics;
	FString OriginalScreenshotFilename;
	FIntPoint OriginalScreenshotSize = FIntPoint::ZeroValue;
	TArray<FBenchmarkCandidate> Candidates;
	TArray<FString> Diagnostics;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

	[[nodiscard]]
	bool SaveBallot(const TMap<FString, TSet<FString>>& Choices, bool bMappingRevealed, FString& OutError) const;

	[[nodiscard]]
	bool SaveManifest(FString& OutError) const;

	/** Recaptures the original and every candidate pane, then recomputes metrics from real Slate geometry. */
	[[nodiscard]]
	bool RefreshRenderedResults(TConstArrayView<TSharedPtr<SGraphEditor>> GraphEditors, FString& OutError);
};

class FGraphFormatterBenchmark
{
public:
	/** Opens a blinded original plus A/B/C/D judge window. The source Blueprint is never modified. */
	[[nodiscard]]
	static bool Open(SGraphEditor& SourceEditor, UObject& ContextObject, FString& OutError);

	/** Creates the transient run without Slate UI; used by automation tests. */
	[[nodiscard]]
	static TSharedPtr<FGraphFormatterBenchmarkRun> CreateRun(SGraphEditor& SourceEditor, UObject& ContextObject, FString& OutError);

	/** Runs the same backends from supplied headless geometry without writing report files. */
	[[nodiscard]]
	static TSharedPtr<FGraphFormatterBenchmarkRun> CreateHeadlessRun(
		UBlueprint& SourceBlueprint, UEdGraph& SourceGraph, const K2::FGraphGeometrySnapshot& SourceGeometry, FString& OutError
	);

	/** Rejects misleading benchmark snapshots while allowing disconnected test graphs to have no pin anchors. */
	[[nodiscard]]
	static bool ValidateGeometryForComparison(const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, FString& OutError);
};
} // namespace GraphFormatter::Benchmark
