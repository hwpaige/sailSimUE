#include "Sailing/SailClothSim.h"
#include "SailSimUE.h"
#include "ProceduralMeshComponent.h"

void FSailClothSim::Clear()
{
	Pos.Reset();
	Prev.Reset();
	FlatRest.Reset();
	ShapeTarget.Reset();
	Normals.Reset();
	bPinned.Reset();
	TriIndices.Reset();
	VertColors.Reset();
	TriIndicesEven.Reset();
	TriIndicesOdd.Reset();
	SeamPaths.Reset();
	SeamRibbonVertCount = 0;
	bCodeZeroPanels = false;
	Springs.Reset();
	Battens.Reset();
	BattenDist.Reset();
	BattenBend.Reset();
	BattenRows.Reset();
	bBattensEngaged = false;
	BattenSettleFrames = 0;
	LuffIndices.Reset();
	LuffU.Reset();
	FootIndices.Reset();
	LeechIndices.Reset();
	HeadRowIndices.Reset();
	ChordU.Reset();
	HeightV.Reset();
	ClewIndex = INDEX_NONE;
	TackIndex = INDEX_NONE;
	HeadIndex = INDEX_NONE;
	ClewPatchMain.Reset();
	ClewPatchJib.Reset();
	bStayValid = false;
	FootRestLen = 0.f;
	LoftFootLen = 0.f;
	LoftLeechChord = 0.f;
	LoftLeechPath = 0.f;
	LoftFootPath = 0.f;
	BoomLenCm = 0.f;
	Nu = Nw = 0;
	bGridTopology = false;
	bInitialized = false;
	LastFillQuality = 0.85f;
	LastCamberCm = 0.f;
	LastStretchRatio = 1.f;
	LastCamberRatio = 0.f;
	LastLuffAmount = 0.f;
	LastPowered01 = 1.f;
	LastMeanCosInc = 0.5f;
	LastWindPushCm = 0.f;
	LiveFootPathCm = LiveLeechPathCm = LiveFootChordCm = LiveLeechChordCm = 0.f;
}

bool FSailClothSim::TryInitGridTopology(int32 NumVerts)
{
	// Known loft grids from sail_geom / web cloth
	if (NumVerts == 44 * 24) { Nu = 44; Nw = 24; return true; }
	if (NumVerts == 40 * 20) { Nu = 40; Nw = 20; return true; }
	if (NumVerts == 40 * 22) { Nu = 40; Nw = 22; return true; }
	if (NumVerts == 36 * 18) { Nu = 36; Nw = 18; return true; }

	// Factor: prefer nu >= nw (height stations ≥ chord stations)
	for (int32 NwCand = 12; NwCand <= 48; ++NwCand)
	{
		if (NumVerts % NwCand != 0) continue;
		const int32 NuCand = NumVerts / NwCand;
		if (NuCand >= 16 && NuCand <= 80 && NuCand >= NwCand - 4)
		{
			Nu = NuCand;
			Nw = NwCand;
			return true;
		}
	}
	return false;
}

bool FSailClothSim::BuildFromMesh(
	UProceduralMeshComponent* Mesh,
	int32 Section,
	ESailRigKind Kind,
	const FVector* OptionalStayTackLocal,
	const FVector* OptionalStayHeadLocal)
{
	Clear();
	if (!Mesh || Section < 0 || Section >= Mesh->GetNumSections())
	{
		return false;
	}

	FProcMeshSection* Sec = Mesh->GetProcMeshSection(Section);
	if (!Sec || Sec->ProcVertexBuffer.Num() < 3 || Sec->ProcIndexBuffer.Num() < 3)
	{
		return false;
	}

	RigKind = Kind;
	// Web SailCloth defaults (webgl-utils.sailing.js hBuildSail / constructor)
	AeroK = (Kind == ESailRigKind::Jib) ? 0.0022f : 0.0036f;
	DragK = 0.00035f;
	Damping = 0.982f;
	Relax = 0.72f;
	PreTen = 0.998f;
	MaxDeltaCm = 0.35f * 30.48f; // web maxDelta 0.35 ft
	ConstraintIters = (Kind == ESailRigKind::Jib) ? 5 : 4; // H_CLOTH_*_ITERS
	InextensiblePasses = 2;
	InextensibleFinalPasses = 3;
	MaxStrain = 1.03f;
	// Near-full outhaul so foot chord ≈ boom E (0.88 left ~0.5 ft of slack on class E)
	Outhaul01 = (Kind == ESailRigKind::Main) ? 0.94f : 0.5f;
	LuffTension01 = (Kind == ESailRigKind::Jib) ? 0.35f : 0.f;
	// Main: shorter leech by default so it doesn't hang open
	LeechTension01 = (Kind == ESailRigKind::Jib) ? 0.15f : 0.62f;
	MaxCamberFrac = (Kind == ESailRigKind::Jib) ? 0.13f : 0.14f;

	SectionIndex = Section;
	const int32 N = Sec->ProcVertexBuffer.Num();
	Pos.SetNum(N);
	Prev.SetNum(N);
	FlatRest.SetNum(N);
	ShapeTarget.SetNum(N);
	Normals.Init(FVector(0.f, 1.f, 0.f), N);
	bPinned.Init(0, N);

	for (int32 I = 0; I < N; ++I)
	{
		const FVector P = Sec->ProcVertexBuffer[I].Position;
		Pos[I] = Prev[I] = FlatRest[I] = ShapeTarget[I] = P;
	}

	bGridTopology = TryInitGridTopology(N);
	if (bGridTopology)
	{
		PinGridTopology(OptionalStayTackLocal, OptionalStayHeadLocal);
		CollectGridEdges();
		ComputeUVParams();
		BuildGridSprings();
		BuildClewPatches();
	}
	else
	{
		UE_LOG(LogSailSim, Warning, TEXT("SailClothSim: no grid for %d verts — heuristic pins"), N);
		if (Kind == ESailRigKind::Main) DetectAndPinMain();
		else DetectAndPinJib(OptionalStayTackLocal, OptionalStayHeadLocal);
		ComputeUVParams();
		CollectFootIndicesHeuristic();
		CollectLeechIndicesHeuristic();

		// Fallback springs from mesh triangles
		TSet<uint64> EdgeKeys;
		auto EdgeKey = [](int32 A, int32 B) -> uint64
		{
			const int32 Lo = FMath::Min(A, B);
			const int32 Hi = FMath::Max(A, B);
			return (uint64(uint32(Lo)) << 32) | uint64(uint32(Hi));
		};
		auto AddSpring = [&](int32 Ia, int32 Ib, float Stiff, ESpringEdge Edge = ESpringEdge::Body)
		{
			if (!Pos.IsValidIndex(Ia) || !Pos.IsValidIndex(Ib) || Ia == Ib) return;
			const uint64 K = EdgeKey(Ia, Ib);
			if (EdgeKeys.Contains(K)) return;
			EdgeKeys.Add(K);
			FSpring S;
			S.A = Ia; S.B = Ib;
			S.FlatRestLen = FVector::Dist(FlatRest[Ia], FlatRest[Ib]);
			S.RestLen = S.FlatRestLen * 0.998f;
			S.Stiffness = Stiff;
			S.Edge = Edge;
			if (S.FlatRestLen > 0.5f) Springs.Add(S);
		};
		const TArray<uint32>& Idx = Sec->ProcIndexBuffer;
		for (int32 T = 0; T + 2 < Idx.Num(); T += 3)
		{
			const int32 A = int32(Idx[T]), B = int32(Idx[T + 1]), C = int32(Idx[T + 2]);
			TriIndices.Add(A); TriIndices.Add(B); TriIndices.Add(C);
			AddSpring(A, B, 1.f); AddSpring(B, C, 1.f); AddSpring(C, A, 1.f);
		}
		for (int32 K = 0; K + 1 < FootIndices.Num(); ++K)
			AddSpring(FootIndices[K], FootIndices[K + 1], 1.4f, ESpringEdge::Foot);
		for (int32 K = 0; K + 1 < LeechIndices.Num(); ++K)
			AddSpring(LeechIndices[K], LeechIndices[K + 1], 1.45f, ESpringEdge::Leech);
	}

	// Always keep tri index buffer for normals (front faces only)
	if (TriIndices.Num() == 0)
	{
		const TArray<uint32>& Idx = Sec->ProcIndexBuffer;
		const int32 Front = Idx.Num();
		// Double-sided meshes duplicate tris — use first half if even and large
		const int32 UseN = (Front > 6 && (Front % 6) == 0) ? Front / 2 : Front;
		TriIndices.Reserve(UseN);
		for (int32 T = 0; T + 2 < UseN; T += 3)
		{
			TriIndices.Add(int32(Idx[T]));
			TriIndices.Add(int32(Idx[T + 1]));
			TriIndices.Add(int32(Idx[T + 2]));
		}
	}

	if (TackIndex != INDEX_NONE && ClewIndex != INDEX_NONE)
	{
		LoftFootLen = FVector::Dist(FlatRest[TackIndex], FlatRest[ClewIndex]);
		FootRestLen = LoftFootLen;
		BoomLenCm = LoftFootLen; // class E
	}
	if (HeadIndex != INDEX_NONE && ClewIndex != INDEX_NONE)
	{
		// Leech chord: prefer head_outer (last leech) over luff head
		const int32 LeechHead = (LeechIndices.Num() > 0) ? LeechIndices.Last() : HeadIndex;
		LoftLeechChord = FVector::Dist(FlatRest[LeechHead], FlatRest[ClewIndex]);
	}
	LoftFootPath = 0.f;
	for (int32 K = 0; K + 1 < FootIndices.Num(); ++K)
		LoftFootPath += FVector::Dist(FlatRest[FootIndices[K]], FlatRest[FootIndices[K + 1]]);
	if (LoftFootPath < 1.f) LoftFootPath = LoftFootLen;

	LoftLeechPath = 0.f;
	for (int32 K = 0; K + 1 < LeechIndices.Num(); ++K)
		LoftLeechPath += FVector::Dist(FlatRest[LeechIndices[K]], FlatRest[LeechIndices[K + 1]]);
	if (LoftLeechPath < 1.f) LoftLeechPath = LoftLeechChord;

	// Web: start on flat loft rest; wind billows the panel (no pre-shape)
	for (int32 I = 0; I < N; ++I)
	{
		Pos[I] = Prev[I] = FlatRest[I];
		ShapeTarget[I] = FlatRest[I];
	}
	ApplyTrimTensions();
	SnapRigToStay();
	if (RigKind == ESailRigKind::Main && ClewIndex != INDEX_NONE)
	{
		ApplyMainBoomConstraints(FlatRest[ClewIndex]);
	}
	ComputeNormals();
	MeasureEdgeLengths();

	bInitialized = Pos.Num() > 0 && Springs.Num() > 0 && LuffIndices.Num() > 0;

	const float FT = 30.48f;
	UE_LOG(LogSailSim, Log,
		TEXT("SailClothSim %s grid=%s %dx%d verts=%d springs=%d | foot=%.2f' (path %.2f') leechChord=%.2f' leechPath=%.2f' | pts foot=%d leech=%d"),
		Kind == ESailRigKind::Main ? TEXT("MAIN") : TEXT("JIB"),
		bGridTopology ? TEXT("yes") : TEXT("no"), Nu, Nw,
		Pos.Num(), Springs.Num(),
		LoftFootLen / FT, LoftFootPath / FT,
		LoftLeechChord / FT, LoftLeechPath / FT,
		FootIndices.Num(), LeechIndices.Num());
	return bInitialized;
}

void FSailClothSim::PinGridTopology(const FVector* OptionalStayTack, const FVector* OptionalStayHead)
{
	// Grid: i height 0..Nu-1, j chord 0..Nw-1
	TackIndex = GridIdx(0, 0);
	ClewIndex = GridIdx(0, Nw - 1);
	HeadIndex = GridIdx(Nu - 1, 0); // head_inner on mast / forestay
	const int32 HeadOuter = GridIdx(Nu - 1, Nw - 1);

	StayTackLocal = OptionalStayTack ? *OptionalStayTack : FlatRest[TackIndex];
	StayHeadLocal = OptionalStayHead ? *OptionalStayHead : FlatRest[HeadIndex];

	// Asym / Code Zero: free-flying luff — pin tack + entire pointed head
	// (head row collapses to one corner; if only j=0 is pinned the outer head
	// verts fly off as a second "ghost" corner).
	if (RigKind == ESailRigKind::AsymSpin)
	{
		LuffIndices.Reset();
		LuffU.Reset();
		for (int32 I = 0; I < Nu; ++I)
		{
			const int32 Idx = GridIdx(I, 0);
			LuffIndices.Add(Idx);
			LuffU.Add((Nu > 1) ? float(I) / float(Nu - 1) : 0.f);
			bPinned[Idx] = 0;
		}
		HeadRowIndices.Reset();
		for (int32 J = 0; J < Nw; ++J)
		{
			const int32 Idx = GridIdx(Nu - 1, J);
			HeadRowIndices.Add(Idx);
			// Whole head ring is the single halyard attachment point
			bPinned[Idx] = 1;
			FlatRest[Idx] = StayHeadLocal;
			Pos[Idx] = Prev[Idx] = ShapeTarget[Idx] = StayHeadLocal;
		}
		bPinned[TackIndex] = 1;
		bPinned[HeadIndex] = 1;
		bPinned[ClewIndex] = 0;
		FlatRest[TackIndex] = StayTackLocal;
		Pos[TackIndex] = Prev[TackIndex] = StayTackLocal;
		bStayValid = true;
		LuffLen = FMath::Max(1.f, FVector::Dist(StayTackLocal, StayHeadLocal));
		return;
	}

	// Luff column j=0 — hard pin to stay line
	LuffIndices.Reset();
	LuffU.Reset();
	for (int32 I = 0; I < Nu; ++I)
	{
		const int32 Idx = GridIdx(I, 0);
		const float U = (Nu > 1) ? float(I) / float(Nu - 1) : 0.f;
		LuffIndices.Add(Idx);
		LuffU.Add(U);
		bPinned[Idx] = 1;
	}
	// Head row i=Nu-1 — headboard pins at loft positions (not remapped)
	HeadRowIndices.Reset();
	for (int32 J = 0; J < Nw; ++J)
	{
		const int32 Idx = GridIdx(Nu - 1, J);
		HeadRowIndices.Add(Idx);
		bPinned[Idx] = 1;
	}

	// Project luff onto stay (main mast / jib forestay)
	FVector Dir = StayHeadLocal - StayTackLocal;
	LuffLen = Dir.Size();
	if (LuffLen < 1.f)
	{
		StayTackLocal = FlatRest[TackIndex];
		StayHeadLocal = FlatRest[HeadIndex];
		Dir = StayHeadLocal - StayTackLocal;
		LuffLen = FMath::Max(1.f, Dir.Size());
	}
	Dir /= LuffLen;
	for (int32 K = 0; K < LuffIndices.Num(); ++K)
	{
		const int32 Idx = LuffIndices[K];
		const FVector On = StayTackLocal + Dir * (LuffU[K] * LuffLen);
		FlatRest[Idx] = On;
		Pos[Idx] = Prev[Idx] = ShapeTarget[Idx] = On;
	}
	// Keep headboard relative layout from loft (only luff end of head is on stay)
	// Head outer / mid stay at loft Z but we leave FlatRest as loft for non-luff head verts.

	bPinned[ClewIndex] = 0; // free — boom / sheet
	bStayValid = true;
	TackIndex = LuffIndices[0];
	HeadIndex = LuffIndices.Last();
}

void FSailClothSim::CollectGridEdges()
{
	FootIndices.Reset();
	LeechIndices.Reset();
	for (int32 J = 0; J < Nw; ++J)
	{
		FootIndices.Add(GridIdx(0, J));
	}
	for (int32 I = 0; I < Nu; ++I)
	{
		LeechIndices.Add(GridIdx(I, Nw - 1));
	}
}

