/*---------------------------------------------------------------------------------------------
 *  Copyright (c) Howaajin. All rights reserved.
 *  Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"
#include "Layout/SlateRect.h"

class FBlueprintEditor;
class FMaterialEditor;
class FSoundCueEditor;
class FBehaviorTreeEditor;
class UEdGraphNode_Comment;
struct FEdGraphEditAction;

struct FFormatter
{
	static FFormatter& Instance();
	static bool IsAssetSupported(const UObject* Object);
	inline static bool IsAutoSizeComment = false;

	bool IsVerticalLayout = false;
	bool IsBehaviorTree = false;
	bool IsBlueprint = false;

	void SetCurrentEditor(SGraphEditor* Editor, UObject* Object);
	SGraphEditor* FindGraphEditorForTopLevelWindow() const;
	SGraphEditor* FindGraphEditorByCursor() const;

	SGraphPanel* GetCurrentPanel() const;
	SGraphNode* GetWidget(const UEdGraphNode* Node) const;
	TSet<UEdGraphNode*> GetAllNodes() const;
	float GetCommentNodeTitleHeight(const UEdGraphNode* Node) const;
	FVector2D GetNodeSize(const UEdGraphNode* Node) const;
	FVector2D GetNodePosition(const UEdGraphNode* Node) const;
	FVector2D GetPinOffset(const UEdGraphPin* Pin) const;
	FSlateRect GetNodesBound(const TSet<UEdGraphNode*> Nodes) const;
	TSet<UEdGraphNode*> GetNodesUnderComment(const UEdGraphNode_Comment* CommentNode) const;

	bool PreCommand();
	void PostCommand();
	bool Translate(const TSet<UEdGraphNode*>& Nodes, FVector2D Offset) const;
	void Format(bool bRouteWires = false, bool bAllowDeferredK2 = true);
	void PlaceBlock();
	void CancelPendingFormat();

private:
	struct FPendingK2Format;

	FFormatter();
	~FFormatter();
	FFormatter(FFormatter const&) = delete;
	void operator=(FFormatter const&) = delete;

	void BeginK2Format(const TSet<UEdGraphNode*>& Nodes, bool bRouteWires);
	void HandleK2PostTick(float DeltaTime);
	void HandlePendingGraphChanged(const FEdGraphEditAction& Action);

	SGraphEditor* CurrentEditor = nullptr;
	SGraphPanel* CurrentPanel = nullptr;
	UEdGraph* CurrentGraph = nullptr;
	TUniquePtr<FPendingK2Format> PendingK2Format;
	FDelegateHandle K2PostTickHandle;
	FDelegateHandle K2GraphChangedHandle;
};
