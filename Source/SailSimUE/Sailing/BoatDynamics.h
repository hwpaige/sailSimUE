// Port of SailSim web 3-DOF VPP (hBoat in webgl-utils.sailing.js).
// Internal units match the web model: feet, pounds, degrees (except r in rad/s).
//
// === Axis & sign conventions (must stay consistent with web + UE visuals) ===
// Dynamics body frame (web cloth/VPP): +X bow, +Z starboard, +Y up.
// UE boat mesh: +X bow, +Y starboard, +Z up  →  map dyn.Z ↔ UE.Y when applying motion.
//
// True wind: meteorological FROM (deg). Air velocity = opposite (downwind TO).
// Apparent wind: air_vel − boat_vel. Signed AWA = FROM angle of apparent wind
//   (atan2 of −air_vel): + = wind from starboard, − = from port.
// LeeSign: +1 = leeward is PORT (wind from stbd / starboard tack);
//          −1 = leeward is STARBOARD (port tack). Matches web _hLeeSign.
// Phi heel: + = starboard rail down. targetPhi = −LeeSign × heelEq
//   → wind from stbd → LeeSign+1 → heel to port (negative Phi).
// Side force (lb, +starboard): on the boat, directed to LEEWARD
//   → SideLb = −LeeSign × |side|  so lee=port ⇒ SideLb < 0.
// Boom (UE): aft boom (−X); +yaw around Z → port (−Y). Lee boom angle uses +LeeSign.

#pragma once

#include "CoreMinimal.h"

/** True wind and simple sail-force inputs (pounds, boat axes). */
struct FSailForceInput
{
	float DriveLb = 0.f; // +bow
	float SideLb = 0.f;  // +starboard (leeward when signed correctly)
};

/**
 * Fossen-style 3-DOF body-frame dynamics + heel equilibrium.
 * Source of truth: sail-sim/frontend/public/webgl-utils.sailing.js (hBoat).
 */
struct FBoatDynamics
{
	// Derived display state
	float V = 0.f;       // ft/s
	float Beta = 0.f;    // leeway deg (+ = bow to stbd / crab to port? atan2(v,u))
	float Phi = 0.f;     // heel deg (+ starboard rail down)
	float Heading = 0.f; // deg, 0 = north / +X world in our UE mapping
	float YawRate = 0.f; // deg/s
	float Rudder = 0.f;  // internal model deg (−35..+35)

	// Body-frame state (web: +X fwd, +Z stbd as v sway)
	float U = 0.f; // surge ft/s
	float Vsway = 0.f; // sway ft/s (+ starboard)
	float R = 0.f; // yaw rad/s

	// Control
	bool bSailing = true;
	bool bAutoHeading = true;
	/** Autopilot mode: heading-hold / AWA-hold / nav waypoints (web autoMode). */
	enum class EAutoMode : uint8 { Hdg = 0, Awa = 1, Nav = 2 };
	EAutoMode AutoMode = EAutoMode::Hdg;
	float AutoTarget = 90.f;      // compass target (hdg + nav)
	float AutoAwaTarget = 45.f;   // signed AWA target (deg): + = stbd, − = port
	float AutoI = 0.f;
	bool bAutoTrim = false;       // AUTO TRIM — sheet toward AWA groove
	bool bTacking = false;
	float TackTarget = 0.f;
	int32 TackDir = 0;            // +1 / −1 (web tackDir)
	int32 NavWpIndex = 0;
	float ManualRudderTarget = 0.f;
	bool bManualHelm = false;
	/** 0 = hard on centerline, 1 = fully eased to leeward. */
	float SheetEase = 0.25f;
	float ClothForceScale = 1.f;

	/** Main outhaul 0..1 (web slider/100 → 70–100% of boom E + foot pull). */
	float Outhaul01 = 0.94f;
	/** Boom vang 0 = hard on (flat boom), 1 = off (boom free to rise). */
	float Vang01 = 0.40f;
	/** Jib car fore/aft on track 0..1. */
	float JibCar01 = 0.45f;
	/** Jib luff / leech line tension 0..1 (web defaults 0.35 / 0.15). */
	float JibLuffTension01 = 0.35f;
	float JibLeechTension01 = 0.15f;

