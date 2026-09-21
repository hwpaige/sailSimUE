#include "Sailing/Terrain/NantucketTerrainSubsystem.h"
#include "Sailing/Terrain/NavtMeshLoader.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Nav/NavGeo.h"
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
#include "UObject/ConstructorHelpers.h"

void UNantucketTerrainSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ReloadManifest();
}

void UNantucketTerrainSubsystem::SetSeason(float InSeason01)
{
	Season01 = FMath::Clamp(InSeason01, 0.f, 1.f);
	for (TPair<uint64, FResidentTile>& Pair : Resident)
	{
		ApplySeasonToMid(Pair.Value.Mid);
	}
}

void UNantucketTerrainSubsystem::ApplySeasonToMid(UMaterialInstanceDynamic* Mid) const
{
	if (!Mid) return;
	Mid->SetScalarParameterValue(TEXT("Season01"), Season01);
}

void UNantucketTerrainSubsystem::Deinitialize()
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

TStatId UNantucketTerrainSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UNantucketTerrainSubsystem, STATGROUP_Tickables);
}

void UNantucketTerrainSubsystem::SetEnabled(bool bInEnabled)
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
	}
}

FString UNantucketTerrainSubsystem::GetStatusLine() const
{
	if (!bManifestLoaded)
	{
		return TEXT("terrain: no manifest");
	}
	return FString::Printf(TEXT("terrain: %d tiles resident  loadR=%d lod0=%d"),
		Resident.Num(), LoadRadius, Lod0Radius);
}

bool UNantucketTerrainSubsystem::ResolveTerrainRoot(FString& OutRoot) const
{
	TArray<FString> Candidates;
	Candidates.Add(FPaths::ProjectContentDir() / TEXT("Terrain/nantucket"));
	Candidates.Add(FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/terrain/nantucket"));
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/sail-sim/frontend/public/terrain/nantucket"));
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/SailSimUE/Content/Terrain/nantucket"));

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

bool UNantucketTerrainSubsystem::ReloadManifest()
{
	bManifestLoaded = false;
	TileDescs.Reset();
	KeyToDescIndex.Empty();
	FailedTileKeys.Empty();

	FString Root;
	if (!ResolveTerrainRoot(Root))
	{
		UE_LOG(LogSailSim, Warning, TEXT("NantucketTerrain: no terrain root found (run Scripts/export_nantucket_terrain_ue.py)"));
		bEnabled = false;
		return false;
	}
	TerrainRoot = Root;
	return LoadManifestFromRoot(Root);
}

bool UNantucketTerrainSubsystem::LoadManifestFromRoot(const FString& Root)
{
	// Prefer UE streaming manifest; fall back to source web manifest
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
	if (StreamCols < 1) StreamCols = 16;
	if (StreamRows < 1) StreamRows = 16;

	const TSharedPtr<FJsonObject>* StreamObj = nullptr;
	if (RootObj->TryGetObjectField(TEXT("stream"), StreamObj) && StreamObj && (*StreamObj).IsValid())
	{
		LoadRadius = FMath::Clamp((int32)(*StreamObj)->GetNumberField(TEXT("loadRadius")), 1, 8);
		UnloadRadius = FMath::Max(LoadRadius + 1, (int32)(*StreamObj)->GetNumberField(TEXT("unloadRadius")));
		if ((*StreamObj)->HasField(TEXT("lod0Radius")))
		{
			Lod0Radius = FMath::Clamp((int32)(*StreamObj)->GetNumberField(TEXT("lod0Radius")), 0, 4);
		}
		if (Lod0Radius > LoadRadius)
		{
			Lod0Radius = LoadRadius;
		}
		// Harbor perf A/B: honor authored lod0Radius (incl. 0). Do NOT raise to
		// LoadRadius — that made LoadRadius=1 / Lod0Radius=0 a no-op. EnsureTile
		// still falls back LOD1→LOD0 if a shell *_l1 mesh is empty.
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

		FNantucketTileDesc D;
		D.Id = T->GetStringField(TEXT("id"));
		D.Tx = (int32)T->GetNumberField(TEXT("tx"));
		D.Ty = (int32)T->GetNumberField(TEXT("ty"));
		D.FileLod0 = T->GetStringField(TEXT("file"));
		if (T->HasField(TEXT("fileLod1")))
		{
			D.FileLod1 = T->GetStringField(TEXT("fileLod1"));
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
		TEXT("NantucketTerrain: loaded %d tiles from %s (loadR=%d unloadR=%d lod0=%d)"),
		TileDescs.Num(), *Path, LoadRadius, UnloadRadius, Lod0Radius);
	return bManifestLoaded;
}

FVector UNantucketTerrainSubsystem::GetFocusLocation() const
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

void UNantucketTerrainSubsystem::ForceStreamAround(FVector FocusWorld)
{
	if (!bEnabled || !bManifestLoaded) return;
	TimeSinceUpdate = 0.f;
	UpdateStreaming(FocusWorld);
}

void UNantucketTerrainSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !bManifestLoaded) return;

	SAIL_PERF_SCOPE(Terrain);
	FSailSimPerf::Get().TerrainTiles = Resident.Num();
	FSailSimPerf::Get().TerrainVerts = ResidentVerts;

	TimeSinceUpdate += DeltaTime;
	if (TimeSinceUpdate < UpdateIntervalSec) return;
	TimeSinceUpdate = 0.f;

	UpdateStreaming(GetFocusLocation());
}

void UNantucketTerrainSubsystem::UpdateStreaming(const FVector& FocusWorld)
{
	// Find nearest tile by world XY (not lat/lon grid index) — robust to irregular tile presence
	int32 BestTx = 0, BestTy = 0;
	float BestD2 = TNumericLimits<float>::Max();
	for (const FNantucketTileDesc& D : TileDescs)
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

	// Desired set
	TSet<uint64> Desired;
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
			Desired.Add(K);
			EnsureTile(Tx, Ty, Lod);
		}
	}

	// Unload far tiles (hysteresis: unloadRadius > loadRadius)
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
	(void)Desired;
}

