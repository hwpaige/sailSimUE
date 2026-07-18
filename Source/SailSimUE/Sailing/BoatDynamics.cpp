#include "Sailing/BoatDynamics.h"
#include "SailSimUE.h"
#include "Math/UnrealMathUtility.h"

void FBoatDynamics::InitJ105()
{
	ApplySailingParams(
		7750.f, 3340.f, 11.f, 29.5f, 6.5f, 1.2f,
		70.f, 50.f, 4.8f, 6.f,
		-1.f, -3.f, 545.f, 7.f, 4.6f,
		34.4f, 44.f);
	// Physics kn such that display ≈ 12 kn (25 phys ≈ 8 display feel).
	TrueWindSpeedKn = FBoatDynamics::WindPhysicsFromDisplay(12.f);
	TrueWindDirDeg = 225.f;
}

void FBoatDynamics::ApplySailingParams(
	float InDispLb, float InBallastLb, float InBeamFt, float InLwlFt, float InDraftFt, float InTcFt,
	float InLatArea, float InKeelArea, float InRudArea, float InKeelSpan,
	float InClrX, float InClrZ, float InSaTotal, float InHullSpeedKn, float InGmFt,
	float InLoaFt, float InMastTopFt)
{
	Disp = InDispLb > 0.f ? InDispLb : Disp;
	Ballast = InBallastLb;
	Beam = InBeamFt > 0.f ? InBeamFt : Beam;
	LWL = InLwlFt > 0.f ? InLwlFt : LWL;
	Draft = InDraftFt > 0.f ? InDraftFt : Draft;
	Tc = InTcFt > 0.f ? InTcFt : Tc;
	LateralArea = InLatArea > 0.f ? InLatArea : LateralArea;
	KeelArea = InKeelArea > 0.f ? InKeelArea : KeelArea;
	RudArea = InRudArea > 0.f ? InRudArea : RudArea;
	KeelSpan = InKeelSpan > 0.f ? InKeelSpan : KeelSpan;
	XCLR = InClrX;
	ClrDepth = FMath::Abs(InClrZ) > 0.1f ? FMath::Abs(InClrZ) : ClrDepth;
	SATotal = InSaTotal > 0.f ? InSaTotal : SATotal;
	HullSpeedKn = InHullSpeedKn > 0.f ? InHullSpeedKn : HullSpeedKn;
	GM = InGmFt > 0.f ? InGmFt : GM;
	if (GM > 5.5f) GM = FMath::Clamp(GM * 0.25f + 3.5f, 4.0f, 5.0f);
	else if (GM < 3.5f) GM = 4.6f;
	LOA = InLoaFt > 0.f ? InLoaFt : LOA;
	CoeRef = InMastTopFt > 0.f ? 0.40f * InMastTopFt : CoeRef;
	// J/105-ish rudder span if not in JSON
	RudSpan = FMath::Max(3.5f, KeelSpan * 0.65f);
	XRud = -0.42f * LOA;
	SailLead = -3.0f;
	CrS = 0.71f;
	CFMax = 1.66f;

	Mass = Disp / G;
	Iz = Mass * FMath::Square(KzFrac * LOA);
	Mu = Mass * (1.f + Xudot);
	Mv = Mass * (1.f + Yvdot);
	Mr = Iz * (1.f + Nrdot);
	ARKeel = 2.f * KeelSpan * KeelSpan / FMath::Max(KeelArea, 1e-3f);
	AKeel = 2.f * PI * ARKeel / (ARKeel + 2.f);
	ARRud = 2.f * RudSpan * RudSpan / FMath::Max(RudArea, 1e-3f);
	ARud = 2.f * PI * ARRud / (ARRud + 2.f);

	Reset();
	Heading = 90.f;
	AutoTarget = Heading;
}

