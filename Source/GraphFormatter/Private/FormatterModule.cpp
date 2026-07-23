/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "CoreMinimal.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphEditor.h"
#include "GraphEditorSettings.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"
#include "SGraphPanel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "Formatter.h"
#include "Benchmark/GraphFormatterBenchmark.h"
#include "FormatterCommands.h"
#include "FormatterLog.h"
#include "FormatterSettings.h"
#include "FormatterStyle.h"

#define LOCTEXT_NAMESPACE "GraphFormatter"

namespace
{
struct FFormatterCommandContext
{
	TArray<TWeakObjectPtr<UObject>> Objects{};
};

struct FResolvedGraphTarget
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
};

TArray<TWeakPtr<FUICommandList>>& GetMappedCommandLists()
{
	static TArray<TWeakPtr<FUICommandList>> CommandLists;
	return CommandLists;
}

const TArray<TSharedPtr<EGraphFormatterPositioningAlgorithm>>& GetAlgorithmOptions()
{
	static const TArray<TSharedPtr<EGraphFormatterPositioningAlgorithm>> Options{
		MakeShared<EGraphFormatterPositioningAlgorithm>(EGraphFormatterPositioningAlgorithm::EEvenlyInLayer),
		MakeShared<EGraphFormatterPositioningAlgorithm>(EGraphFormatterPositioningAlgorithm::EFastAndSimpleMethodTop),
		MakeShared<EGraphFormatterPositioningAlgorithm>(EGraphFormatterPositioningAlgorithm::EFastAndSimpleMethodMedian),
		MakeShared<EGraphFormatterPositioningAlgorithm>(EGraphFormatterPositioningAlgorithm::ELayerSweep),
	};
	return Options;
}

FText GetAlgorithmName(const EGraphFormatterPositioningAlgorithm Algorithm)
{
	switch (Algorithm)
	{
		case EGraphFormatterPositioningAlgorithm::EEvenlyInLayer:
			return LOCTEXT("EvenlyInLayerAlgorithm", "Evenly in layer");
		case EGraphFormatterPositioningAlgorithm::EFastAndSimpleMethodTop:
			return LOCTEXT("FastAndSimpleTopAlgorithm", "FAS Top");
		case EGraphFormatterPositioningAlgorithm::EFastAndSimpleMethodMedian:
			return LOCTEXT("FastAndSimpleMedianAlgorithm", "FAS Median");
		case EGraphFormatterPositioningAlgorithm::ELayerSweep:
			return LOCTEXT("LayerSweepAlgorithm", "Layer sweep");
		default:
			return LOCTEXT("InvalidAlgorithm", "Invalid");
	}
}

TSharedPtr<EGraphFormatterPositioningAlgorithm> FindSelectedAlgorithm()
{
	const EGraphFormatterPositioningAlgorithm Selected = GetDefault<UFormatterSettings>()->PositioningAlgorithm;
	const TArray<TSharedPtr<EGraphFormatterPositioningAlgorithm>>& Options = GetAlgorithmOptions();
	const TSharedPtr<EGraphFormatterPositioningAlgorithm>* Match =
		Options.FindByPredicate([Selected](const TSharedPtr<EGraphFormatterPositioningAlgorithm>& Option)
								{ return Option.IsValid() && *Option == Selected; });
	return Match != nullptr ? *Match : Options[0];
}

bool IsGraphOwnedByObject(const UEdGraph* Graph, const UObject* Object)
{
	for (const UObject* Outer = Graph; Outer != nullptr; Outer = Outer->GetOuter())
	{
		if (Outer == Object) { return true; }
	}
	return false;
}

UObject* FindContextObjectForEditor(const SGraphEditor* Editor, const FFormatterCommandContext& Context)
{
	if (Editor == nullptr) { return nullptr; }
	const UEdGraph* Graph = Editor->GetCurrentGraph();
	if (Graph == nullptr) { return nullptr; }

	for (const TWeakObjectPtr<UObject>& WeakObject : Context.Objects)
	{
		if (UObject* Object = WeakObject.Get(); IsGraphOwnedByObject(Graph, Object)) { return Object; }
	}
	return nullptr;
}

