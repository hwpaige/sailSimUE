#include "Sailing/Terrain/NantucketStructuresSubsystem.h"
#include "Sailing/Terrain/NavtMeshLoader.h"
#include "Sailing/Terrain/FoliagePrototypeFactory.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Ocean/SailEnvPreset.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
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
	UnloadAllResident();
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
		UnloadAllResident();
	}
}

void UNantucketStructuresSubsystem::UnloadAllResident()
{
	TArray<uint64> Keys;
	Resident.GetKeys(Keys);
	for (uint64 K : Keys)
	{
		ReleaseTile(K);
	}
	Resident.Empty();
	ResidentVerts = 0;
	ResidentSm = 0;
	ResidentPmc = 0;
	ResidentVegTiles = 0;
	ResidentFoliageInstances = 0;
	PublishPerfStats();
}

FString UNantucketStructuresSubsystem::GetStatusLine() const
{
	if (!bManifestLoaded)
	{
		return TEXT("struct: no manifest");
	}
	if (bOpenSeaSkipped && Resident.Num() == 0)
	{
		return FString::Printf(TEXT("struct: open-sea (skip >%.0fkm) night=%.0f%%"),
			OpenSeaSkipDistanceCm * 0.00001f, Night01 * 100.f);
	}
	return FString::Printf(TEXT("struct: %d tiles sm=%d pmc=%d foliage=%d (%dkv) night=%.0f%%"),
		Resident.Num(), ResidentSm, ResidentPmc, ResidentFoliageInstances, ResidentVerts / 1000, Night01 * 100.f);
}

void UNantucketStructuresSubsystem::SetNightLighting(float InNight01)
{
	Night01 = FMath::Clamp(InNight01, 0.f, 1.5f);
	for (TPair<uint64, FResidentTile>& Pair : Resident)
	{
		ApplyLightingToMid(Pair.Value.Mid);
		ApplyLightingToFoliageMids(Pair.Value);
	}
}

void UNantucketStructuresSubsystem::SetSeason(float InSeason01)
{
	Season01 = FMath::Clamp(InSeason01, 0.f, 1.f);
	for (TPair<uint64, FResidentTile>& Pair : Resident)
	{
		ApplyLightingToMid(Pair.Value.Mid);
		ApplyLightingToFoliageMids(Pair.Value);
	}
}

void UNantucketStructuresSubsystem::ApplyLightingToFoliageMids(FResidentTile& R) const
{
	for (UMaterialInstanceDynamic* Mid : R.FoliageMids)
	{
		ApplyLightingToMid(Mid);
	}
}

void UNantucketStructuresSubsystem::ApplyLightingToMid(UMaterialInstanceDynamic* Mid) const
{
	if (!Mid) return;
	Mid->SetScalarParameterValue(TEXT("Night01"), Night01);
	Mid->SetScalarParameterValue(TEXT("Season01"), Season01);
	Mid->SetScalarParameterValue(TEXT("WindowEmissive"), WindowEmissive);
	Mid->SetScalarParameterValue(TEXT("LampEmissive"), LampEmissive);
	// Daytime glass / paint pop — pre-Nanite shingle palette read.
	Mid->SetScalarParameterValue(TEXT("DayGlassGlint"), 0.18f);
	Mid->SetVectorParameterValue(TEXT("Tint"), FLinearColor(1.f, 1.f, 1.f, 1.f));
	Mid->SetScalarParameterValue(TEXT("ColorBoost"), 1.35f);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 0.62f);
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
	P.StructureSmTiles = ResidentSm;
	P.StructurePmcTiles = ResidentPmc;
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
	bOpenSeaSkipped = false;

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

