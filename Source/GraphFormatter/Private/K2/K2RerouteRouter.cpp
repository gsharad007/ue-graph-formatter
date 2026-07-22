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
	FBox2D RenderedBounds = FBox2D(EForceInit::ForceInit);
	double RenderedLength = 0.0;
};

struct FReservedRoute
{
	FString StableKey;
	const UEdGraphPin* OutputPin = nullptr;
	const UEdGraphPin* InputPin = nullptr;
	TArray<FVector2D> Points;
	TArray<FBox2D> KnotBounds;
	FBox2D Bounds = FBox2D(EForceInit::ForceInit);
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

TArray<FVector2D> FlattenRenderedPolyline(
	TConstArrayView<FVector2D> LogicalPoints, const bool bReverseFirstTangent, const bool bReverseLastTangent
)
{
	TArray<FVector2D> Result;
	if (LogicalPoints.IsEmpty()) { return Result; }
	TArray<bool> bReversedKnots;
	bReversedKnots.Init(false, LogicalPoints.Num());
	bReversedKnots[0] = bReverseFirstTangent;
	bReversedKnots.Last() = bReverseLastTangent;
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
	const FVector2D Extent(RerouteKnotWidth * 0.5, RerouteKnotHeight * 0.5);
	return FBox2D(Center - Extent, Center + Extent);
}

bool BoxesOverlapWithPositiveArea(const FBox2D& First, const FBox2D& Second)
{
	return First.bIsValid && Second.bIsValid && First.Min.X < Second.Max.X && Second.Min.X < First.Max.X
		&& First.Min.Y < Second.Max.Y && Second.Min.Y < First.Max.Y;
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
	const double SecondLengthSquared = SecondDelta.SquaredLength();
	if (FirstLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
	{
		if (SecondLengthSquared <= UE_DOUBLE_SMALL_NUMBER)
		{
			Result.bIntersects = FirstStart.Equals(SecondStart);
			Result.Intersection = FirstStart;
			return Result;
		}
		// The ordinary collinearity/projection path already handles a degenerate second
		// segment. Swap the arguments so point-versus-segment tests are symmetric.
		return GetSegmentInteraction(SecondStart, SecondEnd, FirstStart, FirstEnd);
	}

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

bool IsTerminalAdjacentSegment(const FVector2D& Point, TConstArrayView<FVector2D> Polyline, const int32 SegmentEndIndex)
{
	return Polyline.Num() > 1
		&& ((SegmentEndIndex == 1 && Point.Equals(Polyline[0]))
			|| (SegmentEndIndex == Polyline.Num() - 1 && Point.Equals(Polyline.Last())));
}

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

FRouteGeometry MakeRouteGeometry(
	TConstArrayView<FVector2D> LogicalPoints, const bool bReverseFirstTangent = false, const bool bReverseLastTangent = false
)
{
	FRouteGeometry Geometry;
	Geometry.RenderedPoints = FlattenRenderedPolyline(LogicalPoints, bReverseFirstTangent, bReverseLastTangent);
	Geometry.RenderedLength = PolylineLength(Geometry.RenderedPoints);
	for (const FVector2D& Point : Geometry.RenderedPoints)
	{
		Geometry.RenderedBounds += Point;
	}
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
						IsTerminalAdjacentSegment(Interaction.Intersection, Candidate.RenderedPoints, CandidateSegment)
						&& IsTerminalAdjacentSegment(Interaction.Intersection, Reserved.Points, ReservedSegment)
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
	FRouteGeometry Geometry = MakeRouteGeometry(LogicalPoints, Edge.bReverseOutputTangent, Edge.bReverseInputTangent);
	Reservation.Points = MoveTemp(Geometry.RenderedPoints);
	Reservation.KnotBounds = MoveTemp(Geometry.KnotBounds);
	Reservation.Bounds = Geometry.RenderedBounds;
	return Reservation;
}

bool CandidateKnotBoundsCollide(
	const FRouteGeometry& Candidate,
	TConstArrayView<FRerouteObstacle> Obstacles,
	TConstArrayView<FReservedRoute> ReservedRoutes,
	FRoutePlanningBudget& Budget
)
{
	for (int32 FirstIndex = 0; FirstIndex < Candidate.KnotBounds.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = FirstIndex + 1; SecondIndex < Candidate.KnotBounds.Num(); ++SecondIndex)
		{
			if (!Budget.TryConsume()) { return false; }
			if (BoxesOverlapWithPositiveArea(Candidate.KnotBounds[FirstIndex], Candidate.KnotBounds[SecondIndex]))
			{
				return true;
			}
		}
	}

	for (const FBox2D& CandidateKnot : Candidate.KnotBounds)
	{
		for (const FRerouteObstacle& Obstacle : Obstacles)
		{
			if (!Budget.TryConsume()) { return false; }
			if (BoxesOverlapWithPositiveArea(CandidateKnot, Obstacle.Bounds)) { return true; }
		}
		for (const FReservedRoute& Reserved : ReservedRoutes)
		{
			for (const FBox2D& ReservedKnot : Reserved.KnotBounds)
			{
				if (!Budget.TryConsume()) { return false; }
				if (BoxesOverlapWithPositiveArea(CandidateKnot, ReservedKnot)) { return true; }
			}
		}
	}
	return false;
}

bool RenderedPolylineIntersectsAnyObstacle(
	const FRouteGeometry& Geometry,
	TConstArrayView<FVector2D> LogicalPoints,
	TConstArrayView<FRerouteObstacle> Obstacles,
	const UEdGraphNode* OutputNode,
	const UEdGraphNode* InputNode,
	const bool bReverseOutputTangent,
	const bool bReverseInputTangent,
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
				// A user knot can reverse its rendered tangent. Retain only the half of the
				// endpoint obstacle opposite the permitted exit corridor.
				if (bReverseOutputTangent)
				{
					EffectiveBounds.Min.X =
						FMath::Max(EffectiveBounds.Min.X, LogicalPoints[0].X + SplineFlatteningTolerance);
				}
				else
				{
					EffectiveBounds.Max.X =
						FMath::Min(EffectiveBounds.Max.X, LogicalPoints[0].X - SplineFlatteningTolerance);
				}
			}
			else if (Obstacle.Node == InputNode)
			{
				// Normal input pins are approached from the left; reversed user knots are
				// approached from the right. Keep the opposite half as a real obstacle.
				if (bReverseInputTangent)
				{
					EffectiveBounds.Max.X =
						FMath::Min(EffectiveBounds.Max.X, LogicalPoints.Last().X - SplineFlatteningTolerance);
				}
				else
				{
					EffectiveBounds.Min.X =
						FMath::Max(EffectiveBounds.Min.X, LogicalPoints.Last().X + SplineFlatteningTolerance);
				}
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

bool PinsSharePhysicalTerminal(const UEdGraphPin* First, const UEdGraphPin* Second)
{
	if (First == nullptr || Second == nullptr) { return false; }
	if (First == Second) { return true; }
	UEdGraphNode* FirstNode = First->GetOwningNodeUnchecked();
	return FirstNode != nullptr && FirstNode == Second->GetOwningNodeUnchecked() && FirstNode->IsA<UK2Node_Knot>();
}

bool IsExactSharedTerminal(
	const FVector2D& Intersection,
	const FRouteGeometry& Baseline,
	const FRerouteEdge& Edge,
	const FReservedRoute& Reserved,
	const int32 BaselineSegment,
	const int32 ReservedSegment
)
{
	if (Baseline.RenderedPoints.IsEmpty() || Reserved.Points.IsEmpty()) { return false; }
	const bool bAtCurrentOutput = BaselineSegment == 1 && Intersection.Equals(Baseline.RenderedPoints[0]);
	const bool bAtCurrentInput = BaselineSegment == Baseline.RenderedPoints.Num() - 1
							  && Intersection.Equals(Baseline.RenderedPoints.Last());
	const bool bAtReservedOutput = ReservedSegment == 1 && Intersection.Equals(Reserved.Points[0]);
	const bool bAtReservedInput = ReservedSegment == Reserved.Points.Num() - 1
							   && Intersection.Equals(Reserved.Points.Last());
	return (bAtCurrentOutput && bAtReservedOutput && PinsSharePhysicalTerminal(Edge.OutputPin, Reserved.OutputPin))
		|| (bAtCurrentOutput && bAtReservedInput && PinsSharePhysicalTerminal(Edge.OutputPin, Reserved.InputPin))
		|| (bAtCurrentInput && bAtReservedOutput && PinsSharePhysicalTerminal(Edge.InputPin, Reserved.OutputPin))
		|| (bAtCurrentInput && bAtReservedInput && PinsSharePhysicalTerminal(Edge.InputPin, Reserved.InputPin));
}

bool RouteIntersectsReservedRoute(
	const FRouteGeometry& Route, const FRerouteEdge& Edge, const FReservedRoute& Reserved, FRoutePlanningBudget& Budget
)
{
	if (!Route.RenderedBounds.bIsValid || !Reserved.Bounds.bIsValid
		|| Route.RenderedBounds.Max.X < Reserved.Bounds.Min.X || Reserved.Bounds.Max.X < Route.RenderedBounds.Min.X
		|| Route.RenderedBounds.Max.Y < Reserved.Bounds.Min.Y || Reserved.Bounds.Max.Y < Route.RenderedBounds.Min.Y)
	{
		return false;
	}

	for (int32 RouteSegment = 1; RouteSegment < Route.RenderedPoints.Num(); ++RouteSegment)
	{
		for (int32 ReservedSegment = 1; ReservedSegment < Reserved.Points.Num(); ++ReservedSegment)
		{
			if (!Budget.TryConsume()) { return false; }
			const FSegmentInteraction Interaction = GetSegmentInteraction(
				Route.RenderedPoints[RouteSegment - 1],
				Route.RenderedPoints[RouteSegment],
				Reserved.Points[ReservedSegment - 1],
				Reserved.Points[ReservedSegment]
			);
			if (!Interaction.bIntersects) { continue; }
			// A shared pin is only a zero-length terminal exemption. Collinear overlap or a
			// later recrossing remains a real wire-pair interaction, matching the formatter gate.
			if (Interaction.bCollinear
				|| !IsExactSharedTerminal(Interaction.Intersection, Route, Edge, Reserved, RouteSegment, ReservedSegment))
			{
				return true;
			}
		}
	}
	return false;
}

int32 CountRouteIntersections(
	const FRouteGeometry& Route, const FRerouteEdge& Edge, const FReservedRoute& Reserved, FRoutePlanningBudget& Budget
)
{
	if (!Route.RenderedBounds.bIsValid || !Reserved.Bounds.bIsValid
		|| Route.RenderedBounds.Max.X < Reserved.Bounds.Min.X || Reserved.Bounds.Max.X < Route.RenderedBounds.Min.X
		|| Route.RenderedBounds.Max.Y < Reserved.Bounds.Min.Y || Reserved.Bounds.Max.Y < Route.RenderedBounds.Min.Y)
	{
		return 0;
	}

	int32 Result = 0;
	for (int32 RouteSegment = 1; RouteSegment < Route.RenderedPoints.Num(); ++RouteSegment)
	{
		for (int32 ReservedSegment = 1; ReservedSegment < Reserved.Points.Num(); ++ReservedSegment)
		{
			if (!Budget.TryConsume()) { return Result; }
			const FSegmentInteraction Interaction = GetSegmentInteraction(
				Route.RenderedPoints[RouteSegment - 1],
				Route.RenderedPoints[RouteSegment],
				Reserved.Points[ReservedSegment - 1],
				Reserved.Points[ReservedSegment]
			);
			if (!Interaction.bIntersects) { continue; }
			if (Interaction.bCollinear
				|| !IsExactSharedTerminal(Interaction.Intersection, Route, Edge, Reserved, RouteSegment, ReservedSegment))
			{
				++Result;
			}
		}
	}
	return Result;
}

TMap<FString, int32> CollectRouteIntersectionCounts(
	const FRouteGeometry& Route,
	const FRerouteEdge& Edge,
	TConstArrayView<FReservedRoute> ReservedRoutes,
	FRoutePlanningBudget& Budget
)
{
	TMap<FString, int32> Result;
	for (const FReservedRoute& Reserved : ReservedRoutes)
	{
		const int32 IntersectionCount = CountRouteIntersections(Route, Edge, Reserved, Budget);
		if (IntersectionCount > 0) { Result.FindOrAdd(Reserved.StableKey) += IntersectionCount; }
		if (Budget.bExhausted) { break; }
	}
	return Result;
}

bool KnotHasAdditionalConnections(const UEdGraphNode* Node)
{
	const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
	if (Knot == nullptr) { return false; }
	const UEdGraphPin* InputPin = Knot->GetInputPin();
	const UEdGraphPin* OutputPin = Knot->GetOutputPin();
	const int32 LinkCount = (InputPin != nullptr ? InputPin->LinkedTo.Num() : 0)
						  + (OutputPin != nullptr ? OutputPin->LinkedTo.Num() : 0);
	return LinkCount > 1;
}

bool CandidatePreservesEndpointKnotTangents(
	TConstArrayView<FVector2D> Candidate, const FRerouteEdge& Edge, const UEdGraphNode* OutputNode, const UEdGraphNode* InputNode
)
{
	if (Candidate.Num() < 3) { return false; }
	if (OutputNode != nullptr && OutputNode->IsA<UK2Node_Knot>())
	{
		const bool bCandidateReversesOutput = Candidate[1].X < Edge.OutputAnchor.X;
		if (bCandidateReversesOutput != Edge.bReverseOutputTangent) { return false; }
	}
	if (InputNode != nullptr && InputNode->IsA<UK2Node_Knot>())
	{
		const bool bCandidateReversesInput = Edge.InputAnchor.X < Candidate[Candidate.Num() - 2].X;
		if (bCandidateReversesInput != Edge.bReverseInputTangent) { return false; }
	}
	return true;
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

		if (RouteIntersectsReservedRoute(Baseline, Edge, Reserved, Budget)) { return true; }
		if (Budget.bExhausted) { return false; }
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
	const FRouteGeometry BaselineGeometry =
		MakeRouteGeometry(Baseline, Edge.bReverseOutputTangent, Edge.bReverseInputTangent);
	const bool bObstructed = RenderedPolylineIntersectsAnyObstacle(
		BaselineGeometry, Baseline, Obstacles, OutputNode, InputNode, Edge.bReverseOutputTangent, Edge.bReverseInputTangent, Clearance, Budget
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
	// A multi-link manual knot derives its tangent direction from averages of every neighbor.
	// Replacing only this edge can flip that average after installation, invalidating all planned
	// spline geometry. Preserve such authored junctions rather than routing from stale tangents.
	if (KnotHasAdditionalConnections(OutputNode) || KnotHasAdditionalConnections(InputNode))
	{
		OutFailure =
			TEXT("A multi-link manual reroute endpoint was preserved because its post-route tangent is ambiguous.");
		return {};
	}
	// Every direct baseline is reserved before planning begins. Requiring each replacement's
	// crossing count with every reserved logical wire to be no greater than its current count
	// keeps both crossing identity and multiplicity monotonically non-increasing across the pass.
	const TMap<FString, int32> BaselineIntersectionCounts =
		CollectRouteIntersectionCounts(BaselineGeometry, Edge, ReservedRoutes, Budget);
	if (Budget.bExhausted) { return {}; }

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
	bool bRejectedCrossingRegression = false;
	const auto ConsiderCandidate = [&](TArray<FVector2D> Candidate, const int32 Order, const bool bPreferred)
	{
		if (Budget.bExhausted) { return; }
		Candidate = SimplifyPolyline(MoveTemp(Candidate));
		if (Candidate.Num() < 3 || !IsOrthogonalPolyline(Candidate)
			|| !CandidatePreservesEndpointKnotTangents(Candidate, Edge, OutputNode, InputNode))
		{
			return;
		}
		const FRouteGeometry CandidateGeometry =
			MakeRouteGeometry(Candidate, Edge.bReverseOutputTangent, Edge.bReverseInputTangent);
		if (RenderedPolylineSelfIntersects(CandidateGeometry, Budget) || Budget.bExhausted
			|| CandidateKnotBoundsCollide(CandidateGeometry, Obstacles, ReservedRoutes, Budget) || Budget.bExhausted
			|| RenderedPolylineIntersectsAnyObstacle(
				CandidateGeometry,
				Candidate,
				Obstacles,
				OutputNode,
				InputNode,
				Edge.bReverseOutputTangent,
				Edge.bReverseInputTangent,
				Clearance,
				Budget
			)
			|| Budget.bExhausted)
		{
			return;
		}

		bFoundObstacleClearRoute = true;
		const int32 RequiredKnots = Candidate.Num() - 2;
		MinimumRequiredKnots = FMath::Min(MinimumRequiredKnots, RequiredKnots);
		if (RequiredKnots > Settings.MaxKnotsPerWire) { return; }

		const TMap<FString, int32> CandidateIntersectionCounts =
			CollectRouteIntersectionCounts(CandidateGeometry, Edge, ReservedRoutes, Budget);
		if (Budget.bExhausted) { return; }
		for (const TPair<FString, int32>& CandidateIntersection : CandidateIntersectionCounts)
		{
			if (CandidateIntersection.Value > BaselineIntersectionCounts.FindRef(CandidateIntersection.Key))
			{
				bRejectedCrossingRegression = true;
				return;
			}
		}

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
	// Preserve the stock Kismet side chosen for user-authored reversed knots. Routing a
	// reversed endpoint through the ordinary right-output/left-input stubs would make the
	// rendered spline curl back through its endpoint node and can also change the knot's
	// reversal after the new neighbor is installed.
	const double OutputStubDirection = Edge.bReverseOutputTangent ? -1.0 : 1.0;
	const double InputStubDirection = Edge.bReverseInputTangent ? 1.0 : -1.0;
	const double BaseOutputChannelX = Snap(Edge.OutputAnchor.X + OutputStubDirection * TerminalStub, GridSize);
	const double BaseInputChannelX = Snap(Edge.InputAnchor.X + InputStubDirection * TerminalStub, GridSize);
	const double PortLaneStep = FMath::Max(Channel, Snap(RerouteKnotWidth + Channel, GridSize));
	const int32 CandidateCount = FMath::Clamp(2 + ReservedRoutes.Num() * 2, 2, MaxChannelCandidateCount);
	for (int32 CandidateIndex = 0; CandidateIndex < CandidateCount && !Budget.bExhausted; ++CandidateIndex)
	{
		const bool bTop = CandidateIndex % 2 == 0;
		const int32 ChannelRing = CandidateIndex / 2;
		const double RouteY = bTop ? TopCandidate - ChannelRing * Channel : BottomCandidate + ChannelRing * Channel;
		const double PortOffset = bGeometricallyBackward ? CandidateIndex * PortLaneStep : 0.0;
		const double OutputChannelX = BaseOutputChannelX + OutputStubDirection * PortOffset;
		const double InputChannelX = BaseInputChannelX + InputStubDirection * PortOffset;
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

	if (bRejectedCrossingRegression)
	{
		OutFailure =
			TEXT("Every route within the configured knot limit would introduce a new wire crossing or repeat an existing one.");
	}
	else if (bFoundObstacleClearRoute && MinimumRequiredKnots != MAX_int32)
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
	if (First.WasTrashed() || Second.WasTrashed()) { return false; }
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

UEdGraphPin* ResolveLivePin(const TWeakObjectPtr<UEdGraphNode>& Node, const FGuid& PinId)
{
	UEdGraphNode* LiveNode = Node.Get();
	if (LiveNode == nullptr || !PinId.IsValid()) { return nullptr; }
	for (UEdGraphPin* Pin : LiveNode->Pins)
	{
		if (Pin != nullptr && !Pin->WasTrashed() && Pin->GetOwningNodeUnchecked() == LiveNode && Pin->PinId == PinId)
		{
			return Pin;
		}
	}
	return nullptr;
}

bool CanStageDirectConnection(
	const UEdGraphSchema_K2& Schema,
	UEdGraphPin& First,
	UEdGraphPin& Second,
	const bool bMayReplaceSecondPinLink,
	ECanCreateConnectionResponse* OutResponse = nullptr
)
{
	const FPinConnectionResponse Response = Schema.CanCreateConnection(&First, &Second);
	if (OutResponse != nullptr) { *OutResponse = Response.Response.GetValue(); }
	return Response.Response == CONNECT_RESPONSE_MAKE
		|| (bMayReplaceSecondPinLink && Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B);
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
	UEdGraph& Graph,
	const TWeakObjectPtr<UEdGraphNode>& OutputNode,
	const FGuid& OutputPinId,
	const TWeakObjectPtr<UEdGraphNode>& InputNode,
	const FGuid& InputPinId,
	TArray<UK2Node_Knot*>& FailedKnots
)
{
	RemoveFailedKnots(Graph, FailedKnots);
	UEdGraphPin* OutputPin = ResolveLivePin(OutputNode, OutputPinId);
	UEdGraphPin* InputPin = ResolveLivePin(InputNode, InputPinId);
	if (OutputPin == nullptr || InputPin == nullptr) { return false; }
	if (HasExactMutualLink(*OutputPin, *InputPin)) { return true; }

	// This exact relationship was valid immediately before replacement. Normalize any asymmetric
	// residue as a last resort rather than leaving a Blueprint with silently corrupted topology.
	OutputPin->Modify(false);
	InputPin->Modify(false);
	OutputPin->LinkedTo.Remove(InputPin);
	InputPin->LinkedTo.Remove(OutputPin);
	OutputPin->MakeLinkTo(InputPin, false);
	return HasExactMutualLink(*OutputPin, *InputPin);
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
	const FPlannedReroute& PlannedRoute,
	TArray<UK2Node_Knot*>& OutKnots,
	FString& OutFailure,
	bool& bOutTopologyRestorationFailed
)
{
	const FRerouteEdge& Edge = PlannedRoute.Edge;
	const TConstArrayView<FVector2D> Waypoints = PlannedRoute.Waypoints;
	bOutTopologyRestorationFailed = false;
	const bool bPackageWasDirty = Graph.GetOutermost()->IsDirty();
	const auto RestoreCleanPackageAfterFailure = [&Graph, bPackageWasDirty]()
	{
		if (!bPackageWasDirty) { Graph.GetOutermost()->SetDirtyFlag(false); }
	};
	UEdGraphPin* OutputPin = ResolveLivePin(PlannedRoute.OutputNode, PlannedRoute.OutputPinId);
	UEdGraphPin* InputPin = ResolveLivePin(PlannedRoute.InputNode, PlannedRoute.InputPinId);
	if (OutputPin == nullptr || InputPin == nullptr || InputPin->LinkedTo.Num() != 1
		|| !HasExactMutualLink(*OutputPin, *InputPin))
	{
		OutFailure = TEXT("The original exclusive-input, exact reciprocal direct link no longer exists.");
		return false;
	}
	const FEdGraphPinType RoutePinType =
		OutputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard ? OutputPin->PinType : InputPin->PinType;

	OutKnots.Reserve(Waypoints.Num());
	for (int32 Index = 0; Index < Waypoints.Num(); ++Index)
	{
		const FVector2D TopLeft = Waypoints[Index] - FVector2D(RerouteKnotWidth * 0.5, RerouteKnotHeight * 0.5);
		UK2Node_Knot* Knot = CreateTransactionNeutralKnot(Graph, TopLeft);
		if (!Knot)
		{
			OutFailure = TEXT("Unreal failed to spawn a knot node.");
			RemoveFailedKnots(Graph, OutKnots);
			RestoreCleanPackageAfterFailure();
			return false;
		}
		Knot->NodeGuid = MakeDeterministicGuid(Edge.StableKey, Index, Graph);
		Knot->GetInputPin()->PinType = RoutePinType;
		Knot->GetOutputPin()->PinType = RoutePinType;
		OutKnots.Add(Knot);
	}

	ECanCreateConnectionResponse SourceResponse = CONNECT_RESPONSE_DISALLOW;
	ECanCreateConnectionResponse DestinationResponse = CONNECT_RESPONSE_DISALLOW;
	const bool bCanStageSource =
		CanStageDirectConnection(Schema, *OutputPin, *OutKnots[0]->GetInputPin(), false, &SourceResponse);
	const bool bCanStageDestination =
		CanStageDirectConnection(Schema, *OutKnots.Last()->GetOutputPin(), *InputPin, true, &DestinationResponse);
	if (!bCanStageSource || !bCanStageDestination)
	{
		OutFailure = FString::Printf(
			TEXT(
				"The K2 schema rejected a staged boundary connection (source response %d, destination response %d); "
				"the exact original direct link was never replaced."
			),
			static_cast<int32>(SourceResponse),
			static_cast<int32>(DestinationResponse)
		);
		RemoveFailedKnots(Graph, OutKnots);
		RestoreCleanPackageAfterFailure();
		return false;
	}
	for (int32 Index = 1; Index < OutKnots.Num(); ++Index)
	{
		ECanCreateConnectionResponse InternalResponse = CONNECT_RESPONSE_DISALLOW;
		if (!CanStageDirectConnection(
				Schema, *OutKnots[Index - 1]->GetOutputPin(), *OutKnots[Index]->GetInputPin(), false, &InternalResponse
			))
		{
			OutFailure = FString::Printf(
				TEXT("The K2 schema rejected staged internal knot connection %d (response %d); the original link was never replaced."),
				Index - 1,
				static_cast<int32>(InternalResponse)
			);
			RemoveFailedKnots(Graph, OutKnots);
			RestoreCleanPackageAfterFailure();
			return false;
		}
	}

	// Replace the direct link as one complete topology edit. Broadcasting a temporary disconnect here
	// can reconstruct wildcard/dynamic K2 pins before the replacement chain exists.
	OutputPin->BreakLinkTo(InputPin, false);
	OutputPin->MakeLinkTo(OutKnots[0]->GetInputPin(), false);
	for (int32 Index = 1; Index < OutKnots.Num(); ++Index)
	{
		OutKnots[Index - 1]->GetOutputPin()->MakeLinkTo(OutKnots[Index]->GetInputPin(), false);
	}
	OutKnots.Last()->GetOutputPin()->MakeLinkTo(InputPin, false);

	for (UK2Node_Knot* Knot : OutKnots)
	{
		Knot->PostReconstructNode();
	}
	if (UEdGraphPin* LiveOutputPin = ResolveLivePin(PlannedRoute.OutputNode, PlannedRoute.OutputPinId))
	{
		LiveOutputPin->GetOwningNode()->PinConnectionListChanged(LiveOutputPin);
	}
	if (UEdGraphPin* LiveInputPin = ResolveLivePin(PlannedRoute.InputNode, PlannedRoute.InputPinId))
	{
		LiveInputPin->GetOwningNode()->PinConnectionListChanged(LiveInputPin);
	}

	OutputPin = ResolveLivePin(PlannedRoute.OutputNode, PlannedRoute.OutputPinId);
	InputPin = ResolveLivePin(PlannedRoute.InputNode, PlannedRoute.InputPinId);
	bool bInternalChainInstalled = true;
	for (int32 Index = 1; Index < OutKnots.Num(); ++Index)
	{
		bInternalChainInstalled &=
			HasExactMutualLink(*OutKnots[Index - 1]->GetOutputPin(), *OutKnots[Index]->GetInputPin());
	}
	const bool bInstalled = OutputPin != nullptr && InputPin != nullptr
						 && HasExactMutualLink(*OutputPin, *OutKnots[0]->GetInputPin()) && bInternalChainInstalled
						 && HasExactMutualLink(*OutKnots.Last()->GetOutputPin(), *InputPin)
						 && InputPin->LinkedTo.Num() == 1;
	if (!bInstalled)
	{
		const FString VerificationFailure = FString::Printf(
			TEXT("live-output=%d live-input=%d internal-chain=%d input-link-count=%d"),
			OutputPin != nullptr,
			InputPin != nullptr,
			bInternalChainInstalled,
			InputPin != nullptr ? InputPin->LinkedTo.Num() : INDEX_NONE
		);
		const bool bRestored = RestoreOriginalDirectLink(
			Graph, PlannedRoute.OutputNode, PlannedRoute.OutputPinId, PlannedRoute.InputNode, PlannedRoute.InputPinId, OutKnots
		);
		bOutTopologyRestorationFailed = !bRestored;
		OutFailure =
			bRestored
				? FString::Printf(
					  TEXT("A K2 node reconstructed a route endpoint (%s); the exact original direct link was restored and verified."),
					  *VerificationFailure
				  )
				: FString::Printf(
					  TEXT("FATAL topology-safety failure: a K2 node reconstructed a route endpoint (%s) and the exact original direct link could not be restored."),
					  *VerificationFailure
				  );
		if (bRestored) { RestoreCleanPackageAfterFailure(); }
		return false;
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

TArray<FVector2D> FK2RerouteRouter::BuildRenderedPolyline(
	const TConstArrayView<FVector2D> LogicalPoints, const bool bReverseFirstTangent, const bool bReverseLastTangent
)
{ return FlattenRenderedPolyline(LogicalPoints, bReverseFirstTangent, bReverseLastTangent); }

bool FK2RerouteRouter::RenderedPolylinesIntersect(
	const TConstArrayView<FVector2D> FirstPolyline, const TConstArrayView<FVector2D> SecondPolyline
)
{
	return RenderedPolylinesIntersectExceptAtSharedTerminals(FirstPolyline, SecondPolyline, TConstArrayView<FVector2D>());
}

bool FK2RerouteRouter::RenderedPolylinesIntersectExceptAtSharedTerminals(
	const TConstArrayView<FVector2D> FirstPolyline,
	const TConstArrayView<FVector2D> SecondPolyline,
	const TConstArrayView<FVector2D> IgnoredSharedTerminals
)
{
	for (int32 FirstIndex = 1; FirstIndex < FirstPolyline.Num(); ++FirstIndex)
	{
		for (int32 SecondIndex = 1; SecondIndex < SecondPolyline.Num(); ++SecondIndex)
		{
			const FSegmentInteraction Interaction = GetSegmentInteraction(
				FirstPolyline[FirstIndex - 1],
				FirstPolyline[FirstIndex],
				SecondPolyline[SecondIndex - 1],
				SecondPolyline[SecondIndex]
			);
			if (!Interaction.bIntersects) { continue; }

			bool bIgnoredSharedTerminal = false;
			if (!Interaction.bCollinear && IsTerminalAdjacentSegment(Interaction.Intersection, FirstPolyline, FirstIndex)
				&& IsTerminalAdjacentSegment(Interaction.Intersection, SecondPolyline, SecondIndex))
			{
				for (const FVector2D& IgnoredTerminal : IgnoredSharedTerminals)
				{
					if (Interaction.Intersection.Equals(IgnoredTerminal))
					{
						bIgnoredSharedTerminal = true;
						break;
					}
				}
			}
			if (!bIgnoredSharedTerminal) { return true; }
		}
	}
	return false;
}

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
			OrderedKnots[Index].Knot->NodePosX + RerouteKnotWidth * 0.5,
			OrderedKnots[Index].Knot->NodePosY + RerouteKnotHeight * 0.5
		));
	}
	return !OutWaypoints.IsEmpty();
}

FReroutePlan FK2RerouteRouter::Plan(
	TConstArrayView<FRerouteEdge> Edges,
	TConstArrayView<FRerouteObstacle> Obstacles,
	const TSet<UEdGraphNode*>& Scope,
	const FRerouteSettings& Settings,
	const double GridSize
)
{
	FReroutePlan Result;
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
		if (Edge.bReservationOnly) { continue; }
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

		FPlannedReroute& PlannedRoute = Result.Routes.AddDefaulted_GetRef();
		PlannedRoute.Edge = Edge;
		PlannedRoute.Waypoints = MoveTemp(Waypoints);
		PlannedRoute.OutputNode = OutputNode;
		PlannedRoute.InputNode = InputNode;
		PlannedRoute.OutputPinId = Edge.OutputPin->PinId;
		PlannedRoute.InputPinId = Edge.InputPin->PinId;
		ReservedRoutes.Add(MakeReservation(Edge, PlannedRoute.Waypoints));
	}
	return Result;
}

FRerouteResult FK2RerouteRouter::ApplyPlan(UEdGraph& Graph, const FReroutePlan& Plan)
{
	FRerouteResult Result;
	Result.SkippedWires = Plan.SkippedWires;
	Result.bPlanningBudgetExhausted = Plan.bPlanningBudgetExhausted;
	Result.Diagnostics = Plan.Diagnostics;
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
	if (!Schema)
	{
		Result.SkippedWires += Plan.Routes.Num();
		Result.Diagnostics.Add(TEXT("Reroute generation is available only for K2 graphs."));
		return Result;
	}

	for (const FPlannedReroute& PlannedRoute : Plan.Routes)
	{
		TArray<UK2Node_Knot*> Knots;
		FString Failure;
		bool bTopologyRestorationFailed = false;
		if (InstallRoute(Graph, *Schema, PlannedRoute, Knots, Failure, bTopologyRestorationFailed))
		{
			++Result.RoutedWires;
			Result.CreatedKnots += Knots.Num();
			continue;
		}

		++Result.SkippedWires;
		Result.bTopologyRestorationFailed |= bTopologyRestorationFailed;
		Result.Diagnostics.Add(FString::Printf(TEXT("%s: %s"), *PlannedRoute.Edge.StableKey, *Failure));
		if (bTopologyRestorationFailed) { break; }
	}
	return Result;
}

FRerouteResult FK2RerouteRouter::Route(
	UEdGraph& Graph,
	TConstArrayView<FRerouteEdge> Edges,
	TConstArrayView<FRerouteObstacle> Obstacles,
	const TSet<UEdGraphNode*>& Scope,
	const FRerouteSettings& Settings,
	const double GridSize
)
{ return ApplyPlan(Graph, Plan(Edges, Obstacles, Scope, Settings, GridSize)); }
} // namespace GraphFormatter::K2
