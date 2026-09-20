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
	/** Shared local Z for ring + wings (flat instrument plane). */
	static constexpr float PlaneZ = 10.f;

	static const FLinearColor RingWhite(0.92f, 0.93f, 0.95f, 1.f);
	static const FLinearColor LubberGold(0.90f, 0.74f, 0.22f, 1.f);
	static const FLinearColor TrueCyan(0.15f, 0.72f, 0.80f, 1.f);
	static const FLinearColor AppAmber(0.95f, 0.55f, 0.14f, 1.f);
	static const FLinearColor NorthRed(0.88f, 0.18f, 0.14f, 1.f);
	/** Dark ink for P/A/N — not pure black so unlit still reads. */
	static const FLinearColor MarkInk(0.12f, 0.12f, 0.14f, 1.f);
	static const FLinearColor VtxWhite = FLinearColor::White;

	static const FColor LabelTrue(55, 200, 215, 255);
	static const FColor LabelApp(255, 150, 55, 255);

	/** Flat triangle, winding forced so front faces +Z (camera above). Two-sided mat covers underside. */
	static void AppendTri(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		const FVector& A, const FVector& B, const FVector& C,
		const FLinearColor& Col)
	{
		FVector Pa = A, Pb = B, Pc = C;
		FVector N = FVector::CrossProduct(Pb - Pa, Pc - Pa).GetSafeNormal();
		if (N.IsNearlyZero()) N = FVector::UpVector;
		if (N.Z < 0.f)
		{
			Swap(Pb, Pc);
			N = -N;
		}
		const int32 Base = Verts.Num();
		Verts.Add(Pa); Verts.Add(Pb); Verts.Add(Pc);
		Norms.Add(N); Norms.Add(N); Norms.Add(N);
		UVs.Add(FVector2D(0.f, 0.f)); UVs.Add(FVector2D(1.f, 0.f)); UVs.Add(FVector2D(0.5f, 1.f));
		Colors.Add(Col); Colors.Add(Col); Colors.Add(Col);
		Tris.Add(Base); Tris.Add(Base + 1); Tris.Add(Base + 2);
	}

	static void AppendQuad(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3,
		const FLinearColor& Col)
	{
		AppendTri(Verts, Tris, Norms, UVs, Colors, P0, P1, P2, Col);
		AppendTri(Verts, Tris, Norms, UVs, Colors, P0, P2, P3, Col);
	}

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
		AppendQuad(Verts, Tris, Norms, UVs, Colors, A + Side, A - Side, B - Side, B + Side, Col);
	}

	static void AppendTick(
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors,
		float Rad, float R0, float R1, float HalfThick, float Z, const FLinearColor& Col)
	{
		const float Cx = FMath::Cos(Rad);
		const float Cy = FMath::Sin(Rad);
		const float Tx = -Cy;
		const float Ty = Cx;
		const FVector P0((R0 * Cx) + Tx * HalfThick, (R0 * Cy) + Ty * HalfThick, Z);
		const FVector P1((R0 * Cx) - Tx * HalfThick, (R0 * Cy) - Ty * HalfThick, Z);
		const FVector P2((R1 * Cx) - Tx * HalfThick, (R1 * Cy) - Ty * HalfThick, Z);
		const FVector P3((R1 * Cx) + Tx * HalfThick, (R1 * Cy) + Ty * HalfThick, Z);
		AppendQuad(Verts, Tris, Norms, UVs, Colors, P0, P1, P2, P3, Col);
	}

	static FVector LetterPt(float SeatRad, float MidR, float Z, float U, float V, float Scale)
	{
		const float Cx = FMath::Cos(SeatRad);
		const float Cy = FMath::Sin(SeatRad);
		const FVector RadDir(Cx, Cy, 0.f);
		const FVector TanDir(-Cy, Cx, 0.f);
		return FVector(MidR * Cx, MidR * Cy, Z) + TanDir * (U * Scale) + RadDir * (V * Scale);
	}

	static void CommitSection(
		UProceduralMeshComponent* Mesh, int32 Section,
		TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Norms,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors)
	{
		if (!Mesh) return;
		TArray<FProcMeshTangent> Tangents;
		if (Mesh->GetNumSections() > Section)
		{
			Mesh->ClearMeshSection(Section);
		}
		Mesh->CreateMeshSection_LinearColor(Section, Verts, Tris, Norms, UVs, Colors, Tangents, false);
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
	bDispInit = false;
	DispTrueWindFromDeg = 0.f;
	BuiltTrueToRad = BuiltAppToRad = BuiltNorthRad = 1.e9f;
	BuiltTrueLenCm = BuiltAppLenCm = -1.f;
}

