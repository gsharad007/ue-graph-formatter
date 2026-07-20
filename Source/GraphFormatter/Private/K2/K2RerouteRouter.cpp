/*---------------------------------------------------------------------------------------------
 * Copyright (c) Howaajin. All rights reserved.
 * Licensed under the MIT License. See License in the project root for license information.
 *--------------------------------------------------------------------------------------------*/

#include "K2/K2RerouteRouter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "GraphEditorSettings.h"
#include "K2Node_Knot.h"
#include "Misc/Crc.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace GraphFormatter::K2
{
namespace
{
constexpr TCHAR GeneratedRerouteMetadataKey[] = TEXT("GraphFormatter.GeneratedReroute");
constexpr double KnotWidth = 42.0;
constexpr double KnotHeight = 24.0;
constexpr int32 MaxChannelCandidateCount = 24;
constexpr double PreferredRouteBonus = 1.0e7;
constexpr double ParallelCrowdingPenalty = 1.0e8;
constexpr double CrossingPenalty = 1.0e10;
constexpr double SegmentOverlapPenalty = 1.0e12;
constexpr double KnotSegmentPenalty = 1.0e13;
constexpr double KnotCollisionPenalty = 1.0e14;
constexpr double SplineFlatteningTolerance = 0.5;
constexpr int32 MaximumSplineSubdivisionDepth = 12;
constexpr double MinimumParallelCosine = 0.9396926207859084; // cos(20 degrees)

struct FRoutePlanningBudget
{
	int32 RemainingComparisons = 0;
	bool bExhausted = false;

	explicit FRoutePlanningBudget(const int32 InComparisonBudget)
		: RemainingComparisons(FMath::Max(0, InComparisonBudget))
	{
	}

	bool TryConsume()
	{
		if (RemainingComparisons <= 0)
		{
			bExhausted = true;
			return false;
		}
		--RemainingComparisons;
		return true;
	}
};

struct FRouteGeometry
{
	TArray<FVector2D> RenderedPoints;
	TArray<FBox2D> KnotBounds;
	double RenderedLength = 0.0;
};

struct FReservedRoute
{
	FString StableKey;
	const UEdGraphPin* OutputPin = nullptr;
	const UEdGraphPin* InputPin = nullptr;
	TArray<FVector2D> Points;
	TArray<FBox2D> KnotBounds;
	bool bExecution = false;
	bool bExistingGeneratedRoute = false;
};

struct FSegmentInteraction
{
	bool bIntersects = false;
	bool bCollinear = false;
	double CollinearOverlap = 0.0;
	FVector2D Intersection = FVector2D::ZeroVector;
};

double Snap(const double Value, const double GridSize)
{ return GridSize > UE_DOUBLE_SMALL_NUMBER ? FMath::GridSnap(Value, GridSize) : Value; }

double PointLineDistanceSquared(const FVector2D& Point, const FVector2D& Start, const FVector2D& End)
{
	const FVector2D Segment = End - Start;
	const double SegmentLengthSquared = Segment.SquaredLength();
	if (SegmentLengthSquared <= UE_DOUBLE_SMALL_NUMBER) { return FVector2D::DistSquared(Point, Start); }
	const double Alpha = FMath::Clamp(FVector2D::DotProduct(Point - Start, Segment) / SegmentLengthSquared, 0.0, 1.0);
	return FVector2D::DistSquared(Point, Start + Segment * Alpha);
}

void AppendFlattenedBezier(
	const FVector2D& Start,
	const FVector2D& FirstControl,
	const FVector2D& SecondControl,
	const FVector2D& End,
	const int32 Depth,
	TArray<FVector2D>& OutPoints
)
{
	const double ToleranceSquared = FMath::Square(SplineFlatteningTolerance);
	const double FlatnessSquared =
		FMath::Max(PointLineDistanceSquared(FirstControl, Start, End), PointLineDistanceSquared(SecondControl, Start, End));
	if (FlatnessSquared <= ToleranceSquared || Depth >= MaximumSplineSubdivisionDepth)
	{
		OutPoints.Add(End);
		return;
	}

	const FVector2D StartFirst = (Start + FirstControl) * 0.5;
	const FVector2D FirstSecond = (FirstControl + SecondControl) * 0.5;
	const FVector2D SecondEnd = (SecondControl + End) * 0.5;
	const FVector2D LeftControl = (StartFirst + FirstSecond) * 0.5;
	const FVector2D RightControl = (FirstSecond + SecondEnd) * 0.5;
	const FVector2D Middle = (LeftControl + RightControl) * 0.5;
	AppendFlattenedBezier(Start, StartFirst, LeftControl, Middle, Depth + 1, OutPoints);
	AppendFlattenedBezier(Middle, RightControl, SecondEnd, End, Depth + 1, OutPoints);
}

TArray<FVector2D> FlattenRenderedPolyline(TConstArrayView<FVector2D> LogicalPoints)
{
	TArray<FVector2D> Result;
	if (LogicalPoints.IsEmpty()) { return Result; }
	TArray<bool> bReversedKnots;
	bReversedKnots.Init(false, LogicalPoints.Num());
	for (int32 Index = 1; Index + 1 < LogicalPoints.Num(); ++Index)
	{
		// FKismetConnectionDrawingPolicy reverses a knot when the average node to its right is
		// left of the average node to its left. Generated chains have exactly one peer per side.
		bReversedKnots[Index] = LogicalPoints[Index + 1].X < LogicalPoints[Index - 1].X;
	}
	Result.Add(LogicalPoints[0]);
	const UGraphEditorSettings* GraphSettings = GetDefault<UGraphEditorSettings>();
	for (int32 Index = 1; Index < LogicalPoints.Num(); ++Index)
	{
		const FVector2D Start = LogicalPoints[Index - 1];
		const FVector2D End = LogicalPoints[Index];
		const FVector2f StartFloat(static_cast<float>(Start.X), static_cast<float>(Start.Y));
		const FVector2f EndFloat(static_cast<float>(End.X), static_cast<float>(End.Y));
		const FVector2f TangentFloat = GraphSettings->ComputeSplineTangent(StartFloat, EndFloat);
		const FVector2D Tangent(static_cast<double>(TangentFloat.X), static_cast<double>(TangentFloat.Y));
		const double StartTangentSign = bReversedKnots[Index - 1] ? -1.0 : 1.0;
		const double EndTangentSign = bReversedKnots[Index] ? -1.0 : 1.0;
		// The Kismet and base connection policies apply the signs above to the Hermite tangent at
		// output starts and input ends. These Bezier controls reproduce that rendered curve.
		AppendFlattenedBezier(
			Start, Start + Tangent * (StartTangentSign / 3.0), End - Tangent * (EndTangentSign / 3.0), End, 0, Result
		);
	}
	return Result;
}

FBox2D Inflate(const FBox2D& Bounds, const double Amount)
{ return FBox2D(Bounds.Min - FVector2D(Amount), Bounds.Max + FVector2D(Amount)); }

bool SegmentIntersectsBox(const FVector2D& Start, const FVector2D& End, const FBox2D& Box)
{
	if (Box.IsInsideOrOn(Start) || Box.IsInsideOrOn(End)) { return true; }

	const FVector2D Delta = End - Start;
	double MinimumT = 0.0;
	double MaximumT = 1.0;

	const auto Clip =
		[&MinimumT, &MaximumT](const double Origin, const double Direction, const double Minimum, const double Maximum)
	{
		if (FMath::IsNearlyZero(Direction)) { return Origin >= Minimum && Origin <= Maximum; }

		double FirstT = (Minimum - Origin) / Direction;
		double SecondT = (Maximum - Origin) / Direction;
		if (FirstT > SecondT) { Swap(FirstT, SecondT); }
		MinimumT = FMath::Max(MinimumT, FirstT);
		MaximumT = FMath::Min(MaximumT, SecondT);
		return MinimumT <= MaximumT;
	};

	return Clip(Start.X, Delta.X, Box.Min.X, Box.Max.X) && Clip(Start.Y, Delta.Y, Box.Min.Y, Box.Max.Y);
}

FBox2D MakeKnotBounds(const FVector2D& Center)
{
	const FVector2D Extent(KnotWidth * 0.5, KnotHeight * 0.5);
	return FBox2D(Center - Extent, Center + Extent);
}

double Cross2D(const FVector2D& Left, const FVector2D& Right) { return Left.X * Right.Y - Left.Y * Right.X; }

FSegmentInteraction GetSegmentInteraction(
	const FVector2D& FirstStart, const FVector2D& FirstEnd, const FVector2D& SecondStart, const FVector2D& SecondEnd
)
{
	FSegmentInteraction Result;
	const FVector2D FirstDelta = FirstEnd - FirstStart;
	const FVector2D SecondDelta = SecondEnd - SecondStart;
	const FVector2D BetweenStarts = SecondStart - FirstStart;
	const double Denominator = Cross2D(FirstDelta, SecondDelta);
	const double FirstLengthSquared = FirstDelta.SquaredLength();
	if (FirstLengthSquared <= UE_DOUBLE_SMALL_NUMBER) { return Result; }

	if (FMath::IsNearlyZero(Denominator))
	{
		if (!FMath::IsNearlyZero(Cross2D(BetweenStarts, FirstDelta))) { return Result; }
		const double FirstT = FVector2D::DotProduct(BetweenStarts, FirstDelta) / FirstLengthSquared;
		const double SecondT = FirstT + FVector2D::DotProduct(SecondDelta, FirstDelta) / FirstLengthSquared;
		const double MinimumT = FMath::Max(0.0, FMath::Min(FirstT, SecondT));
		const double MaximumT = FMath::Min(1.0, FMath::Max(FirstT, SecondT));
		if (MaximumT < MinimumT - UE_DOUBLE_SMALL_NUMBER) { return Result; }
		Result.bIntersects = true;
		Result.Intersection = FirstStart + FirstDelta * MinimumT;
		Result.CollinearOverlap = FMath::Max(0.0, MaximumT - MinimumT) * FMath::Sqrt(FirstLengthSquared);
		Result.bCollinear = Result.CollinearOverlap > UE_DOUBLE_SMALL_NUMBER;
		return Result;
	}

	const double FirstT = Cross2D(BetweenStarts, SecondDelta) / Denominator;
	const double SecondT = Cross2D(BetweenStarts, FirstDelta) / Denominator;
	if (FirstT >= -UE_DOUBLE_SMALL_NUMBER && FirstT <= 1.0 + UE_DOUBLE_SMALL_NUMBER
		&& SecondT >= -UE_DOUBLE_SMALL_NUMBER && SecondT <= 1.0 + UE_DOUBLE_SMALL_NUMBER)
	{
		Result.bIntersects = true;
		Result.Intersection = FirstStart + FirstDelta * FirstT;
	}
	return Result;
}

bool IsPolylineTerminal(const FVector2D& Point, TConstArrayView<FVector2D> Polyline)
{ return Polyline.Num() > 0 && (Point.Equals(Polyline[0]) || Point.Equals(Polyline.Last())); }

double ProjectedOverlap(const double FirstStart, const double FirstEnd, const double SecondStart, const double SecondEnd)
{
	return FMath::Max(
		0.0,
		FMath::Min(FMath::Max(FirstStart, FirstEnd), FMath::Max(SecondStart, SecondEnd))
			- FMath::Max(FMath::Min(FirstStart, FirstEnd), FMath::Min(SecondStart, SecondEnd))
	);
}

double ParallelCrowdingCost(
	const FVector2D& FirstStart,
	const FVector2D& FirstEnd,
	const FVector2D& SecondStart,
	const FVector2D& SecondEnd,
	const double ChannelSpacing
)
{
	if (ChannelSpacing <= UE_DOUBLE_SMALL_NUMBER) { return 0.0; }
	const FVector2D FirstDelta = FirstEnd - FirstStart;
	const FVector2D SecondDelta = SecondEnd - SecondStart;
	const double FirstLength = FirstDelta.Length();
	const double SecondLength = SecondDelta.Length();
	if (FirstLength <= UE_DOUBLE_SMALL_NUMBER || SecondLength <= UE_DOUBLE_SMALL_NUMBER) { return 0.0; }

	const FVector2D Axis = FirstDelta / FirstLength;
	const double ParallelCosine = FMath::Abs(FVector2D::DotProduct(FirstDelta, SecondDelta) / (FirstLength * SecondLength));
	if (ParallelCosine < MinimumParallelCosine) { return 0.0; }

	const double FirstProjectionStart = FVector2D::DotProduct(FirstStart, Axis);
	const double FirstProjectionEnd = FVector2D::DotProduct(FirstEnd, Axis);
	const double SecondProjectionStart = FVector2D::DotProduct(SecondStart, Axis);
	const double SecondProjectionEnd = FVector2D::DotProduct(SecondEnd, Axis);
	const double Overlap =
		ProjectedOverlap(FirstProjectionStart, FirstProjectionEnd, SecondProjectionStart, SecondProjectionEnd);
	if (Overlap <= UE_DOUBLE_SMALL_NUMBER) { return 0.0; }

	const double SeparationSquared = FMath::Min(
		FMath::Min(
			PointLineDistanceSquared(FirstStart, SecondStart, SecondEnd),
			PointLineDistanceSquared(FirstEnd, SecondStart, SecondEnd)
		),
		FMath::Min(
			PointLineDistanceSquared(SecondStart, FirstStart, FirstEnd),
			PointLineDistanceSquared(SecondEnd, FirstStart, FirstEnd)
		)
	);
	const double Separation = FMath::Sqrt(FMath::Max(0.0, SeparationSquared));
	if (Separation >= ChannelSpacing) { return 0.0; }

	const double AlignmentWeight = (ParallelCosine - MinimumParallelCosine) / (1.0 - MinimumParallelCosine);
	return ParallelCrowdingPenalty * AlignmentWeight * (1.0 - Separation / ChannelSpacing) + Overlap;
}

double PolylineLength(TConstArrayView<FVector2D> Points)
{
	double Result = 0.0;
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		Result += FVector2D::Distance(Points[Index - 1], Points[Index]);
	}
	return Result;
}

FRouteGeometry MakeRouteGeometry(TConstArrayView<FVector2D> LogicalPoints)
{
	FRouteGeometry Geometry;
	Geometry.RenderedPoints = FlattenRenderedPolyline(LogicalPoints);
	Geometry.RenderedLength = PolylineLength(Geometry.RenderedPoints);
	for (int32 Index = 1; Index + 1 < LogicalPoints.Num(); ++Index)
	{
		Geometry.KnotBounds.Add(MakeKnotBounds(LogicalPoints[Index]));
	}
	return Geometry;
}

double RouteInteractionCost(
	const FRouteGeometry& Candidate,
	TConstArrayView<FReservedRoute> ReservedRoutes,
	const double ChannelSpacing,
	FRoutePlanningBudget& Budget
)
{
	double Cost = 0.0;

	for (const FReservedRoute& Reserved : ReservedRoutes)
	{
		for (const FBox2D& CandidateKnot : Candidate.KnotBounds)
		{
			const FBox2D SpacedCandidate = Inflate(CandidateKnot, ChannelSpacing * 0.5);
			for (const FBox2D& ReservedKnot : Reserved.KnotBounds)
			{
				if (!Budget.TryConsume()) { return Cost; }
				if (SpacedCandidate.Intersect(Inflate(ReservedKnot, ChannelSpacing * 0.5)))
				{
					Cost += KnotCollisionPenalty;
				}
			}
			for (int32 SegmentIndex = 1; SegmentIndex < Reserved.Points.Num(); ++SegmentIndex)
			{
				if (!Budget.TryConsume()) { return Cost; }
				if (SegmentIntersectsBox(Reserved.Points[SegmentIndex - 1], Reserved.Points[SegmentIndex], SpacedCandidate))
				{
					Cost += KnotSegmentPenalty;
				}
			}
		}

		for (const FBox2D& ReservedKnot : Reserved.KnotBounds)
		{
			const FBox2D SpacedReserved = Inflate(ReservedKnot, ChannelSpacing * 0.5);
			for (int32 SegmentIndex = 1; SegmentIndex < Candidate.RenderedPoints.Num(); ++SegmentIndex)
			{
				if (!Budget.TryConsume()) { return Cost; }
				if (SegmentIntersectsBox(
						Candidate.RenderedPoints[SegmentIndex - 1], Candidate.RenderedPoints[SegmentIndex], SpacedReserved
					))
				{
					Cost += KnotSegmentPenalty;
				}
			}
		}

		for (int32 CandidateSegment = 1; CandidateSegment < Candidate.RenderedPoints.Num(); ++CandidateSegment)
		{
			for (int32 ReservedSegment = 1; ReservedSegment < Reserved.Points.Num(); ++ReservedSegment)
			{
				if (!Budget.TryConsume()) { return Cost; }
				const FVector2D CandidateStart = Candidate.RenderedPoints[CandidateSegment - 1];
				const FVector2D CandidateEnd = Candidate.RenderedPoints[CandidateSegment];
				const FVector2D ReservedStart = Reserved.Points[ReservedSegment - 1];
				const FVector2D ReservedEnd = Reserved.Points[ReservedSegment];
				const FSegmentInteraction Interaction =
					GetSegmentInteraction(CandidateStart, CandidateEnd, ReservedStart, ReservedEnd);
				if (Interaction.bCollinear) { Cost += SegmentOverlapPenalty + Interaction.CollinearOverlap; }
				else if (
					Interaction.bIntersects
					&& !(
						IsPolylineTerminal(Interaction.Intersection, Candidate.RenderedPoints)
						&& IsPolylineTerminal(Interaction.Intersection, Reserved.Points)
					)
				)
				{
					Cost += CrossingPenalty;
				}
				else
				{
					Cost += ParallelCrowdingCost(CandidateStart, CandidateEnd, ReservedStart, ReservedEnd, ChannelSpacing);
				}
			}
		}
	}
	return Cost;
}

FReservedRoute MakeReservation(const FRerouteEdge& Edge, TConstArrayView<FVector2D> Waypoints)
{
	FReservedRoute Reservation;
	Reservation.StableKey = Edge.StableKey;
	Reservation.OutputPin = Edge.OutputPin;
	Reservation.InputPin = Edge.InputPin;
	Reservation.bExecution = Edge.bExecution;
	Reservation.bExistingGeneratedRoute = Edge.bExistingGeneratedRoute;
	TArray<FVector2D> LogicalPoints;
	LogicalPoints.Reserve(Waypoints.Num() + 2);
	LogicalPoints.Add(Edge.OutputAnchor);
	for (const FVector2D& Waypoint : Waypoints)
	{
		LogicalPoints.Add(Waypoint);
	}
	LogicalPoints.Add(Edge.InputAnchor);
	FRouteGeometry Geometry = MakeRouteGeometry(LogicalPoints);
	Reservation.Points = MoveTemp(Geometry.RenderedPoints);
	Reservation.KnotBounds = MoveTemp(Geometry.KnotBounds);
	return Reservation;
}

bool RenderedPolylineIntersectsAnyObstacle(
	const FRouteGeometry& Geometry,
	TConstArrayView<FVector2D> LogicalPoints,
	TConstArrayView<FRerouteObstacle> Obstacles,
	const UEdGraphNode* OutputNode,
	const UEdGraphNode* InputNode,
	const double Clearance,
	FRoutePlanningBudget& Budget
)
{
	for (int32 PointIndex = 1; PointIndex < Geometry.RenderedPoints.Num(); ++PointIndex)
	{
		for (const FRerouteObstacle& Obstacle : Obstacles)
		{
			if (!Obstacle.Bounds.bIsValid) { continue; }
			if (!Budget.TryConsume()) { return false; }
			FBox2D EffectiveBounds = Inflate(Obstacle.Bounds, Clearance + SplineFlatteningTolerance);
			if (Obstacle.Node == OutputNode)
			{
				// Permit only the short terminal corridor leaving the output-facing edge; every
				// later portion of the rendered spline must stay out of the source node body.
				EffectiveBounds.Max.X = FMath::Min(EffectiveBounds.Max.X, LogicalPoints[0].X - SplineFlatteningTolerance);
			}
			else if (Obstacle.Node == InputNode)
			{
				// Input pins are approached from the left. Keep the node body to the right as an
				// obstacle instead of exempting the entire destination node.
				EffectiveBounds.Min.X =
					FMath::Max(EffectiveBounds.Min.X, LogicalPoints.Last().X + SplineFlatteningTolerance);
			}
			if (EffectiveBounds.Min.X > EffectiveBounds.Max.X) { continue; }
			if (SegmentIntersectsBox(
					Geometry.RenderedPoints[PointIndex - 1], Geometry.RenderedPoints[PointIndex], EffectiveBounds
				))
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FVector2D> SimplifyPolyline(TArray<FVector2D> Points)
{
	for (int32 Index = Points.Num() - 2; Index > 0; --Index)
	{
		const FVector2D Previous = Points[Index - 1];
		const FVector2D Current = Points[Index];
		const FVector2D Next = Points[Index + 1];
		const bool bSameX = FMath::IsNearlyEqual(Previous.X, Current.X) && FMath::IsNearlyEqual(Current.X, Next.X);
		const bool bSameY = FMath::IsNearlyEqual(Previous.Y, Current.Y) && FMath::IsNearlyEqual(Current.Y, Next.Y);
		if (bSameX || bSameY || Current.Equals(Previous) || Current.Equals(Next)) { Points.RemoveAt(Index); }
	}
	return Points;
}

bool IsOrthogonalPolyline(TConstArrayView<FVector2D> Points)
{
	for (int32 Index = 1; Index < Points.Num(); ++Index)
	{
		if (!FMath::IsNearlyEqual(Points[Index - 1].X, Points[Index].X)
			&& !FMath::IsNearlyEqual(Points[Index - 1].Y, Points[Index].Y))
		{
			return false;
		}
	}
	return true;
}

bool RenderedPolylineSelfIntersects(const FRouteGeometry& Geometry, FRoutePlanningBudget& Budget)
{
	for (int32 FirstSegment = 1; FirstSegment < Geometry.RenderedPoints.Num(); ++FirstSegment)
	{
		for (int32 SecondSegment = FirstSegment + 2; SecondSegment < Geometry.RenderedPoints.Num(); ++SecondSegment)
		{
			if (!Budget.TryConsume()) { return false; }
			const FSegmentInteraction Interaction = GetSegmentInteraction(
				Geometry.RenderedPoints[FirstSegment - 1],
				Geometry.RenderedPoints[FirstSegment],
				Geometry.RenderedPoints[SecondSegment - 1],
				Geometry.RenderedPoints[SecondSegment]
			);
			if (Interaction.bIntersects) { return true; }
		}
	}
	return false;
}

bool IsExactSharedTerminal(
	const FVector2D& Intersection, const FRouteGeometry& Baseline, const FRerouteEdge& Edge, const FReservedRoute& Reserved
)
{
	if (Baseline.RenderedPoints.IsEmpty() || Reserved.Points.IsEmpty()) { return false; }
	const bool bAtCurrentOutput = Intersection.Equals(Baseline.RenderedPoints[0]);
	const bool bAtCurrentInput = Intersection.Equals(Baseline.RenderedPoints.Last());
	const bool bAtReservedOutput = Intersection.Equals(Reserved.Points[0]);
	const bool bAtReservedInput = Intersection.Equals(Reserved.Points.Last());
	return (bAtCurrentOutput && bAtReservedOutput && Edge.OutputPin == Reserved.OutputPin)
		|| (bAtCurrentOutput && bAtReservedInput && Edge.OutputPin == Reserved.InputPin)
		|| (bAtCurrentInput && bAtReservedOutput && Edge.InputPin == Reserved.OutputPin)
		|| (bAtCurrentInput && bAtReservedInput && Edge.InputPin == Reserved.InputPin);
}

bool BaselineHasBlockingInteraction(
	const FRouteGeometry& Baseline,
	const FRerouteEdge& Edge,
	TConstArrayView<FReservedRoute> ReservedRoutes,
	FRoutePlanningBudget& Budget
)
{
	for (const FReservedRoute& Reserved : ReservedRoutes)
	{
		if (!Reserved.bExistingGeneratedRoute)
		{
			// Execution flow is the primary visual spine. Data wires yield to it on their later pass,
			// rather than bending a clear execution wire merely because a data baseline crosses it.
			if (Edge.bExecution && !Reserved.bExecution) { continue; }
			// Equal-priority wires yield in stable-key order: the earlier key keeps its baseline and
			// the later key routes around it. Later reservations still participate in candidate scoring.
			if (Edge.bExecution == Reserved.bExecution && Edge.StableKey < Reserved.StableKey) { continue; }
		}

		for (const FBox2D& ReservedKnot : Reserved.KnotBounds)
		{
			for (int32 SegmentIndex = 1; SegmentIndex < Baseline.RenderedPoints.Num(); ++SegmentIndex)
			{
				if (!Budget.TryConsume()) { return false; }
				if (SegmentIntersectsBox(
						Baseline.RenderedPoints[SegmentIndex - 1], Baseline.RenderedPoints[SegmentIndex], ReservedKnot
					))
				{
					return true;
				}
			}
		}

		for (int32 BaselineSegment = 1; BaselineSegment < Baseline.RenderedPoints.Num(); ++BaselineSegment)
		{
			for (int32 ReservedSegment = 1; ReservedSegment < Reserved.Points.Num(); ++ReservedSegment)
			{
				if (!Budget.TryConsume()) { return false; }
				const FSegmentInteraction Interaction = GetSegmentInteraction(
					Baseline.RenderedPoints[BaselineSegment - 1],
					Baseline.RenderedPoints[BaselineSegment],
					Reserved.Points[ReservedSegment - 1],
					Reserved.Points[ReservedSegment]
				);
				if (!Interaction.bIntersects) { continue; }
				if (Interaction.bCollinear) { return true; }
				const bool bSharedTerminal = IsExactSharedTerminal(Interaction.Intersection, Baseline, Edge, Reserved);
				if (!bSharedTerminal) { return true; }
			}
		}
	}
	return false;
}

TArray<FVector2D> BuildRoute(
	const FRerouteEdge& Edge,
	TConstArrayView<FRerouteObstacle> Obstacles,
	TConstArrayView<FReservedRoute> ReservedRoutes,
	const FRerouteSettings& Settings,
	const double GridSize,
	FRoutePlanningBudget& Budget,
	bool& bOutRouteRequired,
	FString& OutFailure
)
{
	bOutRouteRequired = false;
	OutFailure.Reset();
	UEdGraphNode* OutputNode = Edge.OutputPin ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
	UEdGraphNode* InputNode = Edge.InputPin ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
	const double Clearance = Settings.ObstacleClearance;
	const double Channel = FMath::Max(Settings.ChannelSpacing, GridSize);
	if (!Edge.bExecution && !Settings.bRouteDataWires) { return {}; }
	const bool bBackward = Edge.InputAnchor.X <= Edge.OutputAnchor.X + Channel;

	TArray<FVector2D> Baseline = { Edge.OutputAnchor, Edge.InputAnchor };
	const FRouteGeometry BaselineGeometry = MakeRouteGeometry(Baseline);
	const bool bObstructed = RenderedPolylineIntersectsAnyObstacle(
		BaselineGeometry, Baseline, Obstacles, OutputNode, InputNode, Clearance, Budget
	);
	if (Budget.bExhausted) { return {}; }
	const bool bWireConflict = BaselineHasBlockingInteraction(BaselineGeometry, Edge, ReservedRoutes, Budget);
	if (Budget.bExhausted) { return {}; }
	const bool bLongData = !Edge.bExecution && Settings.bRouteDataWires
						&& Edge.RankSpan >= Settings.LongDataWireRankThreshold;
	TArray<FVector2D> PreferredRoute;
	if (!Edge.PreferredWaypoints.IsEmpty())
	{
		PreferredRoute.Reserve(Edge.PreferredWaypoints.Num() + 2);
		PreferredRoute.Add(Edge.OutputAnchor);
		PreferredRoute.Append(Edge.PreferredWaypoints);
		PreferredRoute.Add(Edge.InputAnchor);
		PreferredRoute = SimplifyPolyline(MoveTemp(PreferredRoute));
	}
	// A layout-core hint that reduces to the clear direct connection must never bend a straight
	// execution wire. Diagonal hints are ignored because generated knot plans are orthogonal.
	const bool bHasPreferredRoute = PreferredRoute.Num() >= 3 && IsOrthogonalPolyline(PreferredRoute);
	if (!bBackward && !bObstructed && !bWireConflict && !bLongData && !bHasPreferredRoute) { return {}; }
	bOutRouteRequired = true;

	double TopCandidate = FMath::Min(Edge.OutputAnchor.Y, Edge.InputAnchor.Y) - Channel;
	double BottomCandidate = FMath::Max(Edge.OutputAnchor.Y, Edge.InputAnchor.Y) + Channel;
	for (const FRerouteObstacle& Obstacle : Obstacles)
	{
		if (!Obstacle.Bounds.bIsValid) { continue; }
		const FBox2D Inflated = Inflate(Obstacle.Bounds, Clearance);
		const double CorridorMinX = FMath::Min(Edge.OutputAnchor.X, Edge.InputAnchor.X) - Channel;
		const double CorridorMaxX = FMath::Max(Edge.OutputAnchor.X, Edge.InputAnchor.X) + Channel;
		if (Inflated.Max.X >= CorridorMinX && Inflated.Min.X <= CorridorMaxX)
		{
			TopCandidate = FMath::Min(TopCandidate, Inflated.Min.Y - Channel);
			BottomCandidate = FMath::Max(BottomCandidate, Inflated.Max.Y + Channel);
		}
	}
	TopCandidate = Snap(TopCandidate, GridSize);
	BottomCandidate = Snap(BottomCandidate, GridSize);

	TArray<FVector2D> BestRoute;
	double BestScore = TNumericLimits<double>::Max();
	int32 BestOrder = MAX_int32;
	int32 MinimumRequiredKnots = MAX_int32;
	bool bFoundObstacleClearRoute = false;
	const auto ConsiderCandidate = [&](TArray<FVector2D> Candidate, const int32 Order, const bool bPreferred)
	{
		if (Budget.bExhausted) { return; }
		Candidate = SimplifyPolyline(MoveTemp(Candidate));
		if (Candidate.Num() < 3 || !IsOrthogonalPolyline(Candidate)) { return; }
		const FRouteGeometry CandidateGeometry = MakeRouteGeometry(Candidate);
		if (RenderedPolylineSelfIntersects(CandidateGeometry, Budget) || Budget.bExhausted
			|| RenderedPolylineIntersectsAnyObstacle(
				CandidateGeometry, Candidate, Obstacles, OutputNode, InputNode, Clearance, Budget
			)
			|| Budget.bExhausted)
		{
			return;
		}

		bFoundObstacleClearRoute = true;
		const int32 RequiredKnots = Candidate.Num() - 2;
		MinimumRequiredKnots = FMath::Min(MinimumRequiredKnots, RequiredKnots);
		if (RequiredKnots > Settings.MaxKnotsPerWire) { return; }

		double Score = CandidateGeometry.RenderedLength
					 + RouteInteractionCost(CandidateGeometry, ReservedRoutes, Channel, Budget);
		if (Budget.bExhausted) { return; }
		if (bPreferred) { Score -= PreferredRouteBonus; }
		if (Score < BestScore - UE_DOUBLE_SMALL_NUMBER || (FMath::IsNearlyEqual(Score, BestScore) && Order < BestOrder))
		{
			BestScore = Score;
			BestOrder = Order;
			BestRoute = MoveTemp(Candidate);
		}
	};

	if (bHasPreferredRoute) { ConsiderCandidate(MoveTemp(PreferredRoute), -1, true); }

	double TerminalStub = Channel;
	const bool bGeometricallyBackward = Edge.InputAnchor.X <= Edge.OutputAnchor.X;
	if (!bGeometricallyBackward)
	{
		const double HalfAvailableGap = (Edge.InputAnchor.X - Edge.OutputAnchor.X - GridSize) * 0.5;
		TerminalStub = FMath::Min(Channel, FMath::Max(0.0, FMath::FloorToDouble(HalfAvailableGap / GridSize) * GridSize));
	}
	const double BaseOutputChannelX = Snap(Edge.OutputAnchor.X + TerminalStub, GridSize);
	const double BaseInputChannelX = Snap(Edge.InputAnchor.X - TerminalStub, GridSize);
	const double PortLaneStep = FMath::Max(Channel, Snap(KnotWidth + Channel, GridSize));
	const int32 CandidateCount = FMath::Clamp(2 + ReservedRoutes.Num() * 2, 2, MaxChannelCandidateCount);
	for (int32 CandidateIndex = 0; CandidateIndex < CandidateCount && !Budget.bExhausted; ++CandidateIndex)
	{
		const bool bTop = CandidateIndex % 2 == 0;
		const int32 ChannelRing = CandidateIndex / 2;
		const double RouteY = bTop ? TopCandidate - ChannelRing * Channel : BottomCandidate + ChannelRing * Channel;
		const double PortOffset = bGeometricallyBackward ? CandidateIndex * PortLaneStep : 0.0;
		const double OutputChannelX = BaseOutputChannelX + PortOffset;
		const double InputChannelX = BaseInputChannelX - PortOffset;
		if (!bGeometricallyBackward && OutputChannelX >= InputChannelX) { continue; }
		TArray<FVector2D> Candidate;
		Candidate.Add(Edge.OutputAnchor);
		Candidate.Add(FVector2D(OutputChannelX, Edge.OutputAnchor.Y));
		Candidate.Add(FVector2D(OutputChannelX, RouteY));
		Candidate.Add(FVector2D(InputChannelX, RouteY));
		Candidate.Add(FVector2D(InputChannelX, Edge.InputAnchor.Y));
		Candidate.Add(Edge.InputAnchor);
		ConsiderCandidate(MoveTemp(Candidate), CandidateIndex, false);
	}
	if (Budget.bExhausted) { return {}; }

	// The real endpoints are pins, not knot waypoints.
	if (BestRoute.Num() >= 2)
	{
		BestRoute.RemoveAt(BestRoute.Num() - 1);
		BestRoute.RemoveAt(0);
		return BestRoute;
	}

	if (bFoundObstacleClearRoute && MinimumRequiredKnots != MAX_int32)
	{
		OutFailure = FString::Printf(
			TEXT("The safe route needs %d knots, exceeding the configured limit of %d."), MinimumRequiredKnots, Settings.MaxKnotsPerWire
		);
	}
	else
	{
		OutFailure = TEXT("No deterministic routing channel clears the measured node and reserved wire obstacles.");
	}
	return {};
}

FGuid MakeDeterministicGuid(const FString& StableKey, const int32 WaypointIndex, const UEdGraph& Graph)
{
	int32 Salt = 0;
	while (true)
	{
		const FString Seed = FString::Printf(TEXT("%s|%d|%d"), *StableKey, WaypointIndex, Salt);
		const FGuid Candidate(
			FCrc::StrCrc32(*(Seed + TEXT("|A"))),
			FCrc::StrCrc32(*(Seed + TEXT("|B"))),
			FCrc::StrCrc32(*(Seed + TEXT("|C"))),
			FCrc::StrCrc32(*(Seed + TEXT("|D")))
		);
		const bool bCollision = Graph.Nodes.ContainsByPredicate([&Candidate](const TObjectPtr<UEdGraphNode>& Node)
																{ return Node && Node->NodeGuid == Candidate; });
		if (!bCollision) { return Candidate; }
		++Salt;
	}
}

bool HasExactMutualLink(const UEdGraphPin& First, const UEdGraphPin& Second)
{
	int32 ForwardReferences = 0;
	for (const UEdGraphPin* LinkedPin : First.LinkedTo)
	{
		if (LinkedPin == &Second) { ++ForwardReferences; }
	}
	int32 ReverseReferences = 0;
	for (const UEdGraphPin* LinkedPin : Second.LinkedTo)
	{
		if (LinkedPin == &First) { ++ReverseReferences; }
	}
	return ForwardReferences == 1 && ReverseReferences == 1;
}

void RemoveFailedKnots(UEdGraph& Graph, TArray<UK2Node_Knot*>& Knots)
{
	for (UK2Node_Knot* Knot : Knots)
	{
		if (!Knot) { continue; }
		if (UEdGraphPin* InputPin = Knot->GetInputPin()) { InputPin->BreakAllPinLinks(false, false); }
		if (UEdGraphPin* OutputPin = Knot->GetOutputPin()) { OutputPin->BreakAllPinLinks(false, false); }
		Graph.Nodes.RemoveSingle(Knot);
	}
	Knots.Reset();
}

bool RestoreOriginalDirectLink(
	UEdGraph& Graph, const UEdGraphSchema_K2& Schema, UEdGraphPin& OutputPin, UEdGraphPin& InputPin, TArray<UK2Node_Knot*>& FailedKnots
)
{
	RemoveFailedKnots(Graph, FailedKnots);
	if (HasExactMutualLink(OutputPin, InputPin)) { return true; }

	Schema.UEdGraphSchema::TryCreateConnection(&OutputPin, &InputPin);
	if (HasExactMutualLink(OutputPin, InputPin)) { return true; }

	// This exact relationship was valid immediately before replacement. Normalize any asymmetric
	// residue as a last resort rather than leaving a Blueprint with silently corrupted topology.
	OutputPin.Modify(false);
	InputPin.Modify(false);
	OutputPin.LinkedTo.Remove(&InputPin);
	InputPin.LinkedTo.Remove(&OutputPin);
	OutputPin.MakeLinkTo(&InputPin, false);
	return HasExactMutualLink(OutputPin, InputPin);
}

UK2Node_Knot* CreateTransactionNeutralKnot(UEdGraph& Graph, const FVector2D& TopLeft)
{
	EObjectFlags ObjectFlags = RF_Transactional;
	if (Graph.HasAnyFlags(RF_Transient)) { ObjectFlags |= RF_Transient; }
	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(&Graph, NAME_None, ObjectFlags);
	if (!Knot) { return nullptr; }
	Graph.Nodes.Add(Knot);
	Knot->CreateNewGuid();
	Knot->PostPlacedNewNode();
	Knot->AllocateDefaultPins();
	Knot->NodePosX = FMath::RoundToInt(TopLeft.X);
	Knot->NodePosY = FMath::RoundToInt(TopLeft.Y);
	return Knot;
}

bool InstallRoute(
	UEdGraph& Graph,
	const UEdGraphSchema_K2& Schema,
	const FRerouteEdge& Edge,
	TConstArrayView<FVector2D> Waypoints,
	TArray<UK2Node_Knot*>& OutKnots,
	FString& OutFailure,
	bool& bOutTopologyRestorationFailed
)
{
	bOutTopologyRestorationFailed = false;
	const bool bPackageWasDirty = Graph.GetOutermost()->IsDirty();
	const auto RestoreCleanPackageAfterFailure = [&Graph, bPackageWasDirty]()
	{
		if (!bPackageWasDirty) { Graph.GetOutermost()->SetDirtyFlag(false); }
	};
	if (!Edge.OutputPin || !Edge.InputPin || Edge.InputPin->LinkedTo.Num() != 1
		|| !HasExactMutualLink(*Edge.OutputPin, *Edge.InputPin))
	{
		OutFailure = TEXT("The original exclusive-input, exact reciprocal direct link no longer exists.");
		return false;
	}

	OutKnots.Reserve(Waypoints.Num());
	for (int32 Index = 0; Index < Waypoints.Num(); ++Index)
	{
		const FVector2D TopLeft = Waypoints[Index] - FVector2D(KnotWidth * 0.5, KnotHeight * 0.5);
		UK2Node_Knot* Knot = CreateTransactionNeutralKnot(Graph, TopLeft);
		if (!Knot)
		{
			OutFailure = TEXT("Unreal failed to spawn a knot node.");
			RemoveFailedKnots(Graph, OutKnots);
			RestoreCleanPackageAfterFailure();
			return false;
		}
		Knot->NodeGuid = MakeDeterministicGuid(Edge.StableKey, Index, Graph);
		OutKnots.Add(Knot);
	}

	for (int32 Index = 1; Index < OutKnots.Num(); ++Index)
	{
		if (!Schema.UEdGraphSchema::TryCreateConnection(OutKnots[Index - 1]->GetOutputPin(), OutKnots[Index]->GetInputPin()))
		{
			OutFailure = TEXT("The K2 schema rejected an internal knot connection.");
			RemoveFailedKnots(Graph, OutKnots);
			RestoreCleanPackageAfterFailure();
			return false;
		}
	}

	Schema.UEdGraphSchema::BreakSinglePinLink(Edge.OutputPin, Edge.InputPin);
	const bool bSourceConnected = Schema.UEdGraphSchema::TryCreateConnection(Edge.OutputPin, OutKnots[0]->GetInputPin());
	const bool bDestinationConnected =
		bSourceConnected && Schema.UEdGraphSchema::TryCreateConnection(OutKnots.Last()->GetOutputPin(), Edge.InputPin);
	if (!bDestinationConnected)
	{
		const bool bRestored = RestoreOriginalDirectLink(Graph, Schema, *Edge.OutputPin, *Edge.InputPin, OutKnots);
		bOutTopologyRestorationFailed = !bRestored;
		OutFailure =
			bRestored
				? TEXT("The K2 schema rejected a boundary knot connection; the exact original direct link was restored and verified.")
				: TEXT("FATAL topology-safety failure: the K2 schema rejected a boundary knot connection and the exact original direct link could not be restored.");
		if (bRestored) { RestoreCleanPackageAfterFailure(); }
		return false;
	}

	for (UK2Node_Knot* Knot : OutKnots)
	{
		Knot->PostReconstructNode();
	}
	for (int32 Index = 0; Index < OutKnots.Num(); ++Index)
	{
		FMetaData& MetaData = OutKnots[Index]->GetOutermost()->GetMetaData();
		const FString CanonicalNodeGuid = OutKnots[Index]->NodeGuid.ToString(EGuidFormats::Digits);
		MetaData.SetValue(OutKnots[Index], GeneratedRerouteMetadataKey, *CanonicalNodeGuid);
		MetaData.SetValue(OutKnots[Index], TEXT("GraphFormatter.LogicalEdge"), *Edge.StableKey);
		MetaData.SetValue(OutKnots[Index], TEXT("GraphFormatter.RouteOrdinal"), *LexToString(Index));
	}
	return true;
}
} // namespace

bool FK2RerouteRouter::IsGeneratedRerouteNode(const UEdGraphNode* Node)
{
	if (!Node || !Node->IsA<UK2Node_Knot>() || !Node->NodeGuid.IsValid()) { return false; }
	FMetaData& MetaData = Node->GetOutermost()->GetMetaData();
	return MetaData.HasValue(Node, GeneratedRerouteMetadataKey)
		&& MetaData.GetValue(Node, GeneratedRerouteMetadataKey) == Node->NodeGuid.ToString(EGuidFormats::Digits);
}

bool FK2RerouteRouter::TryGetGeneratedRouteKey(const UEdGraphNode* Node, FString& OutLogicalEdgeKey)
{
	OutLogicalEdgeKey.Reset();
	if (!IsGeneratedRerouteNode(Node)) { return false; }
	FMetaData& MetaData = Node->GetOutermost()->GetMetaData();
	if (!MetaData.HasValue(Node, TEXT("GraphFormatter.LogicalEdge"))) { return false; }
	OutLogicalEdgeKey = MetaData.GetValue(Node, TEXT("GraphFormatter.LogicalEdge"));
	return !OutLogicalEdgeKey.IsEmpty();
}

bool FK2RerouteRouter::FindGeneratedRoute(
	UEdGraph& Graph, const FStringView LogicalEdgeKey, TArray<UEdGraphNode*>& OutKnots, TArray<FVector2D>& OutWaypoints
)
{
	struct FOrdinalKnot
	{
		UK2Node_Knot* Knot = nullptr;
		int32 Ordinal = INDEX_NONE;
	};

	OutKnots.Reset();
	OutWaypoints.Reset();
	TArray<FOrdinalKnot> OrderedKnots;
	const FString ExpectedKey(LogicalEdgeKey);
	for (const TObjectPtr<UEdGraphNode>& NodePointer : Graph.Nodes)
	{
		UK2Node_Knot* Knot = Cast<UK2Node_Knot>(NodePointer.Get());
		if (!IsGeneratedRerouteNode(Knot)) { continue; }
		FMetaData& MetaData = Knot->GetOutermost()->GetMetaData();
		if (MetaData.GetValue(Knot, TEXT("GraphFormatter.LogicalEdge")) != ExpectedKey) { continue; }
		if (!MetaData.HasValue(Knot, TEXT("GraphFormatter.RouteOrdinal"))) { return false; }
		const FString OrdinalText = MetaData.GetValue(Knot, TEXT("GraphFormatter.RouteOrdinal"));
		int32 Ordinal = INDEX_NONE;
		if (!LexTryParseString(Ordinal, *OrdinalText) || Ordinal < 0 || LexToString(Ordinal) != OrdinalText)
		{
			return false;
		}
		OrderedKnots.Add({ Knot, Ordinal });
	}
	OrderedKnots.Sort(
		[](const FOrdinalKnot& Left, const FOrdinalKnot& Right)
		{
			if (Left.Ordinal != Right.Ordinal) { return Left.Ordinal < Right.Ordinal; }
			return Left.Knot->NodeGuid.ToString(EGuidFormats::Digits) < Right.Knot->NodeGuid.ToString(EGuidFormats::Digits);
		}
	);
	for (int32 Index = 0; Index < OrderedKnots.Num(); ++Index)
	{
		if (OrderedKnots[Index].Ordinal != Index) { return false; }
		OutKnots.Add(OrderedKnots[Index].Knot);
		OutWaypoints.Add(FVector2D(
			OrderedKnots[Index].Knot->NodePosX + KnotWidth * 0.5, OrderedKnots[Index].Knot->NodePosY + KnotHeight * 0.5
		));
	}
	return !OutWaypoints.IsEmpty();
}

FRerouteResult FK2RerouteRouter::Route(
	UEdGraph& Graph,
	TConstArrayView<FRerouteEdge> Edges,
	TConstArrayView<FRerouteObstacle> Obstacles,
	const TSet<UEdGraphNode*>& Scope,
	const FRerouteSettings& Settings,
	const double GridSize
)
{
	FRerouteResult Result;
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
	if (!Schema)
	{
		Result.Diagnostics.Add(TEXT("Reroute generation is available only for K2 graphs."));
		return Result;
	}
	FRoutePlanningBudget PlanningBudget(Settings.PlanningWorkBudget);

	TArray<FRerouteEdge> SortedEdges;
	SortedEdges.Reserve(Edges.Num());
	for (const FRerouteEdge& Edge : Edges)
	{
		SortedEdges.Add(Edge);
	}
	SortedEdges.Sort(
		[](const FRerouteEdge& Left, const FRerouteEdge& Right)
		{
			if (Left.bExistingGeneratedRoute != Right.bExistingGeneratedRoute)
			{
				return Left.bExistingGeneratedRoute && !Right.bExistingGeneratedRoute;
			}
			if (Left.bExecution != Right.bExecution) { return Left.bExecution && !Right.bExecution; }
			return Left.StableKey < Right.StableKey;
		}
	);

	TArray<FReservedRoute> ReservedRoutes;
	ReservedRoutes.Reserve(SortedEdges.Num() * 2);
	// Seed the complete current wire field before planning. Each direct edge temporarily removes
	// its own baseline below, so early routes account for later untouched wires without making the
	// result depend on caller insertion order.
	for (const FRerouteEdge& Edge : SortedEdges)
	{
		if (Edge.bExistingGeneratedRoute)
		{
			if (!Edge.PreferredWaypoints.IsEmpty())
			{
				ReservedRoutes.Add(MakeReservation(Edge, Edge.PreferredWaypoints));
			}
		}
		else if (Edge.OutputPin != nullptr && Edge.InputPin != nullptr)
		{
			ReservedRoutes.Add(MakeReservation(Edge, TConstArrayView<FVector2D>()));
		}
	}
	for (const FRerouteEdge& Edge : SortedEdges)
	{
		UEdGraphNode* OutputNode = Edge.OutputPin ? Edge.OutputPin->GetOwningNodeUnchecked() : nullptr;
		UEdGraphNode* InputNode = Edge.InputPin ? Edge.InputPin->GetOwningNodeUnchecked() : nullptr;
		if (Edge.bExistingGeneratedRoute)
		{
			if (Edge.PreferredWaypoints.IsEmpty())
			{
				++Result.SkippedWires;
				Result.Diagnostics.Add(
					FString::Printf(TEXT("%s: Existing generated route metadata is incomplete."), *Edge.StableKey)
				);
			}
			continue;
		}
		if (!OutputNode || !InputNode || !Scope.Contains(OutputNode) || !Scope.Contains(InputNode))
		{
			++Result.SkippedWires;
			continue;
		}
		// A previous routing pass already owns this wire segment. Do not grow knot chains on
		// repeated Format + Route invocations; the regular layout may still reposition the knots.
		if (IsGeneratedRerouteNode(OutputNode) || IsGeneratedRerouteNode(InputNode)) { continue; }
		ReservedRoutes.RemoveAll([&Edge](const FReservedRoute& Reserved) { return Reserved.StableKey == Edge.StableKey; });

		bool bRouteRequired = false;
		FString PlanningFailure;
		TArray<FVector2D> Waypoints = BuildRoute(
			Edge, Obstacles, ReservedRoutes, Settings, GridSize, PlanningBudget, bRouteRequired, PlanningFailure
		);
		if (PlanningBudget.bExhausted)
		{
			// The current baseline was removed only for planning. Put it back before aborting so the
			// reservation model remains an exact description of every unchanged wire.
			ReservedRoutes.Add(MakeReservation(Edge, TConstArrayView<FVector2D>()));
			Result.bPlanningBudgetExhausted = true;
			++Result.SkippedWires;
			Result.Diagnostics.Add(
				FString::Printf(
					TEXT("%s: The deterministic routing work budget was exhausted; this wire and all remaining wires were left unchanged."),
					*Edge.StableKey
				)
			);
			break;
		}
		if (Waypoints.IsEmpty())
		{
			if (bRouteRequired)
			{
				++Result.SkippedWires;
				Result.Diagnostics.Add(FString::Printf(TEXT("%s: %s"), *Edge.StableKey, *PlanningFailure));
			}
			// The unchanged real wire still occupies rendered space and must constrain every later
			// route in this deterministic pass, including disabled or unrouteable data wires.
			ReservedRoutes.Add(MakeReservation(Edge, TConstArrayView<FVector2D>()));
			continue;
		}

		TArray<UK2Node_Knot*> Knots;
		FString Failure;
		bool bTopologyRestorationFailed = false;
		if (InstallRoute(Graph, *Schema, Edge, Waypoints, Knots, Failure, bTopologyRestorationFailed))
		{
			++Result.RoutedWires;
			Result.CreatedKnots += Knots.Num();
			ReservedRoutes.Add(MakeReservation(Edge, Waypoints));
		}
		else
		{
			++Result.SkippedWires;
			Result.bTopologyRestorationFailed |= bTopologyRestorationFailed;
			Result.Diagnostics.Add(FString::Printf(TEXT("%s: %s"), *Edge.StableKey, *Failure));
			if (bTopologyRestorationFailed) { break; }
			ReservedRoutes.Add(MakeReservation(Edge, TConstArrayView<FVector2D>()));
		}
	}
	return Result;
}
} // namespace GraphFormatter::K2
