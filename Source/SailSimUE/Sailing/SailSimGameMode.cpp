#include "Sailing/SailSimGameMode.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/SailSimHUD.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"

ASailSimGameMode::ASailSimGameMode()
{
	DefaultPawnClass = ASailBoatPawn::StaticClass();
	HUDClass = ASailSimHUD::StaticClass();
}

void ASailSimGameMode::DestroyLevelPlacedBoats()
{
	UWorld* World = GetWorld();
	if (!World) return;

	TArray<ASailBoatPawn*> ToDestroy;
	for (TActorIterator<ASailBoatPawn> It(World); It; ++It)
	{
		ToDestroy.Add(*It);
	}
	for (ASailBoatPawn* Boat : ToDestroy)
	{
		if (IsValid(Boat))
		{
			Boat->Destroy();
		}
	}

	// Default water-brush landscape island at origin: hide in game so we don't
	// look like we're "on land" even when floated offshore.
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(true);
		It->SetActorEnableCollision(false);
	}
}

void ASailSimGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// Too early for actors in some paths — also clean in RestartPlayer.
}

void ASailSimGameMode::RestartPlayer(AController* NewPlayer)
{
	// Remove any boats already in the map (placed for editing / previous MCP spawn)
	// before Super spawns the one DefaultPawn for this player.
	DestroyLevelPlacedBoats();
	Super::RestartPlayer(NewPlayer);
}

AActor* ASailSimGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// Prefer a PlayerStart if present; otherwise Super (may be null → spawn at origin
	// which SailBoatPawn then relocates to open water).
	return Super::ChoosePlayerStart_Implementation(Player);
}