static FString ReadOptionalStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
{
	if (!Obj.IsValid() || !Obj->HasField(Field))
	{
		return FString();
	}
	FString S = Obj->GetStringField(Field);
	if (S.IsEmpty() || S.Equals(TEXT("null"), ESearchCase::IgnoreCase))
	{
		return FString();
	}
	return S;
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
		// Optional importance distances from manifest (cm).
		if ((*StreamObj)->HasField(TEXT("openSeaSkipDistanceCm")))
		{
			OpenSeaSkipDistanceCm = FMath::Max(50000.f,
				(float)(*StreamObj)->GetNumberField(TEXT("openSeaSkipDistanceCm")));
		}
		if ((*StreamObj)->HasField(TEXT("harborFullDistanceCm")))
		{
			HarborFullDistanceCm = FMath::Max(10000.f,
				(float)(*StreamObj)->GetNumberField(TEXT("harborFullDistanceCm")));
		}
		if ((*StreamObj)->HasField(TEXT("vegLoadDistanceCm")))
		{
			VegLoadDistanceCm = FMath::Max(20000.f,
				(float)(*StreamObj)->GetNumberField(TEXT("vegLoadDistanceCm")));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TilesArr = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("tiles"), TilesArr) || !TilesArr)
	{
		return false;
	}

	int32 CookedSmCount = 0;

	TileDescs.Reserve(TilesArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *TilesArr)
	{
		const TSharedPtr<FJsonObject> T = V->AsObject();
		if (!T.IsValid()) continue;

		FNantucketStructureTileDesc D;
		D.Id = T->GetStringField(TEXT("id"));
		D.Tx = (int32)T->GetNumberField(TEXT("tx"));
		D.Ty = (int32)T->GetNumberField(TEXT("ty"));
		D.FileLod0 = ReadOptionalStringField(T, TEXT("file"));
		D.FileLod1 = ReadOptionalStringField(T, TEXT("fileLod1"));
		if (D.FileLod1.Equals(TEXT("null"), ESearchCase::IgnoreCase))
		{
			D.FileLod1.Reset();
		}
		// C2 cooked StaticMesh paths (optional — PMC fallback when missing).
		D.StaticMesh = ReadOptionalStringField(T, TEXT("staticMesh"));
		D.StaticMeshLod1 = ReadOptionalStringField(T, TEXT("staticMeshLod1"));
		D.FileFoliage = ReadOptionalStringField(T, TEXT("fileFoliage"));
		if (!D.StaticMesh.IsEmpty()) ++CookedSmCount;

		auto ReadOrigin3 = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector& Out, bool& bHas)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Obj->TryGetArrayField(Field, Arr) && Arr && Arr->Num() >= 3)
			{
				Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
				bHas = true;
			}
		};
		ReadOrigin3(T, TEXT("staticMeshOrigin"), D.StaticMeshOrigin, D.bHasStaticMeshOrigin);
		ReadOrigin3(T, TEXT("staticMeshLod1Origin"), D.StaticMeshLod1Origin, D.bHasStaticMeshLod1Origin);

		if (T->HasField(TEXT("vertices")))
		{
			D.Vertices = (int32)T->GetNumberField(TEXT("vertices"));
		}
		if (T->HasField(TEXT("foliageInstances")))
		{
			D.FoliageInstanceCount = (int32)T->GetNumberField(TEXT("foliageInstances"));
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
		TEXT("NantucketStructures: loaded %d tiles from %s (loadR=%d unloadR=%d lod0=%d cookedSM=%d skip=%.0fcm)"),
		TileDescs.Num(), *Path, LoadRadius, UnloadRadius, Lod0Radius, CookedSmCount, OpenSeaSkipDistanceCm);
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
	// Keep season in sync when prefs/UI change Ocean without a structures re-tick path.
	if (UWorld* World = GetWorld())
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			const float S = Ocean->GetSeason01();
			if (FMath::Abs(S - Season01) > 1e-3f)
			{
				SetSeason(S);
			}
		}
	}
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

	const float NearestDist = FMath::Sqrt(BestD2);

	// C3 — open sea: do not keep structure tiles when far from the island.
	if (NearestDist > OpenSeaSkipDistanceCm)
	{
		bOpenSeaSkipped = true;
		if (Resident.Num() > 0)
		{
			UnloadAllResident();
			UE_LOG(LogSailSim, Verbose,
				TEXT("NantucketStructures: open-sea unload (nearest=%.0f m > skip=%.0f m)"),
				NearestDist * 0.01f, OpenSeaSkipDistanceCm * 0.01f);
		}
		PublishPerfStats();
		return;
	}
	bOpenSeaSkipped = false;

	// Harbor: full radii. Approach band (between HarborFull and OpenSeaSkip): tighter ring.
	const float HarborDist = FMath::Min(HarborFullDistanceCm, OpenSeaSkipDistanceCm);
	int32 EffectiveLoadR = LoadRadius;
	int32 EffectiveUnloadR = UnloadRadius;
	int32 EffectiveLod0R = Lod0Radius;
	if (NearestDist > HarborDist)
	{
		EffectiveLoadR = 1;
		EffectiveLod0R = FMath::Min(Lod0Radius, 1);
		EffectiveUnloadR = FMath::Max(EffectiveLoadR + 1, 2);
	}

	for (int32 Dy = -EffectiveLoadR; Dy <= EffectiveLoadR; ++Dy)
	{
		for (int32 Dx = -EffectiveLoadR; Dx <= EffectiveLoadR; ++Dx)
		{
			const int32 Tx = BestTx + Dx;
			const int32 Ty = BestTy + Dy;
			const uint64 K = TileKey(Tx, Ty);
			if (!KeyToDescIndex.Contains(K)) continue;
			const int32 Cheb = FMath::Max(FMath::Abs(Dx), FMath::Abs(Dy));
			const int32 Lod = (Cheb <= EffectiveLod0R) ? 0 : 1;
			EnsureTile(Tx, Ty, Lod, FocusWorld);
		}
	}

	TArray<uint64> ToDrop;
	for (const TPair<uint64, FResidentTile>& Pair : Resident)
	{
		const int32 Cheb = FMath::Max(
			FMath::Abs(Pair.Value.Tx - BestTx),
			FMath::Abs(Pair.Value.Ty - BestTy));
		if (Cheb > EffectiveUnloadR)
		{
			ToDrop.Add(Pair.Key);
		}
	}
	for (uint64 K : ToDrop)
	{
		ReleaseTile(K);
	}
}

