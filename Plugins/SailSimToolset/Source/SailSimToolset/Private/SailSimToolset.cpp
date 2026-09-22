// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolset.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "IAssetViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/SailSimPerf.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/SViewport.h"

namespace SailSimToolsetPrivate
{
	static bool ResolvePlayerView(UWorld* PlayWorld, FVector& OutLoc, FRotator& OutRot, FString& OutSource)
	{
		if (!PlayWorld)
		{
			return false;
		}

		// Prefer possessed pawn camera component (ASailBoatPawn Camera/SpringArm are protected —
		// resolve via FindComponentByClass so we do not need friendship).
		if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				if (UCameraComponent* Cam = Pawn->FindComponentByClass<UCameraComponent>())
				{
					OutLoc = Cam->GetComponentLocation();
					OutRot = Cam->GetComponentRotation();
					OutSource = FString::Printf(TEXT("%s::UCameraComponent"), *Pawn->GetClass()->GetName());
					return true;
				}
				if (USpringArmComponent* Arm = Pawn->FindComponentByClass<USpringArmComponent>())
				{
					OutLoc = Arm->GetComponentLocation();
					OutRot = Arm->GetComponentRotation();
					OutSource = FString::Printf(TEXT("%s::USpringArmComponent"), *Pawn->GetClass()->GetName());
					return true;
				}
			}

			PC->GetPlayerViewPoint(OutLoc, OutRot);
			OutSource = TEXT("PlayerController::GetPlayerViewPoint");
			return true;
		}

		for (TActorIterator<ASailBoatPawn> It(PlayWorld); It; ++It)
		{
			ASailBoatPawn* Boat = *It;
			if (!IsValid(Boat))
			{
				continue;
			}
			if (UCameraComponent* Cam = Boat->FindComponentByClass<UCameraComponent>())
			{
				OutLoc = Cam->GetComponentLocation();
				OutRot = Cam->GetComponentRotation();
				OutSource = FString::Printf(TEXT("ASailBoatPawn::Camera(unpossessed:%s)"), *Boat->GetName());
				return true;
			}
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

	static bool ScreenshotWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutBitmap, FIntPoint& OutSize)
	{
		FIntVector Size3(0, 0, 0);
		if (!FSlateApplication::Get().TakeScreenshot(Widget, OutBitmap, Size3))
		{
			return false;
		}
		OutSize = FIntPoint(Size3.X, Size3.Y);
		return OutSize.X > 0 && OutSize.Y > 0 && OutBitmap.Num() == OutSize.X * OutSize.Y;
	}

	/** Live PIE game viewport widget — what the player actually sees (not free-editor cam). */
	static bool CapturePieGameViewport(TArray<FColor>& OutBitmap, FIntPoint& OutSize, FString& OutPath)
	{
		if (GEngine && GEngine->GameViewport)
		{
			TSharedPtr<SViewport> GameWidget = GEngine->GameViewport->GetGameViewportWidget();
			if (GameWidget.IsValid())
			{
				if (ScreenshotWidget(GameWidget.ToSharedRef(), OutBitmap, OutSize))
				{
					OutPath = TEXT("GameViewportWidget");
					return true;
				}
			}

			if (FViewport* V = GEngine->GameViewport->Viewport)
			{
				OutSize = V->GetSizeXY();
				if (OutSize.X > 0 && OutSize.Y > 0)
				{
					V->Draw();
					if (GetViewportScreenShot(V, OutBitmap) && OutBitmap.Num() == OutSize.X * OutSize.Y)
					{
						OutPath = TEXT("GameViewport::Viewport");
						return true;
					}
				}
			}
		}

		// PIE-in-level-editor: find the play-in-editor slate viewport.
		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditor =
				FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			TSharedPtr<IAssetViewport> Active = LevelEditor.GetFirstActiveViewport();
			if (Active.IsValid())
			{
				if (FViewport* V = Active->GetActiveViewport())
				{
					// When PlayWorld is live, the active level viewport is usually the PIE view.
					OutSize = V->GetSizeXY();
					if (OutSize.X > 0 && OutSize.Y > 0)
					{
						V->Draw();
						OutBitmap.Reset();
						if (GetViewportScreenShot(V, OutBitmap) && OutBitmap.Num() == OutSize.X * OutSize.Y)
						{
							OutPath = TEXT("LevelEditor::ActiveViewport");
							return true;
						}
					}
				}
			}
		}

		return false;
	}

	static FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}
}