UMaterialInstanceDynamic* FWindCompassRing::MakeIndicatorMat(const FLinearColor& Color)
{
	// Prefer dedicated unlit instrument mat (self-lit BaseColor). Never yacht PBR —
	// that was the black fill + white glitch. NavtVertexColor is a temporary fallback
	// (vertex color white × Tint) until create_wind_compass_material.py is run.
	static const TCHAR* Paths[] = {
		TEXT("/Game/Materials/Yacht/M_WindCompass_Unlit.M_WindCompass_Unlit"),
		TEXT("/Game/Materials/Yacht/M_WindCompass_Overlay.M_WindCompass_Overlay"),
		TEXT("/Game/Materials/Navt/M_NavtVertexColor.M_NavtVertexColor"),
	};
	UMaterialInterface* Base = nullptr;
	bool bUnlit = false;
	bool bNavt = false;
	for (const TCHAR* P : Paths)
	{
		Base = LoadObject<UMaterialInterface>(nullptr, P);
		if (!Base) continue;
		const FString S(P);
		bUnlit = S.Contains(TEXT("WindCompass"));
		bNavt = S.Contains(TEXT("NavtVertexColor"));
		break;
	}
	if (!Base)
	{
		UE_LOG(LogSailSim, Error,
			TEXT("WindCompass: missing M_WindCompass_Unlit — run Scripts/create_wind_compass_material.py"));
		Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	}
	if (!Base) return nullptr;

	if (!bUnlit)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogSailSim, Warning,
				TEXT("WindCompass: using fallback mat (close editor + create_wind_compass_material.py for solid unlit look)"));
		}
	}

	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return nullptr;

	FLinearColor C = Color;
	C.A = 1.f;
	Mid->SetVectorParameterValue(TEXT("BaseColor"), C);
	Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
	if (bNavt)
	{
		// BaseColor = VertexColor(white) * Tint * ColorBoost
		Mid->SetVectorParameterValue(TEXT("Tint"), C);
		Mid->SetScalarParameterValue(TEXT("ColorBoost"), 1.6f);
		Mid->SetScalarParameterValue(TEXT("Roughness"), 0.95f);
		Mid->SetScalarParameterValue(TEXT("Night01"), 0.f);
		Mid->SetScalarParameterValue(TEXT("WindowEmissive"), 0.f);
		Mid->SetScalarParameterValue(TEXT("LampEmissive"), 0.f);
		Mid->SetScalarParameterValue(TEXT("DayGlassGlint"), 0.f);
	}
	return Mid;
}