void UNantucketStructuresSubsystem::EnsureTile(int32 Tx, int32 Ty, int32 Lod, const FVector& FocusWorld)
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

	int32 EffectiveLod = Lod;

	// Cooked SM+Nanite only when explicitly preferred AND asset is full quality.
	// Default is PMC from NAVT (full verts + bake vertex colors = pre-Nanite look).
	FString SmPath;
	if (bPreferCookedStaticMesh)
	{
		if (EffectiveLod == 0 || Desc.StaticMeshLod1.IsEmpty())
		{
			SmPath = Desc.StaticMesh;
		}
		else
		{
			SmPath = Desc.StaticMeshLod1;
		}
		if (EffectiveLod == 1 && SmPath.IsEmpty())
		{
			SmPath = Desc.StaticMesh;
		}
	}

	int32 Verts = 0;
	UMaterialInstanceDynamic* Mid = nullptr;
	bool bUsedSm = false;
	UStaticMeshComponent* Smc = nullptr;
	UProceduralMeshComponent* Pmc = nullptr;

	if (!SmPath.IsEmpty())
	{
		const int32 FallbackVerts = Desc.Vertices;
		const int32 Expected = FMath::Max(FallbackVerts, Desc.Vertices);
		// Reject stub cooks (Interchange often keeps ~1–5% of verts).
		if (UStaticMesh* Probe = LoadCookedStaticMesh(SmPath))
		{
			int32 SmVerts = Expected;
			if (const FStaticMeshRenderData* RD = Probe->GetRenderData())
			{
				if (RD->LODResources.Num() > 0)
				{
					SmVerts = RD->LODResources[0].GetNumVertices();
				}
			}
			// Require ~85% of expected verts — stubs used to pass a weak 1/8 test.
			const bool bStubCook = Expected > 100
				&& (SmVerts < 3 || SmVerts < FMath::Max(50, (Expected * 85) / 100));
			if (bStubCook)
			{
				UE_LOG(LogSailSim, Warning,
					TEXT("NantucketStructures: cooked SM incomplete %s verts=%d expected~%d — PMC fallback"),
					*SmPath, SmVerts, Expected);
			}
			else
			{
				FVector PlaceAt = FVector(Desc.WorldCenter.X, Desc.WorldCenter.Y, 0.f);
				if (EffectiveLod == 0 && Desc.bHasStaticMeshOrigin)
				{
					PlaceAt = Desc.StaticMeshOrigin;
				}
				else if (EffectiveLod != 0 && Desc.bHasStaticMeshLod1Origin)
				{
					PlaceAt = Desc.StaticMeshLod1Origin;
				}
				else if (Desc.bHasStaticMeshOrigin)
				{
					PlaceAt = Desc.StaticMeshOrigin;
				}
				else
				{
					FVector FromFile;
					if (LoadMeshOriginFromCooked(SmPath, FromFile))
					{
						PlaceAt = FromFile;
					}
				}
				Smc = CreateTileStaticMesh(SmPath, FallbackVerts, PlaceAt, &Verts, &Mid);
				if (Smc)
				{
					bUsedSm = true;
				}
			}
		}
		if (!bUsedSm && Smc == nullptr)
		{
			UE_LOG(LogSailSim, Verbose,
				TEXT("NantucketStructures: cooked SM miss %s — PMC fallback"), *SmPath);
		}
	}

	if (!bUsedSm)
	{
		FString Rel = (EffectiveLod == 0 || Desc.FileLod1.IsEmpty()) ? Desc.FileLod0 : Desc.FileLod1;
		// Skip building load when this tile is vegetation-only (file points at _veg
		// or is empty) — buildings live in file/fileLod1; trees/grass in fileVeg.
		const bool bRelLooksLikeVeg = Rel.Contains(TEXT("_veg."));
		if (!Rel.IsEmpty() && !bRelLooksLikeVeg)
		{
			if (EffectiveLod == 1 && !Desc.FileLod1.IsEmpty())
			{
				const FString Abs1 = StructuresRoot / Desc.FileLod1;
				if (!FPaths::FileExists(Abs1))
				{
					Rel = Desc.FileLod0;
					EffectiveLod = 0;
				}
			}
			Pmc = CreateTileMesh(Rel, &Verts, &Mid);
		}
		// Allow foliage-only tiles (moor/heath with no OSM buildings).
		if (!Pmc && Desc.FileFoliage.IsEmpty())
		{
			return;
		}
	}

	FResidentTile R;
	R.Tx = Tx;
	R.Ty = Ty;
	R.Lod = EffectiveLod;
	R.VertCount = Verts;
	R.bStaticMesh = bUsedSm;
	R.StaticMeshComp = Smc;
	R.ProcMesh = Pmc;
	R.Mid = Mid;
	ResidentVerts += Verts;
	if (bUsedSm) ++ResidentSm;
	else if (Pmc) ++ResidentPmc;
	// Foliage HISM — only near focus.
	EnsureFoliageLayer(R, Desc, FocusWorld);
	// Drop empty residents (building miss and foliage out of range / failed).
	if (!R.StaticMeshComp && !R.ProcMesh && !R.FoliageActor)
	{
		return;
	}
	Resident.Add(K, MoveTemp(R));
	PublishPerfStats();
}

