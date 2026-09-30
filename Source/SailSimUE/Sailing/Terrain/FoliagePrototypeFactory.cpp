#include "Sailing/Terrain/FoliagePrototypeFactory.h"
#include "SailSimUE.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"

namespace
{
	struct FProtoVert
	{
		FVector P;
		FVector N;
		FLinearColor C;
	};

	struct FProtoMesh
	{
		TArray<FProtoVert> Verts;
		TArray<int32> Indices;

		int32 Add(const FVector& P, const FVector& N, const FLinearColor& C)
		{
			const int32 I = Verts.Num();
			FProtoVert& V = Verts.AddDefaulted_GetRef();
			V.P = P;
			V.N = N.GetSafeNormal();
			V.C = C;
			return I;
		}

		void Tri(int32 A, int32 B, int32 CIdx)
		{
			Indices.Add(A);
			Indices.Add(B);
			Indices.Add(CIdx);
		}
	};

	void FillEllipsoid(FProtoMesh& M, const FVector& Center, const FVector& Radii, int32 Segs, int32 Rings, const FLinearColor& Color)
	{
		Segs = FMath::Clamp(Segs, 6, 24);
		Rings = FMath::Clamp(Rings, 4, 16);
		const int32 Base = M.Verts.Num();
		for (int32 R = 0; R <= Rings; ++R)
		{
			const float V = float(R) / float(Rings);
			const float Phi = PI * V;
			const float Sp = FMath::Sin(Phi);
			const float Cp = FMath::Cos(Phi);
			for (int32 S = 0; S <= Segs; ++S)
			{
				const float U = float(S) / float(Segs);
				const float Th = 2.f * PI * U;
				const float St = FMath::Sin(Th);
				const float Ct = FMath::Cos(Th);
				// Unit sphere then scale → approximate normal.
				const FVector Unit(Sp * Ct, Sp * St, Cp);
				const FVector P = Center + FVector(Unit.X * Radii.X, Unit.Y * Radii.Y, Unit.Z * Radii.Z);
				const FVector N = FVector(Unit.X / FMath::Max(Radii.X, 1.f), Unit.Y / FMath::Max(Radii.Y, 1.f), Unit.Z / FMath::Max(Radii.Z, 1.f)).GetSafeNormal();
				M.Add(P, N, Color);
			}
		}
		const int32 Stride = Segs + 1;
		for (int32 R = 0; R < Rings; ++R)
		{
			for (int32 S = 0; S < Segs; ++S)
			{
				const int32 I0 = Base + R * Stride + S;
				const int32 I1 = I0 + 1;
				const int32 I2 = I0 + Stride;
				const int32 I3 = I2 + 1;
				M.Tri(I0, I2, I1);
				M.Tri(I1, I2, I3);
			}
		}
	}

	void FillCylinder(FProtoMesh& M, const FVector& Base, float Radius, float Height, int32 Segs, const FLinearColor& Color)
	{
		Segs = FMath::Clamp(Segs, 6, 20);
		const int32 BaseIdx = M.Verts.Num();
		for (int32 S = 0; S <= Segs; ++S)
		{
			const float U = float(S) / float(Segs);
			const float Th = 2.f * PI * U;
			const float Ct = FMath::Cos(Th);
			const float St = FMath::Sin(Th);
			const FVector N(Ct, St, 0.f);
			const FVector Ring(Ct * Radius, St * Radius, 0.f);
			M.Add(Base + Ring, N, Color);
			M.Add(Base + Ring + FVector(0.f, 0.f, Height), N, Color);
		}
		for (int32 S = 0; S < Segs; ++S)
		{
			const int32 I0 = BaseIdx + S * 2;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + 2;
			const int32 I3 = I0 + 3;
			M.Tri(I0, I2, I1);
			M.Tri(I1, I2, I3);
		}
		// Caps
		const int32 BotC = M.Add(Base, FVector::DownVector, Color);
		const int32 TopC = M.Add(Base + FVector(0.f, 0.f, Height), FVector::UpVector, Color);
		for (int32 S = 0; S < Segs; ++S)
		{
			const int32 I0 = BaseIdx + S * 2;
			const int32 I2 = I0 + 2;
			const int32 J0 = I0 + 1;
			const int32 J2 = I0 + 3;
			M.Tri(BotC, I2, I0);
			M.Tri(TopC, J0, J2);
		}
	}

