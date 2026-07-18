#include "Sailing/Ocean/BoatWakeSim.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "SailSimUE.h"

#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"


void FBoatWakeSim::Clear()
{
	TimeSec = 0.f;
	RebuildAccum = 0.f;
	CachedAmp = 0.f;
	if (UProceduralMeshComponent* Mesh = MeshW.Get())
	{
		if (Mesh->GetNumSections() > 0)
		{
			Mesh->ClearAllMeshSections();
		}
		Mesh->SetVisibility(false);
		Mesh->SetHiddenInGame(true);
		Mesh->DestroyComponent();
	}
	MeshW.Reset();
	WaterMidW.Reset();
	bMeshReady = false;
}

float FBoatWakeSim::SpeedAmp(float Kn) const
{
	if (Kn < MinSpeedKn) return 0.f;
	const float Hs = FMath::Max(4.f, HullSpeedKn);
	const float T = FMath::Clamp(Kn / Hs, 0.f, 1.25f);
	// Soft onset, plateaus near hull speed (web wake amp pattern).
	return FMath::Clamp(0.12f + 0.88f * FMath::Pow(T, 0.9f), 0.f, 1.15f) * Strength;
}

FVector FBoatWakeSim::SampleBaseSurface(const ASailBoatPawn* Boat, const FVector& WorldXY) const
{
	FVector Surf = WorldXY;
	FVector N = FVector::UpVector;
	if (Boat && Boat->SampleWaterSurfacePublic(WorldXY, Surf, N))
	{
		return Surf;
	}
	const float Z = Boat ? Boat->GetActorLocation().Z : 0.f;
	return FVector(WorldXY.X, WorldXY.Y, Z);
}

float FBoatWakeSim::WakeHeightLocal(
	float LocalX, float LocalY, float HalfLoa, float HalfBeam, float Amp) const
{
	if (Amp < 0.02f) return 0.f;

	// Only disturb water at / behind the hull (slight forward for bow wave).
	const float BowX = HalfLoa * 0.95f;
	const float SternX = -HalfLoa * 0.95f;
	if (LocalX > BowX + 80.f) return 0.f;

	// Distance aft of stern (0 at stern, + behind).
	const float Behind = FMath::Max(0.f, SternX - LocalX);
	// Forward of stern toward bow (for bow-wave region).
	const float AlongHull = FMath::Clamp((LocalX - SternX) / FMath::Max(1.f, BowX - SternX), 0.f, 1.f);

	const float KTan = FMath::Tan(FMath::DegreesToRadians(KelvinHalfAngleDeg));
	// Ideal Kelvin arm lateral offset grows with distance aft.
	const float ArmLat = FMath::Max(HalfBeam * 0.55f, Behind * KTan);
	const float DistArm = FMath::Min(FMath::Abs(LocalY - ArmLat), FMath::Abs(LocalY + ArmLat));

	// Envelope: dies with range and off-track.
	const float RangeDecay = FMath::Exp(-Behind / (PatchAftCm * 0.55f));
	const float LatDecay = FMath::Exp(-FMath::Abs(LocalY) / (PatchHalfWidthCm * 0.55f));

	// --- Centerline / hull trough (hollow behind stern) ---
	const float TroughWidth = FMath::Max(HalfBeam * 0.9f, 80.f + Behind * 0.08f);
	const float TroughProfile = FMath::Exp(-FMath::Square(LocalY / TroughWidth));
	const float TroughAlong = FMath::SmoothStep(0.f, HalfLoa * 0.35f, Behind)
		* FMath::Exp(-Behind / (PatchAftCm * 0.4f));
	const float Trough = -PeakTroughCm * Amp * TroughProfile * TroughAlong;

	// --- Divergent arm crests (Kelvin V) ---
	const float ArmWidth = FMath::Lerp(45.f, 120.f, FMath::Clamp(Behind / 1500.f, 0.f, 1.f));
	const float ArmProfile = FMath::Exp(-FMath::Square(DistArm / ArmWidth));
	const float ArmPhase = Behind * 0.038f - TimeSec * 2.8f * Amp;
	const float ArmCrest = PeakArmCrestCm * Amp * ArmProfile * RangeDecay
		* FMath::Max(0.f, FMath::Sin(ArmPhase));

	// --- Transverse waves (perpendicular crests behind hull) ---
	const float TransEnv = RangeDecay * LatDecay
		* FMath::SmoothStep(0.f, HalfLoa * 0.5f, Behind);
	const float TransPhase = Behind * 0.022f - TimeSec * 2.2f;
	const float Trans = PeakTransverseCm * Amp * TransEnv * FMath::Sin(TransPhase);

	// --- Bow / shoulder push (small positive near entry when making way) ---
	float BowPush = 0.f;
	if (LocalX > SternX && LocalX < BowX + 40.f)
	{
		const float Side = FMath::Abs(LocalY) - HalfBeam * 0.35f;
		if (Side > -HalfBeam * 0.2f && Side < HalfBeam * 1.2f)
		{
			const float SideEnv = FMath::Exp(-FMath::Square(Side / (HalfBeam * 0.55f)));
			const float LonEnv = FMath::Sin(AlongHull * PI); // 0 at stern/bow tips, peak mid-entry
			BowPush = PeakArmCrestCm * 0.35f * Amp * SideEnv * LonEnv
				* FMath::Max(0.f, FMath::Sin(TimeSec * 3.5f + LocalY * 0.02f));
		}
	}

	return (Trough + ArmCrest + Trans + BowPush);
}