float FWindCompassRing::SampleNightGlow01(const UWorld* World)
{
	if (!World) return 0.f;
	if (const USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
	{
		const uint8 Preset = Ocean->GetActiveEnvPreset();
		switch (Preset)
		{
		case 0: return 0.f;      // FairDay
		case 1: return 0.35f;    // GoldenHour
		case 2: return 0.85f;    // Dusk
		case 3: return 1.f;      // Night
		default: break;
		}
	}
	float BestSun = 0.f;
	for (TActorIterator<ADirectionalLight> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ADirectionalLight* L = *It;
		if (!IsValid(L) || L->GetActorNameOrLabel().Contains(TEXT("SailSim_Moon"))) continue;
		if (UDirectionalLightComponent* C = Cast<UDirectionalLightComponent>(L->GetLightComponent()))
		{
			if (C->IsVisible()) BestSun = FMath::Max(BestSun, C->Intensity);
		}
	}
	const float Night01 = FMath::GetMappedRangeValueClamped(
		FVector2D(0.5f, 7.f), FVector2D(1.f, 0.f), BestSun);
	return FMath::GetMappedRangeValueClamped(FVector2D(0.3f, 0.85f), FVector2D(0.f, 1.f), Night01);
}

void FWindCompassRing::ApplyNightEmissive(float NightGlow01)
{
	using namespace WindCompassLocal;
	const float G = FMath::Clamp(NightGlow01, 0.f, 1.f);

	auto SetLook = [G](UMaterialInstanceDynamic* Mid, const FLinearColor& DayCol, float PeakBoost)
	{
		if (!Mid) return;
		// Day: full chroma, boost 0. Night: slightly deeper + soft boost.
		const FLinearColor NightCol = DayCol * 0.72f;
		const FLinearColor Base = FMath::Lerp(DayCol, NightCol, G);
		const float Boost = FMath::Clamp(PeakBoost * G, 0.f, 0.14f);
		Mid->SetVectorParameterValue(TEXT("BaseColor"), Base);
		Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), Boost);
	};

	SetLook(RingMatW.Get(), RingWhite, RingNightBoost);
	SetLook(LubberMatW.Get(), LubberGold, LubberNightBoost);
	SetLook(TrueMatW.Get(), TrueCyan, TrueNightBoost);
	SetLook(AppMatW.Get(), AppAmber, AppNightBoost);
	SetLook(NorthMatW.Get(), NorthRed, NorthNightBoost);
	if (UMaterialInstanceDynamic* M = MarkMatW.Get())
	{
		M->SetVectorParameterValue(TEXT("BaseColor"), MarkInk);
		M->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.f);
	}
	CachedNightGlow01 = G;
}

