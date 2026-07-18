#include "Sailing/SailVisualExtras.h"
#include "Sailing/SailClothSim.h"
#include "SailSimUE.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "Math/UnrealMathUtility.h"

namespace SailVisualLocal
{
	// J/105 class 6.4.3: four main battens divide leech into five equal parts
	// → stations at 1/5, 2/5, 3/5, 4/5 of leech (foot→head).
	static const TArray<float> MainBattenV = { 0.20f, 0.40f, 0.60f, 0.80f };
	static const TArray<float> JibBattenV = { 0.30f, 0.55f, 0.78f };

	// Main rod ~0.20" → cm; jib leech battens thinner
	static constexpr float MainBattenRadCm = 0.55f;
	static constexpr float JibBattenRadCm = 0.38f;

	// Jib body stations (web hUpdateJibBodyTellTales)
	struct FJibStation { float V; float U; };
	static const FJibStation JibStations[] = {
		{ 0.30f, 0.40f },
		{ 0.48f, 0.36f },
		{ 0.65f, 0.32f },
	};

	// Telltales: web physics (length/flutter) but thicker tubes for UE chase-cam
	// distance. Web RAD ~0.02 ft is sub-cm at full boat scale and reads as dust.
	static constexpr int32 NPts = 8;
	static constexpr int32 TubeRadial = 6;               // smoother silhouette than web's 4
	static constexpr float SegCm = 0.32f * 30.48f;       // slightly longer streamer (~2.2 ft)
	static constexpr float YarnLenCm = 1.35f * 30.48f;
	// ~1"–1.2" diameter yarns — readable from the cockpit chase cam
	static constexpr float MainTaleRadCm = 1.55f;
	static constexpr float JibLeechTaleRadCm = 1.35f;
	static constexpr float JibBodyTaleRadCm = 1.25f;
	static constexpr float JibRestHCm = 0.06f * 30.48f;
	static constexpr float JibStallLiftCm = 0.28f * 30.48f;
}

/** Prefer unlit so TAA/Lumen don't dither thin dynamic yarns into pixels. */
static UMaterialInterface* LoadUnlitBase()
{
	const TCHAR* Paths[] = {
		TEXT("/Engine/EngineMaterials/UnlitColor.UnlitColor"),
		TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"),
		TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque.DefaultTextMaterialOpaque"),
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
	};
	for (const TCHAR* P : Paths)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, P))
		{
			return M;
		}
	}
	return nullptr;
}

void FSailVisualExtras::Clear()
{
	MainLeechTales.Reset();
	JibLeechTales.Reset();
	JibBodyTales.Reset();
	MainBattenRods.Reset();
	JibBattenRods.Reset();
	LogoPatch = FMarkPatch();
	NumberPatch = FMarkPatch();
	MarkingsTex = nullptr;
	BattenMat = nullptr;
	MarkBaseMat = nullptr;
	bBuilt = false;
	Phase = 0.f;
}

UProceduralMeshComponent* FSailVisualExtras::MakeChildMesh(UProceduralMeshComponent* Parent, const FName& Name)
{
	if (!Parent) return nullptr;
	// ProceduralMesh is classic raster — never Nanite. Pixelation on moving yarns
	// is TAA/TSR history noise + motion blur on thin dynamic verts, not Nanite.
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(Parent, Name);
	M->SetupAttachment(Parent);
	M->RegisterComponent();
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	M->bCastDynamicShadow = false;
	M->bCastContactShadow = false;
	M->SetReceivesDecals(false);
	M->SetGenerateOverlapEvents(false);
	M->bUseAsyncCooking = false;
	M->bNeverDistanceCull = true;
	M->SetCullDistance(0.f);
	M->SetBoundsScale(12.f);
	// No shadows / decals — keeps thin dynamic yarns out of VSM noise.
	// Motion blur is disabled project-wide (PMC verts update without prior-frame
	// positions → TAA history would crawl otherwise).
	M->SetRelativeLocation(FVector::ZeroVector);
	M->SetRelativeRotation(FRotator::ZeroRotator);
	M->SetRelativeScale3D(FVector::OneVector);
	return M;
}

UMaterialInterface* FSailVisualExtras::MakeSolidMat(FLinearColor Color, float Emissive)
{
	// Unlit + strong emissive: solid colour every frame (web yarns read this way;
	// lit PBR + TAA on thin tubes → crawling pixel edges).
	UMaterialInterface* Base = LoadUnlitBase();
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR_TwoSided.M_Yacht_PBR_TwoSided"));
	}
	if (!Base) return nullptr;
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return Base;
	const float Em = FMath::Max(1.f, Emissive);
	// Bright base + emissive so yarns stay saturated on overcast / Lumen scenes
	const FLinearColor Bright = Color * 1.15f;
	const FLinearColor EmCol = Color * Em;
	Mid->SetVectorParameterValue(TEXT("Color"), Bright);
	Mid->SetVectorParameterValue(TEXT("BaseColor"), Bright);
	Mid->SetVectorParameterValue(TEXT("EmissiveColor"), EmCol);
	Mid->SetVectorParameterValue(TEXT("Emissive"), EmCol);
	Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), Em);
	Mid->SetScalarParameterValue(TEXT("Emissive"), Em);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 1.f);
	Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	return Mid;
}

