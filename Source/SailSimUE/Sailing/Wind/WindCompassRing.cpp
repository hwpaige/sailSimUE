#include "Sailing/Wind/WindCompassRing.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "SailSimUE.h"

#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Font.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "UObject/Package.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

namespace WindCompassLocal
{
	static constexpr float FtToCm = 30.48f;

	// Ring: white day + night. Day = lit white only (no emissive). Night = white + glow.
	static const FLinearColor RingWhite(0.92f, 0.93f, 0.95f, 1.f);
	static const FLinearColor LubberGoldDay(0.85f, 0.70f, 0.22f, 1.f);
	static const FLinearColor LubberGoldNight(0.85f, 0.70f, 0.22f, 1.f);
	// Wind/north arrows: saturated albedos (slightly dimmer at night via emissive path).
	static const FLinearColor TrueCyan(0.12f, 0.55f, 0.62f, 1.f);
	static const FLinearColor TrueCyanNight(0.08f, 0.38f, 0.44f, 1.f);
	static const FLinearColor AppAmber(0.72f, 0.38f, 0.10f, 1.f);
	static const FLinearColor AppAmberNight(0.48f, 0.26f, 0.07f, 1.f);
	static const FLinearColor NorthRed(0.72f, 0.14f, 0.10f, 1.f);
	static const FLinearColor NorthRedNight(0.48f, 0.10f, 0.07f, 1.f);
	// Letter strokes — black ink on colored triangles (no self-light needed).
	static const FLinearColor MarkBlack(0.02f, 0.02f, 0.025f, 1.f);
	static const FLinearColor VtxWhite = FLinearColor::White;

	// Degree labels: full alpha, vivid cyan / amber (TextRender color is sRGB 0–255).
	static const FColor LabelTrue(55, 200, 215, 255);
	static const FColor LabelApp(255, 150, 55, 255);

	static void AppendTri(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		const FVector& A, const FVector& B, const FVector& C,
		const FLinearColor& Col)
	{
		const int32 Base = Verts.Num();
		FVector N = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		if (N.IsNearlyZero()) N = FVector::UpVector;
		if (N.Z < 0.f) N = -N;
		Verts.Add(A); Verts.Add(B); Verts.Add(C);
		Norms.Add(N); Norms.Add(N); Norms.Add(N);
		UVs.Add(FVector2D(0.f, 0.f)); UVs.Add(FVector2D(1.f, 0.f)); UVs.Add(FVector2D(0.5f, 1.f));
		Colors.Add(Col); Colors.Add(Col); Colors.Add(Col);
		Tris.Add(Base); Tris.Add(Base + 1); Tris.Add(Base + 2);
		// Backface
		const int32 Base2 = Verts.Num();
		Verts.Add(A); Verts.Add(C); Verts.Add(B);
		Norms.Add(-N); Norms.Add(-N); Norms.Add(-N);
		UVs.Add(FVector2D(0.f, 0.f)); UVs.Add(FVector2D(0.5f, 1.f)); UVs.Add(FVector2D(1.f, 0.f));
		Colors.Add(Col); Colors.Add(Col); Colors.Add(Col);
		Tris.Add(Base2); Tris.Add(Base2 + 1); Tris.Add(Base2 + 2);
	}

	/** Axis-aligned quad in the horizontal plane (Z = PlaneZ). */
	static void AppendQuad(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3,
		const FLinearColor& Col)
	{
		AppendTri(Verts, Tris, Norms, UVs, Colors, P0, P1, P2, Col);
		AppendTri(Verts, Tris, Norms, UVs, Colors, P0, P2, P3, Col);
	}

	/** Thick stroke from A→B in the XY plane. */
	static void AppendStroke(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		const FVector& A, const FVector& B, float HalfW, const FLinearColor& Col)
	{
		FVector D = (B - A);
		D.Z = 0.f;
		if (D.SizeSquared() < 1.e-4f) return;
		D.Normalize();
		const FVector Side(-D.Y * HalfW, D.X * HalfW, 0.f);
		const FVector P0 = A + Side;
		const FVector P1 = A - Side;
		const FVector P2 = B - Side;
		const FVector P3 = B + Side;
		AppendQuad(Verts, Tris, Norms, UVs, Colors, P0, P1, P2, P3, Col);
	}

	static void AppendTick(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		float Rad, float R0, float R1, float HalfThick, float PlaneZ, const FLinearColor& Col)
	{
		const float Cx = FMath::Cos(Rad);
		const float Cy = FMath::Sin(Rad);
		const float Tx = -Cy;
		const float Ty = Cx;
		const FVector P0((R0 * Cx) + Tx * HalfThick, (R0 * Cy) + Ty * HalfThick, PlaneZ);
		const FVector P1((R0 * Cx) - Tx * HalfThick, (R0 * Cy) - Ty * HalfThick, PlaneZ);
		const FVector P2((R1 * Cx) - Tx * HalfThick, (R1 * Cy) - Ty * HalfThick, PlaneZ);
		const FVector P3((R1 * Cx) + Tx * HalfThick, (R1 * Cy) + Ty * HalfThick, PlaneZ);
		AppendQuad(Verts, Tris, Norms, UVs, Colors, P0, P1, P2, P3, Col);
	}

