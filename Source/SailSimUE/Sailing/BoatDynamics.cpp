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
	TrueWindSpeedKn = 18.f;
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
	U = Vsway = R = 0.f;
	Rudder = 0.f;
	ManualRudderTarget = 0.f;
	bManualHelm = false;
	bAutoHeading = true;
	AutoI = 0.f;
	bLpInit = false;
	bLpNsailInit = false;
	LpDrive = LpSide = LpNsail = NsailYaw = 0.f;
	LeeSign = 1;
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
	const float Rel = Wrap180(TrueWindDirDeg - Heading);
	const float Hyst = 1.5f;
	if (LeeSign == 0)
	{
		LeeSign = Rel >= 0.f ? 1 : -1;
	}
	else if (Rel * LeeSign < 0.f && FMath::Abs(Rel) > Hyst && FMath::Abs(Rel) < 180.f - Hyst)
	{
		LeeSign = Rel > 0.f ? 1 : -1;
	}
}

float FBoatDynamics::GetApparentWindAngleDeg() const
{
	// Boat axes knots: +X forward, +Z starboard (web)
	const float Bx = U / KnToFts;
	const float Bz = Vsway / KnToFts;
	const float Twx = TrueWindSpeedKn * FMath::Cos(FMath::DegreesToRadians(TrueWindDirDeg - Heading));
	const float Twz = TrueWindSpeedKn * FMath::Sin(FMath::DegreesToRadians(TrueWindDirDeg - Heading));
	// Wind relative to boat (from direction → negate boat vel in air frame simplified)
	const float Ax = Twx - Bx;
	const float Az = Twz - Bz;
	return FMath::RadiansToDegrees(FMath::Atan2(Az, Ax));
}

float FBoatDynamics::GetApparentWindSpeedKn() const
{
	const float Bx = U / KnToFts;
	const float Bz = Vsway / KnToFts;
	const float Twx = TrueWindSpeedKn * FMath::Cos(FMath::DegreesToRadians(TrueWindDirDeg - Heading));
	const float Twz = TrueWindSpeedKn * FMath::Sin(FMath::DegreesToRadians(TrueWindDirDeg - Heading));
	const float Ax = Twx - Bx;
	const float Az = Twz - Bz;
	return FMath::Sqrt(Ax * Ax + Az * Az);
}

FSailForceInput FBoatDynamics::ComputeSailForceStub() const
{
	// Simplified polar until cloth is ported. Uses CF_MAX * ½ρ_air V² SA style scaling
	// calibrated loosely to J/105 (joint force/heel cal ~ hundreds of lb).
	const float Aws = GetApparentWindSpeedKn();
	const float Awa = GetApparentWindAngleDeg();
	const float AwaAbs = FMath::Abs(Awa);
	// Ideal sheet ease vs AWA: tight upwind, freer reaching/running
	float IdealEase = 0.12f;
	if (AwaAbs < 40.f) IdealEase = 0.08f;
	else if (AwaAbs < 70.f) IdealEase = 0.12f + 0.25f * ((AwaAbs - 40.f) / 30.f);
	else if (AwaAbs < 120.f) IdealEase = 0.37f + 0.35f * ((AwaAbs - 70.f) / 50.f);
	else IdealEase = 0.72f + 0.25f * FMath::Clamp((AwaAbs - 120.f) / 60.f, 0.f, 1.f);
	const float SheetErr = FMath::Abs(SheetEase - IdealEase);
	// Full power near ideal; falls to ~25% when badly over/under sheeted
	const float SheetEff = FMath::Clamp(1.f - 1.6f * SheetErr, 0.25f, 1.f);

	// No-go / irons
	float Cl = 0.f;
	if (AwaAbs > 28.f && AwaAbs < 160.f)
	{
		// Peak near ~40–50°, fall toward 180
		const float T = (AwaAbs - 28.f) / (50.f - 28.f);
		const float Peak = FMath::Clamp(T, 0.f, 1.f);
		const float Fall = FMath::Clamp(1.f - (AwaAbs - 90.f) / 90.f, 0.f, 1.f);
		Cl = CFMax * Peak * Fall * SheetEff;
	}
	// Air density slug/ft³ ~0.00237; convert kn→ft/s
	const float Va = Aws * KnToFts;
	const float Q = 0.5f * 0.00237f * Va * Va;
	const float Mag = Q * SATotal * Cl;
	const float Sign = (Awa >= 0.f) ? 1.f : -1.f;
	const float AwaRad = FMath::DegreesToRadians(AwaAbs);
	FSailForceInput Out;
	Out.DriveLb = Mag * FMath::Sin(AwaRad);
	Out.SideLb = Sign * Mag * FMath::Cos(AwaRad * 0.85f);
	if (AwaAbs < 28.f)
	{
		Out.DriveLb = -0.02f * Q * SATotal;
		Out.SideLb = 0.f;
	}
	return Out;
}

