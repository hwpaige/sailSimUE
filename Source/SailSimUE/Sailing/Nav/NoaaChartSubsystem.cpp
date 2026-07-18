#include "Sailing/Nav/NoaaChartSubsystem.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "SailSimUE.h"

void UNoaaChartSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ResolveTileRoot();
	UE_LOG(LogSailSim, Log, TEXT("NoaaChartSubsystem: tile root %s (%s)"),
		*TileRoot, bRootValid ? TEXT("ok") : TEXT("MISSING"));
}

void UNoaaChartSubsystem::Deinitialize()
{
	for (auto& Pair : Cache)
	{
		if (Pair.Value.Texture)
		{
			Pair.Value.Texture->RemoveFromRoot();
		}
	}
	Cache.Empty();
	Super::Deinitialize();
}

void UNoaaChartSubsystem::ResolveTileRoot()
{
	bRootValid = false;
	TileRoot.Empty();

	TArray<FString> Candidates;

	// 1) Project Content (symlink or copied tiles)
	Candidates.Add(FPaths::ConvertRelativePathToFull(
		FPaths::ProjectContentDir() / TEXT("Charts/nantucket")));

	// 2) Sibling sail-sim checkout (dev machine layout)
	Candidates.Add(FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/charts/nantucket")));

	// 3) Absolute common path
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/sail-sim/frontend/public/charts/nantucket"));

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	for (const FString& C : Candidates)
	{
		const FString Probe = C / TEXT("11");
		if (PF.DirectoryExists(*C) && PF.DirectoryExists(*Probe))
		{
			TileRoot = C;
			bRootValid = true;
			return;
		}
	}

	// Still set preferred path for logging
	if (Candidates.Num() > 0)
	{
		TileRoot = Candidates[0];
	}
}

FString UNoaaChartSubsystem::TileKey(int32 Z, int32 X, int32 Y)
{
	return FString::Printf(TEXT("%d/%d/%d"), Z, X, Y);
}

FString UNoaaChartSubsystem::TileFilePath(int32 Z, int32 X, int32 Y) const
{
	return TileRoot / FString::Printf(TEXT("%d/%d/%d.png"), Z, X, Y);
}

void UNoaaChartSubsystem::EvictIfNeeded()
{
	if (Cache.Num() <= MaxCache) return;

	// Drop oldest Ready/Missing entries
	TArray<TPair<FString, double>> Ages;
	Ages.Reserve(Cache.Num());
	for (const auto& Pair : Cache)
	{
		if (Pair.Value.State != ETileState::Loading)
		{
			Ages.Emplace(Pair.Key, Pair.Value.LastUseTime);
		}
	}
	Ages.Sort([](const TPair<FString, double>& A, const TPair<FString, double>& B)
	{
		return A.Value < B.Value;
	});

	const int32 ToDrop = FMath::Max(0, Cache.Num() - MaxCache + 32);
	for (int32 I = 0; I < ToDrop && I < Ages.Num(); ++I)
	{
		if (FTileEntry* E = Cache.Find(Ages[I].Key))
		{
			if (E->Texture)
			{
				E->Texture->RemoveFromRoot();
			}
		}
		Cache.Remove(Ages[I].Key);
	}
}

UTexture2D* UNoaaChartSubsystem::LoadTileSync(int32 Z, int32 X, int32 Y)
{
	const FString Path = TileFilePath(Z, X, Y);
	if (!FPaths::FileExists(Path))
	{
		return nullptr;
	}

	// ImportFileAsTexture2D creates a transient texture from disk PNG/JPEG
	UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(Path);
	if (!Tex)
	{
		return nullptr;
	}

	Tex->AddToRoot(); // keep alive outside UObject graph
	Tex->Filter = TF_Bilinear;
	Tex->SRGB = true;
	Tex->UpdateResource();
	return Tex;
}

UTexture2D* UNoaaChartSubsystem::RequestTile(int32 Z, int32 X, int32 Y)
{
	if (!bRootValid) return nullptr;
	const int32 Max = 1 << Z;
	if (X < 0 || Y < 0 || X >= Max || Y >= Max) return nullptr;

	const FString Key = TileKey(Z, X, Y);
	const double Now = FPlatformTime::Seconds();

	if (FTileEntry* Existing = Cache.Find(Key))
	{
		Existing->LastUseTime = Now;
		if (Existing->State == ETileState::Ready)
		{
			return Existing->Texture;
		}
		if (Existing->State == ETileState::Missing)
		{
			return nullptr;
		}
		// Loading — shouldn't stick; treat as miss
		return nullptr;
	}

	// Load synchronously (tiles are local SSD PNGs; LRU keeps hot set)
	FTileEntry Entry;
	Entry.LastUseTime = Now;
	UTexture2D* Tex = LoadTileSync(Z, X, Y);
	if (Tex)
	{
		Entry.State = ETileState::Ready;
		Entry.Texture = Tex;
		Cache.Add(Key, Entry);
		EvictIfNeeded();
		return Tex;
	}

	Entry.State = ETileState::Missing;
	Entry.Texture = nullptr;
	Cache.Add(Key, Entry);
	return nullptr;
}

UNoaaChartSubsystem::FResolvedTile UNoaaChartSubsystem::ResolveTile(int32 Z, int32 X, int32 Y, int32 MinZ)
{
	FResolvedTile Out;

	// Prefer exact
	if (UTexture2D* Exact = RequestTile(Z, X, Y))
	{
		Out.Texture = Exact;
		Out.U0 = 0.f;
		Out.V0 = 0.f;
		Out.U1 = 1.f;
		Out.V1 = 1.f;
		Out.SourceZ = Z;
		return Out;
	}

	// Cascade to coarser parents (web NavChart._resolveTile)
	for (int32 Depth = 1; Depth <= Z - MinZ; ++Depth)
	{
		const int32 Span = 1 << Depth;
		const int32 Pz = Z - Depth;
		const int32 Px = X / Span;
		const int32 Py = Y / Span;
		if (UTexture2D* Parent = RequestTile(Pz, Px, Py))
		{
			const float Cell = 1.f / float(Span);
			const int32 SubX = X % Span;
			const int32 SubY = Y % Span;
			Out.Texture = Parent;
			Out.U0 = SubX * Cell;
			Out.V0 = SubY * Cell;
			Out.U1 = Out.U0 + Cell;
			Out.V1 = Out.V0 + Cell;
			Out.SourceZ = Pz;
			return Out;
		}
	}

	return Out;
}