	/** Map unit letter coords (u across, v along radial out) into world boat-frame. */
	static FVector LetterPt(float SeatRad, float MidR, float PlaneZ, float U, float V, float Scale)
	{
		const float Cx = FMath::Cos(SeatRad);
		const float Cy = FMath::Sin(SeatRad);
		// Radial out = seat dir; tangent CW
		const FVector RadDir(Cx, Cy, 0.f);
		const FVector TanDir(-Cy, Cx, 0.f);
		return FVector(MidR * Cx, MidR * Cy, PlaneZ) + TanDir * (U * Scale) + RadDir * (V * Scale);
	}
}

void FWindCompassRing::Clear()
{
	auto KillMesh = [](TWeakObjectPtr<UProceduralMeshComponent>& W)
	{
		if (UProceduralMeshComponent* M = W.Get())
		{
			M->ClearAllMeshSections();
			M->DestroyComponent();
		}
		W.Reset();
	};
	auto KillLabel = [](TWeakObjectPtr<UTextRenderComponent>& W)
	{
		if (UTextRenderComponent* L = W.Get())
		{
			L->DestroyComponent();
		}
		W.Reset();
	};

	KillMesh(StaticMeshW);
	KillMesh(TrueArrowW);
	KillMesh(AppArrowW);
	KillMesh(NorthArrowW);
	KillLabel(TrueLabelW);
	KillLabel(AppLabelW);

	if (USceneComponent* R = RootW.Get())
	{
		R->DestroyComponent();
	}
	RootW.Reset();
	RingMatW.Reset();
	TrueMatW.Reset();
	AppMatW.Reset();
	NorthMatW.Reset();
	LubberMatW.Reset();
	MarkMatW.Reset();
	LastTrueTxt.Reset();
	LastAppTxt.Reset();
	bBuilt = false;
	BuiltMatRecipe = 0;
	RingNightBoost = TrueNightBoost = AppNightBoost = NorthNightBoost = LubberNightBoost = 0.f;
	CachedNightGlow01 = -1.f;
}

UMaterialInstanceDynamic* FWindCompassRing::MakeIndicatorMat(const FLinearColor& Color, float /*unusedFill*/)
{
	// Prefer unlit translucent overlay so water SSR never samples us.
	// (SSR reads opaque SceneColor and ignores bVisibleInReflections.)
	// Graph: Emissive = BaseColor * (1 + EmissiveBoost * 12)  — see create_wind_compass_material.py
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/Materials/Yacht/M_WindCompass_Overlay.M_WindCompass_Overlay"));
	if (!Base)
	{
		// Engine debug unlit translucent — still skips opaque SSR source.
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	}
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialTranslucent.DefaultTextMaterialTranslucent"));
	}
	// Last resort: opaque yacht PBR (will still mirror on water via SSR).
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Materials/Yacht/M_Yacht_PBR_TwoSided.M_Yacht_PBR_TwoSided"));
	}
	if (!Base)
	{
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}
	if (!Base) return nullptr;

	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return nullptr;

	FLinearColor C = Color;
	C.A = 1.f;

	Mid->SetVectorParameterValue(TEXT("BaseColor"), C);
	Mid->SetVectorParameterValue(TEXT("Color"), C); // M_SimpleUnlitTranslucent / some engine mats
	// Day default: no extra glow. Night boost applied in ApplyNightEmissive.
	Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	// Harmless if the overlay has no PBR pins; helps opaque fallback stay matte.
	Mid->SetVectorParameterValue(TEXT("SpecularTint"), FLinearColor::White);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 0.95f);
	Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Specular"), 0.02f);
	Mid->SetScalarParameterValue(TEXT("ClearCoatBoost"), 0.f);
	return Mid;
}

