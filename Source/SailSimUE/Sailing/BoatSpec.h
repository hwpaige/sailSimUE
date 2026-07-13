#pragma once

#include "CoreMinimal.h"
#include "Sailing/BoatPresets.h"

/**
 * Phase 3 boat specification (imperial, mirrors frontend boat-spec.ts / sail_geom).
 * Full C++ loft is future work; for now this drives presets, live scale, and dynamics.
 */
struct FBoatSpec
{
	FString Id;
	FString Name;
	FString JsonRelativePath;

	// Hull / rig (feet, pounds, ft²) — web units
	float Loa = 34.4f;
	float Lwl = 29.5f;
	float Beam = 11.f;
	float DispLb = 7750.f;
	float BallastLb = 3340.f;
	float SailArea = 545.f;
	float I = 44.f;
	float J = 13.5f;
	float P = 40.f;
	float E = 13.5f;
	float Draft = 6.5f;

	/** Live designer scales (1 = catalog). Applied on top of catalog numbers. */
	float ScaleLoa = 1.f;
	float ScaleBeam = 1.f;
	float ScaleDraft = 1.f;
	float ScaleSail = 1.f;

	float EffectiveLoa() const { return Loa * ScaleLoa; }
	float EffectiveLwl() const { return Lwl * ScaleLoa; }
	float EffectiveBeam() const { return Beam * ScaleBeam; }
	float EffectiveDraft() const { return Draft * ScaleDraft; }
	float EffectiveDisp() const
	{
		// Rough volume scale ~ LOA * beam * draft
		return DispLb * ScaleLoa * ScaleBeam * ScaleDraft;
	}
	float EffectiveSail() const { return SailArea * ScaleSail * ScaleLoa * ScaleBeam; } // area ~ L²-ish

	/** Uniform mesh scale for procedural loft (average linear). */
	float MeshUniformScale() const
	{
		return FMath::Max(0.25f, (ScaleLoa + ScaleBeam + ScaleDraft) / 3.f);
	}

	static FBoatSpec FromPresetId(const FString& PresetId)
	{
		FBoatSpec S;
		S.Id = PresetId;
		if (const FBoatPreset* P = FBoatPresets::Find(PresetId))
		{
			S.Name = P->DisplayName;
			S.JsonRelativePath = P->JsonRelativePath;
		}
		// Catalog numbers (imperial) — match boat-spec.ts
		if (PresetId.Equals(TEXT("j105"), ESearchCase::IgnoreCase))
		{
			S.Name = TEXT("J/105");
			S.Loa = 34.4f; S.Lwl = 29.5f; S.Beam = 11.f; S.DispLb = 7750.f; S.BallastLb = 3340.f;
			S.SailArea = 545.f; S.I = 44.f; S.J = 13.5f; S.P = 40.f; S.E = 13.5f; S.Draft = 6.5f;
		}
		else if (PresetId.Equals(TEXT("endeavour"), ESearchCase::IgnoreCase))
		{
			S.Name = TEXT("Endeavour");
			S.Loa = 129.6f; S.Lwl = 83.3f; S.Beam = 22.f; S.DispLb = 320000.f; S.BallastLb = 156000.f;
			S.SailArea = 6500.f; S.I = 135.f; S.J = 48.f; S.P = 155.f; S.E = 42.f; S.Draft = 14.75f;
		}
		else if (PresetId.Equals(TEXT("melges24"), ESearchCase::IgnoreCase))
		{
			S.Name = TEXT("Melges 24");
			S.Loa = 24.f; S.Lwl = 21.8f; S.Beam = 8.2f; S.DispLb = 1650.f; S.BallastLb = 1025.f;
			S.SailArea = 260.f; S.I = 28.f; S.J = 8.5f; S.P = 28.f; S.E = 10.f; S.Draft = 4.9f;
		}
		else if (PresetId.Equals(TEXT("cruiser36"), ESearchCase::IgnoreCase))
		{
			S.Name = TEXT("Cruiser 36");
			S.Loa = 36.f; S.Lwl = 30.5f; S.Beam = 12.f; S.DispLb = 15000.f; S.BallastLb = 6000.f;
			S.SailArea = 620.f; S.I = 48.f; S.J = 15.f; S.P = 42.f; S.E = 15.f; S.Draft = 6.3f;
		}
		return S;
	}
};
