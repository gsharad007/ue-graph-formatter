/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Formatter.h"
#include "FormatterCommands.h"
#include "FormatterGraph.h"
#include "FormatterSettings.h"
#include "K2/GraphGeometrySnapshot.h"
#include "K2/K2GraphFormatter.h"
#include "UEGraphAdapter.h"
#include "FormatterLog.h"

#include "BehaviorTree/BehaviorTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Math/Ray.h"
#include "Modules/ModuleManager.h"
#include "SGraphNodeComment.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GraphFormatter"

struct FFormatter::FPendingK2Format
{
	TWeakPtr<SGraphEditor> Editor;
	TWeakObjectPtr<UEdGraph> Graph;
	TArray<TWeakObjectPtr<UEdGraphNode>> Scope;
	TSet<TWeakObjectPtr<UEdGraphNode>> ExplicitSelection;
	bool bRouteWires = false;
	bool bGraphChanged = false;
	int32 RetryCount = 0;
};

static void ShowFormatterNotification(const FText& Text, const SNotificationItem::ECompletionState State)
{
	FNotificationInfo Info(Text);
	Info.bFireAndForget = true;
	Info.ExpireDuration = 4.0f;
	if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(State);
	}
}

static void ReportReadOnlyGraph()
{
	UE_LOG(
		LogGraphFormatter,
		Warning,
		TEXT("Formatting was not started because the active graph is read-only. Stop simulation or open the Blueprint that owns the graph.")
	);
	if (GetDefault<UFormatterSettings>()->bShowLayoutNotifications)
	{
		ShowFormatterNotification(
			LOCTEXT(
				"ReadOnlyGraph", "Graph Formatter cannot change this read-only graph. Stop simulation, or open the Blueprint that owns the inherited graph."
			),
			SNotificationItem::CS_Fail
		);
	}
}

void FFormatter::SetCurrentEditor(SGraphEditor* Editor, UObject* Object)
{
	CurrentEditor = Editor;
	IsVerticalLayout = false;
	IsBehaviorTree = false;
	IsBlueprint = false;
	if (Cast<UBehaviorTree>(Object))
	{
		IsVerticalLayout = true;
		IsBehaviorTree = true;
	}
	if (Cast<UBlueprint>(Object)) { IsBlueprint = true; }
	if (Object->GetClass()->GetName() == "NiagaraSystem") { IsVerticalLayout = true; }
}

bool FFormatter::IsAssetSupported(const UObject* Object)
{
	const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
	if (const bool* Enabled = Settings->SupportedAssetTypes.Find(Object->GetClass()->GetName()))
	{
		return Enabled != nullptr && *Enabled;
	}
	return false;
}

/** Matches widgets by type */
struct FWidgetTypeMatcher
{
	FWidgetTypeMatcher(const FName& InType)
		: TypeName(InType)
	{
	}

	bool IsMatch(const TSharedRef<const SWidget>& InWidget) const { return TypeName == InWidget->GetType(); }

	const FName& TypeName;
};

SGraphEditor* FFormatter::FindGraphEditorForTopLevelWindow() const
{
	FSlateApplication& Application = FSlateApplication::Get();
	auto ActiveWindow = Application.GetActiveTopLevelWindow();
	if (!ActiveWindow.IsValid()) { return nullptr; }
	FGeometry InnerWindowGeometry = ActiveWindow->GetWindowGeometryInWindow();
	FArrangedChildren JustWindow(EVisibility::Visible);
	JustWindow.AddWidget(FArrangedWidget(ActiveWindow.ToSharedRef(), InnerWindowGeometry));

	FWidgetPath WidgetPath(ActiveWindow.ToSharedRef(), JustWindow);
	if (WidgetPath.ExtendPathTo(FWidgetTypeMatcher("SGraphEditor"), EVisibility::Visible))
	{
		return StaticCast<SGraphEditor*>(&WidgetPath.GetLastWidget().Get());
	}
	return nullptr;
}