float FBoatWakeSim::SampleWakeHeightCm(const ASailBoatPawn* Boat, const FVector& WorldXY) const
{
	if (!bEnabled || !Boat || CachedAmp < 0.02f) return 0.f;
	const FTransform Xf = Boat->GetActorTransform();
	const FVector Local = Xf.InverseTransformPosition(WorldXY);
	const float HalfLoa = FMath::Max(200.f, Boat->GetHullLengthCm() * 0.48f);
	const float HalfBeam = FMath::Max(80.f, Boat->GetHullBeamCm() * 0.42f);
	return WakeHeightLocal(Local.X, Local.Y, HalfLoa, HalfBeam, CachedAmp);
}

void FBoatWakeSim::EnsureMaterial(ASailBoatPawn* Boat, UProceduralMeshComponent* Mesh)
{
	if (!Mesh) return;
	if (WaterMidW.IsValid())
	{
		Mesh->SetMaterial(0, WaterMidW.Get());
		return;
	}

	// Prefer the live ocean water material so the patch reads as the same sea.
	UMaterialInterface* Base = nullptr;
	if (UWorld* World = Boat ? Boat->GetWorld() : nullptr)
	{
		for (TActorIterator<AWaterBody> It(World); It; ++It)
		{
			if (UWaterBodyComponent* Comp = It->GetWaterBodyComponent())
			{
				// Runtime MID first (matches polished open-ocean look).
				if (UMaterialInstanceDynamic* Inst = Comp->GetWaterMaterialInstance())
				{
					Base = Inst;
					break;
				}
				if (UMaterialInterface* M = Comp->GetWaterMaterial())
				{
					Base = M;
					break;
				}
			}
		}
	}

	// Fallbacks: translucent simple water-like unlit (not bright foam).
	if (!Base)
	{
		static const TCHAR* Fallbacks[] = {
			TEXT("/Engine/EngineDebugMaterials/M_SimpleTranslucent.M_SimpleTranslucent"),
			TEXT("/Engine/EngineMaterials/DefaultTextMaterialTranslucent.DefaultTextMaterialTranslucent"),
			TEXT("/Game/Materials/Yacht/M_Yacht_PBR_TwoSided.M_Yacht_PBR_TwoSided"),
			TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"),
		};
		for (const TCHAR* Path : Fallbacks)
		{
			Base = LoadObject<UMaterialInterface>(nullptr, Path);
			if (Base) break;
		}
	}
	if (!Base) return;

	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, GetTransientPackage());
	if (!Mid) return;

	// Tint toward open-sea; keep unlit fallbacks dark-blue not white.
	const FLinearColor Sea(0.02f, 0.12f, 0.18f, 0.85f);
	const FLinearColor Foam(0.75f, 0.85f, 0.90f, 1.f);
	Mid->SetVectorParameterValue(TEXT("Color"), Sea);
	Mid->SetVectorParameterValue(TEXT("BaseColor"), Sea);
	Mid->SetVectorParameterValue(TEXT("Base Color"), Sea);
	// Soft foam only as mild emissive — crest vertex colors push this further.
	Mid->SetVectorParameterValue(TEXT("EmissiveColor"), Foam * 0.08f);
	Mid->SetScalarParameterValue(TEXT("EmissiveBoost"), 0.06f);
	Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.06f);
	Mid->SetScalarParameterValue(TEXT("Emissive"), 0.06f);
	Mid->SetScalarParameterValue(TEXT("Roughness"), 0.08f);
	Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	Mid->SetScalarParameterValue(TEXT("Specular"), 0.65f);
	Mid->SetScalarParameterValue(TEXT("Opacity"), 0.92f);
	// Match ocean look if water MI exposes these (SailOceanSubsystem polish keys).
	Mid->SetScalarParameterValue(TEXT("Absorption"), 0.15f);
	Mid->SetScalarParameterValue(TEXT("Scattering"), 0.4f);

	WaterMidW = Mid;
	Mesh->SetMaterial(0, Mid);
}

