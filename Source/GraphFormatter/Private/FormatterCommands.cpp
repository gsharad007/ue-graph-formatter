/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "FormatterCommands.h"

#define LOCTEXT_NAMESPACE "GraphFormatter"

void FFormatterCommands::RegisterCommands()
{
	UI_COMMAND(
		FormatGraph,
		"Format Graph",
		"Format this graph with the semantic K2 layout or deterministic generic fallback",
		EUserInterfaceActionType::Button,
		FInputChord()
	);
	UI_COMMAND(
		FormatGraphWithRouting,
		"Format + Route Wires",
		"Format the graph and add standard reroute nodes only where wires remain crossing, obstructed, backward, or "
		"unusually long",
		EUserInterfaceActionType::Button,
		FInputChord()
	);
	UI_COMMAND(
		StraightenConnections,
		"Flat Spline Rendering",
		"Toggle Unreal's global zero-tangent spline rendering; this does not move nodes or route wires",
		EUserInterfaceActionType::ToggleButton,
		FInputChord()
	);
	UI_COMMAND(
		PlaceBlock,
		"Place block",
		"Place selected nodes to appropriate position",
		EUserInterfaceActionType::Button,
		FInputChord(EKeys::E, true, false, false, false)
	);
}

#undef LOCTEXT_NAMESPACE
