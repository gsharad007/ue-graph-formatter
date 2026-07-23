/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/ISlateStyle.h"
#include "FormatterStyle.h"

class FFormatterCommands : public TCommands<FFormatterCommands>
{
public:
	FFormatterCommands()
		: TCommands<FFormatterCommands>(
			  "GraphFormatterEditor",
			  NSLOCTEXT("Contexts", "GraphFormatterEditor", "Grap Formatter Editor"),
			  NAME_None,
			  FFormatterStyle::Get()->GetStyleSetName()
		  )
	{
	}

	TSharedPtr<FUICommandInfo> FormatGraph;
	TSharedPtr<FUICommandInfo> FormatGraphWithRouting;
	// Runs only against transient Blueprint copies; the active graph remains untouched.
	TSharedPtr<FUICommandInfo> CompareFormatters;
	TSharedPtr<FUICommandInfo> StraightenConnections;
	TSharedPtr<FUICommandInfo> PlaceBlock;
	virtual void RegisterCommands() override;
};
