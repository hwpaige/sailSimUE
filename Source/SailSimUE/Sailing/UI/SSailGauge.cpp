#include "Sailing/UI/SSailGauge.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"

void SSailGauge::Construct(const FArguments& InArgs)
{
	Kind = InArgs._Kind;
	Size = InArgs._Size;
	Primary = InArgs._Primary;
	Secondary = InArgs._Secondary;
}

FVector2D SSailGauge::ComputeDesiredSize(float) const
{
	return FVector2D(Size, Size);
}

void SSailGauge::DrawCircle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
	FVector2D Center, float R, const FLinearColor& Color, float Thickness, int32 Segs)
{
	TArray<FVector2D> Pts;
	Pts.Reserve(Segs + 1);
	for (int32 I = 0; I <= Segs; ++I)
	{
		const float A = (2.f * PI * float(I)) / float(Segs);
		Pts.Add(Center + FVector2D(FMath::Cos(A) * R, FMath::Sin(A) * R));
	}
	FSlateDrawElement::MakeLines(
		Out, Layer, Geo.ToPaintGeometry(), Pts,
		ESlateDrawEffect::None, Color, true, Thickness);
}

void SSailGauge::DrawFilledDisc(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
	FVector2D Center, float R, const FSlateBrush* Brush) const
{
	if (!Brush) return;
	// Brush must outlive draw (static style brushes only — never stack temps)
	FSlateDrawElement::MakeBox(
		Out, Layer,
		Geo.ToPaintGeometry(FVector2D(R * 2.f, R * 2.f), FSlateLayoutTransform(Center - FVector2D(R, R))),
		Brush, ESlateDrawEffect::None);
}

void SSailGauge::DrawRadialTick(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
	FVector2D Center, float DegFromUp, float R0, float R1, const FLinearColor& Color, float Thickness)
{
	const float Rad = FMath::DegreesToRadians(DegFromUp);
	const FVector2D D(FMath::Sin(Rad), -FMath::Cos(Rad));
	TArray<FVector2D> Pts;
	Pts.Add(Center + D * R0);
	Pts.Add(Center + D * R1);
	FSlateDrawElement::MakeLines(
		Out, Layer, Geo.ToPaintGeometry(), Pts,
		ESlateDrawEffect::None, Color, true, Thickness);
}

void SSailGauge::DrawNeedle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
	FVector2D Center, float DegFromUp, float Len, const FLinearColor& Color, float Thickness)
{
	DrawRadialTick(Out, Layer, Geo, Center, DegFromUp, Len * 0.15f, Len, Color, Thickness);
	const float Rad = FMath::DegreesToRadians(DegFromUp);
	const FVector2D Tip = Center + FVector2D(FMath::Sin(Rad), -FMath::Cos(Rad)) * Len;
	const FVector2D Perp(FMath::Cos(Rad), FMath::Sin(Rad));
	const FVector2D Back = Tip - FVector2D(FMath::Sin(Rad), -FMath::Cos(Rad)) * 6.5f;
	TArray<FVector2D> Head;
	Head.Add(Tip);
	Head.Add(Back + Perp * 3.5f);
	Head.Add(Back - Perp * 3.5f);
	Head.Add(Tip);
	FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Head,
		ESlateDrawEffect::None, Color, true, Thickness);
}

void SSailGauge::DrawCenteredLabel(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geo,
	FVector2D Center, const FString& Text, const FSlateFontInfo& Font, const FLinearColor& Color) const
{
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FVector2D TS = Measure->Measure(Text, Font);
	FSlateDrawElement::MakeText(
		Out, Layer,
		Geo.ToPaintGeometry(TS, FSlateLayoutTransform(Center - TS * 0.5f)),
		Text, Font, ESlateDrawEffect::None, Color);
}

