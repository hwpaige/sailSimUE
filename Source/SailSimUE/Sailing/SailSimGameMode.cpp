#include "Sailing/SailSimGameMode.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/SailSimHUD.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Nav/NavGeo.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
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

	// Destroy every boat that is not the GameMode session boat.
	// Map-placed SailBoatPawns at origin were being re-possessed → "start on land".
	TArray<ASailBoatPawn*> ToDestroy;
	for (TActorIterator<ASailBoatPawn> It(World); It; ++It)
	{
		if (!It->bPlayerSessionBoat)
		{
			ToDestroy.Add(*It);
		}
	}
	for (ASailBoatPawn* Boat : ToDestroy)
	{
		if (IsValid(Boat))
		{
			if (AController* C = Boat->GetController())
			{
				C->UnPossess();
			}
			UE_LOG(LogSailSim, Log, TEXT("GameMode destroying level boat %s"), *GetNameSafe(Boat));
			Boat->Destroy();
		}
	}

	// Flat open ocean as early as possible (hide island + one Water plane).
	if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const ASailBoatPawn* CDO = GetDefault<ASailBoatPawn>();
		const FVector2D Harbor = FNavGeo::BoatStartWorldCm2D();
		const FVector Hint(
			CDO ? CDO->OpenWaterSpawnXY.X : Harbor.X,
			CDO ? CDO->OpenWaterSpawnXY.Y : Harbor.Y,
			0.f);
		Ocean->PrepareOpenOcean(Hint, 240000.f, 120000.f);
	}
}

void ASailSimGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// Too early for actors in some paths — also clean in RestartPlayer.
}

void ASailSimGameMode::RestartPlayer(AController* NewPlayer)
{
	DestroyLevelPlacedBoats();
	Super::RestartPlayer(NewPlayer);
	// Clean again after spawn — catches any WP race.
	DestroyLevelPlacedBoats();
	if (NewPlayer && !NewPlayer->GetPawn())
	{
		UE_LOG(LogSailSim, Error, TEXT("RestartPlayer: still no pawn after spawn — check collision / DefaultPawnClass"));
	}
	else if (ASailBoatPawn* Boat = Cast<ASailBoatPawn>(NewPlayer ? NewPlayer->GetPawn() : nullptr))
	{
		Boat->bPlayerSessionBoat = true;
		Boat->SnapToWaterSurface(true);
	}
}

APawn* ASailSimGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	// Spawn already at open-water XY so we never start on the island PlayerStart.
	FTransform SpawnXf = SpawnTransform;
	if (const ASailBoatPawn* CDO = GetDefault<ASailBoatPawn>())
	{
		FVector Loc = SpawnXf.GetLocation();
		Loc.X = CDO->OpenWaterSpawnXY.X;
		Loc.Y = CDO->OpenWaterSpawnXY.Y;
		// Z will be corrected in SnapToWaterSurface; start slightly above plane.
		Loc.Z = FMath::Max(Loc.Z, 50.f);
		SpawnXf.SetLocation(Loc);
	}

	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	if (!PawnClass) return nullptr;

	// Deferred spawn so we mark player-session before BeginPlay.
	ASailBoatPawn* Boat = GetWorld()->SpawnActorDeferred<ASailBoatPawn>(
		PawnClass, SpawnXf, nullptr, GetInstigator(),
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Boat)
	{
		// Fallback for non-SailBoatPawn default classes
		FActorSpawnParameters Params;
		Params.Instigator = GetInstigator();
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APawn* Pawn = GetWorld()->SpawnActor<APawn>(PawnClass, SpawnXf, Params);
		if (!Pawn)
		{
			UE_LOG(LogSailSim, Error, TEXT("Failed to spawn default pawn of class %s"), *GetNameSafe(PawnClass));
		}
		return Pawn;
	}

	Boat->bPlayerSessionBoat = true;
	UGameplayStatics::FinishSpawningActor(Boat, SpawnXf);
	UE_LOG(LogSailSim, Log, TEXT("Spawned default pawn %s at %s"),
		*GetNameSafe(Boat), *SpawnXf.GetLocation().ToCompactString());
	return Boat;
}

AActor* ASailSimGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// Prefer a PlayerStart if present; otherwise Super (may be null → spawn at origin
	// which SailBoatPawn then relocates to open water).
	return Super::ChoosePlayerStart_Implementation(Player);
}
