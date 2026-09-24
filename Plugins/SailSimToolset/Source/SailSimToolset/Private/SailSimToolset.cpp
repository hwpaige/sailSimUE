// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolset.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelStreaming.h"
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
#include "Sailing/Nav/MooredBoatSubsystem.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
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

	struct FPlayerView
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float FOV = 72.f;
		float PostProcessBlendWeight = 0.f;
		bool bHasPostProcess = false;
		FPostProcessSettings PostProcess;
		FString Source;
		FString BoatName;
		FVector BoatLocation = FVector::ZeroVector;
		float EditorCameraDistance = -1.f;
	};

	/**
	 * Possessed ASailBoatPawn camera — the PIE view target.
	 * UCameraComponent::GetCameraView supplies FOV and the pawn post-process
	 * (day auto-exposure bias / speeds from ApplyAtmosphereLook).
	 * PlayerCameraManager is accepted only when its cache is already on that camera.
	 * A cache still sitting on the free editor camera is ignored.
	 */
	static bool ResolvePlayerView(UWorld* PlayWorld, FPlayerView& Out, FString& OutError)
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
			// Push the socket out before we read it. A freshly possessed pawn can
			// still report the boom origin (inside the hull).
			Arm->TickComponent(0.016f, ELevelTick::LEVELTICK_All, nullptr);
		}
		Boat->UpdateComponentTransforms();

		if (Cam)
		{
			FMinimalViewInfo POV;
			Cam->GetCameraView(0.f, POV);
			Out.Location = POV.Location;
			Out.Rotation = POV.Rotation;
			Out.FOV = POV.FOV > 1.f ? POV.FOV : Cam->FieldOfView;
			Out.PostProcess = POV.PostProcessSettings;
			Out.PostProcessBlendWeight = FMath::Max(POV.PostProcessBlendWeight, Cam->PostProcessBlendWeight);
			Out.bHasPostProcess = true;
			Out.Source = TEXT("PlayerCamera");

			if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
			{
				APlayerCameraManager* PCM = PC->PlayerCameraManager;
				if (PCM && PC->GetViewTarget() == Boat)
				{
					const FVector PcmLoc = PCM->GetCameraLocation();
					// Cache matches the pawn camera. This is the view the PIE viewport drew.
					if (FVector::Dist(PcmLoc, Out.Location) < 200.f)
					{
						Out.Location = PcmLoc;
						Out.Rotation = PCM->GetCameraRotation();
						const float PcmFov = PCM->GetFOVAngle();
						if (PcmFov > 1.f)
						{
							Out.FOV = PcmFov;
						}
						Out.Source = TEXT("PlayerCameraManager");
					}
				}
			}
		}
		else
		{
			Out.Location = Arm->GetSocketLocation(USpringArmComponent::SocketName);
			Out.Rotation = Arm->GetSocketRotation(USpringArmComponent::SocketName);
			Out.FOV = 72.f;
			Out.Source = TEXT("SpringArmSocket");
		}

		if (GCurrentLevelEditingViewportClient)
		{
			Out.EditorCameraDistance = FVector::Dist(
				Out.Location, GCurrentLevelEditingViewportClient->GetViewLocation());
		}

		const float CameraReach = FVector::Dist(Out.Location, Out.BoatLocation);
		if (CameraReach < 50.f)
		{
			OutError = FString::Printf(
				TEXT("player camera is %.0fcm from the hull (not extended). source=%s"),
				CameraReach, *Out.Source);
			return false;
		}

		// A view that landed on the free editor camera is the empty-grid shot.
		if (Out.EditorCameraDistance >= 0.f && Out.EditorCameraDistance < 10.f
			&& FVector::Dist(Out.BoatLocation, Out.Location) > 10000.f)
		{
			OutError = TEXT("resolved view matches the free editor camera, not the possessed boat camera");
			return false;
		}

		return true;
	}

	/** Tick the possessed chase cam and push it into the PIE camera manager. Does not touch sky or exposure settings. */
	static void AdvancePossessedCamera(UWorld* PlayWorld, float DeltaSeconds)
	{
		if (!PlayWorld)
		{
			return;
		}
		AActor* Boat = FindPossessedSailBoat(PlayWorld);
		APlayerController* PC = PlayWorld->GetFirstPlayerController();
		if (PC && Boat && PC->GetViewTarget() != Boat)
		{
			PC->SetViewTarget(Boat);
		}
		if (Boat)
		{
			if (USpringArmComponent* Arm = Boat->FindComponentByClass<USpringArmComponent>())
			{
				Arm->TickComponent(DeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
			}
			Boat->UpdateComponentTransforms();
		}
		if (PC && PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->UpdateCamera(DeltaSeconds);
		}
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

	/** Prefer-ON stick used by RunPreferOnGate (DSF2). Does not change hero caps or scenery budget. */
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
		// Extend the chase cam so the possessed view target is off the hull before capture.
		if (USpringArmComponent* Arm = Boat->FindComponentByClass<USpringArmComponent>())
		{
			Arm->TickComponent(0.016f, ELevelTick::LEVELTICK_All, nullptr);
		}
		Boat->UpdateComponentTransforms();
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

	/** Frames of possessed-camera advance + PIE viewport present before a shot. */
	static constexpr int32 GPlayerViewSettleFrames = 16;

	static bool BitmapMostlyBlack(const TArray<FColor>& Bitmap)
	{
		if (Bitmap.Num() == 0)
		{
			return true;
		}
		const int32 Step = FMath::Max(1, Bitmap.Num() / 2000);
		int32 Samples = 0;
		int32 Dark = 0;
		for (int32 Index = 0; Index < Bitmap.Num(); Index += Step)
		{
			const FColor& Pixel = Bitmap[Index];
			++Samples;
			if (Pixel.R < 8 && Pixel.G < 8 && Pixel.B < 8)
			{
				++Dark;
			}
		}
		return Samples > 0 && (Dark * 100) / Samples >= 95;
	}

	static bool ViewportHasPixels(const FViewport* Viewport)
	{
		return Viewport && Viewport->GetSizeXY().X > 1 && Viewport->GetSizeXY().Y > 1;
	}

	/**
	 * Framebuffer Harrison is looking at: the active level viewport while it is presenting PIE,
	 * otherwise the PIE game viewport (play-in-new-window). Not a second camera.
	 * Editor-camera distance is ignored — during in-viewport PIE, GetViewLocation can stay on the
	 * free camera while this framebuffer is the Lit game view.
	 */
	static FViewport* FindLitViewportFramebuffer(UWorld* PlayWorld, FString& OutSource)
	{
		OutSource.Reset();
		if (!PlayWorld)
		{
			return nullptr;
		}

		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			FLevelEditorModule& LevelEditorModule =
				FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			if (TSharedPtr<IAssetViewport> Active = LevelEditorModule.GetFirstActiveViewport())
			{
				// During PIE in this panel, GetActiveViewport is the framebuffer on screen
				// (the level viewport and the game viewport are swapped).
				FViewport* Live = Active->GetActiveViewport();
				if (Active->HasPlayInEditorViewport() && ViewportHasPixels(Live))
				{
					OutSource = TEXT("ActiveEditorViewport");
					return Live;
				}
				FEditorViewportClient& Client = Active->GetAssetViewportClient();
				if (ViewportHasPixels(Client.Viewport) && Client.GetWorld() == PlayWorld)
				{
					OutSource = TEXT("ActiveEditorViewport");
					return Client.Viewport;
				}
			}
		}

		if (GCurrentLevelEditingViewportClient
			&& ViewportHasPixels(GCurrentLevelEditingViewportClient->Viewport)
			&& GCurrentLevelEditingViewportClient->GetWorld() == PlayWorld)
		{
			OutSource = TEXT("ActiveEditorViewport");
			return GCurrentLevelEditingViewportClient->Viewport;
		}

		if (GEngine)
		{
			if (UGameViewportClient* GameViewport = GEngine->GameViewportForWorld(PlayWorld))
			{
				if (ViewportHasPixels(GameViewport->Viewport))
				{
					OutSource = TEXT("PIEGameViewport");
					return GameViewport->Viewport;
				}
			}
		}

		if (GEditor)
		{
			for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
			{
				if (!LevelVC || LevelVC->GetWorld() != PlayWorld || !ViewportHasPixels(LevelVC->Viewport))
				{
					continue;
				}
				OutSource = TEXT("LevelViewportPIE");
				return LevelVC->Viewport;
			}
		}
		return nullptr;
	}

	struct FSettledCapture
	{
		TArray<FColor> Bitmap;
		int32 Width = 0;
		int32 Height = 0;
		FString CaptureSource;
		FPlayerView View;
		int32 ExposureFrames = 0;
		FString Error;
		FString ErrorCode;
	};

	/**
	 * Ground truth: the active editor/PIE Lit viewport framebuffer (HighResShot's read).
	 * Framing only moves the possessed camera. Show flags, exposure, and post stay on that viewport.
	 * There is no SceneCapture fallback.
	 */
	static bool CaptureSettledPlayerView(UWorld* PlayWorld, FSettledCapture& Out)
	{
		Out = FSettledCapture();
		if (!PlayWorld || PlayWorld->WorldType != EWorldType::PIE)
		{
			Out.ErrorCode = TEXT("no_pie");
			Out.Error = TEXT("PIE is not running");
			return false;
		}

		const float DeltaSeconds = 1.f / 30.f;
		for (int32 Frame = 0; Frame < GPlayerViewSettleFrames; ++Frame)
		{
			AdvancePossessedCamera(PlayWorld, DeltaSeconds);
		}

		if (!ResolvePlayerView(PlayWorld, Out.View, Out.Error))
		{
			Out.ErrorCode = TEXT("no_boom");
			return false;
		}

		FString ViewportSource;
		FViewport* Viewport = FindLitViewportFramebuffer(PlayWorld, ViewportSource);
		if (!Viewport)
		{
			Out.ErrorCode = TEXT("no_viewport");
			Out.Error = TEXT(
				"active editor/PIE Lit viewport framebuffer was not available. Scene capture is not used.");
			return false;
		}

		for (int32 Frame = 0; Frame < GPlayerViewSettleFrames; ++Frame)
		{
			AdvancePossessedCamera(PlayWorld, DeltaSeconds);
			PlayWorld->SendAllEndOfFrameUpdates();
			Viewport->Draw();
			FlushRenderingCommands();
			++Out.ExposureFrames;
		}

		FString ResolveError;
		FPlayerView Presented;
		if (ResolvePlayerView(PlayWorld, Presented, ResolveError))
		{
			Out.View = Presented;
		}

		const FIntPoint Size = Viewport->GetSizeXY();
		const FIntRect Rect(0, 0, Size.X, Size.Y);
		TArray<FColor> Bitmap;
		// GetViewportScreenShot is the read FScreenshotRequest / editor HighResShot uses. Multiplier stays 1.
		const bool bRead = Size.X > 1 && Size.Y > 1
			&& GetViewportScreenShot(Viewport, Bitmap, Rect)
			&& Bitmap.Num() == Size.X * Size.Y
			&& !BitmapMostlyBlack(Bitmap);
		if (!bRead)
		{
			Out.ErrorCode = TEXT("read_failed");
			Out.Error = TEXT("viewport framebuffer grab failed. Scene capture is not used.");
			return false;
		}

		Out.Bitmap = MoveTemp(Bitmap);
		Out.Width = Size.X;
		Out.Height = Size.Y;
		Out.CaptureSource = ViewportSource;
		return true;
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
	}

	// Pose from the framing preset, pixels from the active Lit viewport framebuffer.
	SailSimToolsetPrivate::FSettledCapture Shot;
	if (!SailSimToolsetPrivate::CaptureSettledPlayerView(PlayWorld, Shot))
	{
		const FString Code = Shot.ErrorCode.IsEmpty() ? TEXT("read_failed") : Shot.ErrorCode;
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("CapturePlayerView code=%s: %s"), *Code, *Shot.Error));
		return Out;
	}
	for (FColor& Pixel : Shot.Bitmap)
	{
		Pixel.A = 255;
	}

	if (!Out.SetFromBitmap(Shot.Bitmap, FIntPoint(Shot.Width, Shot.Height)))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CapturePlayerView code=encode_failed: PNG encode failed."));
		return Out;
	}

	const SailSimToolsetPrivate::FPlayerView& View = Shot.View;
	UE_LOG(LogTemp, Display,
		TEXT("CapturePlayerView OK world=%s package=%s cam=%s grab=ViewportFramebuffer view=%s boat=%s loc=(%.0f,%.0f,%.0f) boatLoc=(%.0f,%.0f,%.0f) editorCamDist=%.0f fov=%.1f ppBlend=%.2f exposureFrames=%d litOverride=0 %dx%d"),
		SailSimToolsetPrivate::WorldKind(PlayWorld),
		*PlayWorld->GetOutermost()->GetName(),
		*Shot.CaptureSource,
		*View.Source,
		*View.BoatName,
		View.Location.X, View.Location.Y, View.Location.Z,
		View.BoatLocation.X, View.BoatLocation.Y, View.BoatLocation.Z,
		View.EditorCameraDistance,
		View.FOV,
		View.PostProcessBlendWeight,
		Shot.ExposureFrames,
		Shot.Width, Shot.Height);
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

	SailSimToolsetPrivate::FPlayerView View;
	FString ViewError;
	if (!SailSimToolsetPrivate::ResolvePlayerView(PlayWorld, View, ViewError))
	{
		Obj->SetBoolField(TEXT("ok"), false);
		Obj->SetStringField(TEXT("code"), TEXT("no_boom"));
		Obj->SetStringField(TEXT("error"), ViewError);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("GetPlayerCameraTransform code=no_boom: %s"), *ViewError));
		return SailSimToolsetPrivate::JsonString(Obj);
	}

	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetStringField(TEXT("error"), TEXT(""));
	Obj->SetNumberField(TEXT("x"), View.Location.X);
	Obj->SetNumberField(TEXT("y"), View.Location.Y);
	Obj->SetNumberField(TEXT("z"), View.Location.Z);
	Obj->SetNumberField(TEXT("pitch"), View.Rotation.Pitch);
	Obj->SetNumberField(TEXT("yaw"), View.Rotation.Yaw);
	Obj->SetNumberField(TEXT("roll"), View.Rotation.Roll);
	Obj->SetNumberField(TEXT("fov"), View.FOV);
	Obj->SetNumberField(TEXT("postProcessBlendWeight"), View.PostProcessBlendWeight);
	Obj->SetStringField(TEXT("source"), View.Source);
	Obj->SetStringField(TEXT("boat"), View.BoatName);
	Obj->SetNumberField(TEXT("boatX"), View.BoatLocation.X);
	Obj->SetNumberField(TEXT("boatY"), View.BoatLocation.Y);
	Obj->SetNumberField(TEXT("boatZ"), View.BoatLocation.Z);
	Obj->SetNumberField(TEXT("editorCameraDistance"), View.EditorCameraDistance);
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

	// 4) Harbor fill gate keys off scenery count (floor ~64, soft = budget, default 96).
	// Heroes (MaxBoats / MaxNearFullBoats, default 1) are reported and do not set the bar.
	constexpr int32 SceneryFloor = 64;
	int32 SceneryBudget = 96;
	int32 SlotCount = -1;
	int32 HeroesMax = 1;
	int32 HeroesNearCap = 1;
	int32 HeroesNear = 0;
	if (UMooredBoatSubsystem* Moored = PlayWorld->GetSubsystem<UMooredBoatSubsystem>())
	{
		SceneryBudget = FMath::Max(1, Moored->MooringSceneryInstanceCount);
		SlotCount = Moored->GetSlotCount();
		HeroesMax = Moored->MaxBoats;
		HeroesNearCap = Moored->MaxNearFullBoats;
		HeroesNear = Moored->GetNearFullCount();
		if (APlayerController* PC = PlayWorld->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				Moored->ForceStreamAround(Pawn->GetActorLocation());
			}
		}
		PumpViewportFrames(PlayWorld, 4, 1.0f);
		LastMoored = SailSimGetPerf().MooredCount;
		HeroesNear = Moored->GetNearFullCount();
		Root->SetNumberField(TEXT("moored"), LastMoored);
	}
	const int32 Sampled = FMath::Min(SceneryBudget, SlotCount > 0 ? SlotCount : SceneryBudget);
	const int32 MinFilled = FMath::Max(SceneryFloor, (Sampled * 2) / 3);
	Root->SetNumberField(TEXT("mooringSceneryBudget"), SceneryBudget);
	Root->SetNumberField(TEXT("mooringSceneryFloor"), MinFilled);
	Root->SetNumberField(TEXT("mooredSlots"), SlotCount);
	Root->SetNumberField(TEXT("heroesMaxBoats"), HeroesMax);
	Root->SetNumberField(TEXT("heroesNearFullCap"), HeroesNearCap);
	Root->SetNumberField(TEXT("heroesNear"), HeroesNear);
	if (LastMoored < MinFilled)
	{
		return Fail(
			TEXT("moored_count"),
			FString::Printf(
				TEXT("expected mid-harbor scenery moored>=%d (soft scenery %d, floor %d, slots=%d); heroes MaxBoats=%d NearFull≤%d near=%d; got moored=%d"),
				MinFilled, SceneryBudget, SceneryFloor, SlotCount, HeroesMax, HeroesNearCap, HeroesNear, LastMoored));
	}

	// 5) Possessed-camera CPV (midHarborMoored already applied). Same path as CapturePlayerView.
	{
		FSettledCapture Shot;
		if (!CaptureSettledPlayerView(PlayWorld, Shot))
		{
			const FString Code = Shot.ErrorCode.IsEmpty() ? TEXT("cpv_failed") : (TEXT("cpv_") + Shot.ErrorCode);
			return Fail(Code, Shot.Error);
		}
		for (FColor& Pixel : Shot.Bitmap)
		{
			Pixel.A = 255;
		}

		const FString ShotDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("SailSim"));
		IFileManager::Get().MakeDirectory(*ShotDir, true);
		const FString FileName = FString::Printf(
			TEXT("ocean-%s-preferON-midHarborMoored-%s-%s.png"),
			*Sha,
			*Shot.CaptureSource,
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
		const FString AbsPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ShotDir, FileName));
		FString SaveError;
		if (!SaveBitmapPng(Shot.Bitmap, Shot.Width, Shot.Height, AbsPath, SaveError))
		{
			return Fail(TEXT("cpv_save_failed"), SaveError);
		}
		Root->SetStringField(TEXT("cpvPath"), AbsPath);
		Root->SetStringField(TEXT("captureSource"), Shot.CaptureSource);
		Root->SetStringField(TEXT("grab"), TEXT("ViewportFramebuffer"));
		Root->SetStringField(TEXT("viewSource"), Shot.View.Source);
		Root->SetNumberField(TEXT("exposureFrames"), Shot.ExposureFrames);
		Root->SetBoolField(TEXT("litOverride"), false);
		Root->SetNumberField(TEXT("fov"), Shot.View.FOV);
		Root->SetNumberField(TEXT("postProcessBlendWeight"), Shot.View.PostProcessBlendWeight);
		CpvPath = AbsPath;
		UE_LOG(LogTemp, Display,
			TEXT("CapturePlayerView OK world=PIE cam=%s grab=ViewportFramebuffer view=%s boat=%s loc=(%.0f,%.0f,%.0f) fov=%.1f ppBlend=%.2f exposureFrames=%d litOverride=0 %dx%d"),
			*Shot.CaptureSource,
			*Shot.View.Source,
			*Shot.View.BoatName,
			Shot.View.Location.X, Shot.View.Location.Y, Shot.View.Location.Z,
			Shot.View.FOV,
			Shot.View.PostProcessBlendWeight,
			Shot.ExposureFrames,
			Shot.Width, Shot.Height);
	}

	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("failCode"), TEXT(""));
	Root->SetStringField(TEXT("error"), TEXT(""));
	const FString Out = JsonString(Root);
	PersistPreferOnGateJson(Out);
	UE_LOG(LogTemp, Display,
		TEXT("RunPreferOnGate OK sha=%s moored=%d scenery=%d floor=%d heroes MaxBoats=%d NearFull≤%d near=%d frameMs_avg=%.2f fps=%.1f cpv=%s capture=%s grab=ViewportFramebuffer litOverride=0"),
		*Sha, LastMoored, SceneryBudget, MinFilled, HeroesMax, HeroesNearCap, HeroesNear, FrameAvg, FpsAvg, *CpvPath,
		*Root->GetStringField(TEXT("captureSource")));
	return Out;
}
