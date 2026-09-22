#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderTimer.h"
#include "Engine/Engine.h"
#include "DynamicRHI.h"
#include "SailSimUE.h"

// Engine CalculateFPSTimings() — same source as console "stat fps".
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

/**
 * Performance diagnostics for Settings → PERFORMANCE.
 *
 * Two layers:
 *  1) Engine unit times (GGameThreadTime, GRenderThreadTime, waits, RHI, GPU) —
 *     these explain wall-clock frame time. Most "unaccounted" was actually
 *     render + game-thread wait for GPU, not missing SailSim scopes.
 *  2) SailSim game-thread scopes — breakdown of our own code as % of GT.
 */
struct FSailSimPerf
{
	// ---- Engine unit sample (ms, EMA) ----
	float WallFrameEmaMs = 16.f;
	float WallFrameLastMs = 16.f;
	float GameThreadEmaMs = 0.f;
	float GameThreadLastMs = 0.f;
	float GameWaitEmaMs = 0.f;   // GT idle waiting (often for render/GPU)
	float GameWaitLastMs = 0.f;
	float RenderThreadEmaMs = 0.f;
	float RenderThreadLastMs = 0.f;
	float RenderWaitEmaMs = 0.f;
	float RenderWaitLastMs = 0.f;
	float RhiThreadEmaMs = 0.f;
	float RhiThreadLastMs = 0.f;
	float SwapBufferEmaMs = 0.f;
	float SwapBufferLastMs = 0.f;
	/** GPU frame time from RHIGetGPUFrameCycles (0 if timestamps unavailable). */
	float GpuFrameEmaMs = 0.f;
	float GpuFrameLastMs = 0.f;
	/** Critical-path GT (includes dependent waits) — closer to "frame limited by GT". */
	float GameCriticalEmaMs = 0.f;
	float RenderCriticalEmaMs = 0.f;

	/**
	 * Dominant unit for the frame (largest of wall-relevant times).
	 * Used for the headline "bottleneck is …" line.
	 */
	enum class EBottleneck : uint8
	{
		Unknown = 0,
		GameThread,
		GameWait,      // GT blocked (vsync/GPU/render)
		RenderThread,
		RHI,
		GPU,
		Balanced
	};
	EBottleneck Bottleneck = EBottleneck::Unknown;
	FString BottleneckLabel;
	/** Actionable one-liner for the PERFORMANCE panel. */
	FString AdviceLabel;

	// ---- SailSim GT scopes ----
	enum class EBucket : uint8
	{
		Dynamics = 0,
		Rigging,
		Sails,
		Water,
		Terrain,
		Houses,
		Moored,
		Aids,
		Wind,
		Ocean,
		UI,
		HUD,
		Count
	};

	static constexpr int32 NumBuckets = static_cast<int32>(EBucket::Count);

	float EmaMs[NumBuckets] = {};
	float FrameAccMs[NumBuckets] = {};
	float LastMs[NumBuckets] = {};
	float PeakMs[NumBuckets] = {};

	/** Alias for older UI code. */
	float FrameEmaMs = 16.f;
	float FrameLastMs = 16.f;

	int32 TerrainTiles = 0;
	int32 TerrainVerts = 0;
	int32 StructureTiles = 0;
	int32 StructureVerts = 0;
	/** Resident structure tiles drawn as cooked StaticMesh vs PMC fallback. */
	int32 StructureSmTiles = 0;
	int32 StructurePmcTiles = 0;
	int32 MooredCount = 0;
	int32 AidCount = 0;
	int32 WindPuffs = 0;

	static FSailSimPerf& Get()
	{
		static FSailSimPerf G;
		return G;
	}

	void AddMs(EBucket B, float Ms)
	{
		const int32 I = static_cast<int32>(B);
		if (I < 0 || I >= NumBuckets) return;
		FrameAccMs[I] += Ms;
	}

	static float CyclesToMs(uint32 Cycles)
	{
		// G*ThreadTime are CPU cycles (set each frame in FViewport::Draw).
		return FPlatformTime::ToMilliseconds(Cycles);
	}

