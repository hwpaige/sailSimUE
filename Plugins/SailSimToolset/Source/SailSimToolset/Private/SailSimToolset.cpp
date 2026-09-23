// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolset.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelStreaming.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "IAssetViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/CriticalSection.h"
#include "PlayInEditorDataTypes.h"
#include "RenderingThread.h"
#include "Sailing/SailSimPerf.h"
#include "Sailing/Nav/NavGeo.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "SceneManagement.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

namespace SailSimToolsetPrivate
{
	static FString JsonString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	static FString JsonArray(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		FString Out;
		const TSharedRef<FJsonValueArray> Root = MakeShared<FJsonValueArray>(Arr);
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, TEXT(""), Writer);
		return Out;
	}

	/** Ops artifact for console + MCP Prefer-ON gate. */
	static void PersistPreferOnGateJson(const FString& Json)
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SailSim"));
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Path = FPaths::Combine(Dir, TEXT("last_prefer_on_gate.json"));
		if (!FFileHelper::SaveStringToFile(Json, *Path))
		{
			UE_LOG(LogTemp, Warning, TEXT("RunPreferOnGate: failed to write %s"), *Path);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("RunPreferOnGate wrote %s"), *Path);
	}

	static bool IsSailBoatClass(const AActor* Actor)
	{
		const UClass* Cls = Actor ? Actor->GetClass() : nullptr;
		return Cls && Cls->GetName().Contains(TEXT("SailBoatPawn"), ESearchCase::IgnoreCase);
	}

	static bool ReadSessionBoatFlag(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		if (const FBoolProperty* Prop = FindFProperty<FBoolProperty>(Actor->GetClass(), TEXT("bPlayerSessionBoat")))
		{
			return Prop->GetPropertyValue_InContainer(Actor);
		}
		return false;
	}

	static UWorld* GetPlayWorld()
	{
		return (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld.Get() : nullptr;
	}

	static UWorld* GetSearchWorld()
	{
		if (UWorld* PlayWorld = GetPlayWorld())
		{
			return PlayWorld;
		}
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static const TCHAR* WorldKind(const UWorld* World)
	{
		if (!World)
		{
			return TEXT("None");
		}
		switch (World->WorldType)
		{
		case EWorldType::PIE: return TEXT("PIE");
		case EWorldType::Editor: return TEXT("Editor");
		case EWorldType::Game: return TEXT("Game");
		case EWorldType::EditorPreview: return TEXT("EditorPreview");
		default: return TEXT("Other");
		}
	}

	static bool IsStreamingBusy(const UWorld* World)
	{
		if (!World)
		{
			return false;
		}
		if (IsAsyncLoading())
		{
			return true;
		}
		for (const ULevelStreaming* Level : World->GetStreamingLevels())
		{
			if (Level && Level->ShouldBeLoaded() && !Level->IsLevelLoaded())
			{
				return true;
			}
		}
		return false;
	}

	/** Possessed session boat in the PIE world. Never an editor-world actor. */
	static AActor* FindPossessedSailBoat(UWorld* PlayWorld)
	{
		if (!PlayWorld)
		{
			return nullptr;
		}

		AActor* SessionBoat = nullptr;
		if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				if (IsSailBoatClass(Pawn))
				{
					return Pawn;
				}
			}
			if (AActor* ViewTarget = PC->GetViewTarget())
			{
				if (IsSailBoatClass(ViewTarget))
				{
					return ViewTarget;
				}
			}
		}

		for (TActorIterator<APawn> It(PlayWorld); It; ++It)
		{
			APawn* Pawn = *It;
			if (!IsValid(Pawn) || !IsSailBoatClass(Pawn))
			{
				continue;
			}
			if (Pawn->IsPlayerControlled())
			{
				return Pawn;
			}
			if (!SessionBoat && ReadSessionBoatFlag(Pawn))
			{
				SessionBoat = Pawn;
			}
		}
		return SessionBoat;
	}

	struct FBoomView
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float FOV = 72.f;
		FString Source;
		FString BoatName;
		FVector BoatLocation = FVector::ZeroVector;
		float EditorCameraDistance = -1.f;
	};

	/**
	 * Camera boom socket on the possessed boat. Does not read the editor viewport
	 * or PlayerCameraManager (those still track the free editor camera on Prefer-ON).
	 */
	static bool ResolveBoomView(UWorld* PlayWorld, FBoomView& Out, FString& OutError)
	{
		AActor* Boat = FindPossessedSailBoat(PlayWorld);
		if (!Boat)
		{
			OutError = TEXT("no possessed ASailBoatPawn in the PIE world");
			return false;
		}

		Out.BoatName = Boat->GetName();
		Out.BoatLocation = Boat->GetActorLocation();

		USpringArmComponent* Arm = Boat->FindComponentByClass<USpringArmComponent>();
		UCameraComponent* Cam = Boat->FindComponentByClass<UCameraComponent>();
		if (!Arm && !Cam)
		{
			OutError = TEXT("possessed boat has no spring arm or camera component");
			return false;
		}

		if (Arm)
		{
			// Push the socket out to TargetArmLength before we read it. A freshly
			// possessed pawn can still report the boom origin (inside the hull).
			Arm->TickComponent(0.016f, ELevelTick::LEVELTICK_All, nullptr);
			Out.Location = Arm->GetSocketLocation(USpringArmComponent::SocketName);
			Out.Rotation = Arm->GetSocketRotation(USpringArmComponent::SocketName);
			Out.Source = TEXT("SpringArmSocket");
			if (Cam)
			{
				Out.FOV = Cam->FieldOfView;
			}
		}
		else
		{
			Out.Location = Cam->GetComponentLocation();
			Out.Rotation = Cam->GetComponentRotation();
			Out.FOV = Cam->FieldOfView;
			Out.Source = TEXT("CameraComponent");
		}

		if (GCurrentLevelEditingViewportClient)
		{
			Out.EditorCameraDistance = FVector::Dist(
				Out.Location, GCurrentLevelEditingViewportClient->GetViewLocation());
		}

		const float BoomReach = FVector::Dist(Out.Location, Out.BoatLocation);
		if (BoomReach < 50.f)
		{
			OutError = FString::Printf(
				TEXT("camera boom is %.0fcm from the hull (not extended). source=%s"),
				BoomReach, *Out.Source);
			return false;
		}

		// Safety: a boom that landed on the free editor camera is the empty-grid shot.
		if (Out.EditorCameraDistance >= 0.f && Out.EditorCameraDistance < 10.f
			&& FVector::Dist(Out.BoatLocation, Out.Location) > 10000.f)
		{
			OutError = TEXT("resolved view matches the free editor camera, not the boat boom");
			return false;
		}

		return true;
	}

	static void AppendSettleFields(const TSharedRef<FJsonObject>& Obj, UWorld* PlayWorld, float MinWorldSeconds)
	{
		const bool bRunning = PlayWorld != nullptr;
		const double WorldSeconds = bRunning ? PlayWorld->GetTimeSeconds() : 0.0;
		const bool bStreaming = bRunning && IsStreamingBusy(PlayWorld);
		AActor* Boat = bRunning ? FindPossessedSailBoat(PlayWorld) : nullptr;
		const bool bTimeOk = MinWorldSeconds <= 0.f || WorldSeconds >= MinWorldSeconds;
		const bool bSettled = bRunning && bTimeOk && Boat != nullptr;

		FString Reason;
		if (!bRunning)
		{
			Reason = TEXT("PIE is not running");
		}
		else if (!Boat)
		{
			Reason = TEXT("PIE is running but no possessed ASailBoatPawn yet");
		}
		else if (!bTimeOk)
		{
			Reason = FString::Printf(
				TEXT("world time %.2fs is below settle threshold %.2fs"),
				WorldSeconds, MinWorldSeconds);
		}
		else
		{
			Reason = TEXT("settled");
		}

		Obj->SetBoolField(TEXT("IsPIERunning"), bRunning);
		Obj->SetBoolField(TEXT("Settled"), bSettled);
		Obj->SetBoolField(TEXT("HasPossessedBoat"), Boat != nullptr);
		Obj->SetBoolField(TEXT("Streaming"), bStreaming);
		Obj->SetNumberField(TEXT("WorldSeconds"), WorldSeconds);
		Obj->SetNumberField(TEXT("MinWorldSeconds"), MinWorldSeconds);
		Obj->SetStringField(TEXT("SettleReason"), Reason);
		if (Boat)
		{
			Obj->SetStringField(TEXT("Boat"), Boat->GetName());
		}
	}

	static bool bPieStartQueued = false;
	static double PieRequestedAtSeconds = 0.0;

	static FString RequestOrDescribePIE(float MinWorldSeconds)
	{
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tool"), TEXT("SailSimToolset"));

		if (!GEditor)
		{
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("code"), TEXT("no_editor"));
			Obj->SetStringField(TEXT("error"), TEXT("editor not available"));
			Obj->SetBoolField(TEXT("IsPIERunning"), false);
			Obj->SetBoolField(TEXT("Requested"), false);
			Obj->SetBoolField(TEXT("AlreadyRunning"), false);
			Obj->SetBoolField(TEXT("Settled"), false);
			return JsonString(Obj);
		}

		if (UWorld* PlayWorld = GetPlayWorld())
		{
			bPieStartQueued = false;
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetStringField(TEXT("code"), TEXT("running"));
			Obj->SetStringField(TEXT("error"), TEXT(""));
			Obj->SetBoolField(TEXT("Requested"), false);
			Obj->SetBoolField(TEXT("AlreadyRunning"), true);
			AppendSettleFields(Obj, PlayWorld, MinWorldSeconds);
			return JsonString(Obj);
		}

		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!EditorWorld)
		{
			Obj->SetBoolField(TEXT("ok"), false);
			Obj->SetStringField(TEXT("code"), TEXT("no_editor_world"));
			Obj->SetStringField(TEXT("error"), TEXT("no editor world to play"));
			Obj->SetBoolField(TEXT("IsPIERunning"), false);
			Obj->SetBoolField(TEXT("Requested"), false);
			Obj->SetBoolField(TEXT("AlreadyRunning"), false);
			Obj->SetBoolField(TEXT("Settled"), false);
			return JsonString(Obj);
		}

		const double Now = FPlatformTime::Seconds();
		if (bPieStartQueued && (Now - PieRequestedAtSeconds) < 5.0)
		{
			Obj->SetBoolField(TEXT("ok"), true);
			Obj->SetStringField(TEXT("code"), TEXT("already_requested"));
			Obj->SetStringField(TEXT("error"), TEXT(""));
			Obj->SetStringField(
				TEXT("message"),
				TEXT("PIE start is already queued. Call again after the editor ticks. This is not a failure."));
			Obj->SetBoolField(TEXT("IsPIERunning"), false);
			Obj->SetBoolField(TEXT("Requested"), true);
			Obj->SetBoolField(TEXT("AlreadyRunning"), false);
			Obj->SetBoolField(TEXT("Settled"), false);
			Obj->SetNumberField(TEXT("MinWorldSeconds"), MinWorldSeconds);
			return JsonString(Obj);
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
		bPieStartQueued = true;
		PieRequestedAtSeconds = Now;

		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("code"), TEXT("requested"));
		Obj->SetStringField(TEXT("error"), TEXT(""));
		Obj->SetStringField(
			TEXT("message"),
			TEXT("PIE start queued. Call again after the editor ticks until code is running. A null return is not used."));
		Obj->SetBoolField(TEXT("IsPIERunning"), false);
		Obj->SetBoolField(TEXT("Requested"), true);
		Obj->SetBoolField(TEXT("AlreadyRunning"), false);
		Obj->SetBoolField(TEXT("Settled"), false);
		Obj->SetNumberField(TEXT("MinWorldSeconds"), MinWorldSeconds);
		return JsonString(Obj);
	}

	static void ApplyLitGameShowFlags(FEngineShowFlags& ShowFlags)
	{
		ShowFlags = FEngineShowFlags(ESFIM_Game);
		ApplyViewMode(VMI_Lit, true, ShowFlags);
		ShowFlags.SetGrid(false);
		ShowFlags.SetModeWidgets(false);
		ShowFlags.SetSelection(false);
		ShowFlags.SetSelectionOutline(false);
		ShowFlags.SetPostProcessing(true);
		ShowFlags.SetLighting(true);
		ShowFlags.SetMaterials(true);
	}

	struct FRooted
	{
		UObject* Object = nullptr;

		explicit FRooted(UObject* InObject)
			: Object(InObject)
		{
			if (Object)
			{
				Object->AddToRoot();
			}
		}

		~FRooted()
		{
			if (Object)
			{
				Object->RemoveFromRoot();
			}
		}
	};

	static bool BitmapIsSentinel(const TArray<FColor>& Bitmap)
	{
		if (Bitmap.Num() == 0)
		{
			return true;
		}
		const int32 Step = FMath::Max(1, Bitmap.Num() / 4000);
		int32 Samples = 0;
		int32 Hits = 0;
		for (int32 Index = 0; Index < Bitmap.Num(); Index += Step)
		{
			const FColor& Pixel = Bitmap[Index];
			++Samples;
			if (Pixel.R > 200 && Pixel.B > 200 && Pixel.G < 48)
			{
				++Hits;
			}
		}
		return Samples > 0 && (Hits * 100) / Samples >= 90;
	}

	static void SplitBatch(const FString& Text, TArray<FString>& OutLines)
	{
		FString Normalized = Text;
		Normalized.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Normalized.ReplaceInline(TEXT("\r"), TEXT("\n"));
		if (!Normalized.Contains(TEXT("\n")) && Normalized.Contains(TEXT(";")))
		{
			Normalized.ReplaceInline(TEXT(";"), TEXT("\n"));
		}
		Normalized.ParseIntoArray(OutLines, TEXT("\n"), true);
		for (FString& Line : OutLines)
		{
			Line = Line.TrimStartAndEnd();
		}
		OutLines.RemoveAll([](const FString& Line)
		{
			return Line.IsEmpty() || Line.StartsWith(TEXT("#"));
		});
	}

	static UWorld* GetExecWorld()
	{
		if (UWorld* PlayWorld = GetPlayWorld())
		{
			return PlayWorld;
		}
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	struct FProfileEvent
	{
		int32 Depth = 0;
		float Ms = 0.f;
		FString Name;
		FString Bucket;
	};

	static bool ExtractMilliseconds(const FString& Line, int32 SearchFrom, float& OutMs, int32& OutMsTokenEnd)
	{
		int32 MsIdx = Line.Find(TEXT("ms"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
		if (MsIdx == INDEX_NONE)
		{
			return false;
		}
		int32 NumEnd = MsIdx;
		while (NumEnd > SearchFrom && FChar::IsWhitespace(Line[NumEnd - 1]))
		{
			--NumEnd;
		}
		int32 NumStart = NumEnd;
		while (NumStart > SearchFrom)
		{
			const TCHAR Ch = Line[NumStart - 1];
			if (!(FChar::IsDigit(Ch) || Ch == TEXT('.')))
			{
				break;
			}
			--NumStart;
		}
		if (NumStart >= NumEnd)
		{
			return false;
		}
		OutMs = FCString::Atof(*Line.Mid(NumStart, NumEnd - NumStart));
		OutMsTokenEnd = MsIdx + 2;
		return true;
	}

	static FString ClassifyGpuBucket(const FString& Name)
	{
		auto Has = [&Name](const TCHAR* Needle)
		{
			return Name.Contains(Needle, ESearchCase::IgnoreCase);
		};

		if (Has(TEXT("SingleLayerWater")) || Has(TEXT("SLW")) || Has(TEXT("Water")))
		{
			return TEXT("SingleLayerWater");
		}
		if (Has(TEXT("LumenReflection")))
		{
			return TEXT("LumenReflections");
		}
		if (Has(TEXT("Nanite")))
		{
			return TEXT("Nanite");
		}
		if (Has(TEXT("Shadow")) || Has(TEXT("VSM")))
		{
			return TEXT("Shadows");
		}
		if (Has(TEXT("Lumen")) || Has(TEXT("ScreenProbe")) || Has(TEXT("RadianceCache"))
			|| Has(TEXT("DiffuseIndirect")))
		{
			return TEXT("LumenGI");
		}
		return TEXT("");
	}

	static void ParseProfileGPU(const FString& Text, float& OutTotalMs, TArray<FProfileEvent>& OutEvents)
	{
		OutTotalMs = -1.f;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);

		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimEnd();
			if (Line.IsEmpty())
			{
				continue;
			}

			// UE 5.8 table row: "... │ 12.345 ms ┃ EventName"
			// Prefer the inclusive Time column (last "N.NNN ms" before the event name).
			int32 BoxIdx = Line.Find(TEXT("┃"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (BoxIdx != INDEX_NONE)
			{
				FString EventName = Line.Mid(BoxIdx + 1).TrimStartAndEnd();
				// Strip trailing box / whitespace
				while (EventName.EndsWith(TEXT("┃")) || EventName.EndsWith(TEXT(" ")))
				{
					EventName = EventName.LeftChop(1).TrimStartAndEnd();
				}
				if (EventName.IsEmpty() || EventName.StartsWith(TEXT("Events")) || EventName.StartsWith(TEXT("Exclusive")))
				{
					continue;
				}

				const FString Before = Line.Left(BoxIdx);
				int32 MsIdx = Before.Find(TEXT("ms"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				if (MsIdx == INDEX_NONE)
				{
					continue;
				}
				int32 NumEnd = MsIdx;
				while (NumEnd > 0 && FChar::IsWhitespace(Before[NumEnd - 1]))
				{
					--NumEnd;
				}
				int32 NumStart = NumEnd;
				while (NumStart > 0)
				{
					const TCHAR Ch = Before[NumStart - 1];
					if (!(FChar::IsDigit(Ch) || Ch == TEXT('.')))
					{
						break;
					}
					--NumStart;
				}
				if (NumStart >= NumEnd)
				{
					continue;
				}
				const float Ms = FCString::Atof(*Before.Mid(NumStart, NumEnd - NumStart));

				int32 Depth = 0;
				while (Depth < EventName.Len() && EventName[Depth] == TEXT(' '))
				{
					++Depth;
				}
				// SpringArm / table indent is 3 spaces per level typically
				Depth = Depth / 3;

				FString Name = EventName.TrimStartAndEnd();
				Name.ReplaceInline(TEXT("\""), TEXT(""));

				if (Line.Contains(TEXT("Frame Time"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("<root>"), ESearchCase::IgnoreCase))
				{
					if (OutTotalMs < 0.f)
					{
						OutTotalMs = Ms;
					}
				}
				if (Name.StartsWith(TEXT("Frame "), ESearchCase::IgnoreCase) && OutTotalMs < 0.f)
				{
					OutTotalMs = Ms;
				}

				FProfileEvent Event;
				Event.Depth = Depth;
				Event.Ms = Ms;
				Event.Name = Name;
				Event.Bucket = ClassifyGpuBucket(Name);
				OutEvents.Add(Event);
				continue;
			}

			// Legacy free-text lines: "12.3ms Name" / "total GPU time"
			int32 Depth = 0;
			while (Depth < Line.Len() && (Line[Depth] == TEXT(' ') || Line[Depth] == TEXT('\t')))
			{
				++Depth;
			}
			float Ms = 0.f;
			int32 TokenEnd = 0;
			if (!ExtractMilliseconds(Line, Depth, Ms, TokenEnd))
			{
				continue;
			}
			if (Line.Contains(TEXT("total GPU time"), ESearchCase::IgnoreCase)
				|| Line.Contains(TEXT("Frame Time"), ESearchCase::IgnoreCase))
			{
				OutTotalMs = Ms;
			}
			FString Name = Line.Mid(TokenEnd).TrimStartAndEnd();
			if (Name.IsEmpty())
			{
				continue;
			}
			FProfileEvent Event;
			Event.Depth = Depth;
			Event.Ms = Ms;
			Event.Name = Name;
			Event.Bucket = ClassifyGpuBucket(Name);
			OutEvents.Add(Event);
		}

		if (OutTotalMs < 0.f)
		{
			for (const FProfileEvent& Event : OutEvents)
			{
				if (Event.Name.StartsWith(TEXT("Frame"), ESearchCase::IgnoreCase) || Event.Name.Equals(TEXT("<root>")))
				{
					OutTotalMs = Event.Ms;
					break;
				}
			}
		}
	}

	static void SumBuckets(const TArray<FProfileEvent>& Events, TMap<FString, float>& OutSums)
	{
		TArray<bool> Counted;
		Counted.Init(false, Events.Num());
		for (int32 Index = 0; Index < Events.Num(); ++Index)
		{
			if (Counted[Index] || Events[Index].Bucket.IsEmpty())
			{
				continue;
			}

			bool bMixedChild = false;
			int32 ChildEnd = Index + 1;
			for (; ChildEnd < Events.Num(); ++ChildEnd)
			{
				if (Events[ChildEnd].Depth <= Events[Index].Depth)
				{
					break;
				}
				if (!Events[ChildEnd].Bucket.IsEmpty() && Events[ChildEnd].Bucket != Events[Index].Bucket)
				{
					bMixedChild = true;
				}
			}
			if (bMixedChild)
			{
				continue;
			}

			float& Slot = OutSums.FindOrAdd(Events[Index].Bucket);
			Slot += Events[Index].Ms;
			for (int32 Child = Index; Child < ChildEnd; ++Child)
			{
				if (Events[Child].Bucket == Events[Index].Bucket)
				{
					Counted[Child] = true;
				}
			}
		}
	}

	class FProfileLogCapture : public FOutputDevice
	{
	public:
		FString Text;
		bool bCapture = false;
		FCriticalSection Mutex;

		/** ProfileGPU / LogRHI table is emitted from the RHI/render thread. */
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return false; } // Mutex protects Text

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (!bCapture || !Message)
			{
				return;
			}
			// While profiling, keep every line — Ops saw the LogRHI table only in the
			// editor log because category/thread filtering dropped it from our dump.
			FScopeLock Lock(&Mutex);
			Text.Append(Category.ToString());
			Text.Append(TEXT(": "));
			Text.Append(Message);
			Text.AppendChar(TEXT('\n'));
		}
	};

	static FViewport* FindFrameViewport(UWorld* PlayWorld, FString& OutSource)
	{
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			UWorld* ViewportWorld = GEngine->GameViewport->GetWorld();
			if (PlayWorld && ViewportWorld == PlayWorld)
			{
				OutSource = TEXT("GameViewport");
				return GEngine->GameViewport->Viewport;
			}
		}
		if (GEditor && PlayWorld)
		{
			for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
			{
				if (LevelVC && LevelVC->Viewport && LevelVC->GetWorld() == PlayWorld)
				{
					OutSource = TEXT("LevelViewportPIE");
					return LevelVC->Viewport;
				}
			}
		}
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			OutSource = TEXT("GameViewportUnmatched");
			return GEngine->GameViewport->Viewport;
		}
		if (GEditor)
		{
			for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
			{
				if (LevelVC && LevelVC->Viewport)
				{
					OutSource = TEXT("LevelViewport");
					return LevelVC->Viewport;
				}
			}
		}
		OutSource = TEXT("none");
		return nullptr;
	}

	static FString NormalizeMapPackage(const FString& MapPath)
	{
		FString Path = MapPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			return Path;
		}
		if (!Path.StartsWith(TEXT("/")))
		{
			Path = TEXT("/Game/Maps/") + Path;
		}
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot) && Dot > 0)
		{
			Path.LeftInline(Dot);
		}
		return Path;
	}

	/** Prefer-ON stick used by RunPreferOnGate (DSF2). Does not touch MaxBoats. */
	static const TCHAR* PreferOnCVarBlock()
	{
		return TEXT(
			"r.Lumen.Reflections.Allow=1\n"
			"r.Lumen.Reflections.DownsampleFactor=2");
	}

	static FString ReadGitShaShort()
	{
		const FString GitDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".git"));
		FString Head;
		if (!FFileHelper::LoadFileToString(Head, *FPaths::Combine(GitDir, TEXT("HEAD"))))
		{
			return TEXT("unknown");
		}
		Head.TrimStartAndEndInline();
		if (Head.StartsWith(TEXT("ref:")))
		{
			const FString Ref = Head.Mid(4).TrimStartAndEnd();
			FString Sha;
			if (FFileHelper::LoadFileToString(Sha, *FPaths::Combine(GitDir, Ref)))
			{
				Sha.TrimStartAndEndInline();
				return Sha.Left(7);
			}
			return TEXT("unknown");
		}
		return Head.Left(7);
	}

	struct FFramingPreset
	{
		FString Name;
		FVector2D XYOffsetCm = FVector2D::ZeroVector; // relative to BoatStart, +X north +Y east
		float YawDeg = 90.f;
		FString Note;
	};

	static bool ResolveFramingPreset(const FString& NameIn, FFramingPreset& Out, FString& OutError)
	{
		const FString Name = NameIn.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			OutError = TEXT("empty");
			return false;
		}
		const FString Key = Name.ToLower();
		Out.Name = Name;
		if (Key == TEXT("midharbormoored") || Key == TEXT("mid_harbor_moored"))
		{
			// Nantucket Harbor inner basin (FNavGeo::BoatStart). Moored fleet prefers near harbor.
			Out.XYOffsetCm = FVector2D::ZeroVector;
			Out.YawDeg = FNavGeo::BoatStartHeadingDeg; // 90 east — moored hulls L/R of player
			Out.Note = TEXT("FNavGeo::BoatStartWorldCm2D + BoatStartHeadingDeg (harbor basin)");
			return true;
		}
		if (Key == TEXT("gelcoathull") || Key == TEXT("gelcoat_hull"))
		{
			// Same basin, yawed so boom fills with lit gelcoat / near hull.
			Out.XYOffsetCm = FVector2D(-1200.f, 600.f);
			Out.YawDeg = 135.f;
			Out.Note = TEXT("BoatStart + (-12m N, +6m E), yaw 135 for close gelcoat");
			return true;
		}
		if (Key == TEXT("horizon"))
		{
			// ~2.5 km north of harbor — open water / horizon, away from moored strip.
			Out.XYOffsetCm = FVector2D(250000.f, 0.f);
			Out.YawDeg = 0.f;
			Out.Note = TEXT("BoatStart + 2.5km north, yaw 0 (horizon)");
			return true;
		}
		OutError = FString::Printf(
			TEXT("unknown FramingPreset '%s' (expected midHarborMoored|gelcoatHull|horizon)"), *Name);
		return false;
	}

	/**
	 * Teleport possessed SailBoatPawn to a SailSim_Ocean framing preset.
	 * Preserves current Z (water snap already applied by game). Clears physics velocity if present.
	 */
	static bool ApplyFramingPreset(UWorld* PlayWorld, const FString& PresetName, FString& OutError, FString& OutApplied)
	{
		OutApplied.Reset();
		if (PresetName.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		FFramingPreset Preset;
		if (!ResolveFramingPreset(PresetName, Preset, OutError))
		{
			return false;
		}
		AActor* Boat = FindPossessedSailBoat(PlayWorld);
		if (!Boat)
		{
			OutError = TEXT("no possessed ASailBoatPawn to teleport");
			return false;
		}
		const FVector2D Harbor = FNavGeo::BoatStartWorldCm2D();
		const FVector Cur = Boat->GetActorLocation();
		FVector NewLoc(Harbor.X + Preset.XYOffsetCm.X, Harbor.Y + Preset.XYOffsetCm.Y, Cur.Z);
		const FRotator NewRot(0.f, Preset.YawDeg, 0.f);
		Boat->SetActorLocationAndRotation(NewLoc, NewRot, false, nullptr, ETeleportType::TeleportPhysics);
		if (UPrimitiveComponent* RootPrim = Cast<UPrimitiveComponent>(Boat->GetRootComponent()))
		{
			RootPrim->SetPhysicsLinearVelocity(FVector::ZeroVector);
			RootPrim->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		}
		// Extend spring arm after teleport so ResolveBoomView does not see a collapsed boom.
		if (USpringArmComponent* Arm = Boat->FindComponentByClass<USpringArmComponent>())
		{
			Arm->TickComponent(0.016f, ELevelTick::LEVELTICK_All, nullptr);
		}
		OutApplied = FString::Printf(
			TEXT("%s loc=(%.0f,%.0f,%.0f) yaw=%.0f note=%s"),
			*Preset.Name, NewLoc.X, NewLoc.Y, NewLoc.Z, Preset.YawDeg, *Preset.Note);
		UE_LOG(LogTemp, Display, TEXT("CapturePlayerView framing %s"), *OutApplied);
		return true;
	}

	static int32 PumpViewportFrames(UWorld* PlayWorld, int32 MaxFrames, float MaxSeconds)
	{
		FString ViewportSource;
		FViewport* Viewport = FindFrameViewport(PlayWorld, ViewportSource);
		if (!Viewport)
		{
			return 0;
		}
		const double Deadline = FPlatformTime::Seconds() + MaxSeconds;
		int32 Frames = 0;
		while (Frames < MaxFrames && FPlatformTime::Seconds() < Deadline)
		{
			Viewport->Draw();
			FlushRenderingCommands();
			++Frames;
		}
		return Frames;
	}

	static bool SaveBitmapPng(const TArray<FColor>& Bitmap, int32 Width, int32 Height, const FString& AbsPath, FString& OutError)
	{
		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid()
			|| !Png->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			OutError = TEXT("png_encode_failed");
			return false;
		}
		const TArray64<uint8>& Compressed = Png->GetCompressed();
		if (!FFileHelper::SaveArrayToFile(Compressed, *AbsPath))
		{
			OutError = TEXT("png_write_failed");
			return false;
		}
		return true;
	}

	static bool LevelLooksLikeOcean(const FString& PackagePath)
	{
		return PackagePath.Contains(TEXT("SailSim_Ocean"), ESearchCase::IgnoreCase);
	}


}