void FSailClothSim::BuildGridSprings()
{
	Springs.Reset();
	TSet<uint64> Keys;
	auto Key = [](int32 A, int32 B) -> uint64
	{
		const int32 Lo = FMath::Min(A, B), Hi = FMath::Max(A, B);
		return (uint64(uint32(Lo)) << 32) | uint64(uint32(Hi));
	};
	auto Add = [&](int32 A, int32 B, float Stiff, ESpringEdge Edge = ESpringEdge::Body,
		ESpringRole Role = ESpringRole::Inextensible)
	{
		if (A == B || !FlatRest.IsValidIndex(A) || !FlatRest.IsValidIndex(B)) return;
		const uint64 K = Key(A, B);
		if (Keys.Contains(K))
		{
			// Upgrade edge role / prefer inextensible if re-tagged as perimeter
			for (FSpring& S : Springs)
			{
				if ((S.A == A && S.B == B) || (S.A == B && S.B == A))
				{
					if (Edge != ESpringEdge::Body) S.Edge = Edge;
					S.Stiffness = FMath::Max(S.Stiffness, Stiff);
					// Never demote Inextensible → Soft
					if (Role == ESpringRole::Inextensible)
					{
						S.Role = ESpringRole::Inextensible;
					}
					break;
				}
			}
			return;
		}
		Keys.Add(K);
		FSpring S;
		S.A = A; S.B = B;
		S.FlatRestLen = FVector::Dist(FlatRest[A], FlatRest[B]);
		S.RestLen = S.FlatRestLen * PreTen; // web preTen 0.998
		S.Stiffness = Stiff;
		S.Edge = Edge;
		S.Role = Role;
		if (S.FlatRestLen > 0.25f) Springs.Add(S);
	};

	// Structural (row/col) + shear (quad diagonals) — INEXTENSIBLE.
	// Fixed edge lengths + fixed diagonals ⇒ cells cannot stretch or shear into
	// parallelograms. The surface can still move and fold (bend is Soft below).
	for (int32 I = 0; I < Nu; ++I)
	{
		for (int32 J = 0; J < Nw; ++J)
		{
			const int32 Idx = GridIdx(I, J);
			if (J < Nw - 1) Add(Idx, GridIdx(I, J + 1), 1.0f); // structural
			if (I < Nu - 1) Add(Idx, GridIdx(I + 1, J), 1.0f); // structural
			if (I < Nu - 1 && J < Nw - 1)
			{
				// Shear diagonals — this is what stops the grid from shearing
				Add(Idx, GridIdx(I + 1, J + 1), 1.0f);
				Add(GridIdx(I, J + 1), GridIdx(I + 1, J), 1.0f);
			}
		}
	}
	// Bend skip-one — SOFT so the sail can still billow / form camber
	for (int32 I = 0; I < Nu; ++I)
		for (int32 J = 0; J < Nw - 2; ++J)
			Add(GridIdx(I, J), GridIdx(I, J + 2), 0.45f, ESpringEdge::Body, ESpringRole::Soft);
	for (int32 I = 0; I < Nu - 2; ++I)
		for (int32 J = 0; J < Nw; ++J)
			Add(GridIdx(I, J), GridIdx(I + 2, J), 0.45f, ESpringEdge::Body, ESpringRole::Soft);

	// Main only: soft load-prop ties from luff into interior
	if (RigKind == ESailRigKind::Main)
	{
		const int32 KMax = FMath::Min(Nw - 1, FMath::Max(7, Nw / 3));
		for (int32 I = 0; I < Nu; ++I)
		{
			const int32 Luff = GridIdx(I, 0);
			for (int32 K = 3; K <= KMax; ++K)
			{
				Add(Luff, GridIdx(I, K), 0.55f, ESpringEdge::Body, ESpringRole::Soft);
			}
		}
	}

	// Perimeter chains — inextensible (same as structural; rest lengths retagged for trim)
	for (int32 J = 0; J < Nw - 1; ++J)
		Add(GridIdx(0, J), GridIdx(0, J + 1), 1.0f, ESpringEdge::Foot, ESpringRole::Inextensible);
	for (int32 I = 0; I < Nu - 1; ++I)
		Add(GridIdx(I, Nw - 1), GridIdx(I + 1, Nw - 1), 1.0f, ESpringEdge::Leech, ESpringRole::Inextensible);
	for (int32 I = 0; I < Nu - 1; ++I)
		Add(GridIdx(I, 0), GridIdx(I + 1, 0), 1.0f, ESpringEdge::Luff, ESpringRole::Inextensible);

	// Physical battens (inactive until engage after settle) — not soft cloth springs
	BuildBattens();
}

void FSailClothSim::BuildBattens()
{
	// Asym / Code Zero: no battens.
	Battens.Reset();
	BattenDist.Reset();
	BattenBend.Reset();
	BattenRows.Reset();
	bBattensEngaged = false;
	BattenSettleFrames = 0;
	if (!bGridTopology || Nu < 4 || Nw < 6) return;
	if (RigKind == ESailRigKind::AsymSpin) return;

	// J/105 Class Rules 6.4.3: four battens; pocket centres divide the leech
	// into five equal parts (±80 mm) → leech stations at 1/5, 2/5, 3/5, 4/5.
	// Length: progressive (short lower leech battens → full top), matching
	// typical OD racing mains (Ullman: "tapered racing battens with a full
	// top batten"). Inset = chord fraction the batten spans from the leech.
	// Jib: three short leech battens (flutter control only).
	struct FSpec { float Frac; float Inset; };
	const FSpec* Specs = nullptr;
	int32 NSpec = 0;
	if (RigKind == ESailRigKind::Jib)
	{
		static const FSpec JibSpecs[] = {
			{ 0.30f, 0.28f },
			{ 0.55f, 0.30f },
			{ 0.78f, 0.32f },
		};
		Specs = JibSpecs;
		NSpec = 3;
		BattenStiffness = 0.80f;
		BattenBendK = 0.78f;
		BattenBendIters = 5;
	}
	else
	{
		// Main — class leech fifths + full top (~luff-to-leech)
		static const FSpec MainSpecs[] = {
			{ 0.20f, 0.50f }, // lowest: ~½ chord leech batten
			{ 0.40f, 0.65f },
			{ 0.60f, 0.80f },
			{ 0.80f, 0.96f }, // full top batten (roach support)
		};
		Specs = MainSpecs;
		NSpec = 4;
		BattenStiffness = 0.88f;
		BattenBendK = 0.88f;
		BattenBendIters = 6;
	}

	for (int32 Bi = 0; Bi < NSpec; ++Bi)
	{
		const float Frac = Specs[Bi].Frac;
		const float Inset = Specs[Bi].Inset;
		const int32 I = FMath::Clamp(FMath::RoundToInt(Frac * float(Nu - 1)), 0, Nu - 1);
		const int32 LeechCol = Nw - 1;
		const int32 JStart = FMath::Clamp(
			FMath::RoundToInt((1.f - Inset) * float(Nw - 1)), 0, Nw - 2);
		const int32 Span = FMath::Max(1, LeechCol - JStart);

		// Distance ties: skip-3..5 chordwise (longer wavelength than cloth bend)
		// Taper soft-forward / stiff-aft like real fibreglass battens.
		for (int32 J = JStart; J < Nw; ++J)
		{
			for (int32 D = 3; D <= 5; ++D)
			{
				if (J + D > LeechCol) continue;
				const float TMid = float((J + D * 0.5f) - JStart) / float(Span);
				FBattenDistTie T;
				T.A = GridIdx(I, J);
				T.B = GridIdx(I, J + D);
				T.RestLen = 0.f; // captured on engage
				T.Stiffness = 0.f;
				T.BattenW = 0.6f + 0.4f * FMath::Clamp(TMid, 0.f, 1.f);
				BattenDist.Add(T);
			}
		}

		// Bend ties: discrete second-difference beam at span 1 and 2
		const int32 VUpBase = FMath::Min(I + 1, Nu - 1);
		const int32 VDnBase = FMath::Max(I - 1, 0);
		for (const int32 Sp : { 1, 2 })
		{
			for (int32 Jc = JStart + Sp; Jc <= LeechCol - Sp; ++Jc)
			{
				const float TMid = float(Jc - JStart) / float(Span);
				FBattenBendTie Bc;
				Bc.A = GridIdx(I, Jc - Sp);
				Bc.B = GridIdx(I, Jc);
				Bc.C = GridIdx(I, Jc + Sp);
				Bc.Vu = GridIdx(VUpBase, Jc);
				Bc.Vd = GridIdx(VDnBase, Jc);
				Bc.Sp = Sp;
				Bc.BattenW = 0.75f + 0.25f * FMath::Clamp(TMid, 0.f, 1.f);
				Bc.Stiffness = 0.f;
				BattenBend.Add(Bc);
			}
		}

		FBattenRow Row;
		Row.I = I;
		Row.JStart = JStart;
		Row.JLeech = LeechCol;
		Row.FracV = Frac;
		Row.Inset = Inset;
		Battens.Add(Row);
	}

	UE_LOG(LogSailSim, Log,
		TEXT("SailClothSim battens %s: %d rows, %d dist ties, %d bend ties (inactive until settle)"),
		RigKind == ESailRigKind::Jib ? TEXT("jib") : TEXT("main"),
		Battens.Num(), BattenDist.Num(), BattenBend.Num());
}

void FSailClothSim::EngageBattens()
{
	if (BattenDist.Num() == 0 && BattenBend.Num() == 0) return;

	// Capture current (billowed) chord lengths for distance ties
	for (FBattenDistTie& C : BattenDist)
	{
		if (!Pos.IsValidIndex(C.A) || !Pos.IsValidIndex(C.B)) continue;
		const float D = FVector::Dist(Pos[C.A], Pos[C.B]);
		if (D > 1e-3f) C.RestLen = D;
		C.Stiffness = BattenStiffness * C.BattenW;
	}

	// Taubin λ|μ smooth along each batten row into scratch, then capture curvature
	// so the beam holds a fair arc rather than high-frequency cloth wrinkles.
	TArray<FVector> S = Pos;
	constexpr float Lam = 0.6f;
	constexpr float Mu = -0.62f;
	constexpr int32 Passes = 32;
	for (const FBattenRow& Bt : Battens)
	{
		for (int32 It = 0; It < Passes; ++It)
		{
			const float F = (It & 1) ? Mu : Lam;
			for (int32 J = Bt.JStart + 1; J < Bt.JLeech; ++J)
			{
				const int32 K = GridIdx(Bt.I, J);
				const int32 Km = GridIdx(Bt.I, J - 1);
				const int32 Kp = GridIdx(Bt.I, J + 1);
				if (!S.IsValidIndex(K) || !S.IsValidIndex(Km) || !S.IsValidIndex(Kp)) continue;
				const FVector Mid = 0.5f * (S[Km] + S[Kp]);
				S[K] += F * (Mid - S[K]);
			}
		}
	}

	auto LocalFrame = [&](const FBattenBendTie& Bc, const TArray<FVector>& P,
		FVector& OutT, FVector& OutU, FVector& OutN)
	{
		FVector T = P[Bc.C] - P[Bc.A];
		if (!T.Normalize()) T = FVector(1.f, 0.f, 0.f);
		FVector V = Pos.IsValidIndex(Bc.Vu) && Pos.IsValidIndex(Bc.Vd)
			? (Pos[Bc.Vu] - Pos[Bc.Vd])
			: FVector(0.f, 0.f, 1.f);
		FVector N = FVector::CrossProduct(T, V);
		if (!N.Normalize()) N = FVector(0.f, 1.f, 0.f);
		FVector U = FVector::CrossProduct(N, T);
		if (!U.Normalize()) U = FVector(0.f, 0.f, 1.f);
		OutT = T; OutU = U; OutN = N;
	};

	for (FBattenBendTie& Bc : BattenBend)
	{
		if (!S.IsValidIndex(Bc.A) || !S.IsValidIndex(Bc.B) || !S.IsValidIndex(Bc.C)) continue;
		const FVector Off = S[Bc.B] - 0.5f * (S[Bc.A] + S[Bc.C]);
		FVector T, U, N;
		LocalFrame(Bc, S, T, U, N);
		Bc.Ct = FVector::DotProduct(Off, T);
		Bc.Cu = FVector::DotProduct(Off, U);
		Bc.Cn = FVector::DotProduct(Off, N);
		Bc.CnOrig = Bc.Cn;
		Bc.bSnapped = false;
		Bc.WnRefSign = 0.f;
		Bc.Stiffness = FMath::Min(1.f, BattenBendK * Bc.BattenW);
	}

	// Per-batten snap state (one row = one physical batten)
	BattenRows.Reset();
	TMap<int32, int32> RowMap;
	for (FBattenBendTie& Bc : BattenBend)
	{
		const int32 RowI = (Nw > 0) ? (Bc.B / Nw) : 0;
		int32* Found = RowMap.Find(RowI);
		if (!Found)
		{
			FBattenRowState Rs;
			Rs.RowI = RowI;
			const int32 Idx = BattenRows.Add(Rs);
			RowMap.Add(RowI, Idx);
			Bc.RowStateIdx = Idx;
		}
		else
		{
			Bc.RowStateIdx = *Found;
		}
	}

	bBattensEngaged = true;
	UE_LOG(LogSailSim, Log,
		TEXT("SailClothSim %s battens ENGAGED (camber captured after settle)"),
		RigKind == ESailRigKind::Jib ? TEXT("jib") : TEXT("main"));
}

