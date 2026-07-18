#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NavWaypointSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FNavWaypoint
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = Nav)
	int32 Id = 0;

	UPROPERTY(BlueprintReadOnly, Category = Nav)
	double Lat = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = Nav)
	double Lon = 0.0;
};

/**
 * Shared chart route store — matches sail-sim dist nav-minimap.js waypoint list.
 * Chart, waypoint panel, and autopilot NAV mode all read/write this.
 */
UCLASS()
class SAILSIMUE_API UNavWaypointSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	const TArray<FNavWaypoint>& GetWaypoints() const { return Waypoints; }
	int32 Num() const { return Waypoints.Num(); }
	int32 GetSelectedIndex() const { return SelectedIndex; }

	UFUNCTION(BlueprintCallable, Category = "Nav|Waypoints")
	void SetSelectedIndex(int32 Index);

	UFUNCTION(BlueprintCallable, Category = "Nav|Waypoints")
	int32 AddWaypoint(double Lat, double Lon);

	UFUNCTION(BlueprintCallable, Category = "Nav|Waypoints")
	void RemoveAt(int32 Index);

	UFUNCTION(BlueprintCallable, Category = "Nav|Waypoints")
	void Move(int32 Index, int32 Delta);

	UFUNCTION(BlueprintCallable, Category = "Nav|Waypoints")
	void Clear();

	bool GetWaypoint(int32 Index, FNavWaypoint& Out) const;

	/** Generation bump so UI/chart repaint when list changes. */
	int32 GetRevision() const { return Revision; }

private:
	TArray<FNavWaypoint> Waypoints;
	int32 SelectedIndex = INDEX_NONE;
	int32 NextId = 1;
	int32 Revision = 0;

	void Bump() { ++Revision; }
};