void FBoatDynamics::Reset()
{
	V = Beta = Phi = YawRate = 0.f;
	PhiRate = 0.f;
	U = Vsway = R = 0.f;
	Rudder = 0.f;
	ManualRudderTarget = 0.f;
	bManualHelm = false;
	bAutoHeading = true;
	AutoMode = EAutoMode::Hdg;
	AutoAwaTarget = 45.f;
	bAutoTrim = false;
	bTacking = false;
	TackTarget = 0.f;
	TackDir = 0;
	NavWpIndex = 0;
	AutoI = 0.f;
	bLpInit = false;
	bLpNsailInit = false;
	LpDrive = LpSide = LpNsail = NsailYaw = 0.f;
	HeelTargetLp = 0.f;
	bHeelTargetLpInit = false;
	LeeSign = 1;
}

void FBoatDynamics::EngageAutopilot(bool bCaptureLive)
{
	bAutoHeading = true;
	bManualHelm = false;
	if (bCaptureLive)
	{
		if (AutoMode == EAutoMode::Awa)
		{
			AutoAwaTarget = GetApparentWindAngleDeg();
		}
		else if (AutoMode == EAutoMode::Nav)
		{
			// NAV bearing is updated externally; keep AutoTarget as-is.
		}
		else
		{
			AutoTarget = Heading;
		}
	}
	Rudder = 0.f;
	ManualRudderTarget = 0.f;
	AutoI = 0.f;
	bLpNsailInit = false;
}

void FBoatDynamics::DisengageAutopilot()
{
	bAutoHeading = false;
	bManualHelm = false;
	bLpNsailInit = false;
	ManualRudderTarget = Rudder;
}

void FBoatDynamics::SetAutoMode(EAutoMode Mode)
{
	if (AutoMode == Mode) return;
	AutoMode = Mode;
	AutoI = 0.f;
	if (bAutoHeading)
	{
		if (Mode == EAutoMode::Awa)
		{
			AutoAwaTarget = GetApparentWindAngleDeg();
		}
		else if (Mode == EAutoMode::Nav)
		{
			// external route acquire
		}
		else
		{
			AutoTarget = Heading;
		}
	}
}

void FBoatDynamics::SetAutoAwaTarget(float SignedDeg)
{
	AutoMode = EAutoMode::Awa;
	AutoAwaTarget = FMath::Clamp(SignedDeg, -170.f, 170.f);
	AutoI = 0.f;
	if (!bAutoHeading)
	{
		EngageAutopilot(false);
	}
}

void FBoatDynamics::SetAutoTrim(bool bOn)
{
	bAutoTrim = bOn;
}

void FBoatDynamics::StartTack()
{
	if (!bSailing || bTacking) return;
	// Mirror heading about true-wind axis (web startTack).
	const float Tgt = Wrap360(2.f * TrueWindDirDeg - Heading);
	const float D = Wrap180(Tgt - Heading);
	TackTarget = Tgt;
	TackDir = (D >= 0.f) ? 1 : -1;
	bTacking = true;
	bAutoHeading = false;
	bManualHelm = false;
}

void FBoatDynamics::UpdateAutoTrim(float Dt)
{
	if (!bAutoTrim || !bSailing) return;
	Dt = FMath::Clamp(Dt, 0.f, 0.05f);
	const float AwaAbs = FMath::Abs(GetApparentWindAngleDeg());
	// web H_AUTO_TRIM_AOA = 16 → boom ≈ AWA − 16
	const float OptMain = FMath::Clamp(AwaAbs - 16.f, 4.f, 85.f);
	// sheetT = (boom/85)^1.35 ; SheetEase matches web sheetT (0 hard in, 1 eased)
	const float TgtEase = FMath::Pow(OptMain / 85.f, 1.35f);
	const float Step = 1.35f * Dt; // H_AUTO_TRIM_RATE
	const float De = TgtEase - SheetEase;
	if (FMath::Abs(De) <= Step) SheetEase = TgtEase;
	else SheetEase += (De > 0.f ? Step : -Step);
	SheetEase = FMath::Clamp(SheetEase, 0.f, 1.f);
}

void FBoatDynamics::SetRudderStarboardPositive(float Deg)
{
	// UI +starboard → model needs negate (web setRudder)
	ManualRudderTarget = -FMath::Clamp(Deg, -35.f, 35.f);
	if (FMath::Abs(Deg) > 0.5f && bAutoHeading)
	{
		bAutoHeading = false;
		bManualHelm = true;
	}
}

