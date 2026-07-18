#include "Sailing/SailSimHUD.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/UI/SSailSimChrome.h"
#include "Sailing/SailSimPerf.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Widgets/SWeakWidget.h"

void ASailSimHUD::BeginPlay()
{
	Super::BeginPlay();
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	EnsureMouseForUI();
	EnsureChrome();
}

void ASailSimHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Chrome.IsValid())
	{
		// Stop-PIE / quit often kills the widget before the debounced save fires.
		Chrome->ForceSavePrefsNow();
		if (GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(Chrome.ToSharedRef());
		}
		Chrome.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void ASailSimHUD::EnsureMouseForUI()
{
	if (APlayerController* PC = GetOwningPlayerController())
	{
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
	}
}

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

void ASailSimHUD::EnsureChrome()
{
	if (Chrome.IsValid()) return;
	if (!GEngine || !GEngine->GameViewport) return;

	ASailBoatPawn* Boat = FindBoat();
	BoundBoat = Boat;

	TWeakObjectPtr<UGameInstance> GI;
	if (UWorld* World = GetWorld())
	{
		GI = World->GetGameInstance();
	}

	SAssignNew(Chrome, SSailSimChrome)
		.Boat(BoundBoat)
		.GameInstance(GI);

	// High Z so chrome sits above world; game input still works under GameAndUI.
	GEngine->GameViewport->AddViewportWidgetContent(
		SNew(SWeakWidget).PossiblyNullContent(Chrome), 100);
}

void ASailSimHUD::Tick(float DeltaSeconds)
{
	SAIL_PERF_SCOPE(HUD);
	Super::Tick(DeltaSeconds);

	if (!Chrome.IsValid())
	{
		EnsureChrome();
		return;
	}

	ASailBoatPawn* Boat = FindBoat();
	if (Boat != BoundBoat.Get())
	{
		BoundBoat = Boat;
		Chrome->SetBoat(BoundBoat);
	}
}
