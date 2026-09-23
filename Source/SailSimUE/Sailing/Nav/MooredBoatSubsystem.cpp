#include "Sailing/Nav/MooredBoatSubsystem.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Nav/EncAidSubsystem.h"
#include "Sailing/Wind/WindFieldSubsystem.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/EngineTypes.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "ProceduralMeshComponent.h"
#include "ProceduralMeshConversion.h"
#include "MeshDescription.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

namespace MooredBoatPrivate
{
	static uint32 HashU32(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	static float Hash01(int32 Seed, int32 Salt)
	{
		return (HashU32(static_cast<uint32>(Seed * 73856093u + Salt * 19349663u)) & 0xFFFFFFu)
			/ static_cast<float>(0xFFFFFFu);
	}
}

void UMooredBoatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	EnsureYachtMaterials();
	ReloadSlots();
}

void UMooredBoatSubsystem::EnsureYachtMaterials()
{
	// Prefer authored DefaultLit yacht instances; fall back to buoy clean / BasicShape.
	auto LoadMI = [](const TCHAR* Path) -> UMaterialInterface*
	{
		return LoadObject<UMaterialInterface>(nullptr, Path);
	};

	// HullPaint uses local-Z hard thresholds → crisp boot/cove (not vertex-color stairsteps).
	UMaterialInterface* HullPaint = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_HullPaint.MI_Yacht_HullPaint"));
	UMaterialInterface* Gelcoat = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Gelcoat.MI_Yacht_Gelcoat"));
	UMaterialInterface* Deck = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Deck.MI_Yacht_Deck"));
	UMaterialInterface* Cabin = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Cabin.MI_Yacht_Cabin"));
	UMaterialInterface* Glass = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Glass.MI_Yacht_Glass"));
	UMaterialInterface* Keel = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Keel.MI_Yacht_Keel"));
	YachtSparMat = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Spar.MI_Yacht_Spar"));
	YachtRopeMat = LoadMI(TEXT("/Game/Materials/Yacht/MI_Yacht_Rope.MI_Yacht_Rope"));

	UMaterialInterface* BuoyLit = LoadMI(TEXT("/Game/Buoys/Materials/M_Buoys_1_clean.M_Buoys_1_clean"));
	UMaterialInterface* Basic = LoadMI(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!Basic)
	{
		Basic = LoadMI(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}
	// Prefer solid gelcoat over textured buoy (buoy UVs on hull look "low-res/pixelated").
	UMaterialInterface* Fallback = HullPaint ? HullPaint : (Gelcoat ? Gelcoat : (BuoyLit ? BuoyLit : Basic));
	UMaterialInterface* HullMat = HullPaint ? HullPaint : Fallback;

	const int32 N = static_cast<int32>(EMooredHullPart::Other) + 1;
	YachtMats.SetNum(N);
	auto Set = [&](EMooredHullPart P, UMaterialInterface* M)
	{
		YachtMats[static_cast<int32>(P)] = M ? M : Fallback;
	};
	// All topsides paint roles share HullPaint (shader does antifoul/boot/stripe/gelcoat).
	Set(EMooredHullPart::HullGloss, HullMat);
	Set(EMooredHullPart::BootStripe, HullMat);
	Set(EMooredHullPart::HullStripe, HullMat);
	Set(EMooredHullPart::Antifoul, HullMat);
	Set(EMooredHullPart::Deck, Deck);
	Set(EMooredHullPart::Cabin, Cabin);
	Set(EMooredHullPart::Windows, Glass);
	Set(EMooredHullPart::Keel, Keel);
	Set(EMooredHullPart::Other, Fallback);

	SparMaterial = YachtSparMat ? YachtSparMat.Get() : Fallback;
	if (YachtRopeMat)
	{
		LineMaterial = UMaterialInstanceDynamic::Create(YachtRopeMat.Get(), this);
	}
	else if (SparMaterial)
	{
		LineMaterial = UMaterialInstanceDynamic::Create(SparMaterial.Get(), this);
		if (LineMaterial)
		{
			const FLinearColor Rope(0.12f, 0.11f, 0.10f, 1.f);
			LineMaterial->SetVectorParameterValue(TEXT("Color"), Rope);
			LineMaterial->SetVectorParameterValue(TEXT("BaseColor"), Rope);
		}
	}

	// Prefer one-sided deck; only windows/glass need two-sided fallback.
	TwoSidedBaseMat = Glass ? Glass : Cabin;
	if (!TwoSidedBaseMat) TwoSidedBaseMat = Fallback;

	UE_LOG(LogSailSim, Log,
		TEXT("MooredBoats: yacht materials hullPaint=%s gelcoat=%s spar=%s"),
		HullPaint ? TEXT("yes") : TEXT("no"),
		Gelcoat ? TEXT("yes") : TEXT("no"),
		YachtSparMat ? TEXT("yes") : TEXT("no"));
}

UMaterialInterface* UMooredBoatSubsystem::MaterialForPart(EMooredHullPart Part) const
{
	const int32 I = static_cast<int32>(Part);
	if (YachtMats.IsValidIndex(I) && YachtMats[I])
	{
		return YachtMats[I].Get();
	}
	return SparMaterial.Get();
}

void UMooredBoatSubsystem::Deinitialize()
{
	ClearAll();
	HullSections.Reset();
	KeelSections.Reset();
	bTemplateReady = false;
	Slots.Reset();
	bSlotsReady = false;
	Super::Deinitialize();
}

bool UMooredBoatSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UMooredBoatSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMooredBoatSubsystem, STATGROUP_Tickables);
}

FString UMooredBoatSubsystem::GetStatusLine() const
{
	return FString::Printf(TEXT("moored scenery hism=%d/%d heroes near=%d (MaxBoats=%d NearFull≤%d)"),
		MidHismSlotToInstance.Num(), MooringSceneryInstanceCount, Resident.Num(),
		MaxBoats, MaxNearFullBoats);
}

bool UMooredBoatSubsystem::ResolveAidsPath(FString& OutPath) const
{
	TArray<FString> Candidates;
	Candidates.Add(FPaths::ProjectContentDir() / TEXT("Nav/enc_aids_nantucket.json"));
	Candidates.Add(FPaths::ProjectDir() / TEXT("Content/Nav/enc_aids_nantucket.json"));
	Candidates.Add(TEXT("/Users/harrison/PycharmProjects/SailSimUE/Content/Nav/enc_aids_nantucket.json"));
	for (const FString& C : Candidates)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(C);
		if (FPaths::FileExists(Full))
		{
			OutPath = Full;
			return true;
		}
	}
	return false;
}

bool UMooredBoatSubsystem::ReloadSlots()
{
	ClearAll();
	Slots.Reset();
	bSlotsReady = false;

	FString Path;
	if (!ResolveAidsPath(Path))
	{
		UE_LOG(LogSailSim, Warning, TEXT("MooredBoats: missing enc_aids_nantucket.json"));
		return false;
	}

	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *Path)) return false;
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("aids"), Arr) || !Arr) return false;

	struct FCand
	{
		FVector World;
		int32 MeshId = 1;
		int32 StableId = 0;
		float DistToHarbor = 0.f;
	};
	const FVector2D Harbor = FNavGeo::BoatStartWorldCm2D();
	TArray<FCand> Cands;
	Cands.Reserve(Arr->Num());
	int32 Idx = 0;
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		++Idx;
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		const FString Kind = O->GetStringField(TEXT("kind"));
		if (!Kind.Equals(TEXT("mooring_point"), ESearchCase::IgnoreCase)) continue;
		const FString Shape = O->GetStringField(TEXT("shape"));
		if (Shape.Equals(TEXT("dolphin"), ESearchCase::IgnoreCase)) continue;
		const int32 MeshId = FMath::Clamp(static_cast<int32>(O->GetNumberField(TEXT("mesh"))), 1, 9);
		if (MeshId != 1 && MeshId != 2) continue;

		const double Lat = O->GetNumberField(TEXT("lat"));
		const double Lon = O->GetNumberField(TEXT("lon"));
		double X = 0, Y = 0;
		FNavGeo::LatLonToWorldCm(Lat, Lon, X, Y);
		FCand C;
		C.World = FVector(static_cast<float>(X), static_cast<float>(Y), 0.f);
		C.MeshId = MeshId;
		C.StableId = static_cast<int32>(O->GetNumberField(TEXT("objectId")));
		if (C.StableId == 0) C.StableId = Idx * 9973;
		C.DistToHarbor = FVector2D::Distance(Harbor, FVector2D(C.World.X, C.World.Y));
		Cands.Add(C);
	}

	// Prefer near-harbor moorings first (so boats are obvious at spawn), then fill out.
	Cands.Sort([](const FCand& A, const FCand& B)
	{
		if (A.DistToHarbor != B.DistToHarbor) return A.DistToHarbor < B.DistToHarbor;
		return MooredBoatPrivate::Hash01(A.StableId, 11) < MooredBoatPrivate::Hash01(B.StableId, 11);
	});

	// Harbor fill = MooringSceneryInstanceCount only. MaxBoats / MaxNearFullBoats
	// are hero caps and must not scale this slot budget (still 96 when MaxBoats=1).
	// OccupancyFraction is a soft hint logged only.
	const int32 OccHint = FMath::Clamp(
		FMath::RoundToInt(Cands.Num() * OccupancyFraction), 1, Cands.Num());
	const int32 Target = FMath::Clamp(MooringSceneryInstanceCount, 1, Cands.Num());
	const float MinSp2 = MinBoatSpacingCm * MinBoatSpacingCm;
	const float PreferR2 = PreferNearHarborCm * PreferNearHarborCm;
	TArray<FVector> ChosenXY;
	ChosenXY.Reserve(Target);

	auto TryAdd = [&](const FCand& C) -> bool
	{
		if (Slots.Num() >= Target) return false;
		for (const FVector& P : ChosenXY)
		{
			if (FVector::DistSquared2D(P, C.World) < MinSp2) return false;
		}
		FMooredBoatSlot S;
		S.MooringWorldCm = C.World;
		S.MooringMeshId = C.MeshId;
		const float Scatter = (MooredBoatPrivate::Hash01(C.StableId, 7) - 0.5f) * 28.f;
		S.HeadingDeg = FMath::Fmod(WindFromDeg + Scatter + 360.f, 360.f);
		S.LineLenCm = BaseLineLenCm
			+ (MooredBoatPrivate::Hash01(C.StableId, 19) - 0.5f) * 160.f;
		Slots.Add(S);
		ChosenXY.Add(C.World);
		return true;
	};

	// Pass 1: pack near harbor start
	for (const FCand& C : Cands)
	{
		if (Slots.Num() >= Target) break;
		if (C.DistToHarbor * C.DistToHarbor > PreferR2) continue;
		TryAdd(C);
	}
	// Pass 2: fill remaining across field
	for (const FCand& C : Cands)
	{
		if (Slots.Num() >= Target) break;
		TryAdd(C);
	}

	bSlotsReady = Slots.Num() > 0;
	UE_LOG(LogSailSim, Log,
		TEXT("MooredBoats: %d slots from %d floating moorings (scenery=%d target=%d heroes MaxBoats=%d NearFull≤%d occHint=%d) windFrom=%.0f°"),
		Slots.Num(), Cands.Num(), MooringSceneryInstanceCount, Target, MaxBoats, MaxNearFullBoats, OccHint, WindFromDeg);
	return bSlotsReady;
}