void FBoatDynamics::SetSheetEase(float Ease01)
{
	SheetEase = FMath::Clamp(Ease01, 0.f, 1.f);
}

void FBoatDynamics::SetOuthaul(float V01)
{
	Outhaul01 = FMath::Clamp(V01, 0.f, 1.f);
}

void FBoatDynamics::SetVang(float V01)
{
	Vang01 = FMath::Clamp(V01, 0.f, 1.f);
}

void FBoatDynamics::SetJibCar(float V01)
{
	JibCar01 = FMath::Clamp(V01, 0.f, 1.f);
}

void FBoatDynamics::SetJibLuffTension(float V01)
{
	JibLuffTension01 = FMath::Clamp(V01, 0.f, 1.f);
}

void FBoatDynamics::SetJibLeechTension(float V01)
{
	JibLeechTension01 = FMath::Clamp(V01, 0.f, 1.f);
}

float FBoatDynamics::Wrap180(float Deg)
{
	float D = FMath::Fmod(Deg + 540.f, 360.f) - 180.f;
	return D;
}

float FBoatDynamics::Wrap360(float Deg)
{
	float D = FMath::Fmod(Deg, 360.f);
	if (D < 0.f) D += 360.f;
	return D;
}

float FBoatDynamics::HeelWindSideAtten(float TwsKn) const
{
	if (TwsKn <= 13.5f) return 1.f;
	if (TwsKn >= 25.f) return 0.43f;
	return 1.f - 0.57f * ((TwsKn - 13.5f) / 11.5f);
}

float FBoatDynamics::HeelWindGmBoost(float TwsKn) const
{
	if (TwsKn <= 12.f) return 1.f;
	if (TwsKn >= 22.f) return 1.35f;
	return 1.f + 0.35f * ((TwsKn - 12.f) / 10.f);
}

float FBoatDynamics::HeelWindMaxDeg(float TwsKn) const
{
	if (TwsKn <= 12.f) return 32.f;
	if (TwsKn >= 24.f) return 26.f;
	return 32.f - 6.f * ((TwsKn - 12.f) / 12.f);
}

float FBoatDynamics::RightingMoment(float PhiDeg) const
{
	const float P = FMath::DegreesToRadians(PhiDeg);
	return Disp * GM * StabCal * FMath::Sin(P) + CrewHikeFtLb * FMath::Cos(P);
}

float FBoatDynamics::HeelingArm() const
{
	const float Arm = CoeRef + ClrDepth;
	return FMath::Clamp(Arm, 17.f, 26.f);
}

float FBoatDynamics::HeelEquilibrium(float SideLb, float TwsKn) const
{
	float Side = FMath::Abs(SideLb);
	if (Side < 2.f) return 0.f;
	Side *= HeelWindSideAtten(TwsKn);
	const float Arm = HeelingArm();
	const float Gm = GM * StabCal * HeelGmScale * HeelWindGmBoost(TwsKn);
	if (Gm < 0.5f) return 0.f;
	float PhiRad = 0.01f;
	for (int32 I = 0; I < 10; ++I)
	{
		const float C = FMath::Cos(PhiRad);
		const float S = FMath::Sin(PhiRad);
		const float Hm = Side * Arm * C;
		const float Rm = Disp * Gm * S + CrewHikeFtLb * C;
		const float F = Hm - Rm;
		const float Df = -Side * Arm * S - Disp * Gm * C - CrewHikeFtLb * S;
		if (FMath::Abs(Df) < 1e-4f) break;
		PhiRad = FMath::Clamp(PhiRad - F / Df, 0.f, 0.96f);
	}
	return FMath::Min(HeelWindMaxDeg(TwsKn), FMath::RadiansToDegrees(PhiRad));
}

