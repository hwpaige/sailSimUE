#include "Sailing/UI/SSailMiniMapPaint.h"
#include "Sailing/UI/SailSimStyle.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Nav/NoaaChartSubsystem.h"
#include "Sailing/Nav/NavWaypointSubsystem.h"
#include "Sailing/Wind/WindFieldSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"

namespace
{
	// Hold fine chart tiles past the integer zoom boundary when zooming out.
	// Larger = finer tiles last longer (scale stretch up to ~2^H before swap).
	constexpr float ZoomOutHysteresis = 1.2f;
	constexpr float PanBreakPx = 4.f;
}

void SSailMiniMapPaint::Construct(const FArguments& InArgs)
{
	BoatLat = InArgs._BoatLat;
	BoatLon = InArgs._BoatLon;
	Heading = InArgs._Heading;
	Twd = InArgs._Twd;
	GameInstance = InArgs._GameInstance;
	UserZoom = 13.f;
	StickyTileZ = 13;
}

UNoaaChartSubsystem* SSailMiniMapPaint::GetCharts() const
{
	if (UGameInstance* GI = GameInstance.Get())
	{
		return GI->GetSubsystem<UNoaaChartSubsystem>();
	}
	return nullptr;
}

UNavWaypointSubsystem* SSailMiniMapPaint::GetWaypoints() const
{
	if (UGameInstance* GI = GameInstance.Get())
	{
		return GI->GetSubsystem<UNavWaypointSubsystem>();
	}
	return nullptr;
}

void SSailMiniMapPaint::CenterOn(double Lat, double Lon)
{
	bFollow = false;
	ViewLat = Lat;
	ViewLon = Lon;
	bViewInit = true;
}

void SSailMiniMapPaint::LocalToLatLon(FVector2D Local, const FVector2D& Size, double& OutLat, double& OutLon) const
{
	const float Zf = FMath::Clamp(UserZoom, 9.f, 16.f);
	const int32 TileZ = ResolveTileZ();
	const double Scale = FMath::Pow(2.0, double(Zf) - double(TileZ));
	double Cx = 0, Cy = 0;
	FNavGeo::LonLatToWorldPx(ViewLon, ViewLat, TileZ, Cx, Cy);
	const double Wx = Cx + (double(Local.X) - Size.X * 0.5) / Scale;
	const double Wy = Cy + (double(Local.Y) - Size.Y * 0.5) / Scale;
	FNavGeo::WorldPxToLonLat(Wx, Wy, TileZ, OutLon, OutLat);
}

int32 SSailMiniMapPaint::HitTestWaypoint(FVector2D Local, const FVector2D& Size, float HitR) const
{
	UNavWaypointSubsystem* Nav = GetWaypoints();
	if (!Nav || Nav->Num() == 0) return INDEX_NONE;
	const float Zf = FMath::Clamp(UserZoom, 9.f, 16.f);
	const int32 TileZ = ResolveTileZ();
	const double CLat = bFollow ? BoatLat.Get(FNavGeo::BoatStartLat) : ViewLat;
	const double CLon = bFollow ? BoatLon.Get(FNavGeo::BoatStartLon) : ViewLon;
	int32 Best = INDEX_NONE;
	float BestD = HitR * HitR;
	const TArray<FNavWaypoint>& Wps = Nav->GetWaypoints();
	for (int32 I = 0; I < Wps.Num(); ++I)
	{
		const FVector2D S = LatLonToLocal(Wps[I].Lat, Wps[I].Lon, CLat, CLon, Zf, TileZ, Size);
		const float D = FVector2D::DistSquared(S, Local);
		if (D <= BestD)
		{
			BestD = D;
			Best = I;
		}
	}
	return Best;
}

