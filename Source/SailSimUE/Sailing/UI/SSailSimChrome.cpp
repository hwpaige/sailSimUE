#include "Sailing/UI/SSailSimChrome.h"
#include "Sailing/UI/SailSimStyle.h"
#include "Sailing/UI/SSailGauge.h"
#include "Sailing/UI/SSailMiniMapPaint.h"
#include "Sailing/UI/SailSimUserPrefs.h"
#include "SailSimUE.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Terrain/NantucketTerrainSubsystem.h"
#include "Sailing/Terrain/NantucketStructuresSubsystem.h"
#include "Sailing/SailSimPerf.h"
#include "Sailing/Nav/NavWaypointSubsystem.h"
#include "Sailing/Nav/EncAidSubsystem.h"
#include "Sailing/Nav/MooredBoatSubsystem.h"
#include "Sailing/Wind/WindFieldSubsystem.h"
#include "Sailing/Nav/NavGeo.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"

#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Images/SImage.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/DateTime.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

void SSailSimChrome::Construct(const FArguments& InArgs)
{
	Boat = InArgs._Boat;
	GameInstance = InArgs._GameInstance;

	// Restore mini-map size before building the widget tree so the first frame
	// is already at the saved size (boat prefs apply later for sail/wind/etc.).
	{
		FSailSimUserPrefs Prefs;
		Prefs.Load();
		MapPanelW = FMath::Clamp(Prefs.MapPanelW, MapMinW, MapMaxW);
		MapPanelH = FMath::Clamp(Prefs.MapPanelH, MapMinH, MapMaxH);
	}

	ChildSlot
	[
		SNew(SOverlay)

		// ---- Top bar gradient feel via deep strip ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Top)
		[
			SNew(SBox)
			.HeightOverride(52.f)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::Transparent())
				.Padding(FMargin(18.f, 10.f, 18.f, 6.f))
				[
					BuildTopBar()
				]
			]
		]

		// ---- Metrics card (top-left): boat, FPS, sail/wind/cloth lines ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(14.f, 12.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(500.f)
			[
				BuildStatusCard()
			]
		]

		// ---- Gauge column (top-right) ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, 58.f, 14.f, 0.f))
		[
			BuildGaugeColumn()
		]

		// ---- Waypoints ABOVE mini map (bottom-left stack) ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(10.f, 0.f, 0.f, 10.f))
		[
			// Explicit vertical stack — do not use a horizontal cluster here.
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
			[
				BuildWaypointPanel()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Left)
			[
				BuildMiniMap()
			]
		]

		// ---- Helm / sails (bottom-center) ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, 0.f, 14.f))
		[
			SNew(SBox)
			.WidthOverride(420.f)
			[
				BuildHelmBar()
			]
		]

		// ---- Autopilot + DC panel (bottom-right) ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, 12.f, 12.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				BuildAutopilotPanel()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			[
				SAssignNew(LightsPanelBorder, SBorder)
				.BorderImage(FSailSimStyle::BreakerPanelBrush())
				.Padding(FMargin(5.f, 8.f, 6.f, 8.f))
				[
					SNew(SBox)
					.WidthOverride(152.f)
					[
						BuildLightsBreakerPanel()
					]
				]
			]
		]

		// ---- Settings drawer ----
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Fill)
		.Padding(FMargin(0.f, 12.f, 12.f, 12.f))
		[
			SAssignNew(SettingsDrawer, SBorder)
			.Visibility_Lambda([this]()
			{
				return bSettingsOpen ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(FSailSimStyle::DrawerBrush())
			.Padding(FMargin(20.f, 18.f))
			[
				SNew(SBox)
				.WidthOverride(360.f)
				[
					BuildSettingsDrawer()
				]
			]
		]
	];
}

