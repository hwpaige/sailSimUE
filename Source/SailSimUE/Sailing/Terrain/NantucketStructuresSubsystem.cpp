#include "Sailing/Terrain/NantucketStructuresSubsystem.h"
#include "Sailing/Terrain/NavtMeshLoader.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Ocean/SailEnvPreset.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void UNantucketStructuresSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ReloadManifest();
}

void UNantucketStructuresSubsystem::Deinitialize()
{
	TArray<uint64> Keys;
	Resident.GetKeys(Keys);
	for (uint64 K : Keys)
	{
		ReleaseTile(K);
	}
	Resident.Empty();
	ResidentVerts = 0;
	Super::Deinitialize();
}

TStatId UNantucketStructuresSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UNantucketStructuresSubsystem, STATGROUP_Tickables);
}

void UNantucketStructuresSubsystem::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		TArray<uint64> Keys;
		Resident.GetKeys(Keys);
		for (uint64 K : Keys)
		{
			ReleaseTile(K);
		}
		Resident.Empty();
		ResidentVerts = 0;
	}
}

FString UNantucketStructuresSubsystem::GetStatusLine() const
{
	if (!bManifestLoaded)
	{
		return TEXT("struct: no manifest");
	}
	return FString::Printf(TEXT("struct: %d tiles (%dkv) night=%.0f%%"),
		Resident.Num(), ResidentVerts / 1000, Night01 * 100.f);
}

void UNantucketStructuresSubsystem::SetNightLighting(float InNight01)
{
	Night01 = FMath::Clamp(InNight01, 0.f, 1.5f);
	for (TPair<uint64, FResidentTile>& Pair : Resident)
	{
		ApplyLightingToMid(Pair.Value.Mid);
	}
}

void UNantucketStructuresSubsystem::ApplyLightingToMid(UMaterialInstanceDynamic* Mid) const
{
	if (!Mid) return;
	Mid->SetScalarParameterValue(TEXT("Night01"), Night01);
	Mid->SetScalarParameterValue(TEXT("WindowEmissive"), WindowEmissive);
	Mid->SetScalarParameterValue(TEXT("LampEmissive"), LampEmissive);
	Mid->SetScalarParameterValue(TEXT("DayGlassGlint"), 0.10f);
	Mid->SetVectorParameterValue(TEXT("Tint"), FLinearColor(1.f, 1.f, 1.f, 1.f));
	Mid->SetScalarParameterValue(TEXT("ColorBoost"), 1.15f);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 0.72f);
}

void UNantucketStructuresSubsystem::SyncNightFromOcean()
{
	UWorld* World = GetWorld();
	if (!World) return;
	USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>();
	if (!Ocean) return;

	// Continuous night amount from time-of-day (Fair Day noon → 0).
	float Target = Ocean->GetNightAmount();
	const ESailEnvPreset P = static_cast<ESailEnvPreset>(Ocean->GetActiveEnvPreset());
	// Mood presets that aren't on the diurnal slider.
	if (P == ESailEnvPreset::Storm) Target = FMath::Max(Target, 0.22f);
	else if (P == ESailEnvPreset::FogBank) Target = FMath::Max(Target, 0.12f);
	else if (P == ESailEnvPreset::Overcast) Target = FMath::Max(Target, 0.08f);
	if (FMath::Abs(Target - Night01) > 0.02f)
	{
		SetNightLighting(Target);
	}
}

void UNantucketStructuresSubsystem::PublishPerfStats() const
{
	FSailSimPerf& P = FSailSimPerf::Get();
	P.StructureTiles = Resident.Num();
	P.StructureVerts = ResidentVerts;
}

