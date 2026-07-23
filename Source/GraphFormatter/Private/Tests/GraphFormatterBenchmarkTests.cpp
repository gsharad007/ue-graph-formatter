/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Benchmark/GraphFormatterBenchmark.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Benchmark/ElkLayoutAdapter.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "FormatterSettings.h"
#include "GraphFormatterAdaptagrams.h"
#include "HAL/FileManager.h"
#include "K2/GraphGeometrySnapshot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace GraphFormatter::Benchmark::Tests
{
namespace BenchmarkTestsPrivate
{
[[nodiscard]]
UEdGraph* FindGraph(UBlueprint& Blueprint, const FName GraphName)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	UEdGraph** Match = Graphs.FindByPredicate([GraphName](const UEdGraph* Graph)
											  { return Graph != nullptr && Graph->GetFName() == GraphName; });
	return Match != nullptr ? *Match : nullptr;
}

[[nodiscard]]
FString CaptureSourceState(const UEdGraph& Graph)
{
	TArray<FString> State;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		State.Add(
			FString::Printf(
				TEXT("%s|%s|%d|%d|%d|%d"),
				*Node->NodeGuid.ToString(EGuidFormats::Digits),
				*Node->GetClass()->GetPathName(),
				Node->NodePosX,
				Node->NodePosY,
				Node->NodeWidth,
				Node->NodeHeight
			)
		);
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output) { continue; }
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin == nullptr) { continue; }
				State.Add(
					FString::Printf(
						TEXT("%s->%s"),
						*Pin->PinId.ToString(EGuidFormats::Digits),
						*LinkedPin->PinId.ToString(EGuidFormats::Digits)
					)
				);
			}
		}
	}
	State.Sort();
	return FString::Join(State, TEXT("\n"));
}

[[nodiscard]]
K2::FGraphGeometrySnapshot BuildDeterministicTestGeometry(UEdGraph& Graph)
{
	K2::FGraphGeometrySnapshot Geometry;
	Geometry.Status = K2::EGraphGeometrySnapshotStatus::Ready;
	int32 NodeIndex = 0;
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		const double Width = Node->NodeWidth > 0 ? static_cast<double>(Node->NodeWidth)
												 : 176.0 + static_cast<double>(NodeIndex % 3) * 24.0;
		const double Height = Node->NodeHeight > 0
								? static_cast<double>(Node->NodeHeight)
								: FMath::Max(80.0, 48.0 + static_cast<double>(FMath::Max(1, Node->Pins.Num())) * 20.0);
		const FVector2D Position(Node->NodePosX, Node->NodePosY);

		K2::FGraphNodeGeometrySnapshot NodeGeometry;
		NodeGeometry.NodeGuid = Node->NodeGuid;
		NodeGeometry.PersistedPosition = Position;
		NodeGeometry.Position = Position;
		NodeGeometry.Bounds = FSlateRect::FromPointAndExtent(Position, FVector2D(Width, Height));
		NodeGeometry.PositionSource = K2::EGraphNodePositionSource::PersistedNode;
		NodeGeometry.SizeSource = K2::EGraphNodeSizeSource::DesiredSize;
		Geometry.Nodes.Add(Node, MoveTemp(NodeGeometry));
		++Geometry.RequestedNodeCount;
		++Geometry.CapturedNodeBoundsCount;

		int32 InputOrdinal = 0;
		int32 OutputOrdinal = 0;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr) { continue; }
			const int32 Ordinal = Pin->Direction == EGPD_Input ? InputOrdinal++ : OutputOrdinal++;
			const double PinY = FMath::Clamp(32.0 + static_cast<double>(Ordinal) * 20.0, 12.0, Height - 12.0);
			const FVector2D Offset(Pin->Direction == EGPD_Input ? 0.0 : Width, PinY);
			K2::FGraphPinGeometrySnapshot PinGeometry;
			PinGeometry.PinId = Pin->PinId;
			PinGeometry.NodeGuid = Node->NodeGuid;
			PinGeometry.NodeOffset = Offset;
			PinGeometry.Anchor = Position + Offset;
			Geometry.Pins.Add(Pin, MoveTemp(PinGeometry));
			++Geometry.RequestedVisiblePinCount;
			++Geometry.CapturedPinCount;
		}
		++NodeIndex;
	}
	return Geometry;
}