FToolsetImage USailSimToolset::CapturePlayerView(float MinWorldSeconds, const FString& FramingPreset)
{
	FToolsetImage Out;

	UWorld* PlayWorld = SailSimToolsetPrivate::GetPlayWorld();
	if (!PlayWorld || PlayWorld->WorldType != EWorldType::PIE)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"CapturePlayerView code=no_pie: PIE is not running. "
			"Call SailSimToolset.StartPIE or EnsurePIE, then retry after code is running. "
			"Do not fall back to CaptureViewport — that is the free editor camera (empty grid)."));
		return Out;
	}

	const double WorldSeconds = PlayWorld->GetTimeSeconds();
	if (MinWorldSeconds > 0.f && WorldSeconds < MinWorldSeconds)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT(
			"CapturePlayerView code=not_settled: world time %.2fs is below %.2fs. "
			"Retry after the first frames. Streaming=%s."),
			WorldSeconds,
			MinWorldSeconds,
			SailSimToolsetPrivate::IsStreamingBusy(PlayWorld) ? TEXT("true") : TEXT("false")));
		return Out;
	}

	{
		FString FramingError;
		FString FramingApplied;
		if (!SailSimToolsetPrivate::ApplyFramingPreset(PlayWorld, FramingPreset, FramingError, FramingApplied))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT(
				"CapturePlayerView code=bad_framing: %s"), *FramingError));
			return Out;
		}
		if (!FramingApplied.IsEmpty())
		{
			// One present so streaming / spring arm catch the teleport before capture.
			SailSimToolsetPrivate::PumpViewportFrames(PlayWorld, 2, 0.5f);
		}
	}

	SailSimToolsetPrivate::FBoomView Boom;
	FString BoomError;
	if (!SailSimToolsetPrivate::ResolveBoomView(PlayWorld, Boom, BoomError))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("CapturePlayerView code=no_boom: %s"), *BoomError));
		return Out;
	}

	if (!PlayWorld->Scene)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"CapturePlayerView code=no_pie_scene: PlayWorld has no scene. The editor viewport was not used."));
		return Out;
	}

	// Deferred scene captures flush when the editor viewport draws (empty grid).
	// CaptureScene() below is immediate, and only after GWorld is switched to PIE.
	const int32 Width = 1920;
	const int32 Height = 1080;

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->ClearColor = FLinearColor(1.f, 0.f, 1.f, 1.f);
	RT->bAutoGenerateMips = false;
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);

	const SailSimToolsetPrivate::FRooted RootedRT(RT);
	FString CaptureError;
	{
		// GWorld stays on the editor world during MCP calls. Spawn and render while
		// it is the PIE world so the capture cannot bind to the editor grid.
		FScopedConditionalWorldSwitcher PlayScope(PlayWorld);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		ASceneCapture2D* CaptureActor = PlayWorld->SpawnActor<ASceneCapture2D>(
			Boom.Location, Boom.Rotation, SpawnParams);
		if (!CaptureActor || CaptureActor->GetWorld() != PlayWorld)
		{
			if (CaptureActor)
			{
				CaptureActor->Destroy();
			}
			CaptureError = TEXT(
				"CapturePlayerView code=wrong_world: scene capture was not spawned in the PIE world.");
		}
		else
		{
			USceneCaptureComponent2D* Cap = CaptureActor->GetCaptureComponent2D();
			Cap->TextureTarget = RT;
			Cap->FOVAngle = Boom.FOV;
			Cap->bCaptureEveryFrame = false;
			Cap->bCaptureOnMovement = false;
			Cap->bAlwaysPersistRenderingState = true;
			Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			Cap->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
			SailSimToolsetPrivate::ApplyLitGameShowFlags(Cap->ShowFlags);
			Cap->SetWorldLocationAndRotation(Boom.Location, Boom.Rotation);
			if (!Cap->IsRegistered())
			{
				Cap->RegisterComponent();
			}
			Cap->MarkRenderStateDirty();

			// CaptureScene() is the immediate capture. It must run while GWorld is the
			// PIE world: a deferred capture is flushed by the editor viewport (empty grid),
			// and on 5.7+ FScene::UpdateSceneCaptureContents takes an internal
			// ISceneRenderBuilder that gameplay code cannot construct.
			// Two passes so the second Lit frame has a view state (Lumen / exposure).
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				PlayWorld->SendAllEndOfFrameUpdates();
				Cap->CaptureScene();
				FlushRenderingCommands();
			}
			CaptureActor->Destroy();
		}
	}
	if (!CaptureError.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(CaptureError);
		return Out;
	}

	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	TArray<FColor> Bitmap;
	const bool bRead = RTResource && RTResource->ReadPixels(Bitmap) && Bitmap.Num() == Width * Height;

	if (!bRead)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"CapturePlayerView code=read_failed: PIE scene capture did not produce pixels."));
		return Out;
	}
	if (SailSimToolsetPrivate::BitmapIsSentinel(Bitmap))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"CapturePlayerView code=capture_did_not_write: render target is still the magenta clear. "
			"The editor viewport was not returned."));
		return Out;
	}
	for (FColor& Pixel : Bitmap)
	{
		Pixel.A = 255;
	}

	if (!Out.SetFromBitmap(Bitmap, FIntPoint(Width, Height)))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView code=encode_failed: PNG encode failed."));
		return Out;
	}

	UE_LOG(LogTemp, Display,
		TEXT("CapturePlayerView OK world=%s package=%s cam=%s boat=%s loc=(%.0f,%.0f,%.0f) boatLoc=(%.0f,%.0f,%.0f) editorCamDist=%.0f fov=%.1f %dx%d"),
		SailSimToolsetPrivate::WorldKind(PlayWorld),
		*PlayWorld->GetOutermost()->GetName(),
		*Boom.Source,
		*Boom.BoatName,
		Boom.Location.X, Boom.Location.Y, Boom.Location.Z,
		Boom.BoatLocation.X, Boom.BoatLocation.Y, Boom.BoatLocation.Z,
		Boom.EditorCameraDistance,
		Boom.FOV, Width, Height);
	return Out;
}