void FSailClothSim::SolveBattens(const FVector& WindLocalDir, float WindSpeedKn)
{
	if (!bBattensEngaged) return;
	const float R = Relax;

	// 1) Distance ties — stiff chordwise beam length (tapered)
	for (const FBattenDistTie& S : BattenDist)
	{
		if (S.Stiffness < 1e-4f || S.RestLen < 1e-3f) continue;
		if (!Pos.IsValidIndex(S.A) || !Pos.IsValidIndex(S.B)) continue;
		FVector& PA = Pos[S.A];
		FVector& PB = Pos[S.B];
		FVector Delta = PA - PB;
		const float Dist = Delta.Size();
		if (Dist < 1e-4f) continue;
		const float Diff = (Dist - S.RestLen) / Dist * R * S.Stiffness;
		const FVector Corr = Delta * Diff;
		const bool Ai = bPinned.IsValidIndex(S.A) && bPinned[S.A] != 0;
		const bool Aj = bPinned.IsValidIndex(S.B) && bPinned[S.B] != 0;
		if (Ai && Aj) continue;
		if (Ai) { PB += Corr * 2.f; }
		else if (Aj) { PA -= Corr * 2.f; }
		else { PA -= Corr; PB += Corr; }
	}

	if (BattenBend.Num() == 0) return;

	// 2) Aero-gated snap-through (whole batten as a unit) — backwinding only
	const FVector WindN = WindLocalDir.GetSafeNormal();
	const float Wspd = FMath::Max(WindSpeedKn * 0.15f, 0.f); // match web wind scale
	const bool bSnapEnabled = true;
	constexpr float SnapAeroThresh = 0.08f;
	constexpr int32 SnapDwell = 45;
	constexpr int32 SnapRecoverDwell = 30;

	if (bSnapEnabled && Wspd > 1e-6f && BattenRows.Num() > 0)
	{
		for (FBattenRowState& Rs : BattenRows)
		{
			Rs.Sum = 0.f;
			Rs.Cnt = 0;
		}
		for (FBattenBendTie& Bc : BattenBend)
		{
			if (Bc.CnOrig == 0.f) continue;
			if (!Pos.IsValidIndex(Bc.B) || !Normals.IsValidIndex(Bc.B)) continue;
			if (!BattenRows.IsValidIndex(Bc.RowStateIdx)) continue;
			FVector Nrm = Normals[Bc.B];
			if (!Nrm.Normalize()) continue;
			const float Wn = FVector::DotProduct(WindN, Nrm);
			if (Bc.WnRefSign == 0.f && FMath::Abs(Wn) > 0.15f)
			{
				Bc.WnRefSign = FMath::Sign(Wn);
			}
			if (Bc.WnRefSign != 0.f)
			{
				FBattenRowState& Rs = BattenRows[Bc.RowStateIdx];
				float Sig = -(Wn * Bc.WnRefSign);
				if (Rs.bSnapped) Sig = -Sig;
				Rs.Sum += Sig;
				Rs.Cnt++;
			}
		}
		for (FBattenRowState& Rs : BattenRows)
		{
			if (Rs.Cnt == 0) continue;
			const float Mean = Rs.Sum / float(Rs.Cnt);
			if (!Rs.bSnapped)
			{
				if (Mean > SnapAeroThresh)
				{
					if (++Rs.SnapHold >= SnapDwell) { Rs.bSnapped = true; Rs.SnapHold = SnapDwell; }
				}
				else if (Rs.SnapHold > 0) { --Rs.SnapHold; }
			}
			else
			{
				if (Rs.SnapHold > 0) { --Rs.SnapHold; }
				else if (Mean < -SnapAeroThresh)
				{
					if (--Rs.SnapHold <= -SnapRecoverDwell) { Rs.bSnapped = false; Rs.SnapHold = 0; }
				}
				else if (Rs.SnapHold < 0) { ++Rs.SnapHold; }
			}
		}
		for (FBattenBendTie& Bc : BattenBend)
		{
			if (!BattenRows.IsValidIndex(Bc.RowStateIdx)) continue;
			const bool Want = BattenRows[Bc.RowStateIdx].bSnapped;
			if (Bc.bSnapped != Want)
			{
				Bc.bSnapped = Want;
				Bc.Cn = -Bc.Cn;
				Bc.Cu = -Bc.Cu;
				// Zero Verlet velocity at a,b,c to avoid overshoot
				if (Pos.IsValidIndex(Bc.A) && Prev.IsValidIndex(Bc.A)) Prev[Bc.A] = Pos[Bc.A];
				if (Pos.IsValidIndex(Bc.B) && Prev.IsValidIndex(Bc.B)) Prev[Bc.B] = Pos[Bc.B];
				if (Pos.IsValidIndex(Bc.C) && Prev.IsValidIndex(Bc.C)) Prev[Bc.C] = Pos[Bc.C];
			}
		}
	}

	// Local frame at a bend tie from live positions
	auto BattenFrame = [&](const FBattenBendTie& Bc, FVector& OutT, FVector& OutU, FVector& OutN)
	{
		FVector T = Pos[Bc.C] - Pos[Bc.A];
		if (!T.Normalize()) T = FVector(1.f, 0.f, 0.f);
		FVector V = (Pos.IsValidIndex(Bc.Vu) && Pos.IsValidIndex(Bc.Vd))
			? (Pos[Bc.Vu] - Pos[Bc.Vd])
			: FVector(0.f, 0.f, 1.f);
		FVector N = FVector::CrossProduct(T, V);
		if (!N.Normalize()) N = FVector(0.f, 1.f, 0.f);
		FVector U = FVector::CrossProduct(N, T);
		if (!U.Normalize()) U = FVector(0.f, 0.f, 1.f);
		OutT = T; OutU = U; OutN = N;
	};

	// 3) Rotate captured curvature into world targets once per step
	for (FBattenBendTie& Bc : BattenBend)
	{
		if (!Pos.IsValidIndex(Bc.A) || !Pos.IsValidIndex(Bc.C)) continue;
		FVector T, U, N;
		BattenFrame(Bc, T, U, N);
		const FVector Target = T * Bc.Ct + U * Bc.Cu + N * Bc.Cn;
		Bc.Txr = Target.X;
		Bc.Tyr = Target.Y;
		Bc.Tzr = Target.Z;
	}

	// 4) Multi-pass span-aware curvature correction (mass-conserving a,b,c)
	const int32 Iters = FMath::Clamp(BattenBendIters, 3, 8);
	for (int32 It = 0; It < Iters; ++It)
	{
		for (const FBattenBendTie& Bc : BattenBend)
		{
			if (Bc.Stiffness < 1e-4f) continue;
			if (!Pos.IsValidIndex(Bc.A) || !Pos.IsValidIndex(Bc.B) || !Pos.IsValidIndex(Bc.C)) continue;
			const FVector Mid = 0.5f * (Pos[Bc.A] + Pos[Bc.C]);
			const FVector TargetOff(Bc.Txr, Bc.Tyr, Bc.Tzr);
			const FVector Err = (Pos[Bc.B] - Mid) - TargetOff;
			const float K = Bc.Stiffness * (Bc.Sp > 1 ? (1.f / float(Bc.Sp)) : 1.f);
			const float Fb = 0.6666667f * K;
			const float Fa = 0.3333333f * K;
			// Pin-aware: don't move hard anchors
			const bool Pa = bPinned.IsValidIndex(Bc.A) && bPinned[Bc.A] != 0;
			const bool Pb = bPinned.IsValidIndex(Bc.B) && bPinned[Bc.B] != 0;
			const bool Pc = bPinned.IsValidIndex(Bc.C) && bPinned[Bc.C] != 0;
			if (!Pb) Pos[Bc.B] -= Err * Fb;
			if (!Pa) Pos[Bc.A] += Err * Fa;
			if (!Pc) Pos[Bc.C] += Err * Fa;
		}
	}

	// 5) Light Laplacian fairing along each batten (removes inter-tie ripple)
	for (const FBattenRow& Bt : Battens)
	{
		for (int32 J = Bt.JStart + 1; J < Bt.JLeech; ++J)
		{
			const int32 K = GridIdx(Bt.I, J);
			const int32 Km = GridIdx(Bt.I, J - 1);
			const int32 Kp = GridIdx(Bt.I, J + 1);
			if (!Pos.IsValidIndex(K) || (bPinned.IsValidIndex(K) && bPinned[K])) continue;
			if (!Pos.IsValidIndex(Km) || !Pos.IsValidIndex(Kp)) continue;
			const FVector Mid = 0.5f * (Pos[Km] + Pos[Kp]);
			Pos[K] += 0.4f * (Mid - Pos[K]);
		}
	}
}

void FSailClothSim::ComputeUVParams()
{
	const int32 N = FlatRest.Num();
	ChordU.SetNum(N);
	HeightV.SetNum(N);
	if (N == 0) return;

	if (bGridTopology && Nu > 1 && Nw > 1)
	{
		for (int32 I = 0; I < Nu; ++I)
		{
			const float V = float(I) / float(Nu - 1);
			for (int32 J = 0; J < Nw; ++J)
			{
				const int32 Idx = GridIdx(I, J);
				ChordU[Idx] = float(J) / float(Nw - 1);
				HeightV[Idx] = V;
			}
		}
		// Chord length from foot
		if (TackIndex != INDEX_NONE && ClewIndex != INDEX_NONE)
		{
			ChordLen = FVector::Dist(FlatRest[TackIndex], FlatRest[ClewIndex]);
		}
		// Max row chord for reference
		float MaxC = ChordLen;
		for (int32 I = 0; I < Nu; ++I)
		{
			MaxC = FMath::Max(MaxC, FVector::Dist(FlatRest[GridIdx(I, 0)], FlatRest[GridIdx(I, Nw - 1)]));
		}
		ChordLen = FMath::Max(MaxC, 1.f);
		return;
	}

	// Heuristic UV
	if (!bStayValid) return;
	FVector StayDir = (StayHeadLocal - StayTackLocal);
	const float StayLen = FMath::Max(1.f, StayDir.Size());
	StayDir /= StayLen;
	FVector ChordAccum = FVector::ZeroVector;
	int32 FreeN = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (bPinned[I]) continue;
		ChordAccum += (FlatRest[I] - StayTackLocal);
		++FreeN;
	}
	FVector ChordDir = FreeN > 0 ? ChordAccum / FreeN : FVector(-1.f, 0.f, 0.f);
	ChordDir = (ChordDir - StayDir * FVector::DotProduct(ChordDir, StayDir)).GetSafeNormal();
	if (ChordDir.IsNearlyZero()) ChordDir = FVector(-1.f, 0.f, 0.f);
	float MaxChord = 1.f;
	for (int32 I = 0; I < N; ++I)
	{
		const FVector FromTack = FlatRest[I] - StayTackLocal;
		HeightV[I] = FMath::Clamp(FVector::DotProduct(FromTack, StayDir) / StayLen, 0.f, 1.f);
		ChordU[I] = FMath::Max(0.f, FVector::DotProduct(FromTack, ChordDir));
		MaxChord = FMath::Max(MaxChord, ChordU[I]);
	}
	ChordLen = MaxChord;
	for (int32 I = 0; I < N; ++I)
	{
		ChordU[I] = FMath::Clamp(ChordU[I] / MaxChord, 0.f, 1.f);
	}
}

void FSailClothSim::RebuildCamberTarget(
	bool bLeeToStarboard,
	float SheetEase01,
	float WindSpeedKn,
	const FVector& BoomTipLocal)
{
	// Mild draft assist ONLY — do not remap planform (that stretched leeches).
	// Web cloth starts flat and wind builds shape.
	const int32 N = FlatRest.Num();
	if (N == 0) return;

	const FVector Lee = FVector(0.f, bLeeToStarboard ? 1.f : -1.f, 0.f);
	const float Ease = FMath::Clamp(SheetEase01, 0.f, 1.f);
	const float Outhaul = FMath::Clamp(Outhaul01, 0.f, 1.f);
	const float WorkingChord = FMath::Max(1.f,
		(RigKind == ESailRigKind::Main && BoomLenCm > 1.f)
			? BoomLenCm * OuthaulFrac()
			: ChordLen);
	const float WindFill = FMath::Clamp(0.45f + 0.55f * (WindSpeedKn / 12.f), 0.45f, 1.1f);
	const float PeakDraft = WorkingChord * MaxCamberFrac
		* FMath::Lerp(1.0f, 0.72f, Ease) * WindFill;
	const float Peak = FMath::Clamp(DraftStation, 0.2f, 0.55f);

	for (int32 I = 0; I < N; ++I)
	{
		if (bPinned[I])
		{
			ShapeTarget[I] = FlatRest[I];
			continue;
		}
		const float U = ChordU.IsValidIndex(I) ? ChordU[I] : 0.5f;
		const float V = HeightV.IsValidIndex(I) ? HeightV[I] : 0.5f;

		float Shape;
		if (U <= Peak)
		{
			const float T = Peak > 1e-3f ? U / Peak : 0.f;
			Shape = T * T * (3.f - 2.f * T);
		}
		else
		{
			const float T = (1.f - Peak) > 1e-3f ? (U - Peak) / (1.f - Peak) : 0.f;
			Shape = 1.f - 0.90f * T * T * (3.f - 2.f * T);
		}

		float VertTaper;
		if (RigKind == ESailRigKind::Main)
		{
			const float Base = FMath::Sin(PI * FMath::Clamp(V, 0.f, 1.f));
			VertTaper = FMath::Pow(Base, 0.85f) * FMath::Lerp(1.05f, 0.78f, V);
			if (V < 0.12f)
			{
				const float FootFade = FMath::Clamp(V / 0.12f, 0.f, 1.f);
				VertTaper *= FMath::Lerp(1.f - 0.95f * Outhaul, 1.f, FootFade);
			}
		}
		else
		{
			VertTaper = 0.70f + 0.30f * FMath::Sin(PI * FMath::Clamp(V, 0.f, 1.f));
		}

		const float Twist = 1.f - Ease * 0.40f * V * V;
		const float Draft = PeakDraft * Shape * VertTaper * Twist;
		// Pure draft off flat loft — preserves planform / leech path
		ShapeTarget[I] = FlatRest[I] + Lee * Draft;
	}

	if (ClewIndex != INDEX_NONE && RigKind == ESailRigKind::Main)
	{
		ShapeTarget[ClewIndex] = BoomTipLocal;
	}
	else if (ClewIndex != INDEX_NONE)
	{
		ShapeTarget[ClewIndex] = FlatRest[ClewIndex];
	}
}

void FSailClothSim::ApplyTrimTensions()
{
	// Leech line shortens rests. Main baseline ~0.62 → ~9% shorter leech path.
	const float LeeT = FMath::Clamp(LeechTension01, 0.f, 1.f);
	const float LeechMaxShort = (RigKind == ESailRigKind::Main) ? 0.15f : 0.10f;
	const float LeechK = 1.f - LeechMaxShort * LeeT;
	const float LuffK = 1.f - 0.10f * FMath::Clamp(LuffTension01, 0.f, 1.f);
	// Outhaul: ≤2% foot pretension (web setFootTighten)
	const float FootK = 1.f - 0.02f * FootBoomPull();

	for (FSpring& S : Springs)
	{
		const float FlatL = (S.FlatRestLen > 0.25f)
			? S.FlatRestLen
			: FVector::Dist(FlatRest[S.A], FlatRest[S.B]);
		if (FlatL < 0.25f) continue;
		S.FlatRestLen = FlatL;

		if (S.Edge == ESpringEdge::Leech)
		{
			S.RestLen = FlatL * LeechK * 0.998f;
		}
		else if (S.Edge == ESpringEdge::Luff)
		{
			S.RestLen = FlatL * LuffK * 0.998f;
		}
		else if (S.Edge == ESpringEdge::Foot)
		{
			S.RestLen = FlatL * FootK * 0.998f;
		}
		else
		{
			S.RestLen = FlatL * 0.998f;
		}
	}
	// Do NOT rescale foot/leech chains to loft path lengths. That forced rest
	// lengths away from the flat grid and dumped planform mismatch into the
	// single clew corner cell — the web sim only pretensions per-edge rests.
}

void FSailClothSim::SyncSpringRests(float Powered01)
{
	// Keep free-edge rests locked; body can bias slightly toward draft
	const float P = FMath::Clamp(Powered01, 0.f, 1.f);
	const float CamberW = 0.02f + 0.08f * P; // mild — web is almost pure fabric rests

	for (FSpring& S : Springs)
	{
		if (S.Edge != ESpringEdge::Body) continue;
		const float FlatL = S.FlatRestLen > 0.25f ? S.FlatRestLen
			: FVector::Dist(FlatRest[S.A], FlatRest[S.B]);
		if (FlatL < 0.25f) continue;
		float ShapeL = FlatL;
		if (ShapeTarget.IsValidIndex(S.A) && ShapeTarget.IsValidIndex(S.B))
		{
			ShapeL = FMath::Max(0.25f, FVector::Dist(ShapeTarget[S.A], ShapeTarget[S.B]));
		}
		ShapeL = FMath::Min(ShapeL, FlatL * MaxStrain);
		S.RestLen = FMath::Lerp(FlatL, ShapeL, CamberW) * 0.998f;
	}
	ApplyTrimTensions();
}

void FSailClothSim::ComputeNormals()
{
	const int32 N = Pos.Num();
	if (N == 0) return;
	Normals.SetNum(N);
	for (FVector& Nr : Normals) Nr = FVector::ZeroVector;

	if (bGridTopology && Nu > 1 && Nw > 1)
	{
		// Grid normals (web _computeNormals)
		for (int32 I = 0; I < Nu; ++I)
		{
			const int32 Ip = FMath::Min(I + 1, Nu - 1);
			const int32 Im = FMath::Max(I - 1, 0);
			for (int32 J = 0; J < Nw; ++J)
			{
				const int32 Jp = FMath::Min(J + 1, Nw - 1);
				const int32 Jm = FMath::Max(J - 1, 0);
				const FVector Du = Pos[GridIdx(Ip, J)] - Pos[GridIdx(Im, J)];
				const FVector Dw = Pos[GridIdx(I, Jp)] - Pos[GridIdx(I, Jm)];
				FVector Nrm = FVector::CrossProduct(Du, Dw);
				if (!Nrm.Normalize()) Nrm = FVector(0.f, 1.f, 0.f);
				Normals[GridIdx(I, J)] = Nrm;
			}
		}
		return;
	}

	for (int32 T = 0; T + 2 < TriIndices.Num(); T += 3)
	{
		const int32 Ia = TriIndices[T], Ib = TriIndices[T + 1], Ic = TriIndices[T + 2];
		if (!Pos.IsValidIndex(Ia) || !Pos.IsValidIndex(Ib) || !Pos.IsValidIndex(Ic)) continue;
		const FVector Nrm = FVector::CrossProduct(Pos[Ib] - Pos[Ia], Pos[Ic] - Pos[Ia]);
		Normals[Ia] += Nrm;
		Normals[Ib] += Nrm;
		Normals[Ic] += Nrm;
	}
	for (int32 I = 0; I < N; ++I)
	{
		if (!Normals[I].Normalize()) Normals[I] = FVector(0.f, 1.f, 0.f);
	}
}

float FSailClothSim::EvaluateIncidenceAndLuff(const FVector& WindVelWeb, float SheetEase01)
{
	const FVector Wdir = WindVelWeb.GetSafeNormal();
	float CosSum = 0.f;
	int32 C = 0;
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (bPinned[I] || I == ClewIndex) continue;
		const float V = HeightV.IsValidIndex(I) ? HeightV[I] : 0.5f;
		if (RigKind == ESailRigKind::Main && V < 0.08f) continue;
		if (!Normals.IsValidIndex(I)) continue;
		CosSum += FMath::Abs(FVector::DotProduct(Wdir, Normals[I]));
		++C;
	}
	LastMeanCosInc = (C > 0) ? (CosSum / C) : 0.2f;
	const float Cos = LastMeanCosInc;
	float Powered = 0.f;
	if (Cos <= LuffCosStart) Powered = 0.f;
	else if (Cos >= PowerCosFull) Powered = 1.f;
	else Powered = (Cos - LuffCosStart) / FMath::Max(1e-3f, PowerCosFull - LuffCosStart);

	// Eased sheet → less power
	const float Ease = FMath::Clamp(SheetEase01, 0.f, 1.f);
	Powered *= FMath::Lerp(1.f, 0.55f, Ease);
	LastPowered01 = FMath::Clamp(Powered, 0.f, 1.f);
	LastLuffAmount = 1.f - LastPowered01;
	return LastPowered01;
}

