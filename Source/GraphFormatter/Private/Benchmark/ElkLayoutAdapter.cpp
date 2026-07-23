/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Benchmark/ElkLayoutAdapter.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "FormatterSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "K2/GraphGeometrySnapshot.h"
#include "K2/K2RerouteRouter.h"
#include "K2Node_Knot.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GraphFormatter::Benchmark
{
namespace ElkLayoutAdapterPrivate
{
constexpr double FallbackNodeWidth = 160.0;
constexpr double FallbackNodeHeight = 80.0;
constexpr double FallbackPinTop = 32.0;
constexpr double FallbackPinPitch = 24.0;
constexpr double PortExtent = 1.0;
constexpr int32 ExecutionEdgePriority = 1000;
constexpr int32 DataEdgePriority = 1;

struct FElkNodeBinding
{
	FString Id;
	UEdGraphNode* Node = nullptr;
	FVector2D Size = FVector2D::ZeroVector;
	FVector2D ElkPosition = FVector2D::ZeroVector;
};

struct FElkEdgeBinding
{
	FString Id;
	UEdGraphPin* OutputPin = nullptr;
	UEdGraphPin* InputPin = nullptr;
	bool bExecution = false;
};

struct FElkProblem
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	TArray<FElkNodeBinding> Nodes;
	TArray<FElkEdgeBinding> Edges;
	int32 SubmittedPorts = 0;
	int32 MeasuredPorts = 0;
};

struct FElkFiles
{
	FString Directory;
	FString Input;
	FString Output;
	FString ProcessLog;
	bool bPersistent = false;
};

[[nodiscard]]
FVector2D RectSize(const FSlateRect& Rect) noexcept
{ return FVector2D(Rect.Right - Rect.Left, Rect.Bottom - Rect.Top); }

[[nodiscard]]
FVector2D ResolveNodeSize(const UEdGraphNode& Node, const K2::FGraphGeometrySnapshot& Geometry)
{
	if (const K2::FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(&Node))
	{
		if (NodeGeometry->Bounds.IsSet()) { return RectSize(NodeGeometry->Bounds.GetValue()); }
	}
	if (Node.IsA<UK2Node_Knot>()) { return FVector2D(K2::RerouteKnotWidth, K2::RerouteKnotHeight); }
	return FVector2D(
		Node.NodeWidth > 0 ? static_cast<double>(Node.NodeWidth) : FallbackNodeWidth,
		Node.NodeHeight > 0 ? static_cast<double>(Node.NodeHeight) : FallbackNodeHeight
	);
}

[[nodiscard]]
int32 DirectionOrdinal(const UEdGraphPin& Pin)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return 0; }
	int32 Ordinal = 0;
	for (const UEdGraphPin* Candidate : Node->Pins)
	{
		if (Candidate == &Pin) { break; }
		if (Candidate != nullptr && Candidate->Direction == Pin.Direction) { ++Ordinal; }
	}
	return Ordinal;
}

[[nodiscard]]
FVector2D ResolvePinOffset(
	const UEdGraphPin& Pin, const FVector2D NodeSize, const K2::FGraphGeometrySnapshot& Geometry, bool& bOutMeasured
)
{
	bOutMeasured = false;
	if (const K2::FGraphPinGeometrySnapshot* PinGeometry = Geometry.FindPin(&Pin))
	{
		bOutMeasured = true;
		return PinGeometry->NodeOffset;
	}
	if (Pin.GetOwningNodeUnchecked() && Pin.GetOwningNodeUnchecked()->IsA<UK2Node_Knot>()) { return NodeSize * 0.5; }
	const double MaximumY = FMath::Max(FallbackPinTop, NodeSize.Y - FallbackPinPitch * 0.5);
	const double Y = FMath::Clamp(
		FallbackPinTop + static_cast<double>(DirectionOrdinal(Pin)) * FallbackPinPitch, FallbackPinTop, MaximumY
	);
	return FVector2D(Pin.Direction == EGPD_Input ? 0.0 : NodeSize.X, Y);
}

[[nodiscard]]
FVector2D ResolvePinAnchor(const UEdGraphPin& Pin, const K2::FGraphGeometrySnapshot& Geometry)
{
	const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
	if (Node == nullptr) { return FVector2D::ZeroVector; }
	bool bMeasured = false;
	return FVector2D(Node->NodePosX, Node->NodePosY)
		 + ResolvePinOffset(Pin, ResolveNodeSize(*Node, Geometry), Geometry, bMeasured);
}

[[nodiscard]]
bool IsExecutionPin(const UEdGraphPin& Pin) noexcept
{ return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; }