FString USailSimToolset::FindActorsByName(const FString& Query, int32 MaxResults)
{
	UWorld* World = SailSimToolsetPrivate::GetSearchWorld();
	if (!World)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("FindActorsByName code=no_world: no world."));
		return TEXT("[]");
	}

	const FString Needle = Query.TrimStartAndEnd();
	if (Needle.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("FindActorsByName code=empty_query: Query is empty."));
		return TEXT("[]");
	}

	const FString WorldLabel = SailSimToolsetPrivate::WorldKind(World);
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

		const FVector Location = Actor->GetActorLocation();
		const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("label"), Label);
		Obj->SetStringField(TEXT("class"), ClassName);
		Obj->SetNumberField(TEXT("x"), Location.X);
		Obj->SetNumberField(TEXT("y"), Location.Y);
		Obj->SetNumberField(TEXT("z"), Location.Z);
		Obj->SetStringField(TEXT("world"), WorldLabel);
		const APawn* Pawn = Cast<APawn>(Actor);
		Obj->SetBoolField(TEXT("playerControlled"), Pawn && Pawn->IsPlayerControlled());
		Arr.Add(MakeShared<FJsonValueObject>(Obj));

		if (Arr.Num() >= Cap)
		{
			break;
		}
	}

	return SailSimToolsetPrivate::JsonArray(Arr);
}

