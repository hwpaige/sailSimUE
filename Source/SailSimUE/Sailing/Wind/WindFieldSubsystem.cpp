#include "Sailing/Wind/WindFieldSubsystem.h"
#include "Sailing/Nav/NavGeo.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/SailSimPerf.h"
#include "SailSimUE.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

namespace WindFieldPrivate
{
	static float Wrap360(float D)
	{
		D = FMath::Fmod(D, 360.f);
		if (D < 0.f) D += 360.f;
		return D;
	}

	static constexpr float KnToCmPerSec = 51.444f;

	static uint32 HashU32(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	static float Hash01(int32 A, int32 B)
	{
		return (HashU32(uint32(A * 374761393 + B * 668265263)) & 0xFFFFFFu) / float(0xFFFFFFu);
	}

	static float ValueNoise2D(float X, float Y)
	{
		const int32 IX = FMath::FloorToInt(X);
		const int32 IY = FMath::FloorToInt(Y);
		const float FX = X - float(IX);
		const float FY = Y - float(IY);
		const float U = FX * FX * (3.f - 2.f * FX);
		const float V = FY * FY * (3.f - 2.f * FY);
		const float A = Hash01(IX, IY);
		const float B = Hash01(IX + 1, IY);
		const float C = Hash01(IX, IY + 1);
		const float D = Hash01(IX + 1, IY + 1);
		return FMath::Lerp(FMath::Lerp(A, B, U), FMath::Lerp(C, D, U), V);
	}
}

void UWindFieldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BaseSpeedKn = 12.f;
	BaseDirFromDeg = 225.f;
	TimeSec = 0.f;
	bBootstrapped = false;
	LastChopApplied = -1.f;
	UE_LOG(LogSailSim, Log, TEXT("WindField: init base=%.0fkn from %.0f° targetPuffs=%d (no mesh footprints)"),
		BaseSpeedKn, BaseDirFromDeg, TargetPuffCount);
}

void UWindFieldSubsystem::Deinitialize()
{
	Puffs.Reset();
	if (UWorld* World = GetWorld())
	{
		if (USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>())
		{
			Ocean->SetSurfaceChopIntensity(0.f);
		}
	}
	Super::Deinitialize();
}

bool UWindFieldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UWindFieldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWindFieldSubsystem, STATGROUP_Tickables);
}

FString UWindFieldSubsystem::GetStatusLine() const
{
	const FVector Focus = LastFocus.IsNearlyZero()
		? FVector(FNavGeo::BoatStartWorldCm2D().X, FNavGeo::BoatStartWorldCm2D().Y, 0.f)
		: LastFocus;
	const FWindSample S = SampleWindAt(Focus);
	return FString::Printf(TEXT("wind %.1fkn @%.0f° (base %.0f) Δ%+.0f° puffs=%d"),
		S.SpeedKn, S.DirFromDeg, BaseSpeedKn, S.DirShiftDeg, Puffs.Num());
}

void UWindFieldSubsystem::SetBaseWind(float SpeedKn, float DirFromDeg)
{
	// Physics knots (caller converts from display). Allow room for ~40 kn display
	// (display = physics × 8/25 → physics max ≈ 125).
	const float NewBase = FMath::Clamp(SpeedKn, 0.f, 130.f);
	const float NewDir = WindFieldPrivate::Wrap360(DirFromDeg);
	const float DBase = FMath::Abs(NewBase - BaseSpeedKn);
	float DDir = NewDir - BaseDirFromDeg;
	while (DDir > 180.f) DDir -= 360.f;
	while (DDir < -180.f) DDir += 360.f;
	BaseSpeedKn = NewBase;
	BaseDirFromDeg = NewDir;
	// User moved TWS/TWD sliders — snap the smoother so the boat follows immediately.
	if (bLocalSmoothInit && (DBase > 0.35f || FMath::Abs(DDir) > 1.5f))
	{
		SmoothSpeedKn = NewBase;
		SmoothDirFromDeg = NewDir;
	}
}

void UWindFieldSubsystem::SetFocus(FVector FocusWorld)
{
	LastFocus = FocusWorld;
}

void UWindFieldSubsystem::ForceFocus(FVector FocusWorld)
{
	LastFocus = FocusWorld;
	// One-shot population rebuild for possess/teleport — never per-frame.
	if (bEnablePuffs)
	{
		MaintainPopulation(/*bForce*/ true);
	}
}

FVector2D UWindFieldSubsystem::Focus2D() const
{
	if (!LastFocus.IsNearlyZero())
	{
		return FVector2D(LastFocus.X, LastFocus.Y);
	}
	return FNavGeo::BoatStartWorldCm2D();
}