bool UMooredBoatSubsystem::EnsureTemplate()
{
	if (bTemplateReady && HullSections.Num() > 0) return true;

	UWorld* World = GetWorld();
	if (!World) return false;

	// Temporary PMC to parse j105 once, then cache CPU section data.
	AActor* Tmp = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	if (!Tmp) return false;
	Tmp->SetActorHiddenInGame(true);
	Tmp->SetActorEnableCollision(false);
	USceneComponent* Root = NewObject<USceneComponent>(Tmp, TEXT("Root"), RF_Transient);
	Root->SetMobility(EComponentMobility::Movable);
	Tmp->SetRootComponent(Root);
	Root->RegisterComponent();
	UProceduralMeshComponent* TmpHull = NewObject<UProceduralMeshComponent>(Tmp, TEXT("Hull"), RF_Transient);
	TmpHull->SetupAttachment(Root);
	TmpHull->SetMobility(EComponentMobility::Movable);
	TmpHull->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TmpHull->SetVisibility(false);
	TmpHull->RegisterComponent();
	// Keel/rudder on a separate mesh so they never cast onto topsides (player path).
	UProceduralMeshComponent* TmpKeel = NewObject<UProceduralMeshComponent>(Tmp, TEXT("Keel"), RF_Transient);
	TmpKeel->SetupAttachment(Root);
	TmpKeel->SetMobility(EComponentMobility::Movable);
	TmpKeel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TmpKeel->SetVisibility(false);
	TmpKeel->RegisterComponent();

	FBoatJsonLoadResult Result;
	const bool bOk = FBoatMeshFromJson::LoadIntoProceduralMesh(
		TmpHull,
		FBoatMeshFromJson::DefaultJ105Path(),
		SparMaterial.Get(),
		&Result,
		/*Main*/ nullptr,
		/*Jib*/ nullptr,
		/*bSkipSailSections*/ true,
		/*AppendagesMesh*/ TmpKeel);
	if (!bOk || TmpHull->GetNumSections() <= 0)
	{
		UE_LOG(LogSailSim, Warning, TEXT("MooredBoats: failed to load J/105 hull template sections=%d"),
			TmpHull->GetNumSections());
		Tmp->Destroy();
		return false;
	}

	// Solid tints only used if HullPaint MI is missing (fallback MID path).
	const FLinearColor ColTopsides(0.96f, 0.97f, 0.99f, 1.f);
	const FLinearColor ColDeck(0.78f, 0.76f, 0.72f, 1.f);
	const FLinearColor ColCabin(0.93f, 0.94f, 0.95f, 1.f);
	const FLinearColor ColGlass(0.15f, 0.22f, 0.28f, 1.f);
	const FLinearColor ColKeel(0.12f, 0.12f, 0.13f, 1.f);

	auto ClassifyPart = [&](float MinZ, float MaxZ, float AvgR, UMaterialInterface* Mat) -> EMooredHullPart
	{
		// Prefer authored MI from BoatMeshFromJson (section names → Glass/Cabin/Deck…).
		// Never use "MinZ > 95 ⇒ Windows" — that lit entire coachroofs as glass.
		if (Mat)
		{
			const FString Path = Mat->GetPathName();
			if (Path.Contains(TEXT("Glass"))) return EMooredHullPart::Windows;
			if (Path.Contains(TEXT("Cabin"))) return EMooredHullPart::Cabin;
			if (Path.Contains(TEXT("Deck"))) return EMooredHullPart::Deck;
			if (Path.Contains(TEXT("Keel"))) return EMooredHullPart::Keel;
			if (Path.Contains(TEXT("HullPaint")) || Path.Contains(TEXT("Gelcoat")))
			{
				return EMooredHullPart::HullGloss;
			}
		}
		if (MaxZ < 5.f && MinZ < -10.f) return EMooredHullPart::Keel;
		if (MinZ < -50.f) return EMooredHullPart::Keel;
		// Solid cabin house / coachroof (not windows).
		if (MinZ > 55.f && MaxZ > 100.f) return EMooredHullPart::Cabin;
		if (MinZ > 45.f && MaxZ < 100.f && AvgR < 0.88f) return EMooredHullPart::Deck;
		if (MinZ < 20.f && MaxZ < 100.f) return EMooredHullPart::HullGloss;
		if (AvgR < 0.88f) return EMooredHullPart::Deck;
		return EMooredHullPart::Other;
	};

	// Do NOT split the hull into Z-band material sections.
	// The loft is only ~1.5k verts — triangle/vertex paint bands look stair-stepped
	// ("pixelated"). MI_Yacht_HullPaint draws crisp horizontal stripes from local Z.
	// Keel/rudder were loaded onto TmpKeel (no cast shadow) — not into HullSections.
	HullSections.Reset();
	KeelSections.Reset();
	auto CacheSections = [&](UProceduralMeshComponent* Src, TArray<FMooredHullSection>& OutArr, bool bForceKeel)
	{
		if (!Src) return;
		const int32 Num = Src->GetNumSections();
		OutArr.Reserve(OutArr.Num() + Num);
		for (int32 Si = 0; Si < Num; ++Si)
		{
			FProcMeshSection* Sec = Src->GetProcMeshSection(Si);
			if (!Sec || Sec->ProcVertexBuffer.Num() == 0) continue;

			FMooredHullSection Out;
			Out.Positions.Reserve(Sec->ProcVertexBuffer.Num());
			Out.Normals.Reserve(Sec->ProcVertexBuffer.Num());
			Out.UV0.Reserve(Sec->ProcVertexBuffer.Num());
			Out.Tangents.Reserve(Sec->ProcVertexBuffer.Num());
			float MinZ = 1.e9f, MaxZ = -1.e9f, SumR = 0.f;
			for (const FProcMeshVertex& V : Sec->ProcVertexBuffer)
			{
				Out.Positions.Add(V.Position);
				Out.Normals.Add(V.Normal);
				Out.UV0.Add(V.UV0);
				Out.Tangents.Add(V.Tangent);
				MinZ = FMath::Min(MinZ, V.Position.Z);
				MaxZ = FMath::Max(MaxZ, V.Position.Z);
				SumR += FLinearColor(V.Color).R;
			}
			const float AvgR = SumR / FMath::Max(1, Out.Positions.Num());
			Out.Indices.Reserve(Sec->ProcIndexBuffer.Num());
			for (uint32 Ix : Sec->ProcIndexBuffer)
			{
				Out.Indices.Add(static_cast<int32>(Ix));
			}

			UMaterialInterface* SecMat = Src->GetMaterial(Si);
			EMooredHullPart BasePart = bForceKeel
				? EMooredHullPart::Keel
				: ClassifyPart(MinZ, MaxZ, AvgR, SecMat);
			Out.Part = BasePart;
			Out.bTwoSided = (BasePart == EMooredHullPart::Windows);
			switch (BasePart)
			{
			case EMooredHullPart::Deck:
				Out.Tint = FLinearColor(0.97f, 0.97f, 0.98f, 1.f);
				Out.Roughness = 0.62f; Out.Metallic = 0.f; Out.Specular = 0.4f; break;
			case EMooredHullPart::Cabin:
				Out.Tint = ColCabin; Out.Roughness = 0.22f; Out.Metallic = 0.f; Out.Specular = 0.5f; break;
			case EMooredHullPart::Windows:
				Out.Tint = ColGlass; Out.Roughness = 0.05f; Out.Metallic = 0.f; Out.Specular = 0.5f; break;
			case EMooredHullPart::Keel:
				// Slightly lighter than pure charcoal so fins don't read as black “shadows”.
				Out.Tint = FLinearColor(0.22f, 0.23f, 0.24f, 1.f);
				Out.Roughness = 0.45f; Out.Metallic = 0.f; Out.Specular = 0.4f; break;
			default:
				Out.Tint = ColTopsides; Out.Roughness = 0.10f; Out.Metallic = 0.f; Out.Specular = 0.5f; break;
			}
			Out.Colors.Init(Out.Tint, Out.Positions.Num());
			OutArr.Add(MoveTemp(Out));
		}
	};
	CacheSections(TmpHull, HullSections, /*bForceKeel*/ false);
	CacheSections(TmpKeel, KeelSections, /*bForceKeel*/ true);
	// Quarantine any residual keel-classified hull sections into KeelSections.
	for (int32 I = HullSections.Num() - 1; I >= 0; --I)
	{
		if (HullSections[I].Part == EMooredHullPart::Keel)
		{
			KeelSections.Add(MoveTemp(HullSections[I]));
			HullSections.RemoveAt(I);
		}
	}
	int32 TotalVerts = 0;
	for (const FMooredHullSection& S : HullSections) TotalVerts += S.Positions.Num();
	for (const FMooredHullSection& S : KeelSections) TotalVerts += S.Positions.Num();

	TemplateSpars = Result.Spars;
	BowOffsetCm = 520.f;
	if (Result.Sailing.bValid && Result.Sailing.LoaFt > 1.f)
	{
		BowOffsetCm = Result.Sailing.LoaFt * 30.48f * 0.5f;
	}
	// Prefer geometric max +X from first section bounds
	float MaxX = BowOffsetCm;
	for (const FMooredHullSection& S : HullSections)
	{
		for (const FVector& P : S.Positions)
		{
			MaxX = FMath::Max(MaxX, P.X);
		}
	}
	BowOffsetCm = MaxX;

	// Rebuild a clean PMC from classified sections (correct materials/stripes),
	// then bake a shared Nanite static mesh so the field leaves VSM non-Nanite path.
	TmpHull->ClearAllMeshSections();
	ApplyHullTo(TmpHull);
	bHullNaniteReady = BuildHullNaniteMesh(TmpHull);

	Tmp->Destroy();
	bTemplateReady = HullSections.Num() > 0;
	UE_LOG(LogSailSim, Log,
		TEXT("MooredBoats: J/105 template ready hullSec=%d keelSec=%d verts=%d bow=%.0fcm boom=%d mast=%d nanite=%d"),
		HullSections.Num(), KeelSections.Num(), TotalVerts, BowOffsetCm,
		TemplateSpars.bBoomValid ? 1 : 0, TemplateSpars.bMastValid ? 1 : 0,
		bHullNaniteReady ? 1 : 0);
	return bTemplateReady;
}

bool UMooredBoatSubsystem::BuildHullNaniteMesh(UProceduralMeshComponent* SourcePmc)
{
	HullNaniteMesh = nullptr;
	if (!SourcePmc || SourcePmc->GetNumSections() <= 0) return false;

	// Convert PMC → MeshDescription (engine helper from ProceduralMeshComponent plugin).
	FMeshDescription MeshDesc = BuildMeshDescription(SourcePmc);
	if (MeshDesc.Vertices().Num() == 0 || MeshDesc.Triangles().Num() == 0)
	{
		UE_LOG(LogSailSim, Warning, TEXT("MooredBoats: empty MeshDescription from PMC"));
		return false;
	}

	UStaticMesh* SM = NewObject<UStaticMesh>(
		GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_MooredJ105_Hull")),
		RF_Public | RF_Transient);
	if (!SM) return false;

	// Unique material slots in section order (BuildMeshDescription keys by material).
	TArray<FStaticMaterial> StaticMats;
	TSet<UMaterialInterface*> Seen;
	for (int32 Si = 0; Si < SourcePmc->GetNumSections(); ++Si)
	{
		UMaterialInterface* Mat = SourcePmc->GetMaterial(Si);
		if (!Mat || Seen.Contains(Mat)) continue;
		Seen.Add(Mat);
		const FName Slot = Mat->GetFName();
		StaticMats.Add(FStaticMaterial(Mat, Slot, Slot));
	}
	if (StaticMats.Num() == 0 && SparMaterial)
	{
		StaticMats.Add(FStaticMaterial(SparMaterial.Get(), TEXT("Default"), TEXT("Default")));
	}
	SM->SetStaticMaterials(StaticMats);

	// Small craft + thin paint-stripe bands: Nanite position quantization makes gelcoat /
	// boot / cove edges look "pixelated". Prefer a full-detail static mesh (still one
	// multi-section SM instead of 14 PMC components — major VSM win). Optional Nanite
	// is only for experiments / distant LODs.
	FMeshNaniteSettings Nanite = SM->GetNaniteSettings();
	Nanite.bEnabled = bHullUseNanite;
	Nanite.bExplicitTangents = true;
	Nanite.KeepPercentTriangles = 1.0f;
	Nanite.TrimRelativeError = 0.0f;
	Nanite.FallbackPercentTriangles = 1.0f;
	Nanite.FallbackRelativeError = 0.0f;
	// ~0.03 mm step: 2^(-12) cm — keeps waterline stripes crisp if Nanite is forced on.
	if (bHullUseNanite)
	{
		Nanite.PositionPrecision = 12;
		Nanite.NormalPrecision = 8;
	}
	SM->SetNaniteSettings(Nanite);

	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = false;
#if WITH_EDITOR
	// Full editor path preserves authored normals (no auto-smooth across paint bands).
	Params.bFastBuild = false;
	Params.bCommitMeshDescription = true;
#else
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
#endif

	TArray<const FMeshDescription*> Descs;
	Descs.Add(&MeshDesc);
	if (!SM->BuildFromMeshDescriptions(Descs, Params))
	{
		UE_LOG(LogSailSim, Warning, TEXT("MooredBoats: BuildFromMeshDescriptions failed"));
		return false;
	}

#if WITH_EDITOR
	// Do not recompute normals/tangents — stripe bands need hard section edges.
	if (SM->GetNumSourceModels() > 0)
	{
		FStaticMeshSourceModel& Src = SM->GetSourceModel(0);
		Src.BuildSettings.bRecomputeNormals = false;
		Src.BuildSettings.bRecomputeTangents = false;
		Src.BuildSettings.bRemoveDegenerates = false;
		Src.BuildSettings.bUseMikkTSpace = true;
		Src.BuildSettings.bGenerateLightmapUVs = false;
	}
	// Rebuild once with the preserved-normals settings (and optional Nanite).
	{
		FMeshNaniteSettings N2 = SM->GetNaniteSettings();
		N2.bEnabled = bHullUseNanite;
		N2.bExplicitTangents = true;
		N2.KeepPercentTriangles = 1.0f;
		N2.TrimRelativeError = 0.0f;
		N2.FallbackPercentTriangles = 1.0f;
		N2.FallbackRelativeError = 0.0f;
		if (bHullUseNanite)
		{
			N2.PositionPrecision = 12;
			N2.NormalPrecision = 8;
		}
		SM->SetNaniteSettings(N2);
		SM->Build(true);
	}
#endif

	if (FStaticMeshRenderData* RD = SM->GetRenderData())
	{
		if (RD->LODResources.Num() > 0)
		{
			const int32 NumSec = RD->LODResources[0].Sections.Num();
			for (int32 Si = 0; Si < NumSec; ++Si)
			{
				FMeshSectionInfo Info = SM->GetSectionInfoMap().Get(0, Si);
				// Belt-and-suspenders: never let keel/rudder sections cast (should be on
				// a separate appendages mesh, but residual mis-classify is possible).
				bool bCast = true;
				if (StaticMats.IsValidIndex(Info.MaterialIndex))
				{
					if (UMaterialInterface* Mat = StaticMats[Info.MaterialIndex].MaterialInterface)
					{
						const FString Path = Mat->GetPathName();
						if (Path.Contains(TEXT("Keel")) || Path.Contains(TEXT("keel")))
						{
							bCast = false;
						}
					}
				}
				Info.bCastShadow = bCast;
				Info.bEnableCollision = false;
				SM->GetSectionInfoMap().Set(0, Si, Info);
			}
		}
	}

	HullNaniteMesh = SM;
	UE_LOG(LogSailSim, Log,
		TEXT("MooredBoats: hull static mesh ready (nanite=%d mats=%d verts~%d)"),
		SM->IsNaniteEnabled() ? 1 : 0, StaticMats.Num(), MeshDesc.Vertices().Num());
	return true;
}

