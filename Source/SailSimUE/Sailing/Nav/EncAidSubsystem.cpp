#include "Sailing/Nav/EncAidSubsystem.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

void UEncAidSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogSailSim, Log, TEXT("EncAid: Initialize world=%s type=%d"),
		*GetNameSafe(GetWorld()), GetWorld() ? static_cast<int32>(GetWorld()->WorldType) : -1);
	ReloadData();
}

void UEncAidSubsystem::Deinitialize()
{
	ClearAllInstances();
	Aids.Reset();
	bDataLoaded = false;
	Super::Deinitialize();
}

bool UEncAidSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UEncAidSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UEncAidSubsystem, STATGROUP_Tickables);
}

void UEncAidSubsystem::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		ClearAllInstances();
	}
}

FString UEncAidSubsystem::GetStatusLine() const
{
	if (!bDataLoaded) return TEXT("ENC aids — no data");
	return FString::Printf(TEXT("ENC aids %d/%d visible"), VisibleCount, Aids.Num());
}

bool UEncAidSubsystem::FindMooringRingWorld(
	const FVector& NearWorld, FVector& OutRingWorld, float MaxDistCm) const
{
	// Prefer dedicated mooring ISMs (colourful balls); fall back to mesh-1/2 navaid ISMs.
	float BestD2 = MaxDistCm * MaxDistCm;
	bool bFound = false;
	FVector BestRing = FVector::ZeroVector;

	auto Consider = [&](UInstancedStaticMeshComponent* Ism)
	{
		if (!Ism || !Ism->GetStaticMesh()) return;
		const FBox LocalBox = Ism->GetStaticMesh()->GetBoundingBox();
		const FVector LocalRing(
			LocalBox.GetCenter().X,
			LocalBox.GetCenter().Y,
			LocalBox.Max.Z);
		const int32 N = Ism->GetInstanceCount();
		for (int32 Ii = 0; Ii < N; ++Ii)
		{
			FTransform Xf;
			if (!Ism->GetInstanceTransform(Ii, Xf, /*bWorldSpace*/ true)) continue;
			const float D2 = FVector::DistSquared2D(NearWorld, Xf.GetLocation());
			if (D2 > BestD2) continue;
			BestD2 = D2;
			BestRing = Xf.TransformPosition(LocalRing);
			BestRing.Z += 6.f;
			bFound = true;
		}
	};

	for (const auto& Pair : IsmByMooring)
	{
		if (!IsValid(Pair.Value)) continue;
		Consider(Pair.Value->FindComponentByClass<UInstancedStaticMeshComponent>());
	}
	if (!bFound)
	{
		for (int32 MeshId : {1, 2})
		{
			const TObjectPtr<AActor>* OwnerPtr = IsmByMesh.Find(MeshId);
			if (!OwnerPtr || !IsValid(*OwnerPtr)) continue;
			Consider((*OwnerPtr)->FindComponentByClass<UInstancedStaticMeshComponent>());
		}
	}

	if (bFound)
	{
		OutRingWorld = BestRing;
	}
	return bFound;
}

namespace EncAidMatUtil
{
	/** Without InstancedStaticMeshes usage, ISM draws DefaultMaterial (dark gray). */
	static void ForceIsmUsage(UMaterialInterface* Interface)
	{
		if (!Interface) return;
		UMaterial* Mat = Interface->GetMaterial();
		if (!Mat) return;

		if (Mat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
		{
			return;
		}

		// UE 5.8: virtual SetMaterialUsage(EMaterialUsage) + SetUsageByFlag.
		const bool bOk = Mat->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		if (!Mat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
		{
			Mat->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		}
		// Ensure shaders exist for the new usage (SetUsageByFlag alone skips recompile).
		if (!Mat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) || !bOk)
		{
			Mat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		}
		Mat->CacheShaders(EMaterialShaderPrecompileMode::Default);

		if (Mat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes))
		{
			UE_LOG(LogSailSim, Log, TEXT("EncAid: InstancedStaticMeshes usage OK on %s"),
				*Mat->GetPathName());
		}
		else
		{
			UE_LOG(LogSailSim, Error,
				TEXT("EncAid: FAILED to set InstancedStaticMeshes on %s — mooring balls will be default gray"),
				*Mat->GetPathName());
		}
	}
}

