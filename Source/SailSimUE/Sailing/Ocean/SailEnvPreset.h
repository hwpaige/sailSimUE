#pragma once

#include "CoreMinimal.h"

/** Named sky / ocean / light looks for the open-ocean map. */
enum class ESailEnvPreset : uint8
{
	/** Captured polished daytime look (map baseline). */
	FairDay = 0,
	GoldenHour,
	Dusk,
	Night,
	Overcast,
	Storm,
	FogBank,
	Count
};

/**
 * Preset knobs. Sun uses *elevation* (deg above horizon).
 *
 * UE directional light shines along component +X. That is the light travel
 * direction (from sun toward the scene), so:
 *   Pitch = -Elevation
 *   elev +55° (noon) → Pitch -55 (light points down into the world)
 *   elev 0° (horizon) → Pitch 0
 *   elev -28° (night) → Pitch +28 (light from below horizon)
 * ApplySunAndSky converts elevation → pitch. Do not store UE pitch here.
 *
 * Intensity muls are relative to the captured Fair Day baseline.
 */
struct FSailEnvPresetDesc
{
	const TCHAR* Name = TEXT("Fair Day");
	const TCHAR* Blurb = TEXT("");

	/**
	 * Sun height above horizon (deg). 90 = zenith, 0 = on horizon, negative = below.
	 * Converted to UE pitch via Pitch = -Elevation in ApplySunAndSky.
	 */
	float SunElevationDeg = 50.f;
	/** Added to captured Fair Day sun yaw (keeps azimuth relative to map). */
	float SunYawOffsetDeg = 0.f;

	/** Multiplier on captured fair-day sun intensity. */
	float SunIntensityMul = 1.f;
	/** Linear sun color (Fair Day restores capture). */
	FLinearColor SunColor = FLinearColor(1.f, 0.98f, 0.94f);
	/** Multiplier on directional light source angle (soft disk at dusk / fog). */
	float SunSourceAngleMul = 1.f;

	float SkyLightIntensityMul = 1.f;
	/** Sky light tint (white for day; cool for night; gray for overcast). */
	FLinearColor SkyLightColor = FLinearColor::White;

	/** 0..1 volumetric cloud layer thickness (scales layer height). */
	float CloudIntensity = 1.f;
	/**
	 * Delta on cloud layer bottom altitude (km). Negative = lower deck
	 * (storm / fog). 0 = leave baseline.
	 */
	float CloudBottomOffsetKm = 0.f;

	/** 0..1 fog slider target (scales captured fog density). */
	float FogIntensity = 1.f;
	/** Extra density scale on top of baseline (storm / fog bank). */
	float FogDensityMul = 1.f;
	FLinearColor FogInscattering = FLinearColor(0.45f, 0.55f, 0.70f);
	bool bOverrideFogColor = false;
	/** Height falloff; < 0 keeps / restores baseline. */
	float FogHeightFalloff = -1.f;
	/** Fog start distance (cm); < 0 keeps baseline. */
	float FogStartDistanceCm = -1.f;

	/** Water surface chop 0..1. */
	float SurfaceChop = 0.08f;

	/** Sky atmosphere multi-scattering factor relative to baseline. */
	float AtmosphereMultiScatter = 1.f;
	/** Rayleigh scale relative to baseline. */
	float AtmosphereRayleighMul = 1.f;
	/** Mie scattering scale — haze / storm. */
	float AtmosphereMieMul = 1.f;
	/** Sky luminance RGB scale (dims night / grays overcast). */
	FLinearColor SkyLuminance = FLinearColor::White;
	/** Height-fog contribution from atmosphere (0..2-ish). < 0 keeps baseline. */
	float HeightFogContribution = -1.f;

	/** If >= 0, suggest true-wind kn. -1 = leave wind alone. */
	float SuggestedWindKn = -1.f;
};