UMaterialInstanceDynamic* FSailVisualExtras::MakeMarkingsMat(UTexture2D* Tex)
{
	if (!Tex) return nullptr;
	// MUST be alpha-masked: the atlas is navy ink on transparent. Opaque/emissive
	// engine mats either ignore alpha (solid quad) or fail to bind SlateUI and
	// draw nothing — that hid the sail number after the TAA experiment.
	// Widget3DPassThrough_Masked is the path that actually showed numbers.
	const TCHAR* Paths[] = {
		TEXT("/Engine/EngineMaterials/Widget3DPassThrough_Masked.Widget3DPassThrough_Masked"),
		TEXT("/Engine/EngineMaterials/DefaultTextMaterialTranslucent.DefaultTextMaterialTranslucent"),
		TEXT("/Engine/EngineMaterials/AntiAliasedTextMaterialTranslucent.AntiAliasedTextMaterialTranslucent"),
		TEXT("/Engine/EngineMaterials/EmissiveTexturedMaterial.EmissiveTexturedMaterial"),
		TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
	};
	UMaterialInterface* Base = nullptr;
	FString UsedPath;
	for (const TCHAR* P : Paths)
	{
		Base = LoadObject<UMaterialInterface>(nullptr, P);
		if (Base)
		{
			UsedPath = P;
			break;
		}
	}
	if (!Base)
	{
		UE_LOG(LogSailSim, Warning, TEXT("SailVisualExtras: no markings base material"));
		return nullptr;
	}
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return nullptr;
	// Widget / text mats sample SlateUI; also set common aliases.
	Mid->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	Mid->SetTextureParameterValue(TEXT("Texture"), Tex);
	Mid->SetTextureParameterValue(TEXT("BaseColorTexture"), Tex);
	Mid->SetTextureParameterValue(TEXT("Diffuse"), Tex);
	Mid->SetTextureParameterValue(TEXT("BaseTexture"), Tex);
	Mid->SetTextureParameterValue(TEXT("MainTex"), Tex);
	Mid->SetTextureParameterValue(TEXT("FontTexture"), Tex);
	Mid->SetTextureParameterValue(TEXT("EmissiveTexture"), Tex);
	// White tint — atlas already has J/105 blue + navy vinyl.
	Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
	Mid->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor::White);
	Mid->SetVectorParameterValue(TEXT("TintColor"), FLinearColor::White);
	Mid->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Black);
	Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	// Soft enough to keep thin glyph stems; hard enough to drop clear background.
	// (0.35 was clipping soft anti-aliased digit edges away entirely.)
	Mid->SetScalarParameterValue(TEXT("OpacityMaskClipValue"), 0.12f);
	UE_LOG(LogSailSim, Log, TEXT("SailVisualExtras: markings mat %s tex=%dx%d"),
		*UsedPath, Tex->GetSizeX(), Tex->GetSizeY());
	return Mid;
}

UTexture2D* FSailVisualExtras::LoadMarkingsTexture()
{
	if (MarkingsTex) return MarkingsTex;

	// Prefer project Content/Data/sails/sail_markings.png (baked RRS G1 plate, 2048²).
	const FString Rel = TEXT("Data/sails/sail_markings.png");
	FString Path = FPaths::ProjectContentDir() / Rel;
	if (!FPaths::FileExists(Path))
	{
		// Dev fallback: web frontend texture tree
		Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("../sail-sim/frontend/public/textures/sails/j105-class-insignia.png"));
	}
	if (!FPaths::FileExists(Path))
	{
		UE_LOG(LogSailSim, Warning, TEXT("SailVisualExtras: markings PNG not found"));
		return nullptr;
	}

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *Path)) return nullptr;

	IImageWrapperModule& ImgMod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = ImgMod.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileData.GetData(), FileData.Num())) return nullptr;

	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw)) return nullptr;

	const int32 W = Wrapper->GetWidth();
	const int32 H = Wrapper->GetHeight();
	// Build a full mip chain so distant sails stay sharp (web LinearMipmapLinearFilter).
	const int32 NumMips = FMath::Max(1, int32(FMath::FloorLog2(uint32(FMath::Max(W, H)))) + 1);
	UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!Tex) return nullptr;
	Tex->SetPlatformData(new FTexturePlatformData());
	Tex->GetPlatformData()->SizeX = W;
	Tex->GetPlatformData()->SizeY = H;
	Tex->GetPlatformData()->PixelFormat = PF_B8G8R8A8;

	TArray64<uint8> Level = MoveTemp(Raw);
	int32 LevelW = W;
	int32 LevelH = H;
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		FTexture2DMipMap* MipMap = new FTexture2DMipMap();
		Tex->GetPlatformData()->Mips.Add(MipMap);
		MipMap->SizeX = LevelW;
		MipMap->SizeY = LevelH;
		MipMap->BulkData.Lock(LOCK_READ_WRITE);
		void* Dest = MipMap->BulkData.Realloc(Level.Num());
		FMemory::Memcpy(Dest, Level.GetData(), Level.Num());
		MipMap->BulkData.Unlock();

		if (Mip + 1 >= NumMips || LevelW <= 1 || LevelH <= 1) break;
		const int32 NextW = FMath::Max(1, LevelW / 2);
		const int32 NextH = FMath::Max(1, LevelH / 2);
		TArray64<uint8> Next;
		Next.SetNumUninitialized(int64(NextW) * NextH * 4);
		for (int32 Y = 0; Y < NextH; ++Y)
		{
			for (int32 X = 0; X < NextW; ++X)
			{
				// Box filter (BGRA). Preserve alpha so glyph edges don't fatten.
				int32 Acc[4] = {0, 0, 0, 0};
				int32 N = 0;
				for (int32 Oy = 0; Oy < 2; ++Oy)
				{
					for (int32 Ox = 0; Ox < 2; ++Ox)
					{
						const int32 Sx = FMath::Min(LevelW - 1, X * 2 + Ox);
						const int32 Sy = FMath::Min(LevelH - 1, Y * 2 + Oy);
						const int64 Off = (int64(Sy) * LevelW + Sx) * 4;
						Acc[0] += Level[Off + 0];
						Acc[1] += Level[Off + 1];
						Acc[2] += Level[Off + 2];
						Acc[3] += Level[Off + 3];
						++N;
					}
				}
				const int64 DOff = (int64(Y) * NextW + X) * 4;
				Next[DOff + 0] = uint8(Acc[0] / N);
				Next[DOff + 1] = uint8(Acc[1] / N);
				Next[DOff + 2] = uint8(Acc[2] / N);
				Next[DOff + 3] = uint8(Acc[3] / N);
			}
		}
		Level = MoveTemp(Next);
		LevelW = NextW;
		LevelH = NextH;
	}

	Tex->SRGB = true;
	Tex->CompressionSettings = TC_EditorIcon; // keep alpha, no BC blockiness on glyphs
	// PlatformData already has our mip chain; NoMipmaps avoids regenerating over it.
	Tex->MipGenSettings = TMGS_NoMipmaps;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->Filter = TF_Trilinear;
	Tex->LODGroup = TEXTUREGROUP_UI;
	Tex->NeverStream = true;
	Tex->UpdateResource();
	MarkingsTex = Tex;

	// Force material rebinds when atlas reloads
	LogoPatch.Mid = nullptr;
	NumberPatch.Mid = nullptr;
	UE_LOG(LogSailSim, Log, TEXT("SailVisualExtras: loaded markings %dx%d mips=%d from %s"),
		W, H, Tex->GetPlatformData()->Mips.Num(), *Path);
	return MarkingsTex;
}