void FSailClothSim::SetStayEndpoints(const FVector& TackLocal, const FVector& HeadLocal)
{
	StayTackLocal = TackLocal;
	StayHeadLocal = HeadLocal;
	bStayValid = true;
	LuffLen = FMath::Max(1.f, FVector::Dist(StayTackLocal, StayHeadLocal));
	if (TackIndex != INDEX_NONE && Pos.IsValidIndex(TackIndex))
	{
		Pos[TackIndex] = Prev[TackIndex] = StayTackLocal;
		if (FlatRest.IsValidIndex(TackIndex)) FlatRest[TackIndex] = StayTackLocal;
	}
	// Pointed head: move the entire head row with the halyard (not just j=0)
	if (RigKind == ESailRigKind::AsymSpin && HeadRowIndices.Num() > 0)
	{
		for (const int32 Idx : HeadRowIndices)
		{
			if (!Pos.IsValidIndex(Idx)) continue;
			Pos[Idx] = Prev[Idx] = StayHeadLocal;
			if (FlatRest.IsValidIndex(Idx)) FlatRest[Idx] = StayHeadLocal;
			if (bPinned.IsValidIndex(Idx)) bPinned[Idx] = 1;
		}
	}
	else if (HeadIndex != INDEX_NONE && Pos.IsValidIndex(HeadIndex))
	{
		Pos[HeadIndex] = Prev[HeadIndex] = StayHeadLocal;
		if (FlatRest.IsValidIndex(HeadIndex)) FlatRest[HeadIndex] = StayHeadLocal;
	}
}

bool FSailClothSim::BuildFromFlatTriangle(
	UProceduralMeshComponent* Mesh,
	int32 Section,
	ESailRigKind Kind,
	const FVector& TackLocal,
	const FVector& HeadLocal,
	const FVector& ClewLocal,
	int32 InNu,
	int32 InNw)
{
	Clear();
	if (!Mesh || InNu < 4 || InNw < 4) return false;

	RigKind = Kind;
	Nu = InNu;
	Nw = InNw;
	bGridTopology = true;
	// Code Zero: flatter / less powered than a full runner A2.
	AeroK = (Kind == ESailRigKind::AsymSpin) ? 0.0036f : 0.0028f;
	DragK = 0.0004f;
	Damping = 0.978f;
	Relax = 0.72f;
	PreTen = 0.998f;
	MaxDeltaCm = 0.45f * 30.48f;
	ConstraintIters = 5;
	InextensiblePasses = 2;
	InextensibleFinalPasses = 3;
	MaxStrain = 1.04f;
	Outhaul01 = 0.5f;
	// Flat Code Zero draft (~10%) vs deep runner asym (~20%+).
	MaxCamberFrac = (Kind == ESailRigKind::AsymSpin) ? 0.10f : 0.14f;

	const int32 N = Nu * Nw;
	Pos.SetNum(N);
	Prev.SetNum(N);
	FlatRest.SetNum(N);
	ShapeTarget.SetNum(N);
	Normals.Init(FVector(0.f, 1.f, 0.f), N);
	bPinned.Init(0, N);
	SectionIndex = Section;

	// -------------------------------------------------------------------------
	// Code Zero = TRUE 3-CORNER sail (tack, head, clew) — never a headboard.
	//
	// Outline:
	//   • Luff  : straight Tack → Head
	//   • Foot  : straight Tack → Clew
	//   • Leech : quadratic Clew → Head with one control point so mid-girth
	//             SMG ≈ 0.68·SF (flat CZ). Endpoints stay Clew & Head only.
	// Interior camber is out-of-plane only (does not add a 4th corner).
	// -------------------------------------------------------------------------
	const float FootLen = FMath::Max(1.f, FVector::Dist(TackLocal, ClewLocal));
	const float SmgRatio = 0.68f; // flat Code Zero mid-girth / foot
	const float RestCamberFrac = 0.04f;

	FVector FaceN = FVector::CrossProduct(HeadLocal - TackLocal, ClewLocal - TackLocal);
	if (!FaceN.Normalize()) FaceN = FVector(0.f, 1.f, 0.f);

	// Mid-height on the straight-edged triangle
	const FVector LuffMid = FMath::Lerp(TackLocal, HeadLocal, 0.5f);
	const FVector LeechMidLinear = FMath::Lerp(ClewLocal, HeadLocal, 0.5f);
	FVector AcrossMid = LeechMidLinear - LuffMid;
	if (AcrossMid.SizeSquared() < 1e-4f)
	{
		AcrossMid = (ClewLocal - TackLocal);
	}
	const FVector AcrossMidU = AcrossMid.GetSafeNormal();
	// Desired mid-leech so |LeechMid - LuffMid| = SMG
	const FVector LeechMidDesired = LuffMid + AcrossMidU * (SmgRatio * FootLen);
	// Quadratic Bezier leech: (1-t)²·Clew + 2(1-t)t·Q + t²·Head
	// At t=0.5: 0.25·C + 0.5·Q + 0.25·H = LeechMidDesired
	// ⇒ Q = 2·LeechMidDesired − 0.5·C − 0.5·H
	const FVector LeechCtrl = LeechMidDesired * 2.f - ClewLocal * 0.5f - HeadLocal * 0.5f;

	auto EdgeLuff = [&](float V) -> FVector
	{
		return FMath::Lerp(TackLocal, HeadLocal, V);
	};
	auto EdgeLeech = [&](float V) -> FVector
	{
		// Exact endpoints → only 3 outline corners
		if (V <= 0.f) return ClewLocal;
		if (V >= 1.f) return HeadLocal;
		const float Omt = 1.f - V;
		return ClewLocal * (Omt * Omt) + LeechCtrl * (2.f * Omt * V) + HeadLocal * (V * V);
	};

	for (int32 I = 0; I < Nu; ++I)
	{
		const float V = (Nu > 1) ? float(I) / float(Nu - 1) : 0.f; // 0 foot → 1 head
		const FVector LuffPt = EdgeLuff(V);
		const FVector LeechPt = EdgeLeech(V);
		const FVector Across = LeechPt - LuffPt;
		const float LocalChord = Across.Size();

		for (int32 J = 0; J < Nw; ++J)
		{
			const float U = (Nw > 1) ? float(J) / float(Nw - 1) : 0.f;
			FVector P;
			if (V >= 0.999f)
			{
				// Single pointed head — every head-row vert is the same corner
				P = HeadLocal;
			}
			else
			{
				// Chord from luff edge → leech edge (both end at Head when V→1)
				const float Camber = 4.f * U * (1.f - U) * (1.f - V) * RestCamberFrac;
				P = LuffPt + Across * U + FaceN * (LocalChord * Camber);
			}
			const int32 Idx = GridIdx(I, J);
			Pos[Idx] = Prev[Idx] = FlatRest[Idx] = ShapeTarget[Idx] = P;
		}
	}

	PinGridTopology(&TackLocal, &HeadLocal);
	CollectGridEdges();
	ComputeUVParams();
	BuildGridSprings();
	BuildClewPatches();

	// Load-prop not wanted on spin — springs already built for Main only
	LoftFootLen = FVector::Dist(FlatRest[TackIndex], FlatRest[ClewIndex]);
	FootRestLen = LoftFootLen;
	LoftLeechChord = FVector::Dist(HeadLocal, ClewLocal);
	LoftFootPath = LoftFootLen;
	// Leech path along the quadratic edge (not a straight chord)
	{
		float Path = 0.f;
		const int32 Segs = FMath::Max(8, Nu - 1);
		FVector PrevP = ClewLocal;
		for (int32 S = 1; S <= Segs; ++S)
		{
			const FVector P = EdgeLeech(float(S) / float(Segs));
			Path += FVector::Dist(PrevP, P);
			PrevP = P;
		}
		LoftLeechPath = FMath::Max(LoftLeechChord, Path);
	}
	LuffLen = FVector::Dist(StayTackLocal, StayHeadLocal);

	// Body tris: normal quads on the lower rows; TOP strip is a triangle FAN
	// into HeadIndex only (no headboard edge between head-row verts).
	TriIndices.Reset();
	const int32 HeadVtx = GridIdx(Nu - 1, 0); // all head-row positions == HeadLocal
	for (int32 I = 0; I < Nu - 2; ++I)
	{
		for (int32 J = 0; J < Nw - 1; ++J)
		{
			const int32 A = GridIdx(I, J);
			const int32 B = GridIdx(I + 1, J);
			const int32 C = GridIdx(I + 1, J + 1);
			const int32 D = GridIdx(I, J + 1);
			TriIndices.Add(A); TriIndices.Add(B); TriIndices.Add(D);
			TriIndices.Add(B); TriIndices.Add(C); TriIndices.Add(D);
		}
	}
	// Fan penultimate row → single head corner (true 3-corner top)
	if (Nu >= 2)
	{
		const int32 I = Nu - 2;
		for (int32 J = 0; J < Nw - 1; ++J)
		{
			const int32 A = GridIdx(I, J);
			const int32 D = GridIdx(I, J + 1);
			// One triangle per foot segment into the pointed head
			TriIndices.Add(A); TriIndices.Add(HeadVtx); TriIndices.Add(D);
		}
	}

	// Tri-radial Code Zero panelling (even/odd shades + radial stitch seams)
	BuildCodeZeroPanelLayout();

	bInitialized = true;
	ComputeNormals();
	PushCodeZeroMesh(Mesh);
	Mesh->SetVisibility(false);
	Mesh->SetHiddenInGame(true);

	// Report mid-girth for sanity (should be ~70% of foot for Code Zero loft)
	float MidGirthFt = 0.f;
	if (Nu >= 2 && Nw >= 2)
	{
		const int32 MidI = (Nu - 1) / 2;
		MidGirthFt = FVector::Dist(FlatRest[GridIdx(MidI, 0)], FlatRest[GridIdx(MidI, Nw - 1)]) / 30.48f;
	}
	UE_LOG(LogSailSim, Log,
		TEXT("SailClothSim CodeZero %dx%d foot=%.1f' luff=%.1f' midGirth=%.1f' (SMG/SF~%.0f%%) seams=%d"),
		Nu, Nw, LoftFootLen / 30.48f, LuffLen / 30.48f, MidGirthFt,
		(LoftFootLen > 1.f) ? (100.f * MidGirthFt / (LoftFootLen / 30.48f)) : 0.f,
		SeamPaths.Num());
	return true;
}

// ---------------------------------------------------------------------------
// Code Zero panelling — triangular wedges from the sail centre + rings.
//
// Real radial / tri-radial Code Zeros have long triangular panels whose seams
// "point in on themselves" toward the middle (load paths head/tack/clew).
// We render that look as:
//   • pie-wedge panels originating at the triangle centroid
//   • concentric panel rings (segmented radials, like real paneled radials)
//   • dark corner reinforcement patches + edge tapes
// ---------------------------------------------------------------------------
namespace CodeZeroPanelLocal
{
	static const FVector2D TackUV(0.f, 0.f);
	static const FVector2D ClewUV(1.f, 0.f);
	static const FVector2D HeadUV(0.5f, 1.f);
	// Triangle centroid — hub of the radial pattern
	static const FVector2D CenUV(0.5f, 1.f / 3.f);

	// Moderated counts — enough visual detail without huge seam meshes
	static constexpr int32 NWedges = 12;
	static constexpr int32 NRings = 3;
	static constexpr int32 SeamSamples = 8;
	static constexpr int32 NCornerRad = 5;

	static void AppendSeamUV(TArray<FVector2D>& Out, const FVector2D& A, const FVector2D& B, int32 Samples)
	{
		for (int32 S = 0; S <= Samples; ++S)
		{
			Out.Add(FMath::Lerp(A, B, float(S) / float(Samples)));
		}
	}

	/** Walk the sail perimeter: foot → leech → luff. S in [0,1). */
	static FVector2D PerimeterPoint(float S)
	{
		S = FMath::Fmod(S, 1.f);
		if (S < 0.f) S += 1.f;
		const float E = S * 3.f;
		if (E < 1.f) return FMath::Lerp(TackUV, ClewUV, E);
		if (E < 2.f) return FMath::Lerp(ClewUV, HeadUV, E - 1.f);
		return FMath::Lerp(HeadUV, TackUV, E - 2.f);
	}

	/** Ray from Cen along Dir hits the perimeter; returns hit + param S∈[0,1). */
	static bool RayHitPerimeter(const FVector2D& DirIn, FVector2D& OutHit, float& OutS)
	{
		FVector2D Dir = DirIn;
		if (!Dir.Normalize())
		{
			OutHit = PerimeterPoint(0.f);
			OutS = 0.f;
			return false;
		}
		const FVector2D Edges[3][2] = {
			{ TackUV, ClewUV },
			{ ClewUV, HeadUV },
			{ HeadUV, TackUV },
		};
		float BestT = TNumericLimits<float>::Max();
		int32 BestE = 0;
		float BestEdgeU = 0.f;
		for (int32 E = 0; E < 3; ++E)
		{
			const FVector2D A = Edges[E][0];
			const FVector2D B = Edges[E][1];
			const FVector2D Edge = B - A;
			// Cen + t*Dir = A + u*Edge  ⇒  [Dir | -Edge] [t;u] = A-Cen
			const FVector2D R = A - CenUV;
			const float Det = Dir.X * (-Edge.Y) - (-Edge.X) * Dir.Y;
			if (FMath::Abs(Det) < 1e-8f) continue;
			const float Tt = (R.X * (-Edge.Y) - (-Edge.X) * R.Y) / Det;
			const float Uu = (Dir.X * R.Y - Dir.Y * R.X) / Det;
			if (Tt > 1e-4f && Uu >= -0.001f && Uu <= 1.001f && Tt < BestT)
			{
				BestT = Tt;
				BestE = E;
				BestEdgeU = FMath::Clamp(Uu, 0.f, 1.f);
			}
		}
		if (BestT > 1e6f)
		{
			OutHit = PerimeterPoint(0.f);
			OutS = 0.f;
			return false;
		}
		OutHit = CenUV + Dir * BestT;
		OutS = (float(BestE) + BestEdgeU) / 3.f;
		return true;
	}

	/** Panel id: centre-origin wedge × ring (triangular panels). */
	static int32 PanelIdAt(float U, float V)
	{
		const FVector2D P(FMath::Clamp(U, 0.f, 1.f), FMath::Clamp(V, 0.f, 1.f));

		// Radial corner reinforcements (solid darker patches)
		if (FVector2D::Distance(P, HeadUV) < 0.09f) return 9000;
		if (FVector2D::Distance(P, TackUV) < 0.10f) return 9001;
		if (FVector2D::Distance(P, ClewUV) < 0.10f) return 9002;

		const FVector2D D = P - CenUV;
		if (D.SizeSquared() < 1e-6f) return 0;

		FVector2D Hit;
		float PeriS = 0.f;
		if (!RayHitPerimeter(D, Hit, PeriS))
		{
			PeriS = 0.f;
			Hit = PerimeterPoint(0.f);
		}
		const float HitDist = FVector2D::Distance(CenUV, Hit);
		const float PDist = FVector2D::Distance(CenUV, P);
		const float RingT = (HitDist > 1e-4f) ? FMath::Clamp(PDist / HitDist, 0.f, 0.999f) : 0.f;

		const int32 Wedge = FMath::Clamp(int32(PeriS * NWedges), 0, NWedges - 1);
		const int32 Ring = FMath::Clamp(int32(RingT * NRings), 0, NRings - 1);
		// Checker by wedge+ring so neighbouring triangles alternate
		return Wedge * NRings + Ring;
	}
}

