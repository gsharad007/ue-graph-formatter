/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "Benchmark/SGraphFormatterBenchmark.h"

#include "Benchmark/GraphFormatterBenchmark.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SGraphPanel.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace GraphFormatter::Benchmark
{
namespace BenchmarkWidgetPrivate
{
struct FCriterionDefinition
{
	const TCHAR* Key;
	const TCHAR* Label;
	const TCHAR* ToolTip;
};

const FCriterionDefinition Criteria[] = {
	{ TEXT("overall_readability"),
     TEXT("Overall readability"),
     TEXT("Which result looks most like a careful human laid it out?")                                     },
	{ TEXT("execution_flow"),
     TEXT("Execution flow"),
     TEXT("Prefer straight, left-to-right execution spines and clear branch lanes.")                       },
	{ TEXT("data_grouping"),
     TEXT("Input grouping"),
     TEXT("Prefer pure/data providers grouped closely and legibly around their consumers.")                },
	{ TEXT("wire_routing"),
     TEXT("Wire routing"),
     TEXT("Prefer fewer crossings, fewer wires under nodes, and useful reroutes rather than clutter.")     },
	{ TEXT("authored_intent"),
     TEXT("Preserves intent"),
     TEXT("Prefer a result that improves defects without needlessly destroying the authored composition.") },
};

constexpr const TCHAR* AllCandidatesChoice = TEXT("All");

[[nodiscard]]
bool SaveWidgetPng(const TSharedRef<SWidget>& Widget, const FString& Filename, FIntPoint& OutSize, FString& OutError)
{
	TArray<FColor> Pixels;
	FIntVector ImageSize;
	if (!FSlateApplication::Get().TakeScreenshot(Widget, Pixels, ImageSize))
	{
		OutError = FString::Printf(TEXT("Could not capture '%s' from Slate."), *Filename);
		return false;
	}
	TArray64<uint8> Compressed;
	FImageUtils::PNGCompressImageArray(
		ImageSize.X, ImageSize.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Compressed
	);
	if (!FFileHelper::SaveArrayToFile(Compressed, *Filename))
	{
		OutError = FString::Printf(TEXT("Could not write screenshot '%s'."), *Filename);
		return false;
	}
	OutSize = FIntPoint(ImageSize.X, ImageSize.Y);
	return true;
}

[[nodiscard]]
bool FindContentBounds(const UEdGraph& Graph, const K2::FGraphGeometrySnapshot& Geometry, FBox2D& OutBounds)
{
	OutBounds = FBox2D(EForceInit::ForceInit);
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		const UEdGraphNode* Node = NodePointer.Get();
		if (Node == nullptr) { continue; }
		FVector2D Size(
			Node->NodeWidth > 0 ? static_cast<double>(Node->NodeWidth) : 160.0,
			Node->NodeHeight > 0 ? static_cast<double>(Node->NodeHeight) : 80.0
		);
		if (const K2::FGraphNodeGeometrySnapshot* NodeGeometry = Geometry.FindNode(Node))
		{
			if (NodeGeometry->Bounds.IsSet())
			{
				const FSlateRect& Bounds = NodeGeometry->Bounds.GetValue();
				Size = FVector2D(Bounds.Right - Bounds.Left, Bounds.Bottom - Bounds.Top);
			}
		}
		const FVector2D Position(Node->NodePosX, Node->NodePosY);
		OutBounds += Position;
		OutBounds += Position + Size;
	}
	return OutBounds.bIsValid;
}

[[nodiscard]]
FText FormatMetrics(const FGraphQualityMetrics& Metrics)
{
	return FText::FromString(
		FString::Printf(
			TEXT(
				"Penalty %.0f  |  overlaps %d  |  wire/node hits %d  |  crossings E%d/D%d  |  exec bends %d (ΔY %.0f)  "
				"|  grid X%d/Y%d  |  semantic moved %d (Σ%.0f)  |  knots %d (+%d), moved %d (Σ%.0f)"
			),
			Metrics.CompositePenalty,
			Metrics.NodeOverlapCount,
			Metrics.WireThroughNodeCount,
			Metrics.ExecutionCrossingCount,
			Metrics.DataCrossingCount,
			Metrics.NonStraightPrimaryExecutionEdgeCount,
			Metrics.PrimaryExecutionVerticalError,
			Metrics.OffGridXCount,
			Metrics.OffGridYCount,
			Metrics.MovedSemanticNodeCount,
			Metrics.TotalNodeMovement,
			Metrics.RerouteNodeCount,
			Metrics.AddedRerouteNodeCount,
			Metrics.MovedRerouteNodeCount,
			Metrics.TotalRerouteNodeMovement
		)
	);
}
} // namespace BenchmarkWidgetPrivate