void UMooredBoatSubsystem::ApplyHullMaterialsToStaticMesh(UStaticMeshComponent* HullSmc) const
{
	if (!HullSmc || !HullNaniteMesh) return;
	const TArray<FStaticMaterial>& Mats = HullNaniteMesh->GetStaticMaterials();
	for (int32 Mi = 0; Mi < Mats.Num(); ++Mi)
	{
		if (UMaterialInterface* M = Mats[Mi].MaterialInterface)
		{
			HullSmc->SetMaterial(Mi, M);
		}
	}
}

void UMooredBoatSubsystem::ApplyBoatPaintScheme(UMeshComponent* HullMesh, int32 SlotIndex) const
{
	if (!HullMesh) return;

	// Harbor palette — more variety than navy/green/red only (stable per slot).
	const FLinearColor WhiteTopsides(0.96f, 0.975f, 0.995f, 1.f);
	const FLinearColor WhiteDeck(0.97f, 0.97f, 0.98f, 1.f);
	const FLinearColor Cream(0.92f, 0.90f, 0.84f, 1.f);
	const FLinearColor Navy(0.02f, 0.06f, 0.16f, 1.f);
	const FLinearColor DarkGreen(0.04f, 0.12f, 0.08f, 1.f);
	const FLinearColor FlagRed(0.42f, 0.06f, 0.06f, 1.f);
	const FLinearColor Teal(0.04f, 0.20f, 0.26f, 1.f);
	const FLinearColor SkyBlue(0.28f, 0.48f, 0.68f, 1.f);
	const FLinearColor Charcoal(0.07f, 0.08f, 0.10f, 1.f);
	const FLinearColor Burgundy(0.26f, 0.05f, 0.10f, 1.f);
	const FLinearColor Sand(0.70f, 0.62f, 0.46f, 1.f);
	const FLinearColor GoldStripe(0.72f, 0.55f, 0.18f, 1.f);
	const FLinearColor BootDefault(0.03f, 0.06f, 0.12f, 1.f);
	const FLinearColor Antifoul(0.16f, 0.10f, 0.08f, 1.f);

	const FLinearColor HullAccents[] = {
		Navy, DarkGreen, FlagRed, Teal, SkyBlue, Charcoal, Burgundy, Sand
	};
	const FLinearColor StripeAccents[] = {
		Navy, FlagRed, GoldStripe, Teal, Cream, Charcoal, SkyBlue, Burgundy
	};
	constexpr int32 NumHull = UE_ARRAY_COUNT(HullAccents);
	constexpr int32 NumStripe = UE_ARRAY_COUNT(StripeAccents);

	// Deterministic per mooring slot (stable across reloads).
	const float H0 = MooredBoatPrivate::Hash01(SlotIndex, 101);
	const float H1 = MooredBoatPrivate::Hash01(SlotIndex, 107);
	const float H2 = MooredBoatPrivate::Hash01(SlotIndex, 113);
	const int32 HullIdx = FMath::Clamp(FMath::FloorToInt(H0 * NumHull), 0, NumHull - 1);
	const int32 StripeIdx = FMath::Clamp(FMath::FloorToInt(H2 * NumStripe), 0, NumStripe - 1);
	const FLinearColor HullAccent = HullAccents[HullIdx];
	const FLinearColor StripeAccent = StripeAccents[StripeIdx];

	// ~42% white hull + colored stripe; rest solid color hull + cream/gold/contrast stripe.
	const bool bWhiteHull = !bVaryBoatPaint || (H1 < 0.42f);
	const FLinearColor HullCol = bWhiteHull ? WhiteTopsides : HullAccent;
	const FLinearColor StripeCol = bWhiteHull ? StripeAccent
		: ((H2 < 0.45f) ? Cream : ((H2 < 0.75f) ? GoldStripe : WhiteTopsides));
	// Boot: navy family for white boats; near-black for colored hulls.
	const FLinearColor BootCol = bWhiteHull ? BootDefault : FLinearColor(0.02f, 0.02f, 0.03f, 1.f);

	// PMC: one material slot per hull section. Shared static mesh: unique materials only
	// (slot index ≠ section index) — classify by asset path in that case.
	const bool bSectionAligned = (HullMesh->GetNumMaterials() == HullSections.Num());
	const int32 NumMats = HullMesh->GetNumMaterials();
	for (int32 Mi = 0; Mi < NumMats; ++Mi)
	{
		UMaterialInterface* Base = HullMesh->GetMaterial(Mi);
		if (!Base) continue;

		EMooredHullPart Part = EMooredHullPart::Other;
		if (bSectionAligned && HullSections.IsValidIndex(Mi))
		{
			Part = HullSections[Mi].Part;
		}

		const FString Path = Base->GetPathName();
		const bool bIsHullPaint =
			Part == EMooredHullPart::HullGloss
			|| Part == EMooredHullPart::BootStripe
			|| Part == EMooredHullPart::HullStripe
			|| Part == EMooredHullPart::Antifoul
			|| Path.Contains(TEXT("HullPaint"))
			|| Path.Contains(TEXT("Gelcoat"));
		const bool bIsDeck =
			Part == EMooredHullPart::Deck
			|| Path.Contains(TEXT("Deck"));
		const bool bIsCabin =
			Part == EMooredHullPart::Cabin
			|| Path.Contains(TEXT("Cabin"));
		const bool bIsGlass =
			Part == EMooredHullPart::Windows
			|| Path.Contains(TEXT("Glass"))
			|| Path.Contains(TEXT("Window"));

		UMaterialInstanceDynamic* Mid = HullMesh->CreateAndSetMaterialInstanceDynamic(Mi);
		if (!Mid) continue;

		if (bIsHullPaint)
		{
			// Painted topsides — soft specular paint, not wet clearcoat varnish.
			Mid->SetVectorParameterValue(TEXT("ColorTopsides"), HullCol);
			Mid->SetVectorParameterValue(TEXT("ColorStripe"), StripeCol);
			Mid->SetVectorParameterValue(TEXT("ColorBoot"), BootCol);
			Mid->SetVectorParameterValue(TEXT("ColorAntifoul"), Antifoul);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), HullCol);
			Mid->SetVectorParameterValue(TEXT("Color"), HullCol);
			const float RoughTop = bWhiteHull ? 0.14f : 0.18f;
			Mid->SetScalarParameterValue(TEXT("RoughTopsides"), RoughTop);
			Mid->SetScalarParameterValue(TEXT("RoughStripe"), 0.20f);
			Mid->SetScalarParameterValue(TEXT("RoughBoot"), 0.28f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), RoughTop);
			Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.f);
			Mid->SetScalarParameterValue(TEXT("ClearCoatRoughness"), 1.f);
			Mid->SetScalarParameterValue(TEXT("ClearCoatBoost"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
			Mid->SetScalarParameterValue(TEXT("Specular"), 0.40f);
		}
		else if (bIsDeck)
		{
			// Clean white deck — matte non-skid, single-sided (no dual-face shimmer).
			Mid->SetVectorParameterValue(TEXT("BaseColor"), WhiteDeck);
			Mid->SetVectorParameterValue(TEXT("Color"), WhiteDeck);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.62f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Specular"), 0.4f);
			Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.0f);
		}
		else if (bIsCabin)
		{
			// Solid coachroof paint — never emissive (only glass glows when lit).
			const FLinearColor CabinWhite(0.94f, 0.95f, 0.96f, 1.f);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), CabinWhite);
			Mid->SetVectorParameterValue(TEXT("Color"), CabinWhite);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Emissive"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.28f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
		}
		else if (bIsGlass)
		{
			// Dark daytime glass; ApplyCabinGlassGlow rewrites when cabin is lit.
			const FLinearColor DarkGlass(0.08f, 0.12f, 0.16f, 1.f);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), DarkGlass);
			Mid->SetVectorParameterValue(TEXT("Color"), DarkGlass);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.05f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Specular"), 0.5f);
			Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.55f);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.008f);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.008f);
			Mid->SetScalarParameterValue(TEXT("Emissive"), 0.008f);
		}
	}

	// Warm interior glow through glass for ~half the fleet (deterministic).
	if (ShouldCabinBeLit(SlotIndex))
	{
		ApplyCabinGlassGlow(HullMesh, /*bLit*/ true, /*VisScale*/ 1.f);
	}

	if (SlotIndex < 3)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("MooredBoats: paint[%d] hull=%s stripeIdx=%d (whiteHull=%d hullAccent=%d cabinLit=%d)"),
			SlotIndex,
			bWhiteHull ? TEXT("white") : TEXT("color"),
			StripeIdx,
			bWhiteHull ? 1 : 0, HullIdx,
			ShouldCabinBeLit(SlotIndex) ? 1 : 0);
	}
}

bool UMooredBoatSubsystem::ShouldCabinBeLit(int32 SlotIndex) const
{
	return MooredBoatPrivate::Hash01(SlotIndex, 211) < FMath::Clamp(CabinLitFraction, 0.f, 1.f);
}

void UMooredBoatSubsystem::ApplyCabinGlassGlow(UMeshComponent* HullMesh, bool bLit, float VisScale) const
{
	if (!HullMesh) return;
	const float V = FMath::Clamp(VisScale, 0.15f, 1.f);
	// ONLY MI_Yacht_Glass (Windows). Coachroof / cabin / deck stay solid paint.
	// No real PointLight — it was lighting the entire cabintop (two-sided cabin MI).
	const FLinearColor DarkGlass(0.08f, 0.12f, 0.16f, 1.f);
	const FLinearColor WarmGlass(1.00f, 0.72f, 0.42f, 1.f);
	const FLinearColor SolidPaint(0.94f, 0.95f, 0.96f, 1.f);

	const float GlassEm = bLit ? FMath::Lerp(0.45f, 3.2f, V) : 0.008f;
	const FLinearColor GlassCol = bLit
		? FLinearColor::LerpUsingHSV(DarkGlass, WarmGlass, FMath::Lerp(0.5f, 0.95f, V))
		: DarkGlass;

	const bool bSectionAligned = (HullMesh->GetNumMaterials() == HullSections.Num());
	const int32 NumMats = HullMesh->GetNumMaterials();
	for (int32 Mi = 0; Mi < NumMats; ++Mi)
	{
		UMaterialInterface* Base = HullMesh->GetMaterial(Mi);
		if (!Base) continue;

		EMooredHullPart Part = EMooredHullPart::Other;
		if (bSectionAligned && HullSections.IsValidIndex(Mi))
		{
			Part = HullSections[Mi].Part;
		}
		const FString Path = Base->GetPathName();
		// Strict: only "Glass" in asset path — not Z-heuristic "Windows" alone.
		const bool bIsGlass = Path.Contains(TEXT("Glass"));
		const bool bIsSolidCoach =
			Part == EMooredHullPart::Cabin
			|| Part == EMooredHullPart::Deck
			|| Path.Contains(TEXT("Cabin"))
			|| Path.Contains(TEXT("Deck"));

		if (!bIsGlass && !bIsSolidCoach) continue;

		UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Base);
		if (!Mid)
		{
			Mid = HullMesh->CreateAndSetMaterialInstanceDynamic(Mi);
		}
		if (!Mid) continue;

		if (bIsGlass)
		{
			Mid->SetVectorParameterValue(TEXT("BaseColor"), GlassCol);
			Mid->SetVectorParameterValue(TEXT("Color"), GlassCol);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), WarmGlass * GlassEm);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), GlassEm);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), GlassEm);
			Mid->SetScalarParameterValue(TEXT("Emissive"), GlassEm);
			Mid->SetScalarParameterValue(TEXT("Roughness"), bLit ? 0.12f : 0.05f);
		}
		else
		{
			// Solid coachroof / cabin house / deck — no glow, no warm wash.
			Mid->SetVectorParameterValue(TEXT("BaseColor"), SolidPaint);
			Mid->SetVectorParameterValue(TEXT("Color"), SolidPaint);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Emissive"), 0.f);
		}
	}
}

