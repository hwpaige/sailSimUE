// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolset.h"

#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "IAssetViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Misc/ScopeExit.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

namespace SailSimToolsetPrivate
{
	static bool ResolvePlayerView(UWorld* PlayWorld, FVector& OutLoc, FRotator& OutRot, FString& OutSource)
	{
		if (!PlayWorld)
		{
			return false;
		}

		if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
		{
			PC->GetPlayerViewPoint(OutLoc, OutRot);
			OutSource = TEXT("PlayerController::GetPlayerViewPoint");
			return true;
		}

		for (TActorIterator<APawn> It(PlayWorld); It; ++It)
		{
			APawn* Pawn = *It;
			if (!IsValid(Pawn) || !Pawn->IsPlayerControlled())
			{
				continue;
			}
			if (UCameraComponent* Cam = Pawn->FindComponentByClass<UCameraComponent>())
			{
				OutLoc = Cam->GetComponentLocation();
				OutRot = Cam->GetComponentRotation();
				OutSource = FString::Printf(TEXT("PawnCamera:%s"), *Pawn->GetName());
				return true;
			}
			OutLoc = Pawn->GetActorLocation() + FVector(0.f, 0.f, 150.f);
			OutRot = Pawn->GetActorRotation();
			OutSource = FString::Printf(TEXT("PawnFallback:%s"), *Pawn->GetName());
			return true;
		}
		return false;
	}

	static UWorld* GetSearchWorld()
	{
		if (GEditor && GEditor->PlayWorld)
		{
			return GEditor->PlayWorld;
		}
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}
}

FToolsetImage USailSimToolset::CapturePlayerView()
{
	FToolsetImage Out;

	if (!GEditor || !GEditor->PlayWorld)
	{
		UKismetSystemLibrary::RaiseScriptError(
			TEXT("CapturePlayerView requires PIE — call EnsurePIE / StartPIE first (or press Play)."));
		return Out;
	}

	FVector CamLoc;
	FRotator CamRot;
	FString Source;
	if (!SailSimToolsetPrivate::ResolvePlayerView(GEditor->PlayWorld, CamLoc, CamRot, Source))
	{
		UKismetSystemLibrary::RaiseScriptError(
			TEXT("CapturePlayerView: no player controller / possessed pawn camera in PIE."));
		return Out;
	}

	FEditorViewportClient* ViewportClient = GCurrentLevelEditingViewportClient;
	if (!ViewportClient || !ViewportClient->Viewport)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView: no level viewport client."));
		return Out;
	}

	FViewport* Viewport = ViewportClient->Viewport;
	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView: viewport has zero size."));
		return Out;
	}

	const FVector SavedLoc = ViewportClient->GetViewLocation();
	const FRotator SavedRot = ViewportClient->GetViewRotation();
	const bool bSavedModeWidgets = ViewportClient->EngineShowFlags.ModeWidgets != 0;
	const bool bSavedSelectionOutline = ViewportClient->EngineShowFlags.SelectionOutline != 0;
	const bool bSavedSelection = ViewportClient->EngineShowFlags.Selection != 0;

	ViewportClient->SetViewLocation(CamLoc);
	ViewportClient->SetViewRotation(CamRot);
	ViewportClient->EngineShowFlags.SetModeWidgets(false);
	ViewportClient->EngineShowFlags.SetSelectionOutline(false);
	ViewportClient->EngineShowFlags.SetSelection(false);

	ON_SCOPE_EXIT
	{
		ViewportClient->SetViewLocation(SavedLoc);
		ViewportClient->SetViewRotation(SavedRot);
		ViewportClient->EngineShowFlags.SetModeWidgets(bSavedModeWidgets);
		ViewportClient->EngineShowFlags.SetSelectionOutline(bSavedSelectionOutline);
		ViewportClient->EngineShowFlags.SetSelection(bSavedSelection);
	};

	Viewport->Draw();

	TArray<FColor> Bitmap;
	if (!GetViewportScreenShot(Viewport, Bitmap))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView: GetViewportScreenShot failed."));
		return Out;
	}
	for (FColor& Pixel : Bitmap)
	{
		Pixel.A = 255;
	}

	if (!Out.SetFromBitmap(Bitmap, Size))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView: failed to encode PNG."));
		return Out;
	}

	UE_LOG(LogTemp, Log,
		TEXT("CapturePlayerView OK source=%s loc=(%.0f,%.0f,%.0f) rot=(%.1f,%.1f,%.1f) %dx%d"),
		*Source, CamLoc.X, CamLoc.Y, CamLoc.Z, CamRot.Pitch, CamRot.Yaw, CamRot.Roll, Size.X, Size.Y);
	return Out;
}