SGraphEditor* FFormatter::FindGraphEditorByCursor() const
{
	FSlateApplication& Application = FSlateApplication::Get();
	FWidgetPath WidgetPath =
		Application.LocateWindowUnderMouse(Application.GetCursorPos(), Application.GetInteractiveTopLevelWindows());
	for (int i = WidgetPath.Widgets.Num() - 1; i >= 0; i--)
	{
		if (WidgetPath.Widgets[i].Widget->GetTypeAsString() == "SGraphEditor")
		{
			return StaticCast<SGraphEditor*>(&WidgetPath.Widgets[i].Widget.Get());
		}
	}
	return nullptr;
}

SGraphPanel* FFormatter::GetCurrentPanel() const { return CurrentEditor->GetGraphPanel(); }

SGraphNode* FFormatter::GetWidget(const UEdGraphNode* Node) const
{
	SGraphPanel* GraphPanel = GetCurrentPanel();
	if (GraphPanel != nullptr)
	{
		TSharedPtr<SGraphNode> NodeWidget = GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
		return NodeWidget.Get();
	}
	return nullptr;
}

TSet<UEdGraphNode*> FFormatter::GetAllNodes() const
{
	TSet<UEdGraphNode*> Nodes;
	if (CurrentEditor)
	{
		for (UEdGraphNode* Node : CurrentEditor->GetCurrentGraph()->Nodes)
		{
			Nodes.Add(Node);
		}
	}
	return Nodes;
}

float FFormatter::GetCommentNodeTitleHeight(const UEdGraphNode* Node) const
{
	/** Titlebar Offset - taken from SGraphNodeComment.cpp */
	static const FSlateRect TitleBarOffset(13, 8, -3, 0);

	SGraphNode* CommentNode = GetWidget(Node);
	if (CommentNode)
	{
		SGraphNodeComment* NodeWidget = StaticCast<SGraphNodeComment*>(CommentNode);
		FSlateRect Rect = NodeWidget->GetTitleRect();
		return Rect.GetSize().Y + TitleBarOffset.Top;
	}
	return 0;
}

FVector2D FFormatter::GetNodeSize(const UEdGraphNode* Node) const
{
	FVector2D Minimum;
	FVector2D Maximum;
	if (SGraphPanel* Panel = GetCurrentPanel(); Panel && Panel->GetBoundsForNode(Node, Minimum, Maximum))
	{
		const FVector2D Size = Maximum - Minimum;
		if (Size.X > 0.0 && Size.Y > 0.0) { return Size; }
	}
	auto GraphNode = GetWidget(Node);
	if (GraphNode != nullptr)
	{
		const FVector2D Size(GraphNode->GetDesiredSize());
		if (Size.X > 0.0 && Size.Y > 0.0) { return Size; }
	}
	const double Width = Node->NodeWidth > 0 ? Node->NodeWidth : 160.0;
	const double Height = Node->NodeHeight > 0 ? Node->NodeHeight : 80.0;
	return FVector2D(Width, Height);
}

FVector2D FFormatter::GetNodePosition(const UEdGraphNode* Node) const
{
	auto GraphNode = GetWidget(Node);
	if (GraphNode != nullptr) { return GraphNode->GetPosition(); }
	return FVector2D(Node->NodePosX, Node->NodePosY);
}

FVector2D FFormatter::GetPinOffset(const UEdGraphPin* Pin) const
{
	auto GraphNode = GetWidget(Pin->GetOwningNodeUnchecked());
	if (GraphNode != nullptr)
	{
		auto PinWidget = GraphNode->FindWidgetForPin(const_cast<UEdGraphPin*>(Pin));
		if (PinWidget.IsValid())
		{
			auto Offset = PinWidget->GetNodeOffset();
			return Offset;
		}
	}
	return FVector2D::ZeroVector;
}

FSlateRect FFormatter::GetNodesBound(const TSet<UEdGraphNode*> Nodes) const
{
	FSlateRect Bound;
	for (auto Node : Nodes)
	{
		FVector2D Pos = GetNodePosition(Node);
		FVector2D Size = GetNodeSize(Node);
		FSlateRect NodeBound = FSlateRect::FromPointAndExtent(Pos, Size);
		Bound = Bound.IsValid() ? Bound.Expand(NodeBound) : NodeBound;
	}
	return Bound;
}