[[nodiscard]]
FString NodeStableKey(const UEdGraphNode& Node)
{ return Node.NodeGuid.IsValid() ? Node.NodeGuid.ToString(EGuidFormats::Digits) : Node.GetPathName(); }

[[nodiscard]]
bool HasLinkedExecutionPin(const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
{
	return Node.Pins.ContainsByPredicate(
		[Direction](const UEdGraphPin* Pin)
		{ return Pin != nullptr && Pin->Direction == Direction && IsExecutionPin(*Pin) && !Pin->LinkedTo.IsEmpty(); }
	);
}

void AddStringOption(const TSharedRef<FJsonObject>& Options, const TCHAR* Key, const FString& Value)
{ Options->SetStringField(Key, Value); }

void AddNumberOption(const TSharedRef<FJsonObject>& Options, const TCHAR* Key, const double Value)
{ Options->SetStringField(Key, FString::SanitizeFloat(Value)); }

[[nodiscard]]
double RoundUpToGrid(const double Value, const double GridSize) noexcept
{ return FMath::CeilToDouble(Value / GridSize) * GridSize; }

[[nodiscard]]
TSharedRef<FJsonObject> MakeRootOptions(const UFormatterSettings& Settings)
{
	const double GridSize = FMath::Max(1.0, static_cast<double>(Settings.K2LayoutCellSize));
	const double HorizontalSpacing =
		RoundUpToGrid(FMath::Max(static_cast<double>(Settings.K2HorizontalSpacing), GridSize) + GridSize, GridSize);
	const double VerticalSpacing =
		RoundUpToGrid(FMath::Max(static_cast<double>(Settings.K2VerticalSpacing), GridSize) + GridSize, GridSize);
	const double ComponentSpacing =
		RoundUpToGrid(FMath::Max(static_cast<double>(Settings.K2ComponentSpacing), GridSize * 2.0) + GridSize, GridSize);
	const double EdgeNodeSpacing = FMath::Max(static_cast<double>(Settings.K2ObstacleClearance), GridSize * 0.5);
	const double EdgeEdgeSpacing = FMath::Max(static_cast<double>(Settings.K2RoutingChannelSpacing), GridSize * 0.25);

	const TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
	AddStringOption(Options, TEXT("org.eclipse.elk.algorithm"), TEXT("layered"));
	AddStringOption(Options, TEXT("org.eclipse.elk.direction"), TEXT("RIGHT"));
	AddStringOption(Options, TEXT("org.eclipse.elk.edgeRouting"), TEXT("ORTHOGONAL"));
	AddStringOption(Options, TEXT("org.eclipse.elk.separateConnectedComponents"), TEXT("false"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.layering.strategy"), TEXT("NETWORK_SIMPLEX"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.cycleBreaking.strategy"), TEXT("MODEL_ORDER"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.crossingMinimization.strategy"), TEXT("LAYER_SWEEP"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.crossingMinimization.greedySwitch.type"), TEXT("TWO_SIDED"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.nodePlacement.strategy"), TEXT("BRANDES_KOEPF"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.nodePlacement.favorStraightEdges"), TEXT("true"));
	AddStringOption(
		Options, TEXT("org.eclipse.elk.layered.nodePlacement.bk.edgeStraightening"), TEXT("IMPROVE_STRAIGHTNESS")
	);
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.considerModelOrder.strategy"), TEXT("NODES_AND_EDGES"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.considerModelOrder.portModelOrder"), TEXT("true"));
	AddStringOption(Options, TEXT("org.eclipse.elk.layered.considerModelOrder.components"), TEXT("FORCE_MODEL_ORDER"));
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.considerModelOrder.crossingCounterNodeInfluence"), 0.001);
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.considerModelOrder.crossingCounterPortInfluence"), 0.001);
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.thoroughness"), 20.0);
	AddNumberOption(Options, TEXT("org.eclipse.elk.spacing.nodeNode"), VerticalSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.spacing.nodeNodeBetweenLayers"), HorizontalSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.spacing.componentComponent"), ComponentSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.spacing.edgeNode"), EdgeNodeSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.spacing.edgeNodeBetweenLayers"), EdgeNodeSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.spacing.edgeEdge"), EdgeEdgeSpacing);
	AddNumberOption(Options, TEXT("org.eclipse.elk.layered.spacing.edgeEdgeBetweenLayers"), EdgeEdgeSpacing);
	AddStringOption(Options, TEXT("org.eclipse.elk.padding"), TEXT("[top=0,left=0,bottom=0,right=0]"));
	return Options;
}

