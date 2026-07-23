/*---------------------------------------------------------------------------------------------
 * Copyright (c) GraphFormatter contributors. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SGraphEditor;
class UEdGraph;

namespace GraphFormatter::Benchmark
{
class FGraphFormatterBenchmarkRun;
struct FGraphQualityMetrics;

class SGraphFormatterBenchmark final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGraphFormatterBenchmark)
		{
		}
		SLATE_ARGUMENT(TSharedPtr<FGraphFormatterBenchmarkRun>, Run)
	SLATE_END_ARGS()

	void Construct(const FArguments& Arguments);
	virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;

private:
	TSharedRef<SWidget> MakeGraphGrid();
	TSharedRef<SWidget> MakeGraphPane(int32 CandidateIndex);
	TSharedRef<SWidget> MakeBallot();
	TSharedRef<SWidget> MakeCriterionRow(const FString& Key, const FText& Label, const FText& ToolTip);
	TSharedRef<SWidget> MakeChoiceButton(const FString& Criterion, const FString& Choice);
	FText GetPaneTitle(int32 CandidateIndex) const;
	FText GetPaneMetrics(int32 CandidateIndex) const;
	FSlateColor GetPaneStatusColor(int32 CandidateIndex) const;
	FSlateColor GetChoiceColor(FString Criterion, FString Choice) const;
	EVisibility GetRevealedVisibility() const;
	FReply SelectChoice(FString Criterion, FString Choice);
	FReply SaveBallot();
	FReply RevealMapping();
	FReply ZoomAllToFit();
	FReply CaptureScreenshots();
	bool CaptureScreenshotsNow(FString& OutError);
	bool ApplyCommonView(FString& OutError);
	bool IsBallotComplete() const;
	bool IsCandidateChoice(const FString& Choice) const;
	bool CanSaveBlindBallot() const;
	bool IsBlindJudging() const;

	TSharedPtr<FGraphFormatterBenchmarkRun> Run;
	TArray<TSharedPtr<SGraphEditor>> GraphEditors;
	TMap<FString, TSet<FString>> Choices;
	FText StatusText;
	bool bMappingRevealed = false;
	bool bScreenshotsCaptured = false;
	int32 InitialViewAttemptsRemaining = 8;
	int32 SuccessfulCommonViewPasses = 0;
};
} // namespace GraphFormatter::Benchmark