void CollectGraphEditors(const TSharedRef<SWidget>& Widget, TArray<SGraphEditor*>& OutEditors)
{
	if (!Widget->GetVisibility().IsVisible()) { return; }
	if (Widget->GetType() == TEXT("SGraphEditor"))
	{
		OutEditors.Add(StaticCast<SGraphEditor*>(&Widget.Get()));
		return;
	}

	FChildren* Children = Widget->GetChildren();
	if (Children == nullptr) { return; }
	for (int32 Index = 0; Index < Children->Num(); ++Index)
	{
		CollectGraphEditors(Children->GetChildAt(Index), OutEditors);
	}
}

void CollectActiveGraphTargets(const FFormatterCommandContext& Context, TArray<FResolvedGraphTarget>& OutTargets)
{
	if (GEditor == nullptr) { return; }
	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (AssetEditors == nullptr) { return; }

	for (const TWeakObjectPtr<UObject>& WeakObject : Context.Objects)
	{
		UObject* Object = WeakObject.Get();
		IAssetEditorInstance* Instance = Object != nullptr ? AssetEditors->FindEditorForAsset(Object, false) : nullptr;
		TSharedPtr<FTabManager> TabManager = Instance != nullptr ? Instance->GetAssociatedTabManager() : nullptr;
		TSharedPtr<SDockTab> EditorRootTab = TabManager.IsValid() ? TabManager->GetOwnerTab() : nullptr;
		if (!EditorRootTab.IsValid()) { continue; }

		TArray<SGraphEditor*> TabEditors;
		// SDockTab only holds its content; the content is presented elsewhere in the docking tree.
		CollectGraphEditors(EditorRootTab->GetContent(), TabEditors);
		for (SGraphEditor* Editor : TabEditors)
		{
			if (!IsGraphOwnedByObject(Editor->GetCurrentGraph(), Object)) { continue; }
			if (!OutTargets.ContainsByPredicate([Editor](const FResolvedGraphTarget& Target)
												{ return Target.Editor == Editor; }))
			{
				OutTargets.Add({ Editor, Object });
			}
		}
	}
}

bool ResolveGraphTarget(const TSharedPtr<const FFormatterCommandContext>& Context, SGraphEditor*& OutEditor, UObject*& OutObject)
{
	OutEditor = nullptr;
	OutObject = nullptr;
	if (!Context.IsValid()) { return false; }

	if (SGraphEditor* CursorEditor = FFormatter::Instance().FindGraphEditorByCursor())
	{
		if (UObject* Object = FindContextObjectForEditor(CursorEditor, *Context))
		{
			OutEditor = CursorEditor;
			OutObject = Object;
			return true;
		}
	}

	// Moving from a graph to its toolbar removes the graph from the cursor path but not from the active window.
	if (SGraphEditor* ActiveEditor = FFormatter::Instance().FindGraphEditorForTopLevelWindow())
	{
		if (UObject* Object = FindContextObjectForEditor(ActiveEditor, *Context))
		{
			OutEditor = ActiveEditor;
			OutObject = Object;
			return true;
		}
	}

	TArray<FResolvedGraphTarget> ActiveTargets;
	CollectActiveGraphTargets(*Context, ActiveTargets);
	if (ActiveTargets.Num() != 1) { return false; }
	OutEditor = ActiveTargets[0].Editor;
	OutObject = ActiveTargets[0].Object;
	return true;
}

bool IsFormattingEnabledForObject(const UObject* Object)
{
	const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
	return Object != nullptr && (FFormatter::IsAssetSupported(Object) || Settings->AutoDetectGraphEditor);
}

bool IsGraphTargetEditable(const SGraphEditor* Editor)
{
	const SGraphPanel* Panel = Editor != nullptr ? Editor->GetGraphPanel() : nullptr;
	return Panel != nullptr && Panel->IsGraphEditable();
}

