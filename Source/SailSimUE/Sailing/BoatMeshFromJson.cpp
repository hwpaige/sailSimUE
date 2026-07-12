#include "Sailing/BoatMeshFromJson.h"
#include "ProceduralMeshComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace BoatMeshJsonPrivate
{
	static float Num(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, float Fallback)
	{
		double V = Fallback;
		if (Obj.IsValid() && Obj->TryGetNumberField(Key, V))
		{
			return static_cast<float>(V);
		}
		return Fallback;
	}

	static bool ReadVec3(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FVector& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 3)
		{
			return false;
		}
		Out = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	static FString ResolvePath(const FString& JsonPathOrContentRelative)
	{
		FString Path = JsonPathOrContentRelative;
		if (!FPaths::FileExists(Path))
		{
			Path = FPaths::ProjectContentDir() / JsonPathOrContentRelative;
		}
		return Path;
	}

	static bool ParseRoot(
		const FString& Path,
		TSharedPtr<FJsonObject>& OutRoot)
	{
		if (!FPaths::FileExists(Path))
		{
			UE_LOG(LogTemp, Error, TEXT("BoatMeshFromJson: missing %s"), *Path);
			return false;
		}
		FString JsonStr;
		if (!FFileHelper::LoadFileToString(JsonStr, *Path))
		{
			UE_LOG(LogTemp, Error, TEXT("BoatMeshFromJson: failed to read %s"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("BoatMeshFromJson: JSON parse failed"));
			return false;
		}
		return true;
	}

	static void FillSailing(const TSharedPtr<FJsonObject>& Root, FBoatJsonSailingParams& OutSailing)
	{
		OutSailing = FBoatJsonSailingParams();
		const TSharedPtr<FJsonObject>* SailObj = nullptr;
		const TSharedPtr<FJsonObject>* DimsObj = nullptr;
		if (Root->TryGetObjectField(TEXT("sailing"), SailObj) && SailObj && (*SailObj).IsValid())
		{
			OutSailing.bValid = true;
			OutSailing.DispLb = Num(*SailObj, TEXT("disp_lb"), OutSailing.DispLb);
			OutSailing.BallastLb = Num(*SailObj, TEXT("ballast_lb"), OutSailing.BallastLb);
			OutSailing.BeamFt = Num(*SailObj, TEXT("beam"), OutSailing.BeamFt);
			OutSailing.LwlFt = Num(*SailObj, TEXT("lwl"), OutSailing.LwlFt);
			OutSailing.DraftFt = Num(*SailObj, TEXT("draft"), OutSailing.DraftFt);
			OutSailing.TcFt = Num(*SailObj, TEXT("Tc"), OutSailing.TcFt);
			OutSailing.LateralArea = Num(*SailObj, TEXT("lateral_area"), OutSailing.LateralArea);
			OutSailing.KeelArea = Num(*SailObj, TEXT("keel_area"), OutSailing.KeelArea);
			OutSailing.RudderArea = Num(*SailObj, TEXT("rudder_area"), OutSailing.RudderArea);
			OutSailing.KeelSpan = Num(*SailObj, TEXT("keel_span"), OutSailing.KeelSpan);
			OutSailing.SaTotal = Num(*SailObj, TEXT("sa_total"), OutSailing.SaTotal);
			OutSailing.HullSpeedKn = Num(*SailObj, TEXT("hull_speed_kn"), OutSailing.HullSpeedKn);
			OutSailing.GmFt = Num(*SailObj, TEXT("gm"), OutSailing.GmFt);
			const TArray<TSharedPtr<FJsonValue>>* Clr = nullptr;
			if ((*SailObj)->TryGetArrayField(TEXT("clr"), Clr) && Clr && Clr->Num() >= 2)
			{
				OutSailing.ClrX = (*Clr)[0]->AsNumber();
				OutSailing.ClrZ = (*Clr)[1]->AsNumber();
			}
		}
		if (Root->TryGetObjectField(TEXT("dims_ft"), DimsObj) && DimsObj && (*DimsObj).IsValid())
		{
			OutSailing.LoaFt = Num(*DimsObj, TEXT("loa"), OutSailing.LoaFt);
			OutSailing.MastTopFt = Num(*DimsObj, TEXT("mast_top"), OutSailing.MastTopFt);
			if (!OutSailing.bValid)
			{
				OutSailing.bValid = true;
			}
		}
	}

	static void FillSpars(const TSharedPtr<FJsonObject>& Root, FBoatJsonSparEndpoints& OutSpars)
	{
		OutSpars = FBoatJsonSparEndpoints();
		const TSharedPtr<FJsonObject>* Spars = nullptr;
		if (!Root->TryGetObjectField(TEXT("spars"), Spars) || !Spars || !(*Spars).IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>* Mast = nullptr;
		const TSharedPtr<FJsonObject>* Boom = nullptr;
		if ((*Spars)->TryGetObjectField(TEXT("mast"), Mast) && Mast && (*Mast).IsValid())
		{
			OutSpars.bMastValid = ReadVec3(*Mast, TEXT("base"), OutSpars.MastBase)
				&& ReadVec3(*Mast, TEXT("top"), OutSpars.MastTop);
		}
		if ((*Spars)->TryGetObjectField(TEXT("boom"), Boom) && Boom && (*Boom).IsValid())
		{
			OutSpars.bBoomValid = ReadVec3(*Boom, TEXT("base"), OutSpars.BoomBase)
				&& ReadVec3(*Boom, TEXT("end"), OutSpars.BoomEnd);
		}
	}
}

FString FBoatMeshFromJson::DefaultJ105Path()
{
	return FPaths::ProjectContentDir() / TEXT("Data/j105_boat3d.json");
}

bool FBoatMeshFromJson::LoadMetadataOnly(
	const FString& JsonPathOrContentRelative,
	FBoatJsonLoadResult& OutResult)
{
	OutResult = FBoatJsonLoadResult();
	const FString Path = BoatMeshJsonPrivate::ResolvePath(JsonPathOrContentRelative);
	OutResult.ResolvedPath = Path;
	TSharedPtr<FJsonObject> Root;
	if (!BoatMeshJsonPrivate::ParseRoot(Path, Root))
	{
		return false;
	}
	BoatMeshJsonPrivate::FillSailing(Root, OutResult.Sailing);
	BoatMeshJsonPrivate::FillSpars(Root, OutResult.Spars);
	OutResult.bOk = true;
	return true;
}

bool FBoatMeshFromJson::LoadIntoProceduralMesh(
	UProceduralMeshComponent* Mesh,
	const FString& JsonPathOrContentRelative,
	UMaterialInterface* DefaultMaterial,
	FBoatJsonLoadResult* OutResult)
{
	if (OutResult)
	{
		*OutResult = FBoatJsonLoadResult();
	}
	if (!Mesh)
	{
		return false;
	}

	const FString Path = BoatMeshJsonPrivate::ResolvePath(JsonPathOrContentRelative);
	if (OutResult)
	{
		OutResult->ResolvedPath = Path;
	}

	TSharedPtr<FJsonObject> Root;
	if (!BoatMeshJsonPrivate::ParseRoot(Path, Root))
	{
		return false;
	}

	FBoatJsonSailingParams Sailing;
	FBoatJsonSparEndpoints Spars;
	BoatMeshJsonPrivate::FillSailing(Root, Sailing);
	BoatMeshJsonPrivate::FillSpars(Root, Spars);

	const TArray<TSharedPtr<FJsonValue>>* MeshArr = nullptr;
	if (!Root->TryGetArrayField(TEXT("meshes"), MeshArr) || !MeshArr)
	{
		return false;
	}

	UMaterialInterface* BaseMat = DefaultMaterial;
	Mesh->ClearAllMeshSections();
	Mesh->bUseComplexAsSimpleCollision = true;
	Mesh->SetCastShadow(true);
	Mesh->SetVisibility(true);
	Mesh->SetHiddenInGame(false);

	int32 Section = 0;
	for (const TSharedPtr<FJsonValue>& Val : *MeshArr)
	{
		const TSharedPtr<FJsonObject> M = Val->AsObject();
		if (!M.IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>>* VertsJ = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* IdxJ = nullptr;
		if (!M->TryGetArrayField(TEXT("verts"), VertsJ) || !M->TryGetArrayField(TEXT("indices"), IdxJ))
		{
			continue;
		}
		if (VertsJ->Num() < 9 || IdxJ->Num() < 3) continue;

		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FProcMeshTangent> Tangents;
		TArray<FLinearColor> Colors;

		Vertices.Reserve(VertsJ->Num() / 3);
		for (int32 I = 0; I + 2 < VertsJ->Num(); I += 3)
		{
			Vertices.Add(FVector(
				(*VertsJ)[I]->AsNumber(),
				(*VertsJ)[I + 1]->AsNumber(),
				(*VertsJ)[I + 2]->AsNumber()));
		}

		Triangles.Reserve(IdxJ->Num());
		for (const TSharedPtr<FJsonValue>& Iv : *IdxJ)
		{
			Triangles.Add(static_cast<int32>(Iv->AsNumber()));
		}

		// Normals from front faces only (before reverse winding) so double-sided
		// back faces do not cancel vertex normals to zero.
		const int32 FrontTriCount = Triangles.Num();
		Normals.Init(FVector::ZeroVector, Vertices.Num());
		for (int32 T = 0; T + 2 < FrontTriCount; T += 3)
		{
			const int32 Ia = Triangles[T], Ib = Triangles[T + 1], Ic = Triangles[T + 2];
			if (!Vertices.IsValidIndex(Ia) || !Vertices.IsValidIndex(Ib) || !Vertices.IsValidIndex(Ic))
			{
				continue;
			}
			const FVector N = FVector::CrossProduct(
				Vertices[Ib] - Vertices[Ia], Vertices[Ic] - Vertices[Ia]).GetSafeNormal();
			Normals[Ia] += N;
			Normals[Ib] += N;
			Normals[Ic] += N;
		}
		for (FVector& N : Normals)
		{
			if (!N.Normalize())
			{
				N = FVector::UpVector;
			}
		}

		FString SectionName;
		M->TryGetStringField(TEXT("name"), SectionName);
		const bool bIsSail = SectionName.Contains(TEXT("sail"), ESearchCase::IgnoreCase);
		const bool bIsKeel = SectionName.Contains(TEXT("keel"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("rudder"), ESearchCase::IgnoreCase);
		const bool bIsWindow = SectionName.Contains(TEXT("window"), ESearchCase::IgnoreCase);
		const bool bIsHull = SectionName.Equals(TEXT("hull"), ESearchCase::IgnoreCase);

		// The hull is exported single-sided AND inward-wound (2968/2970 tris face inward):
		// its normals point into the hull, so the lit exterior renders dark. Fix is two parts —
		// draw double-sided for robustness (here) + flip its normals outward (below).
		const bool bDouble = bIsHull
			|| (M->HasField(TEXT("double_sided")) && M->GetBoolField(TEXT("double_sided")));
		if (bDouble)
		{
			// Copy indices first — TArray::Add(Array[i]) asserts in UE when the
			// element address is from the same container being modified.
			Triangles.Reserve(FrontTriCount * 2);
			TArray<int32> Back;
			Back.Reserve(FrontTriCount);
			for (int32 T = 0; T + 2 < FrontTriCount; T += 3)
			{
				const int32 Ia = Triangles[T];
				const int32 Ib = Triangles[T + 1];
				const int32 Ic = Triangles[T + 2];
				Back.Add(Ia);
				Back.Add(Ic);
				Back.Add(Ib);
			}
			Triangles.Append(Back);
		}

		// Hull normals point inward (see above) -> flip them so the lit exterior reads correctly.
		if (bIsHull)
		{
			for (FVector& N : Normals)
			{
				N = -N;
			}
		}

		FLinearColor Col(0.95f, 0.96f, 0.98f, 1.f);
		const TArray<TSharedPtr<FJsonValue>>* ColJ = nullptr;
		if (M->TryGetArrayField(TEXT("color"), ColJ) && ColJ->Num() >= 3)
		{
			const float Boost = bIsSail ? 1.05f : (bIsKeel || bIsWindow ? 1.0f : 1.25f);
			Col = FLinearColor(
				FMath::Clamp(static_cast<float>((*ColJ)[0]->AsNumber()) * Boost, 0.f, 1.f),
				FMath::Clamp(static_cast<float>((*ColJ)[1]->AsNumber()) * Boost, 0.f, 1.f),
				FMath::Clamp(static_cast<float>((*ColJ)[2]->AsNumber()) * Boost, 0.f, 1.f),
				ColJ->Num() > 3 ? static_cast<float>((*ColJ)[3]->AsNumber()) : 1.f);
		}
		if (bIsSail)
		{
			Col = FLinearColor(0.98f, 0.94f, 0.82f, 1.f);
		}
		else if (bIsHull)
		{
			Col = FLinearColor(0.92f, 0.94f, 0.98f, 1.f);
		}
		else if (SectionName.Equals(TEXT("deck"), ESearchCase::IgnoreCase)
			|| SectionName.Equals(TEXT("cockpit"), ESearchCase::IgnoreCase))
		{
			Col = FLinearColor(0.78f, 0.80f, 0.84f, 1.f);
		}

		UV0.Init(FVector2D(0.5f, 0.5f), Vertices.Num());
		for (int32 Vi = 0; Vi < Vertices.Num(); ++Vi)
		{
			UV0[Vi] = FVector2D(Vertices[Vi].X * 0.001f, Vertices[Vi].Y * 0.001f);
		}
		Colors.Init(Col, Vertices.Num());
		Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), Vertices.Num());

		// Collision on hull only (not sails / windows)
		const bool bCollision = bIsHull || (Section == 0 && SectionName.IsEmpty());
		Mesh->CreateMeshSection_LinearColor(
			Section, Vertices, Triangles, Normals, UV0, Colors, Tangents, bCollision);

		if (BaseMat)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMat, Mesh);
			if (Mid)
			{
				// BasicShapeMaterial primarily uses Color
				Mid->SetVectorParameterValue(TEXT("Color"), Col);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Col);
				Mesh->SetMaterial(Section, Mid);
			}
		}

		++Section;
	}

	if (OutResult)
	{
		OutResult->bOk = Section > 0;
		OutResult->SectionCount = Section;
		OutResult->Sailing = Sailing;
		OutResult->Spars = Spars;
		OutResult->ResolvedPath = Path;
	}

	UE_LOG(LogTemp, Log, TEXT("BoatMeshFromJson: loaded %d sections from %s"), Section, *Path);
	return Section > 0;
}