[[nodiscard]]
TSharedRef<FJsonObject> MakeNodeJson(
	const FElkNodeBinding& Binding,
	const K2::FGraphGeometrySnapshot& Geometry,
	TMap<const UEdGraphPin*, FString>& OutPortIds,
	int32& OutSubmittedPorts,
	int32& OutMeasuredPorts
)
{
	const TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
	NodeJson->SetStringField(TEXT("id"), Binding.Id);
	NodeJson->SetNumberField(TEXT("x"), Binding.Node->NodePosX);
	NodeJson->SetNumberField(TEXT("y"), Binding.Node->NodePosY);
	NodeJson->SetNumberField(TEXT("width"), Binding.Size.X);
	NodeJson->SetNumberField(TEXT("height"), Binding.Size.Y);

	const TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
	AddStringOption(Options, TEXT("org.eclipse.elk.portConstraints"), TEXT("FIXED_POS"));
	const bool bHasIncomingExecution = HasLinkedExecutionPin(*Binding.Node, EGPD_Input);
	const bool bHasOutgoingExecution = HasLinkedExecutionPin(*Binding.Node, EGPD_Output);
	if (!bHasIncomingExecution && bHasOutgoingExecution)
	{
		AddStringOption(Options, TEXT("org.eclipse.elk.layered.layering.layerConstraint"), TEXT("FIRST"));
	}
	else if (bHasIncomingExecution && !bHasOutgoingExecution)
	{
		AddStringOption(Options, TEXT("org.eclipse.elk.layered.layering.layerConstraint"), TEXT("LAST"));
	}
	NodeJson->SetObjectField(TEXT("layoutOptions"), Options);

	TArray<TSharedPtr<FJsonValue>> Ports;
	for (UEdGraphPin* Pin : Binding.Node->Pins)
	{
		if (Pin == nullptr || Pin->LinkedTo.IsEmpty()) { continue; }
		bool bMeasured = false;
		const FVector2D Offset = ResolvePinOffset(*Pin, Binding.Size, Geometry, bMeasured);
		const FString PortId = FString::Printf(TEXT("%s_p%d"), *Binding.Id, Ports.Num());
		OutPortIds.Add(Pin, PortId);

		const TSharedRef<FJsonObject> PortJson = MakeShared<FJsonObject>();
		PortJson->SetStringField(TEXT("id"), PortId);
		PortJson->SetNumberField(
			TEXT("x"), Pin->Direction == EGPD_Input ? -PortExtent * 0.5 : Binding.Size.X - PortExtent * 0.5
		);
		PortJson->SetNumberField(
			TEXT("y"), FMath::Clamp(Offset.Y - PortExtent * 0.5, -PortExtent * 0.5, Binding.Size.Y - PortExtent * 0.5)
		);
		PortJson->SetNumberField(TEXT("width"), PortExtent);
		PortJson->SetNumberField(TEXT("height"), PortExtent);
		const TSharedRef<FJsonObject> PortOptions = MakeShared<FJsonObject>();
		AddStringOption(
			PortOptions, TEXT("org.eclipse.elk.port.side"), Pin->Direction == EGPD_Input ? TEXT("WEST") : TEXT("EAST")
		);
		AddNumberOption(PortOptions, TEXT("org.eclipse.elk.port.index"), DirectionOrdinal(*Pin));
		PortJson->SetObjectField(TEXT("layoutOptions"), PortOptions);
		Ports.Add(MakeShared<FJsonValueObject>(PortJson));
		++OutSubmittedPorts;
		OutMeasuredPorts += bMeasured ? 1 : 0;
	}
	NodeJson->SetArrayField(TEXT("ports"), Ports);
	return NodeJson;
}