float FWindCompassRing::SampleNightGlow01(const UWorld* World)
{
	// Env preset first (authoritative), then sun intensity. Fair Day is always 0.
	if (!World) return 0.f;

	if (const USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const uint8 Preset = Ocean->GetActiveEnvPreset();
		// ESailEnvPreset: FairDay=0, Golden=1, Dusk=2, Night=3, Overcast=4, Storm=5, Fog=6
		switch (Preset)
		{
		case 0: // FairDay
			return 0.f;
		case 1: // GoldenHour
			return 0.35f;
		case 2: // Dusk
			return 0.85f;
		case 3: // Night
			return 1.f;
		default:
			break; // Overcast / Storm / Fog → sun heuristic
		}
	}

	float BestSun = 0.f;
	for (TActorIterator<ADirectionalLight> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ADirectionalLight* L = *It;
		if (!IsValid(L)) continue;
		if (L->GetActorNameOrLabel().Contains(TEXT("SailSim_Moon"))) continue;
		if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(L->GetLightComponent()))
		{
			if (C->IsVisible())
			{
				BestSun = FMath::Max(BestSun, C->Intensity);
			}
		}
	}
	// Bright sun → 0; dim sun → 1. Collapse residual daytime to 0.
	const float Night01 = FMath::GetMappedRangeValueClamped(
		FVector2D(0.5f, 7.f), FVector2D(1.f, 0.f), BestSun);
	return FMath::GetMappedRangeValueClamped(FVector2D(0.3f, 0.85f), FVector2D(0.f, 1.f), Night01);
}

void FWindCompassRing::ApplyNightEmissive(float NightGlow01)
{
	using namespace WindCompassLocal;
	const float G = FMath::Clamp(NightGlow01, 0.f, 1.f);

	auto SetLook = [G](UMaterialInstanceDynamic* Mid, const FLinearColor& DayCol,
		const FLinearColor& NightCol, float PeakBoost)
	{
		if (!Mid) return;
		const FLinearColor Base = FMath::Lerp(DayCol, NightCol, G);
		// Night needs a real boost — mid-gray albedo alone washes white under night exposure.
		// Cap ~0.14 so it glows softly without neon bloom.
		// Overlay graph: Emissive = BaseColor * (1 + EmissiveBoost * 12).
		const float Boost = FMath::Clamp(PeakBoost * G, 0.f, 0.14f);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Base);
		Mid->SetVectorParameterValue(TEXT("Color"), Base);
		Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Boost);
		Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	};

	// Ring: same white BaseColor day/night; only EmissiveBoost turns on at night.
	SetLook(RingMatW.Get(), RingWhite, RingWhite, RingNightBoost);
	SetLook(LubberMatW.Get(), LubberGoldDay, LubberGoldNight, LubberNightBoost);
	// Arrows: same idea, keep chroma
	SetLook(TrueMatW.Get(), TrueCyan, TrueCyanNight, TrueNightBoost);
	SetLook(AppMatW.Get(), AppAmber, AppAmberNight, AppNightBoost);
	SetLook(NorthMatW.Get(), NorthRed, NorthRedNight, NorthNightBoost);
	// Letters stay black ink, never glow
	if (UMaterialInstanceDynamic* M = MarkMatW.Get())
	{
		M->SetVectorParameterValue(TEXT("BaseColor"), MarkBlack);
		M->SetVectorParameterValue(TEXT("Color"), MarkBlack);
		M->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
		M->SetScalarParameterValue(TEXT("Opacity"), 1.f);
	}
	CachedNightGlow01 = G;
}

/** Overlay chrome: skip Lumen/RT/captures. SSR is handled by translucent unlit mats. */
static void ExcludeFromReflections(UPrimitiveComponent* Prim)
{
	if (!Prim) return;
	// UE 5.8: these are bitfields (no setters for all of them).
	Prim->bVisibleInReflectionCaptures = false;
	Prim->bVisibleInRealTimeSkyCaptures = false;
	Prim->bVisibleInReflections = false;
	Prim->SetVisibleInRayTracing(false);
	// Planar-reflection / water scene-captures (not main view).
	Prim->SetHiddenInSceneCapture(true);
	Prim->bAffectDynamicIndirectLighting = false;
	Prim->bAffectDistanceFieldLighting = false;
	Prim->SetCastShadow(false);
	Prim->bCastContactShadow = false;
	Prim->bCastDynamicShadow = false;
	Prim->bCastStaticShadow = false;
	Prim->bCastVolumetricTranslucentShadow = false;
	Prim->SetReceivesDecals(false);
	Prim->SetRenderCustomDepth(false);
	if (Prim->IsRegistered())
	{
		Prim->MarkRenderStateDirty();
	}
}

UProceduralMeshComponent* FWindCompassRing::MakeMesh(
	ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name)
{
	if (!Boat || !Parent) return nullptr;
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(Boat, Name);
	M->SetupAttachment(Parent);
	M->SetMobility(EComponentMobility::Movable);
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetGenerateOverlapEvents(false);
	ExcludeFromReflections(M);
	M->bNeverDistanceCull = true;
	M->SetCullDistance(0.f);
	M->SetBoundsScale(12.f);
	M->bUseAsyncCooking = false;
	// Draw with other translucent overlays (after opaque SceneColor SSR samples for water).
	M->SetTranslucentSortPriority(50);
	M->RegisterComponent();
	ExcludeFromReflections(M); // re-apply + MarkRenderStateDirty after register
	Boat->AddInstanceComponent(M);
	return M;
}