void UMooredBoatSubsystem::AddCabinLight(
	AActor* Boat, USceneComponent* Root, UMeshComponent* HullMesh, int32 SlotIndex) const
{
	if (!Boat || !ShouldCabinBeLit(SlotIndex)) return;
	(void)Root;
	// Glass-emissive only (no PointLight). A real light under the coachroof lights the
	// entire two-sided cabin shell; player boat only glows window MI slots.
	ApplyCabinGlassGlow(HullMesh, /*bLit*/ true, /*VisScale*/ 1.f);
}

void UMooredBoatSubsystem::ApplyHullTo(UProceduralMeshComponent* Hull) const
{
	if (!Hull || HullSections.Num() == 0) return;
	Hull->ClearAllMeshSections();
	Hull->bUseComplexAsSimpleCollision = false;
	Hull->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Match HISM: no cast shadow; distance-cull like mid band (not never-cull).
	Hull->SetCastShadow(false);
	StripMooredReflectionCost(Hull);
	Hull->SetReceivesDecals(false);
	Hull->SetVisibility(true);
	Hull->SetHiddenInGame(false);
	Hull->bNeverDistanceCull = false;
	Hull->SetCullDistance(LoadRadiusCm * 1.1f);

	for (int32 Si = 0; Si < HullSections.Num(); ++Si)
	{
		const FMooredHullSection& S = HullSections[Si];
		Hull->CreateMeshSection_LinearColor(
			Si, S.Positions, S.Indices, S.Normals, S.UV0, S.Colors, S.Tangents,
			/*bCreateCollision*/ false);
		UMaterialInterface* Base = MaterialForPart(S.Part);
		if (!Base && S.bTwoSided) Base = TwoSidedBaseMat.Get();
		if (!Base) Base = SparMaterial.Get();
		if (!Base) continue;
		// Prefer the authored MI as-is (correct PBR). Only force-tint if using a generic fallback
		// whose BaseColor would otherwise be wrong (buoy textures / BasicShape white).
		const bool bYachtAuthored = Base->GetPathName().Contains(TEXT("/Materials/Yacht/"));
		if (bYachtAuthored)
		{
			Hull->SetMaterial(Si, Base);
		}
		else
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, Hull);
			if (Mid)
			{
				Mid->SetVectorParameterValue(TEXT("Color"), S.Tint);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), S.Tint);
				Mid->SetVectorParameterValue(TEXT("Base Color"), S.Tint);
				Mid->SetScalarParameterValue(TEXT("Roughness"), S.Roughness);
				Mid->SetScalarParameterValue(TEXT("Metallic"), S.Metallic);
				Mid->SetScalarParameterValue(TEXT("Specular"), S.Specular);
				Mid->SetScalarParameterValue(TEXT("Smoothness"), 1.f - S.Roughness);
				Hull->SetMaterial(Si, Mid);
			}
			else
			{
				Hull->SetMaterial(Si, Base);
			}
		}
	}
	Hull->MarkRenderStateDirty();
	Hull->UpdateBounds();
}

void UMooredBoatSubsystem::DisableAllCastShadows(UPrimitiveComponent* Prim)
{
	if (!Prim) return;
	Prim->SetCastShadow(false);
	Prim->bCastContactShadow = false;
	Prim->bCastDynamicShadow = false;
	Prim->bCastStaticShadow = false;
	Prim->bCastVolumetricTranslucentShadow = false;
	Prim->bCastInsetShadow = false;
	Prim->bSelfShadowOnly = false;
	StripMooredReflectionCost(Prim);
}

void UMooredBoatSubsystem::StripMooredReflectionCost(UPrimitiveComponent* Prim)
{
	if (!Prim) return;
	// Keep main-pass + auto LODs (8249a15 forced LOD1 crushed mid-harbor hulls into dots).
	// Only strip reflection / Lumen / DF contribution that feeds SLW::LumenReflections.
	Prim->bVisibleInReflectionCaptures = false;
	Prim->SetVisibleInRayTracing(false);
	Prim->SetAffectDistanceFieldLighting(false);
	Prim->bAffectDynamicIndirectLighting = false;
	Prim->bAffectIndirectLightingWhileHidden = false;
	Prim->MarkRenderStateDirty();
}


void UMooredBoatSubsystem::ApplyKeelTo(UProceduralMeshComponent* KeelMesh) const
{
	if (!KeelMesh || KeelSections.Num() == 0) return;
	KeelMesh->ClearAllMeshSections();
	KeelMesh->bUseComplexAsSimpleCollision = false;
	KeelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DisableAllCastShadows(KeelMesh);
	KeelMesh->SetReceivesDecals(false);
	// Still receive scene light so fins aren't black holes under the hull.
	KeelMesh->SetVisibility(true);
	KeelMesh->SetHiddenInGame(false);
	KeelMesh->bNeverDistanceCull = false;
	KeelMesh->SetCullDistance(LoadRadiusCm * 1.1f);

	for (int32 Si = 0; Si < KeelSections.Num(); ++Si)
	{
		const FMooredHullSection& S = KeelSections[Si];
		KeelMesh->CreateMeshSection_LinearColor(
			Si, S.Positions, S.Indices, S.Normals, S.UV0, S.Colors, S.Tangents,
			/*bCreateCollision*/ false);
		UMaterialInterface* Base = MaterialForPart(EMooredHullPart::Keel);
		if (!Base) Base = SparMaterial.Get();
		if (!Base) continue;
		const bool bYachtAuthored = Base->GetPathName().Contains(TEXT("/Materials/Yacht/"));
		if (bYachtAuthored)
		{
			// Lighten keel MI so it reads as gelcoat/lead, not a cast shadow.
			if (UMaterialInstanceDynamic* Mid = KeelMesh->CreateAndSetMaterialInstanceDynamic(Si))
			{
				const FLinearColor KeelCol(0.28f, 0.29f, 0.30f, 1.f);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), KeelCol);
				Mid->SetVectorParameterValue(TEXT("Color"), KeelCol);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.42f);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 0.05f);
				Mid->SetScalarParameterValue(TEXT("Specular"), 0.4f);
			}
			else
			{
				KeelMesh->SetMaterial(Si, Base);
			}
		}
		else
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, KeelMesh);
			if (Mid)
			{
				Mid->SetVectorParameterValue(TEXT("Color"), S.Tint);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), S.Tint);
				Mid->SetScalarParameterValue(TEXT("Roughness"), S.Roughness);
				Mid->SetScalarParameterValue(TEXT("Metallic"), S.Metallic);
				KeelMesh->SetMaterial(Si, Mid);
			}
			else
			{
				KeelMesh->SetMaterial(Si, Base);
			}
		}
	}
	// Re-assert after section create (some paths re-enable cast).
	DisableAllCastShadows(KeelMesh);
	KeelMesh->MarkRenderStateDirty();
	KeelMesh->UpdateBounds();
}

void UMooredBoatSubsystem::ClearAll()
{
	ClearMidHism();

	for (auto& Pair : Resident)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Resident.Reset();
	SwayState.Reset();

	// Sweep any orphaned moored actors (deferred Destroy + double ForceStream
	// can leave a second set alive for a frame — or forever if maps desync).
	if (UWorld* World = GetWorld())
	{
		TArray<AActor*> Leftover;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(FName(TEXT("MooredBoat"))))
			{
				Leftover.Add(*It);
			}
		}
		for (AActor* A : Leftover)
		{
			if (IsValid(A))
			{
				A->Destroy();
			}
		}
	}
}

void UMooredBoatSubsystem::CacheMooringRingHeight()
{
	if (bMooringRingCached) return;
	bMooringRingCached = true;
	// Fallback path when EncAid instances aren't streamed yet.
	float BestTop = 0.f;
	for (int32 MeshId : {1, 2})
	{
		const FString Path = FString::Printf(
			TEXT("/Game/Buoys/StaticMesh/SM_Buoy_%d.SM_Buoy_%d"), MeshId, MeshId);
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path))
		{
			const FBox Box = Mesh->GetBoundingBox();
			BestTop = FMath::Max(BestTop, Box.Max.Z);
		}
	}
	MooringRingLocalZCm = (BestTop > 1.f) ? BestTop : 48.f;

	float Scale = MooringBuoyScale;
	float BuoyZ = MooringBuoyZOffsetCm;
	if (UWorld* World = GetWorld())
	{
		if (const UEncAidSubsystem* Enc = World->GetSubsystem<UEncAidSubsystem>())
		{
			Scale = Enc->MooringScale;
			BuoyZ = Enc->GetMooringActorZ();
		}
	}
	UE_LOG(LogSailSim, Log,
		TEXT("MooredBoats: mooring ring fallback localMaxZ=%.1fcm × scale=%.2f → ringZ≈%.0f (buoyZ=%.0f)"),
		MooringRingLocalZCm, Scale, BuoyZ + MooringRingLocalZCm * Scale, BuoyZ);
}

FVector UMooredBoatSubsystem::GetMooringRingWorld(const FMooredBoatSlot& Slot) const
{
	const FVector Near(Slot.MooringWorldCm.X, Slot.MooringWorldCm.Y, 0.f);

	// Prefer live EncAid instance top (accounts for real scale + mesh AABB).
	if (UWorld* World = GetWorld())
	{
		if (const UEncAidSubsystem* Enc = World->GetSubsystem<UEncAidSubsystem>())
		{
			FVector Ring;
			if (Enc->FindMooringRingWorld(Near, Ring, /*MaxDistCm*/ 3000.f))
			{
				return Ring;
			}
			// EncAid present but instance not streamed — use its scale/Z + mesh top.
			const_cast<UMooredBoatSubsystem*>(this)->CacheMooringRingHeight();
			const float BuoyZ = Enc->GetMooringActorZ();
			const float RingZ = BuoyZ + MooringRingLocalZCm * Enc->MooringScale + 6.f;
			return FVector(Slot.MooringWorldCm.X, Slot.MooringWorldCm.Y, RingZ);
		}
	}

	const_cast<UMooredBoatSubsystem*>(this)->CacheMooringRingHeight();
	const float RingZ = MooringBuoyZOffsetCm + MooringRingLocalZCm * MooringBuoyScale + 6.f;
	return FVector(Slot.MooringWorldCm.X, Slot.MooringWorldCm.Y, RingZ);
}

void UMooredBoatSubsystem::InitSwayState(int32 SlotIndex, TArrayView<UStaticMeshComponent* const> PennantSegsIn)
{
	FMooredBoatSway S;
	S.SlotIndex = SlotIndex;
	S.NoisePhaseA = MooredBoatPrivate::Hash01(SlotIndex, 41) * 2.f * PI;
	S.NoisePhaseB = MooredBoatPrivate::Hash01(SlotIndex, 42) * 2.f * PI;
	S.NoisePhaseC = MooredBoatPrivate::Hash01(SlotIndex, 43) * 2.f * PI;
	const float InitH = (Slots.IsValidIndex(SlotIndex) ? Slots[SlotIndex].HeadingDeg : WindFromDeg);
	S.HeadingDeg = InitH;
	S.SmoothTwdFromDeg = InitH;
	S.SmoothTwsKn = 12.f;
	S.TimeSec = MooredBoatPrivate::Hash01(SlotIndex, 47) * 40.f; // de-sync field
	S.PennantSegs.Reset();
	for (UStaticMeshComponent* C : PennantSegsIn)
	{
		if (C) S.PennantSegs.Add(C);
	}
	S.bInitialized = true;
	SwayState.Add(SlotIndex, S);
}

