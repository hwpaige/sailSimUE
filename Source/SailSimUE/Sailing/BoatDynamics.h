// Port of SailSim web 3-DOF VPP (hBoat in webgl-utils.sailing.js).
// Internal units match the web model: feet, pounds, degrees (except r in rad/s).

#pragma once

#include "CoreMinimal.h"

/** True wind and simple sail-force inputs (pounds, boat axes). */
struct FSailForceInput
{
	float DriveLb = 0.f; // +bow
	float SideLb = 0.f;  // +starboard
};

/**
 * Fossen-style 3-DOF body-frame dynamics + heel equilibrium.
 * Source of truth: sail-sim/frontend/public/webgl-utils.sailing.js (hBoat).
 */
struct FBoatDynamics
{
	// Derived display state
	float V = 0.f;       // ft/s
	float Beta = 0.f;    // leeway deg
	float Phi = 0.f;     // heel deg (+ starboard)
	float Heading = 0.f; // deg, 0 = north / +X world in our UE mapping
	float YawRate = 0.f; // deg/s
	float Rudder = 0.f;  // internal model deg (−35..+35)

	// Body-frame state
	float U = 0.f; // surge ft/s
	float Vsway = 0.f;
	float R = 0.f; // yaw rad/s

	// Control
	bool bSailing = true;
	bool bAutoHeading = true;
	float AutoTarget = 90.f;
	float AutoI = 0.f;
	float ManualRudderTarget = 0.f; // user tiller (starboard +) before model sign flip
	bool bManualHelm = false;
	/** 0 = sheeted hard, 1 = fully eased. Affects stub sail force until cloth measure lands. */
	float SheetEase = 0.20f;

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
	float KpAuto = 14.f;
	float KiAuto = 4.f;
	float KdAuto = 14.f;

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

	// Environment
	float TrueWindSpeedKn = 18.f;
	float TrueWindDirDeg = 225.f; // from which wind blows (met convention)

	// Constants (web)
	static constexpr float RhoWater = 1.9905f; // slug/ft³ seawater
	static constexpr float KnToFts = 1.6878f;
	static constexpr float G = 32.174f;
	static constexpr float HeelGmScale = 0.90f;
	static constexpr float HeelSideGain = 0.93f;
	static constexpr float RudSlewRate = 45.f; // deg/s

	void InitJ105();
	/** Apply sail_geom.boat3d `sailing` + `dims_ft` block (imperial). */
	void ApplySailingParams(
		float InDispLb, float InBallastLb, float InBeamFt, float InLwlFt, float InDraftFt, float InTcFt,
		float InLatArea, float InKeelArea, float InRudArea, float InKeelSpan,
		float InClrX, float InClrZ, float InSaTotal, float InHullSpeedKn, float InGmFt,
		float InLoaFt, float InMastTopFt);
	void Reset();
	void SetRudderStarboardPositive(float Deg);
	/** Sheet ease 0..1 (hard → eased). */
	void SetSheetEase(float Ease01);
	void Update(float Dt);

	float GetSpeedKnots() const { return V / KnToFts; }
	float GetApparentWindAngleDeg() const;
	float GetApparentWindSpeedKn() const;

private:
	float LpDrive = 0.f;
	float LpSide = 0.f;
	bool bLpInit = false;
	float LpNsail = 0.f;
	bool bLpNsailInit = false;
	float NsailYaw = 0.f;
	int32 LeeSign = 1; // +1 wind from stbd (port leeward)

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
	static float Wrap180(float Deg);
	static float Wrap360(float Deg);
};