FBoatDynamics::FFoilForce FBoatDynamics::FoilForce(float Fx, float Fy, float Area, float ASlope, float AR, float MountRad) const
{
	const float V2 = Fx * Fx + Fy * Fy;
	const float Sp = FMath::Sqrt(V2);
	if (Sp < 1e-4f) return {};
	const float Inflow = FMath::Atan2(Fy, Fx);
	float Aoa = Inflow - (PI + MountRad);
	while (Aoa > PI) Aoa -= 2.f * PI;
	while (Aoa < -PI) Aoa += 2.f * PI;
	const float Stall = FMath::DegreesToRadians(22.f);
	const float AEff = FMath::Clamp(Aoa, -Stall, Stall);
	const float CL = ASlope * AEff;
	const float CD = 0.010f + (CL * CL) / (PI * AR * 0.85f);
	const float Q = 0.5f * RhoWater * V2;
	const float L = Q * Area * FMath::Abs(CL);
	const float D = Q * Area * CD;
	const float Ix = Fx / Sp;
	const float Iy = Fy / Sp;
	const float Sgn = (Aoa >= 0.f) ? 1.f : -1.f;
	const float Lx = Sgn * (-Iy);
	const float Ly = Sgn * (Ix);
	return { D * Ix + L * Lx, D * Iy + L * Ly };
}

void FBoatDynamics::UpdateLeeSign()
{
	// Web hLeeSignUpdate: +1 = leeward PORT (wind FROM starboard / stbd tack).
	// Wider hysteresis than web's 1.5° — stops the heel target flipping back and
	// forth when the bow is near the wind (felt as a sudden upright → re-heel snap).
	const float Rel = Wrap180(TrueWindDirDeg - Heading);
	const float Hyst = 6.f;
	if (LeeSign == 0)
	{
		LeeSign = Rel >= 0.f ? 1 : -1;
	}
	else if (Rel * static_cast<float>(LeeSign) < 0.f
		&& FMath::Abs(Rel) > Hyst
		&& FMath::Abs(Rel) < 180.f - Hyst)
	{
		LeeSign = Rel > 0.f ? 1 : -1;
	}
}

float FBoatDynamics::GetTrueWindFromRelDeg() const
{
	return Wrap180(TrueWindDirDeg - Heading);
}

void FBoatDynamics::GetApparentWindAirVelUnit(float& OutDx, float& OutDz) const
{
	// Web hTrueWindDir / hApparentWind: air velocity is DOWNWIND (TO), not FROM.
	const float FromRad = FMath::DegreesToRadians(TrueWindDirDeg - Heading);
	const float Twx = -TrueWindSpeedKn * FMath::Cos(FromRad); // knots, boat +X fwd
	const float Twz = -TrueWindSpeedKn * FMath::Sin(FromRad); // knots, boat +Z stbd
	const float Bx = U / KnToFts;
	const float Bz = Vsway / KnToFts;
	float Ax = Twx - Bx;
	float Az = Twz - Bz;
	const float Sp = FMath::Sqrt(Ax * Ax + Az * Az);
	if (Sp < 1e-5f)
	{
		OutDx = -1.f;
		OutDz = 0.f;
		return;
	}
	OutDx = Ax / Sp;
	OutDz = Az / Sp;
}

float FBoatDynamics::GetApparentWindAngleDeg() const
{
	// Signed AWA = FROM angle of apparent wind (+ = from starboard).
	// Air vel unit is downwind; FROM = opposite of air velocity.
	float Dx, Dz;
	GetApparentWindAirVelUnit(Dx, Dz);
	return FMath::RadiansToDegrees(FMath::Atan2(-Dz, -Dx));
}

float FBoatDynamics::GetApparentWindSpeedKn() const
{
	const float FromRad = FMath::DegreesToRadians(TrueWindDirDeg - Heading);
	const float Twx = -TrueWindSpeedKn * FMath::Cos(FromRad);
	const float Twz = -TrueWindSpeedKn * FMath::Sin(FromRad);
	const float Bx = U / KnToFts;
	const float Bz = Vsway / KnToFts;
	const float Ax = Twx - Bx;
	const float Az = Twz - Bz;
	return FMath::Sqrt(Ax * Ax + Az * Az);
}