void UMooredBoatSubsystem::UpdatePennantRope(
	const FTransform& BoatXf,
	const FMooredBoatSlot& Slot,
	FMooredBoatSway& State,
	float WindFactor,
	float Gust) const
{
	const int32 NSeg = State.PennantSegs.Num();
	if (NSeg < 1) return;

	// Bow stem cleat-ish → top ring on the mooring ball (not buoy centre).
	const FVector BowWorld = BoatXf.TransformPosition(FVector(BowOffsetCm, 0.f, 80.f));
	const FVector RingWorld = GetMooringRingWorld(Slot);

	const FVector Chord = RingWorld - BowWorld;
	const float ChordLen = Chord.Size();
	if (ChordLen < 5.f) return;
	const FVector ChordN = Chord / ChordLen;
	FVector Side = FVector::CrossProduct(FVector::UpVector, ChordN);
	if (!Side.Normalize()) Side = FVector::RightVector;

	// Short mooring pennants stay fairly taut but never a steel bar: parabolic sag
	// (0 at ends) + mild wind belly. All analytic — no cloth / particles.
	const float Wf01 = FMath::Clamp(WindFactor / 1.5f, 0.f, 1.f);
	const float SagCm = FMath::Clamp(
		ChordLen * FMath::Lerp(0.045f, 0.028f, Wf01) + 4.f,
		5.f, 40.f);
	const float BellyCm = (7.f + 10.f * WindFactor) * Gust * 0.40f
		* FMath::Sin(State.TimeSec * 0.55f + State.NoisePhaseB);

	const int32 NPts = NSeg + 1;
	TArray<FVector, TInlineAllocator<5>> Pts;
	Pts.SetNum(NPts);
	for (int32 I = 0; I < NPts; ++I)
	{
		const float T = float(I) / float(NPts - 1);
		FVector P = FMath::Lerp(BowWorld, RingWorld, T);
		// 4t(1-t) peaks at mid-span
		const float Arch = 4.f * T * (1.f - T);
		P.Z -= SagCm * Arch;
		P += Side * (BellyCm * Arch);
		Pts[I] = P;
	}
	Pts[0] = BowWorld;
	Pts[NPts - 1] = RingWorld;

	// Thin rope (BasicShape unit cylinder is 100 cm tall at scale 1).
	constexpr float RopeRadius = 0.014f;
	for (int32 S = 0; S < NSeg; ++S)
	{
		UStaticMeshComponent* Comp = State.PennantSegs[S].Get();
		if (!Comp) continue;
		const FVector A = BoatXf.InverseTransformPosition(Pts[S]);
		const FVector B = BoatXf.InverseTransformPosition(Pts[S + 1]);
		UpdateCylinderTransform(Comp, A, B, RopeRadius);
	}
}

void UMooredBoatSubsystem::ApplySwayToBoat(int32 SlotIndex, AActor* Boat, FMooredBoatSway& State, float DeltaTime)
{
	if (!Boat || !Slots.IsValidIndex(SlotIndex)) return;
	const FMooredBoatSlot& Slot = Slots[SlotIndex];
	// Fixed sub-step so large frame spikes don't look like stop/start teleports.
	const float Dt = FMath::Clamp(DeltaTime, 1.f / 120.f, 1.f / 20.f);

	// ---- Local wind (sample) → heavy low-pass (puffs must not jerk the hull) ----
	float TwsRaw = 12.f;
	float TwdRaw = WindFromDeg;
	if (UWorld* World = GetWorld())
	{
		if (UWindFieldSubsystem* Wind = World->GetSubsystem<UWindFieldSubsystem>())
		{
			const FWindSample Sample = Wind->SampleWindAt(FVector(
				Slot.MooringWorldCm.X, Slot.MooringWorldCm.Y, WaterlineOffsetCm));
			TwsRaw = Sample.SpeedKn;
			TwdRaw = Sample.DirFromDeg;
		}
	}
	const float WindAlpha = 1.f - FMath::Exp(-WindSmoothRate * Dt);
	State.SmoothTwsKn = FMath::Lerp(State.SmoothTwsKn, TwsRaw, WindAlpha);
	{
		const float DWind = FMath::FindDeltaAngleDegrees(State.SmoothTwdFromDeg, TwdRaw);
		// Direction tracks even slower than speed (vane + inertia of the fleet).
		State.SmoothTwdFromDeg = FMath::UnwindDegrees(
			State.SmoothTwdFromDeg + DWind * (1.f - FMath::Exp(-WindSmoothRate * 0.55f * Dt)));
	}

	const float Wf = FMath::Clamp(State.SmoothTwsKn / 12.f, 0.2f, 1.5f);
	State.TimeSec += Dt;

	// Slow filtered "gust noise" (two octaves) — continuous, never resets.
	// Periods ~50–120 s so sheer builds and eases like a real field.
	const float N1 = FMath::Sin(State.TimeSec * (0.055f + 0.02f * Wf) + State.NoisePhaseA);
	const float N2 = FMath::Sin(State.TimeSec * (0.11f + 0.03f * Wf) + State.NoisePhaseB);
	const float N3 = FMath::Sin(State.TimeSec * (0.19f + 0.04f * Wf) + State.NoisePhaseC);
	const float Gust = 0.65f * N1 + 0.28f * N2 + 0.12f * N3; // ~[-1,1]

	// Equilibrium: bow INTO wind (hang downwind of the ball).
	// Small mean bias per boat so the field isn't perfectly aligned.
	const float MeanBiasDeg = 2.5f * Wf * FMath::Sin(State.NoisePhaseA);
	const float TargetHeading = FMath::UnwindDegrees(State.SmoothTwdFromDeg + MeanBiasDeg);

	// Second-order yaw: spring to target + damping + soft gust torque.
	// Accel (deg/s²) keeps motion continuous through zero-crossings (no sin stop/start).
	const float Err = FMath::FindDeltaAngleDegrees(State.HeadingDeg, TargetHeading);
	const float YawAmp = SwayYawDegAt12kn * Wf;
	// Allow sheer past pure head-to-wind; gust drives within ±YawAmp.
	const float GustTorque = (YawAmp * 0.55f) * Gust * (0.4f + 0.6f * Wf);
	const float YawAcc =
		YawSpring * (1.1f + 0.4f * Wf) * Err
		- YawDamping * (0.9f + 0.35f * Wf) * State.YawRateDegS
		+ GustTorque * 0.35f;
	State.YawRateDegS = FMath::Clamp(
		State.YawRateDegS + YawAcc * Dt, -MaxYawRateDegS, MaxYawRateDegS);
	// Soft limit: if far past max sheer, pull rate back (rode / hull damping).
	const float Sheer = FMath::FindDeltaAngleDegrees(TargetHeading, State.HeadingDeg);
	if (FMath::Abs(Sheer) > YawAmp * 1.35f)
	{
		State.YawRateDegS *= (1.f - FMath::Clamp(2.5f * Dt, 0.f, 0.5f));
	}
	State.HeadingDeg = FMath::UnwindDegrees(State.HeadingDeg + State.YawRateDegS * Dt);

	// Rode: spring–damper stretch (sailboat "sails" back on the rode, then eases).
	const float SurgeTarget = Slot.LineLenCm * SwaySurgeFrac * Wf
		* (0.35f + 0.65f * FMath::Max(0.f, Gust)); // longer when "gust on"
	const float SurgeErr = SurgeTarget - State.SurgeCm;
	const float SurgeAcc = 1.8f * SurgeErr - 2.2f * State.SurgeRateCmS;
	State.SurgeRateCmS = FMath::Clamp(State.SurgeRateCmS + SurgeAcc * Dt, -25.f, 25.f);
	State.SurgeCm = FMath::Clamp(State.SurgeCm + State.SurgeRateCmS * Dt,
		-Slot.LineLenCm * SwaySurgeFrac * 1.2f,
		Slot.LineLenCm * SwaySurgeFrac * 1.4f);
	const float LineCm = Slot.LineLenCm + State.SurgeCm;

	// Attitude: heavily damped springs to small wind-driven targets (not free sin).
	const float RollTarget = SwayRollDegMax * Wf * 0.55f * Gust;
	const float PitchTarget = (0.35f + 0.9f * Wf) * (0.4f * N2 + 0.2f * Gust);
	const float HeaveTarget = (1.0f + 2.2f * Wf) * (0.5f + 0.5f * N3);

	auto Spring1D = [Dt](float& X, float& V, float Target, float K, float C, float VMax)
	{
		const float Acc = K * (Target - X) - C * V;
		V = FMath::Clamp(V + Acc * Dt, -VMax, VMax);
		X += V * Dt;
	};
	Spring1D(State.RollDeg, State.RollRateDegS, RollTarget, 1.6f, 2.4f, 8.f);
	Spring1D(State.PitchDeg, State.PitchRateDegS, PitchTarget, 1.8f, 2.6f, 6.f);
	Spring1D(State.HeaveCm, State.HeaveRateCmS, HeaveTarget, 2.0f, 2.8f, 12.f);

	// Geometry: bow always faces the ball along the rode (pendulum constraint).
	const float YawRad = FMath::DegreesToRadians(State.HeadingDeg);
	const FVector Forward(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);
	const FVector OriginXY = Slot.MooringWorldCm - Forward * (BowOffsetCm + LineCm);
	const FVector Origin(OriginXY.X, OriginXY.Y, WaterlineOffsetCm + State.HeaveCm);
	const FRotator Rot(State.PitchDeg, State.HeadingDeg, State.RollDeg);
	const FTransform BoatXf(Rot, Origin);

	// Single transform write — SetActorTransform already updates the root and children.
	Boat->SetActorTransform(BoatXf, false, nullptr, ETeleportType::None);

	// Multi-segment catenary pennant → top ring (transform-only per segment).
	UpdatePennantRope(BoatXf, Slot, State, Wf, Gust);
}

void UMooredBoatSubsystem::UpdateAllSway(float DeltaTime)
{
	if (!bEnableSway) return;
	// Cap dt but allow enough range for smooth integration (not 1/20 hard stop feel).
	const float Dt = FMath::Clamp(DeltaTime, 0.f, 1.f / 15.f);
	if (Dt <= 0.f) return;
	for (auto& Pair : Resident)
	{
		if (!IsValid(Pair.Value)) continue;
		FMooredBoatSway* St = SwayState.Find(Pair.Key);
		if (!St)
		{
			TArray<UStaticMeshComponent*, TInlineAllocator<4>> Segs;
			TArray<UStaticMeshComponent*> Comps;
			Pair.Value->GetComponents<UStaticMeshComponent>(Comps);
			// Prefer Pennant_0, Pennant_1, … order
			Comps.Sort([](const UStaticMeshComponent& A, const UStaticMeshComponent& B)
			{
				return A.GetName() < B.GetName();
			});
			// Note: TArray<UObject*> Sort uses element refs as the pointed-to objects in UE.
			for (UStaticMeshComponent* C : Comps)
			{
				if (C && C->GetName().StartsWith(TEXT("Pennant")))
				{
					Segs.Add(C);
				}
			}
			InitSwayState(Pair.Key, Segs);
			St = SwayState.Find(Pair.Key);
		}
		if (St)
		{
			ApplySwayToBoat(Pair.Key, Pair.Value, *St, Dt);
		}
	}
}

void UMooredBoatSubsystem::UpdateCylinderTransform(
	UStaticMeshComponent* Comp, const FVector& LocalA, const FVector& LocalB,
	float RadiusScale) const
{
	if (!Comp) return;
	const FVector Mid = (LocalA + LocalB) * 0.5f;
	const FVector Dir = LocalB - LocalA;
	const float Len = Dir.Size();
	if (Len < 1.f) return;
	Comp->SetRelativeLocation(Mid);
	Comp->SetRelativeScale3D(FVector(RadiusScale, RadiusScale, Len / 100.f));
	Comp->SetRelativeRotation(FRotationMatrix::MakeFromZ(Dir.GetSafeNormal()).Rotator());
}

void UMooredBoatSubsystem::PlaceCylinder(
	UStaticMeshComponent* Comp, const FVector& LocalA, const FVector& LocalB,
	float RadiusScale, UMaterialInterface* Mat) const
{
	if (!Comp || !CylinderMesh) return;
	const FVector Mid = (LocalA + LocalB) * 0.5f;
	const FVector Dir = LocalB - LocalA;
	const float Len = Dir.Size();
	if (Len < 1.f) return;
	// One-time setup (spawn / standing rig). Hot sway path uses UpdateCylinderTransform.
	if (Comp->GetStaticMesh() != CylinderMesh)
	{
		Comp->SetStaticMesh(CylinderMesh);
	}
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Non-Nanite BasicShape cylinders: no shadows (hull carries the field silhouette).
	Comp->SetCastShadow(false);
	Comp->SetVisibility(true);
	Comp->SetHiddenInGame(false);
	Comp->bNeverDistanceCull = false;
	Comp->SetCullDistance(LoadRadiusCm * 1.1f);
	StripMooredReflectionCost(Comp);
	Comp->SetRelativeLocation(Mid);
	Comp->SetRelativeScale3D(FVector(RadiusScale, RadiusScale, Len / 100.f));
	Comp->SetRelativeRotation(FRotationMatrix::MakeFromZ(Dir.GetSafeNormal()).Rotator());
	if (Mat) Comp->SetMaterial(0, Mat);
}

