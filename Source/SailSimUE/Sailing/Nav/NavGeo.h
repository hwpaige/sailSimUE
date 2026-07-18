#pragma once

#include "CoreMinimal.h"

/**
 * Navigation geography — matches sail-sim frontend/public/nav-geo.js
 *
 * Chart 13237 "Nantucket Sound and Approaches"
 * Sim axes (web): +X = North, +Z = East, feet.
 * UE boat world: +X = North (heading 0°), +Y = East, centimeters.
 */
struct FNavGeo
{
	static constexpr double OriginLat = 41.4800;
	static constexpr double OriginLon = -70.2200;
	/** Nantucket Harbor inner basin — matches web NAV_BOAT_START (chart 13237). */
	static constexpr double BoatStartLat = 41.2850;
	static constexpr double BoatStartLon = -70.0900;
	/** Harbor dock heading (web NAV_BOAT_START.heading). */
	static constexpr float BoatStartHeadingDeg = 90.f;
	static constexpr double FtPerDegLat = 364000.0;
	static constexpr double CmPerFt = 30.48;

	static double FtPerDegLon(double LatDeg)
	{
		return FtPerDegLat * FMath::Cos(FMath::DegreesToRadians(LatDeg));
	}

	/** UE world cm (+X north, +Y east) → lat/lon. World origin = NAV_ORIGIN. */
	static void WorldCmToLatLon(double XCm, double YCm, double& OutLat, double& OutLon)
	{
		const double XFt = XCm / CmPerFt;
		const double ZFt = YCm / CmPerFt; // UE +Y → web +Z east
		OutLat = OriginLat + XFt / FtPerDegLat;
		OutLon = OriginLon + ZFt / FtPerDegLon(OriginLat);
	}

	static void LatLonToWorldCm(double Lat, double Lon, double& OutXCm, double& OutYCm)
	{
		const double XFt = (Lat - OriginLat) * FtPerDegLat;
		const double ZFt = (Lon - OriginLon) * FtPerDegLon(OriginLat);
		OutXCm = XFt * CmPerFt;
		OutYCm = ZFt * CmPerFt;
	}

	/** UE world cm of Nantucket Harbor spawn (+X north, +Y east). */
	static void BoatStartWorldCm(double& OutXCm, double& OutYCm)
	{
		LatLonToWorldCm(BoatStartLat, BoatStartLon, OutXCm, OutYCm);
	}

	static FVector2D BoatStartWorldCm2D()
	{
		double X = 0.0, Y = 0.0;
		BoatStartWorldCm(X, Y);
		return FVector2D(static_cast<float>(X), static_cast<float>(Y));
	}

	static FString FormatLat(double Lat)
	{
		const TCHAR Hemi = Lat >= 0.0 ? TEXT('N') : TEXT('S');
		const double Abs = FMath::Abs(Lat);
		const int32 Deg = int32(Abs);
		const double Min = (Abs - Deg) * 60.0;
		return FString::Printf(TEXT("%d°%.3f'%c"), Deg, Min, Hemi);
	}

	static FString FormatLon(double Lon)
	{
		const TCHAR Hemi = Lon >= 0.0 ? TEXT('E') : TEXT('W');
		const double Abs = FMath::Abs(Lon);
		const int32 Deg = int32(Abs);
		const double Min = (Abs - Deg) * 60.0;
		return FString::Printf(TEXT("%d°%.3f'%c"), Deg, Min, Hemi);
	}

	// --- Web Mercator (EPSG:3857 style, for XYZ tiles) ---

	static constexpr int32 TileSize = 256;

	static double MercX(double Lon)
	{
		return (Lon + 180.0) / 360.0;
	}

	static double MercY(double Lat)
	{
		const double R = Lat * PI / 180.0;
		const double T = FMath::Tan(R) + 1.0 / FMath::Cos(R);
		return (1.0 - FMath::Loge(T) / PI) / 2.0;
	}

	static void LonLatToWorldPx(double Lon, double Lat, double Zoom, double& OutX, double& OutY)
	{
		const double S = FMath::Pow(2.0, Zoom) * TileSize;
		OutX = MercX(Lon) * S;
		OutY = MercY(Lat) * S;
	}