UTextRenderComponent* FWindCompassRing::MakeLabel(
	ASailBoatPawn* Boat, USceneComponent* Parent, const FName& Name, const FColor& Color)
{
	if (!Boat || !Parent) return nullptr;
	UTextRenderComponent* T = NewObject<UTextRenderComponent>(Boat, Name);
	T->SetupAttachment(Parent);
	T->SetMobility(EComponentMobility::Movable);
	T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ExcludeFromReflections(T);
	T->bNeverDistanceCull = true;
	T->SetHorizontalAlignment(EHTA_Center);
	T->SetVerticalAlignment(EVRTA_TextCenter);
	T->SetWorldSize(58.f);
	// Translucent text so degree labels also stay out of opaque SSR / water mirrors.
	UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialTranslucent.DefaultTextMaterialTranslucent"));
	if (!TextMat)
	{
		TextMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque.DefaultTextMaterialOpaque"));
	}
	if (TextMat)
	{
		T->SetTextMaterial(TextMat);
	}
	T->SetTextRenderColor(Color);
	T->SetText(FText::GetEmpty());
	// Prefer reliable engine distance-field font over project face assets.
	if (UFont* Font = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/RobotoDistanceField.RobotoDistanceField")))
	{
		T->SetFont(Font);
	}
	T->RegisterComponent();
	ExcludeFromReflections(T);
	Boat->AddInstanceComponent(T);
	return T;
}

void FWindCompassRing::BuildStaticGeometry(UProceduralMeshComponent* Mesh)
{
	using namespace WindCompassLocal;
	if (!Mesh) return;

	const float R = RadiusCm;
	const float PlaneZ = 6.f; // clear water surface a bit more
	const float Tube = 0.22f * FtToCm; // thicker so it reads from chase cam
	const int32 Segs = 96;

	TArray<FVector> RingVerts, LubVerts;
	TArray<int32> RingTris, LubTris;
	TArray<FVector> RingNorms, LubNorms;
	TArray<FVector2D> RingUVs, LubUVs;
	TArray<FLinearColor> RingCols, LubCols;
	RingVerts.Reserve(Segs * 24 + 400);

	// Annular ring (top face) — white verts; color from material + soft fill
	for (int32 I = 0; I < Segs; ++I)
	{
		const float A0 = (float(I) / Segs) * 2.f * PI;
		const float A1 = (float(I + 1) / Segs) * 2.f * PI;
		const float C0 = FMath::Cos(A0), S0 = FMath::Sin(A0);
		const float C1 = FMath::Cos(A1), S1 = FMath::Sin(A1);
		const float Ri = R - Tube;
		const float Ro = R + Tube;
		const FVector I0(Ri * C0, Ri * S0, PlaneZ);
		const FVector I1(Ri * C1, Ri * S1, PlaneZ);
		const FVector O0(Ro * C0, Ro * S0, PlaneZ);
		const FVector O1(Ro * C1, Ro * S1, PlaneZ);
		AppendTri(RingVerts, RingTris, RingNorms, RingUVs, RingCols, I0, O0, O1, VtxWhite);
		AppendTri(RingVerts, RingTris, RingNorms, RingUVs, RingCols, I0, O1, I1, VtxWhite);
	}

	for (int32 Deg = 0; Deg < 360; Deg += 15)
	{
		const bool bMajor = (Deg % 90 == 0);
		const bool bMid = (Deg % 45 == 0);
		const float Rad = FMath::DegreesToRadians(float(Deg));
		const float Inset = (bMajor ? 2.2f : (bMid ? 1.5f : 0.95f)) * FtToCm;
		const float R0 = R - Inset;
		const float HalfT = (bMajor ? 0.28f : (bMid ? 0.20f : 0.14f)) * FtToCm * 0.5f;
		AppendTick(RingVerts, RingTris, RingNorms, RingUVs, RingCols, Rad, R0, R, HalfT, PlaneZ + 0.5f, VtxWhite);
	}

	// Bow lubber
	{
		const float Y = PlaneZ + 1.f;
		const FVector Tip(R + 2.4f * FtToCm, 0.f, Y);
		const FVector A(R + 0.4f * FtToCm, -1.3f * FtToCm, Y);
		const FVector B(R + 0.4f * FtToCm, 1.3f * FtToCm, Y);
		AppendTri(LubVerts, LubTris, LubNorms, LubUVs, LubCols, Tip, A, B, VtxWhite);
		const FVector Tip2(R + 2.7f * FtToCm, 0.f, Y);
		const FVector A2(R + 0.2f * FtToCm, -1.5f * FtToCm, Y);
		const FVector B2(R + 0.2f * FtToCm, 1.5f * FtToCm, Y);
		AppendTri(LubVerts, LubTris, LubNorms, LubUVs, LubCols, Tip2, A2, Tip, VtxWhite);
		AppendTri(LubVerts, LubTris, LubNorms, LubUVs, LubCols, Tip2, Tip, B2, VtxWhite);
	}

	TArray<FProcMeshTangent> Tangents;
	Mesh->CreateMeshSection_LinearColor(0, RingVerts, RingTris, RingNorms, RingUVs, RingCols, Tangents, false);
	Mesh->CreateMeshSection_LinearColor(1, LubVerts, LubTris, LubNorms, LubUVs, LubCols, Tangents, false);
	Mesh->SetCastShadow(false);
	if (RingMatW.IsValid()) Mesh->SetMaterial(0, RingMatW.Get());
	if (LubberMatW.IsValid()) Mesh->SetMaterial(1, LubberMatW.Get());
}

void FWindCompassRing::BuildFaceLetter(
	UProceduralMeshComponent* Mesh,
	EFaceLetter Letter,
	float SeatRad,
	float MidR,
	float PlaneZCm,
	float ScaleCm) const
{
	using namespace WindCompassLocal;
	if (!Mesh) return;

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Norms;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	const float Z = PlaneZCm + 4.f; // sit slightly above the triangle face
	const float W = 0.11f; // stroke half-width in letter units
	const float HalfW = W * ScaleCm;

	auto Pt = [&](float U, float V) -> FVector
	{
		return LetterPt(SeatRad, MidR, Z, U, V, ScaleCm);
	};
	auto Stroke = [&](float U0, float V0, float U1, float V1)
	{
		AppendStroke(Verts, Tris, Norms, UVs, Colors, Pt(U0, V0), Pt(U1, V1), HalfW, VtxWhite);
	};

	// Unit box roughly −0.5..0.5 in U (tangent) and V (radial out).
	switch (Letter)
	{
	case EFaceLetter::P:
		// Vertical stem + top/right bowl
		Stroke(-0.28f, -0.48f, -0.28f, 0.48f);
		Stroke(-0.28f, 0.48f, 0.22f, 0.48f);
		Stroke(0.22f, 0.48f, 0.22f, 0.05f);
		Stroke(0.22f, 0.05f, -0.28f, 0.05f);
		break;
	case EFaceLetter::A:
		Stroke(-0.32f, -0.48f, 0.f, 0.48f);
		Stroke(0.32f, -0.48f, 0.f, 0.48f);
		Stroke(-0.18f, -0.05f, 0.18f, -0.05f);
		break;
	case EFaceLetter::N:
		Stroke(-0.30f, -0.48f, -0.30f, 0.48f);
		Stroke(0.30f, -0.48f, 0.30f, 0.48f);
		Stroke(-0.30f, 0.48f, 0.30f, -0.48f);
		break;
	}

	TArray<FProcMeshTangent> Tangents;
	if (Mesh->GetNumSections() > 1)
	{
		Mesh->UpdateMeshSection_LinearColor(1, Verts, Norms, UVs, Colors, Tangents);
	}
	else
	{
		// Ensure section 0 exists first (caller builds triangle on 0).
		Mesh->CreateMeshSection_LinearColor(1, Verts, Tris, Norms, UVs, Colors, Tangents, false);
	}
	if (MarkMatW.IsValid())
	{
		Mesh->SetMaterial(1, MarkMatW.Get());
	}
}

void FWindCompassRing::PlaceWindArrow(
	UProceduralMeshComponent* Mesh,
	float FlowToRad,
	float LenCm,
	float PlaneZCm,
	EFaceLetter Letter,
	float& OutSeatRad,
	float& OutAltCm) const
{
	using namespace WindCompassLocal;
	if (!Mesh) return;

	const float R = RadiusCm;
	const float SeatRad = FlowToRad + PI;
	OutSeatRad = SeatRad;

	const float Cx = FMath::Cos(SeatRad);
	const float Cy = FMath::Sin(SeatRad);
	const float Tx = -Cy;
	const float Ty = Cx;

	const float LenFt = LenCm / FtToCm;
	const float Side = (3.2f + LenFt * 0.18f) * FtToCm;
	const float Alt = Side * 0.866025403784f;
	OutAltCm = Alt;
	const float Hs = Side * 0.5f;

	const FVector Tip(R * Cx, R * Cy, PlaneZCm);
	const FVector Bc((R + Alt) * Cx, (R + Alt) * Cy, PlaneZCm);
	const FVector P1 = Bc + FVector(Hs * Tx, Hs * Ty, 0.f);
	const FVector P2 = Bc - FVector(Hs * Tx, Hs * Ty, 0.f);

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Norms;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	AppendTri(Verts, Tris, Norms, UVs, Colors, Tip, P1, P2, VtxWhite);

	TArray<FProcMeshTangent> Tangents;
	if (Mesh->GetNumSections() > 0)
	{
		Mesh->UpdateMeshSection_LinearColor(0, Verts, Norms, UVs, Colors, Tangents);
	}
	else
	{
		Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UVs, Colors, Tangents, false);
	}

	// Centroid of tip + base ≈ R + (2/3) Alt for outboard base
	const float MidR = R + Alt * (2.f / 3.f);
	const float LetterScale = FMath::Clamp(Side * 0.42f, 35.f, 90.f);
	BuildFaceLetter(Mesh, Letter, SeatRad, MidR, PlaneZCm, LetterScale);
}