void UEncAidSubsystem::EnsureMooringColorMaterials()
{
	if (MooringColorMats.Num() > 0) return;

	// HullPaint = same clearcoat gelcoat path as the yachts, with local-Z bands for
	// bottom stripe / top cap. Prefer master M_ over MI_ so we can stamp usage flags.
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_HullPaint.M_Yacht_HullPaint"));
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_HullPaint.MI_Yacht_HullPaint"));
	}
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR.M_Yacht_PBR"));
	}
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}
	if (!Base) return;
	EncAidMatUtil::ForceIsmUsage(Base);

	// Local Z range of SM_Buoy_1/2 so bands sit on the sphere, not boat waterlines.
	MooringMeshMinZ = -50.f;
	MooringMeshMaxZ = 50.f;
	for (int32 MeshId : {1, 2})
	{
		const FString Path = FString::Printf(
			TEXT("/Game/Buoys/StaticMesh/SM_Buoy_%d.SM_Buoy_%d"), MeshId, MeshId);
		if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path))
		{
			const FBox B = Mesh->GetBoundingBox();
			MooringMeshMinZ = FMath::Min(MooringMeshMinZ, B.Min.Z);
			MooringMeshMaxZ = FMath::Max(MooringMeshMaxZ, B.Max.Z);
		}
	}
	const float Z0 = MooringMeshMinZ;
	const float Z1 = MooringMeshMaxZ;
	const float ZH = FMath::Max(1.f, Z1 - Z0);

	// Named "looks" — not a random color×pattern matrix.
	// Most of the field shares one quaint recipe; a few eccentrics break monotony.
	//
	// HullPaint bands (local Z): antifoul (bottom) | boot (mid stripe) | topsides | stripe (cap).
	struct FMooringLook
	{
		const TCHAR* Name;
		FColor Body;     // main sphere
		FColor Bottom;   // lower third (often barn red or darker)
		FColor Band;     // circumference stripe (optional)
		FColor Cap;      // upper cap (optional)
		bool bBand;
		bool bCap;
		bool bAlgae;     // olive film on bottom
		float Weather;   // 0 new PE … 1 chalky
		int32 Weight;    // relative pick weight (sum ≈ 100)
	};

	// Every look has a clear Bottom colour (~lower third). That bottom is what reads charming.
	const FMooringLook Looks[MooringLookCount] = {
		// ── Dominant field ───────────────────────────────────────────────
		// White · blue stripe · barn-red bottom
		{ TEXT("WhiteBlueRed"),
			FColor(0xF8, 0xF9, 0xFA), FColor(0x7C, 0x0A, 0x02),
			FColor(0x00, 0x33, 0xA0), FColor(0xFA, 0xFB, 0xFC),
			true, false, true, 0.08f, 48 },
		// White · cream stripe · deep red bottom
		{ TEXT("WhiteCreamRed"),
			FColor(0xF8, 0xF9, 0xFA), FColor(0x5C, 0x08, 0x01),
			FColor(0xF5, 0xF0, 0xE6), FColor(0xFA, 0xFB, 0xFC),
			true, false, true, 0.12f, 10 },
		// White · no stripe · red bottom only
		{ TEXT("WhiteRedBottom"),
			FColor(0xF8, 0xF9, 0xFA), FColor(0x9B, 0x23, 0x35),
			FColor(0xF8, 0xF9, 0xFA), FColor(0xFA, 0xFB, 0xFC),
			false, false, true, 0.10f, 8 },

		// ── Eclectic ─────────────────────────────────────────────────────
		// Barn red body · cream stripe · darker red bottom
		{ TEXT("BarnRedCream"),
			FColor(0x7C, 0x0A, 0x02), FColor(0x5C, 0x08, 0x01),
			FColor(0xF5, 0xF0, 0xE6), FColor(0x9B, 0x23, 0x35),
			true, false, true, 0.10f, 12 },
		// Forest green · cream stripe · deep green bottom
		{ TEXT("ForestGreenCream"),
			FColor(0x35, 0x5E, 0x3B), FColor(0x1A, 0x3A, 0x1F),
			FColor(0xF8, 0xF4, 0xE8), FColor(0x2E, 0x8B, 0x57),
			true, false, true, 0.12f, 8 },
		// Navy body · white stripe · barn-red bottom (keeps the charming red waterline)
		{ TEXT("NavyWhiteRed"),
			FColor(0x0A, 0x16, 0x28), FColor(0x7C, 0x0A, 0x02),
			FColor(0xFF, 0xFF, 0xFF), FColor(0x1E, 0x3A, 0x5F),
			true, false, true, 0.08f, 6 },
		// Weathered red · cream stripe · dark red bottom
		{ TEXT("WeatheredRed"),
			FColor(0x6B, 0x2E, 0x2E), FColor(0x5C, 0x08, 0x01),
			FColor(0xF5, 0xF0, 0xE6), FColor(0x9B, 0x23, 0x35),
			true, false, true, 0.60f, 5 },
		// Weathered green · cream stripe · deep green bottom
		{ TEXT("WeatheredGreen"),
			FColor(0x2A, 0x4A, 0x32), FColor(0x1A, 0x3A, 0x1F),
			FColor(0xF8, 0xF4, 0xE8), FColor(0x2E, 0x8B, 0x57),
			true, false, true, 0.55f, 3 },
	};

	const float Gloss = FMath::Clamp(MooringBallGloss, 0.f, 1.f);
	const FLinearColor AlgaeGreen(0.11f, 0.17f, 0.10f, 1.f);
	MooringColorMats.Reserve(MooringLookCount);

	for (int32 Li = 0; Li < MooringLookCount; ++Li)
	{
		const FMooringLook& L = Looks[Li];
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
		if (!Mid) continue;

		const FLinearColor Body = FLinearColor::FromSRGBColor(L.Body);
		FLinearColor Bottom = FLinearColor::FromSRGBColor(L.Bottom);
		const FLinearColor Band = FLinearColor::FromSRGBColor(L.Band);
		const FLinearColor Cap = FLinearColor::FromSRGBColor(L.Cap);
		if (L.bAlgae)
		{
			Bottom = FLinearColor::LerpUsingHSV(Bottom, AlgaeGreen,
				FMath::Lerp(0.18f, 0.42f, L.Weather));
		}

		// Soft painted PE: diffuse + modest specular. ClearCoat always off.
		const float RoughTop = FMath::Clamp(0.28f - Gloss * 0.08f + L.Weather * 0.16f, 0.18f, 0.48f);
		const float RoughBot = RoughTop + 0.12f + L.Weather * 0.06f;
		const float Spec = FMath::Clamp(0.32f + Gloss * 0.10f - L.Weather * 0.08f, 0.22f, 0.42f);

		// ALWAYS a solid bottom colour (~lower 40% of the ball).
		// HullPaint: z < BootLo → antifoul (bottom); BootLo..BootHi → boot (optional stripe).
		const float BottomEnd = Z0 + ZH * 0.40f;
		FLinearColor ColAnti = Bottom;
		FLinearColor ColBoot = Bottom;
		FLinearColor ColTop = Body;
		FLinearColor ColStripe = Body;
		float BootLo = BottomEnd;
		float BootHi = BottomEnd; // no mid stripe by default
		float StripeLo = Z1 + 40.f;
		float StripeHi = Z1 + 50.f;

		if (L.bBand)
		{
			// Stripe sits just above the coloured bottom (not replacing it).
			ColBoot = Band;
			BootLo = BottomEnd;
			BootHi = Z0 + ZH * 0.54f;
		}
		if (L.bCap)
		{
			ColStripe = Cap;
			StripeLo = Z0 + ZH * 0.58f;
			StripeHi = Z1 + 8.f;
		}

		Mid->SetVectorParameterValue(TEXT("ColorTopsides"), ColTop);
		Mid->SetVectorParameterValue(TEXT("ColorAntifoul"), ColAnti);
		Mid->SetVectorParameterValue(TEXT("ColorBoot"), ColBoot);
		Mid->SetVectorParameterValue(TEXT("ColorStripe"), ColStripe);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), ColTop);
		Mid->SetVectorParameterValue(TEXT("Color"), ColTop);

		Mid->SetScalarParameterValue(TEXT("ZBootLo"), BootLo);
		Mid->SetScalarParameterValue(TEXT("ZBootHi"), BootHi);
		Mid->SetScalarParameterValue(TEXT("ZStripeLo"), StripeLo);
		Mid->SetScalarParameterValue(TEXT("ZStripeHi"), StripeHi);

		Mid->SetScalarParameterValue(TEXT("RoughTopsides"), RoughTop);
		Mid->SetScalarParameterValue(TEXT("RoughAntifoul"), RoughBot);
		Mid->SetScalarParameterValue(TEXT("RoughBoot"), RoughTop + 0.05f);
		Mid->SetScalarParameterValue(TEXT("RoughStripe"), RoughTop + 0.03f);
		Mid->SetScalarParameterValue(TEXT("Roughness"), RoughTop);
		Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
		Mid->SetScalarParameterValue(TEXT("Specular"), Spec);
		// No clearcoat layer — soft plastic/paint only.
		Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.f);
		Mid->SetScalarParameterValue(TEXT("ClearCoatRoughness"), 1.f);
		Mid->SetScalarParameterValue(TEXT("ClearCoatBoost"), 0.f);

		EncAidMatUtil::ForceIsmUsage(Mid);
		MooringColorMats.Add(Mid);
	}

	const bool bIsmOk = Base->GetMaterial()
		&& Base->GetMaterial()->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes);
	UE_LOG(LogSailSim, Log,
		TEXT("EncAid: mooring looks ready (%d looks, dominant=WhiteBlueRed, gloss=%.2f no-clearcoat, ismUsage=%d)"),
		MooringColorMats.Num(), Gloss, bIsmOk ? 1 : 0);
}