int32 SSailGauge::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D Sz = AllottedGeometry.GetLocalSize();
	const float Side = FMath::Min(Sz.X, Sz.Y);
	const FVector2D C = Sz * 0.5f;
	const float FaceR = Side * 0.5f;

	// Layered modern instrument body
	// 1) Soft drop shadow
	DrawFilledDisc(OutDrawElements, LayerId, AllottedGeometry,
		C + FVector2D(0.f, 3.f), FaceR * 0.98f, FSailSimStyle::GaugeShadowBrush());
	// 2) Outer bezel
	DrawFilledDisc(OutDrawElements, LayerId + 1, AllottedGeometry,
		C, FaceR * 0.99f, FSailSimStyle::GaugeBezelBrush());
	// 3) Glass face with cyan rim
	DrawFilledDisc(OutDrawElements, LayerId + 2, AllottedGeometry,
		C, FaceR * 0.92f, FSailSimStyle::GaugeFaceBrush());
	// 4) Dark inset well for ticks / needles
	DrawFilledDisc(OutDrawElements, LayerId + 3, AllottedGeometry,
		C, FaceR * 0.82f, FSailSimStyle::GaugeWellBrush());

	// Subtle accent ring between face and well
	DrawCircle(OutDrawElements, LayerId + 4, AllottedGeometry, C, FaceR * 0.825f,
		FLinearColor(FSailSimStyle::Accent.R, FSailSimStyle::Accent.G, FSailSimStyle::Accent.B, 0.22f),
		1.2f, 48);

	const float P = Primary.Get(0.f);
	const float S = Secondary.Get(0.f);
	const int32 L = LayerId + 5;

	switch (Kind)
	{
	case ESailGaugeKind::Compass: PaintCompass(AllottedGeometry, OutDrawElements, L, P, S); break;
	case ESailGaugeKind::Heel: PaintHeel(AllottedGeometry, OutDrawElements, L, P); break;
	case ESailGaugeKind::Pitch: PaintPitch(AllottedGeometry, OutDrawElements, L, P); break;
	}

	const FString Value = (Kind == ESailGaugeKind::Pitch)
		? FString::Printf(TEXT("%+.1f°"), P)
		: (Kind == ESailGaugeKind::Heel)
			? FString::Printf(TEXT("%+.0f°"), P)
			: FString::Printf(TEXT("%03.0f"), FMath::Fmod(P + 360.f, 360.f));
	const FString Label = (Kind == ESailGaugeKind::Compass) ? TEXT("HDG")
		: (Kind == ESailGaugeKind::Heel) ? TEXT("HEEL") : TEXT("PITCH");

	// Soft text shadow for readability over dark well
	DrawCenteredLabel(OutDrawElements, LayerId + 12, AllottedGeometry,
		C + FVector2D(0.8f, Side * 0.10f + 0.8f), Value, FSailSimStyle::FontMonoLg(),
		FLinearColor(0.f, 0.f, 0.f, 0.70f));
	DrawCenteredLabel(OutDrawElements, LayerId + 13, AllottedGeometry,
		C + FVector2D(0.f, Side * 0.10f), Value, FSailSimStyle::FontMonoLg(),
		FLinearColor(0.93f, 0.96f, 0.98f, 1.f));

	// Label chip below value
	DrawCenteredLabel(OutDrawElements, LayerId + 14, AllottedGeometry,
		C + FVector2D(0.f, Side * 0.28f), Label, FSailSimStyle::FontLabel(),
		FLinearColor(FSailSimStyle::Accent.R, FSailSimStyle::Accent.G, FSailSimStyle::Accent.B, 0.85f));

	return LayerId + 15;
}

