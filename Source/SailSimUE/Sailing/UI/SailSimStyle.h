#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Fonts/SlateFontInfo.h"

/**
 * Visual tokens matching sail-sim frontend/src/styles.scss
 * (dark glass chrome, cyan accent, port/stbd helm colors).
 */
struct FSailSimStyle
{
	// --bg / --panel
	static inline const FLinearColor Bg0 = FLinearColor::FromSRGBColor(FColor(0x14, 0x14, 0x16));
	static inline const FLinearColor Panel = FLinearColor(0.157f, 0.157f, 0.173f, 0.78f); // #28282c @ 78%
	static inline const FLinearColor PanelSolid = FLinearColor::FromSRGBColor(FColor(0x1c, 0x1c, 0x1f));
	static inline const FLinearColor PanelDeep = FLinearColor(0.05f, 0.05f, 0.06f, 0.88f);
	static inline const FLinearColor Border = FLinearColor(1.f, 1.f, 1.f, 0.10f);
	static inline const FLinearColor BorderHi = FLinearColor(1.f, 1.f, 1.f, 0.16f);

	// --accent #4dd0e1
	static inline const FLinearColor Accent = FLinearColor::FromSRGBColor(FColor(0x4d, 0xd0, 0xe1));
	static inline const FLinearColor AccentDim = FLinearColor(0.302f, 0.816f, 0.882f, 0.16f);
	static inline const FLinearColor AccentGlow = FLinearColor(0.302f, 0.816f, 0.882f, 0.35f);

	// --text / --text-dim
	static inline const FLinearColor Text = FLinearColor::FromSRGBColor(FColor(0xe3, 0xea, 0xf2));
	static inline const FLinearColor TextDim = FLinearColor::FromSRGBColor(FColor(0x9a, 0x9a, 0x9f));
	static inline const FLinearColor TextMute = FLinearColor(0.55f, 0.55f, 0.58f, 0.9f);

	// --helm-port / --helm-stbd
	static inline const FLinearColor Port = FLinearColor::FromSRGBColor(FColor(0xe0, 0x6b, 0x6b));
	static inline const FLinearColor Stbd = FLinearColor::FromSRGBColor(FColor(0x6b, 0xdc, 0x8a));
	static inline const FLinearColor Warn = FLinearColor::FromSRGBColor(FColor(0xff, 0xd2, 0x4d));
	// Autopilot mode accents (web dist styles.css)
	static inline const FLinearColor ApAwa = FLinearColor::FromSRGBColor(FColor(0xff, 0x9f, 0x5a));
	static inline const FLinearColor ApNav = FLinearColor::FromSRGBColor(FColor(0x6b, 0xdc, 0x8a));
	static inline const FLinearColor ApStatLbl = FLinearColor::FromSRGBColor(FColor(0x6b, 0x71, 0x78));
	static inline const FLinearColor ApStatVal = FLinearColor::FromSRGBColor(FColor(0xcf, 0xe8, 0xec));
	static inline const FLinearColor ApHeader = FLinearColor::FromSRGBColor(FColor(0x9a, 0x9a, 0x9f));
	static inline const FLinearColor TrimIdle = FLinearColor::FromSRGBColor(FColor(0x45, 0x40, 0x3a));
	static inline const FLinearColor TrimActive = FLinearColor::FromSRGBColor(FColor(0xc4, 0x7a, 0x2e));
	static inline const FLinearColor Water = FLinearColor(0.035f, 0.09f, 0.13f, 0.95f);
	static inline const FLinearColor Shadow = FLinearColor(0.f, 0.f, 0.f, 0.40f);

	// DC breaker panel (boat electrical)
	static inline const FLinearColor BreakerPanel = FLinearColor::FromSRGBColor(FColor(0x1a, 0x1c, 0x1e));
	static inline const FLinearColor BreakerFace = FLinearColor::FromSRGBColor(FColor(0x2a, 0x2e, 0x32));
	static inline const FLinearColor BreakerLabel = FLinearColor::FromSRGBColor(FColor(0xe8, 0xd4, 0x4a));
	static inline const FLinearColor BreakerOn = FLinearColor::FromSRGBColor(FColor(0x3d, 0xc4, 0x5a));
	static inline const FLinearColor BreakerOff = FLinearColor::FromSRGBColor(FColor(0x55, 0x58, 0x5c));
	static inline const FLinearColor BreakerPilot = FLinearColor::FromSRGBColor(FColor(0xff, 0x3b, 0x30));