int32 UEncAidSubsystem::PickMooringStyleIndex(int32 AidIndex, const FEncAidDesc& A) const
{
	// Stable hash from position + index so reloads don't reshuffle the field.
	uint32 H = GetTypeHash(AidIndex);
	H = HashCombine(H, GetTypeHash(FMath::RoundToInt(A.Lat * 1.0e6)));
	H = HashCombine(H, GetTypeHash(FMath::RoundToInt(A.Lon * 1.0e6)));
	H = HashCombine(H, GetTypeHash(A.MeshId));

	// Weighted pick matching FMooringLook::Weight (sums to 100).
	// 0 WhiteBlueRed 48 · 1 WhiteCreamRed 10 · 2 WhiteRedBottom 8 ·
	// 3 BarnRedCream 12 · 4 ForestGreenCream 8 · 5 NavyWhite 6 ·
	// 6 WeatheredRed 5 · 7 WeatheredGreen 3
	static const int32 CumW[MooringLookCount] = { 48, 58, 66, 78, 86, 92, 97, 100 };
	const int32 Roll = static_cast<int32>(H % 100u);
	for (int32 I = 0; I < MooringLookCount; ++I)
	{
		if (Roll < CumW[I]) return I;
	}
	return 0; // WhiteBlueRed
}

bool UEncAidSubsystem::ResolveDataPath(FString& OutPath) const
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

