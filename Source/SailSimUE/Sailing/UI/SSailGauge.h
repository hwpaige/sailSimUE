#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Sailing/UI/SailSimStyle.h"

enum class ESailGaugeKind : uint8
{
	Compass,
	Heel,
	Pitch,
};

/** Circular instrument face with anti-aliased-ish Slate line drawing. */
class SSailGauge : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SSailGauge)
		: _Kind(ESailGaugeKind::Compass)
		, _Size(FSailSimStyle::GaugeSize)
		, _Primary(0.f)
		, _Secondary(0.f)
	{}
		SLATE_ARGUMENT(ESailGaugeKind, Kind)
		SLATE_ARGUMENT(float, Size)
		SLATE_ATTRIBUTE(float, Primary)
		SLATE_ATTRIBUTE(float, Secondary)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float) const override;

private:
	ESailGaugeKind Kind = ESailGaugeKind::Compass;
	float Size = FSailSimStyle::GaugeSize;
	TAttribute<float> Primary;
	TAttribute<float> Secondary;

	void PaintCompass(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer, float Heading, float Twd) const;
	void PaintHeel(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer, float Heel) const;
	void PaintPitch(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer, float Pitch) const;
	static void DrawCircle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
		FVector2D Center, float R, const FLinearColor& Color, float Thickness, int32 Segs = 48);
	void DrawFilledDisc(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
		FVector2D Center, float R, const FSlateBrush* Brush) const;
	static void DrawRadialTick(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
		FVector2D Center, float DegFromUp, float R0, float R1, const FLinearColor& Color, float Thickness);
	static void DrawNeedle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
		FVector2D Center, float DegFromUp, float Len, const FLinearColor& Color, float Thickness);
	void DrawCenteredLabel(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
		FVector2D Center, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Color) const;
};