using namespace BenchmarkWidgetPrivate;

void SGraphFormatterBenchmark::Construct(const FArguments& Arguments)
{
	Run = Arguments._Run;
	if (!Run.IsValid())
	{
		ChildSlot[SNew(STextBlock).Text(FText::FromString(TEXT("The comparison run is unavailable.")))];
		return;
	}

	ChildSlot[SNew(SBorder)
				  .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
				  .Padding(
					  8.0f
				  )[SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(
						4.0f
					)[SNew(STextBlock)
						  .Text(
							  FText::FromString(
								  FString::Printf(
									  TEXT("Original reference plus %d randomized candidates for %s :: %s. Source asset remains untouched."),
									  Run->Candidates.Num(),
									  *Run->SourceAssetPath,
									  *Run->SourceGraphName
								  )
							  )
						  )
						  .Font(FAppStyle::GetFontStyle(TEXT("HeadingExtraSmall")))]
					+ SVerticalBox::Slot().FillHeight(1.0f).Padding(2.0f)[MakeGraphGrid()]
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)[MakeBallot()]]];
}

TSharedRef<SWidget> SGraphFormatterBenchmark::MakeGraphGrid()
{
	constexpr int32 ColumnCount = 3;
	const int32 PaneCount = Run->Candidates.Num() + 1;
	const int32 RowCount = FMath::DivideAndRoundUp(PaneCount, ColumnCount);
	const TSharedRef<SGridPanel> Grid = SNew(SGridPanel);
	for (int32 Column = 0; Column < ColumnCount; ++Column)
	{
		Grid->SetColumnFill(Column, 1.0f);
	}
	for (int32 Row = 0; Row < RowCount; ++Row)
	{
		Grid->SetRowFill(Row, 1.0f);
	}
	for (int32 Pane = 0; Pane < PaneCount; ++Pane)
	{
		const int32 CandidateIndex = Pane == 0 ? INDEX_NONE : Pane - 1;
		Grid->AddSlot(Pane % ColumnCount, Pane / ColumnCount).Padding(3.0f)[MakeGraphPane(CandidateIndex)];
	}
	return Grid;
}