void UMooredBoatSubsystem::AddStandingRigging(AActor* Boat, USceneComponent* Root) const
{
	// Web hBuildSpreadersAndStanding — J/105 single swept spreaders + standing wire.
	if (!Boat || !Root || !TemplateSpars.bMastValid || !CylinderMesh) return;

	const FVector MastBase = TemplateSpars.MastBase;
	const FVector MastTop = TemplateSpars.MastTop;
	const float MastH = FMath::Max(100.f, MastTop.Z - MastBase.Z);
	const float Mx = MastBase.X;
	const float Mz = MastBase.Z;
	const float HalfB = FMath::Max(80.f, BowOffsetCm * 0.21f); // ~ half-beam from LOA/2 * 0.42

	const float SprZ = Mz + MastH * 0.52f;
	const float SprHalf = FMath::Max(73.f, HalfB * 0.95f);
	const float SprAft = 0.55f * 30.48f;
	const float SprUp = 0.22f * 30.48f;
	const FVector RootPt(Mx, 0.f, SprZ);
	const FVector TipP(Mx - SprAft, -SprHalf, SprZ + SprUp);
	const FVector TipS(Mx - SprAft, SprHalf, SprZ + SprUp);
	const float CpZ = Mz + 0.12f * 30.48f;
	const float CpX = Mx - 0.35f * 30.48f;
	const FVector CpP(CpX, -HalfB * 0.88f, CpZ);
	const FVector CpS(CpX, HalfB * 0.88f, CpZ);
	const FVector Truck(Mx, 0.f, MastTop.Z - 0.15f * 30.48f);
	const float Jcm = 13.5f * 30.48f;
	const FVector FsTack(Mx + Jcm, 0.f, Mz + 0.15f * 30.48f);
	const FVector FsHead(Mx, 0.f, FMath::Min(MastTop.Z - 2.f, Mz + MastH * 0.96f));
	const float LoaCm = BowOffsetCm * 2.f;
	const FVector BsDeck(Mx - LoaCm * 0.55f, 0.f, Mz + 0.2f * 30.48f);

	UMaterialInterface* SparMat = YachtSparMat.Get() ? YachtSparMat.Get() : SparMaterial.Get();
	UMaterialInterface* WireMat = YachtRopeMat.Get()
		? YachtRopeMat.Get()
		: (LineMaterial.Get() ? LineMaterial.Get() : SparMaterial.Get());

	auto Make = [&](const TCHAR* Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Boat, Name, RF_Transient);
		C->SetupAttachment(Root);
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->bNeverDistanceCull = false;
		C->SetCullDistance(LoadRadiusCm * 1.1f);
		StripMooredReflectionCost(C);
		C->RegisterComponent();
		Boat->AddInstanceComponent(C);
		return C;
	};

	auto Place = [&](const TCHAR* Name, const FVector& A, const FVector& B, float Rad, UMaterialInterface* Mat)
	{
		UStaticMeshComponent* C = Make(Name);
		PlaceCylinder(C, A, B, Rad, Mat);
		if (Mat == WireMat || Mat == LineMaterial.Get())
		{
			if (UMaterialInstanceDynamic* Mid = C->CreateAndSetMaterialInstanceDynamic(0))
			{
				const FLinearColor Wire(0.38f, 0.42f, 0.46f, 1.f);
				Mid->SetVectorParameterValue(TEXT("Color"), Wire);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Wire);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 0.9f);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.2f);
			}
		}
	};

	Place(TEXT("SpreaderP"), RootPt, TipP, 0.07f, SparMat);
	Place(TEXT("SpreaderS"), RootPt, TipS, 0.07f, SparMat);
	Place(TEXT("SprCollar"), FVector(Mx, 0.f, SprZ - 3.5f), FVector(Mx, 0.f, SprZ + 3.5f), 0.14f, SparMat);
	Place(TEXT("LowerP"), CpP, TipP, 0.022f, WireMat);
	Place(TEXT("LowerS"), CpS, TipS, 0.022f, WireMat);
	Place(TEXT("UpperP"), TipP, Truck, 0.020f, WireMat);
	Place(TEXT("UpperS"), TipS, Truck, 0.020f, WireMat);
	Place(TEXT("CapP"), CpP, Truck, 0.019f, WireMat);
	Place(TEXT("CapS"), CpS, Truck, 0.019f, WireMat);
	Place(TEXT("Forestay"), FsTack, FsHead, 0.024f, WireMat);
	Place(TEXT("Backstay"), MastTop, BsDeck, 0.021f, WireMat);
}

float UMooredBoatSubsystem::ComputeNightVisScale() const
{
	// Preset-only steps (stable). Do NOT poll sun intensity every tick — that
	// jitters with exposure/clouds and made the mooring field flicker.
	float Night01 = 0.35f;
	UWorld* World = GetWorld();
	if (!World) return Night01;

	if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const uint8 Preset = Ocean->GetActiveEnvPreset();
		// ESailEnvPreset: FairDay=0, Golden=1, Dusk=2, Night=3, Overcast=4, Storm=5, Fog=6
		switch (Preset)
		{
		case 3: Night01 = 1.00f; break; // Night — full COLREGS punch
		case 2: Night01 = 0.90f; break; // Dusk
		case 1: Night01 = 0.60f; break; // Golden
		case 5: Night01 = 0.70f; break; // Storm
		case 6: Night01 = 0.75f; break; // Fog
		case 4: Night01 = 0.45f; break; // Overcast
		default: Night01 = 0.30f; break; // Fair day — still faintly on
		}
	}
	return Night01;
}

void UMooredBoatSubsystem::AddAnchorLight(AActor* Boat, USceneComponent* Root) const
{
	if (!bShowAnchorLights || !Boat || !Root) return;

	// COLREGS all-round white (Rule 21/30): unbroken 360° white, best seen high.
	// Recreational masthead lanterns are typically a frosted globe ~3–5" (8–12 cm)
	// on a short base just clear of the truck — not a tall staff, not a pinhead.
	//
	// Engine BasicShape Sphere: diameter = 100 cm * scale.
	const float StaffAboveTruckCm = 12.f;   // just clear of mast tip / backstay eye
	const float BaseScale = 0.06f;          // ~6 cm dark metal base
	const float GlobeScale = 0.11f;         // ~11 cm frosted all-round globe (readable at range)
	const float HaloScale = 0.16f;          // soft outer glow shell (all-azimuth cue)
	const float GlobeAboveBaseCm = 5.5f;    // base sits under globe, not overlapping

	FVector PosBase = FVector(0.f, 0.f, 1400.f);
	FVector MastTopLocal = PosBase;
	if (TemplateSpars.bMastValid)
	{
		const FVector MastBase = TemplateSpars.MastBase;
		const FVector MastTop = TemplateSpars.MastTop;
		MastTopLocal = FVector(MastBase.X, 0.f, MastTop.Z);
		PosBase = FVector(MastBase.X, 0.f, MastTop.Z + StaffAboveTruckCm);
	}
	const FVector PosGlobe = PosBase + FVector(0.f, 0.f, GlobeAboveBaseCm);
	// Light at globe center — all-round source, not buried in metal.
	const FVector PosLight = PosGlobe;

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cylinder = CylinderMesh.Get()
		? CylinderMesh.Get()
		: LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR_TwoSided.M_Yacht_PBR_TwoSided"));
	if (!BaseMat)
	{
		BaseMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}

	auto PrepMesh = [&](UStaticMeshComponent* C)
	{
		if (!C) return;
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->bCastContactShadow = false;
		C->bCastDynamicShadow = false;
		C->bNeverDistanceCull = false;
		C->SetCullDistance(LoadRadiusCm * 1.1f);
		StripMooredReflectionCost(C);
		C->SetBoundsScale(8.f); // fat bounds — tiny masthead lamps were TAA/cull flicker
		C->SetReceivesDecals(false);
		C->SetVisibility(true);
		C->SetHiddenInGame(false);
	};

	auto MakeSphere = [&](const TCHAR* Name, const FVector& Loc, float Scale) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Boat, Name, RF_Transient);
		C->SetupAttachment(Root);
		PrepMesh(C);
		if (Sphere) C->SetStaticMesh(Sphere);
		if (BaseMat) C->SetMaterial(0, BaseMat);
		C->SetRelativeLocation(Loc);
		C->SetRelativeScale3D(FVector(Scale));
		C->RegisterComponent();
		Boat->AddInstanceComponent(C);
		return C;
	};

	// Short stem truck → base (reads as masthead fitting, keeps globe clear of spar).
	if (Cylinder && TemplateSpars.bMastValid)
	{
		UStaticMeshComponent* Stem = NewObject<UStaticMeshComponent>(Boat, TEXT("AnchorStem"), RF_Transient);
		Stem->SetupAttachment(Root);
		PrepMesh(Stem);
		Stem->SetStaticMesh(Cylinder);
		if (BaseMat) Stem->SetMaterial(0, BaseMat);
		const FVector StemA = MastTopLocal;
		const FVector StemB = PosBase;
		const FVector StemMid = (StemA + StemB) * 0.5f;
		const float Len = FMath::Max(1.f, FVector::Dist(StemA, StemB));
		Stem->SetRelativeLocation(StemMid);
		Stem->SetRelativeScale3D(FVector(0.028f, 0.028f, Len / 100.f));
		Stem->SetRelativeRotation(FRotationMatrix::MakeFromZ((StemB - StemA).GetSafeNormal()).Rotator());
		Stem->RegisterComponent();
		Boat->AddInstanceComponent(Stem);
		if (UMaterialInstanceDynamic* StemMidMat = Stem->CreateAndSetMaterialInstanceDynamic(0))
		{
			const FLinearColor Al(0.55f, 0.56f, 0.58f);
			StemMidMat->SetVectorParameterValue(TEXT("Color"), Al);
			StemMidMat->SetVectorParameterValue(TEXT("BaseColor"), Al);
			StemMidMat->SetScalarParameterValue(TEXT("Metallic"), 0.85f);
			StemMidMat->SetScalarParameterValue(TEXT("Roughness"), 0.35f);
			StemMidMat->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
			StemMidMat->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
		}
	}

	UStaticMeshComponent* Housing = MakeSphere(TEXT("AnchorHousing"), PosBase, BaseScale);
	UStaticMeshComponent* Globe = MakeSphere(TEXT("AnchorLens"), PosGlobe, GlobeScale);
	// Outer halo: larger, dimmer shell so the lamp still reads when partially behind spars.
	UStaticMeshComponent* Halo = MakeSphere(TEXT("AnchorHalo"), PosGlobe, HaloScale);

	const FLinearColor HousingCol(0.08f, 0.08f, 0.09f);
	const FLinearColor AnchorWhite(0.98f, 0.99f, 1.00f);
	const float EmNight = 22.f; // seed full-night; UpdateAllAnchorLights re-scales

	if (Housing)
	{
		if (UMaterialInstanceDynamic* Mid = Housing->CreateAndSetMaterialInstanceDynamic(0))
		{
			Mid->SetVectorParameterValue(TEXT("Color"), HousingCol);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), HousingCol);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.8f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.35f);
		}
	}
	auto SetEmissiveGlobe = [&](UStaticMeshComponent* Mesh, float Em, float BaseMul)
	{
		if (!Mesh) return;
		if (UMaterialInstanceDynamic* Mid = Mesh->CreateAndSetMaterialInstanceDynamic(0))
		{
			Mid->SetVectorParameterValue(TEXT("Color"), AnchorWhite);
			Mid->SetVectorParameterValue(TEXT("BaseColor"), AnchorWhite * BaseMul);
			Mid->SetVectorParameterValue(TEXT("EmissiveColor"), AnchorWhite * Em);
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Em);
			Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Em);
			Mid->SetScalarParameterValue(TEXT("Emissive"), Em);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 0.35f); // frosted glass, not chrome
			Mid->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
		}
	};
	// Bright core + softer outer shell (all-azimuth, less TAA sparkle than a pin).
	SetEmissiveGlobe(Globe, EmNight, 0.55f);
	SetEmissiveGlobe(Halo, EmNight * 0.35f, 0.25f);

	// Optional real point light — OFF by default (see bAnchorPointLights).
	// Emissive globe/halo above already reads as a lantern without deferred light cost.
	if (bAnchorPointLights)
	{
		UPointLightComponent* Pt = NewObject<UPointLightComponent>(Boat, TEXT("PointAnchor"), RF_Transient);
		Pt->SetupAttachment(Root);
		Pt->SetMobility(EComponentMobility::Movable);
		Pt->SetRelativeLocation(PosLight);
		Pt->SetLightColor(AnchorWhite);
		Pt->SetIntensityUnits(ELightUnits::Candelas);
		Pt->SetIntensity(AnchorLightCandelas);
		Pt->SetAttenuationRadius(AnchorLightAttenuationCm);
		Pt->SetSourceRadius(12.f);
		Pt->SetSoftSourceRadius(28.f);
		Pt->SetSpecularScale(0.05f);
		Pt->SetVolumetricScatteringIntensity(0.02f);
		Pt->SetIndirectLightingIntensity(0.15f);
		Pt->SetCastShadows(false);
		Pt->SetUseInverseSquaredFalloff(true);
		Pt->SetVisibility(true);
		Pt->SetHiddenInGame(false);
		Pt->RegisterComponent();
		Boat->AddInstanceComponent(Pt);
	}
}