	// J/105 defaults (imperial)
	float Xudot = 0.07f;
	float Yvdot = 0.85f;
	float Nrdot = 0.55f;
	float KzFrac = 0.24f;
	float XCLR = -1.0f;
	float XRud = -14.5f;
	float SailLead = -3.0f;
	float RudArea = 4.8f;
	float RudSpan = 4.0f;
	float CrS = 0.71f;
	float CdCrossY = 0.50f;
	float CdCrossN = 1.5f;
	/**
	 * Autopilot PID → rudder°. Web used 14/4/14 which saturates full tiller by
	 * ~2.5° error (hard deadband) → bang-bang heading hunt. These hold course
	 * under weather helm without slamming: firm P, strong D, modest I.
	 */
	float KpAuto = 6.5f;
	float KiAuto = 1.4f;
	float KdAuto = 8.0f;
	/** Soft deadband (deg) — residual error only; avoids hard on/off at 2°. */
	float AutoDeadbandDeg = 0.6f;
	/** Max |∫err| (deg·s); Ki*limit ≈ max standing weather-helm offset. */
	float AutoILimit = 10.f;
	/** Filtered yaw rate (°/s) for D term. */
	float AutoYawRateLp = 0.f;

	float Mass = 7750.f / 32.174f;
	float Iz = 1.f;
	float Mu = 1.f;
	float Mv = 1.f;
	float Mr = 1.f;
	float AKeel = 2.5f;
	float ARKeel = 1.4f;
	float ARud = 4.8f;
	float ARRud = 6.0f;

	float Disp = 7750.f;
	float LOA = 34.4f;
	float LWL = 29.5f;
	float Beam = 11.f;
	float Ballast = 3340.f;
	float LateralArea = 70.f;
	float KeelArea = 50.f;
	float KeelSpan = 6.f;
	float Draft = 6.5f;
	float Tc = 1.2f;
	float SATotal = 545.f;
	float CoeRef = 18.f;
	float ClrDepth = 3.f;
	float GM = 4.6f;
	float HullSpeedKn = 7.0f;
	float CFMax = 1.66f;
	float StabCal = 1.0f;
	float CrewHikeFtLb = 0.f;

	// Environment (PHYSICS knots — used by aero / VPP / wind field)
	float TrueWindSpeedKn = 18.f;
	float TrueWindDirDeg = 225.f; // FROM which wind blows (met)

	/**
	 * Wind label calibration (display kn ↔ physics kn).
	 * Empirically the old physics scale was ~3× too optimistic on the sails:
	 * labeled 25 kn looked/felt about 8 kn. Prefer relabeling over retuning
	 * CL/CD so the polar math stays intact.
	 *   DisplayKn = PhysicsKn * (FeelAt / PhysAt)
	 *   PhysicsKn = DisplayKn * (PhysAt / FeelAt)
	 */
	static constexpr float WindLabelPhysAt = 25.f;
	static constexpr float WindLabelFeelAt = 8.f;
	static float WindDisplayFromPhysics(float PhysicsKn)
	{
		return PhysicsKn * (WindLabelFeelAt / WindLabelPhysAt);
	}
	static float WindPhysicsFromDisplay(float DisplayKn)
	{
		return DisplayKn * (WindLabelPhysAt / WindLabelFeelAt);
	}
	/** Max display TWS on sliders (kn). Physics max = this × PhysAt/FeelAt. */
	static constexpr float WindDisplayMaxKn = 40.f;
	static float WindPhysicsMaxKn()
	{
		return WindPhysicsFromDisplay(WindDisplayMaxKn);
	}

	static constexpr float RhoWater = 1.9905f;
	static constexpr float RhoAir = 0.00237f; // slug/ft³
	static constexpr float KnToFts = 1.6878f;
	static constexpr float G = 32.174f;
	static constexpr float HeelGmScale = 0.82f;
	/** Web H_HEEL_SIDE_GAIN — was 1.65 (too aggressive, snap heel + weather helm). */
	static constexpr float HeelSideGain = 0.93f;
	static constexpr float RudSlewRate = 45.f;
	/** Second-order roll (deg/s). Exposed for HUD/debug. */
	float PhiRate = 0.f;