FToolsetImage USailSimToolset::CapturePlayerView()
{
	FToolsetImage Out;

	UWorld* PlayWorld = (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld.Get() : nullptr;
	if (!PlayWorld)
	{
		UKismetSystemLibrary::RaiseScriptError(
			TEXT("CapturePlayerView requires PIE — call EnsurePIE first (or press Play)."));
		return Out;
	}

	FVector CamLoc;
	FRotator CamRot;
	FString CamSource;
	const bool bHaveCam = SailSimToolsetPrivate::ResolvePlayerView(PlayWorld, CamLoc, CamRot, CamSource);

	TArray<FColor> Bitmap;
	FIntPoint Size(0, 0);
	FString Path;
	if (!SailSimToolsetPrivate::CapturePieGameViewport(Bitmap, Size, Path))
	{
		UKismetSystemLibrary::RaiseScriptError(
			TEXT("CapturePlayerView: no PIE game viewport to screenshot (is PIE-in-viewport active?)."));
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

	if (bHaveCam)
	{
		UE_LOG(LogTemp, Log,
			TEXT("CapturePlayerView OK path=%s cam=%s loc=(%.0f,%.0f,%.0f) %dx%d"),
			*Path, *CamSource, CamLoc.X, CamLoc.Y, CamLoc.Z, Size.X, Size.Y);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("CapturePlayerView OK path=%s (no boat cam resolved) %dx%d"),
			*Path, Size.X, Size.Y);
	}
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
		bool bSettled = false;
		FString PawnName;
		if (APlayerController* PC = GEditor->PlayWorld->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				bSettled = true;
				PawnName = Pawn->GetName();
			}
		}

		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("IsPIERunning"), true);
		Obj->SetBoolField(TEXT("AlreadyRunning"), true);
		Obj->SetBoolField(TEXT("Settled"), bSettled);
		if (!PawnName.IsEmpty())
		{
			Obj->SetStringField(TEXT("Pawn"), PawnName);
		}
		return SailSimToolsetPrivate::ToJson(Obj);
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

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("IsPIERunning"), false);
	Obj->SetBoolField(TEXT("Requested"), true);
	Obj->SetBoolField(TEXT("AlreadyRunning"), false);
	Obj->SetBoolField(TEXT("Settled"), false);
	Obj->SetStringField(TEXT("Hint"), TEXT("Poll EnsurePIE until Settled=true before CapturePlayerView"));
	return SailSimToolsetPrivate::ToJson(Obj);
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
	return SailSimToolsetPrivate::ToJson(Obj);
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
	return SailSimToolsetPrivate::ToJson(Obj);
}

FString USailSimToolset::GetPerfSnapshot()
{
	const FSailSimPerf& P = FSailSimPerf::Get();
	const float FrameMs = P.WallFrameEmaMs;
	const float Fps = FrameMs > 0.1f ? (1000.f / FrameMs) : 0.f;

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("fps"), Fps);
	Obj->SetNumberField(TEXT("frameMs"), FrameMs);
	Obj->SetNumberField(TEXT("gpuMs"), P.GpuFrameEmaMs);
	Obj->SetNumberField(TEXT("gtMs"), P.GameThreadEmaMs);
	Obj->SetNumberField(TEXT("rtMs"), P.RenderThreadEmaMs);
	Obj->SetNumberField(TEXT("rhiMs"), P.RhiThreadEmaMs);
	Obj->SetNumberField(TEXT("moored"), P.MooredCount);
	Obj->SetNumberField(TEXT("aids"), P.AidCount);
	Obj->SetNumberField(TEXT("tiles"), P.TerrainTiles);
	Obj->SetNumberField(TEXT("structureTiles"), P.StructureTiles);
	Obj->SetStringField(TEXT("bottleneck"), P.BottleneckLabel);
	Obj->SetBoolField(TEXT("isPIERunning"), GEditor && GEditor->PlayWorld != nullptr);
	return SailSimToolsetPrivate::ToJson(Obj);
}