void UMooredBoatSubsystem::UpdateAllAnchorLights()
{
	if (Resident.Num() == 0) return;

	const float VisScale = ComputeNightVisScale();
	// Skip no-op updates — re-writing intensity/emissive every second caused flicker.
	if (LastAnchorVisScale >= 0.f && FMath::Abs(VisScale - LastAnchorVisScale) < 0.02f)
	{
		return;
	}
	LastAnchorVisScale = VisScale;

	// Strong emissive on the globe is what reads at distance (not just the point light).
	const float EmCore = FMath::Lerp(3.5f, 24.f, VisScale);
	const float EmHalo = EmCore * 0.35f;
	const float Cd = AnchorLightCandelas * VisScale;
	const FLinearColor AnchorWhite(0.98f, 0.99f, 1.00f);

	for (auto& Pair : Resident)
	{
		AActor* Boat = Pair.Value;
		if (!IsValid(Boat)) continue;
		const int32 SlotIndex = Pair.Key;

		TInlineComponentArray<UPointLightComponent*> Points(Boat);
		for (UPointLightComponent* Pt : Points)
		{
			if (!Pt) continue;
			const FString Name = Pt->GetName();
			if (bShowAnchorLights && Name.Contains(TEXT("PointAnchor")))
			{
				Pt->SetIntensity(Cd);
				Pt->SetAttenuationRadius(AnchorLightAttenuationCm);
				Pt->SetSourceRadius(12.f);
				Pt->SetSoftSourceRadius(28.f);
				Pt->SetVisibility(true);
				Pt->SetHiddenInGame(false);
			}
			else if (Name.Contains(TEXT("PointCabin")))
			{
				// Legacy: cabin point lights light the whole coachroof — kill them.
				Pt->SetIntensity(0.f);
				Pt->SetVisibility(false);
				Pt->SetHiddenInGame(true);
			}
		}

		if (bShowAnchorLights)
		{
			TInlineComponentArray<UStaticMeshComponent*> Meshes(Boat);
			for (UStaticMeshComponent* Mesh : Meshes)
			{
				if (!Mesh) continue;
				const FString Name = Mesh->GetName();
				const bool bCore = Name.Contains(TEXT("AnchorLens"));
				const bool bHalo = Name.Contains(TEXT("AnchorHalo"));
				if (!bCore && !bHalo) continue;

				const float Em = bCore ? EmCore : EmHalo;
				const float BaseMul = bCore ? 0.55f : 0.25f;
				UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
				if (!Mid)
				{
					Mid = Mesh->CreateAndSetMaterialInstanceDynamic(0);
				}
				if (!Mid) continue;
				Mid->SetVectorParameterValue(TEXT("Color"), AnchorWhite);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), AnchorWhite * BaseMul);
				Mid->SetVectorParameterValue(TEXT("EmissiveColor"), AnchorWhite * Em);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Em);
				Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Em);
				Mid->SetScalarParameterValue(TEXT("Emissive"), Em);
			}
		}

		// Glass + coachroof warm emissive so cabin light reads through the panes.
		if (ShouldCabinBeLit(SlotIndex))
		{
			UMeshComponent* Hull = Boat->FindComponentByClass<UStaticMeshComponent>();
			// Prefer hull static / procedural by name
			TInlineComponentArray<UMeshComponent*> HullCandidates(Boat);
			for (UMeshComponent* C : HullCandidates)
			{
				if (!C) continue;
				const FString N = C->GetName();
				if (N.Contains(TEXT("Hull")) || N.Contains(TEXT("HullStatic")))
				{
					Hull = C;
					break;
				}
			}
			if (Hull)
			{
				ApplyCabinGlassGlow(Hull, /*bLit*/ true, VisScale);
			}
		}
	}
}

AActor* UMooredBoatSubsystem::SpawnMooredBoat(const FMooredBoatSlot& Slot, int32 SlotIndex)
{
	UWorld* World = GetWorld();
	if (!World || !EnsureTemplate()) return nullptr;

	const float YawRad = FMath::DegreesToRadians(Slot.HeadingDeg);
	const FVector Forward(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);
	const float StemToBall = Slot.LineLenCm;
	const FVector OriginXY = Slot.MooringWorldCm - Forward * (BowOffsetCm + StemToBall);
	const FVector Origin(OriginXY.X, OriginXY.Y, WaterlineOffsetCm);
	const FRotator Rot(0.f, Slot.HeadingDeg, 0.f);
	const FTransform BoatXf(Rot, Origin);

	// Spawn deferred so we can install a real root BEFORE the actor is "finished"
	// (replacing root after SpawnActor was snapping boats to world origin → invisible).
	FActorSpawnParameters Sp;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Sp.bDeferConstruction = true;
	AActor* Boat = World->SpawnActor<AActor>(AActor::StaticClass(), BoatXf, Sp);
	if (!Boat) return nullptr;

	USceneComponent* Root = NewObject<USceneComponent>(Boat, TEXT("Root"), RF_Transient);
	Root->SetMobility(EComponentMobility::Movable);
	Boat->SetRootComponent(Root);
	Root->SetWorldTransform(BoatXf);
	Root->RegisterComponent();
	Boat->AddInstanceComponent(Root);

	// Prefer shared static hull (1 multi-section SM vs 14 PMC → VSM/draw relief).
	// Nanite is optional (bHullUseNanite); off by default for crisp paint edges.
	// Keel/rudder are a SEPARATE no-shadow mesh (never cast onto topsides).
	UStaticMeshComponent* HullSmc = nullptr;
	UProceduralMeshComponent* HullPmc = nullptr;
	if (bHullNaniteReady && HullNaniteMesh)
	{
		HullSmc = NewObject<UStaticMeshComponent>(Boat, TEXT("HullStatic"), RF_Transient);
		HullSmc->SetupAttachment(Root);
		HullSmc->SetMobility(EComponentMobility::Movable);
		HullSmc->SetStaticMesh(HullNaniteMesh.Get());
		HullSmc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Near hull: CastShadow off (match HISM). Hero-only shadow = flip in editor if needed.
		HullSmc->SetCastShadow(false);
		HullSmc->SetReceivesDecals(false);
		// Never force Nanite path if the mesh has Nanite off / we want crisp LODs.
		HullSmc->bDisallowNanite = !bHullUseNanite;
		HullSmc->bNeverDistanceCull = false;
		HullSmc->SetCullDistance(LoadRadiusCm * 1.1f);
		StripMooredReflectionCost(HullSmc);
		HullSmc->SetVisibility(true);
		HullSmc->SetHiddenInGame(false);
		ApplyHullMaterialsToStaticMesh(HullSmc);
		HullSmc->RegisterComponent();
		Boat->AddInstanceComponent(HullSmc);
		// Per-boat hull/stripe palette + white decks (MID overrides).
		ApplyBoatPaintScheme(HullSmc, SlotIndex);
	}
	else
	{
		HullPmc = NewObject<UProceduralMeshComponent>(Boat, TEXT("Hull"), RF_Transient);
		HullPmc->SetupAttachment(Root);
		HullPmc->SetMobility(EComponentMobility::Movable);
		HullPmc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HullPmc->SetCastShadow(false);
		HullPmc->SetReceivesDecals(false);
		HullPmc->bNeverDistanceCull = false;
		HullPmc->SetCullDistance(LoadRadiusCm * 1.1f);
		StripMooredReflectionCost(HullPmc);
		HullPmc->RegisterComponent();
		Boat->AddInstanceComponent(HullPmc);
		ApplyHullTo(HullPmc);
		ApplyBoatPaintScheme(HullPmc, SlotIndex);
	}

	if (KeelSections.Num() > 0)
	{
		UProceduralMeshComponent* KeelPmc = NewObject<UProceduralMeshComponent>(
			Boat, TEXT("KeelAppendages"), RF_Transient);
		KeelPmc->SetupAttachment(Root);
		KeelPmc->SetMobility(EComponentMobility::Movable);
		KeelPmc->RegisterComponent();
		Boat->AddInstanceComponent(KeelPmc);
		ApplyKeelTo(KeelPmc);
	}

	auto MakeCyl = [&](const TCHAR* Name) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Boat, Name, RF_Transient);
		C->SetupAttachment(Root);
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Thin spars/lines stay non-Nanite — do not cast shadows (main VSM pressure was hulls).
		C->SetCastShadow(false);
		C->bNeverDistanceCull = false;
		C->SetCullDistance(LoadRadiusCm * 1.1f);
		StripMooredReflectionCost(C);
		C->RegisterComponent();
		Boat->AddInstanceComponent(C);
		return C;
	};
	UStaticMeshComponent* Mast = MakeCyl(TEXT("Mast"));
	UStaticMeshComponent* Boom = MakeCyl(TEXT("Boom"));

	UMaterialInterface* SparMat = YachtSparMat.Get() ? YachtSparMat.Get() : SparMaterial.Get();
	if (TemplateSpars.bMastValid)
	{
		PlaceCylinder(Mast, TemplateSpars.MastBase, TemplateSpars.MastTop, 0.10f, SparMat);
		if (!YachtSparMat)
		{
			if (UMaterialInstanceDynamic* Mid = Mast->CreateAndSetMaterialInstanceDynamic(0))
			{
				const FLinearColor Al(0.913f, 0.921f, 0.925f);
				Mid->SetVectorParameterValue(TEXT("Color"), Al);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Al);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 1.f);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.28f);
			}
		}
	}
	if (TemplateSpars.bBoomValid)
	{
		const FVector Base = TemplateSpars.BoomBase;
		const FVector End = TemplateSpars.BoomEnd;
		const FVector D = End - Base;
		const float LenXY = FMath::Max(1.f, FVector2D(D.X, D.Y).Size());
		const FVector CenterEnd = Base + FVector(-LenXY, 0.f, D.Z);
		PlaceCylinder(Boom, Base, CenterEnd, 0.08f, SparMat);
		if (!YachtSparMat)
		{
			if (UMaterialInstanceDynamic* Mid = Boom->CreateAndSetMaterialInstanceDynamic(0))
			{
				const FLinearColor Al(0.913f, 0.921f, 0.925f);
				Mid->SetVectorParameterValue(TEXT("Color"), Al);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Al);
				Mid->SetScalarParameterValue(TEXT("Metallic"), 1.f);
				Mid->SetScalarParameterValue(TEXT("Roughness"), 0.28f);
			}
		}
	}

	// Standing rig (spreaders + stays) like the web app
	AddStandingRigging(Boat, Root);

	// Masthead all-round white — vessels at anchor / on a mooring.
	AddAnchorLight(Boat, Root);
	// ~half the fleet: warm cabin light + glass emissive (reads through coachroof windows).
	UMeshComponent* HullForCabin = HullSmc
		? static_cast<UMeshComponent*>(HullSmc)
		: static_cast<UMeshComponent*>(HullPmc);
	AddCabinLight(Boat, Root, HullForCabin, SlotIndex);

	// Bow pennant stem → top ring on the mooring ball (multi-segment rope).
	CacheMooringRingHeight();
	const FVector BowLocal(BowOffsetCm, 0.f, 80.f);
	const FVector RingLocal = BoatXf.InverseTransformPosition(GetMooringRingWorld(Slot));
	UMaterialInterface* RopeMat = YachtRopeMat.Get()
		? YachtRopeMat.Get()
		: (LineMaterial.Get() ? LineMaterial.Get() : SparMaterial.Get());
	const int32 NSeg = FMath::Clamp(PennantSegmentCount, 2, 4);
	TArray<UStaticMeshComponent*, TInlineAllocator<4>> PennantSegs;
	PennantSegs.Reserve(NSeg);
	for (int32 Si = 0; Si < NSeg; ++Si)
	{
		UStaticMeshComponent* Seg = MakeCyl(*FString::Printf(TEXT("Pennant_%d"), Si));
		// Initial straight layout; sway replaces with catenary each tick.
		const float T0 = float(Si) / float(NSeg);
		const float T1 = float(Si + 1) / float(NSeg);
		const FVector A = FMath::Lerp(BowLocal, RingLocal, T0);
		const FVector B = FMath::Lerp(BowLocal, RingLocal, T1);
		PlaceCylinder(Seg, A, B, 0.014f, RopeMat);
		PennantSegs.Add(Seg);
	}

	Boat->Tags.Add(FName(TEXT("MooredBoat")));
	Boat->SetActorEnableCollision(false);
	Boat->SetActorHiddenInGame(false);
	Boat->FinishSpawning(BoatXf, /*bIsDefaultTransform*/ true);
	// Re-assert transform after FinishSpawning (some paths re-root). Single write.
	Boat->SetActorTransform(BoatXf, false, nullptr, ETeleportType::TeleportPhysics);
	if (HullSmc)
	{
		HullSmc->UpdateBounds();
		HullSmc->MarkRenderStateDirty();
	}
	if (HullPmc)
	{
		HullPmc->UpdateBounds();
		HullPmc->MarkRenderStateDirty();
	}

#if WITH_EDITOR
	Boat->SetActorLabel(FString::Printf(TEXT("MooredJ105_%d"), SlotIndex));