void SSailMiniMapPaint::DrawWaypointsAndRoute(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
	const FVector2D& Sz, double BLat, double BLon, float Zf, int32 TileZ) const
{
	UNavWaypointSubsystem* Nav = GetWaypoints();
	if (!Nav || Nav->Num() == 0) return;

	const double CLat = bFollow ? BLat : ViewLat;
	const double CLon = bFollow ? BLon : ViewLon;
	const TArray<FNavWaypoint>& Wps = Nav->GetWaypoints();
	const int32 Sel = Nav->GetSelectedIndex();

	// Route polyline: boat → WP1 → WP2 …
	TArray<FVector2D> Route;
	Route.Reserve(Wps.Num() + 1);
	Route.Add(LatLonToLocal(BLat, BLon, CLat, CLon, Zf, TileZ, Sz));
	for (const FNavWaypoint& W : Wps)
	{
		Route.Add(LatLonToLocal(W.Lat, W.Lon, CLat, CLon, Zf, TileZ, Sz));
	}
	if (Route.Num() >= 2)
	{
		// Under-glow
		FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Route,
			ESlateDrawEffect::None, FLinearColor(0.08f, 0.12f, 0.16f, 0.40f), true, 7.5f);
		// Orange solid route stroke (Slate has no dash)
		FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Route,
			ESlateDrawEffect::None, FLinearColor::FromSRGBColor(FColor(0xff, 0x9f, 0x5a)), true, 3.6f);
	}

	// Markers
	for (int32 I = 0; I < Wps.Num(); ++I)
	{
		const FVector2D P = LatLonToLocal(Wps[I].Lat, Wps[I].Lon, CLat, CLon, Zf, TileZ, Sz);
		const bool bSel = (I == Sel);
		const float R = bSel ? 9.f : 7.5f;
		// Shadow
		{
			const FSlateRoundedBoxBrush Halo(FLinearColor(0.f, 0.f, 0.f, 0.28f), 99.f);
			FSlateDrawElement::MakeBox(Out, Layer + 2, Geo.ToPaintGeometry(
				FVector2D(R * 2.f + 4.f, R * 2.f + 4.f),
				FSlateLayoutTransform(P - FVector2D(R + 2.f, R + 2.f))),
				&Halo, ESlateDrawEffect::None);
		}
		const FLinearColor Fill = bSel
			? FLinearColor::FromSRGBColor(FColor(0xff, 0xb3, 0x6b))
			: FLinearColor::FromSRGBColor(FColor(0xff, 0x9f, 0x5a));
		const FSlateRoundedBoxBrush Mark(Fill, 99.f);
		FSlateDrawElement::MakeBox(Out, Layer + 3, Geo.ToPaintGeometry(
			FVector2D(R * 2.f, R * 2.f),
			FSlateLayoutTransform(P - FVector2D(R, R))),
			&Mark, ESlateDrawEffect::None);
		// Number — same face as AP HDG/AWA values (dark ink for contrast on pin fill)
		FSlateDrawElement::MakeText(Out, Layer + 4,
			Geo.ToPaintGeometry(FVector2D(22.f, 16.f), FSlateLayoutTransform(P + FVector2D(-5.f, -8.f))),
			FString::Printf(TEXT("%d"), I + 1),
			FSailSimStyle::FontApStatVal(),
			ESlateDrawEffect::None,
			FLinearColor(0.12f, 0.08f, 0.04f, 0.95f));
	}
}

TSharedPtr<FSlateBrush> SSailMiniMapPaint::BrushFor(UTexture2D* Tex, float U0, float V0, float U1, float V1) const
{
	if (!Tex) return nullptr;
	// Unique brush per texture + UV so we never mutate a shared brush mid-frame
	const FString Key = FString::Printf(TEXT("%p_%.4f_%.4f_%.4f_%.4f"), Tex, U0, V0, U1, V1);
	if (TSharedPtr<FSlateBrush>* Found = BrushPool.Find(Key))
	{
		return *Found;
	}
	TSharedPtr<FSlateBrush> B = MakeShared<FSlateBrush>();
	B->SetResourceObject(Tex);
	B->ImageSize = FVector2D(FNavGeo::TileSize, FNavGeo::TileSize);
	B->DrawAs = ESlateBrushDrawType::Image;
	B->Tiling = ESlateBrushTileType::NoTile;
	B->SetUVRegion(FBox2D(FVector2D(U0, V0), FVector2D(U1, V1)));
	BrushPool.Add(Key, B);
	// Cap pool
	if (BrushPool.Num() > 400)
	{
		BrushPool.Empty(64);
		BrushPool.Add(Key, B);
	}
	return B;
}