FString USailSimToolset::EnsurePIE(float MinWorldSeconds)
{
	return SailSimToolsetPrivate::RequestOrDescribePIE(MinWorldSeconds);
}

FString USailSimToolset::StartPIE(float MinWorldSeconds)
{
	return SailSimToolsetPrivate::RequestOrDescribePIE(MinWorldSeconds);
}

FString USailSimToolset::GetLevelPath()
{
	FString EditorLevel;
	FString PIELevel;
	UWorld* PlayWorld = SailSimToolsetPrivate::GetPlayWorld();
	const bool bPIE = PlayWorld != nullptr;

	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			EditorLevel = EditorWorld->GetOutermost()->GetName();
		}
		if (bPIE)
		{
			PIELevel = PlayWorld->GetOutermost()->GetName();
		}
	}

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetStringField(TEXT("EditorLevel"), EditorLevel);
	Obj->SetStringField(TEXT("PIELevel"), PIELevel);
	Obj->SetStringField(TEXT("MapName"), bPIE && PlayWorld
		? PlayWorld->GetMapName()
		: (GEditor && GEditor->GetEditorWorldContext().World()
			? GEditor->GetEditorWorldContext().World()->GetMapName()
			: FString()));
	SailSimToolsetPrivate::AppendSettleFields(Obj, PlayWorld, 0.5f);
	return SailSimToolsetPrivate::JsonString(Obj);
}