bool CanExecuteGraphCommand(const TSharedPtr<const FFormatterCommandContext> Context)
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
	return ResolveGraphTarget(Context, Editor, Object) && IsFormattingEnabledForObject(Object)
		&& IsGraphTargetEditable(Editor);
}

bool CanExecuteBenchmarkCommand(const TSharedPtr<const FFormatterCommandContext> Context)
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
	const UEdGraph* Graph = ResolveGraphTarget(Context, Editor, Object) ? Editor->GetCurrentGraph() : nullptr;
	return Graph != nullptr && IsFormattingEnabledForObject(Object) && IsGraphTargetEditable(Editor)
		&& Cast<UEdGraphSchema_K2>(Graph->GetSchema()) != nullptr;
}

void RecordDetectedGraphEditor(UObject* Object)
{
	if (Object == nullptr) { return; }
	UFormatterSettings* Settings = GetMutableDefault<UFormatterSettings>();
	const FString AssetType = Object->GetClass()->GetName();
	if (Settings->SupportedAssetTypes.Contains(AssetType)) { return; }

	Settings->SupportedAssetTypes.Add(AssetType, true);
	Settings->PostEditChange();
	Settings->SaveConfig();
}

void PrepareGraphCommand(UObject* Object, SGraphEditor* Editor)
{
	if (GetDefault<UFormatterSettings>()->AutoDetectGraphEditor) { RecordDetectedGraphEditor(Object); }
	FFormatter::Instance().SetCurrentEditor(Editor, Object);
}

void ExecuteFormatGraph(const TSharedPtr<const FFormatterCommandContext> Context, const bool bRouteWires)
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
	if (!ResolveGraphTarget(Context, Editor, Object) || !IsFormattingEnabledForObject(Object)) { return; }

	PrepareGraphCommand(Object, Editor);
	FFormatter::Instance().Format(bRouteWires);
}

void ExecutePlaceBlock(const TSharedPtr<const FFormatterCommandContext> Context)
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
	if (!ResolveGraphTarget(Context, Editor, Object) || !IsFormattingEnabledForObject(Object)) { return; }

	PrepareGraphCommand(Object, Editor);
	FFormatter::Instance().PlaceBlock();
}

void ExecuteCompareFormatters(const TSharedPtr<const FFormatterCommandContext> Context)
{
	SGraphEditor* Editor = nullptr;
	UObject* Object = nullptr;
	if (!ResolveGraphTarget(Context, Editor, Object) || Editor == nullptr || Object == nullptr) { return; }

	FString Error;
	if (!GraphFormatter::Benchmark::FGraphFormatterBenchmark::Open(*Editor, *Object, Error))
	{
		UE_LOG(LogGraphFormatter, Warning, TEXT("Could not start formatter comparison: %s"), *Error);
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
	}
}

bool IsStraightenConnectionsEnabled()
{
	const UGraphEditorSettings* Settings = GetDefault<UGraphEditorSettings>();
	return Settings->ForwardSplineTangentFromHorizontalDelta.IsZero()
		&& Settings->ForwardSplineTangentFromVerticalDelta.IsZero()
		&& Settings->BackwardSplineTangentFromHorizontalDelta.IsZero()
		&& Settings->BackwardSplineTangentFromVerticalDelta.IsZero();
}

