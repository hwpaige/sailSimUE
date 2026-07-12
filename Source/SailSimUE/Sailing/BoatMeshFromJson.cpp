#include "Sailing/BoatMeshFromJson.h"
#include "ProceduralMeshComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

FString FBoatMeshFromJson::DefaultJ105Path()
{
	return FPaths::ProjectContentDir() / TEXT("Data/j105_boat3d.json");
}

bool FBoatMeshFromJson::LoadIntoProceduralMesh(
	UProceduralMeshComponent* Mesh,
	const FString& JsonPathOrContentRelative,
	UMaterialInterface* DefaultMaterial)
{
	if (!Mesh)
	{
		return false;
	}

	FString Path = JsonPathOrContentRelative;
	if (!FPaths::FileExists(Path))
	{
		Path = FPaths::ProjectContentDir() / JsonPathOrContentRelative;
	}
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

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("BoatMeshFromJson: JSON parse failed"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* MeshArr = nullptr;
	if (!Root->TryGetArrayField(TEXT("meshes"), MeshArr) || !MeshArr)
	{
		return false;
	}

	UMaterialInterface* BaseMat = DefaultMaterial;

	Mesh->ClearAllMeshSections();
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
			const float X = (*VertsJ)[I]->AsNumber();
			const float Y = (*VertsJ)[I + 1]->AsNumber();
			const float Z = (*VertsJ)[I + 2]->AsNumber();
			Vertices.Add(FVector(X, Y, Z));
		}

		Triangles.Reserve(IdxJ->Num());
		for (const TSharedPtr<FJsonValue>& Iv : *IdxJ)
		{
			Triangles.Add(static_cast<int32>(Iv->AsNumber()));
		}

		// Flat normals (ProceduralMesh will recompute if empty? we compute simple)
		Normals.Init(FVector::UpVector, Vertices.Num());
		for (int32 T = 0; T + 2 < Triangles.Num(); T += 3)
		{
			const int32 Ia = Triangles[T], Ib = Triangles[T + 1], Ic = Triangles[T + 2];
			if (!Vertices.IsValidIndex(Ia) || !Vertices.IsValidIndex(Ib) || !Vertices.IsValidIndex(Ic))
			{
				continue;
			}
			const FVector N = FVector::CrossProduct(
				Vertices[Ib] - Vertices[Ia], Vertices[Ic] - Vertices[Ia]).GetSafeNormal();
			Normals[Ia] = N;
			Normals[Ib] = N;
			Normals[Ic] = N;
		}

		UV0.Init(FVector2D::ZeroVector, Vertices.Num());
		Colors.Init(FLinearColor::White, Vertices.Num());
		Tangents.Init(FProcMeshTangent(1.f, 0.f, 0.f), Vertices.Num());

		Mesh->CreateMeshSection_LinearColor(
			Section, Vertices, Triangles, Normals, UV0, Colors, Tangents, true);

		// Color material instance
		FLinearColor Col(0.9f, 0.9f, 0.9f, 1.f);
		const TArray<TSharedPtr<FJsonValue>>* ColJ = nullptr;
		if (M->TryGetArrayField(TEXT("color"), ColJ) && ColJ->Num() >= 3)
		{
			Col = FLinearColor(
				(*ColJ)[0]->AsNumber(),
				(*ColJ)[1]->AsNumber(),
				(*ColJ)[2]->AsNumber(),
				ColJ->Num() > 3 ? (*ColJ)[3]->AsNumber() : 1.0);
		}
		if (BaseMat)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(BaseMat, Mesh);
			if (Mid)
			{
				Mid->SetVectorParameterValue(TEXT("Color"), Col);
				Mid->SetVectorParameterValue(TEXT("BaseColor"), Col);
				Mesh->SetMaterial(Section, Mid);
			}
		}

		const bool bDouble = M->GetBoolField(TEXT("double_sided"));
		// Procedural mesh doesn't expose double-sided easily without mat; leave default

		++Section;
	}

	// Spars as simple cylinders via two-point boxes (optional — skip if missing)
	// Mast/boom handled by optional static cylinders in pawn if needed.

	UE_LOG(LogTemp, Log, TEXT("BoatMeshFromJson: loaded %d sections from %s"), Section, *Path);
	return Section > 0;
}