void UNantucketTerrainSubsystem::EnsureTile(int32 Tx, int32 Ty, int32 Lod)
{
	const uint64 K = TileKey(Tx, Ty);
	if (const FResidentTile* Existing = Resident.Find(K))
	{
		// Keep higher detail; only rebuild if we have LOD1 but now need LOD0
		if (Existing->Lod == 0 || Existing->Lod <= Lod)
		{
			return;
		}
		ReleaseTile(K);
	}

	const int32* DescIdx = KeyToDescIndex.Find(K);
	if (!DescIdx || !TileDescs.IsValidIndex(*DescIdx)) return;
	const FNantucketTileDesc& Desc = TileDescs[*DescIdx];

	// Prefer requested LOD; shell (LOD1) meshes in the current bake are often
	// corrupt (nI=0). Always fall back to LOD0 so the load disc is never hollow.
	int32 EffectiveLod = Lod;
	FString Rel;
	if (EffectiveLod == 1 && !Desc.FileLod1.IsEmpty())
	{
		const FString Abs1 = TerrainRoot / Desc.FileLod1;
		if (FPaths::FileExists(Abs1) && !FailedTileKeys.Contains(FailKey(Tx, Ty, 1)))
		{
			Rel = Desc.FileLod1;
		}
		else
		{
			// Missing or previously-failed shell → full mesh
			EffectiveLod = 0;
			Rel = Desc.FileLod0;
		}
	}
	else
	{
		EffectiveLod = 0;
		Rel = Desc.FileLod0;
	}

	if (FailedTileKeys.Contains(FailKey(Tx, Ty, EffectiveLod)))
	{
		// Last chance: if LOD0 poisoned but LOD1 was never tried (unlikely), stop.
		if (EffectiveLod == 0)
		{
			return;
		}
		EffectiveLod = 0;
		Rel = Desc.FileLod0;
		if (FailedTileKeys.Contains(FailKey(Tx, Ty, 0)))
		{
			return;
		}
	}

	UProceduralMeshComponent* Mesh = CreateTileMesh(Rel);
	if (!Mesh && EffectiveLod == 1)
	{
		// Shell LOD parse fail (typical: nV>0 nI=0) — do not leave a hole.
		FailedTileKeys.Add(FailKey(Tx, Ty, 1));
		EffectiveLod = 0;
		Rel = Desc.FileLod0;
		if (!FailedTileKeys.Contains(FailKey(Tx, Ty, 0)))
		{
			Mesh = CreateTileMesh(Rel);
		}
	}
	if (!Mesh)
	{
		FailedTileKeys.Add(FailKey(Tx, Ty, EffectiveLod));
		return;
	}

	int32 Verts = 0;
	if (AActor* Owner = Mesh->GetOwner())
	{
		for (const FName& Tag : Owner->Tags)
		{
			const FString S = Tag.ToString();
			if (S.StartsWith(TEXT("NavtVerts_")))
			{
				Verts = FCString::Atoi(*S.Mid(10));
				break;
			}
		}
	}

	FResidentTile R;
	R.Tx = Tx;
	R.Ty = Ty;
	R.Lod = EffectiveLod;
	R.VertCount = Verts;
	R.Mesh = Mesh;
	if (UMaterialInterface* Mat = Mesh->GetMaterial(0))
	{
		R.Mid = Cast<UMaterialInstanceDynamic>(Mat);
		ApplySeasonToMid(R.Mid);
	}
	ResidentVerts += Verts;
	Resident.Add(K, MoveTemp(R));
}