	/** Call once per chrome Tick after game systems have run. */
	void NoteFrameMs(float /*ChromeDeltaMsIgnored*/)
	{
		const float A = 0.15f;

		// Wall / FPS: match engine "stat fps" (GAverageMS / GAverageFPS).
		// Previously used FApp::GetDeltaTime() which is *simulation* tick delta —
		// in PIE that often runs longer than the real present interval, so FPS
		// read low vs how smooth the scene felt.
		float WallMs = 0.f;
		if (GAverageMS > 0.5f)
		{
			// Engine already applies EMA in CalculateFPSTimings (0.9/0.1).
			WallMs = GAverageMS;
			WallFrameLastMs = WallMs;
			WallFrameEmaMs = WallMs;
		}
		else
		{
			// Fallback: real wall clock between samples (not sim delta).
			static double LastWallSeconds = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (LastWallSeconds > 0.0)
			{
				WallMs = float((Now - LastWallSeconds) * 1000.0);
			}
			else
			{
				WallMs = FMath::Max(0.01f, float(FApp::GetDeltaTime()) * 1000.f);
			}
			LastWallSeconds = Now;
			// Ignore alt-tab / breakpoint stalls so the meter doesn't tank.
			WallMs = FMath::Clamp(WallMs, 0.01f, 250.f);
			WallFrameLastMs = WallMs;
			WallFrameEmaMs = FMath::Lerp(WallFrameEmaMs, WallMs, A);
		}
		FrameLastMs = WallFrameLastMs;
		FrameEmaMs = WallFrameEmaMs;

		// Engine unit times (busy time, excluding pure idle where noted).
		const float GtMs = CyclesToMs(GGameThreadTime);
		const float GtWaitMs = CyclesToMs(GGameThreadWaitTime);
		const float RtMs = CyclesToMs(GRenderThreadTime);
		const float RtWaitMs = CyclesToMs(GRenderThreadWaitTime);
		const float RhiMs = CyclesToMs(GRHIThreadTime);
		const float SwapMs = CyclesToMs(GSwapBufferTime);
		const float GpuMs = CyclesToMs(RHIGetGPUFrameCycles());
		const float GtCritMs = CyclesToMs(GGameThreadTimeCriticalPath);
		const float RtCritMs = CyclesToMs(GRenderThreadTimeCriticalPath);

		auto Ema = [A](float& Slot, float Sample)
		{
			if (Sample > 0.f || Slot > 0.f)
			{
				Slot = FMath::Lerp(Slot, Sample, A);
			}
		};

		GameThreadLastMs = GtMs;
		GameWaitLastMs = GtWaitMs;
		RenderThreadLastMs = RtMs;
		RenderWaitLastMs = RtWaitMs;
		RhiThreadLastMs = RhiMs;
		SwapBufferLastMs = SwapMs;
		GpuFrameLastMs = GpuMs;

		Ema(GameThreadEmaMs, GtMs);
		Ema(GameWaitEmaMs, GtWaitMs);
		Ema(RenderThreadEmaMs, RtMs);
		Ema(RenderWaitEmaMs, RtWaitMs);
		Ema(RhiThreadEmaMs, RhiMs);
		Ema(SwapBufferEmaMs, SwapMs);
		Ema(GpuFrameEmaMs, GpuMs);
		Ema(GameCriticalEmaMs, GtCritMs);
		Ema(RenderCriticalEmaMs, RtCritMs);

		// Fold SailSim scopes
		for (int32 I = 0; I < NumBuckets; ++I)
		{
			LastMs[I] = FrameAccMs[I];
			EmaMs[I] = FMath::Lerp(EmaMs[I], FrameAccMs[I], A);
			PeakMs[I] = FMath::Max(PeakMs[I], FrameAccMs[I]);
			FrameAccMs[I] = 0.f;
		}

		// Classify bottleneck: who is the long pole?
		const float BusyGt = GameThreadEmaMs;
		const float BusyRt = RenderThreadEmaMs;
		const float BusyRhi = RhiThreadEmaMs;
		const float BusyGpu = GpuFrameEmaMs;
		const float WaitGt = GameWaitEmaMs;
		const float PeakBusy = FMath::Max3(BusyGt, BusyRt, BusyRhi);
		const float PeakBusyGpu = FMath::Max(PeakBusy, BusyGpu);

		// GPU timestamps present and dominate → true GPU bound (Lumen/shadows/water).
		if (BusyGpu > 1.f && BusyGpu >= PeakBusy * 0.95f && BusyGpu >= WaitGt * 0.85f)
		{
			Bottleneck = EBottleneck::GPU;
			BottleneckLabel = TEXT("GPU (Lumen / shadows / water / draw)");
			AdviceLabel = TEXT(
				"GPU-bound: check Lumen reflections (half-res on), VSM, water. "
				"Console: stat gpu · ProfileGPU. Cloth/GT opts will not help.");
		}
		else if (WaitGt > PeakBusyGpu * 1.15f && WaitGt > 2.f)
		{
			// GT spends more time waiting than working → usually GPU / RT bound
			Bottleneck = EBottleneck::GameWait;
			BottleneckLabel = TEXT("WAIT (GT blocked — usually GPU/render/vsync)");
			AdviceLabel = TEXT(
				"GT is idle waiting on render/GPU. Not a SailSim CPU problem. "
				"Console: stat unit · stat gpu. Watch RT ≈ wall.");
		}
		else if (BusyRt >= BusyGt && BusyRt >= BusyRhi && BusyRt > 1.f)
		{
			Bottleneck = EBottleneck::RenderThread;
			BottleneckLabel = TEXT("RENDER THREAD");
			AdviceLabel = TEXT(
				"RT-bound: scene setup, water mesh, Lumen, PMC sails. "
				"Console: stat unit · ProfileGPU. Prefer render cuts over cloth opts.");
		}
		else if (BusyRhi >= BusyGt && BusyRhi >= BusyRt && BusyRhi > 1.f)
		{
			Bottleneck = EBottleneck::RHI;
			BottleneckLabel = TEXT("RHI / GPU SUBMIT");
			AdviceLabel = TEXT(
				"RHI submit bound — many draw calls / state changes. "
				"Console: stat rhi · ProfileGPU.");
		}
		else if (BusyGt > 1.f)
		{
			Bottleneck = EBottleneck::GameThread;
			BottleneckLabel = TEXT("GAME THREAD");
			AdviceLabel = TEXT(
				"GT-bound: see SailSim rows below (sails/cloth, moored, terrain). "
				"Optimize those buckets first.");
		}
		else
		{
			Bottleneck = EBottleneck::Balanced;
			BottleneckLabel = TEXT("balanced / idle");
			AdviceLabel = TEXT("Frame budget OK. Peaks reset when Settings re-opens.");
		}

		// Periodic log for offline profiling (filter: [perf]).
		static double LastPerfLog = 0.0;
		const double NowSec = FPlatformTime::Seconds();
		if (NowSec - LastPerfLog > 3.0)
		{
			LastPerfLog = NowSec;
			// Rank SailSim GT buckets by EMA ms.
			struct FRow { int32 I; float Ms; };
			TArray<FRow, TInlineAllocator<16>> Rows;
			for (int32 I = 0; I < NumBuckets; ++I)
			{
				if (EmaMs[I] > 0.05f) Rows.Add({ I, EmaMs[I] });
			}
			Rows.Sort([](const FRow& A, const FRow& B) { return A.Ms > B.Ms; });
			FString BucketLine;
			for (int32 R = 0; R < FMath::Min(6, Rows.Num()); ++R)
			{
				if (R) BucketLine += TEXT("  ");
				BucketLine += FString::Printf(TEXT("%s=%.2f"),
					BucketName(static_cast<EBucket>(Rows[R].I)), Rows[R].Ms);
			}
			UE_LOG(LogSailSim, Log,
				TEXT("[perf] wall=%.1fms (%.0f fps)  GT=%.1f  GTwait=%.1f  RT=%.1f  RHI=%.1f  GPU=%.1f  | %s  | bottleneck=%s | tiles T=%d/%dk H=%d/%dk moored=%d aids=%d"),
				WallFrameEmaMs, WallFrameEmaMs > 0.1f ? 1000.f / WallFrameEmaMs : 0.f,
				GameThreadEmaMs, GameWaitEmaMs, RenderThreadEmaMs, RhiThreadEmaMs, GpuFrameEmaMs,
				*BucketLine, *BottleneckLabel,
				TerrainTiles, TerrainVerts / 1000, StructureTiles, StructureVerts / 1000,
				MooredCount, AidCount);
		}
	}

