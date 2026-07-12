#include "Sailing/SailSimGameMode.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/SailSimHUD.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "SailSimUE.h"

ASailSimGameMode::ASailSimGameMode()
{
	DefaultPawnClass = ASailBoatPawn::StaticClass();
	HUDClass = ASailSimHUD::StaticClass();
}

void ASailSimGameMode::DestroyLevelPlacedBoats()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Only destroy *level-placed* boats that are not player-possessed.
	// Spawning a new default pawn must not race with an already-possessed boat.
	TArray<ASailBoatPawn*> ToDestroy;
	for (TActorIterator<ASailBoatPawn> It(World); It; ++It)
	{
		if (!It->IsPlayerControlled())
		{
			ToDestroy.Add(*It);
		}
	}
	for (ASailBoatPawn* Boat : ToDestroy)
	{
		if (IsValid(Boat))
		{
			Boat->Destroy();
		}
	}

	// Hide the OpenWorld checkerboard landscape (M_ProcGrid).
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
	// Remove level-placed boats before Super spawns the DefaultPawn.
	DestroyLevelPlacedBoats();
	Super::RestartPlayer(NewPlayer);
	if (NewPlayer && !NewPlayer->GetPawn())
	{
		UE_LOG(LogSailSim, Error, TEXT("RestartPlayer: still no pawn after spawn — check collision / DefaultPawnClass"));
	}
}

APawn* ASailSimGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	// Always spawn; open-water relocate happens in SailBoatPawn::BeginPlay.
	FActorSpawnParameters Params;
	Params.Instigator = GetInstigator();
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer))
	{
		APawn* Pawn = GetWorld()->SpawnActor<APawn>(PawnClass, SpawnTransform, Params);
		if (Pawn)
		{
			UE_LOG(LogSailSim, Log, TEXT("Spawned default pawn %s at %s"),
				*GetNameSafe(Pawn), *SpawnTransform.GetLocation().ToCompactString());
		}
		else
		{
			UE_LOG(LogSailSim, Error, TEXT("Failed to spawn default pawn of class %s"), *GetNameSafe(PawnClass));
		}
		return Pawn;
	}
	return nullptr;
}

AActor* ASailSimGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// Prefer a PlayerStart if present; otherwise Super (may be null → spawn at origin
	// which SailBoatPawn then relocates to open water).
	return Super::ChoosePlayerStart_Implementation(Player);
}