TSharedRef<SWidget> SGraphFormatterBenchmark::MakeGraphPane(const int32 CandidateIndex)
{
	UEdGraph* Graph = CandidateIndex == INDEX_NONE
						? Run->OriginalGraph
						: (Run->Candidates.IsValidIndex(CandidateIndex) ? Run->Candidates[CandidateIndex].Graph : nullptr);
	SGraphEditor::FGraphEditorEvents Events;
	TSharedPtr<SGraphEditor> GraphEditor;
	if (Graph != nullptr)
	{
		GraphEditor = SNew(SGraphEditor)
						  .IsEditable(false)
						  .DisplayAsReadOnly(false)
						  .ShowGraphStateOverlay(false)
						  .AllowConnectionSlicing(false)
						  .GraphToEdit(Graph)
						  .GraphEvents(Events);
		GraphEditors.Add(GraphEditor);
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.DarkGroupBorder")))
		.Padding(
			3.0f
		)[SNew(SVerticalBox)
		  + SVerticalBox::Slot().AutoHeight().Padding(
			  3.0f
		  )[SNew(STextBlock)
				.Text(this, &SGraphFormatterBenchmark::GetPaneTitle, CandidateIndex)
				.ColorAndOpacity(this, &SGraphFormatterBenchmark::GetPaneStatusColor, CandidateIndex)
				.Font(FAppStyle::GetFontStyle(TEXT("HeadingExtraSmall")))]
		  + SVerticalBox::Slot().FillHeight(
			  1.0f
		  )[GraphEditor.IsValid() ? StaticCastSharedRef<SWidget>(GraphEditor.ToSharedRef())
								  : StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(FText::FromString(TEXT("Graph unavailable"))))]
		  + SVerticalBox::Slot().AutoHeight().Padding(
			  3.0f
		  )[SNew(STextBlock)
				.Text(this, &SGraphFormatterBenchmark::GetPaneMetrics, CandidateIndex)
				.Visibility(CandidateIndex == INDEX_NONE ? EVisibility::Visible : TAttribute<EVisibility>(this, &SGraphFormatterBenchmark::GetRevealedVisibility))
				.AutoWrapText(true)]];
}