void FSailVisualExtras::EnsureLeechTales(
	UProceduralMeshComponent* Sail,
	TArray<FTubeTale>& Out,
	int32 Count,
	UMaterialInterface* Mat)
{
	if (!Sail) return;
	while (Out.Num() < Count)
	{
		const int32 Idx = Out.Num();
		FTubeTale T;
		T.Mesh = MakeChildMesh(Sail, *FString::Printf(TEXT("LeechTale_%d"), Idx));
		if (T.Mesh && Mat) T.Mesh->SetMaterial(0, Mat);
		T.Pts.SetNum(SailVisualLocal::NPts);
		Out.Add(MoveTemp(T));
	}
	for (int32 I = 0; I < Out.Num(); ++I)
	{
		if (Out[I].Mesh)
		{
			Out[I].Mesh->SetVisibility(I < Count && bTellTales);
			Out[I].Mesh->SetHiddenInGame(!(I < Count && bTellTales));
		}
	}
}

void FSailVisualExtras::EnsureJibBodyTales(UProceduralMeshComponent* Sail)
{
	if (!Sail) return;
	const int32 N = UE_ARRAY_COUNT(SailVisualLocal::JibStations) * 2;
	while (JibBodyTales.Num() < N)
	{
		const int32 Idx = JibBodyTales.Num();
		FTubeTale T;
		T.Mesh = MakeChildMesh(Sail, *FString::Printf(TEXT("JibBodyTale_%d"), Idx));
		UMaterialInterface* Mat = (Idx % 2 == 0) ? YarnMatRed : YarnMatGreen;
		if (T.Mesh && Mat) T.Mesh->SetMaterial(0, Mat);
		T.Pts.SetNum(SailVisualLocal::NPts);
		JibBodyTales.Add(MoveTemp(T));
	}
}

void FSailVisualExtras::EnsureMarkPatches(UProceduralMeshComponent* MainSail)
{
	if (!MainSail || !bSailMarkings) return;
	UTexture2D* Tex = LoadMarkingsTexture();
	if (!Tex) return;

	auto InitPatch = [&](FMarkPatch& P, const TCHAR* Name, float Cu, float Cv, float Du, float Dv,
		FVector2D TMin, FVector2D TMax, int32 GridU, int32 GridV)
	{
		if (!P.Mesh)
		{
			P.Mesh = MakeChildMesh(MainSail, Name);
			if (P.Mesh)
			{
				P.Mesh->SetCastShadow(false);
				P.Mesh->SetTranslucentSortPriority(20);
			}
		}
		const bool bLayoutChanged =
			P.Cu != Cu || P.Cv != Cv || P.Du != Du || P.Dv != Dv
			|| P.GridU != GridU || P.GridV != GridV
			|| !P.TexMin.Equals(TMin) || !P.TexMax.Equals(TMax);
		P.Cu = Cu; P.Cv = Cv; P.Du = Du; P.Dv = Dv;
		P.TexMin = TMin; P.TexMax = TMax;
		P.GridU = GridU; P.GridV = GridV;
		if (bLayoutChanged)
		{
			P.CachedVertCount = 0; // force CreateMeshSection once
		}
		// Always (re)bind masked atlas mat — live coding / bad prior MID left numbers blank.
		if (P.Mesh)
		{
			P.Mid = MakeMarkingsMat(Tex);
			if (P.Mid)
			{
				P.Mesh->SetMaterial(0, P.Mid);
				P.Mesh->SetVisibility(true);
				P.Mesh->SetHiddenInGame(false);
			}
			else
			{
				UE_LOG(LogSailSim, Warning, TEXT("SailVisualExtras: MakeMarkingsMat failed for %s"), Name);
			}
		}
	};

	// Web physical placement (class rules + RRS G1):
	//   logo centre ~v 0.70, numbers centre ~v 0.48
	// Atlas: left half = J/105 insignia, right half = USA / digits
	// Grid density only needs to follow sail curvature — glyph detail is in the 2048² texture.
	InitPatch(LogoPatch, TEXT("SailLogo"), 0.50f, 0.70f, 0.28f, 0.14f,
		FVector2D(0.02f, 0.05f), FVector2D(0.48f, 0.95f), 28, 40);
	InitPatch(NumberPatch, TEXT("SailNumber"), 0.50f, 0.48f, 0.78f, 0.18f,
		FVector2D(0.52f, 0.30f), FVector2D(0.96f, 0.70f), 48, 28);
}