void FSailClothSim::BuildCodeZeroPanelLayout()
{
	using namespace CodeZeroPanelLocal;
	bCodeZeroPanels = (RigKind == ESailRigKind::AsymSpin && Nu >= 4 && Nw >= 4);
	TriIndicesEven.Reset();
	TriIndicesOdd.Reset();
	SeamPaths.Reset();
	VertColors.Reset();
	if (!bCodeZeroPanels)
	{
		return;
	}

	const int32 N = Nu * Nw;
	// Stronger alternate shades so the centre-radial triangles read clearly
	const FLinearColor PanelA(0.05f, 0.18f, 0.48f, 0.78f);
	const FLinearColor PanelB(0.14f, 0.40f, 0.78f, 0.76f);
	const FLinearColor CornerPatch(0.03f, 0.09f, 0.26f, 0.92f);
	const FLinearColor LuffTape(0.04f, 0.12f, 0.32f, 0.90f);

	VertColors.SetNum(N);
	for (int32 I = 0; I < Nu; ++I)
	{
		const float V = (Nu > 1) ? float(I) / float(Nu - 1) : 0.f;
		for (int32 J = 0; J < Nw; ++J)
		{
			const float U = (Nw > 1) ? float(J) / float(Nw - 1) : 0.f;
			const int32 Idx = GridIdx(I, J);
			const int32 Pid = PanelIdAt(U, V);
			FLinearColor Col = ((Pid % 2) == 0) ? PanelA : PanelB;
			if (Pid >= 9000) Col = CornerPatch;
			if (U < 0.04f) Col = FMath::Lerp(Col, LuffTape, 0.85f);
			if (U > 0.96f) Col = FMath::Lerp(Col, LuffTape, 0.55f);
			if (V < 0.03f) Col = FMath::Lerp(Col, LuffTape, 0.50f);
			VertColors[Idx] = Col;
		}
	}

	auto UVOf = [&](int32 Idx) -> FVector2D
	{
		const int32 Ii = (Nw > 0) ? (Idx / Nw) : 0;
		const int32 Jj = (Nw > 0) ? (Idx % Nw) : 0;
		return FVector2D(
			(Nw > 1) ? float(Jj) / float(Nw - 1) : 0.f,
			(Nu > 1) ? float(Ii) / float(Nu - 1) : 0.f);
	};

	// Split body tris into even/odd (centre-radial panel shades)
	if (TriIndices.Num() >= 3)
	{
		for (int32 T = 0; T + 2 < TriIndices.Num(); T += 3)
		{
			const int32 A = TriIndices[T], B = TriIndices[T + 1], C = TriIndices[T + 2];
			const FVector2D UV = (UVOf(A) + UVOf(B) + UVOf(C)) / 3.f;
			const int32 Pid = PanelIdAt(UV.X, UV.Y);
			TArray<int32>& Dest = ((Pid % 2) == 0) ? TriIndicesEven : TriIndicesOdd;
			Dest.Add(A); Dest.Add(B); Dest.Add(C);
		}
	}

	// --- Seam polylines (UV → grid index chains). Topology fixed at build;
	// PushCodeZeroMesh only updates ribbon verts (no CreateMeshSection/frame).
	TArray<TArray<FVector2D>> SeamUVs;

	// 1) Centre-origin star
	for (int32 K = 0; K < NWedges; ++K)
	{
		const FVector2D EdgePt = PerimeterPoint(float(K) / float(NWedges));
		TArray<FVector2D> Path;
		AppendSeamUV(Path, CenUV, EdgePt, SeamSamples);
		SeamUVs.Add(MoveTemp(Path));
	}
	// 2) Concentric rings
	for (int32 R = 1; R < NRings; ++R)
	{
		const float Frac = float(R) / float(NRings);
		TArray<FVector2D> Ring;
		const int32 RingSamples = NWedges * 2;
		for (int32 K = 0; K <= RingSamples; ++K)
		{
			Ring.Add(FMath::Lerp(CenUV, PerimeterPoint(float(K) / float(RingSamples)), Frac));
		}
		SeamUVs.Add(MoveTemp(Ring));
	}
	// 3) Corner radials (tri-radial load paths)
	for (int32 K = 1; K < NCornerRad; ++K)
	{
		const float T = float(K) / float(NCornerRad);
		{
			TArray<FVector2D> Path;
			AppendSeamUV(Path, HeadUV, FMath::Lerp(TackUV, ClewUV, T), SeamSamples);
			SeamUVs.Add(MoveTemp(Path));
		}
		{
			TArray<FVector2D> Path;
			AppendSeamUV(Path, TackUV, FMath::Lerp(ClewUV, HeadUV, T), SeamSamples);
			SeamUVs.Add(MoveTemp(Path));
		}
		{
			TArray<FVector2D> Path;
			AppendSeamUV(Path, ClewUV, FMath::Lerp(TackUV, HeadUV, T), SeamSamples);
			SeamUVs.Add(MoveTemp(Path));
		}
	}
	// 4) Perimeter tapes
	{
		TArray<FVector2D> LuffP, LeechP, FootP;
		AppendSeamUV(LuffP, TackUV, HeadUV, SeamSamples);
		AppendSeamUV(LeechP, ClewUV, HeadUV, SeamSamples);
		AppendSeamUV(FootP, TackUV, ClewUV, SeamSamples);
		SeamUVs.Add(MoveTemp(LuffP));
		SeamUVs.Add(MoveTemp(LeechP));
		SeamUVs.Add(MoveTemp(FootP));
	}

	SeamPaths.Reset();
	SeamRibbonVertCount = 0;
	SeamPaths.Reserve(SeamUVs.Num());
	for (const TArray<FVector2D>& UVPath : SeamUVs)
	{
		TArray<int32> Chain;
		Chain.Reserve(UVPath.Num());
		int32 PrevIdx = INDEX_NONE;
		for (const FVector2D& UV : UVPath)
		{
			const int32 Ii = FMath::Clamp(FMath::RoundToInt(UV.Y * float(Nu - 1)), 0, Nu - 1);
			const int32 Jj = FMath::Clamp(FMath::RoundToInt(UV.X * float(Nw - 1)), 0, Nw - 1);
			const int32 Idx = GridIdx(Ii, Jj);
			if (Idx != PrevIdx)
			{
				Chain.Add(Idx);
				PrevIdx = Idx;
			}
		}
		if (Chain.Num() >= 2)
		{
			SeamRibbonVertCount += Chain.Num() * 2; // L+R per sample
			SeamPaths.Add(MoveTemp(Chain));
		}
	}
}

FVector FSailClothSim::SampleAtUV(float U, float V) const
{
	if (!bInitialized || Nu < 2 || Nw < 2 || Pos.Num() < Nu * Nw)
	{
		return FVector::ZeroVector;
	}
	U = FMath::Clamp(U, 0.f, 1.f);
	V = FMath::Clamp(V, 0.f, 1.f);
	const float Fi = V * float(Nu - 1);
	const float Fj = U * float(Nw - 1);
	const int32 I0 = FMath::Clamp(int32(Fi), 0, Nu - 2);
	const int32 J0 = FMath::Clamp(int32(Fj), 0, Nw - 2);
	const float Ti = Fi - float(I0);
	const float Tj = Fj - float(J0);
	const FVector P00 = Pos[GridIdx(I0, J0)];
	const FVector P10 = Pos[GridIdx(I0 + 1, J0)];
	const FVector P01 = Pos[GridIdx(I0, J0 + 1)];
	const FVector P11 = Pos[GridIdx(I0 + 1, J0 + 1)];
	return FMath::Lerp(FMath::Lerp(P00, P10, Ti), FMath::Lerp(P01, P11, Ti), Tj);
}

FVector FSailClothSim::SampleNormalAtUV(float U, float V) const
{
	if (!bInitialized || Normals.Num() < Nu * Nw || Nu < 2 || Nw < 2)
	{
		return FVector(0.f, 1.f, 0.f);
	}
	U = FMath::Clamp(U, 0.f, 1.f);
	V = FMath::Clamp(V, 0.f, 1.f);
	const float Fi = V * float(Nu - 1);
	const float Fj = U * float(Nw - 1);
	const int32 I0 = FMath::Clamp(int32(Fi), 0, Nu - 2);
	const int32 J0 = FMath::Clamp(int32(Fj), 0, Nw - 2);
	const float Ti = Fi - float(I0);
	const float Tj = Fj - float(J0);
	FVector N =
		FMath::Lerp(
			FMath::Lerp(Normals[GridIdx(I0, J0)], Normals[GridIdx(I0 + 1, J0)], Ti),
			FMath::Lerp(Normals[GridIdx(I0, J0 + 1)], Normals[GridIdx(I0 + 1, J0 + 1)], Ti),
			Tj);
	if (!N.Normalize()) N = FVector(0.f, 1.f, 0.f);
	return N;
}

void FSailClothSim::PushCodeZeroMesh(UProceduralMeshComponent* Mesh) const
{
	// Section 0 = body (update verts every frame).
	// Section 1 = seam ribbons (topology fixed at first create; verts only after).
	// Never CreateMeshSection every frame — that froze PIE.
	if (!Mesh || !bInitialized || Nu < 2 || Nw < 2 || Pos.Num() == 0) return;

	const int32 N = Pos.Num();
	TArray<FVector> Verts;
	TArray<FVector> Nrms;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Cols;
	TArray<FProcMeshTangent> Tans;
	Verts.SetNum(N);
	Nrms.SetNum(N);
	UV.SetNum(N);
	Cols.SetNum(N);
	Tans.SetNum(N);

	const FLinearColor Fallback(0.08f, 0.28f, 0.62f, 1.f);
	for (int32 I = 0; I < N; ++I)
	{
		FVector P = Pos.IsValidIndex(I) ? Pos[I] : FVector::ZeroVector;
		if (P.ContainsNaN() || !FMath::IsFinite(P.X))
		{
			P = FlatRest.IsValidIndex(I) ? FlatRest[I] : FVector::ZeroVector;
		}
		Verts[I] = P;
		FVector Nrm = Normals.IsValidIndex(I) ? Normals[I] : FVector(0.f, 1.f, 0.f);
		if (!Nrm.Normalize()) Nrm = FVector(0.f, 1.f, 0.f);
		Nrms[I] = Nrm;
		if (ChordU.IsValidIndex(I) && HeightV.IsValidIndex(I))
		{
			UV[I] = FVector2D(ChordU[I], HeightV[I]);
		}
		else
		{
			const int32 Ii = (Nw > 0) ? (I / Nw) : 0;
			const int32 Jj = (Nw > 0) ? (I % Nw) : 0;
			UV[I] = FVector2D(
				(Nw > 1) ? float(Jj) / float(Nw - 1) : 0.f,
				(Nu > 1) ? float(Ii) / float(Nu - 1) : 0.f);
		}
		Cols[I] = VertColors.IsValidIndex(I) ? VertColors[I] : Fallback;
		Cols[I].A = 1.f;
		const FVector Tx = FVector::CrossProduct(Nrm, FVector::UpVector).GetSafeNormal();
		Tans[I] = FProcMeshTangent(Tx.IsNearlyZero() ? FVector(1.f, 0.f, 0.f) : Tx, false);
	}

	// --- Body section 0 ---
	const bool bBodyOk = Mesh->GetNumSections() > 0
		&& Mesh->GetProcMeshSection(0)
		&& Mesh->GetProcMeshSection(0)->ProcVertexBuffer.Num() == N;
	if (bBodyOk)
	{
		Mesh->UpdateMeshSection_LinearColor(0, Verts, Nrms, UV, Cols, Tans);
	}
	else
	{
		const TArray<int32>& Front = (TriIndices.Num() >= 3) ? TriIndices : TriIndicesEven;
		if (Front.Num() < 3) return;
		TArray<int32> Tris = Front;
		for (int32 T = 0; T + 2 < Front.Num(); T += 3)
		{
			Tris.Add(Front[T]);
			Tris.Add(Front[T + 2]);
			Tris.Add(Front[T + 1]);
		}
		for (int32 Si = Mesh->GetNumSections() - 1; Si >= 0; --Si)
		{
			Mesh->ClearMeshSection(Si);
		}
		Mesh->CreateMeshSection_LinearColor(0, Verts, Tris, Nrms, UV, Cols, Tans, false);
	}
	if (FProcMeshSection* Body = Mesh->GetProcMeshSection(0))
	{
		Body->bSectionVisible = true;
	}

	// --- Seam ribbons section 1 (optional) ---
	if (SeamPaths.Num() == 0 || SeamRibbonVertCount < 4) return;

	TArray<FVector> SVerts;
	TArray<int32> STris;
	TArray<FVector> SNrms;
	TArray<FVector2D> SUV;
	TArray<FLinearColor> SCols;
	TArray<FProcMeshTangent> STans;
	SVerts.Reserve(SeamRibbonVertCount);
	SNrms.Reserve(SeamRibbonVertCount);
	SUV.Reserve(SeamRibbonVertCount);
	SCols.Reserve(SeamRibbonVertCount);
	STans.Reserve(SeamRibbonVertCount);

	const FLinearColor SeamCol(0.02f, 0.05f, 0.12f, 1.f);
	constexpr float HalfW = 1.1f;
	constexpr float Lift = 0.40f;

	for (const TArray<int32>& Chain : SeamPaths)
	{
		if (Chain.Num() < 2) continue;
		TArray<FVector, TInlineAllocator<32>> Centre;
		TArray<FVector, TInlineAllocator<32>> ChainN;
		Centre.SetNum(Chain.Num());
		ChainN.SetNum(Chain.Num());
		for (int32 C = 0; C < Chain.Num(); ++C)
		{
			const int32 Idx = Chain[C];
			FVector P = (Pos.IsValidIndex(Idx) && !Pos[Idx].ContainsNaN())
				? Pos[Idx]
				: (FlatRest.IsValidIndex(Idx) ? FlatRest[Idx] : FVector::ZeroVector);
			FVector Nrm = Normals.IsValidIndex(Idx) ? Normals[Idx] : FVector(0.f, 1.f, 0.f);
			if (!Nrm.Normalize()) Nrm = FVector(0.f, 1.f, 0.f);
			Centre[C] = P + Nrm * Lift;
			ChainN[C] = Nrm;
		}

		const int32 Base = SVerts.Num();
		for (int32 S = 0; S < Centre.Num(); ++S)
		{
			FVector T = (S + 1 < Centre.Num())
				? (Centre[S + 1] - Centre[S])
				: (Centre[S] - Centre[S - 1]);
			if (!T.Normalize()) T = FVector(1.f, 0.f, 0.f);
			FVector Side = FVector::CrossProduct(ChainN[S], T);
			if (!Side.Normalize())
			{
				Side = FVector::CrossProduct(FVector::UpVector, T).GetSafeNormal();
			}
			SVerts.Add(Centre[S] - Side * HalfW);
			SVerts.Add(Centre[S] + Side * HalfW);
			SNrms.Add(ChainN[S]);
			SNrms.Add(ChainN[S]);
			SUV.Add(FVector2D(0.f, float(S)));
			SUV.Add(FVector2D(1.f, float(S)));
			SCols.Add(SeamCol);
			SCols.Add(SeamCol);
			STans.Add(FProcMeshTangent(T, false));
			STans.Add(FProcMeshTangent(T, false));
		}
		for (int32 S = 0; S + 1 < Centre.Num(); ++S)
		{
			const int32 I0 = Base + S * 2;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + 2;
			const int32 I3 = I0 + 3;
			STris.Add(I0); STris.Add(I2); STris.Add(I1);
			STris.Add(I1); STris.Add(I2); STris.Add(I3);
			STris.Add(I0); STris.Add(I1); STris.Add(I2);
			STris.Add(I1); STris.Add(I3); STris.Add(I2);
		}
	}

	if (SVerts.Num() < 4 || STris.Num() < 3) return;

	const bool bSeamOk = Mesh->GetNumSections() > 1
		&& Mesh->GetProcMeshSection(1)
		&& Mesh->GetProcMeshSection(1)->ProcVertexBuffer.Num() == SVerts.Num();
	if (bSeamOk)
	{
		Mesh->UpdateMeshSection_LinearColor(1, SVerts, SNrms, SUV, SCols, STans);
	}
	else
	{
		if (Mesh->GetNumSections() > 1)
		{
			Mesh->ClearMeshSection(1);
		}
		Mesh->CreateMeshSection_LinearColor(1, SVerts, STris, SNrms, SUV, SCols, STans, false);
	}
	if (FProcMeshSection* Seam = Mesh->GetProcMeshSection(1))
	{
		Seam->bSectionVisible = true;
	}
}