bool UEncAidSubsystem::ReloadData()
{
	Aids.Reset();
	bDataLoaded = false;
	ClearAllInstances();

	FString Path;
	if (!ResolveDataPath(Path))
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: missing Content/Nav/enc_aids_nantucket.json (run Scripts/bake_enc_aids_ue.py)"));
		return false;
	}
	if (!LoadAidsFromJson(Path))
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: failed to parse %s"), *Path);
		return false;
	}
	bDataLoaded = true;
	UE_LOG(LogSailSim, Log, TEXT("EncAid: loaded %d aids from %s"), Aids.Num(), *Path);
	return true;
}

bool UEncAidSubsystem::LoadAidsFromJson(const FString& AbsPath)
{
	FString JsonStr;
	if (!FFileHelper::LoadFileToString(JsonStr, *AbsPath)) return false;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("aids"), Arr) || !Arr) return false;

	Aids.Reserve(Arr->Num());
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) continue;
		FEncAidDesc A;
		A.Lat = O->GetNumberField(TEXT("lat"));
		A.Lon = O->GetNumberField(TEXT("lon"));
		A.MeshId = FMath::Clamp(static_cast<int32>(O->GetNumberField(TEXT("mesh"))), 1, 9);
		A.Kind = O->GetStringField(TEXT("kind"));
		A.Name = O->GetStringField(TEXT("name"));
		A.Colour = static_cast<int32>(O->GetNumberField(TEXT("colour")));
		A.CatLam = static_cast<int32>(O->GetNumberField(TEXT("catlam")));
		A.Shape = O->GetStringField(TEXT("shape"));
		double X = 0, Y = 0;
		FNavGeo::LatLonToWorldCm(A.Lat, A.Lon, X, Y);
		A.WorldCm = FVector(static_cast<float>(X), static_cast<float>(Y), WaterlineOffsetCm);
		Aids.Add(A);
	}
	return Aids.Num() > 0;
}