bool FFormatter::PreCommand()
{
	CancelPendingFormat();
	if (!CurrentEditor) { return false; }
	CurrentGraph = CurrentEditor->GetCurrentGraph();
	if (!CurrentGraph) { return false; }
	CurrentPanel = GetCurrentPanel();
	if (!CurrentPanel) { return false; }
	if (!CurrentPanel->IsGraphEditable())
	{
		ReportReadOnlyGraph();
		return false;
	}

	CurrentPanel->Update();
	CurrentPanel->SlatePrepass();

	const UFormatterSettings* Settings = GetDefault<UFormatterSettings>();
	FFormatterGraph::HorizontalSpacing = Settings->HorizontalSpacing;
	FFormatterGraph::VerticalSpacing = Settings->VerticalSpacing;
	FFormatterGraph::SpacingFactorOfGroup = Settings->SpacingFactorOfParameterGroup;
	FFormatterGraph::MaxLayerNodes = Settings->MaxLayerNodes;
	FFormatterGraph::MaxOrderingIterations = Settings->MaxOrderingIterations;
	FFormatterGraph::PositioningAlgorithm = Settings->PositioningAlgorithm;
	return true;
}

void FFormatter::PostCommand() { }

bool FFormatter::Translate(const TSet<UEdGraphNode*>& Nodes, FVector2D Offset) const
{
	if (!CurrentEditor) { return false; }
	UEdGraph* Graph = CurrentEditor->GetCurrentGraph();
	if (!Graph) { return false; }
	if (Offset.X == 0 && Offset.Y == 0) { return false; }
	bool bModified = false;
	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node) { continue; }
		const int32 NewX = FMath::RoundToInt(static_cast<double>(Node->NodePosX) + Offset.X);
		const int32 NewY = FMath::RoundToInt(static_cast<double>(Node->NodePosY) + Offset.Y);
		if (Node->NodePosX != NewX || Node->NodePosY != NewY)
		{
			Node->Modify();
			Node->NodePosX = NewX;
			Node->NodePosY = NewY;
			bModified = true;
		}
	}
	return bModified;
}

static TSet<UEdGraphNode*> GetSelectedNodes(const SGraphEditor* GraphEditor)
{
	TSet<UEdGraphNode*> SelectedGraphNodes;
	TSet<UObject*> SelectedNodes = GraphEditor->GetSelectedNodes();
	for (UObject* Node : SelectedNodes)
	{
		UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node);
		if (GraphNode) { SelectedGraphNodes.Add(GraphNode); }
	}
	return SelectedGraphNodes;
}

static bool IsNodeUnderRect(const TSharedRef<SGraphNode> InNodeWidget, const FSlateRect& Rect)
{
	const FVector2D InNodePosition = InNodeWidget->GetPosition();
	const FVector2D InNodeSize = InNodeWidget->GetDesiredSize();
	const FSlateRect NodeGeometryGraphSpace(
		InNodePosition.X, InNodePosition.Y, InNodePosition.X + InNodeSize.X, InNodePosition.Y + InNodeSize.Y
	);
	return FSlateRect::IsRectangleContained(Rect, NodeGeometryGraphSpace);
}

TSet<UEdGraphNode*> FFormatter::GetNodesUnderComment(const UEdGraphNode_Comment* CommentNode) const
{
	TSet<UEdGraphNode*> Result;
	if (IsAutoSizeComment)
	{
		const FCommentNodeSet& NodesUnderComment = CommentNode->GetNodesUnderComment();
		for (UObject* Object : NodesUnderComment)
		{
			if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object)) { Result.Add(Node); }
		}
		return Result;
	}
	SGraphNode* CommentNodeWidget = GetWidget(CommentNode);
	if (!CommentNodeWidget) { return Result; }
	auto CommentSize = CommentNodeWidget->GetDesiredSize();
	if (CommentSize.IsZero()) { return TSet<UEdGraphNode*>(); }
	SGraphPanel* Panel = GetCurrentPanel();
	FChildren* PanelChildren = Panel->GetAllChildren();
	int32 NumChildren = PanelChildren->Num();
	FVector2D CommentNodePosition = CommentNodeWidget->GetPosition();
	FSlateRect CommentRect = FSlateRect(CommentNodePosition, CommentNodePosition + CommentSize);
	for (int32 NodeIndex = 0; NodeIndex < NumChildren; ++NodeIndex)
	{
		const TSharedRef<SGraphNode> SomeNodeWidget = StaticCastSharedRef<SGraphNode>(PanelChildren->GetChildAt(NodeIndex));
		UObject* GraphObject = SomeNodeWidget->GetObjectBeingDisplayed();
		if (GraphObject != CommentNode)
		{
			if (IsNodeUnderRect(SomeNodeWidget, CommentRect)) { Result.Add(Cast<UEdGraphNode>(GraphObject)); }
		}
	}
	return Result;
}