void FSailVisualExtras::EnsureBuilt(UProceduralMeshComponent* MainSail, UProceduralMeshComponent* JibSail)
{
	if (!bEnabled) return;
	MainSailW = MainSail;
	JibSailW = JibSail;

	// Saturated unlit yarns — bright enough from the chase cam
	YarnMatOrange = MakeSolidMat(FLinearColor::FromSRGBColor(FColor(0xff, 0x8a, 0x28)), 1.6f);
	YarnMatRed = MakeSolidMat(FLinearColor::FromSRGBColor(FColor(0xff, 0x2a, 0x2a)), 1.7f);
	YarnMatGreen = MakeSolidMat(FLinearColor::FromSRGBColor(FColor(0x28, 0xe0, 0x58)), 1.7f);

	// Dark carbon/fibreglass batten look (web 0x20242a)
	BattenMat = MakeSolidMat(FLinearColor::FromSRGBColor(FColor(0x28, 0x2c, 0x32)), 0.05f);

	if (bTellTales)
	{
		EnsureLeechTales(MainSail, MainLeechTales, SailVisualLocal::MainBattenV.Num(), YarnMatOrange);
		EnsureLeechTales(JibSail, JibLeechTales, SailVisualLocal::JibBattenV.Num(), YarnMatOrange);
		EnsureJibBodyTales(JibSail);
		// Re-bind mats on already-built meshes (live coding / rebuild)
		for (FTubeTale& T : MainLeechTales)
		{
			if (T.Mesh) T.Mesh->SetMaterial(0, YarnMatOrange);
		}
		for (FTubeTale& T : JibLeechTales)
		{
			if (T.Mesh) T.Mesh->SetMaterial(0, YarnMatOrange);
		}
		for (int32 I = 0; I < JibBodyTales.Num(); ++I)
		{
			if (JibBodyTales[I].Mesh)
			{
				JibBodyTales[I].Mesh->SetMaterial(0, (I % 2 == 0) ? YarnMatRed : YarnMatGreen);
			}
		}
	}
	if (bBattenRods)
	{
		EnsureBattenRods(MainSail, MainBattenRods, SailVisualLocal::MainBattenV.Num());
		EnsureBattenRods(JibSail, JibBattenRods, SailVisualLocal::JibBattenV.Num());
		for (FBattenRod& R : MainBattenRods)
		{
			if (R.Mesh) R.Mesh->SetMaterial(0, BattenMat);
		}
		for (FBattenRod& R : JibBattenRods)
		{
			if (R.Mesh) R.Mesh->SetMaterial(0, BattenMat);
		}
	}
	if (bSailMarkings)
	{
		EnsureMarkPatches(MainSail);
	}
	bBuilt = true;
}

void FSailVisualExtras::EnsureBattenRods(
	UProceduralMeshComponent* Sail,
	TArray<FBattenRod>& Out,
	int32 Count)
{
	if (!Sail || Count <= 0) return;
	while (Out.Num() < Count)
	{
		const int32 Idx = Out.Num();
		FBattenRod R;
		const FName Name(*FString::Printf(TEXT("BattenRod_%s_%d"), *Sail->GetName(), Idx));
		R.Mesh = MakeChildMesh(Sail, Name);
		if (R.Mesh)
		{
			R.Mesh->SetMaterial(0, BattenMat);
			R.Mesh->SetCastShadow(false);
		}
		Out.Add(MoveTemp(R));
	}
}

FVector FSailVisualExtras::SampleCloth(
	const FSailClothSim& Cloth,
	float V01,
	float U01,
	FVector* OutNormal)
{
	if (!Cloth.bInitialized || !Cloth.bGridTopology || Cloth.Nu < 2 || Cloth.Nw < 2)
	{
		if (OutNormal) *OutNormal = FVector(0.f, 1.f, 0.f);
		return FVector::ZeroVector;
	}
	const float Fi = FMath::Clamp(V01, 0.f, 1.f) * float(Cloth.Nu - 1);
	const float Fj = FMath::Clamp(U01, 0.f, 1.f) * float(Cloth.Nw - 1);
	const int32 I0 = FMath::Clamp(int32(Fi), 0, Cloth.Nu - 2);
	const int32 J0 = FMath::Clamp(int32(Fj), 0, Cloth.Nw - 2);
	const float Ti = Fi - float(I0);
	const float Tj = Fj - float(J0);
	auto P = [&](int32 I, int32 J) -> FVector
	{
		const int32 Idx = Cloth.GridIdx(I, J);
		return Cloth.Pos.IsValidIndex(Idx) ? Cloth.Pos[Idx] : FVector::ZeroVector;
	};
	auto N = [&](int32 I, int32 J) -> FVector
	{
		const int32 Idx = Cloth.GridIdx(I, J);
		FVector Nr = Cloth.Normals.IsValidIndex(Idx) ? Cloth.Normals[Idx] : FVector(0.f, 1.f, 0.f);
		if (!Nr.Normalize()) Nr = FVector(0.f, 1.f, 0.f);
		return Nr;
	};
	const FVector P00 = P(I0, J0), P10 = P(I0 + 1, J0), P01 = P(I0, J0 + 1), P11 = P(I0 + 1, J0 + 1);
	const FVector Pos =
		FMath::Lerp(FMath::Lerp(P00, P10, Ti), FMath::Lerp(P01, P11, Ti), Tj);
	if (OutNormal)
	{
		const FVector N00 = N(I0, J0), N10 = N(I0 + 1, J0), N01 = N(I0, J0 + 1), N11 = N(I0 + 1, J0 + 1);
		FVector Nr = FMath::Lerp(FMath::Lerp(N00, N10, Ti), FMath::Lerp(N01, N11, Ti), Tj);
		if (!Nr.Normalize()) Nr = FVector(0.f, 1.f, 0.f);
		*OutNormal = Nr;
	}
	return Pos;
}

