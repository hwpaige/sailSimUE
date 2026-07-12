#include "Sailing/SailClothSim.h"
#include "ProceduralMeshComponent.h"

void FSailClothSim::Clear()
{
	Pos.Reset();
	Prev.Reset();
	Rest.Reset();
	bPinned.Reset();
	Springs.Reset();
	ClewIndex = INDEX_NONE;
	bInitialized = false;
}

bool FSailClothSim::BuildFromMesh(UProceduralMeshComponent* Mesh, int32 Section)
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

	SectionIndex = Section;
	const int32 N = Sec->ProcVertexBuffer.Num();
	Pos.SetNum(N);
	Prev.SetNum(N);
	Rest.SetNum(N);
	bPinned.Init(0, N);

	float MinX = TNumericLimits<float>::Max();
	float MaxX = TNumericLimits<float>::Lowest();
	for (int32 I = 0; I < N; ++I)
	{
		const FVector P = Sec->ProcVertexBuffer[I].Position;
		Pos[I] = Prev[I] = Rest[I] = P;
		MinX = FMath::Min(MinX, P.X);
		MaxX = FMath::Max(MaxX, P.X);
	}

	// Pin luff: near mast edge in sail-pivot space (forward/low-X band)
	const float LuffBand = FMath::Max(8.f, (MaxX - MinX) * 0.08f);
	int32 ClewCandidate = 0;
	float BestClewScore = -TNumericLimits<float>::Max();
	for (int32 I = 0; I < N; ++I)
	{
		if (Pos[I].X <= MinX + LuffBand)
		{
			bPinned[I] = 1;
		}
		const float Score = -Pos[I].X + FMath::Abs(Pos[I].Y) * 0.35f;
		if (Score > BestClewScore && !bPinned[I])
		{
			BestClewScore = Score;
			ClewCandidate = I;
		}
	}
	ClewIndex = ClewCandidate;

	TSet<uint64> EdgeKeys;
	auto EdgeKey = [](int32 A, int32 B) -> uint64
	{
		const int32 Lo = FMath::Min(A, B);
		const int32 Hi = FMath::Max(A, B);
		return (uint64(uint32(Lo)) << 32) | uint64(uint32(Hi));
	};
	const TArray<uint32>& Idx = Sec->ProcIndexBuffer;
	for (int32 T = 0; T + 2 < Idx.Num(); T += 3)
	{
		const int32 A = int32(Idx[T]), B = int32(Idx[T + 1]), C = int32(Idx[T + 2]);
		const int32 Edges[3][2] = { { A, B }, { B, C }, { C, A } };
		for (int32 E = 0; E < 3; ++E)
		{
			const int32 Ia = Edges[E][0], Ib = Edges[E][1];
			if (!Pos.IsValidIndex(Ia) || !Pos.IsValidIndex(Ib)) continue;
			const uint64 K = EdgeKey(Ia, Ib);
			if (EdgeKeys.Contains(K)) continue;
			EdgeKeys.Add(K);
			FSpring S;
			S.A = Ia;
			S.B = Ib;
			S.RestLen = FVector::Dist(Rest[S.A], Rest[S.B]);
			if (S.RestLen > 0.5f)
			{
				Springs.Add(S);
			}
		}
	}

	bInitialized = Pos.Num() > 0 && Springs.Num() > 0;
	UE_LOG(LogTemp, Log, TEXT("SailClothSim: %d verts, %d springs, clew=%d"),
		Pos.Num(), Springs.Num(), ClewIndex);
	return bInitialized;
}