static void AddCommentContentsRecursively(UEdGraphNode_Comment* Comment, TSet<UEdGraphNode*>& Nodes)
{
	if (!Comment) { return; }

	const TSet<UEdGraphNode*> Contents = FFormatter::Instance().GetNodesUnderComment(Comment);
	for (UEdGraphNode* Node : Contents)
	{
		if (!Node || Nodes.Contains(Node)) { continue; }
		Nodes.Add(Node);
		AddCommentContentsRecursively(Cast<UEdGraphNode_Comment>(Node), Nodes);
	}
}

static TSet<UEdGraphNode*> DoSelectionStrategy(UEdGraph* Graph, TSet<UEdGraphNode*> Selected)
{
	if (Selected.Num() != 0)
	{
		TSet<UEdGraphNode*> SelectedGraphNodes;
		for (UEdGraphNode* GraphNode : Selected)
		{
			SelectedGraphNodes.Add(GraphNode);
			if (GraphNode->IsA(UEdGraphNode_Comment::StaticClass()))
			{
				AddCommentContentsRecursively(Cast<UEdGraphNode_Comment>(GraphNode), SelectedGraphNodes);
			}
		}
		return SelectedGraphNodes;
	}
	TSet<UEdGraphNode*> SelectedGraphNodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		SelectedGraphNodes.Add(Node);
	}
	return SelectedGraphNodes;
}

void FFormatter::BeginK2Format(const TSet<UEdGraphNode*>& Nodes, const bool bRouteWires)
{
	CancelPendingFormat();
	if (!CurrentEditor || !CurrentGraph || !CurrentPanel || !FSlateApplication::IsInitialized()) { return; }

	PendingK2Format = MakeUnique<FPendingK2Format>();
	PendingK2Format->Editor = StaticCastSharedRef<SGraphEditor>(CurrentEditor->AsShared());
	PendingK2Format->Graph = CurrentGraph;
	PendingK2Format->bRouteWires = bRouteWires;
	PendingK2Format->Scope.Reserve(Nodes.Num());
	for (UEdGraphNode* Node : Nodes)
	{
		if (Node && Node->GetGraph() == CurrentGraph) { PendingK2Format->Scope.Add(Node); }
	}
	for (UEdGraphNode* Node : GetSelectedNodes(CurrentEditor))
	{
		PendingK2Format->ExplicitSelection.Add(Node);
	}

	K2GraphChangedHandle = CurrentGraph->AddOnGraphChangedHandler(
		FOnGraphChanged::FDelegate::CreateRaw(this, &FFormatter::HandlePendingGraphChanged)
	);
	K2PostTickHandle = FSlateApplication::Get().OnPostTick().AddRaw(this, &FFormatter::HandleK2PostTick);
}

void FFormatter::HandlePendingGraphChanged(const FEdGraphEditAction& Action)
{
	if (PendingK2Format) { PendingK2Format->bGraphChanged = true; }
}