float FSailClothSim::ApplyRollerFurl(float Set01)
{
	// Only the class jib uses a forestay furler (main is on the mast, kite is free-flying).
	if (RigKind != ESailRigKind::Jib || !bStayValid || !bGridTopology || Nu < 2 || Nw < 2)
	{
		return 0.f;
	}
	const float Set = FMath::Clamp(Set01, 0.f, 1.f);
	const float Furl = 1.f - Set; // 0 = flying, 1 = fully wrapped
	// Linear wrap progress (no smoothstep) so the last 20% of Set still moves cloth.
	const float F = Furl;
	if (F < 1e-5f)
	{
		return 0.f;
	}

	FVector Axis = StayHeadLocal - StayTackLocal;
	const float StayLen = FMath::Max(1.f, Axis.Size());
	Axis /= StayLen;

	// Foil radius + multi-turn wrap (Harken MKIV-style visual).
	const float FoilR = 3.8f;       // cm
	const float MaxTurns = 5.5f;    // full wraps when doused
	const float Layer = 0.45f;      // cm of sail pack per chord station
	const float WrapRad = F * MaxTurns * 2.f * PI;

	for (int32 I = 0; I < Nu; ++I)
	{
		const float U = (Nu > 1) ? float(I) / float(Nu - 1) : 0.f;
		const FVector OnStay = StayTackLocal + Axis * (U * StayLen);

		for (int32 J = 0; J < Nw; ++J)
		{
			const int32 Idx = GridIdx(I, J);
			if (!Pos.IsValidIndex(Idx) || !FlatRest.IsValidIndex(Idx)) continue;

			// Luff rides the stay (foil mesh is separate).
			if (J == 0)
			{
				Pos[Idx] = Prev[Idx] = OnStay;
				continue;
			}

			const float Chord = float(J) / float(Nw - 1); // 0 near luff … 1 at leech
			// ALWAYS morph from loft rest → spiral. Never from live Pos / SnapRigToStay
			// (that collapsed mid-furl and popped open mid-unfurl).
			const FVector Pref = FlatRest[Idx];
			FVector Rad = Pref - OnStay;
			Rad -= Axis * FVector::DotProduct(Rad, Axis);
			float R0 = Rad.Size();
			if (R0 < 0.5f)
			{
				FVector Hint = FVector::CrossProduct(Axis, FVector(0.f, 0.f, 1.f));
				if (Hint.SizeSquared() < 1e-4f)
				{
					Hint = FVector::CrossProduct(Axis, FVector(1.f, 0.f, 0.f));
				}
				Rad = Hint.GetSafeNormal() * 20.f;
				R0 = 20.f;
			}
			const FVector RadDir = Rad / R0;
			const FVector POpen = OnStay + RadDir * R0; // open loft in stay frame

			// Spiral onto the foil: leech packs outermost after more wrap.
			const float RFurl = FoilR + Chord * float(Nw) * Layer + 1.2f;
			const float Ang = WrapRad * (0.12f + 0.88f * Chord);
			const float C = FMath::Cos(Ang);
			const float S = FMath::Sin(Ang);
			const FVector RotDir =
				RadDir * C
				+ FVector::CrossProduct(Axis, RadDir) * S
				+ Axis * FVector::DotProduct(Axis, RadDir) * (1.f - C);

			const FVector PFurl = OnStay + RotDir * RFurl;
			// Pure geometric blend: F=0 open loft, F=1 fully rolled sausage.
			Pos[Idx] = FMath::Lerp(POpen, PFurl, F);
			Prev[Idx] = Pos[Idx]; // freeze Verlet while furl-driven
		}
	}

	// Headboard eases onto the foil only near full furl (no early collapse).
	if (F > 0.75f && HeadRowIndices.Num() > 0)
	{
		const float Hf = (F - 0.75f) / 0.25f;
		for (const int32 Idx : HeadRowIndices)
		{
			if (!Pos.IsValidIndex(Idx)) continue;
			Pos[Idx] = FMath::Lerp(Pos[Idx], StayHeadLocal, Hf);
			Prev[Idx] = Pos[Idx];
		}
	}

	return WrapRad;
}

/** Restore free cloth from loft rest so aero Step can resume after unfurl. */
void FSailClothSim::SeedOpenFromLoft()
{
	if (!bGridTopology || Pos.Num() == 0) return;
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (!FlatRest.IsValidIndex(I)) continue;
		// Leave hard pins alone (luff/head will be reasserted by Step/Snap).
		if (bPinned.IsValidIndex(I) && bPinned[I] && I != ClewIndex) continue;
		Pos[I] = Prev[I] = FlatRest[I];
	}
	if (bStayValid)
	{
		SnapRigToStay();
	}
}

void FSailClothSim::SnapRigToStay()
{
	if (!bStayValid || LuffIndices.Num() == 0) return;
	FVector Dir = StayHeadLocal - StayTackLocal;
	const float Len = FMath::Max(1.f, Dir.Size());
	Dir /= Len;
	for (int32 K = 0; K < LuffIndices.Num(); ++K)
	{
		const int32 I = LuffIndices[K];
		float U = LuffU.IsValidIndex(K) ? LuffU[K] : 0.f;
		if (I == TackIndex) U = 0.f;
		if (I == HeadIndex) U = 1.f;
		FVector OnStay = StayTackLocal + Dir * (U * Len);
		if (RigKind == ESailRigKind::Jib && ForestaySag > 0.01f)
		{
			// Sag perpendicular in sail plane
			const FVector FootDir = (ClewIndex != INDEX_NONE)
				? (FlatRest[ClewIndex] - StayTackLocal).GetSafeNormal()
				: FVector(-1.f, 0.f, 0.f);
			FVector SagDir = FVector::CrossProduct(Dir, FVector::CrossProduct(FootDir, Dir)).GetSafeNormal();
			OnStay += SagDir * (4.f * U * (1.f - U) * ForestaySag * Len * 0.03f);
		}
		Pos[I] = Prev[I] = FlatRest[I] = ShapeTarget[I] = OnStay;
		bPinned[I] = 1;
	}
	// Re-pin headboard (non-luff head verts keep loft offset from head_inner)
	if (bGridTopology && HeadRowIndices.Num() == Nw)
	{
		const FVector HeadInner = Pos[GridIdx(Nu - 1, 0)];
		const FVector LoftInner = FlatRest[GridIdx(Nu - 1, 0)];
		// Actually FlatRest for head was overwritten only for luff — restore others from original
		// FlatRest for headboard was never moved except j=0. Good.
		for (int32 J = 1; J < Nw; ++J)
		{
			const int32 Idx = GridIdx(Nu - 1, J);
			// Keep loft headboard geometry relative to loft head_inner, placed at live head_inner
			const FVector Off = FlatRest[Idx] - LoftInner;
			// FlatRest head non-luff still original loft — but FlatRest[luff head] was moved
			// Use stored FlatRest which for J>0 is still loft coords
			Pos[Idx] = HeadInner + (FlatRest[Idx] - FlatRest[GridIdx(Nu - 1, 0)]);
			// Wait FlatRest[GridIdx(Nu-1,0)] was overwritten to OnStay. So Off from loft is wrong.
			// Fix: headboard FlatRest for J>0 is still original loft; loft head_inner is StayHeadLocal originally
			Pos[Idx] = FlatRest[Idx]; // pin at loft headboard
			Prev[Idx] = Pos[Idx];
			bPinned[Idx] = 1;
		}
	}
}

void FSailClothSim::ReassertLuffOnStay(const FVector& /*WindLocalDir*/)
{
	if (!bStayValid) return;

	// Asym / Code Zero: re-pin tack (sprit) + entire pointed head (halyard)
	if (RigKind == ESailRigKind::AsymSpin)
	{
		if (TackIndex != INDEX_NONE && Pos.IsValidIndex(TackIndex))
		{
			Pos[TackIndex] = Prev[TackIndex] = StayTackLocal;
			bPinned[TackIndex] = 1;
		}
		if (HeadRowIndices.Num() > 0)
		{
			for (const int32 Idx : HeadRowIndices)
			{
				if (!Pos.IsValidIndex(Idx)) continue;
				Pos[Idx] = Prev[Idx] = StayHeadLocal;
				bPinned[Idx] = 1;
			}
		}
		else if (HeadIndex != INDEX_NONE && Pos.IsValidIndex(HeadIndex))
		{
			Pos[HeadIndex] = Prev[HeadIndex] = StayHeadLocal;
			bPinned[HeadIndex] = 1;
		}
		return;
	}

	if (LuffIndices.Num() == 0) return;
	FVector Dir = StayHeadLocal - StayTackLocal;
	const float Len = FMath::Max(1.f, Dir.Size());
	Dir /= Len;
	for (int32 K = 0; K < LuffIndices.Num(); ++K)
	{
		const int32 I = LuffIndices[K];
		float U = LuffU.IsValidIndex(K) ? LuffU[K] : float(K) / float(FMath::Max(1, LuffIndices.Num() - 1));
		FVector OnStay = StayTackLocal + Dir * (U * Len);
		if (RigKind == ESailRigKind::Jib && ForestaySag > 0.01f && ClewIndex != INDEX_NONE)
		{
			const FVector FootDir = (Pos[ClewIndex] - StayTackLocal).GetSafeNormal();
			FVector SagDir = FVector::CrossProduct(Dir, FVector::CrossProduct(FootDir, Dir)).GetSafeNormal();
			OnStay += SagDir * (4.f * U * (1.f - U) * ForestaySag * Len * 0.03f);
		}
		Pos[I] = Prev[I] = OnStay;
		bPinned[I] = 1;
	}
	// Headboard hard pin
	for (const int32 Idx : HeadRowIndices)
	{
		if (Idx == HeadIndex) continue;
		if (bGridTopology)
		{
			// Stay at loft headboard (class head width)
			Pos[Idx] = FlatRest[Idx];
			Prev[Idx] = FlatRest[Idx];
		}
		bPinned[Idx] = 1;
	}
}

void FSailClothSim::BuildClewPatches()
{
	// Neighborhood of free verts that translate with the clew (rigid, no falloff).
	ClewPatchMain.Reset();
	ClewPatchJib.Reset();
	if (!bGridTopology || Nu < 2 || Nw < 2 || ClewIndex == INDEX_NONE) return;

	const int32 ClewI = 0;
	const int32 ClewJ = Nw - 1;

	// Main: small corner patch (manhattan ≤ 4)
	for (int32 Di = 0; Di <= 4 && ClewI + Di < Nu; ++Di)
	{
		for (int32 Dj = 0; Dj <= 4 && ClewJ - Dj >= 0; ++Dj)
		{
			if (Di + Dj > 4) continue;
			ClewPatchMain.Add(GridIdx(ClewI + Di, ClewJ - Dj));
		}
	}
	// Jib: a bit further up the leech (sheet load path)
	for (int32 Di = 0; Di <= 10 && ClewI + Di < Nu; ++Di)
	{
		for (int32 Dj = 0; Dj <= 4 && ClewJ - Dj >= 0; ++Dj)
		{
			if (Di + Dj > 10) continue;
			ClewPatchJib.Add(GridIdx(ClewI + Di, ClewJ - Dj));
		}
	}
}

void FSailClothSim::TranslateClewPatch(const FVector& Delta, bool bJibLeechStyle)
{
	// Working fix #1: move the free corner as a rigid unit with the clew.
	// Snapping only the clew vertex is what sheared the grid into parallelograms.
	if (Delta.SizeSquared() < 1e-8f) return;
	if (ClewPatchMain.Num() == 0 && bGridTopology)
	{
		BuildClewPatches();
	}
	const TArray<int32>& Patch = bJibLeechStyle ? ClewPatchJib : ClewPatchMain;
	if (Patch.Num() == 0)
	{
		if (ClewIndex != INDEX_NONE && Pos.IsValidIndex(ClewIndex))
		{
			Pos[ClewIndex] += Delta;
			Prev[ClewIndex] += Delta;
		}
		return;
	}
	for (const int32 Idx : Patch)
	{
		if (!Pos.IsValidIndex(Idx)) continue;
		if (bPinned.IsValidIndex(Idx) && bPinned[Idx] && Idx != ClewIndex) continue;
		Pos[Idx] += Delta;
		Prev[Idx] += Delta;
	}
}

void FSailClothSim::ApplyMainBoomConstraints(const FVector& BoomTipLocal)
{
	// Place clew on gooseneck→boomTip at outhaul distance; rigid-translate patch.
	if (!Pos.IsValidIndex(ClewIndex) || !Pos.IsValidIndex(TackIndex)) return;

	const FVector Goose = StayTackLocal;
	const float ClassBoom = (BoomLenCm > 1.f) ? BoomLenCm
		: ((LoftFootLen > 1.f) ? LoftFootLen : FVector::Dist(Goose, BoomTipLocal));
	const float OuthaulD = FMath::Max(1.f, ClassBoom * OuthaulFrac());

	FVector Dir = BoomTipLocal - Goose;
	if (Dir.SizeSquared() < 1.f) Dir = Pos[ClewIndex] - Goose;
	if (Dir.SizeSquared() < 1.f) Dir = FlatRest[ClewIndex] - Goose;
	if (Dir.SizeSquared() < 1.f) Dir = FVector(-1.f, 0.f, 0.f);
	Dir.Normalize();

	FVector ClewTarget = Goose + Dir * OuthaulD;

	// Match boom tip height (vang drives tip Z on the spar). Hard on → low tip.
	const float Vang = FMath::Clamp(Vang01, 0.f, 1.f);
	const float TipRise = BoomTipLocal.Z - Goose.Z;
	const float MaxRise = FMath::Max(TipRise + 2.f, ClassBoom * FMath::Lerp(0.02f, 0.16f, Vang));
	if (ClewTarget.Z - Goose.Z > MaxRise)
	{
		FVector Horiz = ClewTarget - Goose;
		Horiz.Z = 0.f;
		const float NeedH = FMath::Sqrt(FMath::Max(1.f, OuthaulD * OuthaulD - MaxRise * MaxRise));
		if (Horiz.Size() > 1.f)
		{
			Horiz = Horiz.GetSafeNormal() * NeedH;
			ClewTarget = Goose + Horiz + FVector(0.f, 0.f, MaxRise);
		}
		else
		{
			ClewTarget.Z = Goose.Z + MaxRise;
		}
	}
	// Soft pull: when hard vang, prefer clew Z near boom tip Z
	if (Vang < 0.85f)
	{
		const float Hard = 1.f - Vang;
		ClewTarget.Z = FMath::Lerp(ClewTarget.Z, BoomTipLocal.Z, 0.55f * Hard);
	}

	TranslateClewPatch(ClewTarget - Pos[ClewIndex], /*bJibLeechStyle*/ false);
	Pos[ClewIndex] = ClewTarget;
	Prev[ClewIndex] = ClewTarget;
	Pos[TackIndex] = StayTackLocal;
	Prev[TackIndex] = StayTackLocal;
	FootRestLen = OuthaulD;
}

void FSailClothSim::ApplyClewJib(
	float SheetEase01,
	const FVector& LeadPort,
	const FVector& LeadStbd,
	bool bLeeToStarboard)
{
	if (!Pos.IsValidIndex(ClewIndex)) return;

	const float LoftFoot = (LoftFootLen > 1.f) ? LoftFootLen
		: FVector::Dist(StayTackLocal, FlatRest[ClewIndex]);

	const FVector LeeLead = bLeeToStarboard ? LeadStbd : LeadPort;
	const float Ease = FMath::Clamp(SheetEase01, 0.f, 1.f);

	// Asym / Code Zero: allow longer sheet so the clew can reach an outboard
	// quarter lead (still short of a full runner foot).
	float MinSheet, MaxSheet;
	if (RigKind == ESailRigKind::AsymSpin)
	{
		MinSheet = FMath::Max(50.f, LoftFoot * 0.18f);
		MaxSheet = FMath::Max(MinSheet + 100.f, LoftFoot * 0.85f);
	}
	else
	{
		// Web jib: min ≈ foot*0.18, max ≈ foot*0.85
		MinSheet = FMath::Max(25.f, LoftFoot * 0.18f);
		MaxSheet = FMath::Max(MinSheet + 50.f, LoftFoot * 0.95f);
	}
	const float SheetLen = FMath::Lerp(MinSheet, MaxSheet, Ease);

	auto HaulClewTo = [&](const FVector& Target)
	{
		TranslateClewPatch(Target - Pos[ClewIndex], /*bJibLeechStyle*/ true);
		Pos[ClewIndex] = Target;
		Prev[ClewIndex] = Target;
	};

	{
		const FVector FromLead = Pos[ClewIndex] - LeeLead;
		const float Dist = FromLead.Size();
		if (Dist > SheetLen && Dist > 1.f)
		{
			HaulClewTo(LeeLead + FromLead * (SheetLen / Dist));
		}
	}

	if (TackIndex != INDEX_NONE)
	{
		Pos[TackIndex] = StayTackLocal;
		Prev[TackIndex] = StayTackLocal;
		const float Foot = FVector::Dist(Pos[ClewIndex], StayTackLocal);
		const float MaxFoot = LoftFoot * 1.05f;
		if (Foot > MaxFoot && Foot > 1.f)
		{
			HaulClewTo(StayTackLocal + (Pos[ClewIndex] - StayTackLocal) * (MaxFoot / Foot));
			const FVector FromLead = Pos[ClewIndex] - LeeLead;
			const float Dist = FromLead.Size();
			if (Dist > SheetLen && Dist > 1.f)
			{
				HaulClewTo(LeeLead + FromLead * (SheetLen / Dist));
			}
		}
	}
	FootRestLen = LoftFoot;
}

void FSailClothSim::SolveSoftSprings()
{
	// Bend / load-prop / battens only — sail can still billow.
	const float R = Relax;
	for (const FSpring& S : Springs)
	{
		if (S.Role != ESpringRole::Soft) continue;
		FVector& PA = Pos[S.A];
		FVector& PB = Pos[S.B];
		FVector Delta = PA - PB;
		const float Dist = Delta.Size();
		if (Dist < 1e-4f) continue;
		const float Diff = (Dist - S.RestLen) / Dist * R * S.Stiffness;
		const FVector Corr = Delta * Diff;
		const bool Ai = bPinned.IsValidIndex(S.A) && bPinned[S.A] != 0;
		const bool Aj = bPinned.IsValidIndex(S.B) && bPinned[S.B] != 0;
		if (Ai && Aj) continue;
		if (Ai) { PB += Corr * 2.f; }
		else if (Aj) { PA -= Corr * 2.f; }
		else { PA -= Corr; PB += Corr; }
	}
}

