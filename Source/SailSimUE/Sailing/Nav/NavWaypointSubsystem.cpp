#include "Sailing/Nav/NavWaypointSubsystem.h"

void UNavWaypointSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Waypoints.Reset();
	SelectedIndex = INDEX_NONE;
	NextId = 1;
	Revision = 0;
}

void UNavWaypointSubsystem::SetSelectedIndex(int32 Index)
{
	if (Index == INDEX_NONE)
	{
		SelectedIndex = INDEX_NONE;
		Bump();
		return;
	}
	if (!Waypoints.IsValidIndex(Index)) return;
	if (SelectedIndex == Index) return;
	SelectedIndex = Index;
	Bump();
}

int32 UNavWaypointSubsystem::AddWaypoint(double Lat, double Lon)
{
	FNavWaypoint W;
	W.Id = NextId++;
	W.Lat = Lat;
	W.Lon = Lon;
	Waypoints.Add(W);
	SelectedIndex = Waypoints.Num() - 1;
	Bump();
	return SelectedIndex;
}

void UNavWaypointSubsystem::RemoveAt(int32 Index)
{
	if (!Waypoints.IsValidIndex(Index)) return;
	Waypoints.RemoveAt(Index);
	if (SelectedIndex == Index) SelectedIndex = INDEX_NONE;
	else if (SelectedIndex > Index) --SelectedIndex;
	if (SelectedIndex >= Waypoints.Num()) SelectedIndex = Waypoints.Num() - 1;
	Bump();
}

void UNavWaypointSubsystem::Move(int32 Index, int32 Delta)
{
	const int32 J = Index + Delta;
	if (!Waypoints.IsValidIndex(Index) || !Waypoints.IsValidIndex(J)) return;
	Waypoints.Swap(Index, J);
	SelectedIndex = J;
	Bump();
}

void UNavWaypointSubsystem::Clear()
{
	Waypoints.Reset();
	SelectedIndex = INDEX_NONE;
	Bump();
}

bool UNavWaypointSubsystem::GetWaypoint(int32 Index, FNavWaypoint& Out) const
{
	if (!Waypoints.IsValidIndex(Index)) return false;
	Out = Waypoints[Index];
	return true;
}