[[nodiscard]]
FElkProblem BuildProblem(UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, const UFormatterSettings& Settings)
{
	FElkProblem Problem;
	TArray<UEdGraphNode*> OrderedNodes;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node != nullptr && !Node->IsA<UEdGraphNode_Comment>()) { OrderedNodes.Add(Node); }
	}
	OrderedNodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			if (Left.NodePosY != Right.NodePosY) { return Left.NodePosY < Right.NodePosY; }
			if (Left.NodePosX != Right.NodePosX) { return Left.NodePosX < Right.NodePosX; }
			return NodeStableKey(Left) < NodeStableKey(Right);
		}
	);

	TMap<const UEdGraphNode*, FString> NodeIds;
	TArray<TSharedPtr<FJsonValue>> Children;
	TMap<const UEdGraphPin*, FString> PortIds;
	for (int32 Index = 0; Index < OrderedNodes.Num(); ++Index)
	{
		FElkNodeBinding& Binding = Problem.Nodes.AddDefaulted_GetRef();
		Binding.Id = FString::Printf(TEXT("n%d"), Index);
		Binding.Node = OrderedNodes[Index];
		Binding.Size = ResolveNodeSize(*Binding.Node, Geometry);
		NodeIds.Add(Binding.Node, Binding.Id);
		Children.Add(
			MakeShared<FJsonValueObject>(MakeNodeJson(Binding, Geometry, PortIds, Problem.SubmittedPorts, Problem.MeasuredPorts))
		);
	}

	TArray<TSharedPtr<FJsonValue>> Edges;
	for (FElkNodeBinding& NodeBinding : Problem.Nodes)
	{
		for (UEdGraphPin* OutputPin : NodeBinding.Node->Pins)
		{
			if (OutputPin == nullptr || OutputPin->Direction != EGPD_Output) { continue; }
			for (UEdGraphPin* InputPin : OutputPin->LinkedTo)
			{
				if (InputPin == nullptr || InputPin->Direction != EGPD_Input
					|| !NodeIds.Contains(InputPin->GetOwningNodeUnchecked()))
				{
					continue;
				}
				const FString* SourcePort = PortIds.Find(OutputPin);
				const FString* TargetPort = PortIds.Find(InputPin);
				if (SourcePort == nullptr || TargetPort == nullptr) { continue; }

				FElkEdgeBinding& EdgeBinding = Problem.Edges.AddDefaulted_GetRef();
				EdgeBinding.Id = FString::Printf(TEXT("e%d"), Problem.Edges.Num() - 1);
				EdgeBinding.OutputPin = OutputPin;
				EdgeBinding.InputPin = InputPin;
				EdgeBinding.bExecution = IsExecutionPin(*OutputPin) && IsExecutionPin(*InputPin);

				const TSharedRef<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
				EdgeJson->SetStringField(TEXT("id"), EdgeBinding.Id);
				EdgeJson->SetArrayField(TEXT("sources"), { MakeShared<FJsonValueString>(*SourcePort) });
				EdgeJson->SetArrayField(TEXT("targets"), { MakeShared<FJsonValueString>(*TargetPort) });
				const TSharedRef<FJsonObject> EdgeOptions = MakeShared<FJsonObject>();
				AddNumberOption(
					EdgeOptions,
					TEXT("org.eclipse.elk.layered.priority.direction"),
					EdgeBinding.bExecution ? ExecutionEdgePriority : DataEdgePriority
				);
				AddNumberOption(
					EdgeOptions,
					TEXT("org.eclipse.elk.layered.priority.straightness"),
					EdgeBinding.bExecution ? ExecutionEdgePriority : DataEdgePriority
				);
				EdgeJson->SetObjectField(TEXT("layoutOptions"), EdgeOptions);
				Edges.Add(MakeShared<FJsonValueObject>(EdgeJson));
			}
		}
	}

	Problem.Json->SetStringField(TEXT("id"), TEXT("root"));
	Problem.Json->SetObjectField(TEXT("layoutOptions"), MakeRootOptions(Settings));
	Problem.Json->SetArrayField(TEXT("children"), Children);
	Problem.Json->SetArrayField(TEXT("edges"), Edges);
	return Problem;
}

[[nodiscard]]
bool SerializeJson(const TSharedRef<FJsonObject>& Json, FString& OutPayload)
{
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutPayload);
	return FJsonSerializer::Serialize(Json, Writer);
}

[[nodiscard]]
FElkFiles MakeFiles(const FString& ArtifactDirectory)
{
	FElkFiles Files;
	Files.bPersistent = !ArtifactDirectory.IsEmpty();
	const FString RequestedDirectory =
		Files.bPersistent ? ArtifactDirectory
						  : FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("GraphFormatter"), TEXT("Elk"));
	Files.Directory = FPaths::ConvertRelativePathToFull(RequestedDirectory);
	FPaths::NormalizeDirectoryName(Files.Directory);
	IFileManager::Get().MakeDirectory(*Files.Directory, true);
	const FString Stem = Files.bPersistent ? TEXT("elk") : TEXT("elk-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Files.Input = FPaths::Combine(Files.Directory, Stem + TEXT("_input.json"));
	Files.Output = FPaths::Combine(Files.Directory, Stem + TEXT("_output.json"));
	Files.ProcessLog = FPaths::Combine(Files.Directory, Stem + TEXT("_process.txt"));
	return Files;
}