FString USailSimToolset::GetPlayerCameraTransform()
{
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	UWorld* PlayWorld = SailSimToolsetPrivate::GetPlayWorld();
	if (!PlayWorld)
	{
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("code"), TEXT("no_pie"));
		Obj->SetStringField(TEXT("error"), TEXT("GetPlayerCameraTransform requires PIE."));
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetPlayerCameraTransform code=no_pie: requires PIE."));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	SailSimToolsetPrivate::FBoomView Boom;
	FString BoomError;
	if (!SailSimToolsetPrivate::ResolveBoomView(PlayWorld, Boom, BoomError))
	{
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("code"), TEXT("no_boom"));
		Obj->SetStringField(TEXT("error"), BoomError);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("GetPlayerCameraTransform code=no_boom: %s"), *BoomError));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetStringField(TEXT("error"), TEXT(""));
	Obj->SetNumberField(TEXT("x"), Boom.Location.X);
	Obj->SetNumberField(TEXT("y"), Boom.Location.Y);
	Obj->SetNumberField(TEXT("z"), Boom.Location.Z);
	Obj->SetNumberField(TEXT("pitch"), Boom.Rotation.Pitch);
	Obj->SetNumberField(TEXT("yaw"), Boom.Rotation.Yaw);
	Obj->SetNumberField(TEXT("roll"), Boom.Rotation.Roll);
	Obj->SetNumberField(TEXT("fov"), Boom.FOV);
	Obj->SetStringField(TEXT("source"), Boom.Source);
	Obj->SetStringField(TEXT("boat"), Boom.BoatName);
	Obj->SetNumberField(TEXT("boatX"), Boom.BoatLocation.X);
	Obj->SetNumberField(TEXT("boatY"), Boom.BoatLocation.Y);
	Obj->SetNumberField(TEXT("boatZ"), Boom.BoatLocation.Z);
	Obj->SetNumberField(TEXT("editorCameraDistance"), Boom.EditorCameraDistance);
	Obj->SetStringField(TEXT("world"), SailSimToolsetPrivate::WorldKind(PlayWorld));
	return SailSimToolsetPrivate::JsonString(Obj);
}