TSharedRef<SWidget> SSailSimChrome::MakeGlassCard(const TSharedRef<SWidget>& Content, FMargin Pad)
{
	return SNew(SBorder)
		.BorderImage(FSailSimStyle::CardBrush())
		.Padding(Pad)
		[
			Content
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeSectionCard(const FString& Title, const TSharedRef<SWidget>& Content)
{
	return SNew(SBorder)
		.BorderImage(FSailSimStyle::SectionCardBrush())
		.Padding(FMargin(12.f, 11.f, 12.f, 12.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBorder)
					.BorderImage(FSailSimStyle::SectionHeaderChip())
					.Padding(FMargin(8.f, 3.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(Title))
						.Font(FSailSimStyle::FontLabel())
						.ColorAndOpacity(FSailSimStyle::Accent)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
				[
					SNew(SBox).HeightOverride(1.f)
					[
						SNew(SBorder).BorderImage(FSailSimStyle::PillBrush())
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Content
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeStyledSlider(
	TFunction<float()> GetNorm01,
	TFunction<void(float)> OnNorm01,
	float Height)
{
	const float H = (Height > 1.f) ? Height : FSailSimStyle::SliderRowH;
	return SNew(SBox)
		.HeightOverride(H)
		[
			SNew(SSlider)
			.Style(&FSailSimStyle::SliderStyle())
			.Value_Lambda([GetNorm01]() { return GetNorm01 ? GetNorm01() : 0.f; })
			.OnValueChanged_Lambda([OnNorm01](float V) { if (OnNorm01) OnNorm01(V); })
		];
}

TSharedRef<SWidget> SSailSimChrome::MakePillButton(const FText& Label, FOnClicked OnClicked, bool bAccent)
{
	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(FMargin(12.f, 7.f))
		.OnClicked(OnClicked)
		[
			SNew(SBorder)
			.BorderImage(bAccent ? FSailSimStyle::PillBrushAccent() : FSailSimStyle::PillBrush())
			.Padding(FMargin(12.f, 6.f))
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FSailSimStyle::FontBody())
				.ColorAndOpacity(bAccent ? FSailSimStyle::Accent : FSailSimStyle::Text)
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildTopBar()
{
	// Environment instrument (time + season) + Settings. Noon = Fair Day baseline.
	auto MakeEnvRow = [](
		const FString& Tag,
		TSharedPtr<STextBlock>& OutVal,
		TSharedPtr<SSlider>& OutSlider,
		TFunction<float()> Get01,
		TFunction<void(float)> On01,
		const FString& Initial) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::SectionHeaderChip())
				.Padding(FMargin(7.f, 3.f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Tag))
					.Font(FSailSimStyle::FontSection())
					.ColorAndOpacity(FSailSimStyle::Accent)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::ValueChipBrush())
				.Padding(FMargin(8.f, 4.f))
				[
					SAssignNew(OutVal, STextBlock)
					.Font(FSailSimStyle::FontMonoSm())
					.ColorAndOpacity(FSailSimStyle::Text)
					.MinDesiredWidth(112.f)
					.Text(FText::FromString(Initial))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(200.f)
				.HeightOverride(FSailSimStyle::SliderRowH)
				[
					SAssignNew(OutSlider, SSlider)
					.Style(&FSailSimStyle::SliderStyle())
					.Value_Lambda([Get01]() { return Get01 ? Get01() : 0.f; })
					.OnValueChanged_Lambda([On01](float V) { if (On01) On01(V); })
				]
			];
	};

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			SNew(SSpacer)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBorder)
			.BorderImage(FSailSimStyle::EnvPanelBrush())
			.Padding(FMargin(14.f, 10.f, 16.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeEnvRow(
						TEXT("TIME"),
						TimeOfDayLabel,
						TimeOfDaySlider,
						[this]() { return CachedTimeOfDay01; },
						[this](float V) { OnTimeOfDaySlider(V); },
						TEXT("12:00  Fair Day"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
				[
					MakeEnvRow(
						TEXT("SEASON"),
						SeasonLabel,
						SeasonSlider,
						[this]() { return CachedSeason01; },
						[this](float V) { OnSeasonSlider(V); },
						TEXT("Summer"))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.ContentPadding(0.f)
			.OnClicked_Lambda([this]()
			{
				ToggleSettings();
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.BorderImage_Lambda([this]() -> const FSlateBrush*
				{
					return bSettingsOpen ? FSailSimStyle::ButtonBrushActive() : FSailSimStyle::ButtonBrush();
				})
				.Padding(FMargin(16.f, 12.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return bSettingsOpen
							? FText::FromString(TEXT("Close"))
							: FText::FromString(TEXT("☰  Settings"));
					})
					.Font(FSailSimStyle::FontBody())
					.ColorAndOpacity(FSailSimStyle::Text)
				]
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildStatusCard()
{
	auto Line = [](TSharedPtr<STextBlock>& Out, const FLinearColor& Color) -> TSharedRef<SWidget>
	{
		return SAssignNew(Out, STextBlock)
			.Font(FSailSimStyle::FontMonoSm())
			.ColorAndOpacity(Color)
			.Text(FText::FromString(TEXT("")));
	};

	return MakeGlassCard(
		SNew(SVerticalBox)
		// Boat summary (was top-right pill)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(BoatChip, STextBlock)
			.Font(FSailSimStyle::FontMonoSm())
			.ColorAndOpacity(FSailSimStyle::Accent)
			.Text(FText::FromString(TEXT("—")))
		]
		// Live FPS (from wall frame EMA — same source as PERFORMANCE panel)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[
			SAssignNew(FpsLabel, STextBlock)
			.Font(FSailSimStyle::FontMonoSm())
			.ColorAndOpacity(FSailSimStyle::Stbd)
			.Text(FText::FromString(TEXT("FPS  —")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[ Line(StatusLine1, FSailSimStyle::Text) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[ Line(StatusLine2, FSailSimStyle::Text) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[ Line(StatusLine3, FSailSimStyle::Accent) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
		[ Line(StatusLine4, FSailSimStyle::Stbd) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[ Line(StatusLine5, FSailSimStyle::TextDim) ]
		, FMargin(16.f, 12.f));
}

TSharedRef<SWidget> SSailSimChrome::BuildGaugeColumn()
{
	const float G = FSailSimStyle::GaugeSize;
	// Gauges paint their own layered glass bezel; light outer stack for alignment.
	return SNew(SBorder)
		.BorderImage(FSailSimStyle::Transparent())
		.Padding(FMargin(4.f, 2.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, FSailSimStyle::GaugeGap)
			[
				SNew(SBox).WidthOverride(G).HeightOverride(G)
				[
					SNew(SSailGauge)
					.Kind(ESailGaugeKind::Compass)
					.Size(G)
					.Primary_Lambda([this]() { return CachedHeading; })
					.Secondary_Lambda([this]() { return CachedTwd; })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, FSailSimStyle::GaugeGap)
			[
				SNew(SBox).WidthOverride(G).HeightOverride(G)
				[
					SNew(SSailGauge)
					.Kind(ESailGaugeKind::Heel)
					.Size(G)
					.Primary_Lambda([this]() { return CachedHeel; })
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).WidthOverride(G).HeightOverride(G)
				[
					SNew(SSailGauge)
					.Kind(ESailGaugeKind::Pitch)
					.Size(G)
					.Primary_Lambda([this]() { return CachedPitch; })
				]
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildHelmBar()
{
	// Compact instrument row: muted label | big-thumb slider | mono readout chip
	auto MakeSailRow = [this](
		const FString& Label,
		TSharedPtr<STextBlock>& OutVal,
		TFunction<float()> GetNorm,
		TFunction<void(float)> OnNorm,
		const FString& InitialVal) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(SBox).WidthOverride(58.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font(FSailSimStyle::FontSection())
					.ColorAndOpacity(FSailSimStyle::TextDim)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				MakeStyledSlider(GetNorm, OnNorm)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::ValueChipBrush())
				.Padding(FMargin(8.f, 4.f))
				[
					SNew(SBox).WidthOverride(44.f)
					[
						SAssignNew(OutVal, STextBlock)
						.Font(FSailSimStyle::FontMonoSm())
						.ColorAndOpacity(FSailSimStyle::Accent)
						.Justification(ETextJustify::Right)
						.Text(FText::FromString(InitialVal))
					]
				]
			];
	};

	return MakeGlassCard(
		SNew(SVerticalBox)

		// ---- HELM header strip ----
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("HELM")))
				.Font(FSailSimStyle::FontTitle())
				.ColorAndOpacity(FSailSimStyle::Text)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SBox).HeightOverride(1.f)
				[
					SNew(SBorder).BorderImage(FSailSimStyle::CardBrushDeep())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::ValueChipBrush())
				.Padding(FMargin(10.f, 4.f))
				[
					SAssignNew(HelmReadout, STextBlock)
					.Font(FSailSimStyle::FontMonoSm())
					.ColorAndOpacity(FSailSimStyle::Accent)
					.Text(FText::FromString(TEXT("0°")))
				]
			]
		]

		// P —— rudder track —— S
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 2.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("P")))
				.Font(FSailSimStyle::FontMono())
				.ColorAndOpacity(FSailSimStyle::Port)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10.f, 0.f).VAlign(VAlign_Center)
			[
				SNew(SBox)
				.HeightOverride(FSailSimStyle::SliderRowH)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					.VAlign(VAlign_Center)
					[
						SNew(SBox).HeightOverride(FSailSimStyle::SliderBarPx + 4.f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.f)
							[
								SNew(SBorder).BorderImage(FSailSimStyle::HelmTrackPort())
							]
							+ SHorizontalBox::Slot().FillWidth(1.f)
							[
								SNew(SBorder).BorderImage(FSailSimStyle::HelmTrackStbd())
							]
						]
					]
					+ SOverlay::Slot()
					.VAlign(VAlign_Center)
					[
						SNew(SSlider)
						.Style(&FSailSimStyle::SliderStyle())
						.Value_Lambda([this]()
						{
							return (FMath::Clamp(CachedRudder, -35.f, 35.f) + 35.f) / 70.f;
						})
						.OnValueChanged_Lambda([this](float V) { OnHelmValue(V); })
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("S")))
				.Font(FSailSimStyle::FontMono())
				.ColorAndOpacity(FSailSimStyle::Stbd)
			]
		]

		// SAILS — same section-card language as Settings / env panel
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
		[
			MakeSectionCard(TEXT("SAIL TRIM"),
				SNew(SVerticalBox)
				// Wind + main sheet first (most used underway)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					MakeSailRow(TEXT("TWS"), CompactTwsLabel,
						[this]()
						{
							return FMath::Clamp(CachedTws / FBoatDynamics::WindDisplayMaxKn, 0.f, 1.f);
						},
						[this](float V) { OnTwsSlider(V); },
						TEXT("6kn"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					MakeSailRow(TEXT("SHEET"), CompactSheetLabel,
						[this]() { return FMath::Clamp(CachedSheetEase, 0.f, 1.f); },
						[this](float V) { OnSheetSlider(V); },
						TEXT("25%"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					MakeSailRow(TEXT("OUTHAUL"), CompactOuthaulLabel,
						[this]() { return FMath::Clamp(CachedOuthaul, 0.f, 1.f); },
						[this](float V) { OnOuthaulSlider(V); },
						TEXT("94%"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					MakeSailRow(TEXT("VANG"), CompactVangLabel,
						[this]() { return FMath::Clamp(CachedVang, 0.f, 1.f); },
						[this](float V) { OnVangSlider(V); },
						TEXT("40%"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
				[
					MakeSailRow(TEXT("JIB CAR"), CompactJibCarLabel,
						[this]() { return FMath::Clamp(CachedJibCar, 0.f, 1.f); },
						[this](float V) { OnJibCarSlider(V); },
						TEXT("45%"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
				[
					MakeSailRow(TEXT("SPIN"), CompactSpinSheetLabel,
						[this]() { return FMath::Clamp(CachedSpinSheet, 0.f, 1.f); },
						[this](float V) { OnSpinSheetSlider(V); },
						TEXT("18%"))
				]
				// Jib + kite set / douse
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 5.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(FMargin(0.f))
						.OnClicked_Lambda([this]()
						{
							OnJibSetToggle();
							return FReply::Handled();
						})
						[
							SNew(SBorder)
							.BorderImage_Lambda([this]() -> const FSlateBrush*
							{
								return CachedJibSet
									? FSailSimStyle::PillBrushAccent()
									: FSailSimStyle::PillBrush();
							})
							.Padding(FMargin(10.f, 8.f))
							.HAlign(HAlign_Center)
							[
								SAssignNew(CompactJibSetLabel, STextBlock)
								.Font(FSailSimStyle::FontLabel())
								.Justification(ETextJustify::Center)
								.ColorAndOpacity_Lambda([this]()
								{
									return CachedJibSet ? FSailSimStyle::Accent : FSailSimStyle::TextDim;
								})
								.Text_Lambda([this]()
								{
									return FText::FromString(CachedJibSet
										? TEXT("JIB · SET")
										: TEXT("JIB · DOWN"));
								})
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(5.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(FMargin(0.f))
						.OnClicked_Lambda([this]()
						{
							OnKiteSetToggle();
							return FReply::Handled();
						})
						[
							SNew(SBorder)
							.BorderImage_Lambda([this]() -> const FSlateBrush*
							{
								return CachedKiteSet
									? FSailSimStyle::PillBrushAccent()
									: FSailSimStyle::PillBrush();
							})
							.Padding(FMargin(10.f, 8.f))
							.HAlign(HAlign_Center)
							[
								SAssignNew(CompactKiteSetLabel, STextBlock)
								.Font(FSailSimStyle::FontLabel())
								.Justification(ETextJustify::Center)
								.ColorAndOpacity_Lambda([this]()
								{
									return CachedKiteSet ? FSailSimStyle::Accent : FSailSimStyle::TextDim;
								})
								.Text_Lambda([this]()
								{
									return FText::FromString(CachedKiteSet
										? TEXT("KITE · SET")
										: TEXT("KITE · DOWN"));
								})
							]
						]
					]
				]
			)
		]
		, FMargin(16.f, 14.f, 16.f, 14.f));
}

TSharedRef<SWidget> SSailSimChrome::BuildNavLowerCluster()
{
	// Kept for call-sites; Construct builds the stack inline so layout cannot drift.
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		.Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			BuildWaypointPanel()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		[
			BuildMiniMap()
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeApStatRow(const FString& Lbl, TSharedPtr<STextBlock>& OutVal)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Bottom)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Lbl))
			.Font(FSailSimStyle::FontApStatLbl())
			.ColorAndOpacity(FSailSimStyle::ApStatLbl)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
		[
			SAssignNew(OutVal, STextBlock)
			.Font(FSailSimStyle::FontApStatVal())
			.ColorAndOpacity(FSailSimStyle::ApStatVal)
			.Text(FText::FromString(TEXT("—")))
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeApKey(const FString& Label, float Delta, bool bDec)
{
	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(0.f)
		.OnClicked_Lambda([this, Delta]()
		{
			OnApNudge(Delta);
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderImage(bDec ? FSailSimStyle::ApKeyPort() : FSailSimStyle::ApKeyStbd())
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 7.f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.Font(FSailSimStyle::FontApKey())
				.ColorAndOpacity(FLinearColor::White)
				.Justification(ETextJustify::Center)
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildAutopilotPanel()
{
	// Match web dist .autopilot-panel (128px, mode tabs, stats, keypad, slider, tack/trim)
	return SAssignNew(ApPanelBorder, SBorder)
		.BorderImage(FSailSimStyle::ApPanelBrush())
		.Padding(FMargin(11.f, 10.f, 11.f, 11.f))
		[
			SNew(SBox)
			.WidthOverride(FSailSimStyle::ApPanelW - 22.f) // content inside padding ≈ 106; outer ~128 with pad
			[
				SNew(SVerticalBox)

				// Header
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("AUTOPILOT")))
					.Font(FSailSimStyle::FontApHeader())
					.ColorAndOpacity(FSailSimStyle::ApHeader)
				]

				// Mode tabs: HDG | AWA | Nav
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.OnClicked_Lambda([this]() { OnApMode(0); return FReply::Handled(); })
						[
							SAssignNew(ApTabHdgBorder, SBorder)
							.BorderImage(FSailSimStyle::ApTabIdle())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 6.f))
							[
								SAssignNew(ApTabHdgLbl, STextBlock)
								.Text(FText::FromString(TEXT("HDG")))
								.Font(FSailSimStyle::FontApTab())
								.ColorAndOpacity(FSailSimStyle::ApHeader)
								.Justification(ETextJustify::Center)
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 2.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.OnClicked_Lambda([this]() { OnApMode(1); return FReply::Handled(); })
						[
							SAssignNew(ApTabAwaBorder, SBorder)
							.BorderImage(FSailSimStyle::ApTabIdle())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 6.f))
							[
								SAssignNew(ApTabAwaLbl, STextBlock)
								.Text(FText::FromString(TEXT("AWA")))
								.Font(FSailSimStyle::FontApTab())
								.ColorAndOpacity(FSailSimStyle::ApHeader)
								.Justification(ETextJustify::Center)
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.OnClicked_Lambda([this]() { OnApMode(2); return FReply::Handled(); })
						[
							SAssignNew(ApTabNavBorder, SBorder)
							.BorderImage(FSailSimStyle::ApTabIdle())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 6.f))
							[
								SAssignNew(ApTabNavLbl, STextBlock)
								.Text(FText::FromString(TEXT("NAV")))
								.Font(FSailSimStyle::FontApTab())
								.ColorAndOpacity(FSailSimStyle::ApHeader)
								.Justification(ETextJustify::Center)
							]
						]
					]
				]

				// Stats HDG / AWA / TGT
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight() [ MakeApStatRow(TEXT("HDG"), ApHdgVal) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
					[ MakeApStatRow(TEXT("AWA"), ApAwaVal) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
					[ MakeApStatRow(TEXT("TGT"), ApTgtVal) ]
				]

				// Keypad −10 +10 / −1 +1
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)
						[ MakeApKey(TEXT("−10"), -10.f, true) ]
						+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)
						[ MakeApKey(TEXT("+10"), 10.f, false) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)
						[ MakeApKey(TEXT("−1"), -1.f, true) ]
						+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)
						[ MakeApKey(TEXT("+1"), 1.f, false) ]
					]
				]

				// Target slider label
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
				[
					SAssignNew(ApSliderLbl, STextBlock)
					.Text(FText::FromString(TEXT("TARGET HEADING")))
					.Font(FSailSimStyle::FontApStatLbl())
					.ColorAndOpacity(FSailSimStyle::ApStatLbl)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
				[
					SNew(SBox).HeightOverride(FSailSimStyle::SliderRowH)
					[
						SNew(SOverlay)
						+ SOverlay::Slot().VAlign(VAlign_Center)
						[
							SNew(SBox).HeightOverride(FSailSimStyle::SliderBarPx + 2.f)
							[
								SNew(SBorder)
								.BorderImage_Lambda([this]() -> const FSlateBrush*
								{
									return CachedAutoMode == FBoatDynamics::EAutoMode::Awa
										? FSailSimStyle::ApSliderTrackAwa()
										: FSailSimStyle::ApSliderTrackHdg();
								})
							]
						]
						+ SOverlay::Slot().VAlign(VAlign_Center)
						[
							SNew(SSlider)
							.Style(&FSailSimStyle::SliderStyle())
							.IsEnabled_Lambda([this]()
							{
								return CachedAutoMode != FBoatDynamics::EAutoMode::Nav;
							})
							.Value_Lambda([this]()
							{
								if (CachedAutoMode == FBoatDynamics::EAutoMode::Awa)
								{
									// −170..+170 → 0..1
									return FMath::Clamp((CachedAutoAwaTarget + 170.f) / 340.f, 0.f, 1.f);
								}
								return FMath::Clamp(FMath::Fmod(CachedAutoTarget + 360.f, 360.f) / 359.f, 0.f, 1.f);
							})
							.OnValueChanged_Lambda([this](float V) { OnApSlider(V); })
						]
					]
				]

				// AUTO | Tack | Trim  (AUTO = pilot on/off; TRIM is independent sheet auto)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 7.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 2.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.ToolTipText(FText::FromString(
							TEXT("Engage / stand by autopilot in the selected mode (HDG · AWA · NAV)")))
						.OnClicked_Lambda([this]() { OnApAuto(); return FReply::Handled(); })
						[
							SAssignNew(ApAutoBorder, SBorder)
							.BorderImage(FSailSimStyle::ApModeAutoIdle())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 8.f))
							[
								SAssignNew(ApAutoLbl, STextBlock)
								.Text(FText::FromString(TEXT("AUTO")))
								.Font(FSailSimStyle::FontApMode())
								.ColorAndOpacity(FLinearColor::White)
								.Justification(ETextJustify::Center)
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 2.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.OnClicked_Lambda([this]() { OnApTack(); return FReply::Handled(); })
						[
							SNew(SBorder)
							.BorderImage(FSailSimStyle::ApModeTack())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 8.f))
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("TACK")))
								.Font(FSailSimStyle::FontApMode())
								.ColorAndOpacity(FLinearColor::White)
								.Justification(ETextJustify::Center)
							]
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(2.f, 0.f, 0.f, 0.f)
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.ToolTipText(FText::FromString(
							TEXT("Auto-sheet for live AWA — independent of autopilot on/off")))
						.OnClicked_Lambda([this]() { OnApTrim(); return FReply::Handled(); })
						[
							SAssignNew(ApTrimBorder, SBorder)
							.BorderImage(FSailSimStyle::ApModeTrimIdle())
							.HAlign(HAlign_Center)
							.Padding(FMargin(2.f, 8.f))
							[
								SAssignNew(ApTrimLbl, STextBlock)
								.Text(FText::FromString(TEXT("TRIM")))
								.Font(FSailSimStyle::FontApMode())
								.ColorAndOpacity(FLinearColor::White)
								.Justification(ETextJustify::Center)
							]
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeTelemetryPill(TSharedPtr<STextBlock>& OutText, const FLinearColor& Color)
{
	// Match AP HDG/AWA value type (FontApStatVal + ApStatVal).
	return SNew(SBorder)
		.BorderImage(FSailSimStyle::MapTelemetryPillBrush())
		.Padding(FMargin(6.f, 2.f))
		[
			SAssignNew(OutText, STextBlock)
			.Font(FSailSimStyle::FontApStatVal())
			.ColorAndOpacity(FSailSimStyle::ApStatVal)
		];
}

TSharedRef<SWidget> SSailSimChrome::MakePlotToolBtn(const FString& Label, TSharedPtr<SBorder>& OutBorder,
	TSharedPtr<STextBlock>& OutLbl, FOnClicked OnClick)
{
	return SNew(SButton)
		.ButtonStyle(FCoreStyle::Get(), "NoBorder")
		.ContentPadding(0.f)
		.OnClicked(OnClick)
		[
			SAssignNew(OutBorder, SBorder)
			.BorderImage(FSailSimStyle::ApTabIdle())
			.Padding(FMargin(8.f, 5.f))
			[
				SAssignNew(OutLbl, STextBlock)
				.Text(FText::FromString(Label))
				.Font(FSailSimStyle::FontApStatVal())
				.ColorAndOpacity(FSailSimStyle::ApStatVal)
				.Justification(ETextJustify::Center)
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildMiniMap()
{
	// Full-bleed chart under rounded glass card — tiles fill the panel and are
	// clipped to the card's rounded corners (no inset that shrinks the map).
	return SAssignNew(MapOuterBox, SBox)
		.WidthOverride_Lambda([this]() { return MapPanelW; })
		.HeightOverride_Lambda([this]() { return MapPanelH; })
		[
			SNew(SBorder)
			.BorderImage(FSailSimStyle::CardBrush())
			.Padding(0.f)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::MapPanelBrush())
				.Padding(0.f)
				.Clipping(EWidgetClipping::ClipToBounds)
				[
				SNew(SOverlay)

				// Chart fills the inner panel
				+ SOverlay::Slot()
				[
					SAssignNew(MiniMapPaint, SSailMiniMapPaint)
					.BoatLat_Lambda([this]() { return CachedLat; })
					.BoatLon_Lambda([this]() { return CachedLon; })
					.Heading_Lambda([this]() { return CachedHeading; })
					.Twd_Lambda([this]() { return CachedTwd; })
					.GameInstance(GameInstance)
				]

				// Metrics overlay TOP (datetime + pills)
				+ SOverlay::Slot()
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Top)
				.Padding(FMargin(8.f, 5.f, 22.f, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
					[
						SAssignNew(MapTime, STextBlock)
						.Font(FSailSimStyle::FontApStatVal())
						.ColorAndOpacity(FSailSimStyle::ApStatVal)
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
						[
							SAssignNew(PillSogWrap, SBox)
							[ MakeTelemetryPill(PillSog, FSailSimStyle::MapOverlayText) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
						[ MakeTelemetryPill(PillTws, FSailSimStyle::MapOverlayText) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
						[ MakeTelemetryPill(PillTwd, FSailSimStyle::MapOverlayText) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
						[
							SAssignNew(PillAwaWrap, SBox)
							[ MakeTelemetryPill(PillAwa, FSailSimStyle::MapOverlayText) ]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
						[
							SAssignNew(PillAwsWrap, SBox)
							[ MakeTelemetryPill(PillAws, FSailSimStyle::MapOverlayText) ]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SAssignNew(PillHelmWrap, SBox)
							[ MakeTelemetryPill(PillHelm, FSailSimStyle::MapOverlayText) ]
						]
					]
				]

				// Plot tools (web .nav-minimap-tools) under metrics
				+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Top)
				.Padding(FMargin(8.f, 30.f, 0.f, 0.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
					[
						MakePlotToolBtn(TEXT("+ WP"), PlotBtnAdd, PlotAddLbl,
							FOnClicked::CreateLambda([this]()
							{
								SetPlotMode(1);
								return FReply::Handled();
							}))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
					[
						MakePlotToolBtn(TEXT("− WP"), PlotBtnDel, PlotDelLbl,
							FOnClicked::CreateLambda([this]()
							{
								SetPlotMode(2);
								return FReply::Handled();
							}))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(FCoreStyle::Get(), "NoBorder")
						.ContentPadding(0.f)
						.OnClicked_Lambda([this]() { OnPlotClear(); return FReply::Handled(); })
						[
							SNew(SBorder)
							.BorderImage(FSailSimStyle::ApTabIdle())
							.Padding(FMargin(8.f, 5.f))
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("CLEAR")))
								.Font(FSailSimStyle::FontApStatVal())
								.ColorAndOpacity(FSailSimStyle::Port)
							]
						]
					]
				]

				// Bottom-left coords
				+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Bottom)
				.Padding(FMargin(8.f, 0.f, 0.f, 5.f))
				[
					SNew(SBox)
					.MaxDesiredWidth_Lambda([this]() { return MapPanelW * 0.48f; })
					[
						SAssignNew(MapCoords, STextBlock)
						.Font(FSailSimStyle::FontApStatVal())
						.ColorAndOpacity(FSailSimStyle::ApStatVal)
					]
				]

				// Bottom-right place
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Bottom)
				.Padding(FMargin(0.f, 0.f, 8.f, 5.f))
				[
					SNew(SBox)
					.MaxDesiredWidth_Lambda([this]() { return MapPanelW * 0.48f; })
					[
						SAssignNew(MapPlace, STextBlock)
						.Font(FSailSimStyle::FontApStatVal())
						.ColorAndOpacity(FSailSimStyle::ApStatVal)
						.Justification(ETextJustify::Right)
					]
				]

				// Top-right resize handle
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Top)
				[
					SNew(SBox)
					.WidthOverride(28.f)
					.HeightOverride(28.f)
					[
						SNew(SBorder)
						.BorderImage(FSailSimStyle::Transparent())
						.Cursor(EMouseCursor::CardinalCross)
						.ToolTipText(FText::FromString(TEXT("Drag to resize chart")))
						.OnMouseButtonDown_Lambda([this](const FGeometry& G, const FPointerEvent& E)
						{
							return OnMapResizeDown(G, E);
						})
						[
							SNew(SBox)
							.HAlign(HAlign_Right)
							.VAlign(VAlign_Top)
							.Padding(FMargin(0.f, 5.f, 5.f, 0.f))
							[
								SNew(SBox)
								.WidthOverride(12.f)
								.HeightOverride(12.f)
								[
									SNew(SBorder)
									.BorderImage_Lambda([]() -> const FSlateBrush*
									{
										static const FSlateRoundedBoxBrush B(
											FLinearColor(0.f, 0.f, 0.f, 0.f),
											0.f,
											FLinearColor(0.81f, 0.91f, 0.93f, 0.65f),
											1.5f);
										return &B;
									})
								]
							]
						]
					]
				]

				// Inner rounded frame over full-bleed tiles
				+ SOverlay::Slot()
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					SNew(SBorder)
					.BorderImage(FSailSimStyle::MapPanelFrameBrush())
					.Padding(0.f)
					.Visibility(EVisibility::HitTestInvisible)
				]
				] // inner MapPanelBrush border
			] // outer CardBrush border
		];
}

/**
 * One table line matching web .nav-waypoint-table columns:
 * #  Lat  Lon  BRG  Dist  Cum  TTG  ETA
 * (TTG/ETA strings pre-formatted; pass "—" when SOG is too low.)
 */
static FString FormatWpRowLine(
	int32 Index1Based,
	double Lat, double Lon,
	double Brg, double LegNm, double CumNm,
	const FString& Ttg, const FString& Eta)
{
	return FString::Printf(
		TEXT("%2d  %-11s  %-12s  %-5s  %-7s  %-7s  %-5s  %s"),
		Index1Based,
		*FNavGeo::FormatLat(Lat),
		*FNavGeo::FormatLon(Lon),
		*FNavGeo::FormatBrg(Brg),
		*FNavGeo::FormatNm(LegNm),
		*FNavGeo::FormatNm(CumNm),
		*Ttg,
		*Eta);
}

TSharedRef<SWidget> SSailSimChrome::BuildWaypointPanel()
{
	// Match web .nav-waypoint-panel: title, column header, stacked rows, footer.
	// Rows live in an SVerticalBox of fixed-height SBox children (one line of text each).
	// That layout cannot collapse multiple rows onto the same Y.
	const FLinearColor HeadCol(0.42f, 0.44f, 0.47f, 1.f);
	constexpr float RowH = 28.f;
	constexpr float MaxListH = 8.f * RowH;

	return SAssignNew(WpPanelBox, SBox)
		.WidthOverride_Lambda([this]() { return MapPanelW; })
		.HeightOverride(96.f) // updated in RefreshWaypointPanel
		[
			SNew(SBorder)
			.BorderImage(FSailSimStyle::CardBrush())
			.Padding(FMargin(0.f))
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(10.f, 8.f, 10.f, 6.f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("WAYPOINTS")))
						.Font(FSailSimStyle::FontApHeader())
						.ColorAndOpacity(FSailSimStyle::ApHeader)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SAssignNew(WpSummary, STextBlock)
						.Font(FSailSimStyle::FontApStatLbl())
						.ColorAndOpacity(FLinearColor(1.f, 0.62f, 0.35f, 1.f))
						.Text(FText::FromString(TEXT("No route")))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(6.f, 0.f, 6.f, 0.f))
				[
					SNew(SBorder)
					.BorderImage(FSailSimStyle::ApTabIdle())
					.Padding(FMargin(6.f, 5.f))
					[
						// Match web thead: # Lat Lon BRG Dist Cum TTG ETA
						SNew(STextBlock)
						.Text(FText::FromString(TEXT(" #  Lat          Lon           BRG    Dist     Cum      TTG    ETA")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(HeadCol)
					]
				]
				// Fixed max height; rows stack inside a single scroll content vertical box.
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(4.f, 2.f, 4.f, 4.f))
				[
					SNew(SBox)
					.MinDesiredHeight(36.f)
					.MaxDesiredHeight(MaxListH)
					[
						SNew(SScrollBox)
						.Orientation(Orient_Vertical)
						.ScrollBarThickness(FVector2D(5.f, 5.f))
						.ConsumeMouseWheel(EConsumeMouseWheel::WhenScrollingPossible)
						+ SScrollBox::Slot().AutoSize()
						[
							SAssignNew(WpListBox, SVerticalBox)
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(10.f, 0.f, 10.f, 8.f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("+ WP on chart · click row to select")))
					.Font(FSailSimStyle::FontApStatLbl())
					.ColorAndOpacity(FSailSimStyle::ApStatLbl)
					.AutoWrapText(true)
				]
			]
		];
}

/** Title + col header + N rows + footer; caps row body then scrolls. */
static float WpPanelHeightForCount(int32 NumWps)
{
	constexpr float TitleH = 34.f;
	constexpr float ColHeadH = 30.f;
	constexpr float RowH = 28.f;
	constexpr float FooterH = 26.f;
	constexpr float Pad = 8.f;
	constexpr int32 MaxVisibleRows = 8;
	const int32 Rows = FMath::Clamp(NumWps, 0, MaxVisibleRows);
	const float BodyH = (NumWps <= 0) ? 36.f : float(Rows) * RowH;
	return TitleH + ColHeadH + BodyH + FooterH + Pad;
}

void SSailSimChrome::ApplyMapSize(float W, float H)
{
	float MaxW = MapMaxW;
	float MaxH = MapMaxH;
	if (FSlateApplication::IsInitialized())
	{
		const FVector2D Viewport = FSlateApplication::Get().GetPreferredWorkArea().GetSize();
		if (Viewport.X > 100.f && Viewport.Y > 100.f)
		{
			MaxW = FMath::Min(MapMaxW, Viewport.X * 0.9f);
			MaxH = FMath::Min(MapMaxH, Viewport.Y * 0.75f);
		}
	}

	MapPanelW = FMath::Clamp(W, MapMinW, MaxW);
	MapPanelH = FMath::Clamp(H, MapMinH, MaxH);
	if (MapOuterBox.IsValid())
	{
		MapOuterBox->SetWidthOverride(MapPanelW);
		MapOuterBox->SetHeightOverride(MapPanelH);
		MapOuterBox->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}
	// Keep waypoint panel the same width as the map.
	if (WpPanelBox.IsValid())
	{
		int32 N = 0;
		if (UGameInstance* GI = GameInstance.Get())
		{
			if (UNavWaypointSubsystem* Nav = GI->GetSubsystem<UNavWaypointSubsystem>())
			{
				N = Nav->Num();
			}
		}
		WpPanelBox->SetWidthOverride(MapPanelW);
		WpPanelBox->SetHeightOverride(WpPanelHeightForCount(N));
		WpPanelBox->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}
	Invalidate(EInvalidateWidgetReason::Layout);
	if (bPrefsApplied)
	{
		SchedulePrefsSave();
	}
}

FReply SSailSimChrome::OnMapResizeDown(const FGeometry& Geo, const FPointerEvent& Event)
{
	if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	if (!MapOuterBox.IsValid())
	{
		return FReply::Unhandled();
	}

	// Pin the panel's left & bottom edges (bottom-left anchor). The top-right
	// corner is driven to the cursor so it stays under the mouse 1:1.
	const FGeometry PanelGeo = MapOuterBox->GetCachedGeometry();
	const FVector2D AbsPos = PanelGeo.GetAbsolutePosition();
	const FVector2D AbsSize = PanelGeo.GetAbsoluteSize();
	MapResizeFixedLeftAbs = AbsPos.X;
	MapResizeFixedBottomAbs = AbsPos.Y + AbsSize.Y;
	// Local size = absolute size / scale (Retina / DPI)
	MapResizeGeoScale = (MapPanelW > 1.f) ? (AbsSize.X / MapPanelW) : PanelGeo.Scale;
	if (MapResizeGeoScale < 0.1f)
	{
		MapResizeGeoScale = 1.f;
	}

	bMapResizing = true;
	return FReply::Handled().CaptureMouse(AsShared());
}

FReply SSailSimChrome::OnMapResizeMove(const FGeometry& Geo, const FPointerEvent& Event)
{
	return FReply::Unhandled();
}

FReply SSailSimChrome::OnMapResizeUp(const FGeometry& Geo, const FPointerEvent& Event)
{
	return FReply::Unhandled();
}

FReply SSailSimChrome::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bMapResizing)
	{
		// Cursor in the same absolute space as GetAbsolutePosition/Size
		const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
		// Desired absolute size so top-right corner == cursor
		const float AbsW = float(Cursor.X - MapResizeFixedLeftAbs);
		const float AbsH = float(MapResizeFixedBottomAbs - Cursor.Y);
		// Convert absolute px → local layout units
		const float Scale = FMath::Max(0.1f, MapResizeGeoScale);
		ApplyMapSize(AbsW / Scale, AbsH / Scale);
		return FReply::Handled();
	}
	return SCompoundWidget::OnMouseMove(MyGeometry, MouseEvent);
}

FReply SSailSimChrome::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bMapResizing && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bMapResizing = false;
		// Persist size immediately when the drag ends (not only after debounce).
		if (bPrefsApplied)
		{
			FlushPrefsSave();
		}
		else
		{
			SchedulePrefsSave();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}
	return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
}

TSharedRef<SWidget> SSailSimChrome::MakeLabeledSlider(
	const FString& Title,
	TSharedPtr<STextBlock>& OutLabel,
	TFunction<float()> GetNorm01,
	TFunction<void(float)> OnNorm01)
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Title))
				.Font(FSailSimStyle::FontBody())
				.ColorAndOpacity(FSailSimStyle::Text)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::ValueChipBrush())
				.Padding(FMargin(8.f, 3.f))
				[
					SAssignNew(OutLabel, STextBlock)
					.Font(FSailSimStyle::FontMonoSm())
					.ColorAndOpacity(FSailSimStyle::Accent)
					.Text(FText::FromString(TEXT("—")))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
		[
			MakeStyledSlider(GetNorm01, OnNorm01)
		];
}

TSharedRef<SWidget> SSailSimChrome::MakeBreakerRow(
	const FString& Label,
	TFunction<bool()> IsOn,
	TFunction<void()> OnToggle)
{
	// Tight left pad so labels sit near the panel edge; compact pilot + rocker.
	return SNew(SBorder)
		.BorderImage(FSailSimStyle::BreakerSlotBrush())
		.Padding(FMargin(3.f, 5.f, 4.f, 5.f))
		.OnMouseButtonDown_Lambda([OnToggle](const FGeometry&, const FPointerEvent& Ev)
		{
			if (Ev.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				if (OnToggle) OnToggle();
				return FReply::Handled();
			}
			return FReply::Unhandled();
		})
		[
			SNew(SHorizontalBox)
			// Pilot light
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 5.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(8.f)
				.HeightOverride(8.f)
				[
					SNew(SBorder)
					.BorderImage_Lambda([IsOn]() -> const FSlateBrush*
					{
						return (IsOn && IsOn())
							? FSailSimStyle::BreakerPilotOn()
							: FSailSimStyle::BreakerPilotOff();
					})
				]
			]
			// Label — flush toward left edge of the slot
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			.HAlign(HAlign_Left)
			.Padding(0.f, 0.f, 2.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.Font(FSailSimStyle::FontSection())
				.ColorAndOpacity(FSailSimStyle::BreakerLabel)
			]
			// Rocker
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(32.f)
				.HeightOverride(16.f)
				[
					SNew(SBorder)
					.BorderImage_Lambda([IsOn]() -> const FSlateBrush*
					{
						return (IsOn && IsOn())
							? FSailSimStyle::BreakerToggleOn()
							: FSailSimStyle::BreakerToggleOff();
					})
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([IsOn]()
						{
							return (IsOn && IsOn())
								? FText::FromString(TEXT("ON"))
								: FText::FromString(TEXT("OFF"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 7))
						.ColorAndOpacity(FLinearColor::White)
						.Justification(ETextJustify::Center)
					]
				]
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildLightsBreakerPanel()
{
	auto BoatPtr = [this]() -> ASailBoatPawn*
	{
		return Boat.IsValid() ? Boat.Get() : nullptr;
	};

	return SNew(SVerticalBox)

		// Header
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("DC PANEL")))
				.Font(FSailSimStyle::FontTitle())
				.ColorAndOpacity(FSailSimStyle::BreakerLabel)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("12V LIGHTING  ·  COLREGS")))
				.Font(FSailSimStyle::FontSection())
				.ColorAndOpacity(FSailSimStyle::TextMute)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			MakeBreakerRow(TEXT("NAV LIGHTS"),
				[BoatPtr]() { ASailBoatPawn* B = BoatPtr(); return B && B->IsBreakerNav(); },
				[BoatPtr]()
				{
					if (ASailBoatPawn* B = BoatPtr())
						B->SetBreakerNav(!B->IsBreakerNav());
				})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			MakeBreakerRow(TEXT("STEAMING"),
				[BoatPtr]() { ASailBoatPawn* B = BoatPtr(); return B && B->IsBreakerSteaming(); },
				[BoatPtr]()
				{
					if (ASailBoatPawn* B = BoatPtr())
						B->SetBreakerSteaming(!B->IsBreakerSteaming());
				})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			MakeBreakerRow(TEXT("ANCHOR"),
				[BoatPtr]() { ASailBoatPawn* B = BoatPtr(); return B && B->IsBreakerAnchor(); },
				[BoatPtr]()
				{
					if (ASailBoatPawn* B = BoatPtr())
						B->SetBreakerAnchor(!B->IsBreakerAnchor());
				})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			MakeBreakerRow(TEXT("DECK LIGHTS"),
				[BoatPtr]() { ASailBoatPawn* B = BoatPtr(); return B && B->IsBreakerDeck(); },
				[BoatPtr]()
				{
					if (ASailBoatPawn* B = BoatPtr())
						B->SetBreakerDeck(!B->IsBreakerDeck());
				})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
		[
			MakeBreakerRow(TEXT("CABIN"),
				[BoatPtr]() { ASailBoatPawn* B = BoatPtr(); return B && B->IsBreakerCabin(); },
				[BoatPtr]()
				{
					if (ASailBoatPawn* B = BoatPtr())
						B->SetBreakerCabin(!B->IsBreakerCabin());
				})
		]

		// Master OFF
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.ContentPadding(0.f)
			.OnClicked_Lambda([BoatPtr]()
			{
				if (ASailBoatPawn* B = BoatPtr()) B->SetAllBreakersOff();
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.BorderImage(FSailSimStyle::BreakerSlotBrush())
				.Padding(FMargin(8.f, 7.f))
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("ALL OFF")))
					.Font(FSailSimStyle::FontLabel())
					.ColorAndOpacity(FSailSimStyle::Port)
				]
			]
		];
}