void CleanupDisposableFiles(const FElkFiles& Files)
{
	if (Files.bPersistent) { return; }
	IFileManager::Get().Delete(*Files.Input, false, true);
	IFileManager::Get().Delete(*Files.Output, false, true);
	IFileManager::Get().Delete(*Files.ProcessLog, false, true);
}

[[nodiscard]]
bool FindElkResources(FString& OutScript, FString& OutWorkingDirectory, FString& OutError)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GraphFormatter"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("The GraphFormatter plugin directory could not be resolved.");
		return false;
	}
	OutWorkingDirectory =
		FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("ElkJs")));
	FPaths::NormalizeDirectoryName(OutWorkingDirectory);
	OutScript = FPaths::Combine(OutWorkingDirectory, TEXT("run-layout.cjs"));
	const FString Bundle = FPaths::Combine(OutWorkingDirectory, TEXT("package"), TEXT("lib"), TEXT("elk.bundled.js"));
	if (!FPaths::FileExists(OutScript) || !FPaths::FileExists(Bundle))
	{
		OutError =
			FString::Printf(TEXT("elkjs %s resources are incomplete beneath '%s'."), ElkJsVersion, *OutWorkingDirectory);
		return false;
	}
	return true;
}

[[nodiscard]]
FString ResolveNodeExecutable()
{
	FString Executable = FPlatformMisc::GetEnvironmentVariable(TEXT("GRAPHFORMATTER_ELK_NODE"));
	Executable.TrimStartAndEndInline();
	return Executable.IsEmpty() ? TEXT("node") : Executable;
}

[[nodiscard]]
bool RunProcess(
	const FString& NodeExecutable, const FString& Script, const FElkFiles& Files, const FString& WorkingDirectory, FString& OutError
)
{
	const FString Arguments = FString::Printf(TEXT("\"%s\" \"%s\" \"%s\""), *Script, *Files.Input, *Files.Output);
	int32 ReturnCode = INDEX_NONE;
	FString StdOut;
	FString StdErr;
	const bool bLaunched =
		FPlatformProcess::ExecProcess(*NodeExecutable, *Arguments, &ReturnCode, &StdOut, &StdErr, *WorkingDirectory, true);
	const FString ProcessLog = FString::Printf(
		TEXT("executable=%s\nreturn_code=%d\n\n[stdout]\n%s\n\n[stderr]\n%s\n"), *NodeExecutable, ReturnCode, *StdOut, *StdErr
	);
	FFileHelper::SaveStringToFile(ProcessLog, *Files.ProcessLog, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bLaunched)
	{
		OutError = FString::Printf(
			TEXT("Could not launch '%s'. Install Node.js or set GRAPHFORMATTER_ELK_NODE to its executable path."), *NodeExecutable
		);
		return false;
	}
	if (ReturnCode != 0 || !FPaths::FileExists(Files.Output))
	{
		OutError = FString::Printf(
			TEXT("elkjs %s exited with code %d%s%s."),
			ElkJsVersion,
			ReturnCode,
			StdErr.IsEmpty() ? TEXT("") : TEXT(": "),
			*StdErr.TrimStartAndEnd()
		);
		return false;
	}
	return true;
}

[[nodiscard]]
bool LoadJson(const FString& Filename, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	FString Payload;
	if (!FFileHelper::LoadFileToString(Payload, *Filename))
	{
		OutError = FString::Printf(TEXT("Could not read ELK output '%s'."), *Filename);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not parse ELK output '%s'."), *Filename);
		return false;
	}
	return true;
}

[[nodiscard]]
bool TryReadPoint(const TSharedPtr<FJsonObject>& Json, FVector2D& OutPoint)
{
	if (!Json.IsValid()) { return false; }
	double X = 0.0;
	double Y = 0.0;
	if (!Json->TryGetNumberField(TEXT("x"), X) || !Json->TryGetNumberField(TEXT("y"), Y)) { return false; }
	OutPoint = FVector2D(X, Y);
	return true;
}

[[nodiscard]]
double Snap(const double Value, const double GridSize) noexcept
{ return FMath::RoundToDouble(Value / GridSize) * GridSize; }