static void ExcludeFromReflections(UPrimitiveComponent* Prim)
{
	if (!Prim) return;
	Prim->bVisibleInReflectionCaptures = false;
	Prim->bVisibleInRealTimeSkyCaptures = false;
	Prim->bVisibleInReflections = false;
	Prim->SetVisibleInRayTracing(false);
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
	M->SetCastShadow(false);
	M->RegisterComponent();
	ExcludeFromReflections(M);
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
	if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialOpaque.DefaultTextMaterialOpaque")))
	{
		T->SetTextMaterial(TextMat);
	}
	T->SetTextRenderColor(Color);
	T->SetText(FText::GetEmpty());
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
	const float Z = PlaneZ;
	const float Tube = 0.40f * FtToCm; // readable band width
	const int32 Segs = 72;

	TArray<FVector> RingVerts, LubVerts;
	TArray<int32> RingTris, LubTris;
	TArray<FVector> RingNorms, LubNorms;
	TArray<FVector2D> RingUVs, LubUVs;
	TArray<FLinearColor> RingCols, LubCols;
	RingVerts.Reserve(Segs * 6 + 200);

	// Flat annulus (single layer, two-sided mat).
	for (int32 I = 0; I < Segs; ++I)
	{
		const float A0 = (float(I) / Segs) * 2.f * PI;
		const float A1 = (float(I + 1) / Segs) * 2.f * PI;
		const float C0 = FMath::Cos(A0), S0 = FMath::Sin(A0);
		const float C1 = FMath::Cos(A1), S1 = FMath::Sin(A1);
		const float Ri = R - Tube;
		const float Ro = R + Tube;
		const FVector I0(Ri * C0, Ri * S0, Z);
		const FVector I1(Ri * C1, Ri * S1, Z);
		const FVector O0(Ro * C0, Ro * S0, Z);
		const FVector O1(Ro * C1, Ro * S1, Z);
		AppendQuad(RingVerts, RingTris, RingNorms, RingUVs, RingCols, I0, O0, O1, I1, VtxWhite);
	}

	// Degree ticks slightly above the ring (no z-fight).
	const float TickZ = Z + 2.f;
	for (int32 Deg = 0; Deg < 360; Deg += 15)
	{
		const bool bMajor = (Deg % 90 == 0);
		const bool bMid = (Deg % 45 == 0);
		const float Rad = FMath::DegreesToRadians(float(Deg));
		const float Inset = (bMajor ? 2.2f : (bMid ? 1.5f : 0.95f)) * FtToCm;
		const float HalfT = (bMajor ? 0.28f : (bMid ? 0.20f : 0.14f)) * FtToCm * 0.5f;
		AppendTick(RingVerts, RingTris, RingNorms, RingUVs, RingCols,
			Rad, R - Inset, R + Tube * 0.2f, HalfT, TickZ, VtxWhite);
	}

	// Bow lubber (gold section) — flat tri slightly above ring.
	{
		const float Y = Z + 2.5f;
		const FVector Tip(R + 2.6f * FtToCm, 0.f, Y);
		const FVector A(R + 0.3f * FtToCm, -1.4f * FtToCm, Y);
		const FVector B(R + 0.3f * FtToCm, 1.4f * FtToCm, Y);
		AppendTri(LubVerts, LubTris, LubNorms, LubUVs, LubCols, Tip, A, B, VtxWhite);
	}

	Mesh->ClearAllMeshSections();
	CommitSection(Mesh, 0, RingVerts, RingTris, RingNorms, RingUVs, RingCols);
	CommitSection(Mesh, 1, LubVerts, LubTris, LubNorms, LubUVs, LubCols);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
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

	const float Z = PlaneZCm + 2.f;
	const float HalfW = 0.10f * ScaleCm;

	auto Pt = [&](float U, float V) -> FVector
	{
		return LetterPt(SeatRad, MidR, Z, U, V, ScaleCm);
	};
	auto Stroke = [&](float U0, float V0, float U1, float V1)
	{
		AppendStroke(Verts, Tris, Norms, UVs, Colors, Pt(U0, V0), Pt(U1, V1), HalfW, VtxWhite);
	};

	switch (Letter)
	{
	case EFaceLetter::P:
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

	CommitSection(Mesh, 1, Verts, Tris, Norms, UVs, Colors);
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

	// Flat single triangle (tip on rim, base outboard).
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
	CommitSection(Mesh, 0, Verts, Tris, Norms, UVs, Colors);

	const float MidR = R + Alt * (2.f / 3.f);
	// Small letter — was up to 90cm and covered the whole wing in black.
	const float LetterScale = FMath::Clamp(Side * 0.22f, 18.f, 42.f);
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
	CommitSection(Mesh, 0, Verts, Tris, Norms, UVs, Colors);

	const float MidR = R + Alt * (1.f / 3.f);
	const float LetterScale = FMath::Clamp(Side * 0.22f, 18.f, 42.f);
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
	const bool bOk = bBuilt && RootW.IsValid() && TrueArrowW.IsValid() && NorthArrowW.IsValid()
		&& MarkMatW.IsValid() && BuiltMatRecipe == MatRecipeVersion;
	if (bOk) return;
	if (bBuilt) Clear();
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

	RingNightBoost = FMath::Max(SoftFill * 2.2f, 0.12f);
	TrueNightBoost = FMath::Max(SoftFill * 1.4f, 0.08f);
	AppNightBoost = FMath::Max(SoftFill * 1.4f, 0.08f);
	NorthNightBoost = FMath::Max(SoftFill * 1.5f, 0.09f);
	LubberNightBoost = FMath::Max(SoftFill * 1.8f, 0.10f);

	using namespace WindCompassLocal;
	RingMatW = MakeIndicatorMat(RingWhite);
	TrueMatW = MakeIndicatorMat(TrueCyan);
	AppMatW = MakeIndicatorMat(AppAmber);
	NorthMatW = MakeIndicatorMat(NorthRed);
	LubberMatW = MakeIndicatorMat(LubberGold);
	MarkMatW = MakeIndicatorMat(MarkInk);

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

	TrueLabelW = MakeLabel(Boat, Root, TEXT("WindTrueLabel"), LabelTrue);
	AppLabelW = MakeLabel(Boat, Root, TEXT("WindAppLabel"), LabelApp);

	bBuilt = true;
	BuiltMatRecipe = MatRecipeVersion;
	CachedNightGlow01 = -1.f;
	ApplyNightEmissive(SampleNightGlow01(Boat->GetWorld()));
	UE_LOG(LogSailSim, Log, TEXT("WindCompass: flat unlit rose ready (recipe=%d)"), MatRecipeVersion);
}

void FWindCompassRing::Update(ASailBoatPawn* Boat, float DeltaSeconds)
{
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 1.f / 20.f);
	if (!bEnabled || !Boat)
	{
		if (RootW.IsValid()) RootW->SetVisibility(false, true);
		bDispInit = false;
		return;
	}

	const float Tws = Boat->GetTrueWindSpeedKn();
	if (Tws < MinTwsKn)
	{
		if (RootW.IsValid()) RootW->SetVisibility(false, true);
		bDispInit = false;
		return;
	}

	EnsureBuilt(Boat);
	USceneComponent* Root = RootW.Get();
	if (!Root) return;
	Root->SetVisibility(true, true);

	const float NightGlow = SampleNightGlow01(Boat->GetWorld());
	if (!FMath::IsNearlyEqual(NightGlow, CachedNightGlow01, 0.02f))
	{
		ApplyNightEmissive(NightGlow);
	}

	const FVector BoatLoc = Boat->GetActorLocation();
	const float RawHdg = Boat->GetHeadingDeg();
	// True wind is a WORLD "from" direction. Apparent is boat-relative (AWA).
	const float RawTrueFromDeg = Boat->GetTrueWindDirDeg();
	const float RawAppTo = FMath::DegreesToRadians(Boat->GetApparentWindAngleDeg()) + PI;
	const float Aws = Boat->GetApparentWindSpeedKn();
	const float RawTrueLen = (3.2f + Tws * 0.20f) * WindCompassLocal::FtToCm;
	const float RawAppLen = (3.2f + Aws * 0.20f) * WindCompassLocal::FtToCm;

	auto SmoothAngleDeg = [](float Current, float Target, float Alpha) -> float
	{
		return FMath::UnwindDegrees(Current + FMath::FindDeltaAngleDegrees(Current, Target) * Alpha);
	};
	auto SmoothAngleRad = [](float Current, float Target, float Alpha) -> float
	{
		float D = Target - Current;
		while (D > PI) D -= 2.f * PI;
		while (D < -PI) D += 2.f * PI;
		return Current + D * Alpha;
	};
	auto AngDiff = [](float A, float B) -> float
	{
		float D = B - A;
		while (D > PI) D -= 2.f * PI;
		while (D < -PI) D += 2.f * PI;
		return FMath::Abs(D);
	};

	const float Alpha = 1.f - FMath::Exp(-12.f * Dt);
	if (!bDispInit)
	{
		DispHdgDeg = RawHdg;
		DispTrueWindFromDeg = RawTrueFromDeg;
		DispAppToRad = RawAppTo;
		DispTrueLenCm = RawTrueLen;
		DispAppLenCm = RawAppLen;
		bDispInit = true;
	}
	else
	{
		DispHdgDeg = SmoothAngleDeg(DispHdgDeg, RawHdg, Alpha);
		// Smooth true wind in WORLD space only — never as (TWD − heading).
		DispTrueWindFromDeg = SmoothAngleDeg(DispTrueWindFromDeg, RawTrueFromDeg, Alpha);
		// AWA is boat-relative; fine to smooth in local frame.
		DispAppToRad = SmoothAngleRad(DispAppToRad, RawAppTo, Alpha);
		DispTrueLenCm = FMath::Lerp(DispTrueLenCm, RawTrueLen, Alpha);
		DispAppLenCm = FMath::Lerp(DispAppLenCm, RawAppLen, Alpha);
	}

	// Clear of water / hull bob.
	const float WaterClearCm = 24.f;
	Root->SetWorldLocation(FVector(BoatLoc.X, BoatLoc.Y, BoatLoc.Z + LiftCm + WaterClearCm));
	Root->SetWorldRotation(FRotator(0.f, DispHdgDeg, 0.f));
	Root->SetWorldScale3D(FVector::OneVector);

	// Root is boat-yawed. True local angle must be (worldFrom − dispHdg) so
	// world = DispHdg + local = true wind (stable when turning).
	// PlaceWindArrow: FlowTo = from+π, tip sits on rim at "from" (SeatRad = from).
	const float TrueToRad =
		FMath::DegreesToRadians(DispTrueWindFromDeg - DispHdgDeg) + PI;
	const float AppToRad = DispAppToRad;
	const float TrueLenCm = DispTrueLenCm;
	const float AppLenCm = DispAppLenCm;
	const bool bAppOn = Boat->IsSailing();
	const float PlaneZ = WindCompassLocal::PlaneZ;
	// World north fixed: local = −heading.
	const float NorthRad = FMath::DegreesToRadians(-DispHdgDeg);

	constexpr float AngleEps = 0.012f;
	constexpr float LenEpsCm = 4.f;

	float TrueSeat = 0.f, TrueAlt = 0.f;
	float AppSeat = 0.f, AppAlt = 0.f;
	float NorthSeat = 0.f, NorthAlt = 0.f;

	const bool bNeedTrue = BuiltTrueLenCm < 0.f
		|| AngDiff(BuiltTrueToRad, TrueToRad) > AngleEps
		|| FMath::Abs(TrueLenCm - BuiltTrueLenCm) > LenEpsCm;
	const bool bNeedApp = BuiltAppLenCm < 0.f
		|| AngDiff(BuiltAppToRad, AppToRad) > AngleEps
		|| FMath::Abs(AppLenCm - BuiltAppLenCm) > LenEpsCm;
	const bool bNeedNorth = BuiltNorthRad > 1.e8f
		|| AngDiff(BuiltNorthRad, NorthRad) > AngleEps;

	if (bNeedTrue)
	{
		PlaceWindArrow(TrueArrowW.Get(), TrueToRad, TrueLenCm, PlaneZ, EFaceLetter::P, TrueSeat, TrueAlt);
		BuiltTrueToRad = TrueToRad;
		BuiltTrueLenCm = TrueLenCm;
		if (UProceduralMeshComponent* T = TrueArrowW.Get())
		{
			if (TrueMatW.IsValid()) T->SetMaterial(0, TrueMatW.Get());
			if (MarkMatW.IsValid()) T->SetMaterial(1, MarkMatW.Get());
		}
	}
	else
	{
		TrueSeat = TrueToRad + PI;
		const float LenFt = TrueLenCm / WindCompassLocal::FtToCm;
		const float Side = (3.2f + LenFt * 0.18f) * WindCompassLocal::FtToCm;
		TrueAlt = Side * 0.866025403784f;
	}

	if (bNeedApp)
	{
		PlaceWindArrow(AppArrowW.Get(), AppToRad, AppLenCm, PlaneZ, EFaceLetter::A, AppSeat, AppAlt);
		BuiltAppToRad = AppToRad;
		BuiltAppLenCm = AppLenCm;
		if (UProceduralMeshComponent* A = AppArrowW.Get())
		{
			if (AppMatW.IsValid()) A->SetMaterial(0, AppMatW.Get());
			if (MarkMatW.IsValid()) A->SetMaterial(1, MarkMatW.Get());
		}
	}
	else
	{
		AppSeat = AppToRad + PI;
		const float LenFt = AppLenCm / WindCompassLocal::FtToCm;
		const float Side = (3.2f + LenFt * 0.18f) * WindCompassLocal::FtToCm;
		AppAlt = Side * 0.866025403784f;
	}

	if (bNeedNorth)
	{
		PlaceNorthArrow(NorthArrowW.Get(), NorthRad, PlaneZ + 1.f, NorthSeat, NorthAlt);
		BuiltNorthRad = NorthRad;
		if (UProceduralMeshComponent* N = NorthArrowW.Get())
		{
			if (NorthMatW.IsValid()) N->SetMaterial(0, NorthMatW.Get());
			if (MarkMatW.IsValid()) N->SetMaterial(1, MarkMatW.Get());
		}
	}
	else
	{
		NorthSeat = NorthRad;
		const float Side = 4.2f * WindCompassLocal::FtToCm;
		NorthAlt = Side * 0.866025403784f;
	}

	SetMeshVisible(TrueArrowW.Get(), true);
	SetMeshVisible(AppArrowW.Get(), bAppOn);
	SetMeshVisible(NorthArrowW.Get(), true);
	SetMeshVisible(StaticMeshW.Get(), true);

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