void FBoatDynamics::UpdateHelm(float Dt)
{
	float Want = 0.f;
	bool bHaveWant = false;
	if (bManualHelm || !bAutoHeading)
	{
		// Slew toward manual target
		Want = ManualRudderTarget;
		bHaveWant = true;
	}
	else if (bAutoHeading)
	{
		const float Err = Wrap180(AutoTarget - Heading);
		const float ErrP = FMath::Abs(Err) < 2.f ? 0.f : Err;
		if (FMath::Abs(Err) < 2.f) AutoI *= 0.90f;
		else AutoI += Err * Dt;
		AutoI = FMath::Clamp(AutoI, -6.f, 6.f);
		const float YawDegS = R * 180.f / PI;
		Want = FMath::Clamp(-(KpAuto * ErrP + KiAuto * AutoI) + KdAuto * YawDegS, -35.f, 35.f);
		bHaveWant = true;
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

	const float PhiR = FMath::DegreesToRadians(Phi);
	const float Xsail = LpDrive * FMath::Cos(PhiR);
	const float Ysail = LpSide;
	const float NsailRaw = Ysail * (XCLR + SailLead);
	if (bAutoHeading)
	{
		if (!bLpNsailInit)
		{
			LpNsail = NsailRaw;
			bLpNsailInit = true;
		}
		LpNsail += 0.07f * (NsailRaw - LpNsail);
		NsailYaw = LpNsail;
	}
	else
	{
		bLpNsailInit = false;
		NsailYaw = NsailRaw;
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

	const float TwsKn = TrueWindSpeedKn;
	const float PhiMax = HeelWindMaxDeg(TwsKn);
	const float TargetPhi = -static_cast<float>(LeeSign) * HeelEquilibrium(LpSide * HeelSideGain, TwsKn);
	// First-order heel relax with rate limit (~web hExpAlpha(2.2) approx)
	float DPhi = (TargetPhi - Phi) * (1.f - FMath::Exp(-2.2f * Dt));
	const float MaxD = 14.f * Dt;
	DPhi = FMath::Clamp(DPhi, -MaxD, MaxD);
	Phi = FMath::Clamp(Phi + DPhi, -PhiMax, PhiMax);

	if (!FMath::IsFinite(U)) U = 0.f;
	if (!FMath::IsFinite(Vsway)) Vsway = 0.f;
	if (!FMath::IsFinite(R)) R = 0.f;
	if (!FMath::IsFinite(V)) V = 0.f;
	if (!FMath::IsFinite(Beta)) Beta = 0.f;
	if (!FMath::IsFinite(Phi)) Phi = 0.f;
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
	// Loose Phase-2 band (full ORC polar cert is later calibration work).
	const bool bSpdOk = Spd >= 3.5f && Spd <= 9.5f;
	const bool bHeelOk = HeelAbs >= 5.f && HeelAbs <= 32.f;
	const bool bOk = bSpdOk && bHeelOk && FMath::IsFinite(Spd) && FMath::IsFinite(HeelAbs);

	const FString Report = FString::Printf(
		TEXT("VPP golden TWS12 HDG185: SPD=%.2f kn (want 3.5–9.5) HEEL=%.1f° (want 5–32) %s"),
		Spd, HeelAbs, bOk ? TEXT("PASS") : TEXT("FAIL"));
	UE_LOG(LogSailSim, Log, TEXT("%s"), *Report);
	if (OutReport) *OutReport = Report;
	return bOk;
}