bool UNantucketStructuresSubsystem::ResolveStructuresRoot(FString& OutRoot) const
{
	TArray<FString> Candidates;
	Candidates.Add(FPaths::ProjectContentDir() / TEXT("Structures/nantucket"));
	Candidates.Add(FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/structures/nantucket"));
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/sail-sim/frontend/public/structures/nantucket"));
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/SailSimUE/Content/Structures/nantucket"));

	for (const FString& C : Candidates)
	{
		const FString UeMan = C / TEXT("ue_manifest.json");
		const FString SrcMan = C / TEXT("manifest.json");
		if (FPaths::FileExists(UeMan) || FPaths::FileExists(SrcMan))
		{
			OutRoot = C;
			return true;
		}
	}
	return false;
}

bool UNantucketStructuresSubsystem::ReloadManifest()
{
	bManifestLoaded = false;
	TileDescs.Reset();
	KeyToDescIndex.Empty();

	FString Root;
	if (!ResolveStructuresRoot(Root))
	{
		UE_LOG(LogSailSim, Warning,
			TEXT("NantucketStructures: no root found (run Scripts/export_nantucket_structures_ue.py)"));
		bEnabled = false;
		return false;
	}
	StructuresRoot = Root;
	return LoadManifestFromRoot(Root);
}

bool UNantucketStructuresSubsystem::LoadManifestFromRoot(const FString& Root)
{
	FString Path = Root / TEXT("ue_manifest.json");
	const bool bUe = FPaths::FileExists(Path);
	if (!bUe)
	{
		Path = Root / TEXT("manifest.json");
	}
	if (!FPaths::FileExists(Path))
	{
		return false;
	}

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return false;
	}

	StreamCols = (int32)RootObj->GetNumberField(TEXT("streamTileCols"));
	StreamRows = (int32)RootObj->GetNumberField(TEXT("streamTileRows"));
	if (StreamCols < 1) StreamCols = 8;
	if (StreamRows < 1) StreamRows = 8;

	const TSharedPtr<FJsonObject>* StreamObj = nullptr;
	if (RootObj->TryGetObjectField(TEXT("stream"), StreamObj) && StreamObj && (*StreamObj).IsValid())
	{
		if ((*StreamObj)->HasField(TEXT("loadRadius")))
		{
			LoadRadius = FMath::Clamp((int32)(*StreamObj)->GetNumberField(TEXT("loadRadius")), 1, 6);
		}
		if ((*StreamObj)->HasField(TEXT("unloadRadius")))
		{
			UnloadRadius = FMath::Max(LoadRadius + 1, (int32)(*StreamObj)->GetNumberField(TEXT("unloadRadius")));
		}
		if ((*StreamObj)->HasField(TEXT("lod0Radius")))
		{
			Lod0Radius = FMath::Clamp((int32)(*StreamObj)->GetNumberField(TEXT("lod0Radius")), 0, 3);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TilesArr = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("tiles"), TilesArr) || !TilesArr)
	{
		return false;
	}

	TileDescs.Reserve(TilesArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *TilesArr)
	{
		const TSharedPtr<FJsonObject> T = V->AsObject();
		if (!T.IsValid()) continue;

		FNantucketStructureTileDesc D;
		D.Id = T->GetStringField(TEXT("id"));
		D.Tx = (int32)T->GetNumberField(TEXT("tx"));
		D.Ty = (int32)T->GetNumberField(TEXT("ty"));
		D.FileLod0 = T->GetStringField(TEXT("file"));
		if (T->HasField(TEXT("fileLod1")))
		{
			D.FileLod1 = T->GetStringField(TEXT("fileLod1"));
			if (D.FileLod1.Equals(TEXT("null"), ESearchCase::IgnoreCase))
			{
				D.FileLod1.Reset();
			}
		}
		if (T->HasField(TEXT("vertices")))
		{
			D.Vertices = (int32)T->GetNumberField(TEXT("vertices"));
		}

		if (bUe && T->HasField(TEXT("worldCenter")))
		{
			const TArray<TSharedPtr<FJsonValue>>* C = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Mn = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Mx = nullptr;
			if (T->TryGetArrayField(TEXT("worldCenter"), C) && C && C->Num() >= 2)
			{
				D.WorldCenter = FVector2D((*C)[0]->AsNumber(), (*C)[1]->AsNumber());
			}
			if (T->TryGetArrayField(TEXT("worldMin"), Mn) && Mn && Mn->Num() >= 2)
			{
				D.WorldMin = FVector2D((*Mn)[0]->AsNumber(), (*Mn)[1]->AsNumber());
			}
			if (T->TryGetArrayField(TEXT("worldMax"), Mx) && Mx && Mx->Num() >= 2)
			{
				D.WorldMax = FVector2D((*Mx)[0]->AsNumber(), (*Mx)[1]->AsNumber());
			}
		}
		else if (T->HasField(TEXT("bbox")))
		{
			const TSharedPtr<FJsonObject>* Bb = nullptr;
			if (T->TryGetObjectField(TEXT("bbox"), Bb) && Bb)
			{
				const double W = (*Bb)->GetNumberField(TEXT("west"));
				const double E = (*Bb)->GetNumberField(TEXT("east"));
				const double S = (*Bb)->GetNumberField(TEXT("south"));
				const double N = (*Bb)->GetNumberField(TEXT("north"));
				double X0, Y0, X1, Y1;
				FNavGeo::LatLonToWorldCm(S, W, X0, Y0);
				FNavGeo::LatLonToWorldCm(N, E, X1, Y1);
				D.WorldMin = FVector2D(FMath::Min(X0, X1), FMath::Min(Y0, Y1));
				D.WorldMax = FVector2D(FMath::Max(X0, X1), FMath::Max(Y0, Y1));
				D.WorldCenter = (D.WorldMin + D.WorldMax) * 0.5f;
			}
		}

		const int32 Idx = TileDescs.Num();
		KeyToDescIndex.Add(TileKey(D.Tx, D.Ty), Idx);
		TileDescs.Add(MoveTemp(D));
	}

	bManifestLoaded = TileDescs.Num() > 0;
	UE_LOG(LogSailSim, Log,
		TEXT("NantucketStructures: loaded %d tiles from %s (loadR=%d unloadR=%d lod0=%d)"),
		TileDescs.Num(), *Path, LoadRadius, UnloadRadius, Lod0Radius);
	return bManifestLoaded;
}