TSharedRef<SWidget> SSailSimChrome::BuildSettingsDrawer()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("SETTINGS")))
					.Font(FSailSimStyle::FontBrand())
					.ColorAndOpacity(FSailSimStyle::Text)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Environment · wind · performance")))
					.Font(FSailSimStyle::FontMonoSm())
					.ColorAndOpacity(FSailSimStyle::TextDim)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), "NoBorder")
				.ContentPadding(0.f)
				.OnClicked_Lambda([this]() { bSettingsOpen = false; return FReply::Handled(); })
				[
					SNew(SBorder)
					.BorderImage(FSailSimStyle::ButtonBrush())
					.Padding(FMargin(14.f, 8.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("✕")))
						.Font(FSailSimStyle::FontBody())
						.ColorAndOpacity(FSailSimStyle::Text)
					]
				]
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.f).Padding(0.f, 14.f, 0.f, 0.f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)

				// ---- Environment presets ----
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
				[
					MakeSectionCard(TEXT("ENVIRONMENT"),
						BuildEnvPresetRow())
				]

				// ---- Sky / fog ----
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
				[
					MakeSectionCard(TEXT("SKY / FOG"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							MakeLabeledSlider(
								TEXT("Height fog"),
								FogSliderLabel,
								[this]() { return FMath::Clamp(CachedFogIntensity, 0.f, 1.f); },
								[this](float N) { OnFogSlider(N); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("0 = off · 100% = map default")))
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextMute)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
						[
							MakeLabeledSlider(
								TEXT("Volumetric clouds"),
								CloudSliderLabel,
								[this]() { return FMath::Clamp(CachedCloudIntensity, 0.f, 1.f); },
								[this](float N) { OnCloudSlider(N); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("0 = off · seam test: try both at 0")))
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextMute)
						])
				]

				// ---- Wind ----
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
				[
					MakeSectionCard(TEXT("WIND"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							MakeLabeledSlider(
								TEXT("True wind speed"),
								TwsSliderLabel,
								[this]()
								{
									return FMath::Clamp(CachedTws / FBoatDynamics::WindDisplayMaxKn, 0.f, 1.f);
								},
								[this](float N) { OnTwsSlider(N); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 14.f, 0.f, 0.f)
						[
							MakeLabeledSlider(
								TEXT("True wind direction"),
								TwdSliderLabel,
								[this]() { return FMath::Clamp(FMath::Fmod(CachedTwd + 360.f, 360.f) / 360.f, 0.f, 1.f); },
								[this](float N) { OnTwdSlider(N); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Keys:  [ ] TWS   ·   ; ' TWD")))
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextMute)
						])
				]

				// ---- Performance ----
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
				[
					MakeSectionCard(TEXT("PERFORMANCE"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(FText::FromString(
								TEXT("Engine unit + GPU times + SailSim GT scopes. Wall ≠ our code sum. ")
								TEXT("Also: stat unit · stat gpu")))
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextMute)
							.AutoWrapText(true)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
						[
							BuildPerfRow(TEXT("Wall"), PerfFrameLabel)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							SAssignNew(PerfBottleneckLabel, STextBlock)
							.Font(FSailSimStyle::FontLabel())
							.ColorAndOpacity(FSailSimStyle::Warn)
							.AutoWrapText(true)
							.Text(FText::FromString(TEXT("Bottleneck: —")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("ENGINE")))
							.Font(FSailSimStyle::FontSection())
							.ColorAndOpacity(FSailSimStyle::Accent)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							BuildPerfEngineList()
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("SAILSIM GT")))
							.Font(FSailSimStyle::FontSection())
							.ColorAndOpacity(FSailSimStyle::Accent)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							BuildPerfBucketList()
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							BuildPerfRow(TEXT("OtherGT"), PerfOtherGtLabel)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
						[
							SAssignNew(PerfResidentLabel, STextBlock)
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextDim)
							.AutoWrapText(true)
							.Text(FText::FromString(TEXT("Resident: —")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("RENDER DRIVERS")))
							.Font(FSailSimStyle::FontSection())
							.ColorAndOpacity(FSailSimStyle::Accent)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
						[
							SAssignNew(PerfRenderDriversLabel, STextBlock)
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextDim)
							.AutoWrapText(true)
							.Text(FText::FromString(TEXT("…")))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
						[
							SAssignNew(PerfHintLabel, STextBlock)
							.Font(FSailSimStyle::FontMonoSm())
							.ColorAndOpacity(FSailSimStyle::TextMute)
							.AutoWrapText(true)
							.Text(FText::FromString(
								TEXT("GTwait high → GPU/render bound. GT high → SailSim rows. ")
								TEXT("Peaks reset when Settings re-opens.")))
						])
				]

				// ---- Keys ----
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
				[
					MakeSectionCard(TEXT("CONTROLS"),
						SAssignNew(SettingsBody, STextBlock)
						.Font(FSailSimStyle::FontBody())
						.ColorAndOpacity(FSailSimStyle::TextDim)
						.AutoWrapText(true)
						.Text(FText::FromString(
							TEXT("MAIN TRIM (helm bar)\n")
							TEXT("  Outhaul · Vang  ·  O/P  V/B keys\n\n")
							TEXT("TRIM (hold)\n")
							TEXT("  W/S  sheet in/out\n")
							TEXT("  U/I  jib car fwd/aft\n")
							TEXT("  N/M  jib leech ease/tight\n\n")
							TEXT("BOAT\n")
							TEXT("  A/D helm  ·  T sailing  ·  1–4 preset\n")
							TEXT("  −/= LOA  ·  R re-loft  ·  RMB orbit\n\n")
							TEXT("AUTOPILOT (bottom-right)\n")
							TEXT("  HDG / AWA / Nav  ·  AUTO  ·  ±1 ±10\n")
							TEXT("  Tack  ·  Trim (sheet auto)\n\n")
							TEXT("Prefs auto-save to Saved/Config/"))))
				]
			]
		];
}