void FSailClothSim::Step(
	float Dt,
	const FVector& WindLocalDir,
	float WindSpeedKn,
	const FVector& ClewTargetLocal,
	float SheetEase01)
{
	if (!bInitialized || !bEnabled || Pos.Num() == 0) return;
	Dt = FMath::Clamp(Dt, 0.f, 1.f / 30.f);

	const FVector WindDir = WindLocalDir.GetSafeNormal();
	const float WindQ = FMath::Square(FMath::Max(0.f, WindSpeedKn)) * WindForceScale;

	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		if (bPinned[I])
		{
			Pos[I] = Rest[I];
			Prev[I] = Rest[I];
			continue;
		}
		FVector Vel = (Pos[I] - Prev[I]) * Damping;
		FVector Acc = FVector(0.f, 0.f, GravityCm * 0.12f);
		const FVector Fill = FVector::CrossProduct(WindDir, FVector::UpVector).GetSafeNormal();
		const float Side = (Rest[I].Y >= 0.f) ? 1.f : -1.f;
		Acc += WindDir * WindQ * 0.02f + Fill * Side * WindQ * 0.015f;
		Acc += (Rest[I] - Pos[I]) * 2.2f;

		const FVector Next = Pos[I] + Vel + Acc * (Dt * Dt);
		Prev[I] = Pos[I];
		Pos[I] = Next;
	}

	if (Pos.IsValidIndex(ClewIndex) && !bPinned[ClewIndex])
	{
		const float Pull = FMath::Lerp(0.55f, 0.15f, FMath::Clamp(SheetEase01, 0.f, 1.f)) * SheetPullScale;
		Pos[ClewIndex] = FMath::Lerp(Pos[ClewIndex], ClewTargetLocal, FMath::Clamp(Pull, 0.f, 1.f));
	}

	for (int32 Iter = 0; Iter < ConstraintIters; ++Iter)
	{
		for (const FSpring& S : Springs)
		{
			FVector& PA = Pos[S.A];
			FVector& PB = Pos[S.B];
			FVector Delta = PB - PA;
			const float Len = Delta.Size();
			if (Len < 1e-4f) continue;
			const float Diff = (Len - S.RestLen) / Len;
			const FVector Corr = Delta * 0.5f * Diff * StructuralStiffness;
			if (!bPinned[S.A]) PA += Corr;
			if (!bPinned[S.B]) PB -= Corr;
			if (bPinned[S.A]) PA = Rest[S.A];
			if (bPinned[S.B]) PB = Rest[S.B];
		}
		if (Pos.IsValidIndex(ClewIndex) && !bPinned[ClewIndex])
		{
			const float Pull = FMath::Lerp(0.4f, 0.1f, FMath::Clamp(SheetEase01, 0.f, 1.f)) * SheetPullScale;
			Pos[ClewIndex] = FMath::Lerp(Pos[ClewIndex], ClewTargetLocal, Pull);
		}
	}
}

void FSailClothSim::PushToMesh(UProceduralMeshComponent* Mesh) const
{
	if (!bInitialized || !Mesh || SectionIndex < 0) return;
	FProcMeshSection* Sec = Mesh->GetProcMeshSection(SectionIndex);
	if (!Sec || Sec->ProcVertexBuffer.Num() != Pos.Num()) return;

	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Vertices.SetNum(Pos.Num());
	Normals.SetNum(Pos.Num());
	UV0.SetNum(Pos.Num());
	Colors.SetNum(Pos.Num());
	Tangents.SetNum(Pos.Num());

	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		Vertices[I] = Pos[I];
		UV0[I] = Sec->ProcVertexBuffer[I].UV0;
		Colors[I] = FLinearColor(Sec->ProcVertexBuffer[I].Color);
		Tangents[I] = FProcMeshTangent(Sec->ProcVertexBuffer[I].Tangent.TangentX, false);
	}

	Normals.Init(FVector::ZeroVector, Pos.Num());
	const TArray<uint32>& Idx = Sec->ProcIndexBuffer;
	for (int32 T = 0; T + 2 < Idx.Num(); T += 3)
	{
		const int32 Ia = int32(Idx[T]), Ib = int32(Idx[T + 1]), Ic = int32(Idx[T + 2]);
		if (!Vertices.IsValidIndex(Ia) || !Vertices.IsValidIndex(Ib) || !Vertices.IsValidIndex(Ic)) continue;
		const FVector N = FVector::CrossProduct(
			Vertices[Ib] - Vertices[Ia], Vertices[Ic] - Vertices[Ia]).GetSafeNormal();
		Normals[Ia] += N;
		Normals[Ib] += N;
		Normals[Ic] += N;
	}
	for (FVector& N : Normals)
	{
		if (!N.Normalize()) N = FVector::UpVector;
	}

	Mesh->UpdateMeshSection_LinearColor(SectionIndex, Vertices, Normals, UV0, Colors, Tangents);
}