UStaticMesh* UEncAidSubsystem::LoadBuoyMesh(int32 MeshId) const
{
	const FString Path = FString::Printf(
		TEXT("/Game/Buoys/StaticMesh/SM_Buoy_%d.SM_Buoy_%d"), MeshId, MeshId);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
	if (!Mesh)
	{
		Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *Path));
	}
	if (!Mesh)
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: failed LoadObject %s"), *Path);
		return nullptr;
	}

	// Prefer Nanite (VSM non-Nanite overflow fix). Assets are baked via
	// Scripts/enable_nanite_buoys.py; if still off, try enabling in-editor once.
	if (!Mesh->IsNaniteEnabled())
	{
#if WITH_EDITOR
		FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
		Nanite.bEnabled = true;
		Nanite.FallbackPercentTriangles = 1.0f;
		Mesh->SetNaniteSettings(Nanite);
		Mesh->Build(true);
		UE_LOG(LogSailSim, Log, TEXT("EncAid: enabled Nanite on SM_Buoy_%d (nanite=%d)"),
			MeshId, Mesh->IsNaniteEnabled() ? 1 : 0);
#else
		UE_LOG(LogSailSim, Warning,
			TEXT("EncAid: SM_Buoy_%d is not Nanite — run Scripts/enable_nanite_buoys.py"), MeshId);
#endif
	}
	return Mesh;
}

void UEncAidSubsystem::PrepareMeshMaterialsForIsm(UStaticMesh* Mesh, UInstancedStaticMeshComponent* Ism) const
{
	if (!Mesh || !Ism) return;

	// Pack buoy materials often lack "Used with Instanced Static Meshes".
	// Without that usage flag, ISM draws nothing on a cold editor start (until
	// a mid-session shader recompile). Force the usage and bind MIDs.
	const int32 Num = Mesh->GetStaticMaterials().Num();
	for (int32 Mi = 0; Mi < FMath::Max(1, Num); ++Mi)
	{
		UMaterialInterface* Base = Mesh->GetMaterial(Mi);
		if (!Base)
		{
			Base = LoadObject<UMaterialInterface>(
				nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		if (!Base) continue;

		// Ensure material is compiled for ISM (sets usage bit + queues shader).
		// Keep the pack material (DefaultLit + textures) so buoys light correctly
		// under SkyAtmosphere / directional light — only force the usage flag.
		Base->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		Ism->SetMaterial(Mi, Base);
	}
}

UInstancedStaticMeshComponent* UEncAidSubsystem::EnsureIsm(int32 MeshId, const FVector& AnchorWorld)
{
	if (TObjectPtr<AActor>* Found = IsmByMesh.Find(MeshId))
	{
		if (IsValid(*Found))
		{
			(*Found)->SetActorLocation(AnchorWorld);
			if (UInstancedStaticMeshComponent* I =
					(*Found)->FindComponentByClass<UInstancedStaticMeshComponent>())
			{
				I->SetWorldLocation(AnchorWorld);
				return I;
			}
		}
		else
		{
			IsmByMesh.Remove(MeshId);
		}
	}

	UWorld* World = GetWorld();
	if (!World) return nullptr;
	UStaticMesh* Mesh = LoadBuoyMesh(MeshId);
	if (!Mesh)
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: missing mesh SM_Buoy_%d"), MeshId);
		return nullptr;
	}

	FActorSpawnParameters Sp;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Owner = World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform(FRotator::ZeroRotator, AnchorWorld), Sp);
	if (!Owner)
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: SpawnActor failed for mesh %d"), MeshId);
		return nullptr;
	}
#if WITH_EDITOR
	Owner->SetActorLabel(FString::Printf(TEXT("EncAid_ISM_%d"), MeshId));