void SSailSimChrome::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	{
		SAIL_PERF_SCOPE(UI);
		RefreshFromBoat();
	}
	RefreshPerfPanel(InDeltaTime);
	if (bPrefsDirty && FPlatformTime::Seconds() >= NextPrefsSaveTime)
	{
		FlushPrefsSave();
	}
}

TSharedRef<SWidget> SSailSimChrome::BuildPerfEngineList()
{
	// Fixed engine unit rows (not sorted — order is diagnostic hierarchy).
	static const TCHAR* Names[] = {
		TEXT("GameThrd"), TEXT("GTwait"), TEXT("Render"), TEXT("RHI"), TEXT("GPU"), TEXT("Swap")
	};
	PerfEngineLabels.Reset();
	PerfEngineLabels.SetNum(UE_ARRAY_COUNT(Names));
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I)
	{
		TSharedPtr<STextBlock> Lbl;
		Box->AddSlot()
			.AutoHeight()
			.Padding(0.f, 3.f, 0.f, 0.f)
			[
				BuildPerfRow(Names[I], Lbl)
			];
		PerfEngineLabels[I] = Lbl;
	}
	return Box;
}

TSharedRef<SWidget> SSailSimChrome::BuildPerfBucketList()
{
	PerfBucketLabels.Reset();
	PerfBucketLabels.SetNum(FSailSimPerf::NumBuckets);
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (int32 I = 0; I < FSailSimPerf::NumBuckets; ++I)
	{
		TSharedPtr<STextBlock> Lbl;
		Box->AddSlot()
			.AutoHeight()
			.Padding(0.f, 3.f, 0.f, 0.f)
			[
				BuildPerfRow(TEXT("…"), Lbl)
			];
		PerfBucketLabels[I] = Lbl;
	}
	return Box;
}

TSharedRef<SWidget> SSailSimChrome::BuildPerfRow(const FString& Name, TSharedPtr<STextBlock>& OutLabel)
{
	// Name empty → single full-width value line (used for sorted bucket rows).
	if (Name.IsEmpty() || Name == TEXT("…"))
	{
		return SAssignNew(OutLabel, STextBlock)
			.Font(FSailSimStyle::FontMonoSm())
			.ColorAndOpacity(FSailSimStyle::TextDim)
			.Text(FText::FromString(TEXT("—")));
	}
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(SBox).WidthOverride(78.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Name))
				.Font(FSailSimStyle::FontSection())
				.ColorAndOpacity(FSailSimStyle::Text)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SAssignNew(OutLabel, STextBlock)
			.Font(FSailSimStyle::FontMonoSm())
			.ColorAndOpacity(FSailSimStyle::TextDim)
			.Text(FText::FromString(TEXT("—")))
		];
}

