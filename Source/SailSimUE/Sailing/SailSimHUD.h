#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SailSimHUD.generated.h"

class ASailBoatPawn;

/**
 * Phase-2 instrument panel (Canvas HUD).
 * Replaces GEngine on-screen debug messages with a stable sailing readout.
 */
UCLASS()
class SAILSIMUE_API ASailSimHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

protected:
	void DrawPanelBackground(float X, float Y, float W, float H);
	void DrawLineText(float X, float Y, const FString& Text, const FLinearColor& Color, float Scale = 1.15f);
	ASailBoatPawn* FindBoat() const;
};
