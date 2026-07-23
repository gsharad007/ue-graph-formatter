#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

enum class EGraphFormatterRouteDirection : uint8
{
	Any,
	Left,
	Right,
	Up,
	Down,
};

struct FGraphFormatterAdaptagramsObstacle
{
	FVector2D Minimum = FVector2D::ZeroVector;
	FVector2D Maximum = FVector2D::ZeroVector;
};

struct FGraphFormatterAdaptagramsConnection
{
	int32 StableId = INDEX_NONE;
	FVector2D Source = FVector2D::ZeroVector;
	FVector2D Target = FVector2D::ZeroVector;
	EGraphFormatterRouteDirection SourceDirection = EGraphFormatterRouteDirection::Right;
	EGraphFormatterRouteDirection TargetDirection = EGraphFormatterRouteDirection::Left;
};

struct FGraphFormatterAdaptagramsSettings
{
	double ShapeBufferDistance = 24.0;
	double SegmentPenalty = 50.0;
	double CrossingPenalty = 200.0;
	double SharedPathPenalty = 100.0;
	double PortDirectionPenalty = 100.0;
	double ReverseDirectionPenalty = 250.0;
	double IdealNudgingDistance = 16.0;
};

struct FGraphFormatterAdaptagramsRoute
{
	int32 StableId = INDEX_NONE;
	TArray<FVector2D> Points;
};

struct FGraphFormatterAdaptagramsResult
{
	bool bSucceeded = false;
	TArray<FGraphFormatterAdaptagramsRoute> Routes;
	FString Diagnostic;
};

/** LGPL implementation boundary for Adaptagrams/libavoid. */
class IGraphFormatterAdaptagramsModule : public IModuleInterface
{
public:
	[[nodiscard]]
	static IGraphFormatterAdaptagramsModule& Get()
	{ return FModuleManager::LoadModuleChecked<IGraphFormatterAdaptagramsModule>(TEXT("GraphFormatterAdaptagrams")); }

	[[nodiscard]]
	static bool IsAvailable()
	{ return FModuleManager::Get().IsModuleLoaded(TEXT("GraphFormatterAdaptagrams")); }

	[[nodiscard]]
	virtual FGraphFormatterAdaptagramsResult RouteOrthogonal(
		TConstArrayView<FGraphFormatterAdaptagramsObstacle> Obstacles,
		TConstArrayView<FGraphFormatterAdaptagramsConnection> Connections,
		const FGraphFormatterAdaptagramsSettings& Settings
	) const = 0;
};