TSharedRef<SWidget> SGraphFormatterBenchmark::MakeBallot()
{
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	for (const FCriterionDefinition& Criterion : Criteria)
	{
		Rows->AddSlot().AutoHeight().Padding(
			1.0f
		)[MakeCriterionRow(Criterion.Key, FText::FromString(Criterion.Label), FText::FromString(Criterion.ToolTip))];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
		.Padding(
			6.0f
		)[SNew(SVerticalBox)
		  + SVerticalBox::Slot().AutoHeight().Padding(
			  3.0f
		  )[SNew(STextBlock)
				.Text(FText::FromString(TEXT("Select one candidate, or select any subset of candidates to record a tie.")))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())]
		  + SVerticalBox::Slot().AutoHeight()[Rows]
		  + SVerticalBox::Slot().AutoHeight().Padding(
			  0.0f, 5.0f
		  )[SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(
				2.0f
			)[SNew(SButton)
				  .Text(FText::FromString(TEXT("Save blind ballot")))
				  .ToolTipText(FText::FromString(TEXT("Save your choices before revealing which formatter is A, B, C, or D.")))
				  .IsEnabled(this, &SGraphFormatterBenchmark::CanSaveBlindBallot)
				  .OnClicked(this, &SGraphFormatterBenchmark::SaveBallot)]
			+ SHorizontalBox::Slot().AutoWidth().Padding(
				2.0f
			)[SNew(SButton)
				  .Text(FText::FromString(TEXT("Reveal formatter mapping")))
				  .ToolTipText(FText::FromString(TEXT("Reveal identities and metrics. This permanently ends blind voting for this run.")))
				  .OnClicked(this, &SGraphFormatterBenchmark::RevealMapping)]
			+ SHorizontalBox::Slot().AutoWidth().Padding(
				2.0f
			)[SNew(SButton).Text(FText::FromString(TEXT("Zoom all to fit"))).OnClicked(this, &SGraphFormatterBenchmark::ZoomAllToFit)]
			+ SHorizontalBox::Slot().AutoWidth().Padding(
				2.0f
			)[SNew(SButton)
				  .Text(FText::FromString(TEXT("Capture PNGs")))
				  .ToolTipText(FText::FromString(TEXT("Save the original and blinded A/B/C/D graph panes as PNG artifacts.")))
				  .OnClicked(this, &SGraphFormatterBenchmark::CaptureScreenshots)]
			+ SHorizontalBox::Slot()
				  .FillWidth(1.0f)
				  .VAlign(VAlign_Center)
				  .Padding(8.0f, 0.0f)[SNew(STextBlock).Text_Lambda([this]() { return StatusText; }).AutoWrapText(true)]]
		  + SVerticalBox::Slot().AutoHeight().Padding(
			  2.0f
		  )[SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Artifacts: %s"), *Run->ReportDirectory)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())]];
}

TSharedRef<SWidget> SGraphFormatterBenchmark::MakeCriterionRow(const FString& Key, const FText& Label, const FText& ToolTip)
{
	const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	Row->AddSlot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(3.0f)[SNew(STextBlock).Text(Label).ToolTipText(ToolTip)];
	for (const FBenchmarkCandidate& Candidate : Run->Candidates)
	{
		Row->AddSlot().AutoWidth().Padding(2.0f)[MakeChoiceButton(Key, Candidate.BlindLabel)];
	}
	Row->AddSlot().AutoWidth().Padding(2.0f)[MakeChoiceButton(Key, AllCandidatesChoice)];
	return Row;
}

TSharedRef<SWidget> SGraphFormatterBenchmark::MakeChoiceButton(const FString& Criterion, const FString& Choice)
{
	const bool bAllCandidates = Choice == AllCandidatesChoice;
	return SNew(SButton)
		.Text(FText::FromString(bAllCandidates ? TEXT("All tied") : Choice))
		.ToolTipText(
			FText::FromString(
				bAllCandidates ? TEXT("Select or clear every candidate for this criterion.")
							   : TEXT("Toggle this candidate. Select any additional candidates to record a tie.")
			)
		)
		.ButtonColorAndOpacity(this, &SGraphFormatterBenchmark::GetChoiceColor, Criterion, Choice)
		.IsEnabled(this, &SGraphFormatterBenchmark::IsBlindJudging)
		.OnClicked(this, &SGraphFormatterBenchmark::SelectChoice, Criterion, Choice);
}

FText SGraphFormatterBenchmark::GetPaneTitle(const int32 CandidateIndex) const
{
	if (CandidateIndex == INDEX_NONE) { return FText::FromString(TEXT("Original reference (unchanged)")); }
	if (!Run->Candidates.IsValidIndex(CandidateIndex)) { return FText::FromString(TEXT("Unknown candidate")); }
	const FBenchmarkCandidate& Candidate = Run->Candidates[CandidateIndex];
	FString Title = FString::Printf(TEXT("Candidate %s"), *Candidate.BlindLabel);
	if (bMappingRevealed)
	{
		Title += FString::Printf(
			TEXT(" — %s — %s — %.1f ms"),
			DescribeBackend(Candidate.Backend),
			Candidate.IsValid() ? TEXT("valid") : TEXT("INVALID"),
			Candidate.DurationMilliseconds
		);
	}
	return FText::FromString(Title);
}

FText SGraphFormatterBenchmark::GetPaneMetrics(const int32 CandidateIndex) const
{
	if (CandidateIndex == INDEX_NONE) { return FormatMetrics(Run->OriginalMetrics); }
	if (!Run->Candidates.IsValidIndex(CandidateIndex)) { return FText::GetEmpty(); }
	const FBenchmarkCandidate& Candidate = Run->Candidates[CandidateIndex];
	FString Result = FormatMetrics(Candidate.Metrics).ToString();
	if (!Candidate.Diagnostics.IsEmpty()) { Result += TEXT("\n") + FString::Join(Candidate.Diagnostics, TEXT(" | ")); }
	return FText::FromString(Result);
}

FSlateColor SGraphFormatterBenchmark::GetPaneStatusColor(const int32 CandidateIndex) const
{
	if (CandidateIndex == INDEX_NONE || !Run->Candidates.IsValidIndex(CandidateIndex)
		|| Run->Candidates[CandidateIndex].IsValid())
	{
		return FSlateColor::UseForeground();
	}
	return FSlateColor(FLinearColor(1.0f, 0.2f, 0.15f));
}

FSlateColor SGraphFormatterBenchmark::GetChoiceColor(FString Criterion, FString Choice) const
{
	const TSet<FString>* Selected = Choices.Find(Criterion);
	const bool bSelected =
		Selected != nullptr
		&& (Choice == AllCandidatesChoice ? Selected->Num() == Run->Candidates.Num() : Selected->Contains(Choice));
	return bSelected ? FSlateColor(FLinearColor(0.15f, 0.55f, 0.95f, 1.0f)) : FSlateColor(FLinearColor::White);
}

EVisibility SGraphFormatterBenchmark::GetRevealedVisibility() const
{ return bMappingRevealed ? EVisibility::Visible : EVisibility::Collapsed; }

FReply SGraphFormatterBenchmark::SelectChoice(FString Criterion, FString Choice)
{
	if (Choice != AllCandidatesChoice && !IsCandidateChoice(Choice)) { return FReply::Handled(); }

	TSet<FString>& Selected = Choices.FindOrAdd(Criterion);
	if (Choice == AllCandidatesChoice)
	{
		if (Selected.Num() == Run->Candidates.Num()) { Selected.Reset(); }
		else
		{
			Selected.Reset();
			for (const FBenchmarkCandidate& Candidate : Run->Candidates)
			{
				Selected.Add(Candidate.BlindLabel);
			}
		}
	}
	else if (Selected.Contains(Choice)) { Selected.Remove(Choice); }
	else
	{
		Selected.Add(MoveTemp(Choice));
	}
	StatusText = FText::FromString(
		IsBallotComplete() ? TEXT("Ballot complete; save it before revealing.") : TEXT("Selection updated.")
	);
	return FReply::Handled();
}

FReply SGraphFormatterBenchmark::SaveBallot()
{
	if (!CanSaveBlindBallot())
	{
		StatusText = FText::FromString(
			bMappingRevealed ? TEXT("This run has been revealed; its blind ballot is locked.")
							 : TEXT("Select at least one of A, B, C, or D for every criterion before saving.")
		);
		return FReply::Handled();
	}
	FString ScreenshotError;
	const bool bCaptured = CaptureScreenshotsNow(ScreenshotError);
	FString Error;
	if (Run->SaveBallot(Choices, bMappingRevealed, Error))
	{
		StatusText = FText::FromString(
			bCaptured ? FString::Printf(TEXT("Ballot and %d PNGs saved to %s"), GraphEditors.Num(), *Run->ReportDirectory)
					  : FString::Printf(TEXT("Ballot saved, but PNG capture failed: %s"), *ScreenshotError)
		);
	}
	else
	{
		StatusText = FText::FromString(Error);
	}
	return FReply::Handled();
}

FReply SGraphFormatterBenchmark::RevealMapping()
{
	FString ScreenshotError;
	const bool bCaptured = CaptureScreenshotsNow(ScreenshotError);
	bMappingRevealed = true;
	const FString RevealStatus =
		bCaptured
			? FString::Printf(
				  TEXT("Mapping and objective metrics revealed; %d PNGs were saved. Your judgment remains primary."),
				  GraphEditors.Num()
			  )
			: FString::Printf(TEXT("Mapping revealed, but PNG capture failed: %s"), *ScreenshotError);
	StatusText = FText::FromString(RevealStatus);
	return FReply::Handled();
}

FReply SGraphFormatterBenchmark::ZoomAllToFit()
{
	FString Error;
	if (!ApplyCommonView(Error)) { StatusText = FText::FromString(Error); }
	return FReply::Handled();
}

bool SGraphFormatterBenchmark::ApplyCommonView(FString& OutError)
{
	const int32 ExpectedPaneCount = Run.IsValid() ? Run->Candidates.Num() + 1 : 0;
	if (!Run.IsValid() || GraphEditors.Num() != ExpectedPaneCount)
	{
		OutError = FString::Printf(TEXT("The %d benchmark graph panes are unavailable."), ExpectedPaneCount);
		return false;
	}
	if (!Run->RefreshRenderedResults(GraphEditors, OutError)) { return false; }

	TArray<FBox2D> ContentBounds;
	ContentBounds.Reserve(GraphEditors.Num());
	double MaximumWidth = 1.0;
	double MaximumHeight = 1.0;
	double MinimumViewportWidth = TNumericLimits<double>::Max();
	double MinimumViewportHeight = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < GraphEditors.Num(); ++Index)
	{
		const UEdGraph* Graph = Index == 0 ? Run->OriginalGraph : Run->Candidates[Index - 1].Graph;
		const K2::FGraphGeometrySnapshot& Geometry = Index == 0 ? Run->OriginalGeometry
																: Run->Candidates[Index - 1].Geometry;
		FBox2D Bounds(EForceInit::ForceInit);
		if (Graph == nullptr || !FindContentBounds(*Graph, Geometry, Bounds))
		{
			OutError = FString::Printf(TEXT("Benchmark pane %d has no measurable graph content."), Index + 1);
			return false;
		}
		ContentBounds.Add(Bounds);
		MaximumWidth = FMath::Max(MaximumWidth, Bounds.GetSize().X);
		MaximumHeight = FMath::Max(MaximumHeight, Bounds.GetSize().Y);
		SGraphPanel* Panel = GraphEditors[Index].IsValid() ? GraphEditors[Index]->GetGraphPanel() : nullptr;
		if (Panel == nullptr)
		{
			OutError = FString::Printf(TEXT("Benchmark pane %d has no graph panel."), Index + 1);
			return false;
		}
		const FVector2D ViewportSize(Panel->GetTickSpaceGeometry().GetLocalSize());
		MinimumViewportWidth = FMath::Min(MinimumViewportWidth, ViewportSize.X);
		MinimumViewportHeight = FMath::Min(MinimumViewportHeight, ViewportSize.Y);
	}

	constexpr double GraphPadding = 128.0;
	constexpr double ViewportPadding = 32.0;
	const double AvailableWidth = FMath::Max(1.0, MinimumViewportWidth - ViewportPadding * 2.0);
	const double AvailableHeight = FMath::Max(1.0, MinimumViewportHeight - ViewportPadding * 2.0);
	const float CommonZoom = static_cast<float>(FMath::Clamp(
		FMath::Min(AvailableWidth / (MaximumWidth + GraphPadding * 2.0), AvailableHeight / (MaximumHeight + GraphPadding * 2.0)),
		0.05,
		1.0
	));
	for (int32 Index = 0; Index < GraphEditors.Num(); ++Index)
	{
		SGraphPanel* Panel = GraphEditors[Index]->GetGraphPanel();
		const FVector2D ViewportSize(Panel->GetTickSpaceGeometry().GetLocalSize());
		const FVector2D Center = ContentBounds[Index].GetCenter();
		FVector2D ViewOffset = Center - ViewportSize / (2.0 * CommonZoom);
		GraphEditors[Index]->SetViewLocation(FVector2f(ViewOffset), CommonZoom);
		FVector2f IgnoredLocation;
		float ActualZoom = CommonZoom;
		GraphEditors[Index]->GetViewLocation(IgnoredLocation, ActualZoom);
		ViewOffset = Center - ViewportSize / (2.0 * FMath::Max(ActualZoom, 0.01f));
		GraphEditors[Index]->SetViewLocation(FVector2f(ViewOffset), ActualZoom);
	}
	OutError.Reset();
	return true;
}

FReply SGraphFormatterBenchmark::CaptureScreenshots()
{
	FString Error;
	if (CaptureScreenshotsNow(Error))
	{
		StatusText =
			FText::FromString(FString::Printf(TEXT("%d PNGs saved to %s"), GraphEditors.Num(), *Run->ReportDirectory));
	}
	else
	{
		StatusText = FText::FromString(Error);
	}
	return FReply::Handled();
}

bool SGraphFormatterBenchmark::CaptureScreenshotsNow(FString& OutError)
{
	if (bScreenshotsCaptured) { return true; }
	const int32 ExpectedPaneCount = Run.IsValid() ? Run->Candidates.Num() + 1 : 0;
	if (!Run.IsValid() || GraphEditors.Num() != ExpectedPaneCount)
	{
		OutError = FString::Printf(TEXT("The %d graph panes are not available for screenshot capture."), ExpectedPaneCount);
		return false;
	}
	if (!ApplyCommonView(OutError)) { return false; }
	IFileManager::Get().MakeDirectory(*Run->ReportDirectory, true);
	TArray<FString> Errors;
	Run->OriginalScreenshotFilename = TEXT("original.png");
	const FString OriginalPath = FPaths::Combine(Run->ReportDirectory, Run->OriginalScreenshotFilename);
	FString CaptureError;
	if (!SaveWidgetPng(GraphEditors[0].ToSharedRef(), OriginalPath, Run->OriginalScreenshotSize, CaptureError))
	{
		Errors.Add(MoveTemp(CaptureError));
	}
	for (int32 Index = 0; Index < Run->Candidates.Num() && GraphEditors.IsValidIndex(Index + 1); ++Index)
	{
		FBenchmarkCandidate& Candidate = Run->Candidates[Index];
		Candidate.ScreenshotFilename = FString::Printf(TEXT("candidate_%s.png"), *Candidate.BlindLabel);
		const FString CandidatePath = FPaths::Combine(Run->ReportDirectory, Candidate.ScreenshotFilename);
		CaptureError.Reset();
		if (!SaveWidgetPng(GraphEditors[Index + 1].ToSharedRef(), CandidatePath, Candidate.ScreenshotSize, CaptureError))
		{
			Errors.Add(MoveTemp(CaptureError));
		}
	}
	if (!Errors.IsEmpty()) { Run->Diagnostics.Add(FString::Join(Errors, TEXT(" | "))); }
	FString ManifestError;
	if (!Run->SaveManifest(ManifestError)) { Errors.Add(MoveTemp(ManifestError)); }
	bScreenshotsCaptured = Errors.IsEmpty();
	OutError = FString::Join(Errors, TEXT(" | "));
	return Errors.IsEmpty();
}

bool SGraphFormatterBenchmark::IsBallotComplete() const
{
	for (const FCriterionDefinition& Criterion : Criteria)
	{
		const TSet<FString>* Selected = Choices.Find(Criterion.Key);
		if (Selected == nullptr || Selected->IsEmpty()) { return false; }
	}
	return true;
}

bool SGraphFormatterBenchmark::IsCandidateChoice(const FString& Choice) const
{
	return Run.IsValid()
		&& Run->Candidates.ContainsByPredicate([&Choice](const FBenchmarkCandidate& Candidate)
											   { return Candidate.BlindLabel == Choice; });
}

bool SGraphFormatterBenchmark::CanSaveBlindBallot() const { return !bMappingRevealed && IsBallotComplete(); }

bool SGraphFormatterBenchmark::IsBlindJudging() const { return !bMappingRevealed; }

void SGraphFormatterBenchmark::Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);
	if (InitialViewAttemptsRemaining <= 0) { return; }
	--InitialViewAttemptsRemaining;
	FString Error;
	if (ApplyCommonView(Error))
	{
		++SuccessfulCommonViewPasses;
		if (SuccessfulCommonViewPasses >= 2)
		{
			InitialViewAttemptsRemaining = 0;
			FString CaptureError;
			if (CaptureScreenshotsNow(CaptureError))
			{
				StatusText = FText::FromString(
					FString::Printf(
						TEXT("Objective metrics and %d normalized PNGs saved to %s"), GraphEditors.Num(), *Run->ReportDirectory
					)
				);
			}
			else
			{
				StatusText = FText::FromString(CaptureError);
			}
		}
	}
	else
	{
		SuccessfulCommonViewPasses = 0;
		if (InitialViewAttemptsRemaining == 0) { StatusText = FText::FromString(Error); }
	}
}
} // namespace GraphFormatter::Benchmark