void FSailClothSim::SolveInextensibleGrid()
{
	// Working fix #2: exact rest on structural + shear (pin-aware).
	// Fixed edges + diagonals ⇒ cells cannot shear; free surface still moves.
	for (const FSpring& S : Springs)
	{
		if (S.Role != ESpringRole::Inextensible) continue;
		if (S.RestLen < 1e-4f) continue;
		if (!Pos.IsValidIndex(S.A) || !Pos.IsValidIndex(S.B)) continue;

		FVector& PA = Pos[S.A];
		FVector& PB = Pos[S.B];
		FVector Delta = PA - PB;
		const float Dist = Delta.Size();
		if (Dist < 1e-5f) continue;

		const float Diff = (Dist - S.RestLen) / Dist;
		const FVector Corr = Delta * Diff;

		const bool Ai = bPinned.IsValidIndex(S.A) && bPinned[S.A] != 0;
		const bool Aj = bPinned.IsValidIndex(S.B) && bPinned[S.B] != 0;
		if (Ai && Aj) continue;
		if (Ai) { PB += Corr; }
		else if (Aj) { PA -= Corr; }
		else
		{
			PA -= Corr * 0.5f;
			PB += Corr * 0.5f;
		}
	}
}

void FSailClothSim::EnforceInextensibleWithClewHeld(int32 Passes)
{
	// Working fix #3: after boom/sheet, hold clew fixed and project free cloth
	// onto exact edge/diagonal lengths. Never leave boom as the last op.
	if (Passes < 1) return;
	const bool bHold = (ClewIndex != INDEX_NONE && Pos.IsValidIndex(ClewIndex));
	const uint8 Restore = bHold ? bPinned[ClewIndex] : 0;
	if (bHold) bPinned[ClewIndex] = 1;
	const uint8 TackRestore = (TackIndex != INDEX_NONE && bPinned.IsValidIndex(TackIndex))
		? bPinned[TackIndex] : 0;
	if (TackIndex != INDEX_NONE && bPinned.IsValidIndex(TackIndex)) bPinned[TackIndex] = 1;

	for (int32 P = 0; P < Passes; ++P)
	{
		SolveInextensibleGrid();
	}

	if (bHold) bPinned[ClewIndex] = Restore;
	if (TackIndex != INDEX_NONE && bPinned.IsValidIndex(TackIndex)) bPinned[TackIndex] = TackRestore;
}

void FSailClothSim::EnforcePerimeterEdges()
{
	// Project free-edge springs to rest
	for (const FSpring& S : Springs)
	{
		if (S.Edge == ESpringEdge::Body) continue;
		if (!Pos.IsValidIndex(S.A) || !Pos.IsValidIndex(S.B)) continue;
		FVector& PA = Pos[S.A];
		FVector& PB = Pos[S.B];
		FVector Delta = PB - PA;
		const float Len = Delta.Size();
		if (Len < 1e-4f) continue;
		const float MaxLen = S.RestLen * EdgeMaxStrain;
		if (Len <= MaxLen) continue;
		const FVector N = Delta / Len;
		if (!bPinned[S.A] && !bPinned[S.B])
		{
			const FVector Mid = (PA + PB) * 0.5f;
			PA = Mid - N * (MaxLen * 0.5f);
			PB = Mid + N * (MaxLen * 0.5f);
			Prev[S.A] = PA;
			Prev[S.B] = PB;
		}
		else if (!bPinned[S.A])
		{
			PA = PB - N * MaxLen;
			Prev[S.A] = PA;
		}
		else if (!bPinned[S.B])
		{
			PB = PA + N * MaxLen;
			Prev[S.B] = PB;
		}
	}

	// Foot chord cap
	if (TackIndex != INDEX_NONE && ClewIndex != INDEX_NONE && LoftFootLen > 1.f)
	{
		Pos[TackIndex] = StayTackLocal;
		Prev[TackIndex] = StayTackLocal;
		const float Foot = FVector::Dist(Pos[ClewIndex], StayTackLocal);
		const float MaxFoot = (RigKind == ESailRigKind::Main)
			? FMath::Max(LoftFootLen, BoomLenCm * OuthaulFrac()) * EdgeMaxStrain
			: LoftFootLen * 1.05f;
		if (Foot > MaxFoot && Foot > 1.f && !bPinned[ClewIndex])
		{
			Pos[ClewIndex] = StayTackLocal + (Pos[ClewIndex] - StayTackLocal) * (MaxFoot / Foot);
			Prev[ClewIndex] = Pos[ClewIndex];
		}
	}

	// Leech path: pull free leech verts so path ≤ loft path * strain
	if (LeechIndices.Num() >= 2 && LoftLeechPath > 1.f)
	{
		float Path = 0.f;
		for (int32 K = 0; K + 1 < LeechIndices.Num(); ++K)
			Path += FVector::Dist(Pos[LeechIndices[K]], Pos[LeechIndices[K + 1]]);
		const float MaxPath = LoftLeechPath * EdgeMaxStrain;
		if (Path > MaxPath && Path > 1.f)
		{
			const float Scale = MaxPath / Path;
			// Shrink segments from head (pinned) toward clew
			for (int32 K = 0; K + 1 < LeechIndices.Num(); ++K)
			{
				const int32 A = LeechIndices[K];
				const int32 B = LeechIndices[K + 1];
				if (bPinned[B]) continue;
				const FVector Delta = Pos[B] - Pos[A];
				const float Seg = Delta.Size();
				if (Seg < 1e-4f) continue;
				const float Rest = FVector::Dist(FlatRest[A], FlatRest[B]) * Scale;
				// Move free endpoint
				if (!bPinned[A] && !bPinned[B])
				{
					// Prefer moving B (toward head is pinned often)
					Pos[B] = Pos[A] + Delta * (FMath::Min(Seg, Rest * EdgeMaxStrain) / Seg);
					Prev[B] = Pos[B];
				}
				else if (!bPinned[B])
				{
					Pos[B] = Pos[A] + Delta.GetSafeNormal() * FMath::Min(Seg, Rest * EdgeMaxStrain);
					Prev[B] = Pos[B];
				}
			}
		}
	}

	// Soft roach envelope: leech verts near head→clew chord (class roach ~3.5% main)
	if (LeechIndices.Num() >= 3 && ClewIndex != INDEX_NONE)
	{
		const int32 LeechHead = LeechIndices.Last();
		const FVector HeadP = Pos[LeechHead];
		const FVector ClewP = Pos[ClewIndex];
		const FVector Axis = ClewP - HeadP;
		const float AxisLen = Axis.Size();
		if (AxisLen > 1.f)
		{
			const FVector AxisN = Axis / AxisLen;
			// Soft envelope — main tighter so leech doesn't bag open.
			const float RoachFrac = (RigKind == ESailRigKind::Main) ? 0.06f : 0.10f;
			const float MaxOff = AxisLen * RoachFrac + ChordLen * MaxCamberFrac * 0.55f;
			for (int32 K = 1; K + 1 < LeechIndices.Num(); ++K)
			{
				const int32 I = LeechIndices[K];
				if (bPinned[I]) continue;
				const float T = FMath::Clamp(FVector::DotProduct(Pos[I] - HeadP, AxisN) / AxisLen, 0.f, 1.f);
				const FVector OnChord = HeadP + AxisN * (T * AxisLen);
				const FVector Off = Pos[I] - OnChord;
				const float OffLen = Off.Size();
				const float Allow = MaxOff * (0.35f + 0.65f * FMath::Sin(PI * T)) + AxisLen * 0.03f;
				if (OffLen > Allow && OffLen > 1e-3f)
				{
					Pos[I] = OnChord + Off * (Allow / OffLen);
					// Do NOT zero Prev — keeps leech velocity / flutter alive
				}
			}
		}
	}
}

void FSailClothSim::MeasureEdgeLengths()
{
	LiveFootPathCm = 0.f;
	for (int32 K = 0; K + 1 < FootIndices.Num(); ++K)
		LiveFootPathCm += FVector::Dist(Pos[FootIndices[K]], Pos[FootIndices[K + 1]]);
	LiveLeechPathCm = 0.f;
	for (int32 K = 0; K + 1 < LeechIndices.Num(); ++K)
		LiveLeechPathCm += FVector::Dist(Pos[LeechIndices[K]], Pos[LeechIndices[K + 1]]);
	LiveFootChordCm = (TackIndex != INDEX_NONE && ClewIndex != INDEX_NONE)
		? FVector::Dist(Pos[TackIndex], Pos[ClewIndex]) : 0.f;
	const int32 LH = LeechIndices.Num() ? LeechIndices.Last() : HeadIndex;
	LiveLeechChordCm = (LH != INDEX_NONE && ClewIndex != INDEX_NONE)
		? FVector::Dist(Pos[LH], Pos[ClewIndex]) : 0.f;
}

void FSailClothSim::Step(
	float Dt,
	const FVector& WindLocalDir,
	float WindSpeedKn,
	const FVector& ClewTargetLocal,
	float SheetEase01,
	const FVector& SheetLeadPortLocal,
	const FVector& SheetLeadStbdLocal,
	bool bLeeToStarboard)
{
	// Faithful port of web SailCloth.step() + hStepSailCloth wind scale.
	// See sail-sim/frontend/public/webgl-utils.sailing.js
	if (!bInitialized || !bEnabled || Pos.Num() == 0) return;

	// Web always steps at 1/60 for cloth (independent of display dt spikes)
	const float StepDt = 1.f / 60.f;
	(void)Dt;

	// 0) Luff on stay/mast BEFORE step (web hStepSailCloth)
	ReassertLuffOnStay(WindLocalDir);
	ApplyTrimTensions();
	ComputeNormals();

	// Web: windSpeed = kn * 0.15  (NOT full knots / ft/s — tuned magnitude)
	const FVector Downwind = WindLocalDir.GetSafeNormal();
	const float Vkn = FMath::Max(0.f, WindSpeedKn);
	LastWindSpeedKn = Vkn;
	const float WindMag = Vkn * 0.15f; // web hStepSailCloth
	const FVector WindVelFt = Downwind * WindMag; // feet-scale tuned units

	// Incidence for HUD / force scale only
	const float Powered01 = EvaluateIncidenceAndLuff(Downwind, SheetEase01);

	const float AeroKUse = AeroK;
	const float DragKUse = DragK;
	const float Damp = Damping;
	const float MaxDCm = MaxDeltaCm;
	// Web gravity 32.2 * dt² feet → cm on UE +Z
	const float GravityStepCm = 32.2f * StepDt * StepDt * 30.48f;
	const float CmPerFt = 30.48f;

	float PeakWindPush = 0.f;

	// 1) Verlet + aero (web units for relative wind, convert force to cm)
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (bPinned[I])
		{
			// Hard anchors: hold still this frame (luff reasserted again after)
			Prev[I] = Pos[I];
			continue;
		}

		const FVector P = Pos[I];
		// Particle velocity in web "feet" units (pos delta / 30.48)
		FVector VelFt = (P - Prev[I]) * (Damp / CmPerFt);

		// Relative wind (web: windVel - particleVel)
		const FVector Rw = WindVelFt - VelFt;
		FVector Nrm = Normals.IsValidIndex(I) ? Normals[I] : FVector(0.f, 1.f, 0.f);
		if (!Nrm.Normalize()) Nrm = FVector(0.f, 1.f, 0.f);

		const float Wn = FVector::DotProduct(Rw, Nrm);
		// Surface normal pressure: aeroK * |wn| * wn * n  (feet)
		const float Aero = AeroKUse * FMath::Abs(Wn) * Wn;
		FVector DFt = Nrm * Aero;

		// Tangential drag
		const float Rspd = FMath::Max(Rw.Size(), 1e-6f);
		DFt += Rw * (DragKUse * Rspd);

		// Fill assist when shallow incidence (web)
		{
			const float CosInc = FMath::Abs(Wn) / Rspd;
			const float Shallow = 1.f - FMath::Min(1.f, CosInc / 0.35f);
			if (Shallow > 0.02f && Rspd > 0.4f)
			{
				const float Fill = AeroKUse * 0.55f * Shallow * Rspd;
				DFt += (Rw / Rspd) * Fill;
			}
		}

		// Gravity in −Z (UE up) — web −Y
		DFt.Z -= (32.2f * StepDt * StepDt); // still feet here

		// Clamp per-frame motion in feet, then to cm
		float DmFt = DFt.Size();
		const float MaxDFt = 0.35f;
		if (DmFt > MaxDFt && DmFt > 1e-8f)
		{
			DFt *= MaxDFt / DmFt;
			DmFt = MaxDFt;
		}
		const FVector DCm = DFt * CmPerFt;
		PeakWindPush = FMath::Max(PeakWindPush, DCm.Size());

		// Verlet in cm: particle vel was in cm
		const FVector VelCm = (P - Prev[I]) * Damp;
		Prev[I] = P;
		Pos[I] = P + VelCm + DCm;

		if (Pos[I].ContainsNaN()
			|| !FMath::IsFinite(Pos[I].X) || !FMath::IsFinite(Pos[I].Y) || !FMath::IsFinite(Pos[I].Z))
		{
			Pos[I] = Prev[I] = FlatRest[I];
		}
	}
	LastWindPushCm = PeakWindPush;
	(void)MaxDCm;
	(void)GravityStepCm;

	// 2) Soft bend → boom/sheet (rigid corner) → hard structural+shear (clew held).
	// Hard grid is always last in the iter so the corner cannot re-open.
	const int32 Iters = FMath::Clamp(ConstraintIters, 3, 8);
	const int32 HardN = FMath::Clamp(InextensiblePasses, 1, 6);
	for (int32 Iter = 0; Iter < Iters; ++Iter)
	{
		SolveSoftSprings();

		if (RigKind == ESailRigKind::Main)
		{
			ApplyMainBoomConstraints(ClewTargetLocal);
		}
		else
		{
			ApplyClewJib(SheetEase01, SheetLeadPortLocal, SheetLeadStbdLocal, bLeeToStarboard);
		}

		EnforceInextensibleWithClewHeld(HardN);
	}

	// 2b) Physical battens once per step (web: after constraint pass). Distance
	// ties + beam curvature hold captured camber / resist leech flutter.
	if (bBattensEngaged)
	{
		SolveBattens(WindLocalDir, Vkn);
		// Re-project hard grid so batten corrections don't stretch cells open
		EnforceInextensibleWithClewHeld(FMath::Max(1, HardN));
	}
	else if (BattenDist.Num() > 0)
	{
		// Capture billowed shape after ~1.25 s at 60 Hz (web: 75 frames)
		if (++BattenSettleFrames >= 75)
		{
			EngageBattens();
		}
	}

	// 3) Hard anchors (luff / headboard)
	ReassertLuffOnStay(WindLocalDir);
	if (RigKind == ESailRigKind::AsymSpin)
	{
		// Pointed head: force entire head row onto the halyard every step
		for (const int32 Idx : HeadRowIndices)
		{
			if (!Pos.IsValidIndex(Idx)) continue;
			Pos[Idx] = Prev[Idx] = StayHeadLocal;
			bPinned[Idx] = 1;
		}
	}
	else
	{
		for (const int32 Idx : HeadRowIndices)
		{
			if (!Pos.IsValidIndex(Idx)) continue;
			if (bGridTopology && bPinned[Idx] && Idx != ClewIndex)
			{
				if (Idx != HeadIndex || RigKind == ESailRigKind::Main)
				{
					if (!LuffIndices.Contains(Idx))
					{
						Pos[Idx] = FlatRest[Idx];
						Prev[Idx] = FlatRest[Idx];
					}
				}
			}
		}
	}

	// 4) Final attach + optional foot flatten, then hard grid only (no boom after).
	if (RigKind == ESailRigKind::Main)
	{
		ApplyMainBoomConstraints(ClewTargetLocal);
	}
	else
	{
		ApplyClewJib(SheetEase01, SheetLeadPortLocal, SheetLeadStbdLocal, bLeeToStarboard);
	}
	ReassertLuffOnStay(WindLocalDir);

	if (RigKind == ESailRigKind::Main && FootBoomPull() > 0.01f && ClewIndex != INDEX_NONE)
	{
		const float Pull = 0.5f * FootBoomPull();
		const FVector Goose = StayTackLocal;
		FVector Axis = Pos[ClewIndex] - Goose;
		const float AxisLen = Axis.Size();
		if (AxisLen > 1.f)
		{
			Axis /= AxisLen;
			for (int32 J = 1; J + 1 < FootIndices.Num(); ++J)
			{
				const int32 I = FootIndices[J];
				if (bPinned[I] || I == ClewIndex) continue;
				const float U = ChordU.IsValidIndex(I) ? ChordU[I] : float(J) / float(FootIndices.Num() - 1);
				const FVector OnBoom = Goose + Axis * (U * AxisLen);
				Pos[I] = FMath::Lerp(Pos[I], OnBoom, Pull);
				Prev[I] = FMath::Lerp(Prev[I], Pos[I], Pull);
			}
		}
	}

	EnforceInextensibleWithClewHeld(FMath::Clamp(InextensibleFinalPasses, 1, 8));

	if (ClewIndex != INDEX_NONE && Pos.IsValidIndex(ClewIndex))
	{
		Prev[ClewIndex] = Pos[ClewIndex];
	}
	if (TackIndex != INDEX_NONE) Prev[TackIndex] = Pos[TackIndex];
	for (const int32 Li : LuffIndices)
	{
		if (Pos.IsValidIndex(Li)) Prev[Li] = Pos[Li];
	}

	// ShapeTarget = Pos for measure/HUD (no force)
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		ShapeTarget[I] = Pos[I];
	}

	MeasureEdgeLengths();
	MeasureShape();
	(void)bLeeToStarboard;
	(void)Powered01;
}