void ToggleStraightenConnections()
{
	UGraphEditorSettings* GraphSettings = GetMutableDefault<UGraphEditorSettings>();
	if (IsStraightenConnectionsEnabled())
	{
		const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
		GraphSettings->ForwardSplineTangentFromHorizontalDelta = Settings->ForwardSplineTangentFromHorizontalDelta;
		GraphSettings->ForwardSplineTangentFromVerticalDelta = Settings->ForwardSplineTangentFromVerticalDelta;
		GraphSettings->BackwardSplineTangentFromHorizontalDelta = Settings->BackwardSplineTangentFromHorizontalDelta;
		GraphSettings->BackwardSplineTangentFromVerticalDelta = Settings->BackwardSplineTangentFromVerticalDelta;
	}
	else
	{
		UFormatterSettings* Settings = GetMutableDefault<UFormatterSettings>();
		Settings->ForwardSplineTangentFromHorizontalDelta = GraphSettings->ForwardSplineTangentFromHorizontalDelta;
		Settings->ForwardSplineTangentFromVerticalDelta = GraphSettings->ForwardSplineTangentFromVerticalDelta;
		Settings->BackwardSplineTangentFromHorizontalDelta = GraphSettings->BackwardSplineTangentFromHorizontalDelta;
		Settings->BackwardSplineTangentFromVerticalDelta = GraphSettings->BackwardSplineTangentFromVerticalDelta;
		Settings->PostEditChange();
		Settings->SaveConfig();

		GraphSettings->ForwardSplineTangentFromHorizontalDelta = FVector2D::ZeroVector;
		GraphSettings->ForwardSplineTangentFromVerticalDelta = FVector2D::ZeroVector;
		GraphSettings->BackwardSplineTangentFromHorizontalDelta = FVector2D::ZeroVector;
		GraphSettings->BackwardSplineTangentFromVerticalDelta = FVector2D::ZeroVector;
	}
	GraphSettings->PostEditChange();
	GraphSettings->SaveConfig();
}

void SaveToolbarSpacing(const int32 Spacing, ETextCommit::Type)
{
	UFormatterSettings* Settings = GetMutableDefault<UFormatterSettings>();
	Settings->HorizontalSpacing = Spacing;
	Settings->K2HorizontalSpacing = Spacing;
	Settings->PostEditChange();
	Settings->SaveConfig();
}

void SaveFallbackAlgorithm(const TSharedPtr<EGraphFormatterPositioningAlgorithm> NewOption, const ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !NewOption.IsValid()) { return; }
	UFormatterSettings* Settings = GetMutableDefault<UFormatterSettings>();
	Settings->PositioningAlgorithm = *NewOption;
	Settings->PostEditChange();
	Settings->SaveConfig();
}

TSharedRef<SWidget> MakeAlgorithmWidget(const TSharedPtr<EGraphFormatterPositioningAlgorithm> Algorithm)
{
	return SNew(STextBlock)
		.Text(Algorithm.IsValid() ? GetAlgorithmName(*Algorithm) : LOCTEXT("InvalidAlgorithmWidget", "Invalid"));
}

void FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	const FFormatterCommands& Commands = FFormatterCommands::Get();
	const TArray<TSharedPtr<EGraphFormatterPositioningAlgorithm>>& AlgorithmOptions = GetAlgorithmOptions();

	ToolbarBuilder.BeginSection("GraphFormatter");
	ToolbarBuilder.AddToolBarButton(
		Commands.FormatGraph,
		NAME_None,
		TAttribute<FText>(),
		TAttribute<FText>(),
		FSlateIcon(FFormatterStyle::Get()->GetStyleSetName(), "GraphFormatter.ApplyIcon"),
		TEXT("GraphFormatter")
	);
	ToolbarBuilder.AddToolBarButton(
		Commands.FormatGraphWithRouting,
		NAME_None,
		TAttribute<FText>(),
		TAttribute<FText>(),
		FSlateIcon(FFormatterStyle::Get()->GetStyleSetName(), "GraphFormatter.ApplyIcon"),
		TEXT("GraphFormatterRoute")
	);
	ToolbarBuilder.AddToolBarButton(
		Commands.CompareFormatters,
		NAME_None,
		TAttribute<FText>(),
		TAttribute<FText>(),
		FSlateIcon(FFormatterStyle::Get()->GetStyleSetName(), "GraphFormatter.ApplyIcon"),
		TEXT("GraphFormatterCompare")
	);

	ToolbarBuilder.AddWidget(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().Padding(
			2.0f
		)[SNew(SSpinBox<int32>)
			  .MinValue(0)
			  .MaxValue(1000)
			  .MinSliderValue(0)
			  .MaxSliderValue(1000)
			  .ToolTipText(LOCTEXT("GraphFormatterHorizontalSpacingToolTip", "Horizontal execution-column spacing for K2 graphs and layer spacing for generic graphs."))
			  .Value_Lambda([]() { return GetDefault<UFormatterSettings>()->K2HorizontalSpacing; })
			  .MinDesiredWidth(80)
			  .OnValueCommitted_Static(&SaveToolbarSpacing)]
		+ SHorizontalBox::Slot().Padding(
			2.0f
		)[SNew(SComboBox<TSharedPtr<EGraphFormatterPositioningAlgorithm>>)
			  .ContentPadding(FMargin(6.0f, 2.0f))
			  .OptionsSource(&AlgorithmOptions)
			  .ToolTipText(LOCTEXT(
				  "GraphFormatterFallbackAlgorithmToolTip", "Generic/fallback positioning algorithm. K2 graphs use the semantic execution-first layout."
			  ))
			  .InitiallySelectedItem(FindSelectedAlgorithm())
			  .OnSelectionChanged_Static(&SaveFallbackAlgorithm)
			  .OnGenerateWidget_Static(&MakeAlgorithmWidget)[SNew(STextBlock)
																 .Text_Lambda(
																	 []()
																	 {
																		 return FText::Format(
																			 LOCTEXT(
																				 "GraphFormatterFallbackAlgorithmLabel",
																				 "Fallback: {0}"
																			 ),
																			 GetAlgorithmName(
																				 GetDefault<UFormatterSettings>()->PositioningAlgorithm
																			 )
																		 );
																	 }
																 )]]
	);
	ToolbarBuilder.AddToolBarButton(
		Commands.StraightenConnections,
		NAME_None,
		TAttribute<FText>(),
		TAttribute<FText>(),
		FSlateIcon(FFormatterStyle::Get()->GetStyleSetName(), "GraphFormatter.StraightenIcon"),
		TEXT("GraphFormatterStraighten")
	);
	ToolbarBuilder.EndSection();
}