	void FillCone(FProtoMesh& M, const FVector& Base, float Radius, float Height, int32 Segs, const FLinearColor& Color)
	{
		Segs = FMath::Clamp(Segs, 6, 18);
		const FVector Apex = Base + FVector(0.f, 0.f, Height);
		const int32 ApexI = M.Add(Apex, FVector::UpVector, Color);
		const int32 BaseC = M.Add(Base, FVector::DownVector, Color);
		TArray<int32, TInlineAllocator<32>> Ring;
		Ring.Reserve(Segs + 1);
		for (int32 S = 0; S <= Segs; ++S)
		{
			const float U = float(S) / float(Segs);
			const float Th = 2.f * PI * U;
			const FVector N(FMath::Cos(Th), FMath::Sin(Th), 0.35f);
			const FVector P = Base + FVector(FMath::Cos(Th) * Radius, FMath::Sin(Th) * Radius, 0.f);
			Ring.Add(M.Add(P, N.GetSafeNormal(), Color));
		}
		for (int32 S = 0; S < Segs; ++S)
		{
			M.Tri(ApexI, Ring[S], Ring[S + 1]);
			M.Tri(BaseC, Ring[S + 1], Ring[S]);
		}
	}

	void FillBlade(FProtoMesh& M, const FVector& Base, float Width, float Height, float YawRad, const FLinearColor& Color)
	{
		const float C = FMath::Cos(YawRad);
		const float S = FMath::Sin(YawRad);
		const FVector Right(C * Width * 0.5f, S * Width * 0.5f, 0.f);
		const FVector N(-S, C, 0.f);
		const FVector P0 = Base - Right;
		const FVector P1 = Base + Right;
		const FVector P2 = Base + Right + FVector(0.f, 0.f, Height);
		const FVector P3 = Base - Right + FVector(0.f, 0.f, Height);
		const int32 I0 = M.Add(P0, N, Color);
		const int32 I1 = M.Add(P1, N, Color);
		const int32 I2 = M.Add(P2, N, Color);
		const int32 I3 = M.Add(P3, N, Color);
		M.Tri(I0, I1, I2);
		M.Tri(I0, I2, I3);
		// Back face
		const FVector Nb = -N;
		const int32 J0 = M.Add(P0, Nb, Color);
		const int32 J1 = M.Add(P1, Nb, Color);
		const int32 J2 = M.Add(P2, Nb, Color);
		const int32 J3 = M.Add(P3, Nb, Color);
		M.Tri(J0, J2, J1);
		M.Tri(J0, J3, J2);
	}

	UStaticMesh* BuildFromProto(FName Name, FProtoMesh& Proto, UMaterialInterface* Material)
	{
		if (Proto.Verts.Num() < 3 || Proto.Indices.Num() < 3)
		{
			return nullptr;
		}

		FMeshDescription MeshDesc;
		FStaticMeshAttributes Attributes(MeshDesc);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		Attributes.GetVertexInstanceUVs().SetNumChannels(1);

		const FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
		PolygonGroupMaterialSlotNames[PolyGroup] = FName(TEXT("Foliage"));

		TArray<FVertexID> VertIds;
		VertIds.SetNum(Proto.Verts.Num());
		for (int32 I = 0; I < Proto.Verts.Num(); ++I)
		{
			VertIds[I] = MeshDesc.CreateVertex();
			VertexPositions[VertIds[I]] = FVector3f(Proto.Verts[I].P);
		}

		for (int32 T = 0; T + 2 < Proto.Indices.Num(); T += 3)
		{
			const int32 IA = Proto.Indices[T];
			const int32 IB = Proto.Indices[T + 1];
			const int32 IC = Proto.Indices[T + 2];
			if (!Proto.Verts.IsValidIndex(IA) || !Proto.Verts.IsValidIndex(IB) || !Proto.Verts.IsValidIndex(IC))
			{
				continue;
			}

			TArray<FVertexInstanceID, TInlineAllocator<3>> Inst;
			const int32 Ids[3] = { IA, IB, IC };
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 Src = Ids[K];
				const FVertexInstanceID VI = MeshDesc.CreateVertexInstance(VertIds[Src]);
				VertexInstanceNormals[VI] = FVector3f(Proto.Verts[Src].N);
				const FLinearColor& C = Proto.Verts[Src].C;
				VertexInstanceColors[VI] = FVector4f(C.R, C.G, C.B, C.A);
				VertexInstanceUVs.Set(VI, 0, FVector2f(0.f, 0.f));
				Inst.Add(VI);
			}
			MeshDesc.CreatePolygon(PolyGroup, Inst);
		}

