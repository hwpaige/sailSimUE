#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Sailing/BoatDynamics.h"

class ASailBoatPawn;
class UGameInstance;
class STextBlock;
class SBorder;
class SBox;
class SCheckBox;
class SSlider;
class SVerticalBox;

/**
 * Modern sail-sim instrument chrome (Slate).
 * Rounded glass cards, gauge column, helm, minimap + autopilot panel, settings.
 * Autopilot panel matches web .autopilot-panel (dist styles.css).
 */
class SSailSimChrome : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSailSimChrome) {}
		SLATE_ARGUMENT(TWeakObjectPtr<ASailBoatPawn>, Boat)
		SLATE_ARGUMENT(TWeakObjectPtr<UGameInstance>, GameInstance)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	void SetBoat(TWeakObjectPtr<ASailBoatPawn> InBoat)
	{
		// New pawn → re-apply disk prefs (PIE restarts, repossess, hot reload).
		if (Boat.Get() != InBoat.Get())
		{
			bPrefsApplied = false;
		}
		Boat = InBoat;
	}
	void SetGameInstance(TWeakObjectPtr<UGameInstance> InGI) { GameInstance = InGI; }

	/** Immediate disk write (call on PIE stop / HUD EndPlay). */
	void ForceSavePrefsNow();

private:
	TWeakObjectPtr<ASailBoatPawn> Boat;
	bool bSettingsOpen = false;

	// Live text (metrics card, top-left)
	TSharedPtr<STextBlock> BoatChip;
	TSharedPtr<STextBlock> FpsLabel;
	TSharedPtr<STextBlock> StatusLine1;
	TSharedPtr<STextBlock> StatusLine2;
	TSharedPtr<STextBlock> StatusLine3;
	TSharedPtr<STextBlock> StatusLine4;
	TSharedPtr<STextBlock> StatusLine5;

	TSharedPtr<STextBlock> HelmReadout;

	// Autopilot panel
	TSharedPtr<SBorder> ApPanelBorder;
	TSharedPtr<STextBlock> ApHdgVal;
	TSharedPtr<STextBlock> ApAwaVal;
	TSharedPtr<STextBlock> ApTgtVal;
	TSharedPtr<STextBlock> ApSliderLbl;
	TSharedPtr<STextBlock> ApTabHdgLbl;
	TSharedPtr<STextBlock> ApTabAwaLbl;
	TSharedPtr<STextBlock> ApTabNavLbl;
	TSharedPtr<STextBlock> ApAutoLbl;
	TSharedPtr<STextBlock> ApTrimLbl;
	TSharedPtr<SBorder> ApTabHdgBorder;
	TSharedPtr<SBorder> ApTabAwaBorder;
	TSharedPtr<SBorder> ApTabNavBorder;
	TSharedPtr<SBorder> ApAutoBorder;
	TSharedPtr<SBorder> ApTrimBorder;

	TSharedPtr<STextBlock> MapTime;
	TSharedPtr<STextBlock> MapCoords;
	TSharedPtr<STextBlock> MapPlace;
	TSharedPtr<STextBlock> PillSog;
	TSharedPtr<STextBlock> PillTws;
	TSharedPtr<STextBlock> PillTwd;
	TSharedPtr<STextBlock> PillAwa;
	TSharedPtr<STextBlock> PillAws;
	TSharedPtr<STextBlock> PillHelm;
	TSharedPtr<SBox> PillSogWrap;
	TSharedPtr<SBox> PillAwaWrap;
	TSharedPtr<SBox> PillAwsWrap;
	TSharedPtr<SBox> PillHelmWrap;

	TSharedPtr<SBorder> SettingsDrawer;
	TSharedPtr<SBorder> LightsPanelBorder;
	bool bLightsPanelOpen = true;
	TSharedPtr<STextBlock> SettingsBody;
	TSharedPtr<STextBlock> PerfFrameLabel;
	TSharedPtr<STextBlock> PerfBottleneckLabel;
	/** Engine unit rows: GT, GTwait, RT, RHI, Swap. */
	TArray<TSharedPtr<STextBlock>> PerfEngineLabels;
	/** One row per SailSim GT bucket (sorted by cost). */
	TArray<TSharedPtr<STextBlock>> PerfBucketLabels;
	TSharedPtr<STextBlock> PerfOtherGtLabel;
	TSharedPtr<STextBlock> PerfResidentLabel;
	TSharedPtr<STextBlock> PerfRenderDriversLabel;
	TSharedPtr<STextBlock> PerfHintLabel;
	TSharedPtr<STextBlock> TwsSliderLabel;
	TSharedPtr<STextBlock> TwdSliderLabel;
	TSharedPtr<STextBlock> CloudSliderLabel;
	TSharedPtr<STextBlock> FogSliderLabel;
	TSharedPtr<STextBlock> TimeOfDayLabel;
	TSharedPtr<SSlider> TimeOfDaySlider;
	float CachedTimeOfDay01 = 0.5f; // noon = Fair Day baseline
	TSharedPtr<STextBlock> SeasonLabel;
	TSharedPtr<SSlider> SeasonSlider;
	float CachedSeason01 = 0.5f; // summer
	TSharedPtr<STextBlock> OuthaulSliderLabel;
	TSharedPtr<STextBlock> VangSliderLabel;
	TSharedPtr<STextBlock> CompactTwsLabel;
	TSharedPtr<STextBlock> CompactSheetLabel;
	TSharedPtr<STextBlock> CompactOuthaulLabel;
	TSharedPtr<STextBlock> CompactVangLabel;
	TSharedPtr<STextBlock> CompactJibCarLabel;
	TSharedPtr<STextBlock> CompactSpinSheetLabel;
	TSharedPtr<STextBlock> CompactJibSetLabel;
	TSharedPtr<STextBlock> CompactKiteSetLabel;
	float CachedCloudIntensity = 1.f;
	float CachedFogIntensity = 1.f;
	float CachedSheetEase = 0.25f;
	float CachedOuthaul = 0.94f;
	float CachedVang = 0.40f;
	float CachedJibCar = 0.45f;
	float CachedSpinSheet = 0.18f;
	bool CachedJibSet = true;
	bool CachedKiteSet = false;
	bool bPrefsApplied = false;
	bool bPrefsDirty = false;
	double NextPrefsSaveTime = 0.0;
	TSharedPtr<SBox> MapOuterBox;
	TSharedPtr<SBox> WpPanelBox;
	TSharedPtr<class SSailMiniMapPaint> MiniMapPaint;
	/** Vertical stack of fixed-height WP rows (rebuilt only when route/selection changes). */
	TSharedPtr<SVerticalBox> WpListBox;
	TSharedPtr<STextBlock> WpSummary;
	TSharedPtr<SBorder> PlotBtnAdd;
	TSharedPtr<SBorder> PlotBtnDel;
	TSharedPtr<STextBlock> PlotAddLbl;
	TSharedPtr<STextBlock> PlotDelLbl;
	int32 LastWpRevision = -1;
	int32 LastWpSelected = -2;
	/** Row label widgets for live BRG/LEG/CUM refresh without rebuilding the tree. */
	TArray<TSharedPtr<STextBlock>> WpRowLabels;

	/** Mini-map panel size (web defaults 424×241, corner-resizable). */
	float MapPanelW = 424.f;
	float MapPanelH = 241.f;
	static constexpr float MapMinW = 220.f;
	static constexpr float MapMinH = 160.f;
	static constexpr float MapMaxW = 720.f;
	static constexpr float MapMaxH = 560.f;
	bool bMapResizing = false;
	float MapResizeFixedLeftAbs = 0.f;
	float MapResizeFixedBottomAbs = 0.f;
	float MapResizeGeoScale = 1.f;

	float CachedHeading = 0.f;
	float CachedTwd = 0.f;
	float CachedHeel = 0.f;
	float CachedPitch = 0.f;
	float CachedRudder = 0.f;
	float CachedAutoTarget = 0.f;
	float CachedAutoAwaTarget = 45.f;
	bool bCachedAuto = false;
	bool bCachedAutoTrim = false;
	FBoatDynamics::EAutoMode CachedAutoMode = FBoatDynamics::EAutoMode::Hdg;
	FVector CachedLoc = FVector::ZeroVector;
	float CachedSog = 0.f;
	float CachedAwa = 0.f;
	float CachedAws = 0.f;
	float CachedTws = 0.f;
	bool bCachedSailing = true;
	double CachedLat = 41.48;
	double CachedLon = -70.22;
	TWeakObjectPtr<UGameInstance> GameInstance;

	TSharedRef<SWidget> BuildTopBar();
	TSharedRef<SWidget> BuildStatusCard();
	TSharedRef<SWidget> BuildGaugeColumn();
	TSharedRef<SWidget> BuildHelmBar();
	TSharedRef<SWidget> BuildMiniMap();
	TSharedRef<SWidget> BuildAutopilotPanel();
	TSharedRef<SWidget> BuildWaypointPanel();
	TSharedRef<SWidget> BuildNavLowerCluster();
	TSharedRef<SWidget> BuildSettingsDrawer();
	TSharedRef<SWidget> BuildPerfRow(const FString& Name, TSharedPtr<STextBlock>& OutLabel);
	TSharedRef<SWidget> BuildPerfEngineList();
	TSharedRef<SWidget> BuildPerfBucketList();
	void RefreshPerfPanel(float DeltaSeconds);
	/** Yacht DC breaker panel for COLREGS / cabin lights. */
	TSharedRef<SWidget> BuildLightsBreakerPanel();
	TSharedRef<SWidget> MakeBreakerRow(const FString& Label, TFunction<bool()> IsOn, TFunction<void()> OnToggle);
	TSharedRef<SWidget> MakePlotToolBtn(const FString& Label, TSharedPtr<SBorder>& OutBorder,
		TSharedPtr<STextBlock>& OutLbl, FOnClicked OnClick);
	void RefreshWaypointPanel();
	void SetPlotMode(uint8 Mode); // 0 pan, 1 add, 2 delete
	void OnPlotClear();
	TSharedRef<SWidget> MakeGlassCard(const TSharedRef<SWidget>& Content, FMargin Pad = FMargin(14.f, 12.f));
	/** Nested section card with accent header chip (settings / sail trim). */
	TSharedRef<SWidget> MakeSectionCard(const FString& Title, const TSharedRef<SWidget>& Content);
	TSharedRef<SWidget> MakeTelemetryPill(TSharedPtr<STextBlock>& OutText, const FLinearColor& Color);
	TSharedRef<SWidget> MakePillButton(const FText& Label, FOnClicked OnClicked, bool bAccent = false);
	TSharedRef<SWidget> MakeApKey(const FString& Label, float Delta, bool bDec);
	TSharedRef<SWidget> MakeApStatRow(const FString& Lbl, TSharedPtr<STextBlock>& OutVal);
	/** Instrument slider with large grabber (shared style). */
	TSharedRef<SWidget> MakeStyledSlider(
		TFunction<float()> GetNorm01,
		TFunction<void(float)> OnNorm01,
		float Height = 0.f);
	void RefreshFromBoat();
	void RefreshAutopilotPanel();
	void ToggleSettings();
	void CenterHelm();
	void OnHelmValue(float Norm01);
	void OnTwsSlider(float Norm01);
	void OnTwdSlider(float Norm01);
	void OnCloudSlider(float Norm01);
	void OnFogSlider(float Norm01);
	void OnTimeOfDaySlider(float Norm01);
	void RefreshTimeOfDayLabel();
	void OnSeasonSlider(float Norm01);
	void RefreshSeasonLabel();
	void OnSheetSlider(float Norm01);
	void OnOuthaulSlider(float Norm01);
	void OnVangSlider(float Norm01);
	void OnJibCarSlider(float Norm01);
	void OnSpinSheetSlider(float Norm01);
	void OnJibSetToggle();
	void OnKiteSetToggle();
	void OnEnvPreset(uint8 PresetId);
	void CapturePrefsFromLive();
	void SchedulePrefsSave();
	void FlushPrefsSave();
	void TryApplyUserPrefs();
	TSharedRef<SWidget> BuildEnvPresetRow();
	void OnApMode(uint8 ModeIndex);
	void OnApNudge(float Delta);
	void OnApSlider(float Norm01);
	void OnApAuto();
	void OnApTack();
	void OnApTrim();
	TSharedRef<SWidget> MakeLabeledSlider(
		const FString& Title,
		TSharedPtr<STextBlock>& OutLabel,
		TFunction<float()> GetNorm01,
		TFunction<void(float)> OnNorm01);
	void ApplyMapSize(float W, float H);
	FReply OnMapResizeDown(const FGeometry& Geo, const FPointerEvent& Event);
	FReply OnMapResizeMove(const FGeometry& Geo, const FPointerEvent& Event);
	FReply OnMapResizeUp(const FGeometry& Geo, const FPointerEvent& Event);
};