inline const FSailEnvPresetDesc& GetSailEnvPresetDesc(ESailEnvPreset Id)
{
	// Fair Day elev 50° is the noon default when map capture is horizon-flat.
	// Non-fair / TOD presets use absolute elevation; ApplySunAndSky uses Pitch = -Elev.
	static const FSailEnvPresetDesc Table[] = {
		// Fair Day — capture restore only
		{
			TEXT("Fair Day"), TEXT("Clear polished day — your saved look"),
			/*elev*/ 50.f, /*yawOff*/ 0.f,
			/*sunI*/ 1.f, FLinearColor(1.f, 0.98f, 0.94f), /*srcAng*/ 1.f,
			/*sky*/ 1.f, FLinearColor::White,
			/*cloud*/ 1.f, /*cloudBot*/ 0.f,
			/*fog*/ 1.f, /*fogDens*/ 1.f,
			FLinearColor(0.45f, 0.55f, 0.70f), false,
			/*falloff*/ -1.f, /*startCm*/ -1.f,
			/*chop*/ 0.08f,
			/*atm*/ 1.f, 1.f, 1.f, FLinearColor::White, -1.f,
			/*wind*/ -1.f
		},
		// Golden hour — same family as Fair Day: soft late-day warmth, no cartoon orange.
		// Elevation is relative in time-of-day (built from capture); table values are fallbacks.
		{
			TEXT("Golden Hour"), TEXT("Soft late-day warmth"),
			8.f, 8.f,
			0.92f, FLinearColor(1.f, 0.94f, 0.86f), 1.15f,
			0.95f, FLinearColor(1.f, 0.98f, 0.96f),
			0.9f, 0.4f,
			1.0f, 1.05f,
			FLinearColor(0.45f, 0.55f, 0.70f), false, // keep Fair fog color family
			-1.f, -1.f,
			0.09f,
			1.05f, 1.02f, 1.12f, FLinearColor(1.f, 0.99f, 0.97f), -1.f,
			-1.f
		},
		// Dusk — dimmer Fair Day, cool neutral sky (not purple), soft warm disk.
		{
			TEXT("Dusk"), TEXT("Sun near the horizon"),
			2.5f, 14.f,
			0.42f, FLinearColor(1.f, 0.90f, 0.80f), 1.6f,
			0.55f, FLinearColor(0.94f, 0.95f, 0.98f),
			0.7f, 0.2f,
			0.95f, 1.08f,
			FLinearColor(0.40f, 0.48f, 0.58f), false, // muted blue-gray, not violet
			-1.f, 200.f,
			0.10f,
			0.95f, 0.98f, 1.2f, FLinearColor(0.92f, 0.93f, 0.96f), 1.05f,
			11.f
		},
		// Night — sun fully off; moon + stars in ApplyNightSky. Keep ambient tiny
		// so sky luminance doesn't wash the star field.
		{
			TEXT("Night"), TEXT("Moonlit open water, starry sky"),
			-28.f, -55.f,
			0.f, FLinearColor(0.05f, 0.06f, 0.1f), 1.f, // sun intensity mul = 0
			0.035f, FLinearColor(0.35f, 0.42f, 0.75f), // very low skylight
			0.08f, 0.f, // almost no clouds
			0.2f, 0.45f,
			FLinearColor(0.008f, 0.01f, 0.02f), true,
			0.1f, 100.f,
			0.06f,
			// multi / rayleigh / mie / skyLum / heightFog
			0.05f, 0.25f, 0.15f, FLinearColor(0.012f, 0.015f, 0.03f), 0.12f,
			8.f
		},
		// Overcast — high washed-out sun under a solid deck, flat gray light
		{
			TEXT("Overcast"), TEXT("Flat gray light under solid cloud"),
			55.f, 5.f,
			0.18f, FLinearColor(0.82f, 0.85f, 0.9f), 3.5f,
			0.85f, FLinearColor(0.78f, 0.8f, 0.85f),
			1.0f, -1.2f, // full thick lower deck
			0.6f, 1.25f,
			FLinearColor(0.55f, 0.57f, 0.62f), true,
			0.18f, 1200.f,
			0.22f,
			1.35f, 1.15f, 1.9f, FLinearColor(0.7f, 0.72f, 0.78f), 1.2f,
			14.f
		},
		// Storm — dim green-gray light, heavy low cloud + fog, big chop
		{
			TEXT("Storm"), TEXT("Heavy cloud, wind, and chop"),
			38.f, -12.f,
			0.08f, FLinearColor(0.55f, 0.6f, 0.65f), 4.f,
			0.55f, FLinearColor(0.65f, 0.68f, 0.72f),
			1.0f, -2.5f, // low angry deck
			0.92f, 1.7f,
			FLinearColor(0.28f, 0.3f, 0.34f), true,
			0.1f, 300.f,
			0.95f,
			1.5f, 1.2f, 2.4f, FLinearColor(0.45f, 0.48f, 0.52f), 1.3f,
			32.f
		},
		// Fog bank — soft pale sun disc in thick marine layer
		{
			TEXT("Fog Bank"), TEXT("Thick marine layer, muted sun"),
			28.f, 8.f,
			0.28f, FLinearColor(0.9f, 0.92f, 0.95f), 5.5f,
			0.6f, FLinearColor(0.88f, 0.9f, 0.93f),
			0.45f, -0.8f,
			1.0f, 3.2f,
			FLinearColor(0.7f, 0.74f, 0.78f), true,
			0.4f, 80.f, // hugs surface, starts immediately
			0.14f,
			1.15f, 1.05f, 2.2f, FLinearColor(0.85f, 0.88f, 0.9f), 1.4f,
			6.f
		},
	};
	static_assert(UE_ARRAY_COUNT(Table) == static_cast<int32>(ESailEnvPreset::Count), "preset table size");
	const int32 I = FMath::Clamp(static_cast<int32>(Id), 0, static_cast<int32>(ESailEnvPreset::Count) - 1);
	return Table[I];
}