int32 SSailMiniMapPaint::ResolveTileZ() const
{
	// Ideal tile pyramid level = floor(userZoom). On zoom-out, keep the finer
	// sticky level until fractional zoom falls more than ZoomOutHysteresis
	// below that level (see nav-chart.js _tileZoomInt).
	const float Z = FMath::Clamp(UserZoom, 9.f, 16.f);
	const int32 Ideal = FMath::Clamp(int32(FMath::FloorToFloat(Z + 1e-5f)), 9, 16);

	if (Ideal > StickyTileZ)
	{
		// Zooming in: pick up finer tiles immediately.
		StickyTileZ = Ideal;
	}
	else if (Ideal < StickyTileZ)
	{
		// Zooming out: keep detailed tiles while Z is still in
		// [StickyTileZ - H, StickyTileZ). Drop only once past the band.
		const bool bHoldFine =
			(ZoomDir <= 0) && (Z >= float(StickyTileZ) - ZoomOutHysteresis);
		if (!bHoldFine)
		{
			StickyTileZ = Ideal;
		}
	}
	return StickyTileZ;
}

FVector2D SSailMiniMapPaint::LatLonToLocal(double Lat, double Lon, double CenterLat, double CenterLon,
	float Zf, int32 TileZ, const FVector2D& Size) const
{
	const double Scale = FMath::Pow(2.0, double(Zf) - double(TileZ));
	double Cx = 0, Cy = 0, Px = 0, Py = 0;
	FNavGeo::LonLatToWorldPx(CenterLon, CenterLat, TileZ, Cx, Cy);
	FNavGeo::LonLatToWorldPx(Lon, Lat, TileZ, Px, Py);
	return FVector2D(
		float(Size.X * 0.5 + (Px - Cx) * Scale),
		float(Size.Y * 0.5 + (Py - Cy) * Scale));
}

void SSailMiniMapPaint::ZoomAt(float DeltaZoom, const FGeometry& Geo, FVector2D LocalAnchor)
{
	const float Z0 = FMath::Clamp(UserZoom, 9.f, 16.f);
	const float Z1 = FMath::Clamp(Z0 + DeltaZoom, 9.f, 16.f);
	if (FMath::IsNearlyEqual(Z0, Z1, 1e-5f)) return;

	ZoomDir = (Z1 < Z0) ? -1 : 1;
	const FVector2D Sz = Geo.GetLocalSize();
	if (Sz.X < 1.f || Sz.Y < 1.f) return;

	// Sync view center
	const double BLat = BoatLat.Get(FNavGeo::BoatStartLat);
	const double BLon = BoatLon.Get(FNavGeo::BoatStartLon);
	if (bFollow || !bViewInit)
	{
		ViewLat = BLat;
		ViewLon = BLon;
		bViewInit = true;
	}

	if (bFollow)
	{
		// Zoom about map centre (boat) — vessel stays put (web behaviour)
		UserZoom = Z1;
		return;
	}

	// Free-pan: zoom under cursor — keep world point under anchor fixed
	const int32 TileZ0 = ResolveTileZ();
	const double Scale0 = FMath::Pow(2.0, double(Z0) - double(TileZ0));
	double Cx = 0, Cy = 0;
	FNavGeo::LonLatToWorldPx(ViewLon, ViewLat, TileZ0, Cx, Cy);
	const double Ax = Cx + (double(LocalAnchor.X) - Sz.X * 0.5) / Scale0;
	const double Ay = Cy + (double(LocalAnchor.Y) - Sz.Y * 0.5) / Scale0;
	double AnchorLon = 0, AnchorLat = 0;
	FNavGeo::WorldPxToLonLat(Ax, Ay, TileZ0, AnchorLon, AnchorLat);

	UserZoom = Z1;
	const int32 TileZ1 = ResolveTileZ();
	const double Scale1 = FMath::Pow(2.0, double(Z1) - double(TileZ1));
	double Awx = 0, Awy = 0;
	FNavGeo::LonLatToWorldPx(AnchorLon, AnchorLat, TileZ1, Awx, Awy);
	const double NewCx = Awx - (double(LocalAnchor.X) - Sz.X * 0.5) / Scale1;
	const double NewCy = Awy - (double(LocalAnchor.Y) - Sz.Y * 0.5) / Scale1;
	FNavGeo::WorldPxToLonLat(NewCx, NewCy, TileZ1, ViewLon, ViewLat);
}