#endif

	InitSwayState(SlotIndex, PennantSegs);

	const FVector Actual = Boat->GetActorLocation();
	const float DistPlayerCm = FVector2D::Distance(
		FVector2D(Actual.X, Actual.Y),
		FNavGeo::BoatStartWorldCm2D());
	if (SlotIndex < 3)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("MooredBoats: boat[%d] want=%s got=%s dHarbor=%.0fm heading=%.0f nanite=%d bounds=%s"),
			SlotIndex, *Origin.ToCompactString(), *Actual.ToCompactString(),
			DistPlayerCm * 0.01f, Slot.HeadingDeg, HullSmc ? 1 : 0,
			HullSmc ? *HullSmc->Bounds.ToString()
				: (HullPmc ? *HullPmc->Bounds.ToString() : TEXT("none")));
	}

	return Boat;
}

void UMooredBoatSubsystem::ForceStreamAround(FVector FocusWorld)
{
	if (!bEnabled) return;
	if (!bSlotsReady && !ReloadSlots()) return;

	// BeginPlay + Possess both call this on the same frame; without debounce we
	// spawn 48, Destroy them (deferred), spawn 48 more → two visible sets.
	if (StreamDebounceLeft > 0.f && Resident.Num() > 0)
	{
		LastFocus = FocusWorld;
		return;
	}
	StreamDebounceLeft = StreamDebounceSec;

	// Incremental stream only — do not ClearAll (avoids deferred-destroy doubles).
	RebuildAround(FocusWorld);
}

void UMooredBoatSubsystem::RebuildAround(const FVector& Focus)
{
	LastFocus = Focus;
	if (!bEnabled || !bSlotsReady || Slots.Num() == 0) return;

	const float NearR2 = NearFullRadiusCm * NearFullRadiusCm;
	const float MidR2 = MidHismRadiusCm * MidHismRadiusCm;
	const float LoadR2 = LoadRadiusCm * LoadRadiusCm;
	const float UnloadR2 = UnloadRadiusCm * UnloadRadiusCm;
	const FVector2D F2(Focus.X, Focus.Y);

	// Classify every slot within unload hysteresis.
	TSet<int32> WantNear;
	TSet<int32> WantMid;
	for (int32 I = 0; I < Slots.Num(); ++I)
	{
		const FVector2D P(Slots[I].MooringWorldCm.X, Slots[I].MooringWorldCm.Y);
		const float D2 = FVector2D::DistSquared(F2, P);
		const bool bWasNear = Resident.Contains(I);
		const bool bWasMid = MidHismSlotToInstance.Contains(I);
		const float KeepR2 = (bWasNear || bWasMid) ? UnloadR2 : LoadR2;
		if (D2 > KeepR2) continue;
		if (D2 <= NearR2)
		{
			WantNear.Add(I);
		}
		else if (D2 <= MidR2 || D2 <= LoadR2)
		{
			// Mid band: HISM only (also covers Near..Load if Mid < Load).
			WantMid.Add(I);
		}
	}

	// Collapse NearFull → ≤ MaxNearFullBoats heroes (closest to focus); rest → Static HISM.
	if (MaxNearFullBoats <= 0)
	{
		for (int32 I : WantNear) { WantMid.Add(I); }
		WantNear.Reset();
	}
	else if (WantNear.Num() > MaxNearFullBoats)
	{
		TArray<TPair<float, int32>> Ranked;
		Ranked.Reserve(WantNear.Num());
		for (int32 I : WantNear)
		{
			const FVector2D P(Slots[I].MooringWorldCm.X, Slots[I].MooringWorldCm.Y);
			Ranked.Emplace(FVector2D::DistSquared(F2, P), I);
		}
		Ranked.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
		{
			return A.Key < B.Key;
		});
		WantNear.Reset();
		for (int32 R = 0; R < Ranked.Num(); ++R)
		{
			const int32 I = Ranked[R].Value;
			if (R < MaxNearFullBoats) WantNear.Add(I);
			else WantMid.Add(I);
		}
	}

	// Cap Static HISM to MooringSceneryInstanceCount (closest to focus).
	if (MooringSceneryInstanceCount > 0 && WantMid.Num() > MooringSceneryInstanceCount)
	{
		TArray<TPair<float, int32>> RankedMid;
		RankedMid.Reserve(WantMid.Num());
		for (int32 I : WantMid)
		{
			const FVector2D P(Slots[I].MooringWorldCm.X, Slots[I].MooringWorldCm.Y);
			RankedMid.Emplace(FVector2D::DistSquared(F2, P), I);
		}
		RankedMid.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B)
		{
			return A.Key < B.Key;
		});
		WantMid.Reset();
		for (int32 R = 0; R < RankedMid.Num() && R < MooringSceneryInstanceCount; ++R)
		{
			WantMid.Add(RankedMid[R].Value);
		}
	}

	// Drop near actors no longer wanted (or demoted to mid).
	TArray<int32> DropNear;
	for (auto& Pair : Resident)
	{
		if (!WantNear.Contains(Pair.Key))
		{
			if (IsValid(Pair.Value)) Pair.Value->Destroy();
			DropNear.Add(Pair.Key);
		}
	}
	for (int32 K : DropNear)
	{
		Resident.Remove(K);
		SwayState.Remove(K);
	}

	// Drop mid instances no longer wanted (or promoted to near).
	TArray<int32> DropMid;
	for (const auto& Pair : MidHismSlotToInstance)
	{
		if (!WantMid.Contains(Pair.Key) || WantNear.Contains(Pair.Key))
		{
			DropMid.Add(Pair.Key);
		}
	}
	for (int32 K : DropMid)
	{
		RemoveMidHism(K);
	}

	// Spawn near full boats.
	int32 SpawnedNear = 0;
	for (int32 I : WantNear)
	{
		// Must not also be mid.
		RemoveMidHism(I);
		if (Resident.Contains(I)) continue;
		if (AActor* A = SpawnMooredBoat(Slots[I], I))
		{
			Resident.Add(I, A);
			++SpawnedNear;
		}
	}

	// Mid HISM hulls.
	int32 SpawnedMid = 0;
	for (int32 I : WantMid)
	{
		if (WantNear.Contains(I) || Resident.Contains(I)) continue;
		const bool bHad = MidHismSlotToInstance.Contains(I);
		AddOrUpdateMidHism(I, Slots[I]);
		if (!bHad) ++SpawnedMid;
	}

	if (SpawnedNear > 0 || DropNear.Num() > 0 || SpawnedMid > 0 || DropMid.Num() > 0)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("MooredBoats: near=%d (+%d -%d) hism=%d (+%d -%d) focus=(%.0f,%.0f)"),
			Resident.Num(), SpawnedNear, DropNear.Num(),
			MidHismSlotToInstance.Num(), SpawnedMid, DropMid.Num(),
			Focus.X, Focus.Y);
	}
}


FTransform UMooredBoatSubsystem::MakeMooredHullTransform(const FMooredBoatSlot& Slot) const
{
	const float YawRad = FMath::DegreesToRadians(Slot.HeadingDeg);
	const FVector Forward(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);
	const float StemToBall = Slot.LineLenCm;
	const FVector OriginXY = Slot.MooringWorldCm - Forward * (BowOffsetCm + StemToBall);
	const FVector Origin(OriginXY.X, OriginXY.Y, WaterlineOffsetCm);
	return FTransform(FRotator(0.f, Slot.HeadingDeg, 0.f), Origin);
}

void UMooredBoatSubsystem::EnsureMidHism()
{
	if (MidHullHism && IsValid(MidHismOwner)) return;
	UWorld* World = GetWorld();
	if (!World || !HullNaniteMesh) return;

	FActorSpawnParameters Sp;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Sp);
	if (!Owner) return;
#if WITH_EDITOR
	Owner->SetActorLabel(TEXT("MooredMidHISM"));
#endif
	Owner->Tags.Add(FName(TEXT("MooredBoat")));
	Owner->Tags.Add(FName(TEXT("MooredHISM")));
	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"), RF_Transient);
	Root->SetMobility(EComponentMobility::Static);
	Owner->SetRootComponent(Root);
	Root->RegisterComponent();
	Owner->AddInstanceComponent(Root);

	UHierarchicalInstancedStaticMeshComponent* H =
		NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, TEXT("MidHullHISM"), RF_Transient);
	H->SetupAttachment(Root);
	H->SetMobility(EComponentMobility::Static);
	H->SetStaticMesh(HullNaniteMesh.Get());
	H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	H->SetCastShadow(false);
	H->bDisallowNanite = !bHullUseNanite;
	H->bNeverDistanceCull = false;
	StripMooredReflectionCost(H);
	// Start fade beyond mid band so mid-harbor stays readable boats (not dots);
	// end cull past load radius for far-field scenery.
	H->SetCullDistances(
		FMath::Max(MidHismRadiusCm * 0.85f, 20000.f),
		LoadRadiusCm * 1.25f);
	ApplyHullMaterialsToStaticMesh(H);
	H->RegisterComponent();
	Owner->AddInstanceComponent(H);

	MidHismOwner = Owner;
	MidHullHism = H;
}

void UMooredBoatSubsystem::ClearMidHism()
{
	MidHismSlotToInstance.Reset();
	if (IsValid(MidHismOwner))
	{
		MidHismOwner->Destroy();
	}
	MidHismOwner = nullptr;
	MidHullHism = nullptr;
}

void UMooredBoatSubsystem::AddOrUpdateMidHism(int32 SlotIndex, const FMooredBoatSlot& Slot)
{
	if (!EnsureTemplate() || !bHullNaniteReady || !HullNaniteMesh) return;
	EnsureMidHism();
	if (!MidHullHism) return;

	const FTransform Xf = MakeMooredHullTransform(Slot);
	if (int32* Existing = MidHismSlotToInstance.Find(SlotIndex))
	{
		MidHullHism->UpdateInstanceTransform(*Existing, Xf, true, true, true);
		return;
	}
	const int32 Id = MidHullHism->AddInstance(Xf, /*bWorldSpace*/ true);
	MidHismSlotToInstance.Add(SlotIndex, Id);
}

void UMooredBoatSubsystem::RemoveMidHism(int32 SlotIndex)
{
	int32* InstId = MidHismSlotToInstance.Find(SlotIndex);
	if (!InstId || !MidHullHism)
	{
		MidHismSlotToInstance.Remove(SlotIndex);
		return;
	}
	// Swap-remove: HISM removes by index and may reorder — rebuild map if needed.
	const int32 RemoveIdx = *InstId;
	const int32 LastIdx = MidHullHism->GetInstanceCount() - 1;
	if (RemoveIdx < 0 || RemoveIdx > LastIdx)
	{
		MidHismSlotToInstance.Remove(SlotIndex);
		return;
	}
	if (RemoveIdx != LastIdx)
	{
		// Find which slot owns LastIdx and retarget.
		for (auto& Pair : MidHismSlotToInstance)
		{
			if (Pair.Value == LastIdx)
			{
				Pair.Value = RemoveIdx;
				break;
			}
		}
	}
	MidHullHism->RemoveInstance(RemoveIdx);
	MidHismSlotToInstance.Remove(SlotIndex);
	if (MidHismSlotToInstance.Num() == 0)
	{
		ClearMidHism();
	}
}


FVector UMooredBoatSubsystem::GetFocusLocation() const
{
	UWorld* World = GetWorld();
	if (!World) return LastFocus;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* P = PC->GetPawn())
		{
			return P->GetActorLocation();
		}
	}
	return LastFocus;
}

void UMooredBoatSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !bSlotsReady) return;
	UWorld* World = GetWorld();
	if (!World || World->IsPreviewWorld()) return;

	SAIL_PERF_SCOPE(Moored);
	FSailSimPerf::Get().MooredCount = Resident.Num() + MidHismSlotToInstance.Num();

	if (StreamDebounceLeft > 0.f)
	{
		StreamDebounceLeft = FMath::Max(0.f, StreamDebounceLeft - DeltaTime);
	}

	// Wind-driven pendulum sway every frame.
	if (Resident.Num() > 0)
	{
		UpdateAllSway(DeltaTime);
	}

	// Anchor light intensity follows day/night (cheap; not every frame).
	LightAccum += DeltaTime;
	if (LightAccum >= 1.0f)
	{
		LightAccum = 0.f;
		UpdateAllAnchorLights();
	}

	// Stream load/unload on a slower interval.
	Accum += DeltaTime;
	if (Accum < UpdateIntervalSec) return;
	Accum = 0.f;

	const FVector Focus = GetFocusLocation();
	if (Focus.IsNearlyZero() && LastFocus.IsNearlyZero()) return;
	const int32 Before = Resident.Num();
	RebuildAround(Focus.IsNearlyZero() ? LastFocus : Focus);
	// New residents need a force refresh (seed intensity may not match current preset).
	if (Resident.Num() != Before)
	{
		LastAnchorVisScale = -1.f;
		UpdateAllAnchorLights();
	}
}