void SSailSimChrome::RefreshPerfPanel(float DeltaSeconds)
{
	FSailSimPerf& P = FSailSimPerf::Get();
	P.NoteFrameMs(FMath::Max(0.f, DeltaSeconds) * 1000.f);

	// FPS on metrics card (always on) — same source as console "stat fps".
	if (FpsLabel.IsValid())
	{
		const float WallMs = FMath::Max(P.WallFrameEmaMs, 0.1f);
		// Prefer engine GAverageFPS when live; else derive from wall ms.
		const float Fps = (GAverageFPS > 1.f) ? GAverageFPS : (1000.f / WallMs);
		const float ShowMs = (GAverageMS > 0.5f) ? GAverageMS : WallMs;
		FpsLabel->SetText(FText::FromString(FString::Printf(
			TEXT("FPS  %4.0f    %5.1f ms"), Fps, ShowMs)));
		FpsLabel->SetColorAndOpacity(
			Fps >= 45.f ? FSailSimStyle::Stbd
			: (Fps >= 28.f ? FSailSimStyle::Accent : FSailSimStyle::Warn));
	}

	if (!bSettingsOpen) return;

	auto Bar = [](float Ms, float RefMs) -> FString
	{
		const float Pct = (RefMs > 0.5f) ? FMath::Clamp(Ms / RefMs, 0.f, 1.f) : 0.f;
		const int32 N = FMath::Clamp(FMath::RoundToInt(Pct * 14.f), 0, 14);
		FString S;
		S.Reserve(14);
		for (int32 I = 0; I < 14; ++I)
		{
			S.AppendChar(I < N ? TEXT('█') : TEXT('·'));
		}
		return S;
	};

	const float Wall = FMath::Max(P.WallFrameEmaMs, 0.01f);
	const float Gt = FMath::Max(P.GameThreadEmaMs, 0.01f);

	// ---- Headline ----
	if (PerfFrameLabel.IsValid())
	{
		const float Fps = (P.WallFrameEmaMs > 0.1f) ? (1000.f / P.WallFrameEmaMs) : 0.f;
		PerfFrameLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%5.1f ms  %.0f fps   GT busy %.1f  wait %.1f  RT %.1f  RHI %.1f"),
			P.WallFrameEmaMs, Fps,
			P.GameThreadEmaMs, P.GameWaitEmaMs,
			P.RenderThreadEmaMs, P.RhiThreadEmaMs)));
		PerfFrameLabel->SetColorAndOpacity(
			P.WallFrameEmaMs > 22.f ? FSailSimStyle::Warn : FSailSimStyle::Stbd);
	}
	if (PerfBottleneckLabel.IsValid())
	{
		PerfBottleneckLabel->SetText(FText::FromString(
			FString::Printf(TEXT("BOTTLENECK → %s"), *P.BottleneckLabel)));
		const bool bWarn = P.Bottleneck != FSailSimPerf::EBottleneck::Balanced
			&& P.Bottleneck != FSailSimPerf::EBottleneck::Unknown;
		PerfBottleneckLabel->SetColorAndOpacity(
			bWarn ? FSailSimStyle::Warn : FSailSimStyle::Stbd);
	}

	// ---- Engine unit rows (of wall frame) ----
	// Indices match BuildPerfEngineList order.
	struct FEngRow { float Ms; const TCHAR* Hint; };
	const FEngRow Eng[] = {
		{ P.GameThreadEmaMs, TEXT("busy work on game thread") },
		{ P.GameWaitEmaMs, TEXT("GT idle/blocked (GPU, RT, vsync)") },
		{ P.RenderThreadEmaMs, TEXT("scene draw, water, Lumen…") },
		{ P.RhiThreadEmaMs, TEXT("GPU command submit") },
		{ P.GpuFrameEmaMs, TEXT("GPU timestamps (0 = unavailable)") },
		{ P.SwapBufferEmaMs, TEXT("present / swap") },
	};
	for (int32 I = 0; I < PerfEngineLabels.Num() && I < UE_ARRAY_COUNT(Eng); ++I)
	{
		const TSharedPtr<STextBlock>& L = PerfEngineLabels[I];
		if (!L.IsValid()) continue;
		const float E = Eng[I].Ms;
		const float Pct = 100.f * E / Wall;
		L->SetText(FText::FromString(FString::Printf(
			TEXT("%s  %5.1f ms  %4.0f%% of wall  %s"),
			*Bar(E, Wall), E, Pct, Eng[I].Hint)));
		L->SetColorAndOpacity(E > Wall * 0.40f ? FSailSimStyle::Warn
			: (E > Wall * 0.20f ? FSailSimStyle::Accent : FSailSimStyle::TextDim));
	}

	// ---- SailSim scopes as % of game-thread BUSY time (not wall) ----
	TArray<int32> Order;
	Order.Reserve(FSailSimPerf::NumBuckets);
	for (int32 I = 0; I < FSailSimPerf::NumBuckets; ++I) Order.Add(I);
	Order.Sort([&](int32 A, int32 B)
	{
		if (P.EmaMs[A] != P.EmaMs[B]) return P.EmaMs[A] > P.EmaMs[B];
		return A < B;
	});

	for (int32 Slot = 0; Slot < PerfBucketLabels.Num() && Slot < Order.Num(); ++Slot)
	{
		const TSharedPtr<STextBlock>& L = PerfBucketLabels[Slot];
		if (!L.IsValid()) continue;
		const int32 Bi = Order[Slot];
		const auto B = static_cast<FSailSimPerf::EBucket>(Bi);
		const float E = P.EmaMs[Bi];
		const float Peak = P.PeakMs[Bi];
		const float PctGt = 100.f * E / Gt;
		L->SetText(FText::FromString(FString::Printf(
			TEXT("%-9s %s %5.2fms %4.0f%%GT pk%.1f  %s"),
			FSailSimPerf::BucketName(B),
			*Bar(E, Gt), E, PctGt, Peak,
			FSailSimPerf::BucketHint(B))));
		L->SetColorAndOpacity(E > Gt * 0.25f ? FSailSimStyle::Warn
			: (E > Gt * 0.10f ? FSailSimStyle::Accent : FSailSimStyle::TextDim));
	}

	if (PerfOtherGtLabel.IsValid())
	{
		const float Other = P.OtherGameThreadEmaMs();
		const float Pct = 100.f * Other / Gt;
		PerfOtherGtLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%s  %5.2f ms  %4.0f%%GT  engine actors, Slate, physics, unscoped"),
			*Bar(Other, Gt), Other, Pct)));
		PerfOtherGtLabel->SetColorAndOpacity(
			Other > Gt * 0.5f ? FSailSimStyle::Warn
			: (Other > Gt * 0.25f ? FSailSimStyle::Accent : FSailSimStyle::TextDim));
	}

	if (PerfResidentLabel.IsValid())
	{
		const float Known = P.TotalKnownEmaMs();
		PerfResidentLabel->SetText(FText::FromString(FString::Printf(
			TEXT("Resident  terrain %d / %dkv   houses %d (sm=%d pmc=%d) / %dkv   moored %d   aids %d   puffs %d\n")
			TEXT("SailSim scopes %.2f ms of GT busy %.1f ms (%.0f%%) · GTwait %.1f ms is NOT CPU work"),
			P.TerrainTiles, P.TerrainVerts / 1000,
			P.StructureTiles, P.StructureSmTiles, P.StructurePmcTiles, P.StructureVerts / 1000,
			P.MooredCount, P.AidCount, P.WindPuffs,
			Known, P.GameThreadEmaMs,
			100.f * Known / Gt,
			P.GameWaitEmaMs)));
	}

	// Live cvars + project-ranked RT/GPU suspects (for isolation tests).
	if (PerfRenderDriversLabel.IsValid())
	{
		auto CVarInt = [](const TCHAR* Name, int32 Fallback) -> int32
		{
			if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				return C->GetInt();
			}
			return Fallback;
		};
		auto CVarFloat = [](const TCHAR* Name, float Fallback) -> float
		{
			if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				return C->GetFloat();
			}
			return Fallback;
		};

		const int32 LumenGI = CVarInt(TEXT("r.DynamicGlobalIlluminationMethod"), 1);
		const int32 LumenRefl = CVarInt(TEXT("r.Lumen.Reflections.Allow"), 1);
		const int32 LumenReflDS = CVarInt(TEXT("r.Lumen.Reflections.DownsampleFactor"), 2);
		const int32 Vsm = CVarInt(TEXT("r.Shadow.Virtual.Enable"), 1);
		const int32 RT = CVarInt(TEXT("r.RayTracing"), 0);
		const int32 Clouds = CVarInt(TEXT("r.VolumetricCloud"), 1);
		const int32 Substrate = CVarInt(TEXT("r.Substrate"), 1);
		const int32 SSR = CVarInt(TEXT("r.SSR.Quality"), 3);
		const float ShadowDist = CVarFloat(TEXT("r.Shadow.DistanceScale"), 1.f);
		const float Cloud01 = CachedCloudIntensity;

		const float SailsGt = P.EmaMs[static_cast<int32>(FSailSimPerf::EBucket::Sails)];
		const float OceanGt = P.EmaMs[static_cast<int32>(FSailSimPerf::EBucket::Ocean)];

		const int32 LumenSPG = CVarInt(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), 16);
		PerfRenderDriversLabel->SetText(FText::FromString(FString::Printf(
			TEXT("Live cvars  LumenGI=%d  ReflDS=%d  SPGds=%d  VSM=%d  RT=%d  Clouds=%d(i=%.2f)  Substrate=%d\n")
			TEXT("Applied: SPG=16 (Epic quality) · RT off · waterTess 600m · moored cap 16 · struct Lod0=center\n")
			TEXT("1 Lumen GI+refl   SPG ds=%d  refl ds=%d  RT=%d\n")
			TEXT("2 Vol clouds      TraceMax 50km  (slider i=%.2f)\n")
			TEXT("3 Content arch    houses PMC→cook Nanite · aids cull · no moored point lights\n")
			TEXT("4 Sails           UpdateMesh only · no cast shadow\n")
			TEXT("GT sails %.2fms ocean %.2fms · moored %d  houses sm=%d pmc=%d"),
			LumenGI, LumenReflDS, LumenSPG, Vsm, RT, Clouds, Cloud01, Substrate,
			LumenSPG, LumenReflDS, RT, Cloud01, SailsGt, OceanGt, P.MooredCount,
			P.StructureSmTiles, P.StructurePmcTiles)));
	}

	if (PerfHintLabel.IsValid() && !P.AdviceLabel.IsEmpty())
	{
		// Append isolation recipe when render-bound.
		FString Hint = P.AdviceLabel;
		if (P.Bottleneck == FSailSimPerf::EBottleneck::GPU
			|| P.Bottleneck == FSailSimPerf::EBottleneck::RenderThread
			|| P.Bottleneck == FSailSimPerf::EBottleneck::GameWait)
		{
			Hint += TEXT("\nIsolate (one at a time, watch FPS):  ")
				TEXT("r.VolumetricCloud 0  ·  r.Lumen.Reflections.Allow 0  ·  ")
				TEXT("r.DynamicGlobalIlluminationMethod 0  ·  r.Shadow.Virtual.Enable 0  ·  ")
				TEXT("ProfileGPU / stat gpu");
		}
		PerfHintLabel->SetText(FText::FromString(Hint));
	}

	// Periodic log so we can troubleshoot without the settings drawer open.
	static float LogAccum = 0.f;
	LogAccum += FMath::Max(0.f, DeltaSeconds);
	if (LogAccum > 3.f)
	{
		LogAccum = 0.f;
		UE_LOG(LogSailSim, Log, TEXT("SailSimPerf: %s"), *P.FormatSummary());
	}
}

void SSailSimChrome::TryApplyUserPrefs()
{
	if (bPrefsApplied) return;
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;

	FSailSimUserPrefs Prefs;
	Prefs.Load();
	Prefs.Apply(B, B->GetWorld());
	CachedSheetEase = Prefs.SheetEase01;
	CachedOuthaul = Prefs.Outhaul01;
	CachedVang = Prefs.Vang01;
	CachedJibCar = Prefs.JibCar01;
	CachedSpinSheet = Prefs.SpinSheet01;
	CachedJibSet = Prefs.bJibSet;
	CachedKiteSet = Prefs.bKiteSet;
	CachedTws = Prefs.TwsKn;
	CachedTwd = Prefs.TwdDeg;
	CachedCloudIntensity = Prefs.Cloud01;
	CachedFogIntensity = Prefs.Fog01;
	CachedTimeOfDay01 = FMath::Clamp(Prefs.TimeOfDayHours / 24.f, 0.f, 1.f);
	CachedSeason01 = FMath::Clamp(Prefs.Season01, 0.f, 1.f);
	bLightsPanelOpen = Prefs.bLightsPanelOpen;
	ApplyMapSize(Prefs.MapPanelW, Prefs.MapPanelH);
	RefreshTimeOfDayLabel();
	RefreshSeasonLabel();
	bPrefsApplied = true;
	// Persist once after restore so a missing file gets created with current values.
	bPrefsDirty = true;
	NextPrefsSaveTime = FPlatformTime::Seconds() + 0.5;
}

void SSailSimChrome::CapturePrefsFromLive()
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;
	FSailSimUserPrefs Prefs;
	// Prefs store DISPLAY knots (same as HUD / SetTrueWind).
	Prefs.TwsKn = B->GetTrueWindSpeedKn();
	Prefs.TwdDeg = B->GetTrueWindDirDeg();
	if (UWorld* World = B->GetWorld())
	{
		if (UWindFieldSubsystem* W = World->GetSubsystem<UWindFieldSubsystem>())
		{
			Prefs.TwsKn = FBoatDynamics::WindDisplayFromPhysics(W->BaseSpeedKn);
			Prefs.TwdDeg = W->BaseDirFromDeg;
		}
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Prefs.Cloud01 = Ocean->GetVolumetricCloudIntensity();
			Prefs.Fog01 = Ocean->GetFogIntensity();
			Prefs.EnvPreset = Ocean->GetActiveEnvPreset();
			Prefs.TimeOfDayHours = Ocean->GetTimeOfDayHours();
			Prefs.Season01 = Ocean->GetSeason01();
			CachedTimeOfDay01 = Ocean->GetTimeOfDay01();
			CachedSeason01 = Ocean->GetSeason01();
		}
	}
	Prefs.SheetEase01 = B->GetSheetEase();
	Prefs.Outhaul01 = B->GetOuthaul();
	Prefs.Vang01 = B->GetVang();
	Prefs.JibCar01 = B->GetJibCar();
	Prefs.SpinSheet01 = B->GetSpinSheetEase();
	Prefs.bJibSet = B->IsJibSet();
	Prefs.bKiteSet = B->IsKiteSet();
	Prefs.MapPanelW = MapPanelW;
	Prefs.MapPanelH = MapPanelH;
	Prefs.bLightsPanelOpen = bLightsPanelOpen;
	// Stash into cached fields for SchedulePrefsSave path
	CachedSheetEase = Prefs.SheetEase01;
	CachedOuthaul = Prefs.Outhaul01;
	CachedVang = Prefs.Vang01;
	CachedJibCar = Prefs.JibCar01;
	CachedSpinSheet = Prefs.SpinSheet01;
	CachedJibSet = Prefs.bJibSet;
	CachedKiteSet = Prefs.bKiteSet;
	CachedTws = Prefs.TwsKn;
	CachedTwd = Prefs.TwdDeg;
	CachedCloudIntensity = Prefs.Cloud01;
	CachedFogIntensity = Prefs.Fog01;
}

void SSailSimChrome::SchedulePrefsSave()
{
	// Only persist after we have restored once — otherwise first-frame defaults
	// could overwrite a good disk file before TryApplyUserPrefs runs.
	if (!bPrefsApplied) return;
	bPrefsDirty = true;
	// Debounce disk writes while dragging sliders
	NextPrefsSaveTime = FPlatformTime::Seconds() + 0.35;
}

void SSailSimChrome::ForceSavePrefsNow()
{
	if (!bPrefsApplied && !bPrefsDirty) return;
	FlushPrefsSave();
}

void SSailSimChrome::FlushPrefsSave()
{
	bPrefsDirty = false;
	ASailBoatPawn* B = Boat.Get();
	FSailSimUserPrefs Prefs;
	// Prefer live boat + subsystems; fall back to cached chrome fields.
	Prefs.TwsKn = CachedTws;
	Prefs.TwdDeg = CachedTwd;
	Prefs.SheetEase01 = CachedSheetEase;
	Prefs.Outhaul01 = CachedOuthaul;
	Prefs.Vang01 = CachedVang;
	Prefs.JibCar01 = CachedJibCar;
	Prefs.SpinSheet01 = CachedSpinSheet;
	Prefs.bJibSet = CachedJibSet;
	Prefs.bKiteSet = CachedKiteSet;
	Prefs.Cloud01 = CachedCloudIntensity;
	Prefs.Fog01 = CachedFogIntensity;
	Prefs.TimeOfDayHours = CachedTimeOfDay01 * 24.f;
	Prefs.Season01 = CachedSeason01;
	if (B)
	{
		Prefs.SheetEase01 = B->GetSheetEase();
		Prefs.Outhaul01 = B->GetOuthaul();
		Prefs.Vang01 = B->GetVang();
		Prefs.JibCar01 = B->GetJibCar();
		Prefs.SpinSheet01 = B->GetSpinSheetEase();
		Prefs.bJibSet = B->IsJibSet();
		Prefs.bKiteSet = B->IsKiteSet();
		// Display-scale wind from the boat API (not physics raw).
		Prefs.TwsKn = B->GetTrueWindSpeedKn();
		Prefs.TwdDeg = B->GetTrueWindDirDeg();
		if (UWorld* World = B->GetWorld())
		{
			if (UWindFieldSubsystem* W = World->GetSubsystem<UWindFieldSubsystem>())
			{
				Prefs.TwsKn = FBoatDynamics::WindDisplayFromPhysics(W->BaseSpeedKn);
				Prefs.TwdDeg = W->BaseDirFromDeg;
			}
			if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
			{
				Prefs.Cloud01 = Ocean->GetVolumetricCloudIntensity();
				Prefs.Fog01 = Ocean->GetFogIntensity();
				Prefs.EnvPreset = Ocean->GetActiveEnvPreset();
				Prefs.TimeOfDayHours = Ocean->GetTimeOfDayHours();
				Prefs.Season01 = Ocean->GetSeason01();
			}
		}
	}
	Prefs.MapPanelW = MapPanelW;
	Prefs.MapPanelH = MapPanelH;
	Prefs.bLightsPanelOpen = bLightsPanelOpen;
	Prefs.Save();
}