int32 SSailMiniMapPaint::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const bool bEnabled = ShouldBeEnabled(bParentEnabled);
	const ESlateDrawEffect Effects = bEnabled ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
	const FVector2D Sz = AllottedGeometry.GetLocalSize();
	if (Sz.X < 2.f || Sz.Y < 2.f) return LayerId;

	// NOAA chart water — rounded to the inner well (outer glass card provides the main curve)
	const FLinearColor ChartWater = FLinearColor::FromSRGBColor(FColor(0xaf, 0xcd, 0xe1));
	{
		const FSlateRoundedBoxBrush Plate(ChartWater, FSailSimStyle::RadiusMapInner);
		FSlateDrawElement::MakeBox(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(),
			&Plate, Effects);
	}

	const double BLat = BoatLat.Get(FNavGeo::BoatStartLat);
	const double BLon = BoatLon.Get(FNavGeo::BoatStartLon);
	if (bFollow || !bViewInit)
	{
		ViewLat = BLat;
		ViewLon = BLon;
		bViewInit = true;
	}

	const float Zf = FMath::Clamp(UserZoom, 9.f, 16.f);
	const int32 TileZ = ResolveTileZ();
	const double Scale = FMath::Pow(2.0, double(Zf) - double(TileZ));

	double Cx = 0.0, Cy = 0.0;
	FNavGeo::LonLatToWorldPx(ViewLon, ViewLat, TileZ, Cx, Cy);

	// Pad +1 tile so edges stay filled while panning/zooming (web)
	const double HalfW = (Sz.X * 0.5) / Scale;
	const double HalfH = (Sz.Y * 0.5) / Scale;
	const int32 Tx0 = FMath::FloorToInt((Cx - HalfW) / FNavGeo::TileSize) - 1;
	const int32 Tx1 = FMath::FloorToInt((Cx + HalfW) / FNavGeo::TileSize) + 1;
	const int32 Ty0 = FMath::FloorToInt((Cy - HalfH) / FNavGeo::TileSize) - 1;
	const int32 Ty1 = FMath::FloorToInt((Cy + HalfH) / FNavGeo::TileSize) + 1;

	UNoaaChartSubsystem* Charts = GetCharts();
	if (Charts && Charts->HasTileRoot())
	{
		for (int32 Ty = Ty0; Ty <= Ty1; ++Ty)
		{
			for (int32 Tx = Tx0; Tx <= Tx1; ++Tx)
			{
				const UNoaaChartSubsystem::FResolvedTile R = Charts->ResolveTile(TileZ, Tx, Ty, 9);
				if (!R.Texture) continue;

				TSharedPtr<FSlateBrush> Brush = BrushFor(R.Texture, R.U0, R.V0, R.U1, R.V1);
				if (!Brush.IsValid()) continue;

				// Destination in local space (same math as web translate/scale)
				const double TileWorldX = double(Tx) * FNavGeo::TileSize;
				const double TileWorldY = double(Ty) * FNavGeo::TileSize;
				const float Dx = float((TileWorldX - (Cx - HalfW)) * Scale);
				const float Dy = float((TileWorldY - (Cy - HalfH)) * Scale);
				// Slight overlap (0.5px) kills hairline gaps between tiles
				const float Dw = float(FNavGeo::TileSize * Scale) + 0.5f;
				const float Dh = float(FNavGeo::TileSize * Scale) + 0.5f;

				FSlateDrawElement::MakeBox(
					OutDrawElements, LayerId + 1,
					AllottedGeometry.ToPaintGeometry(
						FVector2D(Dw, Dh),
						FSlateLayoutTransform(FVector2D(Dx, Dy))),
					Brush.Get(),
					Effects);
			}
		}
	}
	else
	{
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(
				FVector2D(Sz.X - 16.f, 18.f),
				FSlateLayoutTransform(FVector2D(10.f, Sz.Y * 0.45f))),
			TEXT("NOAA tiles missing — Content/Charts/nantucket"),
			FSailSimStyle::FontApStatVal(), Effects, FSailSimStyle::ApStatVal);
	}

	// Route + waypoints under boat marker
	DrawWaypointsAndRoute(AllottedGeometry, OutDrawElements, LayerId + 4, Sz, BLat, BLon, Zf, TileZ);

	// Spatial wind field (debug) under boat
	if (bShowWindField)
	{
		DrawWindField(AllottedGeometry, OutDrawElements, LayerId + 6, Sz, ViewLat, ViewLon, Zf, TileZ);
	}

	// Boat at projected lat/lon (center when following)
	const FVector2D BoatPos = LatLonToLocal(BLat, BLon, ViewLat, ViewLon, Zf, TileZ, Sz);
	DrawBoatMarker(AllottedGeometry, OutDrawElements, LayerId + 8, BoatPos, Heading.Get(0.f));

	// Wind FROM feather at boat (local sample — matches instruments)
	{
		const float Wr = FMath::DegreesToRadians(Twd.Get(0.f));
		const FVector2D W0 = BoatPos - FVector2D(FMath::Sin(Wr), -FMath::Cos(Wr)) * 26.f;
		TArray<FVector2D> WindLine;
		WindLine.Add(W0);
		WindLine.Add(BoatPos);
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 10, AllottedGeometry.ToPaintGeometry(),
			WindLine, Effects, FLinearColor(0.75f, 0.18f, 0.12f, 0.92f), true, 2.f);
	}

	return LayerId + 12;
}