FString USailSimToolset::GetPerfSnapshot()
{
	const FSailSimPerf& Perf = SailSimGetPerf();
	const float FrameMs = GAverageMS;
	const float Fps = GAverageFPS > 0.f
		? GAverageFPS
		: (FrameMs > 0.1f ? 1000.f / FrameMs : 0.f);
	const float GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	const bool bPie = SailSimToolsetPrivate::GetPlayWorld() != nullptr;

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetBoolField(TEXT("pie"), bPie);
	Obj->SetNumberField(TEXT("fps"), Fps);
	Obj->SetNumberField(TEXT("frameMs"), FrameMs);
	Obj->SetNumberField(TEXT("gpuMs"), GpuMs);

	const TSharedRef<FJsonObject> Hud = MakeShared<FJsonObject>();
	Hud->SetNumberField(TEXT("wallMs"), Perf.WallFrameEmaMs);
	Hud->SetNumberField(TEXT("gameThreadMs"), Perf.GameThreadEmaMs);
	Hud->SetNumberField(TEXT("gameWaitMs"), Perf.GameWaitEmaMs);
	Hud->SetNumberField(TEXT("renderThreadMs"), Perf.RenderThreadEmaMs);
	Hud->SetNumberField(TEXT("rhiMs"), Perf.RhiThreadEmaMs);
	Hud->SetNumberField(TEXT("gpuMs"), Perf.GpuFrameEmaMs);
	Hud->SetStringField(TEXT("bottleneck"), Perf.BottleneckLabel);
	Hud->SetNumberField(TEXT("moored"), Perf.MooredCount);
	Hud->SetNumberField(TEXT("aids"), Perf.AidCount);
	Hud->SetNumberField(TEXT("tilesT"), Perf.TerrainTiles);
	Hud->SetNumberField(TEXT("tilesTk"), Perf.TerrainVerts / 1000);
	Hud->SetNumberField(TEXT("tilesH"), Perf.StructureTiles);
	Hud->SetNumberField(TEXT("tilesHk"), Perf.StructureVerts / 1000);
	Hud->SetStringField(TEXT("tilesTLabel"), FString::Printf(
		TEXT("%d/%dk"), Perf.TerrainTiles, Perf.TerrainVerts / 1000));
	Hud->SetStringField(TEXT("tilesHLabel"), FString::Printf(
		TEXT("%d/%dk"), Perf.StructureTiles, Perf.StructureVerts / 1000));

	const TSharedRef<FJsonObject> Buckets = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < FSailSimPerf::NumBuckets; ++Index)
	{
		const auto Bucket = static_cast<FSailSimPerf::EBucket>(Index);
		Buckets->SetNumberField(FSailSimPerf::BucketName(Bucket), Perf.EmaMs[Index]);
	}
	Hud->SetObjectField(TEXT("gtBucketsMs"), Buckets);

	const float HudFps = Perf.WallFrameEmaMs > 0.1f ? 1000.f / Perf.WallFrameEmaMs : 0.f;
	const FString PerfLine = FString::Printf(
		TEXT("[perf] wall=%.1fms (%.0f fps)  GT=%.1f  GTwait=%.1f  RT=%.1f  RHI=%.1f  GPU=%.1f  | bottleneck=%s | tiles T=%d/%dk H=%d/%dk moored=%d aids=%d"),
		Perf.WallFrameEmaMs, HudFps,
		Perf.GameThreadEmaMs, Perf.GameWaitEmaMs, Perf.RenderThreadEmaMs, Perf.RhiThreadEmaMs,
		Perf.GpuFrameEmaMs, *Perf.BottleneckLabel,
		Perf.TerrainTiles, Perf.TerrainVerts / 1000, Perf.StructureTiles, Perf.StructureVerts / 1000,
		Perf.MooredCount, Perf.AidCount);
	Hud->SetStringField(TEXT("perfLine"), PerfLine);
	Hud->SetStringField(
		TEXT("note"),
		TEXT("fps/frameMs/gpuMs are live engine timers. HUD EMAs and gtBuckets update when the performance chrome ticks. moored, aids, and tiles update from subsystems."));
	Obj->SetObjectField(TEXT("hud"), Hud);
	return SailSimToolsetPrivate::JsonString(Obj);
}

FString USailSimToolset::SetCVars(const FString& Assignments)
{
	TArray<FString> Lines;
	SailSimToolsetPrivate::SplitBatch(Assignments, Lines);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Results;
	bool bAllOk = true;

	if (Lines.Num() == 0)
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("code"), TEXT("empty"));
		Root->SetStringField(TEXT("error"), TEXT("SetCVars: no assignments. Use newline-separated name=value."));
		Root->SetArrayField(TEXT("results"), Results);
		return SailSimToolsetPrivate::JsonString(Root);
	}

	for (const FString& Line : Lines)
	{
		FString Name;
		FString Value;
		int32 Equals = INDEX_NONE;
		if (Line.FindChar(TEXT('='), Equals))
		{
			Name = Line.Left(Equals).TrimStartAndEnd();
			Value = Line.Mid(Equals + 1).TrimStartAndEnd();
		}
		else
		{
			Line.Split(TEXT(" "), &Name, &Value, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			Name = Name.TrimStartAndEnd();
			Value = Value.TrimStartAndEnd();
		}

		const TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("name"), Name);
		One->SetStringField(TEXT("value"), Value);
		if (Name.IsEmpty() || Value.IsEmpty())
		{
			bAllOk = false;
			One->SetBoolField(TEXT("ok"), false);
			One->SetStringField(TEXT("error"), TEXT("expected name=value or name value"));
		}
		else if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name))
		{
			const FString Before = CVar->GetString();
			CVar->Set(*Value, ECVF_SetByConsole);
			One->SetBoolField(TEXT("ok"), true);
			One->SetStringField(TEXT("error"), TEXT(""));
			One->SetStringField(TEXT("before"), Before);
			One->SetStringField(TEXT("after"), CVar->GetString());
		}
		else
		{
			bAllOk = false;
			One->SetBoolField(TEXT("ok"), false);
			One->SetStringField(TEXT("error"), TEXT("console variable not found"));
		}
		Results.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetBoolField(TEXT("ok"), bAllOk);
	Root->SetStringField(TEXT("code"), bAllOk ? TEXT("ok") : TEXT("partial"));
	Root->SetStringField(TEXT("error"), bAllOk ? TEXT("") : TEXT("one or more cvars failed"));
	Root->SetArrayField(TEXT("results"), Results);
	return SailSimToolsetPrivate::JsonString(Root);
}

FString USailSimToolset::ExecuteConsole(const FString& Commands)
{
	TArray<FString> Lines;
	SailSimToolsetPrivate::SplitBatch(Commands, Lines);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Results;
	UWorld* World = SailSimToolsetPrivate::GetExecWorld();
	if (!GEngine || !World)
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("code"), TEXT("no_world"));
		Root->SetStringField(TEXT("error"), TEXT("ExecuteConsole: no world."));
		Root->SetArrayField(TEXT("results"), Results);
		return SailSimToolsetPrivate::JsonString(Root);
	}
	if (Lines.Num() == 0)
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("code"), TEXT("empty"));
		Root->SetStringField(TEXT("error"), TEXT("ExecuteConsole: no commands."));
		Root->SetArrayField(TEXT("results"), Results);
		return SailSimToolsetPrivate::JsonString(Root);
	}

	bool bAllOk = true;
	for (const FString& Command : Lines)
	{
		const TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("command"), Command);
		const bool bExec = GEngine->Exec(World, *Command);
		One->SetBoolField(TEXT("ok"), bExec);
		One->SetStringField(TEXT("error"), bExec ? TEXT("") : TEXT("Exec returned false"));
		if (!bExec)
		{
			bAllOk = false;
		}
		Results.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetBoolField(TEXT("ok"), bAllOk);
	Root->SetStringField(TEXT("code"), bAllOk ? TEXT("ok") : TEXT("partial"));
	Root->SetStringField(TEXT("error"), bAllOk ? TEXT("") : TEXT("one or more commands returned false"));
	Root->SetStringField(TEXT("world"), SailSimToolsetPrivate::WorldKind(World));
	Root->SetArrayField(TEXT("results"), Results);
	return SailSimToolsetPrivate::JsonString(Root);
}