		UStaticMesh* SM = NewObject<UStaticMesh>(
			GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), Name),
			RF_Public | RF_Transient);
		if (!SM) return nullptr;

		if (Material)
		{
			SM->SetStaticMaterials({ FStaticMaterial(Material, TEXT("Foliage"), TEXT("Foliage")) });
		}

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bBuildSimpleCollision = false;
		Params.bAllowCpuAccess = false;
		Params.bFastBuild = true;
		Params.bCommitMeshDescription = false;

		TArray<const FMeshDescription*> Descs;
		Descs.Add(&MeshDesc);
		if (!SM->BuildFromMeshDescriptions(Descs, Params))
		{
			UE_LOG(LogSailSim, Warning, TEXT("FoliagePrototype: BuildFromMeshDescriptions failed for %s"), *Name.ToString());
			return nullptr;
		}

		UE_LOG(LogSailSim, Log, TEXT("FoliagePrototype: built %s verts=%d tris=%d"),
			*Name.ToString(), Proto.Verts.Num(), Proto.Indices.Num() / 3);
		return SM;
	}

	// Vertex colors: green-dominant so M_NavtVertexColor season path treats as foliage.
	const FLinearColor TrunkBrown(0.22f, 0.16f, 0.10f, 1.f);
	const FLinearColor OakGreen(0.18f, 0.38f, 0.12f, 1.f);
	const FLinearColor CedarGreen(0.14f, 0.32f, 0.14f, 1.f);
	const FLinearColor ScrubGreen(0.22f, 0.36f, 0.14f, 1.f);
	const FLinearColor LawnGreen(0.20f, 0.42f, 0.12f, 1.f);
	const FLinearColor BeachGreen(0.34f, 0.36f, 0.14f, 1.f);
	const FLinearColor MarshGreen(0.18f, 0.30f, 0.14f, 1.f);
}

UStaticMesh* FFoliagePrototypeFactory::TryLoadContentOverride(const FString& Species)
{
	const FString Cap = Species.Left(1).ToUpper() + Species.Mid(1).ToLower();
	const FString Paths[] = {
		FString::Printf(TEXT("/Game/Foliage/SM_%s.SM_%s"), *Cap, *Cap),
		FString::Printf(TEXT("/Game/Foliage/SM_%s.SM_%s"), *Species, *Species),
		FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Species, *Species),
	};
	for (const FString& Path : Paths)
	{
		if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, *Path))
		{
			UE_LOG(LogSailSim, Log, TEXT("FoliagePrototype: content override %s → %s"), *Species, *Path);
			return M;
		}
	}
	return nullptr;
}

UStaticMesh* FFoliagePrototypeFactory::BuildOak(UMaterialInterface* Material)
{
	FProtoMesh M;
	// Trunk ~2.4 m, crown ~3.5 m radius-ish
	FillCylinder(M, FVector(0.f, 0.f, 0.f), 18.f, 280.f, 8, TrunkBrown);
	FillEllipsoid(M, FVector(0.f, 0.f, 360.f), FVector(220.f, 220.f, 180.f), 12, 8, OakGreen);
	FillEllipsoid(M, FVector(90.f, 40.f, 320.f), FVector(120.f, 110.f, 100.f), 10, 6, OakGreen * 1.05f);
	FillEllipsoid(M, FVector(-70.f, -50.f, 340.f), FVector(100.f, 110.f, 90.f), 10, 6, OakGreen * 0.95f);
	return BuildFromProto(TEXT("SM_Foliage_Oak"), M, Material);
}