void FFormatter::HandleK2PostTick(float DeltaTime)
{
	if (!PendingK2Format)
	{
		CancelPendingFormat();
		return;
	}

	const TSharedPtr<SGraphEditor> Editor = PendingK2Format->Editor.Pin();
	UEdGraph* Graph = PendingK2Format->Graph.Get();
	if (!Editor || !Graph || PendingK2Format->bGraphChanged || Editor->GetCurrentGraph() != Graph)
	{
		CancelPendingFormat();
		return;
	}

	TSet<TWeakObjectPtr<UEdGraphNode>> CurrentSelection;
	for (UEdGraphNode* Node : GetSelectedNodes(Editor.Get()))
	{
		CurrentSelection.Add(Node);
	}
	if (CurrentSelection.Num() != PendingK2Format->ExplicitSelection.Num()
		|| !CurrentSelection.Includes(PendingK2Format->ExplicitSelection))
	{
		CancelPendingFormat();
		return;
	}

	SGraphPanel* Panel = Editor->GetGraphPanel();
	if (!Panel)
	{
		CancelPendingFormat();
		return;
	}
	if (!Panel->IsGraphEditable())
	{
		CancelPendingFormat();
		ReportReadOnlyGraph();
		return;
	}

	GraphFormatter::K2::FGraphGeometrySnapshot Geometry = GraphFormatter::K2::FGraphGeometrySnapshot::Capture(*Panel);
	if (Geometry.ShouldRetry() && PendingK2Format->RetryCount++ == 0)
	{
		Panel->Update();
		return;
	}
	if (!Geometry.IsReady())
	{
		for (const GraphFormatter::K2::FGraphGeometryDiagnostic& Diagnostic : Geometry.Diagnostics)
		{
			UE_LOG(LogGraphFormatter, Warning, TEXT("%s"), *Diagnostic.ToString());
		}
		const bool bShowNotification = GetDefault<UFormatterSettings>()->bShowLayoutNotifications;
		CancelPendingFormat();
		if (bShowNotification)
		{
			ShowFormatterNotification(
				LOCTEXT("GeometryUnavailable", "Graph Formatter could not obtain reliable node geometry; no nodes were changed."),
				SNotificationItem::CS_Fail
			);
		}
		return;
	}

	TSet<UEdGraphNode*> Scope;
	for (const TWeakObjectPtr<UEdGraphNode>& WeakNode : PendingK2Format->Scope)
	{
		if (UEdGraphNode* Node = WeakNode.Get(); Node && Node->GetGraph() == Graph) { Scope.Add(Node); }
	}
	const bool bRouteWires = PendingK2Format->bRouteWires;
	CancelPendingFormat();

	const UFormatterSettings& Settings = *GetDefault<UFormatterSettings>();
	const GraphFormatter::K2::FK2FormatResult Result =
		GraphFormatter::K2::FK2GraphFormatter::Format(*Editor, *Graph, Geometry, Scope, bRouteWires, Settings);
	for (const FString& Diagnostic : Result.Diagnostics)
	{
		UE_LOG(LogGraphFormatter, Log, TEXT("%s"), *Diagnostic);
	}
	if (Settings.bShowLayoutNotifications && !Result.Message.IsEmpty())
	{
		const SNotificationItem::ECompletionState State =
			Result.Status == GraphFormatter::K2::EK2FormatStatus::Formatted
					|| (Result.Status == GraphFormatter::K2::EK2FormatStatus::NoChanges && !Result.bSafetyRejected)
				? SNotificationItem::CS_Success
				: SNotificationItem::CS_Fail;
		ShowFormatterNotification(FText::FromString(Result.Message), State);
	}
}

void FFormatter::CancelPendingFormat()
{
	if (K2PostTickHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnPostTick().Remove(K2PostTickHandle);
	}
	K2PostTickHandle.Reset();

	if (K2GraphChangedHandle.IsValid() && PendingK2Format)
	{
		if (UEdGraph* Graph = PendingK2Format->Graph.Get())
		{
			Graph->RemoveOnGraphChangedHandler(K2GraphChangedHandle);
		}
	}
	K2GraphChangedHandle.Reset();
	PendingK2Format.Reset();
}