#endif
	Owner->Tags.Add(FName(TEXT("EncAid")));
	Owner->SetActorEnableCollision(false);
	Owner->SetActorHiddenInGame(false);

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"), RF_Transient);
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->SetWorldLocation(AnchorWorld);
	Root->RegisterComponent();
	Owner->AddInstanceComponent(Root);
	Owner->SetActorLocation(AnchorWorld);

	UInstancedStaticMeshComponent* Ism =
		NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("ISM"), RF_Transient);
	Ism->SetupAttachment(Root);
	Ism->SetMobility(EComponentMobility::Movable);
	Ism->SetStaticMesh(Mesh);
	PrepareMeshMaterialsForIsm(Mesh, Ism);
	Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ism->SetCastShadow(true);
	Ism->SetVisibility(true);
	Ism->SetHiddenInGame(false);
	Ism->bNeverDistanceCull = true;
	// Large end distance — never fade. (0,0 can be treated as "cull immediately" on some paths.)
	Ism->SetCullDistances(0, 5000000);
	Ism->RegisterComponent();
	Ism->SetWorldLocation(AnchorWorld);
	Owner->AddInstanceComponent(Ism);

	IsmByMesh.Add(MeshId, Owner);
	UE_LOG(LogSailSim, Log,
		TEXT("EncAid: ISM SM_Buoy_%d at (%.0f,%.0f) mats=%d actor=%s"),
		MeshId, AnchorWorld.X, AnchorWorld.Y, Mesh->GetStaticMaterials().Num(), *GetNameSafe(Owner));
	return Ism;
}

UInstancedStaticMeshComponent* UEncAidSubsystem::EnsureMooringIsm(
	int32 MeshId, int32 StyleIndex, const FVector& AnchorWorld)
{
	EnsureMooringColorMaterials();
	const int32 Key = MooringIsmKey(StyleIndex, MeshId);
	if (TObjectPtr<AActor>* Found = IsmByMooring.Find(Key))
	{
		if (IsValid(*Found))
		{
			(*Found)->SetActorLocation(AnchorWorld);
			if (UInstancedStaticMeshComponent* I =
					(*Found)->FindComponentByClass<UInstancedStaticMeshComponent>())
			{
				I->SetWorldLocation(AnchorWorld);
				return I;
			}
		}
		else
		{
			IsmByMooring.Remove(Key);
		}
	}

	UWorld* World = GetWorld();
	if (!World) return nullptr;
	// Moorings only use the round floating balls (1 / 2).
	const int32 BallMesh = (MeshId == 2) ? 2 : 1;
	UStaticMesh* Mesh = LoadBuoyMesh(BallMesh);
	if (!Mesh)
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: missing mooring mesh SM_Buoy_%d"), BallMesh);
		return nullptr;
	}

	FActorSpawnParameters Sp;
	Sp.ObjectFlags = RF_Transient;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Owner = World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform(FRotator::ZeroRotator, AnchorWorld), Sp);
	if (!Owner) return nullptr;
#if WITH_EDITOR
	Owner->SetActorLabel(FString::Printf(TEXT("EncAid_Mooring_s%d_m%d"), StyleIndex, BallMesh));
#endif
	Owner->Tags.Add(FName(TEXT("EncAid")));
	Owner->Tags.Add(FName(TEXT("MooringBall")));
	Owner->SetActorEnableCollision(false);

	USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("Root"), RF_Transient);
	Root->SetMobility(EComponentMobility::Movable);
	Owner->SetRootComponent(Root);
	Root->SetWorldLocation(AnchorWorld);
	Root->RegisterComponent();
	Owner->AddInstanceComponent(Root);

	UInstancedStaticMeshComponent* Ism =
		NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("ISM"), RF_Transient);
	Ism->SetupAttachment(Root);
	Ism->SetMobility(EComponentMobility::Movable);
	// Dynamic yacht MIDs often lack Nanite usage → DefaultMaterial (dark gray).
	// ~300 mooring balls are fine without Nanite; colors + clearcoat need the raster path.
	Ism->bDisallowNanite = true;
	Ism->SetStaticMesh(Mesh);
	// Shiny hull-paint style (plain / bottom stripe / top cap) — not pack textures.
	UMaterialInterface* Mat = nullptr;
	if (MooringColorMats.IsValidIndex(StyleIndex) && MooringColorMats[StyleIndex])
	{
		Mat = MooringColorMats[StyleIndex].Get();
	}
	else if (MooringColorMats.Num() > 0)
	{
		Mat = MooringColorMats[0].Get();
	}
	if (Mat)
	{
		EncAidMatUtil::ForceIsmUsage(Mat);
		const int32 NumSlots = FMath::Max(1, Mesh->GetStaticMaterials().Num());
		for (int32 Mi = 0; Mi < NumSlots; ++Mi)
		{
			Ism->SetMaterial(Mi, Mat);
		}
	}
	else
	{
		PrepareMeshMaterialsForIsm(Mesh, Ism);
	}
	Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ism->SetCastShadow(true);
	Ism->SetVisibility(true);
	Ism->SetHiddenInGame(false);
	Ism->bNeverDistanceCull = true;
	Ism->SetCullDistances(0, 5000000);
	Ism->RegisterComponent();
	Ism->SetWorldLocation(AnchorWorld);
	Owner->AddInstanceComponent(Ism);

	IsmByMooring.Add(Key, Owner);
	return Ism;
}