UStaticMesh* UNantucketStructuresSubsystem::GetOrCreateFoliageMesh(const FString& Species)
{
	const FString Key = Species.ToLower();
	if (TObjectPtr<UStaticMesh>* Found = FoliageMeshes.Find(Key))
	{
		if (Found->Get()) return Found->Get();
	}
	UMaterialInterface* Mat = GetOrCreateStructureMaterial();
	UStaticMesh* Mesh = FFoliagePrototypeFactory::ResolveOrBuild(Key, Mat);
	if (Mesh)
	{
		FoliageMeshes.Add(Key, Mesh);
	}
	return Mesh;
}

void UNantucketStructuresSubsystem::EnsureFoliageLayer(
	FResidentTile& R, const FNantucketStructureTileDesc& Desc, const FVector& FocusWorld)
{
	if (Desc.FileFoliage.IsEmpty())
	{
		return;
	}
	const float Dx = FocusWorld.X - Desc.WorldCenter.X;
	const float Dy = FocusWorld.Y - Desc.WorldCenter.Y;
	const float Dist = FMath::Sqrt(Dx * Dx + Dy * Dy);
	if (Dist > VegLoadDistanceCm)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World) return;

	const FString Abs = StructuresRoot / Desc.FileFoliage;
	FString JsonStr;
	if (!FPaths::FileExists(Abs) || !FFileHelper::LoadFileToString(JsonStr, *Abs))
	{
		UE_LOG(LogSailSim, Warning, TEXT("NantucketStructures: foliage missing %s"), *Abs);
		return;
	}

	TSharedPtr<FJsonObject> RootObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* InstArr = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("instances"), InstArr) || !InstArr || InstArr->Num() == 0)
	{
		return;
	}

	// Group transforms by species for one HISM per species.
	TMap<FString, TArray<FTransform>> BySpecies;
	BySpecies.Reserve(8);
	for (const TSharedPtr<FJsonValue>& V : *InstArr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		FString Species = O->HasField(TEXT("s")) ? O->GetStringField(TEXT("s")) : TEXT("scrub");
		Species = Species.ToLower();
		double Lon = 0.0, Lat = 0.0, ElevM = 0.0;
		if (O->HasField(TEXT("lon"))) Lon = O->GetNumberField(TEXT("lon"));
		if (O->HasField(TEXT("lat"))) Lat = O->GetNumberField(TEXT("lat"));
		if (O->HasField(TEXT("elevM"))) ElevM = O->GetNumberField(TEXT("elevM"));
		const float Yaw = O->HasField(TEXT("yaw")) ? (float)O->GetNumberField(TEXT("yaw")) : 0.f;
		const float Scale = O->HasField(TEXT("scale")) ? FMath::Max(0.2f, (float)O->GetNumberField(TEXT("scale"))) : 1.f;

		double XCm = 0.0, YCm = 0.0;
		FNavGeo::LatLonToWorldCm(Lat, Lon, XCm, YCm);
		const FVector Loc((float)XCm, (float)YCm, (float)(ElevM * 100.0));
		const FRotator Rot(0.f, Yaw, 0.f);
		const FVector Scl(Scale);
		BySpecies.FindOrAdd(Species).Emplace(Rot, Loc, Scl);
	}
	if (BySpecies.Num() == 0) return;

	FActorSpawnParameters Sp;
	Sp.Name = NAME_None;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner) return;
	Owner->Tags.Add(FName(TEXT("NantucketFoliage")));
	Owner->Tags.Add(FName(TEXT("NantucketStructures")));

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->RegisterComponent();

	int32 TotalInst = 0;
	for (TPair<FString, TArray<FTransform>>& Pair : BySpecies)
	{
		UStaticMesh* Mesh = GetOrCreateFoliageMesh(Pair.Key);
		if (!Mesh) continue;

		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		Hism->SetupAttachment(Root);
		Hism->SetMobility(EComponentMobility::Movable);
		Hism->SetStaticMesh(Mesh);
		Hism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Trees cast soft shadow; dense grass does not (budget).
		const bool bTree = Pair.Key == TEXT("oak") || Pair.Key == TEXT("cedar") || Pair.Key == TEXT("scrub");
		Hism->SetCastShadow(bTree);
		Hism->SetAffectDistanceFieldLighting(false);
		Hism->bAffectDistanceFieldLighting = false;
		Hism->SetCullDistances(80000.f, 180000.f); // 800 m → 1.8 km fade
		Hism->NumCustomDataFloats = 0;

		UMaterialInterface* BaseMat = GetOrCreateStructureMaterial();
		if (BaseMat)
		{
			if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMat, Hism))
			{
				ApplyLightingToMid(Mid);
				const int32 NumMats = FMath::Max(1, Hism->GetNumMaterials());
				for (int32 Mi = 0; Mi < NumMats; ++Mi)
				{
					Hism->SetMaterial(Mi, Mid);
				}
				R.FoliageMids.Add(Mid);
			}
		}

		Hism->RegisterComponent();
		Hism->PreAllocateInstancesMemory(Pair.Value.Num());
		for (const FTransform& Xf : Pair.Value)
		{
			Hism->AddInstance(Xf, /*bWorldSpace*/ true);
		}
		Hism->BuildTreeIfOutdated(/*Async*/ true, /*ForceUpdate*/ false);
		TotalInst += Pair.Value.Num();
	}

	if (TotalInst <= 0)
	{
		Owner->Destroy();
		return;
	}

	R.FoliageActor = Owner;
	R.FoliageInstanceCount = TotalInst;
	ResidentFoliageInstances += TotalInst;
	++ResidentVegTiles;
	// Approx cost for HUD (instances, not verts).
	ResidentVerts += TotalInst * 8;
	UE_LOG(LogSailSim, Log, TEXT("NantucketStructures: foliage tile %d_%d instances=%d species=%d"),
		Desc.Tx, Desc.Ty, TotalInst, BySpecies.Num());
}