void SSailSimChrome::RefreshFromBoat()
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;

	// First frame with a live boat: restore prefs from disk
	TryApplyUserPrefs();

	CachedHeading = B->GetHeadingDeg();
	CachedTwd = B->GetTrueWindDirDeg();
	CachedHeel = B->GetHeelDeg();
	CachedPitch = B->GetPitchDeg();
	CachedRudder = B->GetRudderStarboardDeg();
	CachedAutoTarget = B->GetAutoTargetHeading();
	CachedAutoAwaTarget = B->GetAutoAwaTarget();
	bCachedAuto = B->IsAutoHeading();
	bCachedAutoTrim = B->IsAutoTrim();
	CachedAutoMode = B->GetAutoMode();
	CachedLoc = B->GetActorLocation();
	CachedSog = B->GetSpeedKnots();
	CachedAwa = B->GetApparentWindAngleDeg();
	// AWS/TWS already on the display scale (pawn getters convert)
	CachedAws = B->GetApparentWindSpeedKn();
	CachedTws = B->GetTrueWindSpeedKn();
	bCachedSailing = B->IsSailing();
	// Keyboard / auto-trim can move sail controls — mark dirty so they persist.
	const float LiveSheet = B->GetSheetEase();
	const float LiveOut = B->GetOuthaul();
	const float LiveVang = B->GetVang();
	const float LiveCar = B->GetJibCar();
	const float LiveSpin = B->GetSpinSheetEase();
	const bool LiveJib = B->IsJibSet();
	const bool LiveKite = B->IsKiteSet();
	if (FMath::Abs(LiveSheet - CachedSheetEase) > 0.002f
		|| FMath::Abs(LiveOut - CachedOuthaul) > 0.002f
		|| FMath::Abs(LiveVang - CachedVang) > 0.002f
		|| FMath::Abs(LiveCar - CachedJibCar) > 0.002f
		|| FMath::Abs(LiveSpin - CachedSpinSheet) > 0.002f
		|| LiveJib != CachedJibSet
		|| LiveKite != CachedKiteSet)
	{
		SchedulePrefsSave();
	}
	CachedSheetEase = LiveSheet;
	CachedOuthaul = LiveOut;
	CachedVang = LiveVang;
	CachedJibCar = LiveCar;
	CachedSpinSheet = LiveSpin;
	CachedJibSet = LiveJib;
	CachedKiteSet = LiveKite;
	if (CompactSheetLabel.IsValid())
	{
		CompactSheetLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%"), CachedSheetEase * 100.f)));
	}
	if (CompactOuthaulLabel.IsValid())
	{
		CompactOuthaulLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%"), CachedOuthaul * 100.f)));
	}
	if (CompactVangLabel.IsValid())
	{
		// Display as "hard" at 0 and "ease" toward 100 (matches control sense)
		CompactVangLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%"), CachedVang * 100.f)));
	}
	if (CompactJibCarLabel.IsValid())
	{
		CompactJibCarLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%"), CachedJibCar * 100.f)));
	}
	if (CompactSpinSheetLabel.IsValid())
	{
		CompactSpinSheetLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%"), CachedSpinSheet * 100.f)));
	}
	if (OuthaulSliderLabel.IsValid())
	{
		const float FootPct = (0.70f + 0.30f * CachedOuthaul) * 100.f;
		OuthaulSliderLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%  (foot %.0f%% E)"), CachedOuthaul * 100.f, FootPct)));
	}
	if (VangSliderLabel.IsValid())
	{
		VangSliderLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%  (%s)"), CachedVang * 100.f,
			CachedVang < 0.25f ? TEXT("hard") : (CachedVang > 0.75f ? TEXT("eased") : TEXT("set")))));
	}

	// World cm → Nantucket Sound lat/lon (chart 13237)
	FNavGeo::WorldCmToLatLon(CachedLoc.X, CachedLoc.Y, CachedLat, CachedLon);

	if (BoatChip.IsValid())
	{
		BoatChip->SetText(FText::FromString(B->GetSpecSummary()));
	}

	const TCHAR* Tack = (CachedAwa >= 0.f) ? TEXT("STBD TACK") : TEXT("PORT TACK");
	const float Luff = B->GetMainLuffAmount();

	if (StatusLine1.IsValid())
	{
		StatusLine1->SetText(FText::FromString(FString::Printf(
			TEXT("SOG  %5.1f kn    HDG  %5.1f°    HEEL  %+5.1f°    PITCH  %+4.1f°"),
			CachedSog, CachedHeading, CachedHeel, CachedPitch)));
	}
	if (StatusLine2.IsValid())
	{
		StatusLine2->SetText(FText::FromString(FString::Printf(
			TEXT("AWA  %+5.0f°     AWS  %5.1f kn    TWS  %4.0f kn    TWD  %5.0f°"),
			CachedAwa, CachedAws, CachedTws, CachedTwd)));
	}
	if (StatusLine3.IsValid())
	{
		StatusLine3->SetText(FText::FromString(FString::Printf(
			TEXT("%s  RUD%+4.0f°  SHEET %2.0f%%  OUT %2.0f%%  VANG %2.0f%%  SPIN %2.0f%%  %s"),
			Tack, CachedRudder, B->GetSheetEase() * 100.f,
			B->GetOuthaul() * 100.f, B->GetVang() * 100.f,
			B->GetSpinDeploy01() * 100.f,
			bCachedAuto ? TEXT("AUTO") : TEXT("HELM"))));
		StatusLine3->SetColorAndOpacity(CachedAwa >= 0.f ? FSailSimStyle::Stbd : FSailSimStyle::Port);
	}
	if (StatusLine4.IsValid())
	{
		const float WindPush = B->GetMainWindPushCm();
		const float FT = 30.48f;
		// Live vs loft edge lengths (ft) — should stay near class E / leech path
		StatusLine4->SetText(FText::FromString(FString::Printf(
			TEXT("EDGES  main foot %.1f'/%.1f'  leech %.1f'/%.1f'  jib foot %.1f'/%.1f'  leech %.1f'/%.1f'"),
			B->GetMainLiveFootCm() / FT, B->GetMainLoftFootCm() / FT,
			B->GetMainLiveLeechCm() / FT, B->GetMainLoftLeechCm() / FT,
			B->GetJibLiveFootCm() / FT, B->GetJibLoftFootCm() / FT,
			B->GetJibLiveLeechCm() / FT, B->GetJibLoftLeechCm() / FT)));
		const bool bEdgesOk =
			B->GetMainLiveFootCm() > 1.f
			&& B->GetMainLiveFootCm() < B->GetMainLoftFootCm() * 1.08f
			&& B->GetMainLiveLeechCm() < B->GetMainLoftLeechCm() * 1.10f;
		StatusLine4->SetColorAndOpacity(bEdgesOk ? FSailSimStyle::Stbd : FSailSimStyle::Warn);
	}
	if (StatusLine5.IsValid())
	{
		const float WindPush = B->GetMainWindPushCm();
		FString Terr = TEXT("terrain —");
		FString Struct = TEXT("struct —");
		FString Aids = TEXT("aids —");
		FString Moored = TEXT("moored —");
		FString Wind = TEXT("wind —");
		if (UWorld* World = B->GetWorld())
		{
			if (UNantucketTerrainSubsystem* T = World->GetSubsystem<UNantucketTerrainSubsystem>())
			{
				Terr = T->GetStatusLine();
			}
			if (UNantucketStructuresSubsystem* S = World->GetSubsystem<UNantucketStructuresSubsystem>())
			{
				Struct = S->GetStatusLine();
			}
			if (UEncAidSubsystem* E = World->GetSubsystem<UEncAidSubsystem>())
			{
				Aids = E->GetStatusLine();
			}
			if (UMooredBoatSubsystem* M = World->GetSubsystem<UMooredBoatSubsystem>())
			{
				Moored = M->GetStatusLine();
			}
			if (UWindFieldSubsystem* W = World->GetSubsystem<UWindFieldSubsystem>())
			{
				Wind = W->GetStatusLine();
			}
		}
		StatusLine5->SetText(FText::FromString(FString::Printf(
			TEXT("CLOTH windΔ %.1fcm  luff %.0f%%  ·  %s  ·  %s  ·  %s  ·  %s  ·  %s"),
			WindPush, Luff * 100.f, *Terr, *Struct, *Aids, *Moored, *Wind)));
		StatusLine5->SetColorAndOpacity(WindPush > 0.5f ? FSailSimStyle::Stbd : FSailSimStyle::Warn);
	}

	if (HelmReadout.IsValid())
	{
		FString T;
		if (FMath::Abs(CachedRudder) < 0.5f) T = TEXT("Helm  0°");
		else T = FString::Printf(TEXT("Helm  %.0f° %s"), FMath::Abs(CachedRudder),
			CachedRudder < 0.f ? TEXT("P") : TEXT("S"));
		HelmReadout->SetText(FText::FromString(T));
		HelmReadout->SetColorAndOpacity(
			CachedRudder < -0.5f ? FSailSimStyle::Port
			: CachedRudder > 0.5f ? FSailSimStyle::Stbd
			: FSailSimStyle::Text);
	}
	RefreshAutopilotPanel();
	RefreshWaypointPanel();

	// Chart overlays — match web dist formatting
	if (MapTime.IsValid())
	{
		// America/New_York-ish display: "Sat, Jul 12, 1:05 AM EDT"
		// Slate has no TZ DB; use local time with 12h clock + short weekday/month (close to en-US).
		const FDateTime Now = FDateTime::Now();
		const FString AmPm = (Now.GetHour() >= 12) ? TEXT("PM") : TEXT("AM");
		int32 H12 = Now.GetHour() % 12;
		if (H12 == 0) H12 = 12;
		static const TCHAR* WDays[] = {
			TEXT("Mon"), TEXT("Tue"), TEXT("Wed"), TEXT("Thu"), TEXT("Fri"), TEXT("Sat"), TEXT("Sun")
		};
		static const TCHAR* Months[] = {
			TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
			TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec")
		};
		// EDayOfWeek: Monday=0 … Sunday=6
		const int32 Wdi = FMath::Clamp(static_cast<int32>(Now.GetDayOfWeek()), 0, 6);
		const int32 Mi = FMath::Clamp(Now.GetMonth() - 1, 0, 11);
		MapTime->SetText(FText::FromString(FString::Printf(
			TEXT("%s, %s %d, %d:%02d %s"),
			WDays[Wdi], Months[Mi], Now.GetDay(), H12, Now.GetMinute(), *AmPm)));
	}
	if (MapCoords.IsValid())
	{
		MapCoords->SetText(FText::FromString(
			FNavGeo::FormatLat(CachedLat) + TEXT("  ") + FNavGeo::FormatLon(CachedLon)));
	}
	if (MapPlace.IsValid())
	{
		// web navFormatPlace → "City, ST" (Nantucket harbor area)
		MapPlace->SetText(FText::FromString(TEXT("Nantucket, MA")));
	}

	// Telemetry pills (web NAV_MINIMAP_TELEMETRY strings)
	const EVisibility SailVis = bCachedSailing ? EVisibility::Visible : EVisibility::Collapsed;
	if (PillSogWrap.IsValid()) PillSogWrap->SetVisibility(SailVis);
	if (PillAwaWrap.IsValid()) PillAwaWrap->SetVisibility(SailVis);
	if (PillAwsWrap.IsValid()) PillAwsWrap->SetVisibility(SailVis);
	if (PillHelmWrap.IsValid()) PillHelmWrap->SetVisibility(SailVis);

	if (PillSog.IsValid())
	{
		PillSog->SetText(FText::FromString(FString::Printf(TEXT("SOG %.1f kt"), CachedSog)));
	}
	if (PillTws.IsValid())
	{
		PillTws->SetText(FText::FromString(FString::Printf(TEXT("TWS %.0f kt"), CachedTws)));
	}
	if (PillTwd.IsValid())
	{
		const float TwdShow = FMath::Fmod(CachedTwd + 360.f, 360.f);
		PillTwd->SetText(FText::FromString(FString::Printf(TEXT("TWD %.0f°"), TwdShow)));
	}
	if (PillAwa.IsValid())
	{
		// web: 'AWA ' + Math.round(n.awa) + '°'  (no forced +)
		PillAwa->SetText(FText::FromString(FString::Printf(TEXT("AWA %.0f°"), CachedAwa)));
	}
	if (PillAws.IsValid())
	{
		PillAws->SetText(FText::FromString(FString::Printf(TEXT("AWS %.0f kt"), CachedAws)));
	}
	if (PillHelm.IsValid())
	{
		FString H;
		if (FMath::Abs(CachedRudder) < 0.5f) H = TEXT("Helm 0°");
		else H = FString::Printf(TEXT("Helm %.0f° %s"), FMath::Abs(CachedRudder),
			CachedRudder < 0.f ? TEXT("P") : TEXT("S"));
		if (bCachedAuto) H += TEXT(" AUTO");
		PillHelm->SetText(FText::FromString(H));
	}

	if (CompactTwsLabel.IsValid())
	{
		CompactTwsLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f kn"), CachedTws)));
	}
	if (TwsSliderLabel.IsValid())
	{
		TwsSliderLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f kn"), CachedTws)));
	}
	if (TwdSliderLabel.IsValid())
	{
		const float TwdShow = FMath::Fmod(CachedTwd + 360.f, 360.f);
		TwdSliderLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f°"), TwdShow)));
	}
	// Sync fog / cloud intensity from ocean subsystem (and labels).
	if (UWorld* World = B->GetWorld())
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			CachedCloudIntensity = Ocean->GetVolumetricCloudIntensity();
			CachedFogIntensity = Ocean->GetFogIntensity();
		}
	}
	auto FmtPctOff = [](float V) -> FText
	{
		if (V <= KINDA_SMALL_NUMBER) return FText::FromString(TEXT("OFF"));
		return FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f));
	};
	if (FogSliderLabel.IsValid())
	{
		FogSliderLabel->SetText(FmtPctOff(CachedFogIntensity));
	}
	if (CloudSliderLabel.IsValid())
	{
		CloudSliderLabel->SetText(FmtPctOff(CachedCloudIntensity));
	}
}