void FBoatWakeSim::EnsureMesh(ASailBoatPawn* Boat, USceneComponent* AttachRoot)
{
	if (!Boat || !AttachRoot) return;
	if (MeshW.IsValid()) return;

	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Boat, TEXT("BoatWakeMesh"));
	Mesh->SetupAttachment(AttachRoot);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->bCastContactShadow = false;
	Mesh->bCastDynamicShadow = false;
	Mesh->bCastStaticShadow = false;
	Mesh->SetReceivesDecals(false);
	Mesh->bNeverDistanceCull = true;
	Mesh->SetCullDistance(0.f);
	Mesh->SetBoundsScale(20.f);
	// Slightly above main water tess so deformed patch wins z without looking like a prop.
	Mesh->SetTranslucentSortPriority(5);
	Mesh->bUseAsyncCooking = false;
	Mesh->SetVisibility(false);
	Mesh->SetHiddenInGame(true);
	Mesh->RegisterComponent();
	Boat->AddInstanceComponent(Mesh);

	EnsureMaterial(Boat, Mesh);
	MeshW = Mesh;
	bMeshReady = true;
}

void FBoatWakeSim::Update(ASailBoatPawn* Boat, float DeltaSeconds)
{
	if (!bEnabled || !Boat) return;
	EnsureMesh(Boat, Boat->GetRootComponent());
	if (!MeshW.IsValid()) return;

	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.1f);
	TimeSec += Dt;
	const float Kn = FMath::Abs(Boat->GetSpeedKnots());
	CachedAmp = SpeedAmp(Kn);

	RebuildAccum += Dt;
	// Always rebuild promptly when stopping so the patch clears; otherwise throttle.
	const bool bNeedClear = (CachedAmp < 0.02f && MeshW->GetNumSections() > 0);
	if (!bNeedClear && RebuildAccum < RebuildIntervalSec)
	{
		return;
	}
	RebuildAccum = 0.f;
	RebuildMesh(Boat);
}