void FSailVisualExtras::BuildYarnTubeMesh(
	UProceduralMeshComponent* Mesh,
	const TArray<FVector>& Centerline,
	float RadiusCm,
	UMaterialInterface* Mat,
	float ForwardThinScale,
	float AftThickScale)
{
	// Round tube like web THREE.TubeGeometry(curve, tubular, radius, radial=4).
	if (!Mesh || Centerline.Num() < 2 || RadiusCm < 0.01f) return;
	const int32 Ns = Centerline.Num();
	const int32 Radial = SailVisualLocal::TubeRadial;
	const int32 Ring = Radial; // verts per ring (no duplicate seam — close with tris)

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Norms;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Cols;
	TArray<FProcMeshTangent> Tans;
	Verts.Reserve(Ns * Ring);
	Norms.Reserve(Ns * Ring);

	// Parallel-transport frame so the tube doesn't spin along the path
	FVector PrevN = FVector::ZeroVector;
	for (int32 S = 0; S < Ns; ++S)
	{
		const FVector C = Centerline[S];
		FVector T = (S + 1 < Ns) ? (Centerline[S + 1] - C) : (C - Centerline[S - 1]);
		if (!T.Normalize()) T = FVector(1.f, 0.f, 0.f);

		FVector N = FVector::CrossProduct(T, FVector::UpVector);
		if (N.SizeSquared() < 1e-6f) N = FVector::CrossProduct(T, FVector::RightVector);
		N.Normalize();
		if (!PrevN.IsNearlyZero() && FVector::DotProduct(N, PrevN) < 0.f) N = -N;
		if (!PrevN.IsNearlyZero())
		{
			const FVector NBlend = (PrevN - T * FVector::DotProduct(PrevN, T)).GetSafeNormal();
			if (!NBlend.IsNearlyZero()) N = (N * 0.35f + NBlend * 0.65f).GetSafeNormal();
		}
		PrevN = N;
		const FVector B = FVector::CrossProduct(T, N).GetSafeNormal();

		const float Along = float(S) / float(Ns - 1);
		// Yarns: thick root → thin free end. Battens: thin forward → full at leech.
		const float R = RadiusCm * FMath::Lerp(ForwardThinScale, AftThickScale, Along);

		for (int32 K = 0; K < Radial; ++K)
		{
			const float Ang = (2.f * PI * float(K)) / float(Radial);
			const FVector Off = N * (FMath::Cos(Ang) * R) + B * (FMath::Sin(Ang) * R);
			const FVector OutN = Off.GetSafeNormal();
			Verts.Add(C + Off);
			Norms.Add(OutN.IsNearlyZero() ? N : OutN);
			UV.Add(FVector2D(float(K) / float(Radial), Along));
			Cols.Add(FLinearColor::White);
			Tans.Add(FProcMeshTangent(T, false));
		}
	}

	for (int32 S = 0; S < Ns - 1; ++S)
	{
		for (int32 K = 0; K < Radial; ++K)
		{
			const int32 K1 = (K + 1) % Radial;
			const int32 I0 = S * Ring + K;
			const int32 I1 = S * Ring + K1;
			const int32 I2 = (S + 1) * Ring + K;
			const int32 I3 = (S + 1) * Ring + K1;
			// Outward-facing winding
			Tris.Add(I0); Tris.Add(I2); Tris.Add(I1);
			Tris.Add(I1); Tris.Add(I2); Tris.Add(I3);
		}
	}
	// End caps
	{
		const int32 Root = 0;
		for (int32 K = 1; K + 1 < Radial; ++K)
		{
			Tris.Add(Root); Tris.Add(Root + K); Tris.Add(Root + K + 1);
		}
		const int32 Tip = (Ns - 1) * Ring;
		for (int32 K = 1; K + 1 < Radial; ++K)
		{
			Tris.Add(Tip); Tris.Add(Tip + K + 1); Tris.Add(Tip + K);
		}
	}

	if (Mesh->GetNumSections() == 0)
	{
		Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UV, Cols, Tans, false);
	}
	else
	{
		if (Mesh->GetProcMeshSection(0)
			&& Mesh->GetProcMeshSection(0)->ProcVertexBuffer.Num() == Verts.Num())
		{
			Mesh->UpdateMeshSection_LinearColor(0, Verts, Norms, UV, Cols, Tans);
		}
		else
		{
			Mesh->ClearMeshSection(0);
			Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UV, Cols, Tans, false);
		}
	}
	if (Mat) Mesh->SetMaterial(0, Mat);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	Mesh->bNeverDistanceCull = true;
	Mesh->SetCullDistance(0.f);
	Mesh->SetBoundsScale(8.f);
	Mesh->SetCastShadow(false);
	Mesh->SetRenderCustomDepth(false);
}

void FSailVisualExtras::UpdateBattenRods(
	const FSailClothSim& Cloth,
	TArray<FBattenRod>& Rods,
	float RadiusCm)
{
	if (!Cloth.bInitialized || !Cloth.bGridTopology || Cloth.Battens.Num() == 0) return;
	// Ensure count matches live cloth (class layout may differ from defaults)
	if (Rods.Num() < Cloth.Battens.Num())
	{
		// Caller should EnsureBuilt first; grow silently if cloth rebuilt
		return;
	}

	for (int32 Bi = 0; Bi < Cloth.Battens.Num() && Bi < Rods.Num(); ++Bi)
	{
		const FSailClothSim::FBattenRow& Bn = Cloth.Battens[Bi];
		FBattenRod& Rod = Rods[Bi];
		if (!Rod.Mesh) continue;

		const int32 NPts = Bn.JLeech - Bn.JStart + 1;
		if (NPts < 2) continue;
		Rod.Pts.SetNum(NPts);
		int32 Pi = 0;
		for (int32 J = Bn.JStart; J <= Bn.JLeech; ++J)
		{
			const int32 Idx = Cloth.GridIdx(Bn.I, J);
			Rod.Pts[Pi++] = Cloth.Pos.IsValidIndex(Idx) ? Cloth.Pos[Idx] : FVector::ZeroVector;
		}
		// Offset slightly to windward of cloth so the rod reads on white sail
		// (use average normal along row)
		FVector AvgN = FVector::ZeroVector;
		int32 Nc = 0;
		for (int32 J = Bn.JStart; J <= Bn.JLeech; J += FMath::Max(1, (Bn.JLeech - Bn.JStart) / 4))
		{
			const int32 Idx = Cloth.GridIdx(Bn.I, J);
			if (Cloth.Normals.IsValidIndex(Idx))
			{
				AvgN += Cloth.Normals[Idx];
				++Nc;
			}
		}
		if (Nc > 0)
		{
			AvgN /= float(Nc);
			if (AvgN.Normalize())
			{
				const float Lift = RadiusCm * 0.85f;
				for (FVector& P : Rod.Pts) P += AvgN * Lift;
			}
		}

		// Taper soft-forward (0.65) → full at leech (1.0) — real tapered battens
		BuildYarnTubeMesh(Rod.Mesh, Rod.Pts, RadiusCm, BattenMat, 0.65f, 1.0f);
	}
}

