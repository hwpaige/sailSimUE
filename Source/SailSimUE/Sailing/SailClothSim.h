#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;

/** Which sail rig topology to pin (matches sail-sim web cloth). */
enum class ESailRigKind : uint8
{
	Main,
	Jib,
	/** Free-flying asymmetric: pin tack+head only; clew sheeted. */
	AsymSpin,
};

/**
 * Verlet sail cloth — faithful port of sail-sim web `SailCloth` (webgl-utils.sailing.js).
 *
 * Grid (sail_geom._flat_main_sail):
 *   i=0 row   foot (j=0 tack, j=nw-1 clew)
 *   i=nu-1    head
 *   j=0 col   luff
 *   j=nw-1    leech
 *
 * Units: mesh is UE cm. Aero uses the web's tuned kn*0.15 wind magnitude and
 * aeroK in feet, then converts displacements to cm (see Step).
 */
struct FSailClothSim
{
	bool bEnabled = true;
	bool bInitialized = false;
	int32 SectionIndex = 0;
	ESailRigKind RigKind = ESailRigKind::Main;

	int32 Nu = 0;
	int32 Nw = 0;
	bool bGridTopology = false;

	TArray<FVector> Pos;
	TArray<FVector> Prev;       // oldPos (Verlet)
	TArray<FVector> FlatRest;   // loft rest (cm)
	TArray<FVector> ShapeTarget; // unused by web step; kept for HUD/camber measure
	TArray<FVector> Normals;
	TArray<uint8> bPinned;      // hard anchors after constraint pass
	TArray<int32> TriIndices;
	/**
	 * Code Zero tri-radial panelling:
	 *  - VertColors: per-vert cloth tint (even/odd panel + corner patches)
	 *  - PanelId per quad centre drives even/odd mesh sections
	 *  - SeamPaths: grid-index polylines for stitched radial seams
	 */
	TArray<FLinearColor> VertColors;
	TArray<int32> TriIndicesEven;
	TArray<int32> TriIndicesOdd;
	/** Grid-index polylines for Code Zero seam ribbons (built once). */
	TArray<TArray<int32>> SeamPaths;
	/** Expected seam-section vertex count (2 per chain sample). */
	int32 SeamRibbonVertCount = 0;
	bool bCodeZeroPanels = false;

	enum class ESpringEdge : uint8 { Body, Foot, Leech, Luff };

	/**
	 * Grid spring role:
	 *   Inextensible — structural (row/col) + shear (quad diagonals) + perimeter.
	 *     Projected to exact rest length every step → cells cannot stretch or shear
	 *     into parallelograms, but the whole patch can still translate/billow.
	 *   Soft — bend (skip-one) + load-prop + battens. Allow curvature / camber.
	 */
	enum class ESpringRole : uint8 { Inextensible, Soft };

	struct FSpring
	{
		int32 A = 0;
		int32 B = 0;
		float RestLen = 0.f;      // cm
		float Stiffness = 1.f;
		float FlatRestLen = 0.f;  // cm
		ESpringEdge Edge = ESpringEdge::Body;
		ESpringRole Role = ESpringRole::Inextensible;
	};
	TArray<FSpring> Springs;

	/**
	 * Physical battens (J/105 class: four main battens whose pocket centres
	 * divide the leech into five equal parts; progressive length with a full
	 * top batten). Port of web SailCloth battens + bending beam.
	 */
	struct FBattenRow
	{
		int32 I = 0;       // height row (foot→head)
		int32 JStart = 0;  // forward (inset) column
		int32 JLeech = 0;  // leech column (nw-1)
		float FracV = 0.f; // height 0..1 foot→head
		float Inset = 0.f; // chord fraction of local row the batten spans
	};
	/** Chordwise distance ties (skip-3..5) — hold length along the beam. */
	struct FBattenDistTie
	{
		int32 A = 0;
		int32 B = 0;
		float RestLen = 0.f;
		float Stiffness = 0.f; // 0 until engageBattens
		float BattenW = 1.f;   // taper weight: soft forward, stiff aft
	};
	/**
	 * Beam curvature tie: holds mid-node B at a captured offset from midpoint(A,C)
	 * expressed in the batten local frame (rotation-invariant).
	 */
	struct FBattenBendTie
	{
		int32 A = 0, B = 0, C = 0;
		int32 Vu = 0, Vd = 0; // vertical neighbours for local normal
		float Ct = 0.f, Cu = 0.f, Cn = 0.f; // captured local-frame offset
		float CnOrig = 0.f;
		float Txr = 0.f, Tyr = 0.f, Tzr = 0.f; // world target this step
		float Stiffness = 0.f;
		int32 Sp = 1;          // node span (1 or 2)
		float BattenW = 1.f;
		bool bSnapped = false;
		float WnRefSign = 0.f;
		int32 RowStateIdx = INDEX_NONE;
	};
	struct FBattenRowState
	{
		int32 RowI = 0;
		bool bSnapped = false;
		int32 SnapHold = 0;
		float Sum = 0.f;
		int32 Cnt = 0;
	};