void SSailMiniMapPaint::DrawWindField(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
	const FVector2D& Sz, double CLat, double CLon, float Zf, int32 TileZ) const
{
	// Resolve play world robustly (GI->GetWorld can be null in some editor paint paths).
	UWorld* World = nullptr;
	if (UGameInstance* GI = GameInstance.Get())
	{
		World = GI->GetWorld();
	}
	if (!World && GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if ((Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game) && Ctx.World())
			{
				World = Ctx.World();
				break;
			}
		}
	}
	if (!World) return;

	UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>();
	if (!Wind)
	{
		// Still draw a simple constant field from boat TWD so debug overlay is never blank.
		const int32 Nx = 9, Ny = 6;
		const float Wr = FMath::DegreesToRadians(Twd.Get(225.f) + 180.f);
		const FVector2D Dir(FMath::Sin(Wr), -FMath::Cos(Wr));
		for (int32 Jy = 0; Jy < Ny; ++Jy)
		{
			for (int32 Jx = 0; Jx < Nx; ++Jx)
			{
				const FVector2D Local(((Jx + 0.5f) / Nx) * Sz.X, ((Jy + 0.5f) / Ny) * Sz.Y);
				const FVector2D Tip = Local + Dir * 12.f;
				TArray<FVector2D> Shaft = { Local - Dir * 4.f, Tip };
				FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Shaft,
					ESlateDrawEffect::None, FLinearColor(1.f, 0.35f, 0.15f, 0.9f), true, 2.f);
			}
		}
		return;
	}

	const int32 Nx = 10;
	const int32 Ny = 7;
	const float BaseKn = FMath::Max(1.f, Wind->BaseSpeedKn);

	for (int32 Jy = 0; Jy < Ny; ++Jy)
	{
		for (int32 Jx = 0; Jx < Nx; ++Jx)
		{
			const float U = (Jx + 0.5f) / float(Nx);
			const float V = (Jy + 0.5f) / float(Ny);
			const FVector2D Local(U * Sz.X, V * Sz.Y);
			double Lat = 0, Lon = 0;
			LocalToLatLon(Local, Sz, Lat, Lon);
			const FWindSample S = Wind->SampleWindLatLon(Lat, Lon);

			const float ToDeg = S.DirFromDeg + 180.f;
			const float Rad = FMath::DegreesToRadians(ToDeg);
			const FVector2D Dir(FMath::Sin(Rad), -FMath::Cos(Rad));
			const float Len = FMath::Clamp(9.f + 11.f * (S.SpeedKn / 18.f), 8.f, 22.f);
			const FVector2D Tip = Local + Dir * Len;
			const FVector2D Tail = Local - Dir * (Len * 0.3f);

			const float G = FMath::Clamp((S.SpeedKn - BaseKn + 3.f) / 10.f, 0.f, 1.f);
			const float Shift = FMath::Clamp(FMath::Abs(S.DirShiftDeg) / 14.f, 0.f, 1.f);
			// High-contrast arrows for debug
			FLinearColor Col(
				0.15f + 0.75f * G + 0.15f * Shift,
				0.85f - 0.45f * G,
				0.25f + 0.2f * (1.f - G) + 0.4f * Shift,
				0.92f);

			TArray<FVector2D> Shaft;
			Shaft.Add(Tail);
			Shaft.Add(Tip);
			FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Shaft,
				ESlateDrawEffect::None, Col, true, 2.1f);

			const FVector2D N(-Dir.Y, Dir.X);
			const FVector2D H1 = Tip - Dir * (Len * 0.34f) + N * (Len * 0.24f);
			const FVector2D H2 = Tip - Dir * (Len * 0.34f) - N * (Len * 0.24f);
			TArray<FVector2D> Head = { H1, Tip, H2 };
			FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Head,
				ESlateDrawEffect::None, Col, true, 2.1f);
		}
	}

	for (const FWindPuff& P : Wind->GetPuffs())
	{
		double Lat = 0, Lon = 0;
		FNavGeo::WorldCmToLatLon(P.CenterCm.X, P.CenterCm.Y, Lat, Lon);
		const FVector2D C = LatLonToLocal(Lat, Lon, CLat, CLon, Zf, TileZ, Sz);
		if (C.X < -50.f || C.Y < -50.f || C.X > Sz.X + 50.f || C.Y > Sz.Y + 50.f) continue;
		const float Life = FMath::Clamp(P.AgeSec / FMath::Max(0.1f, P.LifeSec), 0.f, 1.f);
		const float Env = FMath::Sin(Life * PI);
		if (Env < 0.06f) continue;
		double Lat2 = 0, Lon2 = 0;
		FNavGeo::WorldCmToLatLon(P.CenterCm.X + P.MajorCm, P.CenterCm.Y, Lat2, Lon2);
		const FVector2D C2 = LatLonToLocal(Lat2, Lon2, CLat, CLon, Zf, TileZ, Sz);
		const float Rx = FMath::Clamp(FVector2D::Distance(C, C2), 8.f, 90.f);
		const float Ry = Rx * (P.MinorCm / FMath::Max(1.f, P.MajorCm));
		const int32 Segs = 16;
		TArray<FVector2D> Ring;
		Ring.Reserve(Segs + 1);
		const float ToRad = FMath::DegreesToRadians(Wind->BaseDirFromDeg + 180.f);
		const float Cs = FMath::Cos(ToRad), Sn = FMath::Sin(ToRad);
		for (int32 I = 0; I <= Segs; ++I)
		{
			const float A = (2.f * PI * I) / Segs;
			const float Lx = FMath::Cos(A) * Rx;
			const float Ly = FMath::Sin(A) * Ry;
			const float Wx = Lx * Cs - Ly * Sn;
			const float Wy = Lx * Sn + Ly * Cs;
			// World +X north,+Y east → screen +X east,+Y south
			Ring.Add(C + FVector2D(Wy, -Wx));
		}
		FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), Ring,
			ESlateDrawEffect::None, FLinearColor(1.f, 0.9f, 0.15f, 0.55f * Env), true, 1.6f);
	}
}