void FFormatter::Format(const bool bRouteWires, const bool bAllowDeferredK2)
{
	if (!PreCommand()) { return; }
	auto SelectedNodes = GetSelectedNodes(CurrentEditor);
	SelectedNodes = DoSelectionStrategy(CurrentGraph, SelectedNodes);
	const UFormatterSettings& Settings = *GetDefault<UFormatterSettings>();
	if (bAllowDeferredK2 && Settings.bEnableK2Formatter
		&& GraphFormatter::K2::FK2GraphFormatter::CanFormat(*CurrentGraph, SelectedNodes))
	{
		BeginK2Format(SelectedNodes, bRouteWires);
		PostCommand();
		return;
	}
	auto Graph = UEGraphAdapter::Build(SelectedNodes, IsVerticalLayout);
	if (IsBehaviorTree)
	{
		Graph->NodeComparer = [](const FFormatterNode& A, const FFormatterNode& B)
		{
			const FVector2D APosition = A.GetPosition();
			const FVector2D BPosition = B.GetPosition();
			if (!FMath::IsNearlyEqual(APosition.X, BPosition.X)) { return APosition.X < BPosition.X; }
			if (!FMath::IsNearlyEqual(APosition.Y, BPosition.Y)) { return APosition.Y < BPosition.Y; }
			return A.Guid < B.Guid;
		};
	}
	Graph->Format();
	auto BoundMap = Graph->GetBoundMap();
	delete Graph;
	TArray<TPair<UEdGraphNode*, FBox2D>> ChangedNodes;
	for (auto [Node, Rect] : BoundMap)
	{
		UEdGraphNode* UEdNode = static_cast<UEdGraphNode*>(Node);
		if (!UEdNode) { continue; }
		const FVector2D RectMinimum = Rect.Min;
		const bool bPositionChanged = UEdNode->NodePosX != FMath::RoundToInt(RectMinimum.X)
								   || UEdNode->NodePosY != FMath::RoundToInt(RectMinimum.Y);
		bool bBoundsChanged = false;
		if (UEdNode->IsA(UEdGraphNode_Comment::StaticClass()))
		{
			const FVector2D CurrentSize(UEdNode->NodeWidth, UEdNode->NodeHeight);
			bBoundsChanged = !CurrentSize.Equals(Rect.GetSize(), 0.5);
		}
		if (bPositionChanged || bBoundsChanged) { ChangedNodes.Emplace(UEdNode, Rect); }
	}
	if (!ChangedNodes.IsEmpty())
	{
		const FScopedTransaction Transaction(FFormatterCommands::Get().FormatGraph->GetLabel());
		CurrentGraph->Modify();
		for (const TPair<UEdGraphNode*, FBox2D>& Change : ChangedNodes)
		{
			UEdGraphNode* Node = Change.Key;
			const FBox2D& Rect = Change.Value;
			Node->Modify();
			const FVector2D RectMinimum = Rect.Min;
			const FVector2D RectMaximum = Rect.Max;
			Node->NodePosX = FMath::RoundToInt(RectMinimum.X);
			Node->NodePosY = FMath::RoundToInt(RectMinimum.Y);
			if (UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node))
			{
				CommentNode->SetBounds(FSlateRect(RectMinimum, RectMaximum));
			}
		}
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(CurrentGraph))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
		CurrentGraph->NotifyGraphChanged();
	}
	if (bRouteWires)
	{
		UE_LOG(LogGraphFormatter, Warning, TEXT("Wire routing is available only through the semantic K2 formatter."));
	}
	PostCommand();
}

