#include "Sailing/Terrain/NavtMeshLoader.h"
#include "SailSimUE.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

bool FNavtMeshLoader::LoadFile(const FString& AbsPath, FNavtMeshData& Out)
{
	Out = FNavtMeshData();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *AbsPath) || Bytes.Num() < 16)
	{
		UE_LOG(LogSailSim, Warning, TEXT("NavtMesh: failed to read %s"), *AbsPath);
		return false;
	}

	const uint8* D = Bytes.GetData();
	if (D[0] != 'N' || D[1] != 'A' || D[2] != 'V' || D[3] != 'T')
	{
		UE_LOG(LogSailSim, Warning, TEXT("NavtMesh: bad magic %s"), *AbsPath);
		return false;
	}

	const uint16 Ver = *reinterpret_cast<const uint16*>(D + 4);
	const uint32 NV = *reinterpret_cast<const uint32*>(D + 8);
	const uint32 NI = *reinterpret_cast<const uint32*>(D + 12);
	if (Ver != 1 || NV < 3 || NI < 3)
	{
		UE_LOG(LogSailSim, Warning, TEXT("NavtMesh: bad header ver=%u nV=%u nI=%u %s"),
			Ver, NV, NI, *AbsPath);
		return false;
	}

	// bake-terrain-tiles: Float32 xyz, Uint8 RGB (3 bytes/vert), pad to 4, Uint32 indices
	// (see writeNavtMesh — colors are NOT rgba)
	const int64 PosBytes = int64(NV) * 12;
	const int64 ColBytes = int64(NV) * 3;
	const int64 ColPad = (4 - (ColBytes % 4)) % 4;
	const int64 IdxBytes = int64(NI) * 4;
	const int64 Need = 16 + PosBytes + ColBytes + ColPad + IdxBytes;
	if (Bytes.Num() < Need)
	{
		UE_LOG(LogSailSim, Warning, TEXT("NavtMesh: truncated file %s need=%lld have=%d nV=%u nI=%u"),
			*AbsPath, Need, Bytes.Num(), NV, NI);
		return false;
	}

	const float* PosF = reinterpret_cast<const float*>(D + 16);
	Out.Positions.SetNumUninitialized(int32(NV));
	Out.Normals.SetNumUninitialized(int32(NV));
	Out.Colors.SetNumUninitialized(int32(NV));
	Out.Bounds.Init();

	for (uint32 I = 0; I < NV; ++I)
	{
		const float XFt = PosF[I * 3 + 0];
		const float ElevM = PosF[I * 3 + 1];
		const float ZFt = PosF[I * 3 + 2];
		const FVector P = NavtToUeCm(XFt, ElevM, ZFt);
		Out.Positions[int32(I)] = P;
		Out.Bounds += P;
		Out.Normals[int32(I)] = FVector::UpVector;
	}

	const uint8* Col = D + 16 + PosBytes;
	for (uint32 I = 0; I < NV; ++I)
	{
		const uint8* C = Col + I * 3;
		Out.Colors[int32(I)] = FLinearColor(
			float(C[0]) / 255.f,
			float(C[1]) / 255.f,
			float(C[2]) / 255.f,
			1.f);
	}

	const uint32* Idx = reinterpret_cast<const uint32*>(D + 16 + PosBytes + ColBytes + ColPad);
	Out.Indices.SetNumUninitialized(int32(NI));
	for (uint32 I = 0; I < NI; ++I)
	{
		Out.Indices[int32(I)] = int32(Idx[I]);
	}

	// Face normals averaged
	for (int32 T = 0; T + 2 < Out.Indices.Num(); T += 3)
	{
		const int32 Ia = Out.Indices[T], Ib = Out.Indices[T + 1], Ic = Out.Indices[T + 2];
		if (!Out.Positions.IsValidIndex(Ia) || !Out.Positions.IsValidIndex(Ib)
			|| !Out.Positions.IsValidIndex(Ic))
		{
			continue;
		}
		const FVector N = FVector::CrossProduct(
			Out.Positions[Ib] - Out.Positions[Ia],
			Out.Positions[Ic] - Out.Positions[Ia]);
		Out.Normals[Ia] += N;
		Out.Normals[Ib] += N;
		Out.Normals[Ic] += N;
	}
	for (FVector& N : Out.Normals)
	{
		if (!N.Normalize())
		{
			N = FVector::UpVector;
		}
	}

	return Out.IsValid();
}