FVector UNantucketStructuresSubsystem::GetFocusLocation() const
{
	UWorld* World = GetWorld();
	if (!World) return FVector::ZeroVector;

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* P = PC->GetPawn())
		{
			return P->GetActorLocation();
		}
	}
	for (TActorIterator<ASailBoatPawn> It(World); It; ++It)
	{
		if (It->bPlayerSessionBoat)
		{
			return It->GetActorLocation();
		}
	}
	return FVector::ZeroVector;
}

void UNantucketStructuresSubsystem::ForceStreamAround(FVector FocusWorld)
{
	if (!bEnabled || !bManifestLoaded) return;
	TimeSinceUpdate = 0.f;
	UpdateStreaming(FocusWorld);
}

void UNantucketStructuresSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !bManifestLoaded) return;

	SAIL_PERF_SCOPE(Houses);
	SyncNightFromOcean();
	PublishPerfStats();

	TimeSinceUpdate += DeltaTime;
	if (TimeSinceUpdate < UpdateIntervalSec) return;
	TimeSinceUpdate = 0.f;

	UpdateStreaming(GetFocusLocation());
}

void UNantucketStructuresSubsystem::UpdateStreaming(const FVector& FocusWorld)
{
	int32 BestTx = 0, BestTy = 0;
	float BestD2 = TNumericLimits<float>::Max();
	for (const FNantucketStructureTileDesc& D : TileDescs)
	{
		const float Dx = FocusWorld.X - D.WorldCenter.X;
		const float Dy = FocusWorld.Y - D.WorldCenter.Y;
		const float D2 = Dx * Dx + Dy * Dy;
		if (D2 < BestD2)
		{
			BestD2 = D2;
			BestTx = D.Tx;
			BestTy = D.Ty;
		}
	}

	for (int32 Dy = -LoadRadius; Dy <= LoadRadius; ++Dy)
	{
		for (int32 Dx = -LoadRadius; Dx <= LoadRadius; ++Dx)
		{
			const int32 Tx = BestTx + Dx;
			const int32 Ty = BestTy + Dy;
			const uint64 K = TileKey(Tx, Ty);
			if (!KeyToDescIndex.Contains(K)) continue;
			const int32 Cheb = FMath::Max(FMath::Abs(Dx), FMath::Abs(Dy));
			const int32 Lod = (Cheb <= Lod0Radius) ? 0 : 1;
			EnsureTile(Tx, Ty, Lod);
		}
	}

	TArray<uint64> ToDrop;
	for (const TPair<uint64, FResidentTile>& Pair : Resident)
	{
		const int32 Cheb = FMath::Max(
			FMath::Abs(Pair.Value.Tx - BestTx),
			FMath::Abs(Pair.Value.Ty - BestTy));
		if (Cheb > UnloadRadius)
		{
			ToDrop.Add(Pair.Key);
		}
	}
	for (uint64 K : ToDrop)
	{
		ReleaseTile(K);
	}
}

void UNantucketStructuresSubsystem::EnsureTile(int32 Tx, int32 Ty, int32 Lod)
{
	const uint64 K = TileKey(Tx, Ty);
	if (const FResidentTile* Existing = Resident.Find(K))
	{
		if (Existing->Lod == 0 || Existing->Lod <= Lod)
		{
			return;
		}
		ReleaseTile(K);
	}

	const int32* DescIdx = KeyToDescIndex.Find(K);
	if (!DescIdx || !TileDescs.IsValidIndex(*DescIdx)) return;
	const FNantucketStructureTileDesc& Desc = TileDescs[*DescIdx];

	FString Rel = (Lod == 0 || Desc.FileLod1.IsEmpty()) ? Desc.FileLod0 : Desc.FileLod1;
	if (Lod == 1 && !Desc.FileLod1.IsEmpty())
	{
		const FString Abs1 = StructuresRoot / Desc.FileLod1;
		if (!FPaths::FileExists(Abs1))
		{
			Rel = Desc.FileLod0;
			Lod = 0;
		}
	}

	int32 Verts = 0;
	UMaterialInstanceDynamic* Mid = nullptr;
	UProceduralMeshComponent* Mesh = CreateTileMesh(Rel, &Verts, &Mid);
	if (!Mesh) return;

	FResidentTile R;
	R.Tx = Tx;
	R.Ty = Ty;
	R.Lod = Lod;
	R.VertCount = Verts;
	R.Mesh = Mesh;
	R.Mid = Mid;
	ResidentVerts += Verts;
	Resident.Add(K, MoveTemp(R));
	PublishPerfStats();
}