void SSailGauge::PaintCompass(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer,
	float Heading, float Twd) const
{
	const FVector2D Sz = Geo.GetLocalSize();
	const float Side = FMath::Min(Sz.X, Sz.Y);
	const FVector2D C = Sz * 0.5f;
	const float R = Side * 0.38f;
	const float H = FMath::Fmod(Heading + 360.f, 360.f);

	for (int32 I = 0; I < 72; ++I)
	{
		const float Card = I * 5.f;
		const float Screen = Card - H;
		const bool bMajor = (I % 6 == 0);
		const bool bMid = (I % 2 == 0);
		if (!bMid) continue;
		DrawRadialTick(Out, Layer, Geo, C, Screen,
			R * (bMajor ? 0.78f : 0.88f), R * 0.98f,
			bMajor ? FLinearColor(0.85f, 0.93f, 0.95f, 0.92f) : FLinearColor(0.50f, 0.55f, 0.60f, 0.50f),
			bMajor ? 1.6f : 1.f);
	}

	auto Cardinal = [&](float Deg, const TCHAR* Lbl, const FLinearColor& Col)
	{
		const float Screen = Deg - H;
		const float Rad = FMath::DegreesToRadians(Screen);
		const FVector2D P = C + FVector2D(FMath::Sin(Rad), -FMath::Cos(Rad)) * (R * 0.58f);
		DrawCenteredLabel(Out, Layer + 1, Geo, P, Lbl, FSailSimStyle::FontLabel(), Col);
	};
	Cardinal(0.f, TEXT("N"), FLinearColor::FromSRGBColor(FColor(0xff, 0x6e, 0x6e)));
	Cardinal(90.f, TEXT("E"), FSailSimStyle::TextDim);
	Cardinal(180.f, TEXT("S"), FSailSimStyle::TextDim);
	Cardinal(270.f, TEXT("W"), FSailSimStyle::TextDim);

	// Lubber
	DrawRadialTick(Out, Layer + 2, Geo, C, 0.f, R * 0.46f, R * 0.99f, FSailSimStyle::Accent, 2.2f);
	// Wind FROM
	DrawNeedle(Out, Layer + 2, Geo, C, Twd - H, R * 0.48f,
		FLinearColor::FromSRGBColor(FColor(0xff, 0xd2, 0x4d)), 1.8f);

	DrawFilledDisc(Out, Layer + 3, Geo, C, 5.0f, FSailSimStyle::GaugeHubOuterBrush());
	DrawFilledDisc(Out, Layer + 4, Geo, C, 2.8f, FSailSimStyle::GaugeHubBrush());
}

void SSailGauge::PaintHeel(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer, float Heel) const
{
	const FVector2D Sz = Geo.GetLocalSize();
	const float Side = FMath::Min(Sz.X, Sz.Y);
	const FVector2D C = Sz * 0.5f;
	const float R = Side * 0.38f;
	const float ClampH = FMath::Clamp(Heel, -40.f, 40.f);
	const float Rad = FMath::DegreesToRadians(ClampH);

	// Port / stbd rim accents
	TArray<FVector2D> PortArc, StbdArc;
	for (int32 I = 0; I <= 18; ++I)
	{
		const float A = FMath::Lerp(-55.f, -5.f, float(I) / 18.f);
		const float Ar = FMath::DegreesToRadians(A);
		PortArc.Add(C + FVector2D(FMath::Sin(Ar), -FMath::Cos(Ar)) * (R * 0.94f));
	}
	for (int32 I = 0; I <= 18; ++I)
	{
		const float A = FMath::Lerp(5.f, 55.f, float(I) / 18.f);
		const float Ar = FMath::DegreesToRadians(A);
		StbdArc.Add(C + FVector2D(FMath::Sin(Ar), -FMath::Cos(Ar)) * (R * 0.94f));
	}
	FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), PortArc,
		ESlateDrawEffect::None, FLinearColor(FSailSimStyle::Port.R, FSailSimStyle::Port.G, FSailSimStyle::Port.B, 0.65f), true, 2.8f);
	FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), StbdArc,
		ESlateDrawEffect::None, FLinearColor(FSailSimStyle::Stbd.R, FSailSimStyle::Stbd.G, FSailSimStyle::Stbd.B, 0.65f), true, 2.8f);

	// Horizon
	const FVector2D Dir(FMath::Cos(Rad), FMath::Sin(Rad));
	TArray<FVector2D> Horizon;
	Horizon.Add(C - Dir * (R * 0.82f));
	Horizon.Add(C + Dir * (R * 0.82f));
	FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Horizon,
		ESlateDrawEffect::None, FLinearColor(0.85f, 0.93f, 0.95f, 0.95f), true, 2.2f);

	// Fixed wings
	TArray<FVector2D> LWing = { C + FVector2D(-R * 0.40f, 0.f), C + FVector2D(-R * 0.12f, 0.f) };
	TArray<FVector2D> RWing = { C + FVector2D(R * 0.12f, 0.f), C + FVector2D(R * 0.40f, 0.f) };
	FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), LWing,
		ESlateDrawEffect::None, FSailSimStyle::Accent, true, 2.2f);
	FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), RWing,
		ESlateDrawEffect::None, FSailSimStyle::Accent, true, 2.2f);
	DrawFilledDisc(Out, Layer + 2, Geo, C, 4.5f, FSailSimStyle::GaugeHubOuterBrush());
	DrawFilledDisc(Out, Layer + 3, Geo, C, 2.4f, FSailSimStyle::GaugeHubBrush());
}