void FSailVisualExtras::UpdateLeechTales(
	const FSailClothSim& Cloth,
	TArray<FTubeTale>& Tales,
	const TArray<float>& BattenV,
	const FVector& WindLocal,
	float RadiusCm)
{
	using namespace SailVisualLocal;
	if (!Cloth.bInitialized || !Cloth.bGridTopology) return;

	FVector W = WindLocal;
	const bool bHaveWind = W.Normalize();
	if (!bHaveWind) W = FVector(-1.f, 0.f, 0.f);

	for (int32 B = 0; B < BattenV.Num() && B < Tales.Num(); ++B)
	{
		FTubeTale& Tale = Tales[B];
		if (!Tale.Mesh) continue;

		const float V = BattenV[B];
		FVector Nrm;
		const FVector Lee = SampleCloth(Cloth, V, 1.f, &Nrm); // leech
		const FVector In = SampleCloth(Cloth, V, FMath::Max(0.f, 1.f - 1.f / float(Cloth.Nw)));
		FVector Chord = Lee - In;
		if (!Chord.Normalize()) Chord = FVector(-1.f, 0.f, 0.f);

		// Stall: wind opposing trailing-edge exit
		const float Align = FVector::DotProduct(W, Chord);
		float Stall = (0.2f - Align) / 0.6f;
		Stall = FMath::Clamp(Stall, 0.f, 1.f);

		// Leeward normal (face away from wind)
		if (FVector::DotProduct(Nrm, W) < 0.f) Nrm = -Nrm;
		FVector Q = FVector::CrossProduct(W, Nrm);
		if (!Q.Normalize()) Q = FVector::CrossProduct(Chord, Nrm).GetSafeNormal();

		// Faithful port of web hUpdateTellTales streamer integration
		Tale.Pts.SetNum(NPts);
		Tale.Pts[0] = Lee;
		FVector P = Lee;
		const float PhaseB = Phase * 0.55f + float(B) * 2.3f;
		for (int32 S = 1; S < NPts; ++S)
		{
			const float T = float(S) / float(NPts - 1);
			// Web: start from wind (or down if no wind). THREE Y-up gravity → UE Z-up.
			FVector D = bHaveWind ? W : FVector(0.f, 0.f, -1.f);
			D.Z -= 0.35f * T; // light gravity droop
			// Stall curl to leeward + lift (web dx/dy/dz stall terms)
			D += Nrm * (Stall * 0.9f * T);
			D.Z += 0.6f * Stall * T;

			// Travelling flutter wave (amp grows ~t², free tip whips)
			const float Amp = (0.45f + 0.85f * Stall) * T * T;
			const float F1 = Amp * (FMath::Sin(PhaseB - float(S) * 1.7f)
				+ 0.4f * FMath::Sin(PhaseB * 1.9f - float(S) * 2.7f));
			const float F2 = Amp * 0.8f * FMath::Sin(PhaseB * 1.35f - float(S) * 2.1f + 2.1f);
			D += Nrm * F1 + Q * F2;
			if (!D.Normalize()) D = W;
			P += D * SegCm;
			Tale.Pts[S] = P;
		}
		BuildYarnTubeMesh(Tale.Mesh, Tale.Pts, RadiusCm, Tale.Mesh->GetMaterial(0));
	}
}