void UNantucketStructuresSubsystem::ReleaseTile(uint64 Key)
{
	FResidentTile* R = Resident.Find(Key);
	if (!R) return;
	ResidentVerts = FMath::Max(0, ResidentVerts - R->VertCount);
	if (R->bStaticMesh)
	{
		ResidentSm = FMath::Max(0, ResidentSm - 1);
	}
	else
	{
		ResidentPmc = FMath::Max(0, ResidentPmc - 1);
	}
	R->Mid = nullptr;

	auto DestroyCompOwner = [](USceneComponent* Any)
	{
		if (!Any) return;
		if (AActor* Owner = Any->GetOwner())
		{
			Owner->Destroy();
		}
		else
		{
			Any->DestroyComponent();
		}
	};

	if (R->FoliageInstanceCount > 0)
	{
		ResidentVerts = FMath::Max(0, ResidentVerts - R->FoliageInstanceCount * 8);
		ResidentFoliageInstances = FMath::Max(0, ResidentFoliageInstances - R->FoliageInstanceCount);
		ResidentVegTiles = FMath::Max(0, ResidentVegTiles - 1);
	}
	if (R->FoliageActor)
	{
		R->FoliageActor->Destroy();
		R->FoliageActor = nullptr;
	}
	R->FoliageMids.Reset();
	R->FoliageInstanceCount = 0;

	USceneComponent* Any = R->StaticMeshComp ? static_cast<USceneComponent*>(R->StaticMeshComp.Get())
		: static_cast<USceneComponent*>(R->ProcMesh.Get());
	DestroyCompOwner(Any);
	R->StaticMeshComp = nullptr;
	R->ProcMesh = nullptr;
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

UStaticMesh* UNantucketStructuresSubsystem::LoadCookedStaticMesh(const FString& AssetPath) const
{
	if (AssetPath.IsEmpty()) return nullptr;

	// Accept either full object path or soft package path without object name.
	FString Path = AssetPath;
	if (!Path.StartsWith(TEXT("/")))
	{
		// Relative content path → /Game/...
		Path = FString(TEXT("/Game/")) / Path;
	}

	if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path))
	{
		return Mesh;
	}
	// Soft path without duplicate name suffix: /Game/Foo/SM_Bar → /Game/Foo/SM_Bar.SM_Bar
	if (!Path.Contains(TEXT(".")))
	{
		const FString Leaf = FPackageName::GetShortName(Path);
		const FString Full = FString::Printf(TEXT("%s.%s"), *Path, *Leaf);
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Full))
		{
			return Mesh;
		}
		return Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *Full));
	}
	return Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *Path));
}