	void ResetPeaks()
	{
		for (int32 I = 0; I < NumBuckets; ++I)
		{
			PeakMs[I] = LastMs[I];
		}
	}

	float TotalKnownEmaMs() const
	{
		float S = 0.f;
		for (int32 I = 0; I < NumBuckets; ++I) S += EmaMs[I];
		return S;
	}

	/** GT time not covered by SailSim scopes. */
	float OtherGameThreadEmaMs() const
	{
		return FMath::Max(0.f, GameThreadEmaMs - TotalKnownEmaMs());
	}

	static const TCHAR* BucketName(EBucket B)
	{
		switch (B)
		{
		case EBucket::Dynamics: return TEXT("Dynamics");
		case EBucket::Rigging: return TEXT("Rigging");
		case EBucket::Sails: return TEXT("Sails");
		case EBucket::Water: return TEXT("Water");
		case EBucket::Terrain: return TEXT("Terrain");
		case EBucket::Houses: return TEXT("Houses");
		case EBucket::Moored: return TEXT("Moored");
		case EBucket::Aids: return TEXT("Aids");
		case EBucket::Wind: return TEXT("Wind");
		case EBucket::Ocean: return TEXT("Ocean");
		case EBucket::UI: return TEXT("UI");
		case EBucket::HUD: return TEXT("HUD");
		default: return TEXT("?");
		}
	}