float UWindFieldSubsystem::PuffWeight(const FWindPuff& P, const FVector2D& Pos) const
{
	const float Life = FMath::Clamp(P.AgeSec / FMath::Max(0.1f, P.LifeSec), 0.f, 1.f);
	const float Envelope = FMath::Sin(Life * PI);
	if (Envelope < 0.02f) return 0.f;

	const float ToDeg = BaseDirFromDeg + 180.f;
	const float Rad = FMath::DegreesToRadians(ToDeg);
	const FVector2D Along(FMath::Cos(Rad), FMath::Sin(Rad));
	const FVector2D Across(-Along.Y, Along.X);
	const FVector2D D = Pos - P.CenterCm;
	const float U = FVector2D::DotProduct(D, Along) / FMath::Max(100.f, P.MajorCm);
	const float V = FVector2D::DotProduct(D, Across) / FMath::Max(100.f, P.MinorCm);
	const float R2 = U * U + V * V;
	if (R2 > 1.f) return 0.f;
	return FMath::Square(1.f - R2) * Envelope;
}

FWindSample UWindFieldSubsystem::SampleWindAt(FVector WorldPos) const
{
	FWindSample Out;
	Out.SpeedKn = BaseSpeedKn;
	Out.DirFromDeg = BaseDirFromDeg;
	Out.GustFrac = 0.f;
	Out.DirShiftDeg = 0.f;

	// ---- Mesoscale speed (slow): ~2–5 min variability, not multi-Hz jitter ----
	// Spatial wavelength ~ several km; temporal ~ minutes.
	const float NX = WorldPos.X * 0.35e-5f + TimeSec * 0.012f;
	const float NY = WorldPos.Y * 0.35e-5f - TimeSec * 0.009f;
	const float N1 = WindFieldPrivate::ValueNoise2D(NX, NY) * 2.f - 1.f;
	const float MesoGustKn = N1 * 1.4f;
	// Long breathing gust (~2.6 min period), low amplitude
	const float Pulse = 0.35f * FMath::Sin(TimeSec * 0.040f);

	// ---- Mesoscale direction: ~5–12 min scales (real oscillating shifts) ----
	const float NXd = WorldPos.X * 0.22e-5f + TimeSec * 0.0055f;
	const float NYd = WorldPos.Y * 0.22e-5f - TimeSec * 0.0045f;
	const float N2 = WindFieldPrivate::ValueNoise2D(NXd + 17.3f, NYd - 9.1f) * 2.f - 1.f;
	const float MesoDirDeg = N2 * 3.5f;
	// ~5.2 min gentle oscillation (was ~74 s and felt like the wind was twitching)
	const float DirPulse = 0.8f * FMath::Sin(TimeSec * 0.020f + 0.7f);

	float GustSum = MesoGustKn + Pulse;
	float DirSum = MesoDirDeg + DirPulse;

	if (bEnablePuffs)
	{
		const FVector2D P(WorldPos.X, WorldPos.Y);
		for (const FWindPuff& Pf : Puffs)
		{
			// Soft spatial falloff: dir uses W^0.75 so headers build as you sail in.
			const float W = PuffWeight(Pf, P);
			if (W <= 0.f) continue;
			const float WDir = FMath::Pow(W, 0.75f);
			GustSum += W * Pf.SpeedBoostKn;
			DirSum += WDir * Pf.DirShiftDeg;
		}
	}

	Out.SpeedKn = FMath::Clamp(BaseSpeedKn + GustSum, 0.5f, 130.f);
	Out.DirShiftDeg = DirSum;
	Out.DirFromDeg = WindFieldPrivate::Wrap360(BaseDirFromDeg + DirSum);
	Out.GustFrac = (BaseSpeedKn > 0.5f)
		? FMath::Max(0.f, (Out.SpeedKn - BaseSpeedKn) / BaseSpeedKn)
		: 0.f;
	return Out;
}