void SSailMiniMapPaint::DrawBoatMarker(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
	FVector2D Pos, float HdgDeg) const
{
	const float Rad = FMath::DegreesToRadians(HdgDeg);
	const float Len = 13.f;
	const FVector2D Nose = Pos + FVector2D(FMath::Sin(Rad), -FMath::Cos(Rad)) * Len;
	const FVector2D Port = Pos + FVector2D(FMath::Sin(Rad + 2.35f), -FMath::Cos(Rad + 2.35f)) * (Len * 0.55f);
	const FVector2D Stbd = Pos + FVector2D(FMath::Sin(Rad - 2.35f), -FMath::Cos(Rad - 2.35f)) * (Len * 0.55f);

	TArray<FVector2D> Halo = { Nose, Port, Stbd, Nose };
	FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Halo,
		ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.5f), true, 4.5f);
	TArray<FVector2D> Hull = { Nose, Port, Stbd, Nose };
	FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Hull,
		ESlateDrawEffect::None, FLinearColor::FromSRGBColor(FColor(0x00, 0xd0, 0xe8)), true, 2.2f);
}

FReply SSailMiniMapPaint::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// web: step ~0.0015 * deltaY; trackpad larger. UE wheel delta is typically ±1 per notch.
	const float Wheel = MouseEvent.GetWheelDelta();
	// Smooth but responsive: ~0.12 level per notch (was 0.35 — too jumpy)
	const float Delta = Wheel * 0.12f;
	if (FMath::Abs(Delta) < 1e-4f) return FReply::Handled();

	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D Anchor = bFollow ? (MyGeometry.GetLocalSize() * 0.5f) : Local;
	ZoomAt(Delta, MyGeometry, Anchor);
	return FReply::Handled();
}