UStaticMesh* FFoliagePrototypeFactory::BuildCedar(UMaterialInterface* Material)
{
	FProtoMesh M;
	FillCylinder(M, FVector(0.f, 0.f, 0.f), 14.f, 200.f, 7, TrunkBrown);
	// Layered cones → pitch-pine / cedar silhouette
	FillCone(M, FVector(0.f, 0.f, 160.f), 160.f, 140.f, 10, CedarGreen);
	FillCone(M, FVector(0.f, 0.f, 260.f), 120.f, 130.f, 10, CedarGreen * 1.04f);
	FillCone(M, FVector(0.f, 0.f, 360.f), 80.f, 150.f, 10, CedarGreen * 0.96f);
	FillCone(M, FVector(0.f, 0.f, 460.f), 40.f, 120.f, 8, CedarGreen);
	return BuildFromProto(TEXT("SM_Foliage_Cedar"), M, Material);
}

UStaticMesh* FFoliagePrototypeFactory::BuildScrub(UMaterialInterface* Material)
{
	FProtoMesh M;
	FillEllipsoid(M, FVector(0.f, 0.f, 70.f), FVector(110.f, 100.f, 70.f), 10, 6, ScrubGreen);
	FillEllipsoid(M, FVector(40.f, -30.f, 50.f), FVector(70.f, 65.f, 45.f), 8, 5, ScrubGreen * 0.95f);
	return BuildFromProto(TEXT("SM_Foliage_Scrub"), M, Material);
}

UStaticMesh* FFoliagePrototypeFactory::BuildGrassClump(UMaterialInterface* Material, float HeightCm, float WidthCm, int32 Blades)
{
	FProtoMesh M;
	const FLinearColor Col = HeightCm > 120.f ? BeachGreen : (HeightCm > 70.f ? MarshGreen : LawnGreen);
	Blades = FMath::Clamp(Blades, 3, 10);
	for (int32 I = 0; I < Blades; ++I)
	{
		const float Yaw = (float(I) / float(Blades)) * PI + 0.17f * I;
		const float Ox = FMath::Cos(Yaw * 2.f) * WidthCm * 0.15f;
		const float Oy = FMath::Sin(Yaw * 2.f) * WidthCm * 0.15f;
		const float H = HeightCm * (0.75f + 0.08f * (I % 4));
		const float W = WidthCm * (0.55f + 0.05f * (I % 3));
		FillBlade(M, FVector(Ox, Oy, 0.f), W, H, Yaw, Col);
	}
	const FName Name = HeightCm > 120.f ? TEXT("SM_Foliage_Beach")
		: (HeightCm > 70.f ? TEXT("SM_Foliage_Marsh") : TEXT("SM_Foliage_Lawn"));
	return BuildFromProto(Name, M, Material);
}

UStaticMesh* FFoliagePrototypeFactory::ResolveOrBuild(const FString& Species, UMaterialInterface* Material)
{
	if (UStaticMesh* Override = TryLoadContentOverride(Species))
	{
		return Override;
	}

	const FString Key = Species.ToLower();
	if (Key == TEXT("oak")) return BuildOak(Material);
	if (Key == TEXT("cedar")) return BuildCedar(Material);
	if (Key == TEXT("scrub")) return BuildScrub(Material);
	if (Key == TEXT("beach")) return BuildGrassClump(Material, 160.f, 45.f, 6);
	if (Key == TEXT("marsh")) return BuildGrassClump(Material, 90.f, 40.f, 5);
	if (Key == TEXT("lawn")) return BuildGrassClump(Material, 45.f, 35.f, 7);
	// Unknown → scrub
	return BuildScrub(Material);
}