[[nodiscard]]
bool IsElkRuntimeUnavailable(const TConstArrayView<FString> Diagnostics)
{
	return Diagnostics.ContainsByPredicate([](const FString& Diagnostic)
										   { return Diagnostic.Contains(TEXT("Could not launch")); });
}

[[nodiscard]]
bool SegmentEntersObstacle(const FVector2D Start, const FVector2D End, const FGraphFormatterAdaptagramsObstacle& Obstacle)
{
	const bool bHorizontal = FMath::IsNearlyEqual(Start.Y, End.Y, 0.01);
	const bool bVertical = FMath::IsNearlyEqual(Start.X, End.X, 0.01);
	if (bHorizontal)
	{
		return Start.Y > Obstacle.Minimum.Y && Start.Y < Obstacle.Maximum.Y
			&& FMath::Max(Start.X, End.X) > Obstacle.Minimum.X && FMath::Min(Start.X, End.X) < Obstacle.Maximum.X;
	}
	if (bVertical)
	{
		return Start.X > Obstacle.Minimum.X && Start.X < Obstacle.Maximum.X
			&& FMath::Max(Start.Y, End.Y) > Obstacle.Minimum.Y && FMath::Min(Start.Y, End.Y) < Obstacle.Maximum.Y;
	}
	return true;
}

struct FScopedTestDirectory
{
	explicit FScopedTestDirectory(FString InPath)
		: Path(MoveTemp(InPath))
	{
	}

	~FScopedTestDirectory() { IFileManager::Get().DeleteDirectory(*Path, false, true); }