FWindSample UWindFieldSubsystem::SampleSmoothedWindAt(FVector WorldPos, float DeltaSeconds)
{
	const FWindSample Raw = SampleWindAt(WorldPos);
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.1f);

	if (!bLocalSmoothInit)
	{
		SmoothSpeedKn = Raw.SpeedKn;
		SmoothDirFromDeg = Raw.DirFromDeg;
		bLocalSmoothInit = true;
	}
	else
	{
		const float TauS = FMath::Max(2.f, LocalSpeedSmoothTauSec);
		const float TauD = FMath::Max(2.f, LocalDirSmoothTauSec);
		const float AS = 1.f - FMath::Exp(-Dt / TauS);
		const float AD = 1.f - FMath::Exp(-Dt / TauD);
		SmoothSpeedKn += AS * (Raw.SpeedKn - SmoothSpeedKn);
		// Shortest-arc lerp on compass direction
		float Derr = Raw.DirFromDeg - SmoothDirFromDeg;
		while (Derr > 180.f) Derr -= 360.f;
		while (Derr < -180.f) Derr += 360.f;
		SmoothDirFromDeg = WindFieldPrivate::Wrap360(SmoothDirFromDeg + AD * Derr);
	}

	FWindSample Out = Raw;
	Out.SpeedKn = SmoothSpeedKn;
	Out.DirFromDeg = SmoothDirFromDeg;
	Out.DirShiftDeg = SmoothDirFromDeg - BaseDirFromDeg;
	while (Out.DirShiftDeg > 180.f) Out.DirShiftDeg -= 360.f;
	while (Out.DirShiftDeg < -180.f) Out.DirShiftDeg += 360.f;
	Out.GustFrac = (BaseSpeedKn > 0.5f)
		? FMath::Max(0.f, (Out.SpeedKn - BaseSpeedKn) / BaseSpeedKn)
		: 0.f;
	return Out;
}

FWindSample UWindFieldSubsystem::SampleWindLatLon(double Lat, double Lon) const
{
	double X = 0, Y = 0;
	FNavGeo::LatLonToWorldCm(Lat, Lon, X, Y);
	return SampleWindAt(FVector(float(X), float(Y), 0.f));
}

void UWindFieldSubsystem::SpawnPuffNear(const FVector2D& Focus, bool bAimAtBoat)
{
	const int32 Seed = (Puffs.Num() + 1) * 9176 + FMath::Rand() + int32(TimeSec * 10.f);
	FWindPuff P;

	const float ToRad = FMath::DegreesToRadians(BaseDirFromDeg + 180.f);
	const FVector2D WindTo(FMath::Cos(ToRad), FMath::Sin(ToRad));

	if (bAimAtBoat)
	{
		const float Upwind = FMath::Lerp(2500.f, 14000.f, WindFieldPrivate::Hash01(Seed, 1));
		const float Across = (WindFieldPrivate::Hash01(Seed, 2) - 0.5f) * 9000.f;
		const FVector2D AcrossDir(-WindTo.Y, WindTo.X);
		P.CenterCm = Focus - WindTo * Upwind + AcrossDir * Across;
	}
	else
	{
		const float Ang = WindFieldPrivate::Hash01(Seed, 1) * 2.f * PI;
		const float Dist = FMath::Sqrt(WindFieldPrivate::Hash01(Seed, 2)) * SpawnRadiusCm;
		P.CenterCm = Focus + FVector2D(FMath::Cos(Ang), FMath::Sin(Ang)) * Dist;
	}

	// Larger footprints so sailing through a puff ramps over many seconds, not a blink.
	P.MajorCm = FMath::Lerp(14000.f, 38000.f, WindFieldPrivate::Hash01(Seed, 3));
	P.MinorCm = P.MajorCm * FMath::Lerp(0.45f, 0.80f, WindFieldPrivate::Hash01(Seed, 4));
	P.SpeedBoostKn = FMath::Lerp(MinGustKn, MaxGustKn, WindFieldPrivate::Hash01(Seed, 5));
	const float Sign = (WindFieldPrivate::Hash01(Seed, 6) < 0.5f) ? -1.f : 1.f;
	// Mild peak shifts; long life + sin envelope already softens onset.
	P.DirShiftDeg = Sign * FMath::Lerp(2.f, MaxDirShiftDeg, WindFieldPrivate::Hash01(Seed, 7));
	P.LifeSec = FMath::Lerp(MinLifeSec, MaxLifeSec, WindFieldPrivate::Hash01(Seed, 8));
	// Enter mid-life less often — avoid spawning already-peaked shifts on the boat.
	P.AgeSec = bAimAtBoat ? FMath::Lerp(0.f, P.LifeSec * 0.12f, WindFieldPrivate::Hash01(Seed, 10)) : 0.f;
	Puffs.Add(P);
}