FSailForceInput FBoatDynamics::ComputeSailForceStub() const
{
	// Lift+drag style force in boat axes, side directed to LEEWARD (web hMeasureSailForce).
	// Continuous vs AWA — no hard |AWA|<22° zero. That cliff made weather helm
	// snap on as the boat exited irons and yaw the bow back upwind.
	const float Aws = GetApparentWindSpeedKn();
	const float Awa = GetApparentWindAngleDeg(); // signed FROM
	const float AwaAbs = FMath::Abs(Awa);        // 0..180 off bow for CL

	// Ideal sheet ease vs AWA (point of sail)
	float IdealEase = 0.12f;
	if (AwaAbs < 40.f) IdealEase = 0.08f;
	else if (AwaAbs < 70.f) IdealEase = 0.12f + 0.25f * ((AwaAbs - 40.f) / 30.f);
	else if (AwaAbs < 120.f) IdealEase = 0.37f + 0.35f * ((AwaAbs - 70.f) / 50.f);
	else IdealEase = 0.72f + 0.25f * FMath::Clamp((AwaAbs - 120.f) / 60.f, 0.f, 1.f);
	const float SheetErr = FMath::Abs(SheetEase - IdealEase);
	const float SheetEff = FMath::Clamp(1.f - 1.6f * SheetErr, 0.25f, 1.f);

	// Web fill×taper: CL builds from ~9.4° (not a step at 22°). Head-to-wind → Fill=0.
	const float Fill = FMath::Clamp((AwaAbs - 9.4f) / 18.2f, 0.f, 1.f);
	const float Taper = FMath::Clamp((175.f - AwaAbs) / 75.f, 0.f, 1.f);
	// Slot boost only once the sail is actually filling — avoids max weather-helm
	// leverage on the first few degrees of recovery from irons.
	const float SlotRaw = (AwaAbs < 29.f) ? (1.f + 0.40f * (29.f - AwaAbs) / 29.f) : 1.f;
	const float UpwindSlot = FMath::Lerp(1.f, SlotRaw, Fill);
	float Cl = 0.f;
	if (AwaAbs < 165.f && Fill > 0.f && Taper > 0.f)
	{
		Cl = CFMax * Fill * Taper * UpwindSlot * SheetEff
			* FMath::Clamp(ClothForceScale, 0.3f, 1.25f);
	}

	const float Va = Aws * KnToFts;
	const float Q = 0.5f * RhoAir * Va * Va;
	// CD rises off the wind (running); mild luff drag near irons (no hard replace)
	const float SAwa = FMath::Square(FMath::Sin(FMath::DegreesToRadians(AwaAbs * 0.5f)));
	float Cd = 0.116f + (0.80f - 0.116f) * SAwa;
	// Deep in irons (Fill≈0): keep a little reverse drag so she still washes off
	// without a discontinuous force rewrite at 22°.
	const float IronsBlend = 1.f - Fill; // 1 head-to-wind → 0 once filled
	if (IronsBlend > 0.f)
	{
		// Reduce parasitic run-drag while luffing; small aft force remains via Whx
		Cd = FMath::Lerp(Cd, 0.08f, IronsBlend);
	}

	const float Lift = Q * SATotal * Cl;
	// When still luffing, add a small continuous reverse drive (was the old 0.02·Q·SA
	// hard override). Scales out as Fill rises so no step at the old 22° gate.
	const float LuffDragLb = IronsBlend * IronsBlend * 0.02f * Q * SATotal;
	const float Drag = Q * SATotal * Cd;

	// Air-velocity unit (downwind) in body frame
	float Whx, Whz;
	GetApparentWindAirVelUnit(Whx, Whz);
	// Leeward perpendicular to downwind (web: lefx=-whz, lefz=whx), then × LeeSign
	// LeeSign +1 (lee=port): leeward force has negative Z component when lift dominates upwind.
	const float Lefx = -Whz;
	const float Lefz = Whx;
	const float Ls = static_cast<float>(LeeSign); // must be updated before this is called

	FSailForceInput Out;
	// Drive/side = lift toward lee + drag downwind − soft luff reverse
	Out.DriveLb = Lift * Lefx * Ls + Drag * Whx - LuffDragLb;
	// Side from lift + drag; no hard zero — ramps with Fill via Cl
	Out.SideLb = Lift * Lefz * Ls + Drag * Whz;
	// Soft-kill residual side very near head-to-wind (numerical noise only)
	if (AwaAbs < 6.f)
	{
		const float SideGate = FMath::Square(AwaAbs / 6.f);
		Out.SideLb *= SideGate;
	}
	return Out;
}