	TArray<FBattenRow> Battens;
	TArray<FBattenDistTie> BattenDist;
	TArray<FBattenBendTie> BattenBend;
	TArray<FBattenRowState> BattenRows;
	bool bBattensEngaged = false;
	int32 BattenSettleFrames = 0;
	/** Global batten distance stiffness (web main 0.88 / jib 0.80). */
	float BattenStiffness = 0.88f;
	/** Beam curvature stiffness scale (web _battenBendK). */
	float BattenBendK = 0.85f;
	int32 BattenBendIters = 6;

	TArray<int32> LuffIndices;
	TArray<float> LuffU;
	TArray<int32> FootIndices;
	TArray<int32> LeechIndices;
	TArray<int32> HeadRowIndices;
	TArray<float> ChordU;
	TArray<float> HeightV;

	float LoftFootLen = 0.f;
	float LoftLeechChord = 0.f;
	float LoftLeechPath = 0.f;
	float LoftFootPath = 0.f;

	int32 ClewIndex = INDEX_NONE;
	int32 TackIndex = INDEX_NONE;
	int32 HeadIndex = INDEX_NONE;

	/**
	 * Free verts near the clew that move with a boom/sheet snap (rigid
	 * translate — same delta as the clew). Built once at mesh init.
	 */
	TArray<int32> ClewPatchMain;
	TArray<int32> ClewPatchJib;

	FVector StayTackLocal = FVector::ZeroVector;
	FVector StayHeadLocal = FVector::ZeroVector;
	bool bStayValid = false;
	float FootRestLen = 0.f;
	float ChordLen = 100.f;
	float LuffLen = 100.f;
	float BoomLenCm = 0.f;

	// --- Web SailCloth opts (aero in feet-scale; see Step) ---
	/** Web aeroK: main 0.0036, jib 0.0022 (feet). */
	float AeroK = 0.0036f;
	/** Web dragK default 0.00035. */
	float DragK = 0.00035f;
	/** Web damping 0.982. */
	float Damping = 0.982f;
	/** Web relax 0.72 — soft (bend) constraint fraction only. */
	float Relax = 0.72f;
	/** Web preTen 0.998. */
	float PreTen = 0.998f;
	/** Web maxDelta 0.35 ft → cm. */
	float MaxDeltaCm = 0.35f * 30.48f;
	/** Web constraint iters: main 4, jib 5. */
	int32 ConstraintIters = 4;
	/** Hard structural+shear passes per constraint iter (with clew held). */
	int32 InextensiblePasses = 2;
	/** Final hard passes after boom/foot (keep small — 2–4 is enough). */
	int32 InextensibleFinalPasses = 3;
	/** Legacy stretch cap (unused by main path; kept for SyncSpringRests). */
	float MaxStrain = 1.03f;
	float EdgeMaxStrain = 1.02f;

	// Trim (runtime)
	float Outhaul01 = 0.94f;
	float Vang01 = 0.40f;
	float LuffTension01 = 0.35f;
	float LeechTension01 = 0.15f;
	float ForestaySag = 0.02f;
	float SheetPullScale = 0.95f;

	// Measure / HUD
	float MaxCamberFrac = 0.14f;
	float DraftStation = 0.40f;
	float LuffCosStart = 0.10f;
	float PowerCosFull = 0.38f;
	float LastFillQuality = 0.85f;
	float LastCamberCm = 0.f;
	float LastStretchRatio = 1.f;
	float LastCamberRatio = 0.f;
	float LastLuffAmount = 0.f;
	float LastPowered01 = 1.f;
	float LastMeanCosInc = 0.5f;
	float LastWindPushCm = 0.f;
	float LastWindSpeedKn = 0.f;
	float LiveFootPathCm = 0.f;
	float LiveLeechPathCm = 0.f;
	float LiveFootChordCm = 0.f;
	float LiveLeechChordCm = 0.f;

	// Compatibility stubs (UI / old call sites)
	float StructuralStiffness = 0.72f;
	float WindPressureCm = 0.f;
	float WindStreamFrac = 0.f;
	float MaxAeroDeltaCm = 0.f;
	float MaxSpeedCm = 0.f;
	float ShapeAttract = 0.f;
	float ShapeBlend = 0.f;
	float WindResidualFrac = 0.f;
	float FlutterAmp = 0.f;
	float MaxOffsetFromLoftCm = 200.f;
	float GravityCm = -980.f;

	bool BuildFromMesh(
		UProceduralMeshComponent* Mesh,
		int32 Section,
		ESailRigKind Kind,
		const FVector* OptionalStayTackLocal = nullptr,
		const FVector* OptionalStayHeadLocal = nullptr);