void UEncAidSubsystem::ClearInstancesKeepActors()
{
	auto ClearMap = [](TMap<int32, TObjectPtr<AActor>>& Map, const FVector& Anchor)
	{
		for (auto& Pair : Map)
		{
			if (!IsValid(Pair.Value)) continue;
			Pair.Value->SetActorLocation(Anchor);
			if (UInstancedStaticMeshComponent* I =
					Pair.Value->FindComponentByClass<UInstancedStaticMeshComponent>())
			{
				I->ClearInstances();
				I->SetWorldLocation(Anchor);
			}
		}
	};
	ClearMap(IsmByMesh, IsmAnchor);
	ClearMap(IsmByMooring, IsmAnchor);
}

void UEncAidSubsystem::ClearAllInstances()
{
	auto DestroyMap = [](TMap<int32, TObjectPtr<AActor>>& Map)
	{
		for (auto& Pair : Map)
		{
			if (IsValid(Pair.Value))
			{
				if (UInstancedStaticMeshComponent* I =
						Pair.Value->FindComponentByClass<UInstancedStaticMeshComponent>())
				{
					I->ClearInstances();
				}
				Pair.Value->Destroy();
			}
		}
		Map.Reset();
	};
	DestroyMap(IsmByMesh);
	DestroyMap(IsmByMooring);
	Resident.Reset();
	VisibleCount = 0;
	bAnchorValid = false;
	DeferredRepassLeft = 0;
	DeferredRepassTimer = 0.f;
}

FVector UEncAidSubsystem::GetFocusLocation() const
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

void UEncAidSubsystem::ForceStreamAround(FVector FocusWorld)
{
	if (!bEnabled)
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: ForceStreamAround skipped (disabled)"));
		return;
	}
	if (!bDataLoaded && !ReloadData())
	{
		UE_LOG(LogSailSim, Warning, TEXT("EncAid: ForceStreamAround skipped (no data)"));
		return;
	}
	// Cold-start: materials may compile for ISM a beat later — re-place twice after first stream.
	DeferredRepassLeft = 2;
	DeferredRepassTimer = 0.35f;
	RebuildAround(FocusWorld, /*bForceRebuild*/ true);
}