void FFormatter::PlaceBlock()
{
	if (!PreCommand()) { return; }
	auto SelectedNodes = GetSelectedNodes(CurrentEditor);
	auto ConnectedNodesLeft = UEGraphAdapter::GetNodesConnected(SelectedNodes, EFormatterPinDirection::In);
	FVector2D ConnectCenter;
	const UFormatterSettings& Settings = *GetDefault<UFormatterSettings>();
	FScopedTransaction Transaction(FFormatterCommands::Get().PlaceBlock->GetLabel());
	bool bModified = false;
	if (UEGraphAdapter::GetNodesConnectCenter(SelectedNodes, ConnectCenter, EFormatterPinDirection::In))
	{
		auto Center = FVector(ConnectCenter.X, ConnectCenter.Y, 0);
		auto Direction = IsVerticalLayout ? FVector(0, 1, 0) : FVector(1, 0, 0);
		auto RightRay = FRay(Center, Direction, true);
		FSlateRect Bound = GetNodesBound(ConnectedNodesLeft);
		auto RightBound = IsVerticalLayout ? FVector(0, Bound.Bottom, 0) : FVector(Bound.Right, 0, 0);
		auto LinkedCenter3D = RightRay.PointAt(RightRay.GetParameter(RightBound));
		auto LinkedCenterTo =
			FVector2D(LinkedCenter3D)
			+ (IsVerticalLayout ? FVector2D(0, Settings.HorizontalSpacing) : FVector2D(Settings.HorizontalSpacing, 0));
		UEGraphAdapter::GetNodesConnectCenter(SelectedNodes, ConnectCenter, EFormatterPinDirection::In, true);
		Center = FVector(ConnectCenter.X, ConnectCenter.Y, 0);
		Direction = IsVerticalLayout ? FVector(0, -1, 0) : FVector(-1, 0, 0);
		auto LeftRay = FRay(Center, Direction, true);
		Bound = GetNodesBound(SelectedNodes);
		auto LeftBound = IsVerticalLayout ? FVector(0, Bound.Top, 0) : FVector(Bound.Left, 0, 0);
		LinkedCenter3D = LeftRay.PointAt(LeftRay.GetParameter(LeftBound));
		auto LinkedCenterFrom = FVector2D(LinkedCenter3D);
		FVector2D Offset = LinkedCenterTo - LinkedCenterFrom;
		bModified |= Translate(SelectedNodes, Offset);
	}
	auto ConnectedNodesRight = UEGraphAdapter::GetNodesConnected(SelectedNodes, EFormatterPinDirection::Out);
	if (UEGraphAdapter::GetNodesConnectCenter(SelectedNodes, ConnectCenter, EFormatterPinDirection::Out))
	{
		auto Center = FVector(ConnectCenter.X, ConnectCenter.Y, 0);
		auto Direction = IsVerticalLayout ? FVector(0, -1, 0) : FVector(-1, 0, 0);
		auto LeftRay = FRay(Center, Direction, true);
		FSlateRect Bound = GetNodesBound(ConnectedNodesRight);
		auto LeftBound = IsVerticalLayout ? FVector(0, Bound.Top, 0) : FVector(Bound.Left, 0, 0);
		auto LinkedCenter3D = LeftRay.PointAt(LeftRay.GetParameter(LeftBound));
		auto LinkedCenterTo =
			FVector2D(LinkedCenter3D)
			- (IsVerticalLayout ? FVector2D(0, Settings.HorizontalSpacing) : FVector2D(Settings.HorizontalSpacing, 0));
		UEGraphAdapter::GetNodesConnectCenter(SelectedNodes, ConnectCenter, EFormatterPinDirection::Out, true);
		Center = FVector(ConnectCenter.X, ConnectCenter.Y, 0);
		Direction = IsVerticalLayout ? FVector(0, 1, 0) : FVector(1, 0, 0);
		auto RightRay = FRay(Center, Direction, true);
		Bound = GetNodesBound(SelectedNodes);
		auto RightBound = IsVerticalLayout ? FVector(0, Bound.Bottom, 0) : FVector(Bound.Right, 0, 0);
		LinkedCenter3D = RightRay.PointAt(RightRay.GetParameter(RightBound));
		auto LinkedCenterFrom = FVector2D(LinkedCenter3D);
		FVector2D Offset = LinkedCenterFrom - LinkedCenterTo;
		bModified |= Translate(ConnectedNodesRight, Offset);
	}
	if (bModified)
	{
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(CurrentGraph))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}
		else
		{
			CurrentGraph->MarkPackageDirty();
		}
		CurrentGraph->NotifyGraphChanged();
	}
	else
	{
		Transaction.Cancel();
	}
	PostCommand();
}

FFormatter& FFormatter::Instance()
{
	static FFormatter Context;
	return Context;
}

FFormatter::FFormatter()
{
	if (FModuleManager::Get().GetModule(FName("AutoSizeComments"))) { IsAutoSizeComment = true; }
}

FFormatter::~FFormatter() { CancelPendingFormat(); }

#undef LOCTEXT_NAMESPACE
