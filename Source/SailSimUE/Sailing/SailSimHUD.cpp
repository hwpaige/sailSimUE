#include "Sailing/SailSimHUD.h"
#include "Sailing/SailBoatPawn.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "CanvasItem.h"

ASailBoatPawn* ASailSimHUD::FindBoat() const
{
	if (APlayerController* PC = GetOwningPlayerController())
	{
		if (ASailBoatPawn* PawnBoat = Cast<ASailBoatPawn>(PC->GetPawn()))
		{
			return PawnBoat;
		}
	}
	UWorld* World = GetWorld();
	if (!World) return nullptr;
	for (TActorIterator<ASailBoatPawn> It(World); It; ++It)
	{
		if (It->IsPlayerControlled())
		{
			return *It;
		}
	}
	return nullptr;
}

void ASailSimHUD::DrawPanelBackground(float X, float Y, float W, float H)
{
	if (!Canvas) return;
	FCanvasTileItem Tile(FVector2D(X, Y), FVector2D(W, H), FLinearColor(0.02f, 0.04f, 0.07f, 0.72f));
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
}

void ASailSimHUD::DrawLineText(float X, float Y, const FString& Text, const FLinearColor& Color, float Scale)
{
	if (!Canvas) return;
	FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Text), GEngine->GetSmallFont(), Color);
	Item.Scale = FVector2D(Scale, Scale);
	Item.bOutlined = true;
	Item.OutlineColor = FLinearColor(0.f, 0.f, 0.f, 0.85f);
	Canvas->DrawItem(Item);
}

void ASailSimHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas) return;

	ASailBoatPawn* Boat = FindBoat();
	if (!Boat) return;

	const float Margin = 18.f;
	const float PanelW = 420.f;
	const float LineH = 20.f;
	const int32 Lines = 10;
	const float PanelH = 28.f + Lines * LineH;
	const float X = Margin;
	const float Y = Margin;

	DrawPanelBackground(X, Y, PanelW, PanelH);

	const FLinearColor Title(0.55f, 0.85f, 1.f, 1.f);
	const FLinearColor Body(0.92f, 0.95f, 0.98f, 1.f);
	const FLinearColor Dim(0.7f, 0.75f, 0.8f, 1.f);
	const FLinearColor Accent(0.35f, 0.95f, 0.55f, 1.f);
	const FLinearColor Warn(1.f, 0.75f, 0.35f, 1.f);

	float Row = Y + 10.f;
	DrawLineText(X + 12.f, Row, FString::Printf(TEXT("SAILSIM  ·  %s"), *Boat->GetSpecSummary()), Title, 1.15f);
	Row += LineH + 4.f;

	DrawLineText(X + 12.f, Row, FString::Printf(
		TEXT("SPD  %5.1f kn     HEEL  %+5.1f°     HDG  %5.1f°"),
		Boat->GetSpeedKnots(), Boat->GetHeelDeg(), Boat->GetHeadingDeg()), Body);
	Row += LineH;

	DrawLineText(X + 12.f, Row, FString::Printf(
		TEXT("RUD  %+5.0f°      SHEET %4.0f%%      %s"),
		Boat->GetRudderStarboardDeg(),
		Boat->GetSheetEase() * 100.f,
		Boat->IsAutoHeading() ? TEXT("AUTO") : TEXT("HELM")), Body);
	Row += LineH;

	DrawLineText(X + 12.f, Row, FString::Printf(
		TEXT("AWA  %+5.0f°      AWS  %5.1f kn"),
		Boat->GetApparentWindAngleDeg(), Boat->GetApparentWindSpeedKn()), Body);
	Row += LineH;

	DrawLineText(X + 12.f, Row, FString::Printf(
		TEXT("TWS  %5.0f kn     TWD  %5.0f°      %s"),
		Boat->GetTrueWindSpeedKn(),
		Boat->GetTrueWindDirDeg(),
		Boat->IsSailing() ? TEXT("SAILING") : TEXT("DRIFT")),
		Boat->IsSailing() ? Accent : Warn);
	Row += LineH + 4.f;

	DrawLineText(X + 12.f, Row, TEXT("A/D helm · W/S sheet · RMB orbit · wheel zoom"), Dim, 1.0f);
	Row += LineH;
	DrawLineText(X + 12.f, Row, TEXT("[ ] TWS · 1-4 preset · -/= scale · R re-loft (python)"), Dim, 1.0f);
}