void FWindCompassRing::PlaceNorthArrow(
	UProceduralMeshComponent* Mesh,
	float NorthRad,
	float PlaneZCm,
	float& OutSeatRad,
	float& OutAltCm) const
{
	using namespace WindCompassLocal;
	if (!Mesh) return;

	OutSeatRad = NorthRad;
	const float R = RadiusCm;
	const float Cx = FMath::Cos(NorthRad);
	const float Cy = FMath::Sin(NorthRad);
	const float Tx = -Cy;
	const float Ty = Cx;

	const float Side = 4.2f * FtToCm;
	const float Alt = Side * 0.866025403784f;
	OutAltCm = Alt;
	const float Hs = Side * 0.5f;

	const FVector Tip((R + Alt) * Cx, (R + Alt) * Cy, PlaneZCm);
	const FVector Bc(R * Cx, R * Cy, PlaneZCm);
	const FVector P1 = Bc + FVector(Hs * Tx, Hs * Ty, 0.f);
	const FVector P2 = Bc - FVector(Hs * Tx, Hs * Ty, 0.f);

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Norms;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	AppendTri(Verts, Tris, Norms, UVs, Colors, Tip, P1, P2, VtxWhite);

	TArray<FProcMeshTangent> Tangents;
	if (Mesh->GetNumSections() > 0)
	{
		Mesh->UpdateMeshSection_LinearColor(0, Verts, Norms, UVs, Colors, Tangents);
	}
	else
	{
		Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UVs, Colors, Tangents, false);
	}

	// Centroid for tip-outboard triangle ≈ R + Alt/3
	const float MidR = R + Alt * (1.f / 3.f);
	const float LetterScale = FMath::Clamp(Side * 0.42f, 35.f, 90.f);
	BuildFaceLetter(Mesh, EFaceLetter::N, NorthRad, MidR, PlaneZCm, LetterScale);
}