[[nodiscard]]
bool ApplyNodePositions(
	const TSharedPtr<FJsonObject>& Output,
	TArray<FElkNodeBinding>& Bindings,
	const double GridSize,
	FVector2D& OutTranslation,
	int32& OutPositionedNodes,
	FString& OutError
)
{
	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (!Output->TryGetArrayField(TEXT("children"), Children) || Children == nullptr)
	{
		OutError = TEXT("ELK output contains no child-node array.");
		return false;
	}

	TMap<FString, FVector2D> Positions;
	FVector2D MinimumOutput(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
	{
		const TSharedPtr<FJsonObject> Child = ChildValue.IsValid() ? ChildValue->AsObject() : nullptr;
		if (!Child.IsValid()) { continue; }
		FString Id;
		double X = 0.0;
		double Y = 0.0;
		if (!Child->TryGetStringField(TEXT("id"), Id) || !Child->TryGetNumberField(TEXT("x"), X)
			|| !Child->TryGetNumberField(TEXT("y"), Y))
		{
			continue;
		}
		Positions.Add(Id, FVector2D(X, Y));
		MinimumOutput.X = FMath::Min(MinimumOutput.X, X);
		MinimumOutput.Y = FMath::Min(MinimumOutput.Y, Y);
	}
	if (Positions.Num() != Bindings.Num())
	{
		OutError =
			FString::Printf(TEXT("ELK returned positions for %d/%d submitted nodes."), Positions.Num(), Bindings.Num());
		return false;
	}

	FVector2D MinimumOriginal(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	for (const FElkNodeBinding& Binding : Bindings)
	{
		MinimumOriginal.X = FMath::Min(MinimumOriginal.X, static_cast<double>(Binding.Node->NodePosX));
		MinimumOriginal.Y = FMath::Min(MinimumOriginal.Y, static_cast<double>(Binding.Node->NodePosY));
	}
	const FVector2D SnappedAnchor(Snap(MinimumOriginal.X, GridSize), Snap(MinimumOriginal.Y, GridSize));
	OutTranslation = SnappedAnchor - MinimumOutput;
	OutPositionedNodes = 0;
	for (FElkNodeBinding& Binding : Bindings)
	{
		const FVector2D* Position = Positions.Find(Binding.Id);
		if (Position == nullptr) { continue; }
		Binding.ElkPosition = *Position + OutTranslation;
		Binding.Node->NodePosX = FMath::RoundToInt32(Snap(Binding.ElkPosition.X, GridSize));
		Binding.Node->NodePosY = FMath::RoundToInt32(Snap(Binding.ElkPosition.Y, GridSize));
		++OutPositionedNodes;
	}
	return OutPositionedNodes == Bindings.Num();
}

[[nodiscard]]
bool IsHorizontal(const FVector2D First, const FVector2D Second) noexcept
{ return FMath::IsNearlyEqual(First.Y, Second.Y, 0.5); }

[[nodiscard]]
bool IsVertical(const FVector2D First, const FVector2D Second) noexcept
{ return FMath::IsNearlyEqual(First.X, Second.X, 0.5); }

[[nodiscard]]
TArray<FVector2D> SimplifyOrthogonalPolyline(TArray<FVector2D> Points)
{
	TArray<FVector2D> Result;
	for (const FVector2D Point : Points)
	{
		if (!Result.IsEmpty() && Result.Last().Equals(Point, 0.5)) { continue; }
		Result.Add(Point);
		while (Result.Num() >= 3)
		{
			const int32 Last = Result.Num() - 1;
			if ((!IsHorizontal(Result[Last - 2], Result[Last - 1]) || !IsHorizontal(Result[Last - 1], Result[Last]))
				&& (!IsVertical(Result[Last - 2], Result[Last - 1]) || !IsVertical(Result[Last - 1], Result[Last])))
			{
				break;
			}
			Result.RemoveAt(Last - 1);
		}
	}
	return Result;
}

[[nodiscard]]
TArray<FVector2D> RetargetRoute(
	TArray<FVector2D> Points, const FVector2D OutputAnchor, const FVector2D InputAnchor, const double GridSize
)
{
	if (Points.Num() < 2) { return {}; }
	if (Points.Num() == 2 && !FMath::IsNearlyEqual(OutputAnchor.Y, InputAnchor.Y, 0.5))
	{
		double MiddleX = Snap((OutputAnchor.X + InputAnchor.X) * 0.5, GridSize);
		if (FMath::IsNearlyEqual(MiddleX, OutputAnchor.X, 0.5) || FMath::IsNearlyEqual(MiddleX, InputAnchor.X, 0.5))
		{
			MiddleX = (OutputAnchor.X + InputAnchor.X) * 0.5;
		}
		return SimplifyOrthogonalPolyline(
			{ OutputAnchor, FVector2D(MiddleX, OutputAnchor.Y), FVector2D(MiddleX, InputAnchor.Y), InputAnchor }
		);
	}

	const FVector2D OriginalStart = Points[0];
	const FVector2D OriginalSecond = Points[1];
	const FVector2D OriginalBeforeEnd = Points[Points.Num() - 2];
	const FVector2D OriginalEnd = Points.Last();
	Points[0] = OutputAnchor;
	Points.Last() = InputAnchor;
	if (IsHorizontal(OriginalStart, OriginalSecond)) { Points[1].Y = OutputAnchor.Y; }
	else if (IsVertical(OriginalStart, OriginalSecond)) { Points[1].X = OutputAnchor.X; }
	if (IsHorizontal(OriginalBeforeEnd, OriginalEnd)) { Points[Points.Num() - 2].Y = InputAnchor.Y; }
	else if (IsVertical(OriginalBeforeEnd, OriginalEnd)) { Points[Points.Num() - 2].X = InputAnchor.X; }
	return SimplifyOrthogonalPolyline(MoveTemp(Points));
}

[[nodiscard]]
TArray<FVector2D> ReadSectionPoints(const TSharedPtr<FJsonObject>& Section, const FVector2D Translation)
{
	TArray<FVector2D> Points;
	if (!Section.IsValid()) { return Points; }
	FVector2D Point;
	const TSharedPtr<FJsonObject>* StartPoint = nullptr;
	if (!Section->TryGetObjectField(TEXT("startPoint"), StartPoint) || StartPoint == nullptr
		|| !TryReadPoint(*StartPoint, Point))
	{
		return {};
	}
	Points.Add(Point + Translation);
	const TArray<TSharedPtr<FJsonValue>>* BendPoints = nullptr;
	if (Section->TryGetArrayField(TEXT("bendPoints"), BendPoints) && BendPoints != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& BendValue : *BendPoints)
		{
			if (TryReadPoint(BendValue.IsValid() ? BendValue->AsObject() : nullptr, Point))
			{
				Points.Add(Point + Translation);
			}
		}
	}
	const TSharedPtr<FJsonObject>* EndPoint = nullptr;
	if (!Section->TryGetObjectField(TEXT("endPoint"), EndPoint) || EndPoint == nullptr || !TryReadPoint(*EndPoint, Point))
	{
		return {};
	}
	Points.Add(Point + Translation);
	return Points;
}

[[nodiscard]]
K2::FRerouteResult ApplyEdgeRoutes(
	UEdGraph& Graph,
	const TSharedPtr<FJsonObject>& Output,
	const TArray<FElkEdgeBinding>& Bindings,
	const K2::FGraphGeometrySnapshot& Geometry,
	const UFormatterSettings& Settings,
	const FVector2D Translation,
	int32& OutEdgesWithRoutes,
	int32& OutKnotLimitRejections
)
{
	OutEdgesWithRoutes = 0;
	OutKnotLimitRejections = 0;
	TMap<FString, const FElkEdgeBinding*> BindingById;
	for (const FElkEdgeBinding& Binding : Bindings)
	{
		BindingById.Add(Binding.Id, &Binding);
	}

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	if (!Output->TryGetArrayField(TEXT("edges"), Edges) || Edges == nullptr) { return {}; }
	K2::FReroutePlan Plan;
	for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
	{
		const TSharedPtr<FJsonObject> EdgeJson = EdgeValue.IsValid() ? EdgeValue->AsObject() : nullptr;
		if (!EdgeJson.IsValid()) { continue; }
		FString Id;
		if (!EdgeJson->TryGetStringField(TEXT("id"), Id)) { continue; }
		const FElkEdgeBinding* const* BindingPointer = BindingById.Find(Id);
		if (BindingPointer == nullptr || *BindingPointer == nullptr) { continue; }
		const FElkEdgeBinding& Binding = **BindingPointer;

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!EdgeJson->TryGetArrayField(TEXT("sections"), Sections) || Sections == nullptr || Sections->Num() != 1)
		{
			continue;
		}
		TArray<FVector2D> Points = ReadSectionPoints((*Sections)[0]->AsObject(), Translation);
		if (Points.Num() < 2) { continue; }
		++OutEdgesWithRoutes;
		const FVector2D OutputAnchor = ResolvePinAnchor(*Binding.OutputPin, Geometry);
		const FVector2D InputAnchor = ResolvePinAnchor(*Binding.InputPin, Geometry);
		Points = RetargetRoute(MoveTemp(Points), OutputAnchor, InputAnchor, Settings.K2LayoutCellSize);
		if (Points.Num() <= 2) { continue; }
		const int32 KnotCount = Points.Num() - 2;
		if (KnotCount > Settings.K2MaxGeneratedKnots)
		{
			++OutKnotLimitRejections;
			continue;
		}

		K2::FPlannedReroute& Route = Plan.Routes.AddDefaulted_GetRef();
		Route.Edge.OutputPin = Binding.OutputPin;
		Route.Edge.InputPin = Binding.InputPin;
		Route.Edge.OutputAnchor = OutputAnchor;
		Route.Edge.InputAnchor = InputAnchor;
		Route.Edge.StableKey = TEXT("elk:") + Binding.Id;
		Route.Edge.bExecution = Binding.bExecution;
		Route.Waypoints.Append(&Points[1], KnotCount);
		Route.OutputNode = Binding.OutputPin->GetOwningNodeUnchecked();
		Route.InputNode = Binding.InputPin->GetOwningNodeUnchecked();
		Route.OutputPinId = Binding.OutputPin->PinId;
		Route.InputPinId = Binding.InputPin->PinId;
	}
	return K2::FK2RerouteRouter::ApplyPlan(Graph, Plan);
}
} // namespace ElkLayoutAdapterPrivate