FString USailSimToolset::ProfileGPUDump()
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	UWorld* World = SailSimToolsetPrivate::GetExecWorld();
	if (!GEngine || !World)
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("code"), TEXT("no_world"));
		Root->SetStringField(TEXT("error"), TEXT("ProfileGPUDump: no world."));
		return SailSimToolsetPrivate::JsonString(Root);
	}

	FString ViewportSource;
	FViewport* Viewport = SailSimToolsetPrivate::FindFrameViewport(
		SailSimToolsetPrivate::GetPlayWorld(), ViewportSource);
	if (!Viewport)
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("code"), TEXT("no_viewport"));
		Root->SetStringField(TEXT("error"), TEXT("ProfileGPUDump: no viewport to present a frame."));
		return SailSimToolsetPrivate::JsonString(Root);
	}

	IConsoleVariable* ShowUI = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ProfileGPU.ShowUI"));
	const int32 PreviousShowUI = ShowUI ? ShowUI->GetInt() : 1;
	if (ShowUI)
	{
		ShowUI->Set(0, ECVF_SetByCode);
	}

	SailSimToolsetPrivate::FProfileLogCapture Capture;
	if (GLog)
	{
		GLog->AddOutputDevice(&Capture);
	}
	Capture.bCapture = true;
	const bool bTriggered = GEngine->Exec(World, TEXT("ProfileGPU"));
	// ProfileGPU dumps ~3–6 frames later under LogRHI. Keep the capture device
	// attached and present until the table appears (or we hit a frame budget).
	const double Deadline = FPlatformTime::Seconds() + 2.5;
	int32 FramesPumped = 0;
	while (FPlatformTime::Seconds() < Deadline && FramesPumped < 12)
	{
		Viewport->Draw();
		FlushRenderingCommands();
		if (GLog)
		{
			GLog->Flush();
		}
		++FramesPumped;
		if ((Capture.Text.Contains(TEXT("GPU Profile")) || Capture.Text.Contains(TEXT("LogRHI")))
			&& Capture.Text.Contains(TEXT("ms"))
			&& Capture.Text.Len() > 800)
		{
			// Give one extra frame so leaf rows finish streaming into the log.
			Viewport->Draw();
			FlushRenderingCommands();
			if (GLog)
			{
				GLog->Flush();
			}
			++FramesPumped;
			break;
		}
	}
	if (GLog)
	{
		GLog->Flush();
		GLog->RemoveOutputDevice(&Capture);
	}
	Capture.bCapture = false;
	if (ShowUI)
	{
		ShowUI->Set(PreviousShowUI, ECVF_SetByCode);
	}

	const FString DumpDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Profiling"));
	IFileManager::Get().MakeDirectory(*DumpDir, true);
	const FString DumpPath = FPaths::Combine(
		DumpDir,
		FString::Printf(TEXT("SailSimProfileGPU-%s.txt"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	FFileHelper::SaveStringToFile(Capture.Text, *DumpPath);

	float TotalMs = -1.f;
	TArray<SailSimToolsetPrivate::FProfileEvent> Events;
	SailSimToolsetPrivate::ParseProfileGPU(Capture.Text, TotalMs, Events);

	TMap<FString, float> Sums;
	SailSimToolsetPrivate::SumBuckets(Events, Sums);
	auto BucketMs = [&Sums](const TCHAR* Name) -> float
	{
		const float* Found = Sums.Find(Name);
		return Found ? *Found : 0.f;
	};
	const float Water = BucketMs(TEXT("SingleLayerWater"));
	const float LumenGI = BucketMs(TEXT("LumenGI"));
	const float LumenReflections = BucketMs(TEXT("LumenReflections"));
	const float Shadows = BucketMs(TEXT("Shadows"));
	const float Nanite = BucketMs(TEXT("Nanite"));
	const float Known = Water + LumenGI + LumenReflections + Shadows + Nanite;
	const float Other = TotalMs >= 0.f ? FMath::Max(0.f, TotalMs - Known) : -1.f;
	const bool bParsed = Events.Num() > 0;

	const TSharedRef<FJsonObject> Buckets = MakeShared<FJsonObject>();
	Buckets->SetNumberField(TEXT("SingleLayerWater"), Water);
	Buckets->SetNumberField(TEXT("LumenGI"), LumenGI);
	Buckets->SetNumberField(TEXT("LumenReflections"), LumenReflections);
	Buckets->SetNumberField(TEXT("Lumen"), LumenGI + LumenReflections);
	Buckets->SetNumberField(TEXT("Shadows"), Shadows);
	Buckets->SetNumberField(TEXT("Nanite"), Nanite);
	if (Other >= 0.f)
	{
		Buckets->SetNumberField(TEXT("Other"), Other);
	}

	Events.Sort([](const SailSimToolsetPrivate::FProfileEvent& A, const SailSimToolsetPrivate::FProfileEvent& B)
	{
		return A.Ms > B.Ms;
	});
	TArray<TSharedPtr<FJsonValue>> Top;
	const int32 TopCount = FMath::Min(12, Events.Num());
	for (int32 Index = 0; Index < TopCount; ++Index)
	{
		const TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("name"), Events[Index].Name);
		One->SetNumberField(TEXT("ms"), Events[Index].Ms);
		One->SetStringField(TEXT("bucket"), Events[Index].Bucket);
		Top.Add(MakeShared<FJsonValueObject>(One));
	}

	Root->SetBoolField(TEXT("ok"), bParsed);
	Root->SetBoolField(TEXT("triggered"), bTriggered);
	Root->SetNumberField(TEXT("framesPumped"), FramesPumped);
	Root->SetNumberField(TEXT("captureChars"), Capture.Text.Len());
	Root->SetBoolField(TEXT("parsed"), bParsed);
	Root->SetStringField(TEXT("code"), bParsed ? TEXT("ok") : TEXT("profile_not_emitted"));
	Root->SetStringField(TEXT("error"), bParsed
		? TEXT("")
		: TEXT("ProfileGPU did not emit a parseable hierarchy. Dump file has the captured log lines. Present a frame and call again."));
	Root->SetStringField(TEXT("dumpPath"), DumpPath);
	Root->SetStringField(TEXT("profiledWorld"), SailSimToolsetPrivate::WorldKind(World));
	Root->SetStringField(TEXT("viewport"), ViewportSource);
	if (TotalMs >= 0.f)
	{
		Root->SetNumberField(TEXT("totalGpuMs"), TotalMs);
	}
	Root->SetObjectField(TEXT("bucketsMs"), Buckets);
	Root->SetArrayField(TEXT("topEvents"), Top);
	Root->SetStringField(
		TEXT("note"),
		TEXT("Bucket ms sum the shallowest matching pass (children of the same bucket are not added again). Other = totalGpuMs minus those buckets, so overlap across buckets can shrink Other."));
	return SailSimToolsetPrivate::JsonString(Root);
}

FString USailSimToolset::LoadMap(const FString& MapPath)
{
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!GEditor)
	{
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetBoolField(TEXT("loaded"), false);
		Obj->SetStringField(TEXT("code"), TEXT("no_editor"));
		Obj->SetStringField(TEXT("error"), TEXT("LoadMap: editor not available."));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	const FString Package = SailSimToolsetPrivate::NormalizeMapPackage(MapPath);
	Obj->SetStringField(TEXT("map"), Package);
	if (Package.IsEmpty())
	{
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetBoolField(TEXT("loaded"), false);
		Obj->SetStringField(TEXT("code"), TEXT("empty"));
		Obj->SetStringField(TEXT("error"), TEXT("LoadMap: MapPath is empty."));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	if (SailSimToolsetPrivate::GetPlayWorld() != nullptr)
	{
		GEditor->RequestEndPlayMap();
		SailSimToolsetPrivate::bPieStartQueued = false;
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetBoolField(TEXT("loaded"), false);
		Obj->SetBoolField(TEXT("endedPIE"), true);
		Obj->SetStringField(TEXT("code"), TEXT("ended_pie"));
		Obj->SetStringField(
			TEXT("error"),
			TEXT("PIE was running. End requested; the map was not loaded. Call LoadMap again once IsPIERunning is false."));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
	FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
	if (DirtyPackages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		const int32 Limit = FMath::Min(DirtyPackages.Num(), 20);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			if (DirtyPackages[Index])
			{
				Names.Add(MakeShared<FJsonValueString>(DirtyPackages[Index]->GetName()));
			}
		}
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetBoolField(TEXT("loaded"), false);
		Obj->SetBoolField(TEXT("endedPIE"), false);
		Obj->SetStringField(TEXT("code"), TEXT("unsaved_packages"));
		Obj->SetStringField(
			TEXT("error"),
			TEXT("Unsaved packages are open. LoadMap will not prompt or discard them. Save in the editor, then call again."));
		Obj->SetArrayField(TEXT("dirtyPackages"), Names);
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	const bool bLoaded = FEditorFileUtils::LoadMap(Package, false, false);
	Obj->SetBoolField(TEXT("ok"), bLoaded);
	Obj->SetBoolField(TEXT("loaded"), bLoaded);
	Obj->SetBoolField(TEXT("endedPIE"), false);
	Obj->SetStringField(TEXT("code"), bLoaded ? TEXT("loaded") : TEXT("load_failed"));
	Obj->SetStringField(TEXT("error"), bLoaded ? TEXT("") : TEXT("FEditorFileUtils::LoadMap returned false."));
	return SailSimToolsetPrivate::JsonString(Obj);
}

FString USailSimToolset::RunPreferOnGate()
{
	using namespace SailSimToolsetPrivate;
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString Sha = ReadGitShaShort();
	Root->SetStringField(TEXT("sha"), Sha);
	Root->SetBoolField(TEXT("ok"), false);

	auto Fail = [&](const FString& Code, const FString& Error) -> FString
	{
		Root->SetBoolField(TEXT("ok"), false);
		Root->SetStringField(TEXT("failCode"), Code);
		Root->SetStringField(TEXT("error"), Error);
		if (!Root->HasField(TEXT("moored")))
		{
			Root->SetNumberField(TEXT("moored"), -1);
		}
		if (!Root->HasField(TEXT("frameMs_avg")))
		{
			Root->SetNumberField(TEXT("frameMs_avg"), 0);
		}
		if (!Root->HasField(TEXT("fps")))
		{
			Root->SetNumberField(TEXT("fps"), 0);
		}
		if (!Root->HasField(TEXT("cpvPath")))
		{
			Root->SetStringField(TEXT("cpvPath"), TEXT(""));
		}
		const FString Out = JsonString(Root);
		PersistPreferOnGateJson(Out);
		UE_LOG(LogTemp, Error, TEXT("RunPreferOnGate FAIL code=%s sha=%s err=%s"), *Code, *Sha, *Error);
		return Out;
	};

	if (!GEditor)
	{
		return Fail(TEXT("no_editor"), TEXT("editor not available"));
	}

	// Map check: Prefer-ON gate is SailSim_Ocean only.
	FString EditorLevel;
	if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
	{
		EditorLevel = EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : EditorWorld->GetMapName();
	}
	UWorld* PlayWorld = GetPlayWorld();
	FString PieLevel;
	if (PlayWorld)
	{
		PieLevel = PlayWorld->GetOutermost() ? PlayWorld->GetOutermost()->GetName() : PlayWorld->GetName();
	}
	const FString LevelForCheck = !PieLevel.IsEmpty() ? PieLevel : EditorLevel;
	if (!LevelLooksLikeOcean(LevelForCheck))
	{
		return Fail(
			TEXT("wrong_map"),
			FString::Printf(
				TEXT("expected SailSim_Ocean, got EditorLevel=%s PIELevel=%s (LoadMap then EnsurePIE; do not kill the editor)"),
				*EditorLevel, *PieLevel));
	}

	// 1) EnsurePIE — already-playing = success. Request if down; one short pump; else fail.
	if (!PlayWorld)
	{
		RequestOrDescribePIE(0.5f);
		PumpViewportFrames(nullptr, 4, 1.0f);
		PlayWorld = GetPlayWorld();
		if (!PlayWorld)
		{
			return Fail(
				TEXT("pie_not_running"),
				TEXT("EnsurePIE queued but PIE is not up yet. Retry RunPreferOnGate after the editor ticks (do not kill UnrealEditor)."));
		}
	}

	// 2) Prefer-ON cvars (DSF2 stick)
	const FString CVarResult = USailSimToolset::SetCVars(PreferOnCVarBlock());
	Root->SetStringField(TEXT("cvars"), CVarResult);

	// 3) Settle — teleport mid-harbor first so moored stream in, then pump + sample.
	{
		FString FramingError;
		FString FramingApplied;
		if (!ApplyFramingPreset(PlayWorld, TEXT("midHarborMoored"), FramingError, FramingApplied))
		{
			return Fail(TEXT("bad_framing"), FramingError);
		}
		Root->SetStringField(TEXT("framing"), FramingApplied);
	}

	TArray<float> FrameSamples;
	TArray<float> FpsSamples;
	int32 LastMoored = -1;
	const int32 Pumped = PumpViewportFrames(PlayWorld, 8, 2.0f);
	Root->SetNumberField(TEXT("framesPumped"), Pumped);
	for (int32 I = 0; I < 5; ++I)
	{
		PumpViewportFrames(PlayWorld, 1, 0.35f);
		const FSailSimPerf& Perf = SailSimGetPerf();
		const float FrameMs = GAverageMS;
		const float Fps = GAverageFPS > 0.f ? GAverageFPS : (FrameMs > 0.1f ? 1000.f / FrameMs : 0.f);
		FrameSamples.Add(FrameMs);
		FpsSamples.Add(Fps);
		LastMoored = Perf.MooredCount;
	}

	float FrameSum = 0.f;
	float FpsSum = 0.f;
	for (float V : FrameSamples) { FrameSum += V; }
	for (float V : FpsSamples) { FpsSum += V; }
	const float FrameAvg = FrameSamples.Num() > 0 ? FrameSum / FrameSamples.Num() : 0.f;
	const float FpsAvg = FpsSamples.Num() > 0 ? FpsSum / FpsSamples.Num() : 0.f;
	Root->SetNumberField(TEXT("frameMs_avg"), FrameAvg);
	Root->SetNumberField(TEXT("fps"), FpsAvg);
	Root->SetNumberField(TEXT("moored"), LastMoored);
	FString CpvPath;

	// 4) Assert moored == 16
	if (LastMoored != 16)
	{
		return Fail(
			TEXT("moored_count"),
			FString::Printf(
				TEXT("expected moored=16 from MooredBoatSubsystem/FSailSimPerf, got %d (Prefer-ON gate does not deepen moored LOD)"),
				LastMoored));
	}

	// 5) Noon CPV — same path as CapturePlayerView (midHarborMoored already applied); save PNG for cpvPath.
	{
		FBoomView Boom;
		FString BoomError;
		if (!ResolveBoomView(PlayWorld, Boom, BoomError))
		{
			return Fail(TEXT("cpv_no_boom"), BoomError);
		}
		const int32 Width = 1920;
		const int32 Height = 1080;
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		RT->RenderTargetFormat = RTF_RGBA8;
		RT->ClearColor = FLinearColor(1.f, 0.f, 1.f, 1.f);
		RT->bAutoGenerateMips = false;
		RT->InitAutoFormat(Width, Height);
		RT->UpdateResourceImmediate(true);
		const FRooted RootedRT(RT);
		FString CaptureError;
		{
			FScopedConditionalWorldSwitcher PlayScope(PlayWorld);
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ASceneCapture2D* CaptureActor = PlayWorld->SpawnActor<ASceneCapture2D>(
				Boom.Location, Boom.Rotation, SpawnParams);
			if (!CaptureActor || CaptureActor->GetWorld() != PlayWorld)
			{
				if (CaptureActor) { CaptureActor->Destroy(); }
				CaptureError = TEXT("wrong_world");
			}
			else
			{
				USceneCaptureComponent2D* Cap = CaptureActor->GetCaptureComponent2D();
				Cap->TextureTarget = RT;
				Cap->FOVAngle = Boom.FOV;
				Cap->bCaptureEveryFrame = false;
				Cap->bCaptureOnMovement = false;
				Cap->bAlwaysPersistRenderingState = true;
				Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
				Cap->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
				ApplyLitGameShowFlags(Cap->ShowFlags);
				Cap->SetWorldLocationAndRotation(Boom.Location, Boom.Rotation);
				if (!Cap->IsRegistered()) { Cap->RegisterComponent(); }
				Cap->MarkRenderStateDirty();
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					PlayWorld->SendAllEndOfFrameUpdates();
					Cap->CaptureScene();
					FlushRenderingCommands();
				}
				CaptureActor->Destroy();
			}
		}
		if (!CaptureError.IsEmpty())
		{
			return Fail(TEXT("cpv_failed"), CaptureError);
		}
		FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
		TArray<FColor> Bitmap;
		if (!RTResource || !RTResource->ReadPixels(Bitmap) || Bitmap.Num() != Width * Height)
		{
			return Fail(TEXT("cpv_read_failed"), TEXT("ReadPixels failed"));
		}
		if (BitmapIsSentinel(Bitmap))
		{
			return Fail(TEXT("cpv_sentinel"), TEXT("magenta clear — capture did not write"));
		}
		for (FColor& Pixel : Bitmap) { Pixel.A = 255; }

		const FString ShotDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("SailSim"));
		IFileManager::Get().MakeDirectory(*ShotDir, true);
		const FString FileName = FString::Printf(
			TEXT("ocean-%s-preferON-midHarborMoored-%s.png"),
			*Sha,
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		const FString AbsPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ShotDir, FileName));
		FString SaveError;
		if (!SaveBitmapPng(Bitmap, Width, Height, AbsPath, SaveError))
		{
			return Fail(TEXT("cpv_save_failed"), SaveError);
		}
		Root->SetStringField(TEXT("cpvPath"), AbsPath);
		CpvPath = AbsPath;
	}

	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("failCode"), TEXT(""));
	Root->SetStringField(TEXT("error"), TEXT(""));
	const FString Out = JsonString(Root);
	PersistPreferOnGateJson(Out);
	UE_LOG(LogTemp, Display,
		TEXT("RunPreferOnGate OK sha=%s moored=%d frameMs_avg=%.2f fps=%.1f cpv=%s"),
		*Sha, LastMoored, FrameAvg, FpsAvg, *CpvPath);
	return Out;
}