void FSailClothSim::MeasureShape()
{
	if (!bInitialized || Pos.Num() == 0)
	{
		LastFillQuality = 0.85f;
		LastCamberCm = 0.f;
		LastStretchRatio = 1.f;
		LastCamberRatio = 0.f;
		return;
	}

	float PeakCamber = 0.f;
	float CamberSum = 0.f;
	int32 Free = 0;
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (bPinned[I] || I == ClewIndex) continue;
		const float V = HeightV.IsValidIndex(I) ? HeightV[I] : 0.5f;
		if (RigKind == ESailRigKind::Main && V < 0.12f) continue;
		const float D = FVector::Dist(Pos[I], FlatRest[I]);
		PeakCamber = FMath::Max(PeakCamber, D);
		CamberSum += D;
		++Free;
	}
	LastCamberCm = PeakCamber;
	LastCamberRatio = ChordLen > 1.f ? LastCamberCm / ChordLen : 0.f;
	const float MeanCamber = (Free > 0) ? (CamberSum / Free) : 0.f;

	float StretchSum = 0.f;
	int32 Sc = 0;
	for (const FSpring& S : Springs)
	{
		if (S.RestLen < 1.f) continue;
		StretchSum += FVector::Dist(Pos[S.A], Pos[S.B]) / S.RestLen;
		++Sc;
	}
	LastStretchRatio = (Sc > 0) ? (StretchSum / Sc) : 1.f;

	const float IdealPeak = ChordLen * MaxCamberFrac * 0.85f;
	const float CamberErr = IdealPeak > 1.f ? FMath::Abs(LastCamberCm - IdealPeak) / IdealPeak : 0.f;
	const float StretchErr = FMath::Abs(LastStretchRatio - 1.f) * 4.f;
	const float Depth = IdealPeak > 1.f ? FMath::Clamp(MeanCamber / (IdealPeak * 0.40f), 0.f, 1.2f) : 1.f;
	const float PoweredFill = 0.50f + 0.50f * LastPowered01;
	LastFillQuality = FMath::Clamp(
		(0.55f + 0.30f * Depth - 0.20f * CamberErr - 0.15f * StretchErr) * PoweredFill
			+ 0.15f * (1.f - LastLuffAmount),
		0.35f, 1.2f);
}

float FSailClothSim::GetForceScale() const
{
	if (!bInitialized || !bEnabled) return 1.f;
	const float LuffScale = FMath::Lerp(1.05f, 0.88f, LastLuffAmount);
	return FMath::Clamp(LastFillQuality * LuffScale, 0.40f, 1.2f);
}

void FSailClothSim::ForceAsymLeeSide(float LeeYSign)
{
	if (RigKind != ESailRigKind::AsymSpin || !bInitialized || Pos.Num() == 0) return;
	if (FMath::Abs(LeeYSign) < 0.5f) return;
	if (ClewIndex == INDEX_NONE || !Pos.IsValidIndex(ClewIndex)) return;

	const float S = (LeeYSign > 0.f) ? 1.f : -1.f; // +1 lee stbd, −1 lee port

	// Only flip when the clew is clearly on the windward side (avoids thrash).
	if (Pos[ClewIndex].Y * S >= -30.f) return;

	// Mirror free cloth across centreline. Head row (i = Nu-1) stays pinned at head.
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (I == TackIndex || I == HeadIndex) continue;
		if (Nw > 0 && Nu > 0 && (I / Nw) == (Nu - 1)) continue; // head ring
		Pos[I].Y = -Pos[I].Y;
		if (Prev.IsValidIndex(I)) Prev[I].Y = -Prev[I].Y;
		if (FlatRest.IsValidIndex(I)) FlatRest[I].Y = FMath::Abs(FlatRest[I].Y) * S;
		if (ShapeTarget.IsValidIndex(I)) ShapeTarget[I].Y = -ShapeTarget[I].Y;
	}
	// Guarantee clew on lee after flip
	if (Pos[ClewIndex].Y * S < 0.f)
	{
		Pos[ClewIndex].Y = FMath::Abs(Pos[ClewIndex].Y) * S;
		Prev[ClewIndex].Y = Pos[ClewIndex].Y;
	}
}

void FSailClothSim::PushToMesh(UProceduralMeshComponent* Mesh) const
{
	if (!bInitialized || !Mesh || SectionIndex < 0) return;

	// Code Zero: multi-section body (even/odd panels) + live seam ribbons
	if (RigKind == ESailRigKind::AsymSpin && bCodeZeroPanels)
	{
		PushCodeZeroMesh(Mesh);
		return;
	}

	FProcMeshSection* Sec = Mesh->GetProcMeshSection(SectionIndex);
	if (!Sec) return;
	// Mesh may be single-sided (N verts) or still matching cloth; reject size mismatch.
	if (Sec->ProcVertexBuffer.Num() != Pos.Num()) return;

	TArray<FVector> Vertices;
	TArray<FVector> MeshNormals;
	TArray<FVector2D> UV0;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	const int32 N = Pos.Num();
	Vertices.SetNum(N);
	MeshNormals.SetNum(N);
	UV0.SetNum(N);
	Colors.SetNum(N);
	Tangents.SetNum(N);

	// Bright cloth white (same both faces via TwoSided sail material)
	const FLinearColor SailCol(0.98f, 0.97f, 0.94f, 1.f);
	int32 Bad = 0;
	for (int32 I = 0; I < N; ++I)
	{
		FVector P = Pos.IsValidIndex(I) ? Pos[I] : FVector::ZeroVector;
		if (P.ContainsNaN() || !FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z))
		{
			P = FlatRest.IsValidIndex(I) ? FlatRest[I] : FVector::ZeroVector;
			++Bad;
		}
		Vertices[I] = P;

		FVector Nrm = Normals.IsValidIndex(I) ? Normals[I] : FVector(0.f, 1.f, 0.f);
		if (!Nrm.Normalize())
		{
			Nrm = FVector(0.f, 1.f, 0.f);
		}
		MeshNormals[I] = Nrm;

		if (ChordU.IsValidIndex(I) && HeightV.IsValidIndex(I))
		{
			UV0[I] = FVector2D(ChordU[I], HeightV[I]);
		}
		else
		{
			UV0[I] = Sec->ProcVertexBuffer[I].UV0;
		}
		Colors[I] = SailCol;
		const FVector Tx = FVector::CrossProduct(Nrm, FVector::UpVector).GetSafeNormal();
		Tangents[I] = FProcMeshTangent(Tx.IsNearlyZero() ? FVector(1.f, 0.f, 0.f) : Tx, false);
	}

	if (Bad > N / 4)
	{
		// Too many bad verts — push loft rest so the sail reappears
		for (int32 I = 0; I < N; ++I)
		{
			Vertices[I] = FlatRest.IsValidIndex(I) ? FlatRest[I] : Vertices[I];
		}
	}

	Mesh->UpdateMeshSection_LinearColor(SectionIndex, Vertices, MeshNormals, UV0, Colors, Tangents);
	// Ensure section stays renderable after dynamic updates
	if (FProcMeshSection* Updated = Mesh->GetProcMeshSection(SectionIndex))
	{
		Updated->bSectionVisible = true;
	}
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);
}

// ---- Heuristic fallback (non-grid meshes) ----

void FSailClothSim::DetectAndPinMain()
{
	float MinX = TNumericLimits<float>::Max(), MaxX = TNumericLimits<float>::Lowest();
	float MinZ = TNumericLimits<float>::Max(), MaxZ = TNumericLimits<float>::Lowest();
	for (const FVector& P : FlatRest)
	{
		MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X);
		MinZ = FMath::Min(MinZ, P.Z); MaxZ = FMath::Max(MaxZ, P.Z);
	}
	const float ZSpan = FMath::Max(1.f, MaxZ - MinZ);
	const float HeadBand = FMath::Max(14.f, ZSpan * 0.05f);
	const float FootBand = FMath::Max(12.f, ZSpan * 0.08f);
	const int32 NumU = 32;
	TArray<int32> BestInBucket;
	TArray<float> BestXInBucket;
	BestInBucket.Init(INDEX_NONE, NumU);
	BestXInBucket.Init(-TNumericLimits<float>::Max(), NumU);
	float BestClew = -TNumericLimits<float>::Max();
	for (int32 I = 0; I < FlatRest.Num(); ++I)
	{
		const FVector& P = FlatRest[I];
		const float U = FMath::Clamp((P.Z - MinZ) / ZSpan, 0.f, 1.f);
		const int32 Bucket = FMath::Clamp(int32(U * (NumU - 1)), 0, NumU - 1);
		if (P.X > BestXInBucket[Bucket])
		{
			BestXInBucket[Bucket] = P.X;
			BestInBucket[Bucket] = I;
		}
		if (P.Z >= MaxZ - HeadBand) bPinned[I] = 1;
		if (P.Z <= MinZ + FootBand)
		{
			const float Score = -P.X - P.Z * 0.02f;
			if (Score > BestClew) { BestClew = Score; ClewIndex = I; }
		}
	}
	for (int32 B = 0; B < NumU; ++B)
	{
		const int32 I = BestInBucket[B];
		if (I == INDEX_NONE) continue;
		bPinned[I] = 1;
		LuffIndices.Add(I);
		LuffU.Add(float(B) / float(NumU - 1));
	}
	if (LuffIndices.Num() > 0)
	{
		TackIndex = LuffIndices[0];
		HeadIndex = LuffIndices.Last();
		StayTackLocal = FlatRest[TackIndex];
		StayHeadLocal = FlatRest[HeadIndex];
		LuffLen = FVector::Dist(StayTackLocal, StayHeadLocal);
	}
	if (ClewIndex != INDEX_NONE) bPinned[ClewIndex] = 0;
	bStayValid = LuffIndices.Num() > 0;
}

void FSailClothSim::DetectAndPinJib(const FVector* OptionalStayTack, const FVector* OptionalStayHead)
{
	float MinX = TNumericLimits<float>::Max(), MaxX = TNumericLimits<float>::Lowest();
	float MinZ = TNumericLimits<float>::Max(), MaxZ = TNumericLimits<float>::Lowest();
	for (const FVector& P : FlatRest)
	{
		MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X);
		MinZ = FMath::Min(MinZ, P.Z); MaxZ = FMath::Max(MaxZ, P.Z);
	}
	if (OptionalStayTack && OptionalStayHead)
	{
		StayTackLocal = *OptionalStayTack;
		StayHeadLocal = *OptionalStayHead;
	}
	else
	{
		float BestTack = -TNumericLimits<float>::Max(), BestHead = -TNumericLimits<float>::Max();
		const float FootBand = FMath::Max(15.f, (MaxZ - MinZ) * 0.12f);
		const float MastBand = FMath::Max(20.f, (MaxX - MinX) * 0.15f);
		for (int32 I = 0; I < FlatRest.Num(); ++I)
		{
			const FVector& P = FlatRest[I];
			if (P.Z <= MinZ + FootBand)
			{
				const float Sc = P.X - P.Z * 0.05f;
				if (Sc > BestTack) { BestTack = Sc; TackIndex = I; StayTackLocal = P; }
			}
			if (P.X <= MinX + MastBand || P.Z >= MaxZ - (MaxZ - MinZ) * 0.12f)
			{
				const float Sc = P.Z - P.X * 0.15f;
				if (Sc > BestHead) { BestHead = Sc; HeadIndex = I; StayHeadLocal = P; }
			}
		}
	}
	const FVector StayAxis = StayHeadLocal - StayTackLocal;
	const float StayLen = StayAxis.Size();
	if (StayLen < 1.f) return;
	const FVector StayDir = StayAxis / StayLen;
	LuffLen = StayLen;
	bStayValid = true;
	const float LuffTol = FMath::Max(8.f, StayLen * 0.018f);
	const int32 NumU = 28;
	TArray<int32> BestInBucket;
	TArray<float> BestDist;
	BestInBucket.Init(INDEX_NONE, NumU);
	BestDist.Init(TNumericLimits<float>::Max(), NumU);
	float BestClew = -TNumericLimits<float>::Max();
	for (int32 I = 0; I < FlatRest.Num(); ++I)
	{
		const FVector& P = FlatRest[I];
		const float U = FMath::Clamp(FVector::DotProduct(P - StayTackLocal, StayDir) / StayLen, 0.f, 1.f);
		const FVector OnStay = StayTackLocal + StayDir * (U * StayLen);
		const float Dist = FVector::Dist(P, OnStay);
		const int32 Bucket = FMath::Clamp(int32(U * (NumU - 1)), 0, NumU - 1);
		if (Dist < BestDist[Bucket]) { BestDist[Bucket] = Dist; BestInBucket[Bucket] = I; }
		if (Dist > LuffTol * 2.f)
		{
			const float Score = Dist + FMath::Abs(P.Y) * 0.5f - P.Z * 0.02f;
			if (Score > BestClew) { BestClew = Score; ClewIndex = I; }
		}
	}
	for (int32 B = 0; B < NumU; ++B)
	{
		const int32 I = BestInBucket[B];
		if (I == INDEX_NONE || BestDist[B] > LuffTol * 2.5f) continue;
		const float U = float(B) / float(NumU - 1);
		const FVector OnStay = StayTackLocal + StayDir * (U * StayLen);
		bPinned[I] = 1;
		LuffIndices.Add(I);
		LuffU.Add(U);
		FlatRest[I] = OnStay;
		Pos[I] = Prev[I] = ShapeTarget[I] = OnStay;
	}
	if (ClewIndex != INDEX_NONE) bPinned[ClewIndex] = 0;
	if (LuffIndices.Num() > 0)
	{
		TackIndex = LuffIndices[0];
		HeadIndex = LuffIndices.Last();
	}
}

void FSailClothSim::CollectFootIndicesHeuristic()
{
	FootIndices.Reset();
	const int32 NumBuckets = 28;
	TArray<int32> Best; TArray<float> BestH;
	Best.Init(INDEX_NONE, NumBuckets);
	BestH.Init(TNumericLimits<float>::Max(), NumBuckets);
	for (int32 I = 0; I < FlatRest.Num(); ++I)
	{
		if (!HeightV.IsValidIndex(I) || !ChordU.IsValidIndex(I)) continue;
		if (HeightV[I] > 0.14f) continue;
		const int32 B = FMath::Clamp(int32(ChordU[I] * (NumBuckets - 1) + 0.5f), 0, NumBuckets - 1);
		if (HeightV[I] < BestH[B]) { BestH[B] = HeightV[I]; Best[B] = I; }
	}
	for (int32 B = 0; B < NumBuckets; ++B)
		if (Best[B] != INDEX_NONE) FootIndices.Add(Best[B]);
	if (TackIndex != INDEX_NONE) { FootIndices.Remove(TackIndex); FootIndices.Insert(TackIndex, 0); }
	if (ClewIndex != INDEX_NONE) { FootIndices.Remove(ClewIndex); FootIndices.Add(ClewIndex); }
}

void FSailClothSim::CollectLeechIndicesHeuristic()
{
	LeechIndices.Reset();
	const int32 NumBuckets = 40;
	TArray<int32> Best; TArray<float> BestU;
	Best.Init(INDEX_NONE, NumBuckets);
	BestU.Init(-1.f, NumBuckets);
	for (int32 I = 0; I < FlatRest.Num(); ++I)
	{
		if (!HeightV.IsValidIndex(I) || !ChordU.IsValidIndex(I)) continue;
		if (ChordU[I] < 0.72f) continue;
		const int32 B = FMath::Clamp(int32(HeightV[I] * (NumBuckets - 1) + 0.5f), 0, NumBuckets - 1);
		if (ChordU[I] > BestU[B]) { BestU[B] = ChordU[I]; Best[B] = I; }
	}
	for (int32 B = 0; B < NumBuckets; ++B)
		if (Best[B] != INDEX_NONE) LeechIndices.Add(Best[B]);
	if (ClewIndex != INDEX_NONE) { LeechIndices.Remove(ClewIndex); LeechIndices.Insert(ClewIndex, 0); }
}