void FBoatDynamics::UpdateHelm(float Dt)
{
	// Web split: updateHelm drives AP/tack → rudder; manual helm slews from
	// ManualRudderTarget whenever the pilot is off (hApplyBoatAttitude).
	float Want = 0.f;
	bool bHaveWant = false;

	// TACK / GYBE: full helm until bow past wind, then hand back to AP.
	if (bTacking)
	{
		Want = -35.f * static_cast<float>(TackDir);
		bHaveWant = true;
		const float Terr = Wrap180(TackTarget - Heading);
		if (FMath::Abs(Terr) < 8.f || (Terr * static_cast<float>(TackDir)) < 0.f)
		{
			bTacking = false;
			bAutoHeading = true;
			bManualHelm = false;
			AutoI = 0.f;
			if (AutoMode == EAutoMode::Awa)
			{
				AutoAwaTarget = -AutoAwaTarget; // mirror AWA onto new tack
			}
			else if (AutoMode == EAutoMode::Nav)
			{
				// keep AutoTarget as external bearing
			}
			else
			{
				AutoTarget = TackTarget;
			}
		}
	}
	else if (bAutoHeading)
	{
		float Err = 0.f;
		if (AutoMode == EAutoMode::Awa)
		{
			Err = Wrap180(AutoAwaTarget - GetApparentWindAngleDeg());
		}
		else
		{
			// hdg + nav both track AutoTarget as compass heading
			Err = Wrap180(AutoTarget - Heading);
		}
		const float ErrP = FMath::Abs(Err) < 2.f ? 0.f : Err;
		if (FMath::Abs(Err) < 2.f) AutoI *= 0.90f;
		else AutoI += Err * Dt;
		AutoI = FMath::Clamp(AutoI, -6.f, 6.f);
		const float YawDegS = R * 180.f / PI;
		Want = FMath::Clamp(-(KpAuto * ErrP + KiAuto * AutoI) + KdAuto * YawDegS, -35.f, 35.f);
		bHaveWant = true;
	}
	else
	{
		// Manual sticky tiller: track user target (held angle). Slew for feel only.
		Want = ManualRudderTarget;
		bHaveWant = true;
		bManualHelm = FMath::Abs(ManualRudderTarget) > 0.5f;
	}

	if (!bHaveWant) return;
	const float MaxStep = RudSlewRate * Dt;
	const float Rerr = Want - Rudder;
	if (FMath::Abs(Rerr) <= MaxStep) Rudder = Want;
	else Rudder += (Rerr > 0.f ? MaxStep : -MaxStep);
}

void FBoatDynamics::PhysStep(float H, float Xsail, float Ysail)
{
	const float u = U;
	const float v = Vsway;
	const float r = R;
	const FFoilForce Kf = FoilForce(-u, -v, LateralArea, AKeel, ARKeel, 0.f);
	const float Nkeel = Kf.Fy * XCLR;
	const float VRud = v + r * XRud;
	const FFoilForce Rf = FoilForce(-u, -VRud, RudArea, ARud, ARRud, FMath::DegreesToRadians(Rudder));
	const float Nrud = Rf.Fy * XRud;
	const float Nsail = NsailYaw;
	const float Vh = HullSpeedKn * KnToFts;
	const float Rr = FMath::Abs(u) / FMath::Max(Vh, 1e-3f);
	const float Rhull = 0.5f * RhoWater * CrS * u * FMath::Abs(u) * (1.f + 1.5f * FMath::Pow(Rr, 8.f));
	const float Ycross = -0.5f * RhoWater * CdCrossY * LWL * v * FMath::Abs(v);
	const float Ncross = -0.5f * RhoWater * CdCrossN * FMath::Pow(LWL, 3.f) / 12.f * r * FMath::Abs(r);
	const float X = Xsail + Kf.Fx + Rf.Fx - Rhull;
	const float Y = Ysail + Kf.Fy + Rf.Fy + Ycross;
	const float N = Nkeel + Nrud + Ncross + Nsail;
	U += (X + Mv * v * r) / Mu * H;
	Vsway += (Y - Mu * u * r) / Mv * H;
	R += (N / Mr) * H;
}