void UNantucketStructuresSubsystem::ReleaseTile(uint64 Key)
{
	FResidentTile* R = Resident.Find(Key);
	if (!R) return;
	ResidentVerts = FMath::Max(0, ResidentVerts - R->VertCount);
	R->Mid = nullptr;
	if (R->Mesh)
	{
		if (AActor* Owner = R->Mesh->GetOwner())
		{
			Owner->Destroy();
		}
		else
		{
			R->Mesh->DestroyComponent();
		}
		R->Mesh = nullptr;
	}
	Resident.Remove(Key);
	PublishPerfStats();
}

UMaterialInterface* UNantucketStructuresSubsystem::GetOrCreateStructureMaterial()
{
	if (StructureMaterial) return StructureMaterial;

	// Prefer authored vertex-color material (shingle / roof / hydrangea palette).
	static const TCHAR* Candidates[] = {
		TEXT("/Game/Materials/Navt/M_NavtVertexColor.M_NavtVertexColor"),
		TEXT("/Game/Materials/M_NavtVertexColor.M_NavtVertexColor"),
		TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"),
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
	};
	for (const TCHAR* Path : Candidates)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, Path))
		{
			StructureMaterial = M;
			UE_LOG(LogSailSim, Log, TEXT("NantucketStructures: material %s"), Path);
			return StructureMaterial;
		}
	}
	return nullptr;
}

UProceduralMeshComponent* UNantucketStructuresSubsystem::CreateTileMesh(
	const FString& RelPath, int32* OutVerts, UMaterialInstanceDynamic** OutMid)
{
	if (OutVerts) *OutVerts = 0;
	if (OutMid) *OutMid = nullptr;

	UWorld* World = GetWorld();
	if (!World) return nullptr;

	FString Abs = FPaths::ConvertRelativePathToFull(StructuresRoot / RelPath);
	Abs = FPaths::ConvertRelativePathToFull(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Abs));
	if (!FPaths::FileExists(Abs))
	{
		const FString Fallback = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/structures/nantucket") / RelPath);
		if (FPaths::FileExists(Fallback))
		{
			Abs = Fallback;
		}
		else
		{
			UE_LOG(LogSailSim, Warning, TEXT("NantucketStructures: missing mesh %s"), *Abs);
			return nullptr;
		}
	}

	FNavtMeshData Data;
	if (!FNavtMeshLoader::LoadFile(Abs, Data))
	{
		UE_LOG(LogSailSim, Warning, TEXT("NantucketStructures: failed parse %s"), *Abs);
		return nullptr;
	}

	FActorSpawnParameters Sp;
	Sp.Name = FName(*FString::Printf(TEXT("StructTile_%s"), *FPaths::GetBaseFilename(RelPath)));
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner)
	{
		return nullptr;
	}
	Owner->Tags.Add(FName(TEXT("NantucketStructures")));

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Owner->SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Static);
	Root->RegisterComponent();

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner, NAME_None, RF_Transient);
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Static);
	Mesh->RegisterComponent();
	// Visual-only for dense architecture (harbor tile ~500k verts).
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	Mesh->bNeverDistanceCull = true;

	TArray<FVector2D> UV0;
	UV0.SetNum(Data.Positions.Num());
	for (int32 I = 0; I < Data.Positions.Num(); ++I)
	{
		// Planar UV from world XY — cladding detail can key off this later
		UV0[I] = FVector2D(Data.Positions[I].X * 0.001f, Data.Positions[I].Y * 0.001f);
	}
	TArray<FProcMeshTangent> Tangents;
	Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), Data.Positions.Num());

	Mesh->CreateMeshSection_LinearColor(
		0,
		Data.Positions,
		Data.Indices,
		Data.Normals,
		UV0,
		Data.Colors,
		Tangents,
		/*bCreateCollision*/ false);

	UE_LOG(LogSailSim, Log, TEXT("NantucketStructures: loaded %s verts=%d tris=%d bounds=%s"),
		*RelPath, Data.Positions.Num(), Data.Indices.Num() / 3,
		*Data.Bounds.ToString());

	if (OutVerts) *OutVerts = Data.Positions.Num();

	if (UMaterialInterface* Mat = GetOrCreateStructureMaterial())
	{
		if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Mat, Mesh))
		{
			ApplyLightingToMid(Mid);
			Mesh->SetMaterial(0, Mid);
			if (OutMid) *OutMid = Mid;
		}
		else
		{
			Mesh->SetMaterial(0, Mat);
		}
	}

	return Mesh;
}
