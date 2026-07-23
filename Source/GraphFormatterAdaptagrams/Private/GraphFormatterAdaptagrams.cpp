#include "GraphFormatterAdaptagrams.h"

#include "libavoid/libavoid.h"

namespace
{
[[nodiscard]]
Avoid::ConnDirFlags ToAvoidDirection(const EGraphFormatterRouteDirection Direction) noexcept
{
	switch (Direction)
	{
		case EGraphFormatterRouteDirection::Left:
			return Avoid::ConnDirLeft;
		case EGraphFormatterRouteDirection::Right:
			return Avoid::ConnDirRight;
		case EGraphFormatterRouteDirection::Up:
			return Avoid::ConnDirUp;
		case EGraphFormatterRouteDirection::Down:
			return Avoid::ConnDirDown;
		case EGraphFormatterRouteDirection::Any:
		default:
			return Avoid::ConnDirAll;
	}
}

void ConfigureRouter(Avoid::Router& Router, const FGraphFormatterAdaptagramsSettings& Settings)
{
	Router.setRoutingParameter(Avoid::shapeBufferDistance, Settings.ShapeBufferDistance);
	Router.setRoutingParameter(Avoid::segmentPenalty, Settings.SegmentPenalty);
	Router.setRoutingParameter(Avoid::crossingPenalty, Settings.CrossingPenalty);
	Router.setRoutingParameter(Avoid::fixedSharedPathPenalty, Settings.SharedPathPenalty);
	Router.setRoutingParameter(Avoid::portDirectionPenalty, Settings.PortDirectionPenalty);
	Router.setRoutingParameter(Avoid::reverseDirectionPenalty, Settings.ReverseDirectionPenalty);
	Router.setRoutingParameter(Avoid::idealNudgingDistance, Settings.IdealNudgingDistance);
	Router.setRoutingOption(Avoid::nudgeOrthogonalSegmentsConnectedToShapes, true);
	Router.setRoutingOption(Avoid::nudgeOrthogonalTouchingColinearSegments, true);
	Router.setRoutingOption(Avoid::nudgeSharedPathsWithCommonEndPoint, true);
}

void AddObstacles(Avoid::Router& Router, const TConstArrayView<FGraphFormatterAdaptagramsObstacle> Obstacles)
{
	for (int32 Index = 0; Index < Obstacles.Num(); ++Index)
	{
		const FGraphFormatterAdaptagramsObstacle& Obstacle = Obstacles[Index];
		Avoid::Rectangle Rectangle(
			Avoid::Point(Obstacle.Minimum.X, Obstacle.Minimum.Y), Avoid::Point(Obstacle.Maximum.X, Obstacle.Maximum.Y)
		);
		new Avoid::ShapeRef(&Router, Rectangle, static_cast<unsigned int>(Index + 1));
	}
}

TArray<Avoid::ConnRef*> AddConnections(
	Avoid::Router& Router, const TConstArrayView<FGraphFormatterAdaptagramsConnection> Connections, const int32 FirstObjectId
)
{
	TArray<Avoid::ConnRef*> References;
	References.Reserve(Connections.Num());
	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		const FGraphFormatterAdaptagramsConnection& Connection = Connections[Index];
		const Avoid::ConnEnd Source(
			Avoid::Point(Connection.Source.X, Connection.Source.Y), ToAvoidDirection(Connection.SourceDirection)
		);
		const Avoid::ConnEnd Target(
			Avoid::Point(Connection.Target.X, Connection.Target.Y), ToAvoidDirection(Connection.TargetDirection)
		);
		Avoid::ConnRef* Reference =
			new Avoid::ConnRef(&Router, Source, Target, static_cast<unsigned int>(FirstObjectId + Index));
		Reference->setRoutingType(Avoid::ConnType_Orthogonal);
		References.Add(Reference);
	}
	return References;
}

FGraphFormatterAdaptagramsRoute CaptureRoute(const int32 StableId, Avoid::ConnRef& Reference)
{
	FGraphFormatterAdaptagramsRoute Result;
	Result.StableId = StableId;
	for (const Avoid::Point& Point : Reference.displayRoute().ps)
	{
		Result.Points.Emplace(Point.x, Point.y);
	}
	return Result;
}
} // namespace

class FGraphFormatterAdaptagramsModule final : public IGraphFormatterAdaptagramsModule
{
public:
	[[nodiscard]]
	virtual FGraphFormatterAdaptagramsResult RouteOrthogonal(
		const TConstArrayView<FGraphFormatterAdaptagramsObstacle> Obstacles,
		const TConstArrayView<FGraphFormatterAdaptagramsConnection> Connections,
		const FGraphFormatterAdaptagramsSettings& Settings
	) const override
	{
		FGraphFormatterAdaptagramsResult Result;
		if (Connections.IsEmpty())
		{
			Result.bSucceeded = true;
			Result.Diagnostic = TEXT("No connections required routing.");
			return Result;
		}

		Avoid::Router Router(Avoid::OrthogonalRouting);
		ConfigureRouter(Router, Settings);
		AddObstacles(Router, Obstacles);
		const TArray<Avoid::ConnRef*> References = AddConnections(Router, Connections, Obstacles.Num() + 1);
		if (!Router.processTransaction())
		{
			Result.Diagnostic = TEXT("libavoid reported no completed routing transaction.");
			return Result;
		}

		Result.Routes.Reserve(References.Num());
		for (int32 Index = 0; Index < References.Num(); ++Index)
		{
			Avoid::ConnRef* Reference = References[Index];
			if (Reference == nullptr || Reference->displayRoute().ps.size() < 2)
			{
				Result.Diagnostic = FString::Printf(TEXT("libavoid returned an empty route for connection %d."), Index);
				return Result;
			}
			Result.Routes.Add(CaptureRoute(Connections[Index].StableId, *Reference));
		}
		Result.bSucceeded = true;
		Result.Diagnostic = FString::Printf(TEXT("libavoid routed %d connections."), Result.Routes.Num());
		return Result;
	}
};

IMPLEMENT_MODULE(FGraphFormatterAdaptagramsModule, GraphFormatterAdaptagrams)