void UNantucketTerrainSubsystem::ReleaseTile(uint64 Key)
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
}

UMaterialInterface* UNantucketTerrainSubsystem::GetOrCreateTerrainMaterial()
{
	if (TerrainMaterial) return TerrainMaterial;

	// AAA pattern: assign a Material *Instance* (MIC), create per-tile MIDs for
	// Season01 — never mutate the master material asset at runtime.
	static const TCHAR* Candidates[] = {
		TEXT("/Game/Materials/Navt/MI_NavtTerrain.MI_NavtTerrain"),
		TEXT("/Game/Materials/Navt/M_NavtTerrain.M_NavtTerrain"),
		TEXT("/Game/Materials/M_NavtTerrain.M_NavtTerrain"),
		TEXT("/Game/Materials/Navt/M_NavtVertexColor.M_NavtVertexColor"),
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
	};
	for (const TCHAR* Path : Candidates)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, Path))
		{
			TerrainMaterial = M;
			UE_LOG(LogSailSim, Log, TEXT("NantucketTerrain: material %s"), Path);
			return TerrainMaterial;
		}
	}
	return TerrainMaterial;
}

UProceduralMeshComponent* UNantucketTerrainSubsystem::CreateTileMesh(const FString& RelPath)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	FString Abs = FPaths::ConvertRelativePathToFull(TerrainRoot / RelPath);
	// Resolve symlinks (Content/Terrain/nantucket/tiles → sail-sim bake)
	Abs = FPaths::ConvertRelativePathToFull(IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Abs));
	if (!FPaths::FileExists(Abs))
	{
		// Fallback: direct sail-sim tree
		const FString Fallback = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/terrain/nantucket") / RelPath);
		if (FPaths::FileExists(Fallback))
		{
			Abs = Fallback;
		}
		else
		{
			UE_LOG(LogSailSim, Warning, TEXT("NantucketTerrain: missing mesh %s"), *Abs);
			return nullptr;
		}
	}
	FNavtMeshData Data;
	if (!FNavtMeshLoader::LoadFile(Abs, Data))
	{
		UE_LOG(LogSailSim, Warning, TEXT("NantucketTerrain: failed parse %s"), *Abs);
		return nullptr;
	}

	// -------------------------------------------------------------------------
	// AAA open-world mesh pattern (same as cooked structures):
	//   • Vertices in LOCAL space (centered on tile AABB)
	//   • Actor/root at tile world origin
	// Absolute verts at |XY|≈2e6 cm (harbor ~22 km from UE origin) break
	// Lumen / VSM / float precision — surfaces look bright for a frame then go dark.
	// Structures already do this; terrain was the outlier.
	// -------------------------------------------------------------------------
	const FVector TileOrigin = Data.Bounds.GetCenter();
	for (FVector& P : Data.Positions)
	{
		P -= TileOrigin;
	}
	// Normals are direction-only; recompute not required after pure translation.

	FActorSpawnParameters Sp;
	Sp.Name = NAME_None;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner)
	{
		return nullptr;
	}
	Owner->Tags.Add(FName(TEXT("NantucketTerrain")));

	// Movable root so SetWorldLocation is valid (Static + SetWorldTransform warns).
	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->SetWorldLocation(TileOrigin);
	Root->RegisterComponent();

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner, NAME_None, RF_Transient);
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetRelativeLocation(FVector::ZeroVector);

	// Material before geometry (first drawn frame has correct shader).
	UMaterialInterface* BaseMat = GetOrCreateTerrainMaterial();
	UMaterialInstanceDynamic* Mid = nullptr;
	if (BaseMat)
	{
		Mid = UMaterialInstanceDynamic::Create(BaseMat, Mesh);
		if (Mid)
		{
			Mid->SetVectorParameterValue(TEXT("Tint"), FLinearColor(1.f, 1.f, 1.f, 1.f));
			Mid->SetScalarParameterValue(TEXT("ColorBoost"), 1.4f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Specular"), 0.25f);
			ApplySeasonToMid(Mid);
			Mesh->SetMaterial(0, Mid);
		}
		else
		{
			Mesh->SetMaterial(0, BaseMat);
		}
	}

	Mesh->RegisterComponent();
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	Mesh->bUseComplexAsSimpleCollision = false;
	// Large ground sheets: no self-shadow (VSM acne). Receive sun + Lumen.
	Mesh->SetCastShadow(false);
	Mesh->bCastDynamicShadow = false;
	Mesh->bCastStaticShadow = false;
	Mesh->bCastContactShadow = false;
	Mesh->bCastFarShadow = false;
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	Mesh->bNeverDistanceCull = false;
	Mesh->bAffectDynamicIndirectLighting = true;
	// PMC has no mesh SDF; claiming DF lighting does nothing useful — leave off.
	Mesh->SetAffectDistanceFieldLighting(false);
	Mesh->bAffectDistanceFieldLighting = false;
	Mesh->SetRenderInMainPass(true);
	Mesh->SetRenderCustomDepth(false);
	Mesh->bReceivesDecals = true;

	TArray<FVector2D> UV0;
	UV0.SetNum(Data.Positions.Num());
	TArray<FProcMeshTangent> Tangents;
	Tangents.SetNum(Data.Positions.Num());
	for (int32 I = 0; I < Data.Positions.Num(); ++I)
	{
		// Planar UV from local XY (stable after centering)
		UV0[I] = FVector2D(Data.Positions[I].X * 0.0001f, Data.Positions[I].Y * 0.0001f);
		const FVector N = Data.Normals.IsValidIndex(I) ? Data.Normals[I] : FVector::UpVector;
		FVector T = FVector::CrossProduct(FVector::UpVector, N);
		if (T.SizeSquared() < 1e-8f)
		{
			T = FVector::CrossProduct(FVector(1.f, 0.f, 0.f), N);
		}
		T.Normalize();
		Tangents[I] = FProcMeshTangent(T, /*bFlipTangentY*/ false);
	}

	Mesh->CreateMeshSection_LinearColor(
		0,
		Data.Positions,
		Data.Indices,
		Data.Normals,
		UV0,
		Data.Colors,
		Tangents,
		/*bCreateCollision*/ false);
	if (Mid)
	{
		Mesh->SetMaterial(0, Mid);
	}
	else if (BaseMat)
	{
		Mesh->SetMaterial(0, BaseMat);
	}

	// Force bounds update so Lumen/culling see local-space mesh at world origin.
	Mesh->UpdateBounds();
	Mesh->MarkRenderStateDirty();

	Owner->Tags.Add(FName(*FString::Printf(TEXT("NavtVerts_%d"), Data.Positions.Num())));

	UE_LOG(LogSailSim, Log,
		TEXT("NantucketTerrain: loaded %s verts=%d tris=%d origin=(%.0f,%.0f,%.0f) mat=%s"),
		*RelPath, Data.Positions.Num(), Data.Indices.Num() / 3,
		TileOrigin.X, TileOrigin.Y, TileOrigin.Z,
		BaseMat ? *BaseMat->GetName() : TEXT("none"));

	return Mesh;
}