FString USailSimToolset::FindActorsByName(const FString& Query, int32 MaxResults)
{
	UWorld* World = SailSimToolsetPrivate::GetSearchWorld();
	if (!World)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("FindActorsByName: no world."));
		return TEXT("[]");
	}

	const FString Needle = Query.TrimStartAndEnd();
	if (Needle.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("FindActorsByName: Query is empty."));
		return TEXT("[]");
	}

	const int32 Cap = FMath::Clamp(MaxResults <= 0 ? 50 : MaxResults, 1, 200);
	TArray<TSharedPtr<FJsonValue>> Arr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		const FString Name = Actor->GetName();
		const FString Label = Actor->GetActorLabel();
		const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
		const FString ClassNameA = ClassName.StartsWith(TEXT("A")) ? ClassName : (TEXT("A") + ClassName);

		const bool bHit =
			Name.Contains(Needle, ESearchCase::IgnoreCase) ||
			Label.Contains(Needle, ESearchCase::IgnoreCase) ||
			ClassName.Contains(Needle, ESearchCase::IgnoreCase) ||
			ClassNameA.Contains(Needle, ESearchCase::IgnoreCase);

		if (!bHit)
		{
			continue;
		}

		const FVector L = Actor->GetActorLocation();
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("label"), Label);
		Obj->SetStringField(TEXT("class"), ClassName);
		Obj->SetNumberField(TEXT("x"), L.X);
		Obj->SetNumberField(TEXT("y"), L.Y);
		Obj->SetNumberField(TEXT("z"), L.Z);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));

		if (Arr.Num() >= Cap)
		{
			break;
		}
	}

	FString Out;
	const TSharedRef<FJsonValueArray> Root = MakeShared<FJsonValueArray>(Arr);
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, TEXT(""), Writer);
	return Out;
}

FString USailSimToolset::EnsurePIE()
{
	if (!GEditor)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("EnsurePIE: editor not available."));
		return TEXT("{\"IsPIERunning\":false,\"Error\":\"no editor\"}");
	}

	if (GEditor->PlayWorld)
	{
		return TEXT("{\"IsPIERunning\":true,\"AlreadyRunning\":true}");
	}

	FRequestPlaySessionParams Params;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;

	if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
	{
		FLevelEditorModule& LevelEditorModule =
			FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();
		if (ActiveLevelViewport.IsValid())
		{
			Params.DestinationSlateViewport = ActiveLevelViewport;
		}
	}

	GEditor->RequestPlaySession(Params);
	return TEXT("{\"IsPIERunning\":false,\"Requested\":true,\"AlreadyRunning\":false}");
}

FString USailSimToolset::GetLevelPath()
{
	FString EditorLevel;
	FString PIELevel;
	const bool bPIE = GEditor && GEditor->PlayWorld != nullptr;

	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			EditorLevel = EditorWorld->GetOutermost()->GetName();
		}
		if (bPIE && GEditor->PlayWorld)
		{
			PIELevel = GEditor->PlayWorld->GetOutermost()->GetName();
		}
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("EditorLevel"), EditorLevel);
	Obj->SetStringField(TEXT("PIELevel"), PIELevel);
	Obj->SetBoolField(TEXT("IsPIERunning"), bPIE);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

FString USailSimToolset::GetPlayerCameraTransform()
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetPlayerCameraTransform requires PIE."));
		return TEXT("{}");
	}

	FVector Loc;
	FRotator Rot;
	FString Source;
	if (!SailSimToolsetPrivate::ResolvePlayerView(GEditor->PlayWorld, Loc, Rot, Source))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetPlayerCameraTransform: no player view."));
		return TEXT("{}");
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("x"), Loc.X);
	Obj->SetNumberField(TEXT("y"), Loc.Y);
	Obj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	Obj->SetNumberField(TEXT("roll"), Rot.Roll);
	Obj->SetStringField(TEXT("source"), Source);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}
