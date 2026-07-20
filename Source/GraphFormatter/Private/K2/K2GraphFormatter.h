/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"

class SGraphEditor;
class UEdGraph;
class UEdGraphNode;
class UFormatterSettings;

namespace GraphFormatter::K2
{
struct FGraphGeometrySnapshot;

enum class EK2FormatStatus : uint8
{
	Formatted,
	NoChanges,
	UnsupportedGraph,
	InvalidGeometry,
	LayoutFailed,
};

struct FK2FormatResult
{
	EK2FormatStatus Status = EK2FormatStatus::NoChanges;
	int32 MovedNodeCount = 0;
	int32 ResizedCommentCount = 0;
	int32 RoutedWireCount = 0;
	int32 CreatedKnotCount = 0;
	int32 SkippedRerouteWireCount = 0;
	FString Message;
	TArray<FString> Diagnostics;

	[[nodiscard]]
	bool WasModified() const noexcept
	{ return MovedNodeCount > 0 || ResizedCommentCount > 0 || CreatedKnotCount > 0; }
};

/** Bridges measured Blueprint graph state to the deterministic, engine-independent K2 layout core. */
class FK2GraphFormatter
{
public:
	[[nodiscard]]
	static bool CanFormat(const UEdGraph& Graph, const TSet<UEdGraphNode*>& Scope);

	[[nodiscard]]
	static FK2FormatResult Format(
		SGraphEditor& GraphEditor,
		UEdGraph& Graph,
		const FGraphGeometrySnapshot& Geometry,
		const TSet<UEdGraphNode*>& Scope,
		bool bRouteWires,
		const UFormatterSettings& Settings
	);
};
} // namespace GraphFormatter::K2