bool ShouldExtendToolbar(const TArray<UObject*>& Objects)
{
	const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
	if (Settings->DisableToolbar) { return false; }
	if (Settings->AutoDetectGraphEditor) { return Objects.Num() > 0; }

	for (const UObject* Object : Objects)
	{
		if (Object != nullptr && FFormatter::IsAssetSupported(Object)) { return true; }
	}
	return false;
}

TSharedRef<FExtender> CreateToolbarExtender(const TSharedRef<FUICommandList> CommandList, const TArray<UObject*> ContextSensitiveObjects)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	if (!ShouldExtendToolbar(ContextSensitiveObjects)) { return Extender; }

	TSharedRef<FFormatterCommandContext> Context = MakeShared<FFormatterCommandContext>();
	for (UObject* Object : ContextSensitiveObjects)
	{
		if (Object != nullptr) { Context->Objects.Add(Object); }
	}
	const TSharedPtr<const FFormatterCommandContext> SharedContext = Context;

	const FFormatterCommands& Commands = FFormatterCommands::Get();
	CommandList->MapAction(
		Commands.FormatGraph,
		FExecuteAction::CreateStatic(&ExecuteFormatGraph, SharedContext, false),
		FCanExecuteAction::CreateStatic(&CanExecuteGraphCommand, SharedContext)
	);
	CommandList->MapAction(
		Commands.FormatGraphWithRouting,
		FExecuteAction::CreateStatic(&ExecuteFormatGraph, SharedContext, true),
		FCanExecuteAction::CreateStatic(&CanExecuteGraphCommand, SharedContext)
	);
	CommandList->MapAction(
		Commands.CompareFormatters,
		FExecuteAction::CreateStatic(&ExecuteCompareFormatters, SharedContext),
		FCanExecuteAction::CreateStatic(&CanExecuteBenchmarkCommand, SharedContext)
	);
	CommandList->MapAction(
		Commands.PlaceBlock,
		FExecuteAction::CreateStatic(&ExecutePlaceBlock, SharedContext),
		FCanExecuteAction::CreateStatic(&CanExecuteGraphCommand, SharedContext)
	);
	CommandList->MapAction(
		Commands.StraightenConnections,
		FExecuteAction::CreateStatic(&ToggleStraightenConnections),
		FCanExecuteAction(),
		FIsActionChecked::CreateStatic(&IsStraightenConnectionsEnabled)
	);
	GetMappedCommandLists().AddUnique(TWeakPtr<FUICommandList>(CommandList));

	Extender->AddToolBarExtension(
		"Asset", EExtensionHook::After, CommandList, FToolBarExtensionDelegate::CreateStatic(&FillToolbar)
	);
	return Extender;
}