bool UNantucketStructuresSubsystem::LoadMeshOriginFromCooked(const FString& AssetPath, FVector& OutOrigin) const
{
	const FString OriginsFile = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectContentDir() / TEXT("Structures/nantucket/Cooked/mesh_origins.json"));
	if (!FPaths::FileExists(OriginsFile))
	{
		return false;
	}
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *OriginsFile))
	{
		return false;
	}
	TSharedPtr<FJsonObject> RootObj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return false;
	}
	FString Leaf = FPaths::GetBaseFilename(AssetPath);
	// AssetPath may be /Game/.../SM_Struct_x_y_LOD0 without extension
	if (Leaf.IsEmpty())
	{
		Leaf = AssetPath;
		const int32 Slash = Leaf.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Slash != INDEX_NONE) Leaf = Leaf.Mid(Slash + 1);
		const int32 Dot = Leaf.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE) Leaf = Leaf.Left(Dot);
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!RootObj->TryGetArrayField(Leaf, Arr) || !Arr || Arr->Num() < 3)
	{
		return false;
	}
	OutOrigin = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	return true;
}

UStaticMeshComponent* UNantucketStructuresSubsystem::CreateTileStaticMesh(
	const FString& AssetPath, int32 FallbackVerts, const FVector& WorldOrigin,
	int32* OutVerts, UMaterialInstanceDynamic** OutMid)
{
	if (OutVerts) *OutVerts = 0;
	if (OutMid) *OutMid = nullptr;

	UWorld* World = GetWorld();
	if (!World) return nullptr;

	UStaticMesh* SM = LoadCookedStaticMesh(AssetPath);
	if (!SM)
	{
		return nullptr;
	}

	// Local-space cooked mesh must be placed at WorldOrigin. Identity + huge
	// baked world verts triggers FDFMatrix OriginMax ensures (Distance Fields).
	const FTransform Xf(FRotator::ZeroRotator, WorldOrigin);
	FActorSpawnParameters Sp;
	// NAME_None — never assign fixed tile names (unique-name pool exhausts / crashes PIE).
	Sp.Name = NAME_None;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// AActor has no native root — SpawnActor(Xf) does NOT stick without a root.
	// Create Movable root, apply WorldOrigin, then attach mesh at relative zero.
	// (Static + SetWorldTransform spams mobility warnings; Static without set
	// left every building at 0,0,0 ~20 km from the harbor.)
	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner)
	{
		return nullptr;
	}
	Owner->Tags.Add(FName(TEXT("NantucketStructures")));

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->SetWorldTransform(Xf);
	Root->RegisterComponent();

	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetStaticMesh(SM);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	// Allow engine distance cull; never-cull + huge world was a DF/VSM tax.
	Mesh->bNeverDistanceCull = false;
	// Harbor tiles live ~20 km from origin. Mesh distance fields use a limited
	// relative float origin (OriginMax) and ensure-fail on huge world matrices.
	Mesh->SetAffectDistanceFieldLighting(false);
	Mesh->bAffectDistanceFieldLighting = false;
	// Nanite is whatever the cooked asset has; do not force-disable.
	Mesh->RegisterComponent();

	int32 Verts = FallbackVerts;
	if (const FStaticMeshRenderData* RD = SM->GetRenderData())
	{
		if (RD->LODResources.Num() > 0)
		{
			Verts = RD->LODResources[0].GetNumVertices();
		}
	}
	if (OutVerts) *OutVerts = Verts;

	// Always force the authored vertex-color material (cooks ship gray defaults).
	UMaterialInterface* BaseMat = GetOrCreateStructureMaterial();
	if (!BaseMat)
	{
		BaseMat = Mesh->GetMaterial(0);
	}
	if (BaseMat)
	{
		if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMat, Mesh))
		{
			ApplyLightingToMid(Mid);
			// Multi-section cooks: paint every slot so no gray leftovers.
			const int32 NumMats = FMath::Max(1, Mesh->GetNumMaterials());
			for (int32 Mi = 0; Mi < NumMats; ++Mi)
			{
				Mesh->SetMaterial(Mi, Mid);
			}
			if (OutMid) *OutMid = Mid;
		}
	}

	UE_LOG(LogSailSim, Log, TEXT("NantucketStructures: SM %s verts=%d nanite=%d at (%.0f,%.0f,%.0f)"),
		*AssetPath, Verts, SM->IsNaniteEnabled() ? 1 : 0,
		WorldOrigin.X, WorldOrigin.Y, WorldOrigin.Z);

	return Mesh;
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

	// Localize verts around bounds center — absolute world cm blows DF OriginMax.
	FVector Origin = Data.Bounds.GetCenter();
	for (FVector& P : Data.Positions)
	{
		P -= Origin;
	}
	Data.Bounds = FBox(ForceInit);
	for (const FVector& P : Data.Positions)
	{
		Data.Bounds += P;
	}

	const FTransform Xf(FRotator::ZeroRotator, Origin);
	FActorSpawnParameters Sp;
	Sp.Name = NAME_None; // see CreateTileStaticMesh — fixed names crash PIE unique-name pool
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Same root-before-transform rule as CreateTileStaticMesh — PMC verts are
	// local; actor must sit at mesh bounds center in world cm.
	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner)
	{
		return nullptr;
	}
	Owner->Tags.Add(FName(TEXT("NantucketStructures")));

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->SetWorldTransform(Xf);
	Root->RegisterComponent();

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner, NAME_None, RF_Transient);
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->RegisterComponent();
	// Visual-only for dense architecture (harbor tile ~500k verts).
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	Mesh->bNeverDistanceCull = false;
	Mesh->SetAffectDistanceFieldLighting(false);
	Mesh->bAffectDistanceFieldLighting = false;

	TArray<FVector2D> UV0;
	UV0.SetNum(Data.Positions.Num());
	for (int32 I = 0; I < Data.Positions.Num(); ++I)
	{
		// Planar UV from local XY
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

	UE_LOG(LogSailSim, Log,
		TEXT("NantucketStructures: PMC %s verts=%d tris=%d origin=(%.0f,%.0f,%.0f) localBounds=%s"),
		*RelPath, Data.Positions.Num(), Data.Indices.Num() / 3,
		Origin.X, Origin.Y, Origin.Z, *Data.Bounds.ToString());

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