void SSailSimChrome::ToggleSettings()
{
	bSettingsOpen = !bSettingsOpen;
	if (bSettingsOpen)
	{
		FSailSimPerf::Get().ResetPeaks();
	}
}

void SSailSimChrome::OnTwsSlider(float Norm01)
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;
	// Slider 0..1 → display knots 0..40 (calibrated feel scale, not raw physics).
	const float Kn = FMath::Clamp(Norm01, 0.f, 1.f) * FBoatDynamics::WindDisplayMaxKn;
	// Use base TWD (not local puff sample) so sliders control the synoptic field.
	float Dir = B->GetTrueWindDirDeg();
	if (UWorld* World = B->GetWorld())
	{
		if (UWindFieldSubsystem* W = World->GetSubsystem<UWindFieldSubsystem>())
		{
			Dir = W->BaseDirFromDeg;
		}
	}
	B->SetTrueWind(Kn, Dir);
	CachedTws = Kn;
	if (CompactTwsLabel.IsValid())
	{
		CompactTwsLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f kn"), Kn)));
	}
	if (TwsSliderLabel.IsValid())
	{
		TwsSliderLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f kn"), Kn)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnTwdSlider(float Norm01)
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;
	const float Deg = FMath::Clamp(Norm01, 0.f, 1.f) * 360.f;
	// Keep display TWS when only TWD moves (BaseSpeedKn is physics).
	float Kn = B->GetTrueWindSpeedKn();
	if (UWorld* World = B->GetWorld())
	{
		if (UWindFieldSubsystem* W = World->GetSubsystem<UWindFieldSubsystem>())
		{
			Kn = FBoatDynamics::WindDisplayFromPhysics(W->BaseSpeedKn);
		}
	}
	B->SetTrueWind(Kn, Deg);
	CachedTwd = Deg;
	if (TwdSliderLabel.IsValid())
	{
		TwdSliderLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f°"), Deg)));
	}
	SchedulePrefsSave();
}

static UWorld* SailSimResolveGameWorld(ASailBoatPawn* Boat)
{
	if (Boat && Boat->GetWorld()) return Boat->GetWorld();
	if (!GEngine) return nullptr;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
		{
			return Ctx.World();
		}
	}
	return nullptr;
}

TSharedRef<SWidget> SSailSimChrome::BuildEnvPresetRow()
{
	// Wrap chips in a wrap-box style vertical stack of horizontal rows
	TSharedRef<SVerticalBox> Cols = SNew(SVerticalBox);
	static const TCHAR* Names[] = {
		TEXT("Fair Day"), TEXT("Golden"), TEXT("Dusk"), TEXT("Night"),
		TEXT("Overcast"), TEXT("Storm"), TEXT("Fog")
	};
	constexpr int32 N = 7;
	TSharedPtr<SHorizontalBox> Row;
	for (int32 I = 0; I < N; ++I)
	{
		if (I % 3 == 0)
		{
			Row = SNew(SHorizontalBox);
			Cols->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)[Row.ToSharedRef()];
		}
		const uint8 Id = static_cast<uint8>(I);
		Row->AddSlot()
			.FillWidth(1.f)
			.Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), "NoBorder")
				.ContentPadding(0.f)
				.OnClicked_Lambda([this, Id]()
				{
					OnEnvPreset(Id);
					return FReply::Handled();
				})
				[
					SNew(SBorder)
					.BorderImage_Lambda([this, Id]() -> const FSlateBrush*
					{
						ASailBoatPawn* B = Boat.Get();
						(void)B;
						uint8 Active = 0;
						if (UWorld* World = B && B->GetWorld() ? B->GetWorld() : nullptr)
						{
							if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
							{
								Active = Ocean->GetActiveEnvPreset();
							}
						}
						return Active == Id
							? FSailSimStyle::ButtonBrushActive()
							: FSailSimStyle::ButtonBrush();
					})
					.Padding(FMargin(6.f, 7.f))
					.HAlign(HAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Names[I]))
						.Font(FSailSimStyle::FontLabel())
						.ColorAndOpacity(FSailSimStyle::Text)
						.Justification(ETextJustify::Center)
					]
				]
			];
	}
	return Cols;
}

void SSailSimChrome::OnEnvPreset(uint8 PresetId)
{
	UWorld* World = nullptr;
	if (ASailBoatPawn* B = Boat.Get())
	{
		World = B->GetWorld();
	}
	if (!World) return;

	if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const float WindKn = Ocean->ApplyEnvPreset(PresetId);
		CachedCloudIntensity = Ocean->GetVolumetricCloudIntensity();
		CachedFogIntensity = Ocean->GetFogIntensity();
		if (CloudSliderLabel.IsValid())
		{
			CloudSliderLabel->SetText(FText::FromString(
				FString::Printf(TEXT("%d%%"), FMath::RoundToInt(CachedCloudIntensity * 100.f))));
		}
		if (FogSliderLabel.IsValid())
		{
			FogSliderLabel->SetText(FText::FromString(
				FString::Printf(TEXT("%d%%"), FMath::RoundToInt(CachedFogIntensity * 100.f))));
		}
		if (WindKn >= 0.f)
		{
			if (ASailBoatPawn* B = Boat.Get())
			{
				B->SetTrueWind(WindKn, B->GetTrueWindDirDeg());
				CachedTws = WindKn;
			}
		}
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnCloudSlider(float Norm01)
{
	// Allow true zero — full off for testing.
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedCloudIntensity = V;
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Ocean->SetVolumetricCloudIntensity(V);
		}
	}
	if (CloudSliderLabel.IsValid())
	{
		CloudSliderLabel->SetText(V <= KINDA_SMALL_NUMBER
			? FText::FromString(TEXT("OFF"))
			: FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnFogSlider(float Norm01)
{
	// Allow true zero — full off for testing.
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedFogIntensity = V;
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Ocean->SetFogIntensity(V);
		}
	}
	if (FogSliderLabel.IsValid())
	{
		FogSliderLabel->SetText(V <= KINDA_SMALL_NUMBER
			? FText::FromString(TEXT("OFF"))
			: FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnTimeOfDaySlider(float Norm01)
{
	CachedTimeOfDay01 = FMath::Clamp(Norm01, 0.f, 1.f);
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Ocean->SetTimeOfDay01(CachedTimeOfDay01);
			// Keep user fog/cloud after TOD (TOD does not overwrite them; reassert for safety).
			Ocean->SetVolumetricCloudIntensity(CachedCloudIntensity);
			Ocean->SetFogIntensity(CachedFogIntensity);
		}
	}
	RefreshTimeOfDayLabel();
	SchedulePrefsSave();
}

void SSailSimChrome::RefreshTimeOfDayLabel()
{
	if (!TimeOfDayLabel.IsValid()) return;
	FString Label = TEXT("12:00  Fair Day");
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Label = Ocean->GetTimeOfDayLabel();
			CachedTimeOfDay01 = Ocean->GetTimeOfDay01();
		}
	}
	TimeOfDayLabel->SetText(FText::FromString(Label));
}

void SSailSimChrome::OnSeasonSlider(float Norm01)
{
	CachedSeason01 = FMath::Clamp(Norm01, 0.f, 1.f);
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Ocean->SetSeason01(CachedSeason01);
		}
	}
	RefreshSeasonLabel();
	SchedulePrefsSave();
}

void SSailSimChrome::RefreshSeasonLabel()
{
	if (!SeasonLabel.IsValid()) return;
	FString Label = TEXT("Summer");
	if (UWorld* World = SailSimResolveGameWorld(Boat.Get()))
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Label = Ocean->GetSeasonLabel();
			CachedSeason01 = Ocean->GetSeason01();
		}
	}
	else
	{
		// Offline label from cache only
		const float S = CachedSeason01;
		if (S < 0.0625f || S >= 0.9375f) Label = TEXT("Winter");
		else if (S < 0.1875f) Label = TEXT("Late Winter");
		else if (S < 0.3125f) Label = TEXT("Spring");
		else if (S < 0.4375f) Label = TEXT("Late Spring");
		else if (S < 0.5625f) Label = TEXT("Summer");
		else if (S < 0.6875f) Label = TEXT("Late Summer");
		else if (S < 0.8125f) Label = TEXT("Autumn");
		else Label = TEXT("Late Autumn");
	}
	SeasonLabel->SetText(FText::FromString(Label));
}

void SSailSimChrome::OnSheetSlider(float Norm01)
{
	// 0 = hard in, 1 = eased (matches dynamics SheetEase)
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedSheetEase = V;
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetSheetEase(V);
	}
	if (CompactSheetLabel.IsValid())
	{
		CompactSheetLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnOuthaulSlider(float Norm01)
{
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedOuthaul = V;
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetOuthaul(V);
	}
	if (CompactOuthaulLabel.IsValid())
	{
		CompactOuthaulLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	if (OuthaulSliderLabel.IsValid())
	{
		const float FootPct = (0.70f + 0.30f * V) * 100.f;
		OuthaulSliderLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%  (foot %.0f%% E)"), V * 100.f, FootPct)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnVangSlider(float Norm01)
{
	// 0 = hard on (boom flat), 1 = eased (boom free to rise) — same as web vangT
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedVang = V;
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetVang(V);
	}
	if (CompactVangLabel.IsValid())
	{
		CompactVangLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	if (VangSliderLabel.IsValid())
	{
		VangSliderLabel->SetText(FText::FromString(FString::Printf(
			TEXT("%.0f%%  (%s)"), V * 100.f,
			V < 0.25f ? TEXT("hard") : (V > 0.75f ? TEXT("eased") : TEXT("set")))));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnJibCarSlider(float Norm01)
{
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedJibCar = V;
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetJibCar(V);
	}
	if (CompactJibCarLabel.IsValid())
	{
		CompactJibCarLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnSpinSheetSlider(float Norm01)
{
	const float V = FMath::Clamp(Norm01, 0.f, 1.f);
	CachedSpinSheet = V;
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetSpinSheetEase(V);
	}
	if (CompactSpinSheetLabel.IsValid())
	{
		CompactSpinSheetLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), V * 100.f)));
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnJibSetToggle()
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->ToggleJibSet();
		CachedJibSet = B->IsJibSet();
	}
	else
	{
		CachedJibSet = !CachedJibSet;
	}
	SchedulePrefsSave();
}

void SSailSimChrome::OnKiteSetToggle()
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->ToggleKiteSet();
		CachedKiteSet = B->IsKiteSet();
	}
	else
	{
		CachedKiteSet = !CachedKiteSet;
	}
	SchedulePrefsSave();
}

void SSailSimChrome::SetPlotMode(uint8 Mode)
{
	ENavPlotMode M = ENavPlotMode::Pan;
	if (Mode == 1) M = ENavPlotMode::Add;
	else if (Mode == 2) M = ENavPlotMode::Delete;

	// Toggle off if already active
	if (MiniMapPaint.IsValid() && MiniMapPaint->GetPlotMode() == M && M != ENavPlotMode::Pan)
	{
		M = ENavPlotMode::Pan;
	}
	if (MiniMapPaint.IsValid())
	{
		MiniMapPaint->SetPlotMode(M);
	}

	const bool bAdd = (M == ENavPlotMode::Add);
	const bool bDel = (M == ENavPlotMode::Delete);
	if (PlotBtnAdd.IsValid())
	{
		PlotBtnAdd->SetBorderImage(bAdd ? FSailSimStyle::ApTabAwaActive() : FSailSimStyle::ApTabIdle());
	}
	if (PlotBtnDel.IsValid())
	{
		PlotBtnDel->SetBorderImage(bDel
			? FSailSimStyle::ApKeyPort() // reuse port red-ish
			: FSailSimStyle::ApTabIdle());
	}
	if (PlotAddLbl.IsValid())
	{
		PlotAddLbl->SetColorAndOpacity(bAdd ? FSailSimStyle::ApAwa : FSailSimStyle::ApStatVal);
	}
	if (PlotDelLbl.IsValid())
	{
		PlotDelLbl->SetColorAndOpacity(bDel ? FLinearColor::White : FSailSimStyle::ApStatVal);
	}
}

void SSailSimChrome::OnPlotClear()
{
	if (UGameInstance* GI = GameInstance.Get())
	{
		if (UNavWaypointSubsystem* Nav = GI->GetSubsystem<UNavWaypointSubsystem>())
		{
			Nav->Clear();
		}
	}
	SetPlotMode(0);
	RefreshWaypointPanel();
}