	FString Path;
};
} // namespace BenchmarkTestsPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphFormatterBenchmarkBackendsTest,
	"Project.Unit Tests.GraphFormatter.Benchmark.Backends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FGraphFormatterBenchmarkBackendsTest::RunTest(const FString& Parameters)
{
	using namespace BenchmarkTestsPrivate;
	UBlueprint* SourceBlueprint =
		LoadObject<UBlueprint>(nullptr, TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier"));
	if (!TestNotNull(TEXT("loads the representative ResourceCarrier Blueprint"), SourceBlueprint)) { return false; }
	UEdGraph* SourceGraph = FindGraph(*SourceBlueprint, TEXT("DropHeldActor"));
	if (!TestNotNull(TEXT("finds the representative DropHeldActor graph"), SourceGraph)) { return false; }

	const FString SourceStateBefore = CaptureSourceState(*SourceGraph);
	UPackage* SourcePackage = SourceBlueprint->GetOutermost();
	const bool bSourcePackageWasDirty = SourcePackage != nullptr && SourcePackage->IsDirty();
	const K2::FGraphGeometrySnapshot Geometry = BuildDeterministicTestGeometry(*SourceGraph);
	FString Error;
	TSharedPtr<FGraphFormatterBenchmarkRun> Run =
		FGraphFormatterBenchmark::CreateHeadlessRun(*SourceBlueprint, *SourceGraph, Geometry, Error);
	if (!TestTrue(*FString::Printf(TEXT("creates the full comparison run: %s"), *Error), Run.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("creates exactly four blinded candidates"), Run->Candidates.Num(), 4);
	TSet<EFormatterBackend> SeenBackends;
	TSet<FString> SeenLabels;
	for (const FBenchmarkCandidate& Candidate : Run->Candidates)
	{
		SeenBackends.Add(Candidate.Backend);
		SeenLabels.Add(Candidate.BlindLabel);
		TestNotNull(*FString::Printf(TEXT("%s has a transient graph"), DescribeBackend(Candidate.Backend)), Candidate.Graph);
		const bool bElkRuntimeUnavailable = Candidate.Backend == EFormatterBackend::ElkLayered
										 && IsElkRuntimeUnavailable(Candidate.Diagnostics);
		if (bElkRuntimeUnavailable)
		{
			AddWarning(TEXT("ELK benchmark assertions were skipped because its optional Node.js runtime is unavailable."));
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s completes: %s"),
					DescribeBackend(Candidate.Backend),
					*FString::Join(Candidate.Diagnostics, TEXT(" | "))
				),
				Candidate.bSucceeded
			);
		}
		TestTrue(
			*FString::Printf(TEXT("%s preserves logical topology"), DescribeBackend(Candidate.Backend)),
			Candidate.bTopologyPreserved
		);
		TestTrue(
			*FString::Printf(TEXT("%s produces finite quality metrics"), DescribeBackend(Candidate.Backend)),
			FMath::IsFinite(Candidate.Metrics.CompositePenalty) && Candidate.Metrics.SemanticNodeCount >= 2
		);
		TestFalse(
			*FString::Printf(TEXT("%s records its comparison configuration"), DescribeBackend(Candidate.Backend)),
			Candidate.Configuration.IsEmpty()
		);
		if (Candidate.Backend == EFormatterBackend::GraphFormatterLibavoid)
		{
			const double RequiredRoutes = Candidate.Telemetry.FindRef(TEXT("connections_requiring_routing"));
			const double AcceptedRoutes = Candidate.Telemetry.FindRef(TEXT("libavoid_routes_accepted"));
			const double CreatedKnots = Candidate.Telemetry.FindRef(TEXT("libavoid_knots_created"));
			TestTrue(TEXT("libavoid accepts only defect-triggered routes"), AcceptedRoutes <= RequiredRoutes);
			TestTrue(
				TEXT("libavoid respects the per-wire knot cap"),
				CreatedKnots <= AcceptedRoutes * GetDefault<UFormatterSettings>()->K2MaxGeneratedKnots
			);
		}
		else if (Candidate.Backend == EFormatterBackend::ElkLayered && !bElkRuntimeUnavailable)
		{
			const double SubmittedPorts = Candidate.Telemetry.FindRef(TEXT("ports_submitted"));
			const double MeasuredPorts = Candidate.Telemetry.FindRef(TEXT("measured_port_offsets_used"));
			const double SubmittedEdges = Candidate.Telemetry.FindRef(TEXT("edges_submitted"));
			const double EdgesWithRoutes = Candidate.Telemetry.FindRef(TEXT("elk_edges_with_routes"));
			const double MaterializedRoutes = Candidate.Telemetry.FindRef(TEXT("elk_routes_materialized"));
			const double CreatedKnots = Candidate.Telemetry.FindRef(TEXT("elk_knots_created"));
			TestTrue(TEXT("ELK receives linked pin ports"), SubmittedPorts > 0.0);
			TestEqual(TEXT("ELK receives a captured position for every submitted port"), MeasuredPorts, SubmittedPorts);
			TestTrue(TEXT("ELK receives logical edges"), SubmittedEdges > 0.0);
			TestTrue(TEXT("ELK returns routed sections for the representative graph"), EdgesWithRoutes > 0.0);
			TestTrue(TEXT("ELK returns at most one route per submitted edge"), EdgesWithRoutes <= SubmittedEdges);
			TestTrue(TEXT("ELK materializes at least one orthogonal route"), MaterializedRoutes > 0.0);
			TestTrue(TEXT("ELK materializes at most one route per returned edge"), MaterializedRoutes <= EdgesWithRoutes);
			TestTrue(TEXT("ELK materialized routes contain reroute knots"), CreatedKnots > 0.0);
			TestTrue(
				TEXT("ELK respects the per-wire knot cap"),
				CreatedKnots <= MaterializedRoutes * GetDefault<UFormatterSettings>()->K2MaxGeneratedKnots
			);
		}
	}
	TestEqual(TEXT("contains all four backend identities"), SeenBackends.Num(), 4);
	TestEqual(TEXT("assigns four unique blind labels"), SeenLabels.Num(), 4);
	TestEqual(TEXT("does not mutate the source graph"), CaptureSourceState(*SourceGraph), SourceStateBefore);
	if (SourcePackage != nullptr)
	{
		TestEqual(TEXT("does not change the source package dirty state"), SourcePackage->IsDirty(), bSourcePackageWasDirty);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphFormatterElkAuditArtifactsTest,
	"Project.Unit Tests.GraphFormatter.Benchmark.ElkAuditArtifacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FGraphFormatterElkAuditArtifactsTest::RunTest(const FString& Parameters)
{
	using namespace BenchmarkTestsPrivate;
	UBlueprint* SourceBlueprint =
		LoadObject<UBlueprint>(nullptr, TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier"));
	if (!TestNotNull(TEXT("loads the ELK audit-test Blueprint"), SourceBlueprint)) { return false; }
	UEdGraph* SourceGraph = FindGraph(*SourceBlueprint, TEXT("DropHeldActor"));
	if (!TestNotNull(TEXT("finds the ELK audit-test graph"), SourceGraph)) { return false; }
	const FString SourceStateBefore = CaptureSourceState(*SourceGraph);
	const K2::FGraphGeometrySnapshot Geometry = BuildDeterministicTestGeometry(*SourceGraph);

	FString Error;
	TSharedPtr<FGraphFormatterBenchmarkRun> Run =
		FGraphFormatterBenchmark::CreateHeadlessRun(*SourceBlueprint, *SourceGraph, Geometry, Error);
	if (!TestTrue(*FString::Printf(TEXT("creates an ELK audit run: %s"), *Error), Run.IsValid())) { return false; }
	const FBenchmarkCandidate* ElkCandidate =
		Run->Candidates.FindByPredicate([](const FBenchmarkCandidate& Candidate)
										{ return Candidate.Backend == EFormatterBackend::ElkLayered; });
	if (!TestNotNull(TEXT("finds the ELK candidate"), ElkCandidate)) { return false; }
	if (IsElkRuntimeUnavailable(ElkCandidate->Diagnostics))
	{
		AddWarning(TEXT("ELK audit-artifact assertions were skipped because its optional Node.js runtime is unavailable."));
		return true;
	}

	const FString ArtifactDirectory = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("GraphFormatterTests"),
		TEXT("ElkAudit-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
	);
	const FScopedTestDirectory Cleanup(ArtifactDirectory);
	const FElkLayoutResult Result = RunElkLayeredLayout(
		*Run->OriginalGraph, Run->OriginalGeometry, *GetDefault<UFormatterSettings>(), ArtifactDirectory
	);
	if (!TestTrue(
			*FString::Printf(TEXT("runs the persistent ELK adapter: %s"), *FString::Join(Result.Diagnostics, TEXT(" | "))),
			Result.bSucceeded
		))
	{
		return false;
	}

	const FString InputFilename = FPaths::Combine(ArtifactDirectory, TEXT("elk_input.json"));
	const FString OutputFilename = FPaths::Combine(ArtifactDirectory, TEXT("elk_output.json"));
	const FString ProcessFilename = FPaths::Combine(ArtifactDirectory, TEXT("elk_process.txt"));
	TestTrue(TEXT("preserves the exact ELK input JSON"), FPaths::FileExists(InputFilename));
	TestTrue(TEXT("preserves the raw ELK output JSON"), FPaths::FileExists(OutputFilename));
	TestTrue(TEXT("preserves the ELK process log"), FPaths::FileExists(ProcessFilename));

	FString InputPayload;
	TSharedPtr<FJsonObject> InputJson;
	bool bInputParsed = false;
	if (FFileHelper::LoadFileToString(InputPayload, *InputFilename))
	{
		const TSharedRef<TJsonReader<>> InputReader = TJsonReaderFactory<>::Create(InputPayload);
		bInputParsed = FJsonSerializer::Deserialize(InputReader, InputJson) && InputJson.IsValid();
	}
	if (!TestTrue(TEXT("parses the preserved ELK input"), bInputParsed)) { return false; }
	const TSharedPtr<FJsonObject> Options = InputJson->GetObjectField(TEXT("layoutOptions"));
	TestEqual(TEXT("requests ELK Layered"), Options->GetStringField(TEXT("org.eclipse.elk.algorithm")), TEXT("layered"));
	TestEqual(
		TEXT("requests left-to-right placement"), Options->GetStringField(TEXT("org.eclipse.elk.direction")), TEXT("RIGHT")
	);
	TestEqual(
		TEXT("requests orthogonal routing"), Options->GetStringField(TEXT("org.eclipse.elk.edgeRouting")), TEXT("ORTHOGONAL")
	);
	TestEqual(
		TEXT("keeps disconnected roots in one aligned layout"),
		Options->GetStringField(TEXT("org.eclipse.elk.separateConnectedComponents")),
		TEXT("false")
	);

	const TArray<TSharedPtr<FJsonValue>>& Children = InputJson->GetArrayField(TEXT("children"));
	int32 JsonPortCount = 0;
	int32 FirstLayerCount = 0;
	int32 LastLayerCount = 0;
	for (const TSharedPtr<FJsonValue>& ChildValue : Children)
	{
		const TSharedPtr<FJsonObject> Child = ChildValue.IsValid() ? ChildValue->AsObject() : nullptr;
		if (!Child.IsValid()) { continue; }
		JsonPortCount += Child->GetArrayField(TEXT("ports")).Num();
		const TSharedPtr<FJsonObject>* NodeOptions = nullptr;
		if (!Child->TryGetObjectField(TEXT("layoutOptions"), NodeOptions) || NodeOptions == nullptr) { continue; }
		FString LayerConstraint;
		if (!(*NodeOptions)->TryGetStringField(TEXT("org.eclipse.elk.layered.layering.layerConstraint"), LayerConstraint))
		{
			continue;
		}
		FirstLayerCount += LayerConstraint == TEXT("FIRST") ? 1 : 0;
		LastLayerCount += LayerConstraint == TEXT("LAST") ? 1 : 0;
	}
	TestEqual(TEXT("serializes every submitted linked-pin port"), JsonPortCount, Result.SubmittedPorts);
	TestEqual(TEXT("uses captured offsets for every submitted port"), Result.MeasuredPorts, Result.SubmittedPorts);
	TestTrue(TEXT("marks at least one execution root as a first-layer node"), FirstLayerCount > 0);
	TestTrue(TEXT("marks at least one execution sink as a last-layer node"), LastLayerCount > 0);

	const TArray<TSharedPtr<FJsonValue>>& Edges = InputJson->GetArrayField(TEXT("edges"));
	TestEqual(TEXT("serializes every submitted logical edge"), Edges.Num(), Result.SubmittedEdges);
	bool bSawExecutionPriority = false;
	bool bSawDataPriority = false;
	for (const TSharedPtr<FJsonValue>& EdgeValue : Edges)
	{
		const TSharedPtr<FJsonObject> Edge = EdgeValue.IsValid() ? EdgeValue->AsObject() : nullptr;
		if (!Edge.IsValid()) { continue; }
		const TSharedPtr<FJsonObject>* EdgeOptions = nullptr;
		if (!Edge->TryGetObjectField(TEXT("layoutOptions"), EdgeOptions) || EdgeOptions == nullptr) { continue; }
		FString DirectionPriority;
		if (!(*EdgeOptions)->TryGetStringField(TEXT("org.eclipse.elk.layered.priority.direction"), DirectionPriority))
		{
			continue;
		}
		const double ParsedPriority = FCString::Atod(*DirectionPriority);
		bSawExecutionPriority |= FMath::IsNearlyEqual(ParsedPriority, 1000.0);
		bSawDataPriority |= FMath::IsNearlyEqual(ParsedPriority, 1.0);
	}
	TestTrue(TEXT("gives execution edges the high direction priority"), bSawExecutionPriority);
	TestTrue(TEXT("retains lower-priority data edges"), bSawDataPriority);

	FString OutputPayload;
	TSharedPtr<FJsonObject> OutputJson;
	bool bOutputParsed = false;
	if (FFileHelper::LoadFileToString(OutputPayload, *OutputFilename))
	{
		const TSharedRef<TJsonReader<>> OutputReader = TJsonReaderFactory<>::Create(OutputPayload);
		bOutputParsed = FJsonSerializer::Deserialize(OutputReader, OutputJson) && OutputJson.IsValid();
	}
	TestTrue(TEXT("parses the preserved raw ELK output"), bOutputParsed);
	FString ProcessPayload;
	TestTrue(TEXT("reads the ELK process log"), FFileHelper::LoadFileToString(ProcessPayload, *ProcessFilename));
	TestTrue(TEXT("records a successful ELK process exit"), ProcessPayload.Contains(TEXT("return_code=0")));
	TestEqual(TEXT("the audit run does not mutate the source graph"), CaptureSourceState(*SourceGraph), SourceStateBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphFormatterLibavoidDeterminismTest,
	"Project.Unit Tests.GraphFormatter.Benchmark.LibavoidDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FGraphFormatterLibavoidDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace BenchmarkTestsPrivate;
	FGraphFormatterAdaptagramsObstacle Obstacle;
	Obstacle.Minimum = FVector2D(150.0, -80.0);
	Obstacle.Maximum = FVector2D(250.0, 80.0);
	FGraphFormatterAdaptagramsConnection Connection;
	Connection.StableId = 17;
	Connection.Source = FVector2D(0.0, 0.0);
	Connection.Target = FVector2D(400.0, 0.0);
	Connection.SourceDirection = EGraphFormatterRouteDirection::Right;
	Connection.TargetDirection = EGraphFormatterRouteDirection::Left;
	FGraphFormatterAdaptagramsSettings Settings;
	Settings.ShapeBufferDistance = 16.0;

	const TArray<FGraphFormatterAdaptagramsObstacle> Obstacles{ Obstacle };
	const TArray<FGraphFormatterAdaptagramsConnection> Connections{ Connection };
	const FGraphFormatterAdaptagramsResult First =
		IGraphFormatterAdaptagramsModule::Get().RouteOrthogonal(Obstacles, Connections, Settings);
	const FGraphFormatterAdaptagramsResult Second =
		IGraphFormatterAdaptagramsModule::Get().RouteOrthogonal(Obstacles, Connections, Settings);
	if (!TestTrue(*FString::Printf(TEXT("first route succeeds: %s"), *First.Diagnostic), First.bSucceeded)
		|| !TestTrue(*FString::Printf(TEXT("second route succeeds: %s"), *Second.Diagnostic), Second.bSucceeded))
	{
		return false;
	}
	TestEqual(TEXT("returns one route"), First.Routes.Num(), 1);
	TestEqual(TEXT("route count is deterministic"), Second.Routes.Num(), First.Routes.Num());
	if (First.Routes.IsEmpty() || Second.Routes.IsEmpty()) { return false; }
	TestEqual(TEXT("preserves the stable connection id"), First.Routes[0].StableId, Connection.StableId);
	TestTrue(TEXT("adds at least one obstacle-avoiding bend"), First.Routes[0].Points.Num() >= 4);
	TestEqual(TEXT("point count is deterministic"), Second.Routes[0].Points.Num(), First.Routes[0].Points.Num());
	for (int32 Index = 0; Index < First.Routes[0].Points.Num(); ++Index)
	{
		TestTrue(
			*FString::Printf(TEXT("route point %d is deterministic"), Index),
			First.Routes[0].Points[Index].Equals(Second.Routes[0].Points[Index], 0.01)
		);
		if (Index > 0)
		{
			const FVector2D Start = First.Routes[0].Points[Index - 1];
			const FVector2D End = First.Routes[0].Points[Index];
			TestFalse(
				*FString::Printf(TEXT("segment %d avoids the obstacle"), Index - 1),
				SegmentEntersObstacle(Start, End, Obstacle)
			);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphFormatterBenchmarkRejectsFallbackOnlyGeometryTest,
	"Project.Unit Tests.GraphFormatter.Benchmark.RejectsFallbackOnlyGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FGraphFormatterBenchmarkRejectsFallbackOnlyGeometryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace BenchmarkTestsPrivate;
	UBlueprint* SourceBlueprint =
		LoadObject<UBlueprint>(nullptr, TEXT("/Game/Player/HeldObject/BPC_ResourceCarrier.BPC_ResourceCarrier"));
	if (!TestNotNull(TEXT("loads the representative ResourceCarrier Blueprint"), SourceBlueprint)) { return false; }
	UEdGraph* SourceGraph = FindGraph(*SourceBlueprint, TEXT("DropHeldActor"));
	if (!TestNotNull(TEXT("finds the representative DropHeldActor graph"), SourceGraph)) { return false; }

	K2::FGraphGeometrySnapshot Geometry;
	Geometry.Status = K2::EGraphGeometrySnapshotStatus::Ready;
	FString Error;
	TestFalse(
		TEXT("a nominally ready snapshot with no real Slate bounds or pin anchors is rejected"),
		FGraphFormatterBenchmark::ValidateGeometryForComparison(*SourceGraph, Geometry, Error)
	);
	TestTrue(
		TEXT("the rejection explains that benchmark geometry is not ready"),
		Error.StartsWith(TEXT("Benchmark geometry is not ready"))
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGraphFormatterBenchmarkPartialTieBallotTest,
	"Project.Unit Tests.GraphFormatter.Benchmark.PartialTieBallot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FGraphFormatterBenchmarkPartialTieBallotTest::RunTest(const FString& Parameters)
{
	using namespace BenchmarkTestsPrivate;
	FGraphFormatterBenchmarkRun Run;
	Run.RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Run.SourceAssetPath = TEXT("/Game/Test/BP_Ballot");
	Run.SourceGraphName = TEXT("EventGraph");
	Run.ReportDirectory = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("GraphFormatterTests"), Run.RunId);
	const FScopedTestDirectory Cleanup(Run.ReportDirectory);

	FBenchmarkCandidate CandidateA;
	CandidateA.BlindLabel = TEXT("A");
	CandidateA.Backend = EFormatterBackend::GraphFormatter;
	Run.Candidates.Add(MoveTemp(CandidateA));
	FBenchmarkCandidate CandidateB;
	CandidateB.BlindLabel = TEXT("B");
	CandidateB.Backend = EFormatterBackend::BlueprintAutoLayout;
	Run.Candidates.Add(MoveTemp(CandidateB));
	FBenchmarkCandidate CandidateC;
	CandidateC.BlindLabel = TEXT("C");
	CandidateC.Backend = EFormatterBackend::GraphFormatterLibavoid;
	Run.Candidates.Add(MoveTemp(CandidateC));
	FBenchmarkCandidate CandidateD;
	CandidateD.BlindLabel = TEXT("D");
	CandidateD.Backend = EFormatterBackend::ElkLayered;
	Run.Candidates.Add(MoveTemp(CandidateD));

	TMap<FString, TSet<FString>> Choices;
	TSet<FString> TwoWayTie;
	TwoWayTie.Add(TEXT("C"));
	TwoWayTie.Add(TEXT("B"));
	Choices.Add(TEXT("two_way"), MoveTemp(TwoWayTie));
	TSet<FString> ThreeWayTie;
	ThreeWayTie.Add(TEXT("C"));
	ThreeWayTie.Add(TEXT("A"));
	ThreeWayTie.Add(TEXT("B"));
	Choices.Add(TEXT("three_way"), MoveTemp(ThreeWayTie));
	TSet<FString> FourWayTie;
	FourWayTie.Add(TEXT("D"));
	FourWayTie.Add(TEXT("B"));
	FourWayTie.Add(TEXT("A"));
	FourWayTie.Add(TEXT("C"));
	Choices.Add(TEXT("four_way"), MoveTemp(FourWayTie));

	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("saves a partial-tie ballot: %s"), *Error), Run.SaveBallot(Choices, false, Error)))
	{
		return false;
	}
	FString Payload;
	if (!TestTrue(
			TEXT("reads the saved ballot"),
			FFileHelper::LoadFileToString(Payload, *FPaths::Combine(Run.ReportDirectory, TEXT("ballot.json")))
		))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!TestTrue(TEXT("parses the saved ballot"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("uses the subset-aware ballot schema"), Json->GetStringField(TEXT("schema")), TEXT("graph-formatter-ballot-v2")
	);
	const TSharedPtr<FJsonObject> VisibleChoices = Json->GetObjectField(TEXT("choices"));
	const TSharedPtr<FJsonObject> ResolvedChoices = Json->GetObjectField(TEXT("resolved_choices"));
	const TArray<TSharedPtr<FJsonValue>>& TwoWayValues = VisibleChoices->GetArrayField(TEXT("two_way"));
	TestEqual(TEXT("records two candidates for a partial tie"), TwoWayValues.Num(), 2);
	if (TwoWayValues.Num() == 2)
	{
		TestEqual(TEXT("sorts the first partial-tie label"), TwoWayValues[0]->AsString(), TEXT("B"));
		TestEqual(TEXT("sorts the second partial-tie label"), TwoWayValues[1]->AsString(), TEXT("C"));
	}
	const TArray<TSharedPtr<FJsonValue>>& ResolvedTwoWayValues = ResolvedChoices->GetArrayField(TEXT("two_way"));
	TestEqual(TEXT("records two resolved backends for a partial tie"), ResolvedTwoWayValues.Num(), 2);
	if (ResolvedTwoWayValues.Num() == 2)
	{
		TestEqual(
			TEXT("keeps resolved backend order aligned with B"),
			ResolvedTwoWayValues[0]->AsString(),
			TEXT("blueprint_auto_layout_0_6_9")
		);
		TestEqual(
			TEXT("keeps resolved backend order aligned with C"),
			ResolvedTwoWayValues[1]->AsString(),
			TEXT("graph_formatter_libavoid")
		);
	}
	const TArray<TSharedPtr<FJsonValue>>& ThreeWayValues = VisibleChoices->GetArrayField(TEXT("three_way"));
	TestEqual(TEXT("records all candidates for a three-way tie"), ThreeWayValues.Num(), 3);
	if (ThreeWayValues.Num() == 3)
	{
		TestEqual(TEXT("sorts three-way label A"), ThreeWayValues[0]->AsString(), TEXT("A"));
		TestEqual(TEXT("sorts three-way label B"), ThreeWayValues[1]->AsString(), TEXT("B"));
		TestEqual(TEXT("sorts three-way label C"), ThreeWayValues[2]->AsString(), TEXT("C"));
	}
	const TArray<TSharedPtr<FJsonValue>>& FourWayValues = VisibleChoices->GetArrayField(TEXT("four_way"));
	TestEqual(TEXT("records every candidate for a four-way tie"), FourWayValues.Num(), 4);
	if (FourWayValues.Num() == 4)
	{
		TestEqual(TEXT("sorts four-way label A"), FourWayValues[0]->AsString(), TEXT("A"));
		TestEqual(TEXT("sorts four-way label B"), FourWayValues[1]->AsString(), TEXT("B"));
		TestEqual(TEXT("sorts four-way label C"), FourWayValues[2]->AsString(), TEXT("C"));
		TestEqual(TEXT("sorts four-way label D"), FourWayValues[3]->AsString(), TEXT("D"));
	}
	return true;
}
} // namespace GraphFormatter::Benchmark::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