	void InitJ105();
	void ApplySailingParams(
		float InDispLb, float InBallastLb, float InBeamFt, float InLwlFt, float InDraftFt, float InTcFt,
		float InLatArea, float InKeelArea, float InRudArea, float InKeelSpan,
		float InClrX, float InClrZ, float InSaTotal, float InHullSpeedKn, float InGmFt,
		float InLoaFt, float InMastTopFt);
	void Reset();
	void SetRudderStarboardPositive(float Deg);
	void SetSheetEase(float Ease01);
	void SetOuthaul(float V01);
	void SetVang(float V01);
	void SetJibCar(float V01);
	void SetJibLuffTension(float V01);
	void SetJibLeechTension(float V01);
	void Update(float Dt);

	/** Start a tack/gybe through the wind (web startTack / tackBoat). */
	void StartTack();
	/** Engage AP capturing live setpoint for current mode (web hEngageAutopilot). */
	void EngageAutopilot(bool bCaptureLive);
	void DisengageAutopilot();
	/** Set mode; re-seeds setpoint if already engaged. */
	void SetAutoMode(EAutoMode Mode);
	void SetAutoAwaTarget(float SignedDeg);
	void SetAutoTrim(bool bOn);
	/** Closed-loop / open-loop sheet toward optimum AWA groove (web hUpdateAutoTrim). */
	void UpdateAutoTrim(float Dt);

	static bool RunGoldenSelfCheck(FString* OutReport = nullptr);

	float GetSpeedKnots() const { return V / KnToFts; }
	/** Signed AWA (deg): wind FROM relative to bow; + = from starboard. */
	float GetApparentWindAngleDeg() const;
	float GetApparentWindSpeedKn() const;
	/** +1 leeward is port (stbd tack); −1 leeward is starboard (port tack). */
	int32 GetLeeSign() const { return LeeSign; }
	/** True-wind FROM relative to heading, −180..180; + = from starboard. */
	float GetTrueWindFromRelDeg() const;
	/** Apparent wind air-velocity unit in body frame (+X fwd, +Z stbd). */
	void GetApparentWindAirVelUnit(float& OutDx, float& OutDz) const;
	/** Last low-passed sail force (lb). */
	float GetLpDriveLb() const { return LpDrive; }
	float GetLpSideLb() const { return LpSide; }
	/** Weather-helm yaw moment (ft·lb) fed into PhysStep. */
	float GetNsailYaw() const { return NsailYaw; }

	static float Wrap180(float Deg);
	static float Wrap360(float Deg);

private:
	float LpDrive = 0.f;
	float LpSide = 0.f;
	bool bLpInit = false;
	float LpNsail = 0.f;
	bool bLpNsailInit = false;
	float NsailYaw = 0.f;
	/** Low-passed equilibrium heel so LeeSign / force flips don't snap the rail. */
	float HeelTargetLp = 0.f;
	bool bHeelTargetLpInit = false;
	/** +1 = lee port (wind from stbd); −1 = lee starboard. */
	int32 LeeSign = 1;

	struct FFoilForce
	{
		float Fx = 0.f;
		float Fy = 0.f;
	};

	FFoilForce FoilForce(float Fx, float Fy, float Area, float ASlope, float AR, float MountRad) const;
	void UpdateHelm(float Dt);
	void PhysStep(float H, float Xsail, float Ysail);
	float RightingMoment(float PhiDeg) const;
	float HeelingArm() const;
	float HeelEquilibrium(float SideLb, float TwsKn) const;
	float HeelWindSideAtten(float TwsKn) const;
	float HeelWindGmBoost(float TwsKn) const;
	float HeelWindMaxDeg(float TwsKn) const;
	void UpdateLeeSign();
	FSailForceInput ComputeSailForceStub() const;
	/** Web-style CL/CD depower vs display TWS (flatten/reef). See ComputeSailForceStub. */
	static float SailDepowerScale(float TwsDisplayKn);
};