FReply SSailMiniMapPaint::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		const FVector2D Sz = MyGeometry.GetLocalSize();

		// Plot tools (web _handlePlotClick) — add/delete/select without starting a pan.
		if (PlotMode == ENavPlotMode::Add)
		{
			if (UNavWaypointSubsystem* Nav = GetWaypoints())
			{
				// Ensure view center is valid for conversion
				if (bFollow || !bViewInit)
				{
					ViewLat = BoatLat.Get(FNavGeo::BoatStartLat);
					ViewLon = BoatLon.Get(FNavGeo::BoatStartLon);
					bViewInit = true;
				}
				double Lat = 0, Lon = 0;
				LocalToLatLon(Local, Sz, Lat, Lon);
				Nav->AddWaypoint(Lat, Lon);
			}
			return FReply::Handled();
		}
		if (PlotMode == ENavPlotMode::Delete)
		{
			const int32 Hit = HitTestWaypoint(Local, Sz, 16.f);
			if (Hit != INDEX_NONE)
			{
				if (UNavWaypointSubsystem* Nav = GetWaypoints())
				{
					Nav->RemoveAt(Hit);
				}
			}
			return FReply::Handled();
		}

		// Pan mode: start drag; click without move selects WP on mouse-up.
		bPanning = true;
		bPanMoved = false;
		LastMouseAbs = MouseEvent.GetScreenSpacePosition();
		PanStartAbs = LastMouseAbs;
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	return FReply::Unhandled();
}

FReply SSailMiniMapPaint::OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent)
{
	// web: dblclick re-enables follow
	bFollow = true;
	bPanning = false;
	return FReply::Handled();
}

FReply SSailMiniMapPaint::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && bPanning)
	{
		// Click (no pan) selects nearest waypoint (web pan-mode plot click).
		if (!bPanMoved && PlotMode == ENavPlotMode::Pan)
		{
			const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
			const int32 Hit = HitTestWaypoint(Local, MyGeometry.GetLocalSize(), 16.f);
			if (Hit != INDEX_NONE)
			{
				if (UNavWaypointSubsystem* Nav = GetWaypoints())
				{
					Nav->SetSelectedIndex(Hit);
				}
			}
		}
		bPanning = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FCursorReply SSailMiniMapPaint::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	if (PlotMode == ENavPlotMode::Add) return FCursorReply::Cursor(EMouseCursor::Crosshairs);
	if (PlotMode == ENavPlotMode::Delete) return FCursorReply::Cursor(EMouseCursor::Hand);
	if (bPanning && bPanMoved) return FCursorReply::Cursor(EMouseCursor::GrabHandClosed);
	return FCursorReply::Cursor(EMouseCursor::Default);
}

FReply SSailMiniMapPaint::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bPanning) return FReply::Unhandled();

	const FVector2D Now = MouseEvent.GetScreenSpacePosition();
	const FVector2D FromStart = Now - PanStartAbs;
	if (!bPanMoved && FromStart.SizeSquared() >= PanBreakPx * PanBreakPx)
	{
		bPanMoved = true;
		bFollow = false; // break boat track only after real pan (web)
		const double BLat = BoatLat.Get(FNavGeo::BoatStartLat);
		const double BLon = BoatLon.Get(FNavGeo::BoatStartLon);
		ViewLat = BLat;
		ViewLon = BLon;
	}

	if (!bPanMoved)
	{
		LastMouseAbs = Now;
		return FReply::Handled();
	}

	const FVector2D Delta = Now - LastMouseAbs;
	LastMouseAbs = Now;

	const float Zf = FMath::Clamp(UserZoom, 9.f, 16.f);
	const int32 TileZ = ResolveTileZ();
	const double Scale = FMath::Pow(2.0, double(Zf) - double(TileZ));

	double Cx = 0.0, Cy = 0.0;
	FNavGeo::LonLatToWorldPx(ViewLon, ViewLat, TileZ, Cx, Cy);
	Cx -= double(Delta.X) / Scale;
	Cy -= double(Delta.Y) / Scale;
	FNavGeo::WorldPxToLonLat(Cx, Cy, TileZ, ViewLon, ViewLat);
	return FReply::Handled();
}