void FBoatWakeSim::RebuildMesh(ASailBoatPawn* Boat)
{
	UProceduralMeshComponent* Mesh = MeshW.Get();
	if (!Mesh || !Boat) return;

	const float Amp = CachedAmp;
	if (Amp < 0.02f)
	{
		if (Mesh->GetNumSections() > 0)
		{
			Mesh->ClearMeshSection(0);
		}
		Mesh->SetVisibility(false);
		Mesh->SetHiddenInGame(true);
		return;
	}

	const float HalfLoa = FMath::Max(200.f, Boat->GetHullLengthCm() * 0.48f);
	const float HalfBeam = FMath::Max(80.f, Boat->GetHullBeamCm() * 0.42f);
	const FTransform BoatXf = Boat->GetActorTransform();
	// Yaw-only frame so pitch/roll of the hull doesn't tilt the whole sea patch.
	const FRotator YawOnly(0.f, BoatXf.Rotator().Yaw, 0.f);
	const FVector Origin = BoatXf.GetLocation();
	const FVector Fwd = YawOnly.Vector().GetSafeNormal2D();
	const FVector Right(-Fwd.Y, Fwd.X, 0.f);

	const int32 Nx = FMath::Clamp(GridAlong, 8, 96);
	const int32 Ny = FMath::Clamp(GridAcross, 6, 64);
	// Local extents: slightly forward of stern through far wake; full width.
	const float X0 = -PatchAftCm;                 // far aft
	const float X1 = FMath::Max(PatchFwdCm, HalfLoa * 0.15f); // near / slight forward
	const float Y0 = -PatchHalfWidthCm;
	const float Y1 = PatchHalfWidthCm;

	const int32 NumVerts = Nx * Ny;
	TArray<FVector> Verts;
	TArray<FVector> Norms;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Cols;
	TArray<FProcMeshTangent> Tans;
	TArray<float> Heights; // world Z for normal estimation
	Verts.SetNum(NumVerts);
	Norms.SetNum(NumVerts);
	UV.SetNum(NumVerts);
	Cols.SetNum(NumVerts);
	Tans.SetNum(NumVerts);
	Heights.SetNum(NumVerts);

	// World XY + height field
	for (int32 Iy = 0; Iy < Ny; ++Iy)
	{
		const float V = (Ny == 1) ? 0.5f : float(Iy) / float(Ny - 1);
		const float LocalY = FMath::Lerp(Y0, Y1, V);
		for (int32 Ix = 0; Ix < Nx; ++Ix)
		{
			const float U = (Nx == 1) ? 0.5f : float(Ix) / float(Nx - 1);
			const float LocalX = FMath::Lerp(X0, X1, U);
			const int32 Idx = Iy * Nx + Ix;

			const FVector WorldXY = Origin + Fwd * LocalX + Right * LocalY;
			const FVector Base = SampleBaseSurface(Boat, WorldXY);
			const float Dz = WakeHeightLocal(LocalX, LocalY, HalfLoa, HalfBeam, Amp);
			const FVector WorldP(Base.X, Base.Y, Base.Z + Dz);

			// Store in boat local so the component rides with the hull.
			Verts[Idx] = BoatXf.InverseTransformPosition(WorldP);
			Heights[Idx] = WorldP.Z;
			UV[Idx] = FVector2D(U, V);

			// Mild crest foam (vertex color) — only where surface is raised, not a white ribbon object.
			const float Crest = FMath::Clamp(Dz / FMath::Max(1.f, PeakArmCrestCm * Amp), 0.f, 1.f);
			const float Foam = FMath::Pow(Crest, 1.6f) * 0.55f * Amp;
			// Blend toward pale foam on peaks; base stays sea-colored via material.
			Cols[Idx] = FLinearColor(
				FMath::Lerp(0.15f, 0.85f, Foam),
				FMath::Lerp(0.35f, 0.92f, Foam),
				FMath::Lerp(0.45f, 0.98f, Foam),
				FMath::Lerp(0.75f, 1.f, Foam));
		}
	}

	// Normals from finite differences in world XY grid (better than boat-local).
	const float Dx = (X1 - X0) / FMath::Max(1, Nx - 1);
	const float Dy = (Y1 - Y0) / FMath::Max(1, Ny - 1);
	for (int32 Iy = 0; Iy < Ny; ++Iy)
	{
		for (int32 Ix = 0; Ix < Nx; ++Ix)
		{
			const int32 Idx = Iy * Nx + Ix;
			const int32 Ix0 = FMath::Max(0, Ix - 1);
			const int32 Ix1 = FMath::Min(Nx - 1, Ix + 1);
			const int32 Iy0 = FMath::Max(0, Iy - 1);
			const int32 Iy1 = FMath::Min(Ny - 1, Iy + 1);
			const float Dzx = (Heights[Iy * Nx + Ix1] - Heights[Iy * Nx + Ix0])
				/ FMath::Max(1.f, Dx * float(Ix1 - Ix0));
			const float Dzy = (Heights[Iy1 * Nx + Ix] - Heights[Iy0 * Nx + Ix])
				/ FMath::Max(1.f, Dy * float(Iy1 - Iy0));
			// World normal; convert to boat local for mesh.
			const FVector NWorld = FVector(-Dzx, -Dzy, 1.f).GetSafeNormal();
			const FVector NLocal = BoatXf.InverseTransformVectorNoScale(NWorld).GetSafeNormal();
			Norms[Idx] = NLocal.IsNearlyZero() ? FVector::UpVector : NLocal;
			const FVector TWorld = FVector(1.f, 0.f, Dzx).GetSafeNormal();
			const FVector TLocal = BoatXf.InverseTransformVectorNoScale(TWorld).GetSafeNormal();
			Tans[Idx] = FProcMeshTangent(TLocal.IsNearlyZero() ? FVector(1.f, 0.f, 0.f) : TLocal, false);
		}
	}

	TArray<int32> Tris;
	Tris.Reserve((Nx - 1) * (Ny - 1) * 6);
	for (int32 Iy = 0; Iy < Ny - 1; ++Iy)
	{
		for (int32 Ix = 0; Ix < Nx - 1; ++Ix)
		{
			const int32 I00 = Iy * Nx + Ix;
			const int32 I10 = I00 + 1;
			const int32 I01 = I00 + Nx;
			const int32 I11 = I01 + 1;
			// Upward facing
			Tris.Add(I00); Tris.Add(I01); Tris.Add(I10);
			Tris.Add(I10); Tris.Add(I01); Tris.Add(I11);
		}
	}

	EnsureMaterial(Boat, Mesh);
	if (Mesh->GetNumSections() > 0)
	{
		Mesh->ClearMeshSection(0);
	}
	Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UV, Cols, Tans, false);
	Mesh->SetMaterial(0, WaterMidW.Get());
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
	Mesh->SetCastShadow(false);
}