void FSailVisualExtras::UpdateJibBodyTales(const FSailClothSim& Cloth, const FVector& WindLocal)
{
	using namespace SailVisualLocal;
	if (!Cloth.bInitialized || !Cloth.bGridTopology) return;
	if (JibBodyTales.Num() < 6) return;

	FVector W = WindLocal;
	const bool bHaveWind = W.Normalize();

	for (int32 Si = 0; Si < UE_ARRAY_COUNT(JibStations); ++Si)
	{
		const FJibStation St = JibStations[Si];
		FVector RootN;
		const FVector Root = SampleCloth(Cloth, St.V, St.U, &RootN);
		// Prefer +Y (starboard) as +n for red/green faces
		if (RootN.Y < 0.f) RootN = -RootN;

		const FVector Aft = SampleCloth(Cloth, St.V, FMath::Min(1.f, St.U + 0.12f));
		FVector Chord = Aft - Root;
		if (!Chord.Normalize()) Chord = FVector(-1.f, 0.f, 0.f);

		// Surface-tangent flow
		FVector Stream = Chord;
		if (bHaveWind)
		{
			const float Wn = FVector::DotProduct(W, RootN);
			FVector T = W - RootN * Wn;
			if (T.Size() > 0.08f)
			{
				T.Normalize();
				if (FVector::DotProduct(T, Chord) < 0.f) T = -T;
				Stream = T;
			}
		}
		FVector Q = FVector::CrossProduct(RootN, Stream).GetSafeNormal();
		const float Align = bHaveWind ? FVector::DotProduct(W, Chord) : 0.f;
		float Stall = (0.15f - Align) / 0.55f;
		Stall = FMath::Clamp(Stall, 0.f, 1.f);

		// Web jSpan: how far along the grid the yarn spans toward the leech
		const float ClSeg = FMath::Max(0.15f * 30.48f, FVector::Dist(Root, Aft));
		const int32 JSpan = FMath::Max(2, FMath::RoundToInt((YarnLenCm / FMath::Max(0.15f * 30.48f, ClSeg * 0.5f)) * 0.5f));
		const int32 J0 = FMath::Clamp(FMath::RoundToInt(St.U * float(Cloth.Nw - 1)), 1, Cloth.Nw - 3);
		const int32 I0 = FMath::Clamp(FMath::RoundToInt(St.V * float(Cloth.Nu - 1)), 0, Cloth.Nu - 1);

		for (int32 Face = 0; Face < 2; ++Face)
		{
			const int32 Yi = Si * 2 + Face;
			if (!JibBodyTales.IsValidIndex(Yi) || !JibBodyTales[Yi].Mesh) continue;
			const float Sn = (Face == 0) ? -1.f : 1.f; // red port (−n) / green stbd (+n)
			const float FaceDot = bHaveWind ? FVector::DotProduct(W, RootN * Sn) : 0.f;
			// Windward face stalls more (yarn lifts); leeward flatter when attached
			const float FaceStall = FMath::Clamp(
				Stall * 0.55f + FMath::Max(0.f, -FaceDot) * 0.65f, 0.f, 1.f);
			const float PhaseF = Phase * 0.55f + float(Si) * 1.7f + float(Face) * 1.1f;

			FTubeTale& Tale = JibBodyTales[Yi];
			Tale.Pts.SetNum(NPts);
			for (int32 Pi = 0; Pi < NPts; ++Pi)
			{
				const float T = float(Pi) / float(NPts - 1);
				// Walk grid toward leech (web: jj = j0 + round(t * jSpan))
				const int32 Jj = FMath::Clamp(J0 + FMath::RoundToInt(T * float(JSpan)), 0, Cloth.Nw - 1);
				const float U = float(Jj) / float(Cloth.Nw - 1);
				FVector Cn;
				FVector C = SampleCloth(Cloth, St.V, U, &Cn);
				if (Cn.Y < 0.f) Cn = -Cn;

				// Height above THIS face only — web restH + stallLift * t² + flutter
				const float Flutter = FaceStall * (0.06f * 30.48f) * T * T
					* FMath::Sin(PhaseF - float(Pi) * 1.8f);
				float H = JibRestHCm
					+ FaceStall * JibStallLiftCm * T * T
					+ FMath::Max(0.f, Flutter);
				H = FMath::Max(JibRestHCm * 0.85f, H);

				// In-plane wiggle (web 0.04+0.10*faceStall ft)
				const float Wig = ((0.04f + 0.10f * FaceStall) * 30.48f) * T * T
					* FMath::Sin(PhaseF * 1.3f - float(Pi) * 2.1f + 0.7f);
				const float Along = T * YarnLenCm * 0.15f * FaceStall;

				Tale.Pts[Pi] = C + Cn * (Sn * H) + Stream * Along + Q * Wig;
			}
			BuildYarnTubeMesh(Tale.Mesh, Tale.Pts, JibBodyTaleRadCm, Tale.Mesh->GetMaterial(0));
		}
	}
}

void FSailVisualExtras::UpdateMarkPatch(const FSailClothSim& Cloth, FMarkPatch& Patch)
{
	if (!Patch.Mesh || !Cloth.bInitialized || !Cloth.bGridTopology) return;
	if (!MarkingsTex)
	{
		LoadMarkingsTexture();
	}
	if (!MarkingsTex)
	{
		Patch.Mesh->SetVisibility(false);
		return;
	}
	if (!Patch.Mid)
	{
		Patch.Mid = MakeMarkingsMat(MarkingsTex);
		if (Patch.Mid) Patch.Mesh->SetMaterial(0, Patch.Mid);
	}

	// Cloth-following UV island. Glyph / logo detail lives in the 2048² atlas —
	// the mesh only needs enough segments to bend with the sail (web TSL sample path).
	const int32 Gu = FMath::Clamp(Patch.GridU, 4, 96);
	const int32 Gv = FMath::Clamp(Patch.GridV, 4, 96);
	const int32 Nu = Gu + 1;
	const int32 Nv = Gv + 1;
	const int32 VertsPerSide = Nu * Nv;
	// Front + back so both faces read correctly (real vinyl on both sides of dacron).
	const int32 ExpectedVerts = VertsPerSide * 2;
	const int32 ExpectedTris = Gu * Gv * 6 * 2;
	// Slight lift so markings sit on top of the sail surface without z-fight.
	const float LiftCm = 0.9f;

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Norms;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Cols;
	TArray<FProcMeshTangent> Tans;
	Verts.SetNumUninitialized(ExpectedVerts);
	Norms.SetNumUninitialized(ExpectedVerts);
	UV.SetNumUninitialized(ExpectedVerts);
	Cols.SetNumUninitialized(ExpectedVerts);
	Tans.SetNumUninitialized(ExpectedVerts);

	const bool bNeedTris = (Patch.CachedVertCount != ExpectedVerts)
		|| Patch.Mesh->GetNumSections() == 0
		|| !Patch.Mesh->GetProcMeshSection(0)
		|| Patch.Mesh->GetProcMeshSection(0)->ProcVertexBuffer.Num() != ExpectedVerts;
	if (bNeedTris)
	{
		Tris.SetNumUninitialized(ExpectedTris);
	}

	const FLinearColor White(1.f, 1.f, 1.f, 1.f);
	int32 TriWrite = 0;

	// Side 0 = front (outside), Side 1 = back (readable from leeward / opposite).
	// Front uses island U as-is; back flips U so the opposite face also reads correctly
	// when viewed from outside (real vinyl on both faces of the dacron).
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float SideSign = (Side == 0) ? 1.f : -1.f;
		const int32 VertBase = Side * VertsPerSide;
		for (int32 Iv = 0; Iv < Nv; ++Iv)
		{
			const float Tv = float(Iv) / float(Gv); // 0 = foot of island → 1 = head
			const float SailV = Patch.Cv - Patch.Dv * 0.5f + Tv * Patch.Dv;
			for (int32 Iu = 0; Iu < Nu; ++Iu)
			{
				const float Tu = float(Iu) / float(Gu); // 0 = luff side of island
				const float SailU = Patch.Cu - Patch.Du * 0.5f + Tu * Patch.Du;

				FVector Nrm;
				const FVector P = SampleCloth(Cloth, SailV, SailU, &Nrm);
				if (!Nrm.Normalize()) Nrm = FVector(0.f, 1.f, 0.f);

				// Tangents for lighting / smooth shading along cloth
				const FVector Pu = SampleCloth(Cloth, SailV, FMath::Clamp(SailU + 0.01f, 0.f, 1.f));
				FVector Tangent = (Pu - P);
				if (!Tangent.Normalize()) Tangent = FVector(0.f, 1.f, 0.f);

				const int32 Vi = VertBase + Iv * Nu + Iu;
				Verts[Vi] = P + Nrm * (SideSign * LiftCm);
				Norms[Vi] = Nrm * SideSign;
				Tans[Vi] = FProcMeshTangent(Tangent, false);
				Cols[Vi] = White;

				// Atlas UV. Canvas/web: v increases toward head content at top of PNG.
				// Our bulk upload is top-down rows → V=1 is top in UE filter space when
				// using standard UV; match previous ink path: foot (Tv=0) → TexMax.Y.
				const float IslandU = (Side == 0) ? Tu : (1.f - Tu);
				const float TexU = FMath::Lerp(Patch.TexMin.X, Patch.TexMax.X, IslandU);
				const float TexV = FMath::Lerp(Patch.TexMin.Y, Patch.TexMax.Y, 1.f - Tv);
				UV[Vi] = FVector2D(TexU, TexV);
			}
		}

		if (bNeedTris)
		{
			for (int32 Iv = 0; Iv < Gv; ++Iv)
			{
				for (int32 Iu = 0; Iu < Gu; ++Iu)
				{
					const int32 I00 = VertBase + Iv * Nu + Iu;
					const int32 I10 = I00 + 1;
					const int32 I01 = I00 + Nu;
					const int32 I11 = I01 + 1;
					if (Side == 0)
					{
						// CCW when viewed along +normal
						Tris[TriWrite++] = I00; Tris[TriWrite++] = I01; Tris[TriWrite++] = I10;
						Tris[TriWrite++] = I10; Tris[TriWrite++] = I01; Tris[TriWrite++] = I11;
					}
					else
					{
						// Flip winding for back face
						Tris[TriWrite++] = I00; Tris[TriWrite++] = I10; Tris[TriWrite++] = I01;
						Tris[TriWrite++] = I10; Tris[TriWrite++] = I11; Tris[TriWrite++] = I01;
					}
				}
			}
		}
	}

	if (bNeedTris)
	{
		if (Patch.Mesh->GetNumSections() > 0)
		{
			Patch.Mesh->ClearMeshSection(0);
		}
		Patch.Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UV, Cols, Tans, false);
		Patch.CachedVertCount = ExpectedVerts;
		if (Patch.Mid) Patch.Mesh->SetMaterial(0, Patch.Mid);
		Patch.Mesh->SetCastShadow(false);
		Patch.Mesh->bNeverDistanceCull = true;
		Patch.Mesh->SetCullDistance(0.f);
		Patch.Mesh->SetBoundsScale(8.f);
	}
	else
	{
		// Fixed topology: deform only (cheap UpdateMeshSection).
		Patch.Mesh->UpdateMeshSection_LinearColor(0, Verts, Norms, UV, Cols, Tans);
	}
	Patch.Mesh->SetVisibility(true);
	Patch.Mesh->SetHiddenInGame(false);
}