void UEncAidSubsystem::RebuildAround(const FVector& Focus, bool bForceRebuild)
{
	LastFocus = Focus;
	if (!bDataLoaded || Aids.Num() == 0) return;

	const float LoadR2 = LoadRadiusCm * LoadRadiusCm;
	const float UnloadR2 = UnloadRadiusCm * UnloadRadiusCm;
	const FVector2D F2(Focus.X, Focus.Y);

	// Fixed anchor near first focus — do not chase the boat every tick (flicker).
	if (!bAnchorValid || bForceRebuild)
	{
		IsmAnchor = FVector(Focus.X, Focus.Y, WaterlineOffsetCm);
		bAnchorValid = true;
	}

	TSet<int32> Want;
	Want.Reserve(Aids.Num());
	for (int32 I = 0; I < Aids.Num(); ++I)
	{
		const FVector2D P(Aids[I].WorldCm.X, Aids[I].WorldCm.Y);
		const float D2 = FVector2D::DistSquared(F2, P);
		if (Resident.Contains(I))
		{
			if (D2 <= UnloadR2) Want.Add(I);
		}
		else if (D2 <= LoadR2)
		{
			Want.Add(I);
		}
	}

	if (!bForceRebuild && Want.Num() == Resident.Num() && Resident.Num() > 0)
	{
		bool bSame = true;
		for (int32 I : Want)
		{
			if (!Resident.Contains(I)) { bSame = false; break; }
		}
		if (bSame) return;
	}

	// Clear instances; keep actors at fixed anchor (navaids + coloured moorings).
	ClearInstancesKeepActors();
	if (bColorfulMooringBalls)
	{
		EnsureMooringColorMaterials();
	}

	int32 Placed = 0;
	int32 PlacedMoorings = 0;
	int32 Mesh1 = 0, Mesh2 = 0;
	float NearestCm = TNumericLimits<float>::Max();

	for (int32 I : Want)
	{
		const FEncAidDesc& A = Aids[I];
		const bool bMooring = A.Kind.Equals(TEXT("mooring_point"), ESearchCase::IgnoreCase);

		UInstancedStaticMeshComponent* Comp = nullptr;
		if (bMooring && bColorfulMooringBalls)
		{
			const int32 StyleIdx = PickMooringStyleIndex(I, A);
			Comp = EnsureMooringIsm(A.MeshId, StyleIdx, IsmAnchor);
		}
		else
		{
			Comp = EnsureIsm(A.MeshId, IsmAnchor);
		}
		if (!Comp) continue;

		const float S = bMooring ? MooringScale : MeshScale;
		const float Z = WaterlineOffsetCm + (bMooring ? MooringZOffsetCm : 0.f);

		// World-space instances: unambiguous even if component transform updates late.
		FTransform Xf;
		Xf.SetLocation(FVector(A.WorldCm.X, A.WorldCm.Y, Z));
		Xf.SetRotation(FQuat::Identity);
		Xf.SetScale3D(FVector(S));
		Comp->AddInstance(Xf, /*bWorldSpace*/ true);
		++Placed;
		if (bMooring)
		{
			++PlacedMoorings;
			if (A.MeshId == 1) ++Mesh1;
			else if (A.MeshId == 2) ++Mesh2;
		}

		const float D = FVector2D::Distance(F2, FVector2D(A.WorldCm.X, A.WorldCm.Y));
		NearestCm = FMath::Min(NearestCm, D);
	}

	int32 InstanceSum = 0;
	auto FinishMap = [&](TMap<int32, TObjectPtr<AActor>>& Map)
	{
		for (auto& Pair : Map)
		{
			if (!IsValid(Pair.Value)) continue;
			if (UInstancedStaticMeshComponent* I =
					Pair.Value->FindComponentByClass<UInstancedStaticMeshComponent>())
			{
				InstanceSum += I->GetInstanceCount();
				if (I->GetInstanceCount() > 0)
				{
					I->MarkRenderStateDirty();
					I->UpdateBounds();
					I->SetVisibility(true);
					I->SetHiddenInGame(false);
				}
			}
			Pair.Value->SetActorHiddenInGame(false);
		}
	};
	FinishMap(IsmByMesh);
	FinishMap(IsmByMooring);

	Resident = MoveTemp(Want);
	VisibleCount = Placed;
	UE_LOG(LogSailSim, Log,
		TEXT("EncAid: placed %d/%d aids (%d moorings m1=%d m2=%d colors=%d instSum=%d near (%.0f,%.0f) nearest=%.0fm force=%d)"),
		Placed, Aids.Num(), PlacedMoorings, Mesh1, Mesh2, MooringColorMats.Num(), InstanceSum,
		Focus.X, Focus.Y,
		NearestCm < TNumericLimits<float>::Max() ? NearestCm * 0.01f : -1.f,
		bForceRebuild ? 1 : 0);
}

void UEncAidSubsystem::Tick(float DeltaTime)
{
	if (!bEnabled || !bDataLoaded) return;
	UWorld* World = GetWorld();
	if (!World || World->IsPreviewWorld()) return;

	SAIL_PERF_SCOPE(Aids);
	FSailSimPerf::Get().AidCount = Resident.Num();

	// Cold-start re-pass: ISM material shaders often finish compiling a moment after first place.
	if (DeferredRepassLeft > 0)
	{
		DeferredRepassTimer -= DeltaTime;
		if (DeferredRepassTimer <= 0.f)
		{
			--DeferredRepassLeft;
			DeferredRepassTimer = 0.75f;
			const FVector Focus = GetFocusLocation();
			if (!Focus.IsNearlyZero() || !LastFocus.IsNearlyZero())
			{
				UE_LOG(LogSailSim, Log, TEXT("EncAid: deferred re-place (%d left)"), DeferredRepassLeft);
				RebuildAround(Focus.IsNearlyZero() ? LastFocus : Focus, /*bForceRebuild*/ true);
			}
		}
	}

	Accum += DeltaTime;
	if (Accum < UpdateIntervalSec) return;
	Accum = 0.f;

	const FVector Focus = GetFocusLocation();
	if (Focus.IsNearlyZero() && LastFocus.IsNearlyZero()) return;
	RebuildAround(Focus.IsNearlyZero() ? LastFocus : Focus, /*bForceRebuild*/ false);
}