FString FWindCompassRing::FormatSignedAwa(float SignedAwaDeg)
{
	int32 A = FMath::RoundToInt(SignedAwaDeg);
	if (A == 0 || A == 360 || A == -360) return TEXT("000°");
	int32 Abs = FMath::Abs(A);
	if (Abs > 180) Abs = 360 - Abs;
	const TCHAR Side = A > 0 ? TEXT('S') : TEXT('P');
	return FString::Printf(TEXT("%c%03d°"), Side, Abs);
}

void FWindCompassRing::SetMeshVisible(UProceduralMeshComponent* Mesh, bool bVis)
{
	if (!Mesh) return;
	Mesh->SetVisibility(bVis);
	Mesh->SetHiddenInGame(!bVis);
}

void FWindCompassRing::SetLabelVisible(UTextRenderComponent* Lab, bool bVis)
{
	if (!Lab) return;
	Lab->SetVisibility(bVis);
	Lab->SetHiddenInGame(!bVis);
}

void FWindCompassRing::EnsureBuilt(ASailBoatPawn* Boat)
{
	// Rebuild if marks missing or material recipe changed (hot-reload can leave bright MIDs).
	const bool bOk = bBuilt && RootW.IsValid() && TrueArrowW.IsValid() && NorthArrowW.IsValid()
		&& MarkMatW.IsValid() && BuiltMatRecipe == MatRecipeVersion;
	if (bOk)
	{
		return;
	}
	if (bBuilt)
	{
		Clear();
	}
	if (!Boat || !Boat->GetRootComponent()) return;

	USceneComponent* Root = NewObject<USceneComponent>(Boat, TEXT("WindCompassRoot"));
	Root->SetupAttachment(Boat->GetRootComponent());
	Root->SetMobility(EComponentMobility::Movable);
	Root->SetUsingAbsoluteLocation(true);
	Root->SetUsingAbsoluteRotation(true);
	Root->SetUsingAbsoluteScale(true);
	Root->RegisterComponent();
	Boat->AddInstanceComponent(Root);
	RootW = Root;

	// Peak night boosts. Ring needs enough to read as a soft white instrument light.
	RingNightBoost = FMath::Max(SoftFill * 2.2f, 0.12f);
	TrueNightBoost = FMath::Max(SoftFill * 1.4f, 0.08f);
	AppNightBoost = FMath::Max(SoftFill * 1.4f, 0.08f);
	NorthNightBoost = FMath::Max(SoftFill * 1.5f, 0.09f);
	LubberNightBoost = FMath::Max(SoftFill * 1.8f, 0.10f);

	// Start with day BaseColors; ApplyNightEmissive sets day/night blend immediately after.
	RingMatW = MakeIndicatorMat(WindCompassLocal::RingWhite);
	TrueMatW = MakeIndicatorMat(WindCompassLocal::TrueCyan);
	AppMatW = MakeIndicatorMat(WindCompassLocal::AppAmber);
	NorthMatW = MakeIndicatorMat(WindCompassLocal::NorthRed);
	LubberMatW = MakeIndicatorMat(WindCompassLocal::LubberGoldDay);
	// Letters: black BaseColor, never emissive.
	MarkMatW = MakeIndicatorMat(WindCompassLocal::MarkBlack);

	UProceduralMeshComponent* StaticM = MakeMesh(Boat, Root, TEXT("WindCompassStatic"));
	StaticMeshW = StaticM;
	BuildStaticGeometry(StaticM);

	UProceduralMeshComponent* TrueM = MakeMesh(Boat, Root, TEXT("WindTrueArrow"));
	TrueArrowW = TrueM;
	if (TrueMatW.IsValid()) TrueM->SetMaterial(0, TrueMatW.Get());
	if (MarkMatW.IsValid()) TrueM->SetMaterial(1, MarkMatW.Get());

	UProceduralMeshComponent* AppM = MakeMesh(Boat, Root, TEXT("WindAppArrow"));
	AppArrowW = AppM;
	if (AppMatW.IsValid()) AppM->SetMaterial(0, AppMatW.Get());
	if (MarkMatW.IsValid()) AppM->SetMaterial(1, MarkMatW.Get());

	UProceduralMeshComponent* NorthM = MakeMesh(Boat, Root, TEXT("WindNorthArrow"));
	NorthArrowW = NorthM;
	if (NorthMatW.IsValid()) NorthM->SetMaterial(0, NorthMatW.Get());
	if (MarkMatW.IsValid()) NorthM->SetMaterial(1, MarkMatW.Get());

	TrueLabelW = MakeLabel(Boat, Root, TEXT("WindTrueLabel"), WindCompassLocal::LabelTrue);
	AppLabelW = MakeLabel(Boat, Root, TEXT("WindAppLabel"), WindCompassLocal::LabelApp);

	bBuilt = true;
	BuiltMatRecipe = MatRecipeVersion;
	CachedNightGlow01 = -1.f;
	ApplyNightEmissive(SampleNightGlow01(Boat->GetWorld()));
	UE_LOG(LogSailSim, Log, TEXT("WindCompass: built ring + P/A/N (night soft fill=%.3f recipe=%d)"),
		SoftFill, MatRecipeVersion);
}