void HandleAssetEditorOpened(UObject* Object, IAssetEditorInstance*)
{
	if (Object == nullptr) { return; }
	UE_LOG(LogGraphFormatter, Log, TEXT("AssetEditorOpened for: %s"), *Object->GetClass()->GetName());

	const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
	if (!Settings->AutoDetectGraphEditor || FFormatter::IsAssetSupported(Object)) { return; }

	TSharedRef<FFormatterCommandContext> Context = MakeShared<FFormatterCommandContext>();
	Context->Objects.Add(Object);
	SGraphEditor* Editor = nullptr;
	UObject* ResolvedObject = nullptr;
	if (ResolveGraphTarget(Context, Editor, ResolvedObject)) { RecordDetectedGraphEditor(ResolvedObject); }
}

void UnmapCommands()
{
	if (!FFormatterCommands::IsRegistered())
	{
		GetMappedCommandLists().Reset();
		return;
	}

	const FFormatterCommands& Commands = FFormatterCommands::Get();
	for (const TWeakPtr<FUICommandList>& WeakCommandList : GetMappedCommandLists())
	{
		if (const TSharedPtr<FUICommandList> CommandList = WeakCommandList.Pin())
		{
			CommandList->UnmapAction(Commands.FormatGraph);
			CommandList->UnmapAction(Commands.FormatGraphWithRouting);
			CommandList->UnmapAction(Commands.CompareFormatters);
			CommandList->UnmapAction(Commands.PlaceBlock);
			CommandList->UnmapAction(Commands.StraightenConnections);
		}
	}
	GetMappedCommandLists().Reset();
}
} // namespace

class FFormatterModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FDelegateHandle AssetOpenedDelegateHandle;
	FDelegateHandle ToolbarExtenderDelegateHandle;
};

IMPLEMENT_MODULE(FFormatterModule, GraphFormatter)

void FFormatterModule::StartupModule()
{
	FFormatterStyle::Initialize();
	FFormatterCommands::Register();

	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings(
			"Editor",
			"Plugins",
			"GraphFormatter",
			LOCTEXT("GraphFormatterSettingsName", "Graph Formatter"),
			LOCTEXT("GraphFormatterSettingsDescription", "Configure the Graph Formatter plug-in."),
			GetMutableDefault<UFormatterSettings>()
		);
	}

	FAssetEditorExtender ToolbarDelegate = FAssetEditorExtender::CreateStatic(&CreateToolbarExtender);
	ToolbarExtenderDelegateHandle = ToolbarDelegate.GetHandle();
	FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates().Add(MoveTemp(ToolbarDelegate));

	if (GEditor)
	{
		AssetOpenedDelegateHandle =
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OnAssetOpenedInEditor().AddStatic(&HandleAssetEditorOpened);
	}
}

void FFormatterModule::ShutdownModule()
{
	FFormatter::Instance().CancelPendingFormat();

	if (GEditor && AssetOpenedDelegateHandle.IsValid())
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OnAssetOpenedInEditor().Remove(AssetOpenedDelegateHandle);
		AssetOpenedDelegateHandle.Reset();
	}

	if (ToolbarExtenderDelegateHandle.IsValid())
	{
		TArray<FAssetEditorExtender>& Delegates =
			FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();
		Delegates.RemoveAll([this](const FAssetEditorExtender& Delegate)
							{ return Delegate.GetHandle() == ToolbarExtenderDelegateHandle; });
		ToolbarExtenderDelegateHandle.Reset();
	}

	UnmapCommands();
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Editor", "Plugins", "GraphFormatter");
	}
	FFormatterCommands::Unregister();
	FFormatterStyle::Shutdown();
}

#undef LOCTEXT_NAMESPACE