void FBoatDynamics::Update(float Dt)
{
	if (!bSailing)
	{
		return;
	}
	Dt = (Dt > 0.f) ? FMath::Min(0.05f, Dt) : (1.f / 60.f);
	UpdateLeeSign();
	UpdateAutoTrim(Dt);

	const FSailForceInput Sf = ComputeSailForceStub();
	const bool bSlow = V < 3.5f * KnToFts;
	const float ADrive = bAutoHeading ? (bSlow ? 0.18f : 0.12f) : 0.22f;
	const float ASide = bAutoHeading ? (bSlow ? 0.22f : 0.18f) : 0.24f;
	if (!bLpInit)
	{
		LpDrive = Sf.DriveLb;
		LpSide = Sf.SideLb;
		bLpInit = true;
	}
	LpDrive += ADrive * (Sf.DriveLb - LpDrive);
	LpSide += ASide * (Sf.SideLb - LpSide);

	// Heel reduces projected sail force. Drive uses cos(φ); side force also drops
	// gently (√cos) so weather helm does not overwhelm the rudder when hard-pressed
	// — real sails lose projected area as they heel, and the helm stays usable.
	const float PhiR = FMath::DegreesToRadians(Phi);
	const float Cphi = FMath::Cos(PhiR);
	const float CphiSide = FMath::Sqrt(FMath::Clamp(Cphi, 0.30f, 1.f));
	const float Xsail = LpDrive * Cphi;
	const float Ysail = LpSide * CphiSide;
	// Weather-helm lever ramps with fill-out of the no-go (smoothstep ~10–32° AWA).
	// Prevents a full CoE moment on the first trickle of side force while the
	// boat is still nearly stopped exiting irons (rudder ~V², sails ~AWS²).
	const float AwaAbsHelm = FMath::Abs(GetApparentWindAngleDeg());
	const float HelmT = FMath::Clamp((AwaAbsHelm - 10.f) / 22.f, 0.f, 1.f);
	const float HelmArmScale = HelmT * HelmT * (3.f - 2.f * HelmT); // smoothstep
	const float NsailRaw = Ysail * (XCLR + SailLead) * HelmArmScale;
	if (bAutoHeading)
	{
		if (!bLpNsailInit)
		{
			LpNsail = NsailRaw;
			bLpNsailInit = true;
		}
		// Slightly slower weather-helm LP when slow — damps exit-from-irons kick
		const float ANs = bSlow ? 0.05f : 0.07f;
		LpNsail += ANs * (NsailRaw - LpNsail);
		NsailYaw = LpNsail;
	}
	else
	{
		// Manual helm: low-pass weather helm a little so it does not fight every
		// tiller twitch, but keep it present so the boat still wants to round up.
		if (!bLpNsailInit)
		{
			LpNsail = NsailRaw;
			bLpNsailInit = true;
		}
		const float ANs = bSlow ? 0.10f : 0.18f;
		LpNsail += ANs * (NsailRaw - LpNsail);
		NsailYaw = LpNsail;
	}

	UpdateHelm(Dt);
	constexpr int32 NSub = 4;
	const float H = Dt / NSub;
	for (int32 I = 0; I < NSub; ++I)
	{
		PhysStep(H, Xsail, Ysail);
		Heading = Wrap360(Heading + R * H * 180.f / PI);
	}

	V = FMath::Sqrt(U * U + Vsway * Vsway);
	if (V < 0.35f)
	{
		Beta = 0.f;
	}
	else
	{
		Beta = FMath::RadiansToDegrees(FMath::Atan2(Vsway, FMath::Max(U, 0.35f)));
		Beta = FMath::Clamp(Beta, -35.f, 35.f);
	}
	YawRate = R * 180.f / PI;

	// ---- Heel: second-order roll toward a low-passed equilibrium ----
	// Mag from |side|, sign from LeeSign (web: targetPhi = −leeSign × heelEq).
	// LeeSign +1 (lee=port) → Phi negative (port rail down) = leeward heel.
	const float TwsKn = TrueWindSpeedKn;
	const float PhiMax = HeelWindMaxDeg(TwsKn);
	const float TargetPhiRaw = -static_cast<float>(LeeSign)
		* HeelEquilibrium(LpSide * HeelSideGain, TwsKn);

	// Soft target: absorbs LeeSign flips and force spikes (~0.7 s rise).
	if (!bHeelTargetLpInit)
	{
		HeelTargetLp = TargetPhiRaw;
		bHeelTargetLpInit = true;
	}
	{
		const float ATgt = 1.f - FMath::Exp(-1.4f * Dt);
		HeelTargetLp += ATgt * (TargetPhiRaw - HeelTargetLp);
	}

	// Critically-ish damped second-order roll (keelboat inertia + form stability).
	// Natural frequency ~1.1 rad/s → ~5–6 s full rail-to-rail, not a snap.
	const float Omega = 1.15f; // rad/s
	const float Zeta = 0.92f;
	const float Accel = Omega * Omega * (HeelTargetLp - Phi) - 2.f * Zeta * Omega * PhiRate;
	PhiRate += Accel * Dt;
	// Hard rate cap (~keelboat max roll rate); softer than old 14°/s step limit.
	PhiRate = FMath::Clamp(PhiRate, -9.5f, 9.5f);
	Phi += PhiRate * Dt;
	// Soft wall at PhiMax: bleed rate instead of hard clamp snap
	if (Phi > PhiMax)
	{
		Phi = PhiMax;
		if (PhiRate > 0.f) PhiRate *= 0.25f;
	}
	else if (Phi < -PhiMax)
	{
		Phi = -PhiMax;
		if (PhiRate < 0.f) PhiRate *= 0.25f;
	}

	if (!FMath::IsFinite(U)) U = 0.f;
	if (!FMath::IsFinite(Vsway)) Vsway = 0.f;
	if (!FMath::IsFinite(R)) R = 0.f;
	if (!FMath::IsFinite(V)) V = 0.f;
	if (!FMath::IsFinite(Beta)) Beta = 0.f;
	if (!FMath::IsFinite(Phi)) Phi = 0.f;
	if (!FMath::IsFinite(PhiRate)) PhiRate = 0.f;
}