	static void WorldPxToLonLat(double X, double Y, double Zoom, double& OutLon, double& OutLat)
	{
		const double S = FMath::Pow(2.0, Zoom) * TileSize;
		OutLon = (X / S) * 360.0 - 180.0;
		const double N = PI - 2.0 * PI * (Y / S);
		OutLat = (180.0 / PI) * FMath::Atan(0.5 * (FMath::Exp(N) - FMath::Exp(-N)));
	}

	// --- Route metrics (web navDistNm / navBearingDeg on flat chart) ---
	static constexpr double FtPerNm = 6076.1154855643;

	/** Rhumb distance (nm) on the sim flat chart projection. */
	static double DistNm(double Lat1, double Lon1, double Lat2, double Lon2)
	{
		double X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
		LatLonToWorldCm(Lat1, Lon1, X1, Y1);
		LatLonToWorldCm(Lat2, Lon2, X2, Y2);
		const double DxFt = (X2 - X1) / CmPerFt;
		const double DzFt = (Y2 - Y1) / CmPerFt;
		return FMath::Sqrt(DxFt * DxFt + DzFt * DzFt) / FtPerNm;
	}

	/**
	 * True bearing (deg) from 1 → 2 on the sim chart.
	 * 0° = north (+X), 90° = east (+Y) — matches boat heading.
	 */
	static double BearingDeg(double Lat1, double Lon1, double Lat2, double Lon2)
	{
		double X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
		LatLonToWorldCm(Lat1, Lon1, X1, Y1);
		LatLonToWorldCm(Lat2, Lon2, X2, Y2);
		const double Dx = (X2 - X1) / CmPerFt;
		const double Dz = (Y2 - Y1) / CmPerFt;
		if (FMath::Abs(Dx) < 1e-9 && FMath::Abs(Dz) < 1e-9) return 0.0;
		double Brg = FMath::RadiansToDegrees(FMath::Atan2(Dz, Dx));
		if (Brg < 0.0) Brg += 360.0;
		return Brg;
	}

	static FString FormatNm(double Nm)
	{
		if (!FMath::IsFinite(Nm)) return TEXT("—");
		if (Nm < 0.1) return FString::Printf(TEXT("%.0f m"), Nm * 1852.0);
		if (Nm < 10.0) return FString::Printf(TEXT("%.2f nm"), Nm);
		return FString::Printf(TEXT("%.1f nm"), Nm);
	}

	static FString FormatBrg(double Deg)
	{
		if (!FMath::IsFinite(Deg)) return TEXT("—");
		const int32 D = ((int32)FMath::RoundToInt(Deg) % 360 + 360) % 360;
		return FString::Printf(TEXT("%03d°"), D);
	}

	/** Duration hours → H:MM or MM:SS (web navFormatDuration / TTG). */
	static FString FormatDurationH(double Hours)
	{
		if (!FMath::IsFinite(Hours) || Hours < 0.0) return TEXT("—");
		if (Hours > 48.0) return FString::Printf(TEXT("%dd"), (int32)FMath::RoundToInt(Hours / 24.0));
		const int32 TotalMin = (int32)FMath::RoundToInt(Hours * 60.0);
		if (TotalMin < 60)
		{
			const int32 S = (int32)FMath::RoundToInt(FMath::Fmod(Hours * 3600.0, 60.0));
			return FString::Printf(TEXT("%02d:%02d"), TotalMin, S);
		}
		return FString::Printf(TEXT("%d:%02d"), TotalMin / 60, TotalMin % 60);
	}

	/** Local clock ETA from hours-from-now (web navFormatEta). */
	static FString FormatEta(double HoursFromNow)
	{
		if (!FMath::IsFinite(HoursFromNow) || HoursFromNow < 0.0) return TEXT("—");
		const FDateTime T = FDateTime::Now() + FTimespan::FromHours(HoursFromNow);
		return FString::Printf(TEXT("%02d:%02d"), T.GetHour(), T.GetMinute());
	}
};