void SSailGauge::PaintPitch(const FGeometry& Geo, FSlateWindowElementList& Out, int32 Layer, float Pitch) const
{
	const FVector2D Sz = Geo.GetLocalSize();
	const float Side = FMath::Min(Sz.X, Sz.Y);
	const FVector2D C = Sz * 0.5f;
	const float R = Side * 0.38f;
	const float ClampP = FMath::Clamp(Pitch, -18.f, 18.f);
	const float Rad = FMath::DegreesToRadians(-ClampP);
	const FVector2D Dir(FMath::Cos(Rad), FMath::Sin(Rad));
	const FVector2D Nrm(-Dir.Y, Dir.X);

	for (int32 Deg = -15; Deg <= 15; Deg += 5)
	{
		if (Deg == 0) continue;
		const float Off = float(Deg) / 18.f * (R * 0.65f);
		const FVector2D Mid = C + Nrm * Off;
		const float Half = (FMath::Abs(Deg) >= 10) ? R * 0.26f : R * 0.16f;
		TArray<FVector2D> Mark = { Mid - Dir * Half, Mid + Dir * Half };
		FSlateDrawElement::MakeLines(Out, Layer, Geo.ToPaintGeometry(), Mark,
			ESlateDrawEffect::None, FLinearColor(0.7f, 0.78f, 0.82f, 0.45f), true, 1.2f);
	}

	TArray<FVector2D> Horizon = { C - Dir * (R * 0.82f), C + Dir * (R * 0.82f) };
	FSlateDrawElement::MakeLines(Out, Layer + 1, Geo.ToPaintGeometry(), Horizon,
		ESlateDrawEffect::None, FLinearColor(0.85f, 0.93f, 0.95f, 0.95f), true, 2.2f);

	TArray<FVector2D> LWing = { C + FVector2D(-R * 0.38f, 0.f), C + FVector2D(-R * 0.10f, 0.f) };
	TArray<FVector2D> RWing = { C + FVector2D(R * 0.10f, 0.f), C + FVector2D(R * 0.38f, 0.f) };
	FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), LWing,
		ESlateDrawEffect::None, FSailSimStyle::Accent, true, 2.2f);
	FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), RWing,
		ESlateDrawEffect::None, FSailSimStyle::Accent, true, 2.2f);

	TArray<FVector2D> Nose = {
		C + FVector2D(R * 0.46f, -3.5f),
		C + FVector2D(R * 0.58f, 0.f),
		C + FVector2D(R * 0.46f, 3.5f)
	};
	FSlateDrawElement::MakeLines(Out, Layer + 2, Geo.ToPaintGeometry(), Nose,
		ESlateDrawEffect::None, FSailSimStyle::Accent, true, 1.6f);

	DrawFilledDisc(Out, Layer + 3, Geo, C, 4.5f, FSailSimStyle::GaugeHubOuterBrush());
	DrawFilledDisc(Out, Layer + 4, Geo, C, 2.4f, FSailSimStyle::GaugeHubBrush());
}