	/**
	 * Build cloth + procedural section from tack/head/clew corners.
	 * For AsymSpin: Code Zero loft (SMG≈70% SF, straight luff, flat camber)
	 * rather than a pure bilinear triangle. Units: sail-local cm.
	 */
	bool BuildFromFlatTriangle(
		UProceduralMeshComponent* Mesh,
		int32 Section,
		ESailRigKind Kind,
		const FVector& TackLocal,
		const FVector& HeadLocal,
		const FVector& ClewLocal,
		int32 InNu = 28,
		int32 InNw = 16);

	void Clear();
	void SnapRigToStay();
	/** Move pinned tack/head targets (spin tack follows bowsprit tip). */
	void SetStayEndpoints(const FVector& TackLocal, const FVector& HeadLocal);

	float OuthaulFrac() const { return 0.70f + 0.30f * FMath::Clamp(Outhaul01, 0.f, 1.f); }
	float FootBoomPull() const
	{
		const float T = FMath::Clamp(Outhaul01, 0.f, 1.f);
		return FMath::Clamp((T - 0.80f) / 0.20f, 0.f, 1.f);
	}

	void Step(
		float Dt,
		const FVector& WindLocalDir,
		float WindSpeedKn,
		const FVector& ClewTargetLocal,
		float SheetEase01,
		const FVector& SheetLeadPortLocal = FVector::ZeroVector,
		const FVector& SheetLeadStbdLocal = FVector::ZeroVector,
		bool bLeeToStarboard = true);

	void PushToMesh(UProceduralMeshComponent* Mesh) const;
	/**
	 * Roller-furling morph for the jib: wraps free cloth around the forestay as
	 * Set01 goes 1→0 (douse). Luff stays on the foil; leech piles on the outside.
	 * Call after Step(), before PushToMesh(). Returns total wrap angle (radians)
	 * for driving a furler drum visual (0 = fully set).
	 */
	float ApplyRollerFurl(float Set01);
	/** After unfurl finishes, reset free verts to loft so cloth Step doesn't pop. */
	void SeedOpenFromLoft();
	/**
	 * Asym / Code Zero: if the kite is on the windward side, mirror free verts
	 * across the centreline so it always fills to leeward.
	 * LeeYSign: +1 = lee to starboard, −1 = lee to port.
	 */
	void ForceAsymLeeSide(float LeeYSign);
	void MeasureShape();
	float GetForceScale() const;
	float GetLuffAmount() const { return LastLuffAmount; }

	int32 GridIdx(int32 I, int32 J) const { return I * Nw + J; }
	/** Sample cloth position at sail UV (u luff→leech, v foot→head). */
	FVector SampleAtUV(float U, float V) const;
	FVector SampleNormalAtUV(float U, float V) const;

private:
	/** Build tri-radial panel IDs, even/odd tris, and seam polylines (AsymSpin). */
	void BuildCodeZeroPanelLayout();
	/** Write even/odd body sections + seam ribbon section. */
	void PushCodeZeroMesh(UProceduralMeshComponent* Mesh) const;
	bool TryInitGridTopology(int32 NumVerts);
	void PinGridTopology(const FVector* OptionalStayTack, const FVector* OptionalStayHead);
	void BuildGridSprings();
	/** Build inactive batten distance + bend ties (J/105 layout). */
	void BuildBattens();
	/** Capture settled camber as rest shape and enable battens. */
	void EngageBattens();
	/** Distance + beam-curvature solve (call after soft springs, engaged only). */
	void SolveBattens(const FVector& WindLocalDir, float WindSpeedKn);
	void CollectGridEdges();
	void ComputeUVParams();
	void ApplyTrimTensions();
	void ComputeNormals();
	float EvaluateIncidenceAndLuff(const FVector& WindVelUnit, float SheetEase01);
	void ReassertLuffOnStay(const FVector& WindLocalDir);
	void BuildClewPatches();
	/** Rigid-translate free clew-neighborhood verts by Delta (no falloff). */
	void TranslateClewPatch(const FVector& Delta, bool bJibLeechStyle);
	void ApplyMainBoomConstraints(const FVector& BoomTipLocal);
	void ApplyClewJib(
		float SheetEase01,
		const FVector& LeadPort,
		const FVector& LeadStbd,
		bool bLeeToStarboard);
	/** Soft springs only (bend / load-prop). */
	void SolveSoftSprings();
	/** One Gauss–Seidel pass: exact rest on structural + shear (pin-aware). */
	void SolveInextensibleGrid();
	/** Pin clew+tack, run Passes hard projections, restore pins. */
	void EnforceInextensibleWithClewHeld(int32 Passes);
	void MeasureEdgeLengths();
	void RebuildCamberTarget(bool bLee, float Ease, float WindKn, const FVector& BoomTip);
	void SyncSpringRests(float Powered01);
	void EnforcePerimeterEdges();

	void DetectAndPinMain();
	void DetectAndPinJib(const FVector* OptionalStayTack, const FVector* OptionalStayHead);
	void CollectFootIndicesHeuristic();
	void CollectLeechIndicesHeuristic();
};