void SSailSimChrome::RefreshWaypointPanel()
{
	UNavWaypointSubsystem* Nav = nullptr;
	if (UGameInstance* GI = GameInstance.Get())
	{
		Nav = GI->GetSubsystem<UNavWaypointSubsystem>();
	}
	if (!Nav || !WpListBox.IsValid()) return;

	const int32 Rev = Nav->GetRevision();
	const TArray<FNavWaypoint>& Wps = Nav->GetWaypoints();
	const int32 Sel = Nav->GetSelectedIndex();
	const int32 N = Wps.Num();

	// Width tracks map; height grows with WP count (scroll after 8 rows).
	if (WpPanelBox.IsValid())
	{
		WpPanelBox->SetWidthOverride(MapPanelW);
		WpPanelBox->SetHeightOverride(WpPanelHeightForCount(N));
		WpPanelBox->Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
	}

	// Summary + live Lat/Lon/BRG/Dist/Cum/TTG/ETA every tick (cheap text updates only).
	// TTG = leg time at SOG; ETA = clock for cumulative time (web navComputeWaypointLegs).
	const bool bHaveSog = CachedSog > 0.15f;
	double TotNm = 0.0;
	double TotH = 0.0;
	{
		double PrevLat = CachedLat, PrevLon = CachedLon;
		double Cum = 0.0;
		double CumH = 0.0;
		for (int32 I = 0; I < N; ++I)
		{
			const FNavWaypoint& W = Wps[I];
			const double Leg = FNavGeo::DistNm(PrevLat, PrevLon, W.Lat, W.Lon);
			const double Brg = FNavGeo::BearingDeg(PrevLat, PrevLon, W.Lat, W.Lon);
			Cum += Leg;
			TotNm = Cum;
			const FString Ttg = bHaveSog
				? FNavGeo::FormatDurationH(Leg / double(CachedSog))
				: FString(TEXT("—"));
			if (bHaveSog)
			{
				CumH += Leg / double(CachedSog);
				TotH = CumH;
			}
			const FString Eta = bHaveSog ? FNavGeo::FormatEta(CumH) : FString(TEXT("—"));
			if (WpRowLabels.IsValidIndex(I) && WpRowLabels[I].IsValid())
			{
				WpRowLabels[I]->SetText(FText::FromString(
					FormatWpRowLine(I + 1, W.Lat, W.Lon, Brg, Leg, Cum, Ttg, Eta)));
			}
			PrevLat = W.Lat;
			PrevLon = W.Lon;
		}
	}
	if (WpSummary.IsValid())
	{
		if (N == 0)
		{
			WpSummary->SetText(FText::FromString(TEXT("No route")));
		}
		else
		{
			// Web: tot · TTG · ETA · SOG
			FString Sum = FNavGeo::FormatNm(TotNm);
			Sum += TEXT(" · ");
			Sum += bHaveSog ? (TEXT("TTG ") + FNavGeo::FormatDurationH(TotH)) : TEXT("TTG —");
			Sum += TEXT(" · ");
			Sum += bHaveSog ? (TEXT("ETA ") + FNavGeo::FormatEta(TotH)) : TEXT("ETA —");
			Sum += TEXT(" · ");
			Sum += bHaveSog
				? FString::Printf(TEXT("%.1f kt"), CachedSog)
				: FString(TEXT("SOG —"));
			WpSummary->SetText(FText::FromString(Sum));
		}
	}

	// Rebuild widget tree only when route structure or selection changes.
	// Rebuilding every tick was stacking/invalidating row geometry so text piled up.
	const bool bNeedRebuild = (Rev != LastWpRevision) || (Sel != LastWpSelected)
		|| (WpRowLabels.Num() != N);
	if (!bNeedRebuild)
	{
		return;
	}
	LastWpRevision = Rev;
	LastWpSelected = Sel;

	constexpr float RowH = 28.f;
	const FLinearColor RowVal(0.81f, 0.91f, 0.93f, 1.f);
	const FLinearColor SelFg(1.f, 0.62f, 0.35f, 1.f);

	WpListBox->ClearChildren();
	WpRowLabels.Reset();
	WpRowLabels.SetNum(N);

	if (N == 0)
	{
		WpListBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(6.f, 8.f))
			[
				SNew(SBox)
				.HeightOverride(RowH)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Empty — use + WP")))
					.Font(FSailSimStyle::FontApStatLbl())
					.ColorAndOpacity(FSailSimStyle::ApStatLbl)
				]
			];
		return;
	}

	double PrevLat = CachedLat;
	double PrevLon = CachedLon;
	double Cum = 0.0;
	double CumH = 0.0;

	for (int32 I = 0; I < N; ++I)
	{
		const FNavWaypoint& W = Wps[I];
		const double Leg = FNavGeo::DistNm(PrevLat, PrevLon, W.Lat, W.Lon);
		const double Brg = FNavGeo::BearingDeg(PrevLat, PrevLon, W.Lat, W.Lon);
		Cum += Leg;
		PrevLat = W.Lat;
		PrevLon = W.Lon;
		const FString Ttg = bHaveSog
			? FNavGeo::FormatDurationH(Leg / double(CachedSog))
			: FString(TEXT("—"));
		if (bHaveSog)
		{
			CumH += Leg / double(CachedSog);
		}
		const FString Eta = bHaveSog ? FNavGeo::FormatEta(CumH) : FString(TEXT("—"));
		const bool bSel = (I == Sel);
		const int32 Idx = I;
		const FString Line = FormatWpRowLine(I + 1, W.Lat, W.Lon, Brg, Leg, Cum, Ttg, Eta);
		const FLinearColor LineCol = bSel ? SelFg : RowVal;

		TSharedPtr<STextBlock> LineLbl;
		WpListBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.f, 0.f, 0.f, 2.f))
			[
				// Forced height — rows cannot share the same Y.
				SNew(SBox)
				.HeightOverride(RowH)
				.WidthOverride_Lambda([this]() { return FMath::Max(120.f, MapPanelW - 16.f); })
				[
					SNew(SBorder)
					.BorderImage(bSel ? FSailSimStyle::ApTabAwaActive() : FSailSimStyle::ApTabIdle())
					.Padding(FMargin(6.f, 3.f))
					.Cursor(EMouseCursor::Hand)
					.OnMouseButtonDown_Lambda([this, Idx](const FGeometry&, const FPointerEvent& Ev) -> FReply
					{
						if (Ev.GetEffectingButton() != EKeys::LeftMouseButton)
						{
							return FReply::Unhandled();
						}
						if (UGameInstance* GI = GameInstance.Get())
						{
							if (UNavWaypointSubsystem* NavSel = GI->GetSubsystem<UNavWaypointSubsystem>())
							{
								NavSel->SetSelectedIndex(Idx);
							}
						}
						RefreshWaypointPanel();
						return FReply::Handled();
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.f)
						.VAlign(VAlign_Center)
						[
							SAssignNew(LineLbl, STextBlock)
							.Text(FText::FromString(Line))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							.ColorAndOpacity(LineCol)
							.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.f, 0.f, 0.f, 0.f)
						[
							SNew(SButton)
							.ButtonStyle(FCoreStyle::Get(), "NoBorder")
							.ContentPadding(FMargin(3.f, 1.f))
							.IsEnabled(Idx > 0)
							.OnClicked_Lambda([this, Idx]()
							{
								if (UGameInstance* GI = GameInstance.Get())
								{
									if (UNavWaypointSubsystem* NavM = GI->GetSubsystem<UNavWaypointSubsystem>())
									{
										NavM->Move(Idx, -1);
									}
								}
								RefreshWaypointPanel();
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("↑")))
								.Font(FSailSimStyle::FontApKey()).ColorAndOpacity(RowVal)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FCoreStyle::Get(), "NoBorder")
							.ContentPadding(FMargin(3.f, 1.f))
							.IsEnabled(Idx + 1 < N)
							.OnClicked_Lambda([this, Idx]()
							{
								if (UGameInstance* GI = GameInstance.Get())
								{
									if (UNavWaypointSubsystem* NavM = GI->GetSubsystem<UNavWaypointSubsystem>())
									{
										NavM->Move(Idx, +1);
									}
								}
								RefreshWaypointPanel();
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("↓")))
								.Font(FSailSimStyle::FontApKey()).ColorAndOpacity(RowVal)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FCoreStyle::Get(), "NoBorder")
							.ContentPadding(FMargin(3.f, 1.f))
							.ToolTipText(FText::FromString(TEXT("Go to on chart")))
							.OnClicked_Lambda([this, Idx]()
							{
								if (UGameInstance* GI = GameInstance.Get())
								{
									if (UNavWaypointSubsystem* NavG = GI->GetSubsystem<UNavWaypointSubsystem>())
									{
										NavG->SetSelectedIndex(Idx);
										FNavWaypoint Wp;
										if (NavG->GetWaypoint(Idx, Wp) && MiniMapPaint.IsValid())
										{
											MiniMapPaint->CenterOn(Wp.Lat, Wp.Lon);
										}
									}
								}
								RefreshWaypointPanel();
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("◎")))
								.Font(FSailSimStyle::FontApKey())
								.ColorAndOpacity(FLinearColor(0.3f, 0.82f, 0.9f, 1.f))
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FCoreStyle::Get(), "NoBorder")
							.ContentPadding(FMargin(3.f, 1.f))
							.OnClicked_Lambda([this, Idx]()
							{
								if (UGameInstance* GI = GameInstance.Get())
								{
									if (UNavWaypointSubsystem* NavR = GI->GetSubsystem<UNavWaypointSubsystem>())
									{
										NavR->RemoveAt(Idx);
									}
								}
								RefreshWaypointPanel();
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("×")))
								.Font(FSailSimStyle::FontApKey()).ColorAndOpacity(FSailSimStyle::Port)
							]
						]
					]
				]
			];
		WpRowLabels[I] = LineLbl;
	}
}

void SSailSimChrome::RefreshAutopilotPanel()
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;

	// Panel border by engaged mode
	if (ApPanelBorder.IsValid())
	{
		const FSlateBrush* Br = FSailSimStyle::ApPanelBrush();
		if (bCachedAuto)
		{
			if (CachedAutoMode == FBoatDynamics::EAutoMode::Awa) Br = FSailSimStyle::ApPanelEngagedAwa();
			else if (CachedAutoMode == FBoatDynamics::EAutoMode::Nav) Br = FSailSimStyle::ApPanelEngagedNav();
			else Br = FSailSimStyle::ApPanelEngagedHdg();
		}
		ApPanelBorder->SetBorderImage(Br);
	}

	// Mode tabs
	const bool bHdg = CachedAutoMode == FBoatDynamics::EAutoMode::Hdg;
	const bool bAwa = CachedAutoMode == FBoatDynamics::EAutoMode::Awa;
	const bool bNav = CachedAutoMode == FBoatDynamics::EAutoMode::Nav;
	if (ApTabHdgBorder.IsValid())
	{
		ApTabHdgBorder->SetBorderImage(bHdg ? FSailSimStyle::ApTabHdgActive() : FSailSimStyle::ApTabIdle());
	}
	if (ApTabAwaBorder.IsValid())
	{
		ApTabAwaBorder->SetBorderImage(bAwa ? FSailSimStyle::ApTabAwaActive() : FSailSimStyle::ApTabIdle());
	}
	if (ApTabNavBorder.IsValid())
	{
		ApTabNavBorder->SetBorderImage(bNav ? FSailSimStyle::ApTabNavActive() : FSailSimStyle::ApTabIdle());
	}
	if (ApTabHdgLbl.IsValid())
	{
		ApTabHdgLbl->SetColorAndOpacity(bHdg ? FSailSimStyle::Accent : FSailSimStyle::ApHeader);
	}
	if (ApTabAwaLbl.IsValid())
	{
		ApTabAwaLbl->SetColorAndOpacity(bAwa ? FSailSimStyle::ApAwa : FSailSimStyle::ApHeader);
	}
	if (ApTabNavLbl.IsValid())
	{
		ApTabNavLbl->SetColorAndOpacity(bNav ? FSailSimStyle::ApNav : FSailSimStyle::ApHeader);
	}

	// Stats
	const float HdgShow = FMath::Fmod(CachedHeading + 360.f, 360.f);
	if (ApHdgVal.IsValid())
	{
		ApHdgVal->SetText(FText::FromString(FString::Printf(TEXT("%03.0f°"), HdgShow)));
	}
	if (ApAwaVal.IsValid())
	{
		const TCHAR Side = (CachedAwa >= 0.f) ? TEXT('S') : TEXT('P');
		ApAwaVal->SetText(FText::FromString(FString::Printf(TEXT("%c%03.0f°"), Side, FMath::Abs(CachedAwa))));
	}
	if (ApTgtVal.IsValid())
	{
		ApTgtVal->SetText(FText::FromString(B->GetAutoTargetDisplayText()));
		FLinearColor TgtCol = FSailSimStyle::ApStatVal;
		if (bCachedAuto)
		{
			if (bAwa) TgtCol = FSailSimStyle::ApAwa;
			else if (bNav) TgtCol = FSailSimStyle::ApNav;
			else TgtCol = FSailSimStyle::Accent;
		}
		ApTgtVal->SetColorAndOpacity(TgtCol);
	}

	// Slider label
	if (ApSliderLbl.IsValid())
	{
		if (bAwa) ApSliderLbl->SetText(FText::FromString(TEXT("TARGET AWA")));
		else if (bNav) ApSliderLbl->SetText(FText::FromString(TEXT("BEARING TO WAYPOINT")));
		else ApSliderLbl->SetText(FText::FromString(TEXT("TARGET HEADING")));
	}

	// AUTO (pilot engage) — independent of TRIM
	if (ApAutoBorder.IsValid())
	{
		ApAutoBorder->SetBorderImage(
			bCachedAuto ? FSailSimStyle::ApModeAutoActive() : FSailSimStyle::ApModeAutoIdle());
	}
	if (ApAutoLbl.IsValid())
	{
		ApAutoLbl->SetText(FText::FromString(bCachedAuto ? TEXT("AUTO") : TEXT("STBY")));
	}

	// Trim button (sheet auto — independent of pilot engage)
	if (ApTrimBorder.IsValid())
	{
		ApTrimBorder->SetBorderImage(
			bCachedAutoTrim ? FSailSimStyle::ApModeTrimActive() : FSailSimStyle::ApModeTrimIdle());
	}
}

void SSailSimChrome::OnApMode(uint8 ModeIndex)
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SelectAutoMode(ModeIndex);
	}
}

void SSailSimChrome::OnApNudge(float Delta)
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->NudgeAutoTarget(Delta);
	}
}

void SSailSimChrome::OnApSlider(float Norm01)
{
	ASailBoatPawn* B = Boat.Get();
	if (!B) return;
	const float N = FMath::Clamp(Norm01, 0.f, 1.f);
	if (B->GetAutoMode() == FBoatDynamics::EAutoMode::Awa)
	{
		// −170..+170 — only if meaningfully different (avoid I-reset spam)
		const float Want = N * 340.f - 170.f;
		if (FMath::Abs(Want - B->GetAutoAwaTarget()) > 0.5f)
		{
			B->SetAutoAwaTarget(Want);
		}
	}
	else if (B->GetAutoMode() != FBoatDynamics::EAutoMode::Nav)
	{
		const float Want = N * 359.f;
		if (FMath::Abs(FBoatDynamics::Wrap180(Want - B->GetAutoTargetHeading())) > 0.5f)
		{
			B->SetAutoTargetHeading(Want);
		}
	}
}

void SSailSimChrome::OnApAuto()
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		// Toggle pilot engage for the currently selected mode (HDG / AWA / NAV).
		// Does not touch auto-trim.
		B->ToggleAutoHeading();
	}
}

void SSailSimChrome::OnApTack()
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->StartTack();
	}
}

void SSailSimChrome::OnApTrim()
{
	if (ASailBoatPawn* B = Boat.Get())
	{
		// Sheet auto-trim only — independent of autopilot on/off.
		B->ToggleAutoTrim();
	}
}

void SSailSimChrome::CenterHelm()
{
	// Explicit center only (sticky tiller otherwise keeps last angle)
	if (ASailBoatPawn* B = Boat.Get())
	{
		B->SetHelmInput(0.f);
	}
}

void SSailSimChrome::OnHelmValue(float Norm01)
{
	// Slider sets sticky tiller. Ignore while AUTO owns the helm so accidental
	// clicks / slider commits cannot disengage the pilot mid-hold.
	if (ASailBoatPawn* B = Boat.Get())
	{
		if (B->IsAutoHeading()) return;
		B->SetHelmInput(FMath::Clamp(Norm01, 0.f, 1.f) * 70.f - 35.f);
	}
}