using namespace ElkLayoutAdapterPrivate;

FElkLayoutResult RunElkLayeredLayout(
	UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, const UFormatterSettings& Settings, const FString& ArtifactDirectory
)
{
	FElkLayoutResult Result;
	FElkProblem Problem = BuildProblem(Graph, Geometry, Settings);
	Result.SubmittedPorts = Problem.SubmittedPorts;
	Result.MeasuredPorts = Problem.MeasuredPorts;
	Result.SubmittedEdges = Problem.Edges.Num();
	if (Problem.Nodes.Num() < 2)
	{
		Result.Diagnostics.Add(TEXT("ELK needs at least two non-comment graph nodes."));
		return Result;
	}

	FString InputPayload;
	if (!SerializeJson(Problem.Json, InputPayload))
	{
		Result.Diagnostics.Add(TEXT("Could not serialize the ELK input graph."));
		return Result;
	}
	const FElkFiles Files = MakeFiles(ArtifactDirectory);
	if (!FFileHelper::SaveStringToFile(InputPayload, *Files.Input, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.Diagnostics.Add(FString::Printf(TEXT("Could not write ELK input '%s'."), *Files.Input));
		CleanupDisposableFiles(Files);
		return Result;
	}

	FString Script;
	FString WorkingDirectory;
	FString Error;
	if (!FindElkResources(Script, WorkingDirectory, Error))
	{
		Result.Diagnostics.Add(MoveTemp(Error));
		CleanupDisposableFiles(Files);
		return Result;
	}
	Result.NodeExecutable = ResolveNodeExecutable();
	if (!RunProcess(Result.NodeExecutable, Script, Files, WorkingDirectory, Error))
	{
		Result.Diagnostics.Add(MoveTemp(Error));
		CleanupDisposableFiles(Files);
		return Result;
	}

	TSharedPtr<FJsonObject> Output;
	if (!LoadJson(Files.Output, Output, Error))
	{
		Result.Diagnostics.Add(MoveTemp(Error));
		CleanupDisposableFiles(Files);
		return Result;
	}
	FVector2D Translation = FVector2D::ZeroVector;
	if (!ApplyNodePositions(
			Output, Problem.Nodes, FMath::Max(1.0, static_cast<double>(Settings.K2LayoutCellSize)), Translation, Result.PositionedNodes, Error
		))
	{
		Result.Diagnostics.Add(MoveTemp(Error));
		CleanupDisposableFiles(Files);
		return Result;
	}

	const K2::FRerouteResult RouteResult = ApplyEdgeRoutes(
		Graph, Output, Problem.Edges, Geometry, Settings, Translation, Result.EdgesWithRoutes, Result.KnotLimitRejections
	);
	Result.MaterializedRoutes = RouteResult.RoutedWires;
	Result.CreatedKnots = RouteResult.CreatedKnots;
	Result.Diagnostics.Append(RouteResult.Diagnostics);
	Result.bSucceeded = Result.PositionedNodes == Problem.Nodes.Num() && !RouteResult.HasFatalError();
	Result.Diagnostics.Add(
		FString::Printf(
			TEXT("elkjs %s positioned %d nodes with %d/%d measured linked ports; returned %d/%d edge routes and materialized %d routes (%d knots, %d over the knot cap)."),
			ElkJsVersion,
			Result.PositionedNodes,
			Result.MeasuredPorts,
			Result.SubmittedPorts,
			Result.EdgesWithRoutes,
			Result.SubmittedEdges,
			Result.MaterializedRoutes,
			Result.CreatedKnots,
			Result.KnotLimitRejections
		)
	);
	CleanupDisposableFiles(Files);
	return Result;
}
} // namespace GraphFormatter::Benchmark
