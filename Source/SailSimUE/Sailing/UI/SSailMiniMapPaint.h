#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class UNoaaChartSubsystem;
class UNavWaypointSubsystem;
class UGameInstance;
class UTexture2D;

/** Chart plot tool mode (web NavChart._plotMode). */
enum class ENavPlotMode : uint8
{
	Pan = 0,
	Add = 1,
	Delete = 2,
};

/**
 * NOAA chart compositor — zoom/pan/follow match sail-sim nav-chart.js.
 * Waypoint plot tools + route overlay match dist nav-minimap.js.
 */
class SSailMiniMapPaint : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSailMiniMapPaint)
		: _BoatLat(41.48)
		, _BoatLon(-70.22)
		, _Heading(0.f)
		, _Twd(0.f)
	{}
		SLATE_ATTRIBUTE(double, BoatLat)
		SLATE_ATTRIBUTE(double, BoatLon)
		SLATE_ATTRIBUTE(float, Heading)
		SLATE_ATTRIBUTE(float, Twd)
		SLATE_ARGUMENT(TWeakObjectPtr<UGameInstance>, GameInstance)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(320.f, 180.f); }

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

	float GetZoom() const { return UserZoom; }
	bool IsFollowing() const { return bFollow; }
	void SetFollow(bool bOn) { bFollow = bOn; }

	ENavPlotMode GetPlotMode() const { return PlotMode; }
	void SetPlotMode(ENavPlotMode Mode) { PlotMode = Mode; }

	/** Center chart on lat/lon and break follow (web navGotoWaypoint). */
	void CenterOn(double Lat, double Lon);

	bool IsWindFieldVisible() const { return bShowWindField; }
	void SetWindFieldVisible(bool bOn) { bShowWindField = bOn; }

private:
	TAttribute<double> BoatLat;
	TAttribute<double> BoatLon;
	TAttribute<float> Heading;
	TAttribute<float> Twd;
	TWeakObjectPtr<UGameInstance> GameInstance;

	mutable float UserZoom = 13.f;
	mutable int32 StickyTileZ = 13;
	mutable int32 ZoomDir = 0;

	mutable bool bFollow = true;
	mutable bool bPanning = false;
	mutable bool bPanMoved = false;
	mutable FVector2D LastMouseAbs = FVector2D::ZeroVector;
	mutable FVector2D PanStartAbs = FVector2D::ZeroVector;
	mutable double ViewLat = 41.48;
	mutable double ViewLon = -70.22;
	mutable bool bViewInit = false;

	mutable ENavPlotMode PlotMode = ENavPlotMode::Pan;
	mutable TMap<FString, TSharedPtr<FSlateBrush>> BrushPool;

	UNoaaChartSubsystem* GetCharts() const;
	UNavWaypointSubsystem* GetWaypoints() const;
	TSharedPtr<FSlateBrush> BrushFor(UTexture2D* Tex, float U0, float V0, float U1, float V1) const;
	int32 ResolveTileZ() const;
	void ZoomAt(float DeltaZoom, const FGeometry& Geo, FVector2D LocalAnchor);
	FVector2D LatLonToLocal(double Lat, double Lon, double CenterLat, double CenterLon,
		float Zf, int32 TileZ, const FVector2D& Size) const;
	void LocalToLatLon(FVector2D Local, const FVector2D& Size, double& OutLat, double& OutLon) const;
	int32 HitTestWaypoint(FVector2D Local, const FVector2D& Size, float HitR = 14.f) const;
	void DrawBoatMarker(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
		FVector2D Pos, float HdgDeg) const;
	void DrawWaypointsAndRoute(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
		const FVector2D& Sz, double BLat, double BLon, float Zf, int32 TileZ) const;
	/** Debug: wind vector field (direction TO / strength by color). */
	void DrawWindField(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
		const FVector2D& Sz, double CLat, double CLon, float Zf, int32 TileZ) const;

	/** Toggle wind vector grid (debug). Default on. */
	mutable bool bShowWindField = true;
};
