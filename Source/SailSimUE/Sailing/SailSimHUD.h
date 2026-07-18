#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SailSimHUD.generated.h"

class ASailBoatPawn;
class SSailSimChrome;

/**
 * Hosts the modern Slate sailing chrome (rounded glass instruments).
 * Canvas is unused for chrome — Slate scales correctly on Retina.
 */
UCLASS()
class SAILSIMUE_API ASailSimHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	void EnsureChrome();
	void EnsureMouseForUI();
	ASailBoatPawn* FindBoat() const;

	TSharedPtr<SSailSimChrome> Chrome;
	TWeakObjectPtr<ASailBoatPawn> BoundBoat;
};