bool FBoatDynamics::RunGoldenSelfCheck(FString* OutReport)
{
	// Settle close-hauled-ish at TWS 12 kn (wind from 225°, head ~185° → AWA ~40°).
	FBoatDynamics D;
	D.InitJ105();
	D.TrueWindSpeedKn = 12.f;
	D.TrueWindDirDeg = 225.f;
	D.Heading = 185.f;
	D.AutoTarget = 185.f;
	D.bAutoHeading = true;
	D.bSailing = true;
	D.SheetEase = 0.12f;

	constexpr float Dt = 0.05f;
	for (int32 I = 0; I < 800; ++I)
	{
		D.Update(Dt);
	}

	const float Spd = D.GetSpeedKnots();
	const float HeelAbs = FMath::Abs(D.Phi);
	const float Awa = D.GetApparentWindAngleDeg();
	// Starboard tack (TWD 225, HDG 185): AWA > 0, LeeSign +1, heel to port (Phi < 0), side force to port (LpSide < 0).
	const bool bSpdOk = Spd >= 3.5f && Spd <= 9.5f;
	const bool bHeelOk = HeelAbs >= 5.f && HeelAbs <= 32.f;
	const bool bAwaOk = Awa > 15.f && Awa < 70.f;
	const bool bLeeOk = D.GetLeeSign() > 0 && D.Phi < 0.f && D.GetLpSideLb() < 0.f;
	const bool bOk = bSpdOk && bHeelOk && bAwaOk && bLeeOk
		&& FMath::IsFinite(Spd) && FMath::IsFinite(HeelAbs);

	const FString Report = FString::Printf(
		TEXT("VPP golden TWS12 HDG185: SPD=%.2f kn HEEL=%+.1f° AWA=%+.0f° Lee=%d Side=%.0f %s"),
		Spd, D.Phi, Awa, D.GetLeeSign(), D.GetLpSideLb(),
		bOk ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogSailSim, Log, TEXT("%s"), *Report);
	if (OutReport) *OutReport = Report;
	return bOk;
}
