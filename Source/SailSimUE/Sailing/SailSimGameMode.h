#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SailSimGameMode.generated.h"

UCLASS()
class SAILSIMUE_API ASailSimGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASailSimGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;

protected:
	/** Strip level-placed boats so only the GameMode-spawned pawn remains (avoids double boats in PIE). */
	void DestroyLevelPlacedBoats();
};