	static constexpr float RadiusCard = 16.f;
	static constexpr float RadiusPill = 10.f;
	/** Match glass/trim cards (MakeGlassCard / helm uses RadiusCard). */
	static constexpr float RadiusMap = 16.f;
	/**
	 * Chart is full-bleed under the glass card so tiles fill to the rounded
	 * edges and get clipped (no inset gutter that shrinks the map).
	 */
	static constexpr float MapCardPad = 0.f;
	/** Inner chart well radius matches outer card (full-bleed + clip). */
	static constexpr float RadiusMapInner = 16.f;
	static constexpr float RadiusGauge = 999.f; // fully round
	static constexpr float GaugeSize = 128.f; // web --gauge-size
	static constexpr float GaugeGap = 8.f;

	static FSlateFontInfo FontBrand()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 20);
	}
	static FSlateFontInfo FontTitle()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 13);
	}
	static FSlateFontInfo FontLabel()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 10);
	}
	static FSlateFontInfo FontBody()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 12);
	}
	static FSlateFontInfo FontMono()
	{
		// Instrument readouts — bold, slightly larger
		return FCoreStyle::GetDefaultFontStyle("Bold", 14);
	}
	static FSlateFontInfo FontMonoLg()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 22);
	}
	static FSlateFontInfo FontMonoSm()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 11);
	}
	static FSlateFontInfo FontSection()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 9);
	}

	/** Frosted glass card. */
	static const FSlateBrush* CardBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			Panel,
			RadiusCard,
			Border,
			1.0f);
		return &Brush;
	}

	static const FSlateBrush* BreakerPanelBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			BreakerPanel,
			8.f,
			FLinearColor(0.08f, 0.08f, 0.09f, 1.f),
			2.0f);
		return &Brush;
	}

	static const FSlateBrush* BreakerSlotBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			BreakerFace,
			4.f,
			FLinearColor(0.12f, 0.12f, 0.13f, 1.f),
			1.0f);
		return &Brush;
	}

	static const FSlateBrush* BreakerToggleOn()
	{
		static const FSlateRoundedBoxBrush Brush(BreakerOn, 3.f);
		return &Brush;
	}

	static const FSlateBrush* BreakerToggleOff()
	{
		static const FSlateRoundedBoxBrush Brush(BreakerOff, 3.f);
		return &Brush;
	}

	static const FSlateBrush* BreakerPilotOn()
	{
		static const FSlateRoundedBoxBrush Brush(BreakerPilot, 99.f);
		return &Brush;
	}

	static const FSlateBrush* BreakerPilotOff()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.15f, 0.05f, 0.05f, 1.f), 99.f);
		return &Brush;
	}

	static const FSlateBrush* CardBrushDeep()
	{
		static const FSlateRoundedBoxBrush Brush(
			PanelDeep,
			RadiusCard,
			Border,
			1.0f);
		return &Brush;
	}

	/** Pill / chip background. */
	static const FSlateBrush* PillBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.f, 0.f, 0.f, 0.40f),
			RadiusPill,
			Border,
			1.0f);
		return &Brush;
	}

	static const FSlateBrush* PillBrushAccent()
	{
		static const FSlateRoundedBoxBrush Brush(
			AccentDim,
			RadiusPill,
			FLinearColor(Accent.R, Accent.G, Accent.B, 0.55f),
			1.25f);
		return &Brush;
	}

	static const FSlateBrush* ButtonBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.f, 0.f, 0.f, 0.38f),
			8.f,
			Border,
			1.0f);
		return &Brush;
	}

	static const FSlateBrush* ButtonBrushActive()
	{
		static const FSlateRoundedBoxBrush Brush(
			AccentGlow,
			8.f,
			Accent,
			1.5f);
		return &Brush;
	}

	/**
	 * Same glass as metrics card / chart chrome (CardBrush fill + border),
	 * circular for instruments.
	 */
	static const FSlateBrush* GaugeFaceBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			Panel,       // same as CardBrush / metrics panel
			RadiusGauge,
			Border,      // same 1px rim as cards & map
			1.0f);
		return &Brush;
	}

	static const FSlateBrush* GaugeShadowBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			Shadow,
			RadiusGauge);
		return &Brush;
	}

	static const FSlateBrush* GaugeHubBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			Accent,
			RadiusGauge);
		return &Brush;
	}

	static const FSlateBrush* HelmTrackPort()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(Port.R, Port.G, Port.B, 0.55f),
			4.f);
		return &Brush;
	}

	static const FSlateBrush* HelmTrackStbd()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(Stbd.R, Stbd.G, Stbd.B, 0.55f),
			4.f);
		return &Brush;
	}

	static const FSlateBrush* WaterBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			Water,
			12.f,
			Border,
			1.f);
		return &Brush;
	}

	// Chart text on the map surface (coords / place)
	static inline const FLinearColor MapOverlayText = FLinearColor::White;
	static inline const FLinearColor MapOverlayShadow = FLinearColor(0.f, 0.f, 0.f, 0.85f);
	// Metrics header above chart (dark bar)
	static inline const FLinearColor MapHeaderText = FLinearColor::FromSRGBColor(FColor(0xcf, 0xe8, 0xec));

	/** Inner chart well (sits inside CardBrush pad). */
	static const FSlateBrush* MapPanelBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.686f, 0.804f, 0.882f, 0.22f),
			RadiusMapInner,
			BorderHi,
			1.f);
		return &Brush;
	}

	/** Outline-only frame over chart tiles (inner well radius). */
	static const FSlateBrush* MapPanelFrameBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.f, 0.f, 0.f, 0.f),
			RadiusMapInner,
			BorderHi,
			1.f);
		return &Brush;
	}

	/** Solid metrics strip above the chart body. */
	static const FSlateBrush* MapHeaderBarBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.063f, 0.067f, 0.078f, 0.92f), // rgba(16,17,20,~0.92)
			FVector4(RadiusMap, RadiusMap, 0.f, 0.f));
		return &Brush;
	}

	static const FSlateBrush* MapHeaderBrush()
	{
		return MapHeaderBarBrush();
	}

	static const FSlateBrush* MapFooterBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.063f, 0.067f, 0.078f, 0.60f),
			FVector4(0.f, 0.f, RadiusMap, RadiusMap));
		return &Brush;
	}

	/** web .nav-minimap-pill */
	static const FSlateBrush* MapTelemetryPillBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.f, 0.f, 0.f, 0.35f),
			3.f,
			FLinearColor(1.f, 1.f, 1.f, 0.10f),
			1.f);
		return &Brush;
	}

	/** Mini-map overlay text — same as AP HDG/AWA values. */
	static FSlateFontInfo FontMapOverlay()
	{
		return FontApStatVal();
	}

	/** Telemetry pills — same as AP HDG/AWA values. */
	static FSlateFontInfo FontMapPill()
	{
		return FontApStatVal();
	}

	static FSlateFontInfo FontMapTiny()
	{
		return FontApStatVal();
	}

	static const FSlateBrush* DrawerBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.09f, 0.09f, 0.10f, 0.94f),
			FVector4(16.f, 0.f, 0.f, 16.f), // left corners only when docked right — full round ok
			FLinearColor(Accent.R, Accent.G, Accent.B, 0.35f),
			1.25f);
		return &Brush;
	}

	static const FSlateBrush* SliderFill()
	{
		static const FSlateRoundedBoxBrush Brush(Accent, 3.f);
		return &Brush;
	}

	static const FSlateBrush* SliderTrack()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(1.f, 1.f, 1.f, 0.08f), 3.f);
		return &Brush;
	}

	static const FSlateBrush* Transparent()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor(0.f, 0.f, 0.f, 0.f), 0.f);
		return &Brush;
	}

	// ---- Autopilot panel (web .autopilot-panel dist: 128px, rgba(12,13,15,0.78)) ----
	static constexpr float ApPanelW = 128.f;
	static constexpr float ApRadius = 10.f;

	static const FSlateBrush* ApPanelBrush()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.047f, 0.051f, 0.059f, 0.78f), // rgba(12,13,15,0.78)
			ApRadius,
			FLinearColor(1.f, 1.f, 1.f, 0.10f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApPanelEngagedHdg()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.047f, 0.051f, 0.059f, 0.78f),
			ApRadius,
			FLinearColor(Accent.R, Accent.G, Accent.B, 0.35f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApPanelEngagedAwa()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.047f, 0.051f, 0.059f, 0.78f),
			ApRadius,
			FLinearColor(ApAwa.R, ApAwa.G, ApAwa.B, 0.40f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApPanelEngagedNav()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.047f, 0.051f, 0.059f, 0.78f),
			ApRadius,
			FLinearColor(ApNav.R, ApNav.G, ApNav.B, 0.40f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApTabIdle()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(0.118f, 0.125f, 0.141f, 0.85f), // rgba(30,32,36,0.85)
			5.f,
			FLinearColor(1.f, 1.f, 1.f, 0.10f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApTabHdgActive()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(Accent.R, Accent.G, Accent.B, 0.28f),
			5.f,
			FLinearColor(Accent.R, Accent.G, Accent.B, 0.45f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApTabAwaActive()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(ApAwa.R, ApAwa.G, ApAwa.B, 0.28f),
			5.f,
			FLinearColor(ApAwa.R, ApAwa.G, ApAwa.B, 0.45f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApTabNavActive()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor(ApNav.R, ApNav.G, ApNav.B, 0.28f),
			5.f,
			FLinearColor(ApNav.R, ApNav.G, ApNav.B, 0.45f),
			1.f);
		return &Brush;
	}

	static const FSlateBrush* ApKeyPort()
	{
		static const FSlateRoundedBoxBrush Brush(Port, 5.f);
		return &Brush;
	}

	static const FSlateBrush* ApKeyStbd()
	{
		static const FSlateRoundedBoxBrush Brush(Stbd, 5.f);
		return &Brush;
	}

	static const FSlateBrush* ApModeTack()
	{
		static const FSlateRoundedBoxBrush Brush(Stbd, 5.f);
		return &Brush;
	}

	/** Web .ap-mode--auto idle (#4a5058). */
	static const FSlateBrush* ApModeAutoIdle()
	{
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor::FromSRGBColor(FColor(0x4a, 0x50, 0x58)), 5.f);
		return &Brush;
	}

	/** Web .ap-mode--auto.is-active (accent). */
	static const FSlateBrush* ApModeAutoActive()
	{
		static const FSlateRoundedBoxBrush Brush(Accent, 5.f);
		return &Brush;
	}

	static const FSlateBrush* ApModeTrimIdle()
	{
		static const FSlateRoundedBoxBrush Brush(TrimIdle, 5.f);
		return &Brush;
	}

	static const FSlateBrush* ApModeTrimActive()
	{
		static const FSlateRoundedBoxBrush Brush(TrimActive, 5.f);
		return &Brush;
	}

	static const FSlateBrush* ApSliderTrackHdg()
	{
		// Approximate gradient #6b7178 → #4dd0e1 with solid mid blend
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor::FromSRGBColor(FColor(0x5c, 0x9f, 0xac)),
			3.f);
		return &Brush;
	}

	static const FSlateBrush* ApSliderTrackAwa()
	{
		// port → grey → stbd blend
		static const FSlateRoundedBoxBrush Brush(
			FLinearColor::FromSRGBColor(FColor(0x8a, 0x8a, 0x8e)),
			3.f);
		return &Brush;
	}

	static FSlateFontInfo FontApHeader()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 10);
	}
	static FSlateFontInfo FontApTab()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 10);
	}
	static FSlateFontInfo FontApStatLbl()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 9);
	}
	static FSlateFontInfo FontApStatVal()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 12);
	}
	static FSlateFontInfo FontApKey()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 11);
	}
	static FSlateFontInfo FontApMode()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 10);
	}
};