	static const TCHAR* BucketHint(EBucket B)
	{
		switch (B)
		{
		case EBucket::Dynamics: return TEXT("VPP, helm, AP");
		case EBucket::Rigging: return TEXT("boom, sheets, lights, compass");
		case EBucket::Sails: return TEXT("cloth + kite");
		case EBucket::Water: return TEXT("buoyancy samples");
		case EBucket::Terrain: return TEXT("land tiles");
		case EBucket::Houses: return TEXT("building tiles");
		case EBucket::Moored: return TEXT("moored fleet");
		case EBucket::Aids: return TEXT("buoys / beacons");
		case EBucket::Wind: return TEXT("puffs / field");
		case EBucket::Ocean: return TEXT("water zone follow");
		case EBucket::UI: return TEXT("chrome / gauges");
		case EBucket::HUD: return TEXT("HUD tick");
		default: return TEXT("");
		}
	}

	FString FormatSummary() const
	{
		return FString::Printf(
			TEXT("wall %.1f  GT %.1f  GTwait %.1f  RT %.1f  RHI %.1f  GPU %.1f  | sails %.2f ocean %.2f  bottleneck=%s"),
			WallFrameEmaMs, GameThreadEmaMs, GameWaitEmaMs, RenderThreadEmaMs, RhiThreadEmaMs,
			GpuFrameEmaMs,
			EmaMs[static_cast<int32>(EBucket::Sails)],
			EmaMs[static_cast<int32>(EBucket::Ocean)],
			*BottleneckLabel);
	}
};

/**
 * The FSailSimPerf singleton that game subsystems and the performance chrome write.
 * Editor modules must call this. FSailSimPerf::Get() is inline and would be a
 * different copy inside another DLL.
 */
SAILSIMUE_API FSailSimPerf& SailSimGetPerf();

struct FSailSimPerfScope
{
	FSailSimPerf::EBucket Bucket;
	double Start = 0.0;

	explicit FSailSimPerfScope(FSailSimPerf::EBucket InBucket)
		: Bucket(InBucket)
		, Start(FPlatformTime::Seconds())
	{
	}

	~FSailSimPerfScope()
	{
		const float Ms = float((FPlatformTime::Seconds() - Start) * 1000.0);
		FSailSimPerf::Get().AddMs(Bucket, Ms);
	}
};

#define SAIL_PERF_SCOPE_JOIN2(a, b) a##b
#define SAIL_PERF_SCOPE_JOIN(a, b) SAIL_PERF_SCOPE_JOIN2(a, b)
#define SAIL_PERF_SCOPE(BucketEnum) \
	FSailSimPerfScope SAIL_PERF_SCOPE_JOIN(SailPerfScope_, __LINE__)(FSailSimPerf::EBucket::BucketEnum)