void FSailVisualExtras::Update(
	float DeltaSeconds,
	const FSailClothSim& MainCloth,
	const FSailClothSim& JibCloth,
	const FVector& WindLocalMain,
	const FVector& WindLocalJib)
{
	if (!bEnabled) return;
	if (!bBuilt)
	{
		EnsureBuilt(MainSailW.Get(), JibSailW.Get());
	}
	// Web: hTellTalePhase++ once per cloth step (not wall-clock scaled)
	Phase += 1.f;

	if (bBattenRods)
	{
		if (MainCloth.bInitialized && MainCloth.Battens.Num() > 0)
		{
			if (MainBattenRods.Num() < MainCloth.Battens.Num())
			{
				EnsureBattenRods(MainSailW.Get(), MainBattenRods, MainCloth.Battens.Num());
			}
			UpdateBattenRods(MainCloth, MainBattenRods, SailVisualLocal::MainBattenRadCm);
		}
		if (JibCloth.bInitialized && JibCloth.Battens.Num() > 0)
		{
			if (JibBattenRods.Num() < JibCloth.Battens.Num())
			{
				EnsureBattenRods(JibSailW.Get(), JibBattenRods, JibCloth.Battens.Num());
			}
			UpdateBattenRods(JibCloth, JibBattenRods, SailVisualLocal::JibBattenRadCm);
		}
	}

	if (bTellTales)
	{
		if (MainCloth.bInitialized)
		{
			UpdateLeechTales(MainCloth, MainLeechTales, SailVisualLocal::MainBattenV,
				WindLocalMain, SailVisualLocal::MainTaleRadCm);
		}
		// Code Zero / kite does not get yarns; hide jib yarns while rolled away.
		if (bJibTellTales && JibCloth.bInitialized)
		{
			UpdateLeechTales(JibCloth, JibLeechTales, SailVisualLocal::JibBattenV,
				WindLocalJib, SailVisualLocal::JibLeechTaleRadCm);
			UpdateJibBodyTales(JibCloth, WindLocalJib);
		}
		else
		{
			for (FTubeTale& T : JibLeechTales)
			{
				if (T.Mesh)
				{
					T.Mesh->SetVisibility(false);
					T.Mesh->SetHiddenInGame(true);
				}
			}
			for (FTubeTale& T : JibBodyTales)
			{
				if (T.Mesh)
				{
					T.Mesh->SetVisibility(false);
					T.Mesh->SetHiddenInGame(true);
				}
			}
		}
	}

	if (bSailMarkings && MainCloth.bInitialized)
	{
		UpdateMarkPatch(MainCloth, LogoPatch);
		UpdateMarkPatch(MainCloth, NumberPatch);
	}
}