FString USailSimToolset::SetCVars(const FString& CVarsText)
{
	TArray<FString> Lines;
	CVarsText.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);

	// Also allow semicolon-separated on one line.
	TArray<FString> Pairs;
	for (const FString& Line : Lines)
	{
		TArray<FString> Semi;
		Line.ParseIntoArray(Semi, TEXT(";"), true);
		Pairs.Append(Semi);
	}

	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Failed;

	for (FString Pair : Pairs)
	{
		Pair.TrimStartAndEndInline();
		if (Pair.IsEmpty() || Pair.StartsWith(TEXT("#")))
		{
			continue;
		}

		FString Name, Value;
		if (!Pair.Split(TEXT("="), &Name, &Value))
		{
			Failed.Add(MakeShared<FJsonValueString>(Pair + TEXT(" (expected name=value)")));
			continue;
		}
		Name.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();

		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!CVar)
		{
			Failed.Add(MakeShared<FJsonValueString>(Name + TEXT(" (not found)")));
			continue;
		}
		CVar->Set(*Value, ECVF_SetByCode);
		Applied.Add(MakeShared<FJsonValueString>(Name + TEXT("=") + Value));
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), Failed.Num() == 0);
	Obj->SetArrayField(TEXT("applied"), Applied);
	Obj->SetArrayField(TEXT("failed"), Failed);
	return SailSimToolsetPrivate::ToJson(Obj);
}

FString USailSimToolset::ExecuteConsole(const FString& Command)
{
	const FString Cmd = Command.TrimStartAndEnd();
	if (Cmd.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ExecuteConsole: empty command."));
		return TEXT("{\"ok\":false}");
	}

	UWorld* World = SailSimToolsetPrivate::GetSearchWorld();
	bool bOk = false;
	if (GEngine)
	{
		bOk = GEngine->Exec(World, *Cmd);
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), bOk);
	Obj->SetStringField(TEXT("command"), Cmd);
	Obj->SetStringField(TEXT("world"), World ? World->GetName() : FString(TEXT("none")));
	return SailSimToolsetPrivate::ToJson(Obj);
}

FString USailSimToolset::ProfileGPUDump()
{
	UWorld* World = SailSimToolsetPrivate::GetSearchWorld();
	if (GEngine)
	{
		GEngine->Exec(World, TEXT("ProfileGPU"));
	}

	const FSailSimPerf& P = FSailSimPerf::Get();
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetStringField(TEXT("command"), TEXT("ProfileGPU"));
	Obj->SetStringField(TEXT("profilingDir"), FPaths::ProjectSavedDir() / TEXT("Profiling"));
	Obj->SetStringField(TEXT("hint"),
		TEXT("Structured SLW/Lumen/shadows are in the ProfileGPU OutputLog dump; snapshot below is FSailSimPerf."));
	Obj->SetNumberField(TEXT("frameMs"), P.WallFrameEmaMs);
	Obj->SetNumberField(TEXT("gpuMs"), P.GpuFrameEmaMs);
	Obj->SetNumberField(TEXT("moored"), P.MooredCount);
	Obj->SetNumberField(TEXT("aids"), P.AidCount);
	Obj->SetNumberField(TEXT("tiles"), P.TerrainTiles);
	return SailSimToolsetPrivate::ToJson(Obj);
}