void FWindCompassRing::Update(ASailBoatPawn* Boat, float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (!bEnabled || !Boat)
	{
		if (RootW.IsValid()) RootW->SetVisibility(false, true);
		return;
	}

	const float Tws = Boat->GetTrueWindSpeedKn();
	if (Tws < MinTwsKn)
	{
		if (RootW.IsValid()) RootW->SetVisibility(false, true);
		return;
	}

	EnsureBuilt(Boat);
	USceneComponent* Root = RootW.Get();
	if (!Root) return;
	Root->SetVisibility(true, true);

	// Day: EmissiveBoost=0. Night/dusk: soft BaseColor×boost (same yacht PBR path).
	const float NightGlow = SampleNightGlow01(Boat->GetWorld());
	if (!FMath::IsNearlyEqual(NightGlow, CachedNightGlow01, 0.02f))
	{
		ApplyNightEmissive(NightGlow);
	}

	const FVector BoatLoc = Boat->GetActorLocation();
	const float Hdg = Boat->GetHeadingDeg();
	Root->SetWorldLocation(FVector(BoatLoc.X, BoatLoc.Y, BoatLoc.Z + LiftCm));
	Root->SetWorldRotation(FRotator(0.f, Hdg, 0.f));
	Root->SetWorldScale3D(FVector::OneVector);

	const float TrueFromRad = FMath::DegreesToRadians(Boat->GetTrueWindDirDeg() - Hdg);
	const float AppFromRad = FMath::DegreesToRadians(Boat->GetApparentWindAngleDeg());
	const float TrueToRad = TrueFromRad + PI;
	const float AppToRad = AppFromRad + PI;

	const float Aws = Boat->GetApparentWindSpeedKn();
	const float TrueLenCm = (3.2f + Tws * 0.20f) * WindCompassLocal::FtToCm;
	const float AppLenCm = (3.2f + Aws * 0.20f) * WindCompassLocal::FtToCm;
	const bool bAppOn = Boat->IsSailing();
	const float PlaneZ = 6.f;

	float TrueSeat = 0.f, TrueAlt = 0.f;
	float AppSeat = 0.f, AppAlt = 0.f;
	float NorthSeat = 0.f, NorthAlt = 0.f;

	PlaceWindArrow(TrueArrowW.Get(), TrueToRad, TrueLenCm, PlaneZ, EFaceLetter::P, TrueSeat, TrueAlt);
	PlaceWindArrow(AppArrowW.Get(), AppToRad, AppLenCm, PlaneZ, EFaceLetter::A, AppSeat, AppAlt);

	const float NorthRad = FMath::DegreesToRadians(-Hdg);
	PlaceNorthArrow(NorthArrowW.Get(), NorthRad, PlaneZ + 0.5f, NorthSeat, NorthAlt);

	// Re-apply mats every update (procedural mesh section rebuild can drop them).
	if (UProceduralMeshComponent* T = TrueArrowW.Get())
	{
		if (TrueMatW.IsValid()) T->SetMaterial(0, TrueMatW.Get());
		if (MarkMatW.IsValid()) T->SetMaterial(1, MarkMatW.Get());
	}
	if (UProceduralMeshComponent* A = AppArrowW.Get())
	{
		if (AppMatW.IsValid()) A->SetMaterial(0, AppMatW.Get());
		if (MarkMatW.IsValid()) A->SetMaterial(1, MarkMatW.Get());
	}
	if (UProceduralMeshComponent* N = NorthArrowW.Get())
	{
		if (NorthMatW.IsValid()) N->SetMaterial(0, NorthMatW.Get());
		if (MarkMatW.IsValid()) N->SetMaterial(1, MarkMatW.Get());
	}
	if (UProceduralMeshComponent* S = StaticMeshW.Get())
	{
		if (RingMatW.IsValid()) S->SetMaterial(0, RingMatW.Get());
		if (LubberMatW.IsValid()) S->SetMaterial(1, LubberMatW.Get());
	}

	SetMeshVisible(TrueArrowW.Get(), true);
	SetMeshVisible(AppArrowW.Get(), bAppOn);
	SetMeshVisible(NorthArrowW.Get(), true);
	SetMeshVisible(StaticMeshW.Get(), true);

	// Outer degree labels
	const float LabelOut = RadiusCm + TrueAlt + 5.2f * WindCompassLocal::FtToCm;
	const float AppLabelOut = RadiusCm + AppAlt + 5.2f * WindCompassLocal::FtToCm;

	int32 Twd = FMath::RoundToInt(FMath::Fmod(Boat->GetTrueWindDirDeg(), 360.f));
	if (Twd < 0) Twd += 360;
	const FString TrueTxt = FString::Printf(TEXT("%03d°"), Twd);
	const FString AppTxt = FormatSignedAwa(Boat->GetApparentWindAngleDeg());

	FVector CamLoc = BoatLoc + FVector(0.f, -500.f, 200.f);
	if (UWorld* World = Boat->GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				CamLoc = PC->PlayerCameraManager->GetCameraLocation();
			}
		}
	}

	auto PlaceLabel = [&](UTextRenderComponent* Lab, const FString& Txt, float SeatRad, float OutR, bool bShow, FString& Cache, const FColor& BodyCol)
	{
		if (!Lab) return;
		if (!bShow)
		{
			SetLabelVisible(Lab, false);
			return;
		}
		SetLabelVisible(Lab, true);
		if (Cache != Txt)
		{
			Cache = Txt;
			Lab->SetText(FText::FromString(Txt));
		}
		const float Cx = FMath::Cos(SeatRad);
		const float Cy = FMath::Sin(SeatRad);
		Lab->SetRelativeLocation(FVector(OutR * Cx, OutR * Cy, PlaneZ + 28.f));
		FVector ToCam = CamLoc - Lab->GetComponentLocation();
		ToCam.Z = 0.f;
		if (!ToCam.IsNearlyZero())
		{
			Lab->SetWorldRotation(FRotator(0.f, ToCam.Rotation().Yaw, 0.f));
		}
		Lab->SetTextRenderColor(BodyCol);
	};

	PlaceLabel(TrueLabelW.Get(), TrueTxt, TrueSeat, LabelOut, true, LastTrueTxt, WindCompassLocal::LabelTrue);
	PlaceLabel(AppLabelW.Get(), AppTxt, AppSeat, AppLabelOut, bAppOn, LastAppTxt, WindCompassLocal::LabelApp);
}