void UWindFieldSubsystem::MaintainPopulation(bool bForce)
{
	const FVector2D F = Focus2D();
	const float CullR2 = CullRadiusCm * CullRadiusCm;
	for (int32 I = Puffs.Num() - 1; I >= 0; --I)
	{
		FWindPuff& P = Puffs[I];
		const float D2 = FVector2D::DistSquared(P.CenterCm, F);
		if (P.AgeSec >= P.LifeSec || D2 > CullR2)
		{
			Puffs.RemoveAtSwap(I);
		}
	}

	int32 Aimed = 0;
	const float ToRad = FMath::DegreesToRadians(BaseDirFromDeg + 180.f);
	const FVector2D WindTo(FMath::Cos(ToRad), FMath::Sin(ToRad));
	for (const FWindPuff& P : Puffs)
	{
		const FVector2D D = F - P.CenterCm;
		if (FVector2D::DotProduct(D, WindTo) > 0.f && D.Size() < SpawnRadiusCm * 0.7f)
		{
			++Aimed;
		}
	}

	int32 Guard = 0;
	while (Aimed < 4 && Puffs.Num() < TargetPuffCount + 4 && Guard++ < 8)
	{
		SpawnPuffNear(F, /*bAimAtBoat*/ true);
		++Aimed;
	}
	Guard = 0;
	while (Puffs.Num() < TargetPuffCount && Guard++ < TargetPuffCount)
	{
		SpawnPuffNear(F, /*bAimAtBoat*/ false);
	}

	// Log once on first bootstrap only (bForce used to log every call and tanked FPS
	// when the boat called ForceFocus each tick).
	if (!bBootstrapped)
	{
		UE_LOG(LogSailSim, Log, TEXT("WindField: population=%d focus=(%.0f,%.0f) base=%.1fkn@%.0f"),
			Puffs.Num(), F.X, F.Y, BaseSpeedKn, BaseDirFromDeg);
		bBootstrapped = true;
	}
	else if (bForce)
	{
		UE_LOG(LogSailSim, Verbose, TEXT("WindField: force rebuild population=%d focus=(%.0f,%.0f)"),
			Puffs.Num(), F.X, F.Y);
	}
}

void UWindFieldSubsystem::EvolvePuffs(float Dt)
{
	const float ToRad = FMath::DegreesToRadians(BaseDirFromDeg + 180.f);
	const FVector2D WindTo(FMath::Cos(ToRad), FMath::Sin(ToRad));
	const float Drift = BaseSpeedKn * WindFieldPrivate::KnToCmPerSec * Dt * 0.85f;

	for (FWindPuff& P : Puffs)
	{
		P.AgeSec += Dt;
		const float LocalBoost = 1.f + 0.25f * (P.SpeedBoostKn / FMath::Max(1.f, MaxGustKn));
		P.CenterCm += WindTo * (Drift * LocalBoost);
	}
}

void UWindFieldSubsystem::ApplyWaterChopFromLocalGust()
{
	if (WaterChopResponse <= 0.f) return;
	UWorld* World = GetWorld();
	if (!World) return;
	USailOceanSubsystem* Ocean = World->GetSubsystem<USailOceanSubsystem>();
	if (!Ocean) return;

	const FWindSample S = SampleWindAt(LastFocus);
	// Map gust fraction → 0..1 chop (short high-frequency normal detail on water material).
	const float Chop01 = FMath::Clamp(S.GustFrac * 1.35f, 0.f, 1.f) * WaterChopResponse;
	if (FMath::Abs(Chop01 - LastChopApplied) < 0.02f) return;
	LastChopApplied = Chop01;
	Ocean->SetSurfaceChopIntensity(Chop01);
}

void UWindFieldSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || World->IsPreviewWorld()) return;

	SAIL_PERF_SCOPE(Wind);
	const float Dt = FMath::Clamp(DeltaTime, 0.f, 0.1f);
	TimeSec += Dt;
	FSailSimPerf::Get().WindPuffs = Puffs.Num();

	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			LastFocus = Pawn->GetActorLocation();
		}
	}
	if (LastFocus.IsNearlyZero())
	{
		const FVector2D H = FNavGeo::BoatStartWorldCm2D();
		LastFocus = FVector(H.X, H.Y, 0.f);
	}

	if (bEnablePuffs)
	{
		SpawnAccum += Dt;
		// Rebuild population every ~2 s (was 0.6 s) — fewer spawn/despawn discontinuities.
		if (!bBootstrapped || SpawnAccum > 2.0f)
		{
			SpawnAccum = 0.f;
			MaintainPopulation(/*bForce*/ !bBootstrapped);
		}
		EvolvePuffs(Dt);
	}

	ApplyWaterChopFromLocalGust();

	static float LogAccum = 0.f;
	LogAccum += Dt;
	if (LogAccum > 2.5f)
	{
		LogAccum = 0.f;
		const FWindSample S = SampleWindAt(LastFocus);
		UE_LOG(LogSailSim, Log,
			TEXT("WindField: local %.1fkn @%.0f° (base %.1f@%.0f Δ%+.1f° gust+%.0f%%) puffs=%d chop=%.2f"),
			S.SpeedKn, S.DirFromDeg, BaseSpeedKn, BaseDirFromDeg, S.DirShiftDeg,
			S.GustFrac * 100.f, Puffs.Num(), LastChopApplied);
	}
}
