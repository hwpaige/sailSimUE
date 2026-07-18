#include "Sailing/BoatMeshFromJson.h"
#include "SailSimUE.h"
#include "ProceduralMeshComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/Package.h"

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
			UE_LOG(LogSailSim, Error, TEXT("BoatMeshFromJson: missing %s"), *Path);
			return false;
		}
		FString JsonStr;
		if (!FFileHelper::LoadFileToString(JsonStr, *Path))
		{
			UE_LOG(LogSailSim, Error, TEXT("BoatMeshFromJson: failed to read %s"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			UE_LOG(LogSailSim, Error, TEXT("BoatMeshFromJson: JSON parse failed"));
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

	static void FillRigging(const TSharedPtr<FJsonObject>& Root, FBoatJsonRunningRigging& Out)
	{
		Out = FBoatJsonRunningRigging();
		const TSharedPtr<FJsonObject>* Jib = nullptr;
		if (Root->TryGetObjectField(TEXT("jibsheet"), Jib) && Jib && (*Jib).IsValid())
		{
			Out.bJibTracksValid =
				ReadVec3(*Jib, TEXT("port_track_fwd"), Out.JibPortTrackFwd)
				&& ReadVec3(*Jib, TEXT("port_track_aft"), Out.JibPortTrackAft)
				&& ReadVec3(*Jib, TEXT("stbd_track_fwd"), Out.JibStbdTrackFwd)
				&& ReadVec3(*Jib, TEXT("stbd_track_aft"), Out.JibStbdTrackAft);
			// sail_geom labels +Y as "port"; UE uses +Y = starboard. If the loaded
			// "port" rail sits on +Y, swap so Port = −Y and Stbd = +Y.
			if (Out.bJibTracksValid)
			{
				const float PortY = 0.5f * (Out.JibPortTrackFwd.Y + Out.JibPortTrackAft.Y);
				const float StbdY = 0.5f * (Out.JibStbdTrackFwd.Y + Out.JibStbdTrackAft.Y);
				if (PortY > StbdY)
				{
					Swap(Out.JibPortTrackFwd, Out.JibStbdTrackFwd);
					Swap(Out.JibPortTrackAft, Out.JibStbdTrackAft);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Main = nullptr;
		if (Root->TryGetObjectField(TEXT("mainsheet"), Main) && Main && (*Main).IsValid())
		{
			Out.bMainsheetValid =
				ReadVec3(*Main, TEXT("gooseneck"), Out.MainsheetGooseneck)
				&& ReadVec3(*Main, TEXT("lead"), Out.MainsheetLead);
			double BoomLen = 0.0;
			if ((*Main)->TryGetNumberField(TEXT("boom_length"), BoomLen))
			{
				Out.BoomLengthCm = static_cast<float>(BoomLen);
			}
			double VDrop = 0.0, VBoom = 0.0;
			if ((*Main)->TryGetNumberField(TEXT("vang_mast_drop_frac"), VDrop))
			{
				Out.VangMastDropFrac = static_cast<float>(VDrop);
			}
			if ((*Main)->TryGetNumberField(TEXT("vang_boom_frac"), VBoom))
			{
				Out.VangBoomFrac = static_cast<float>(VBoom);
			}
		}
		// Sheet terminals: jib primaries + spin quarter blocks + cabin winches.
		const TSharedPtr<FJsonObject>* SL = nullptr;
		if (Root->TryGetObjectField(TEXT("sheet_leads"), SL) && SL && (*SL).IsValid())
		{
			const bool bJ =
				ReadVec3(*SL, TEXT("jib_port_winch"), Out.JibPortWinch)
				&& ReadVec3(*SL, TEXT("jib_stbd_winch"), Out.JibStbdWinch);
			const bool bS =
				ReadVec3(*SL, TEXT("spin_port_block"), Out.SpinPortBlock)
				&& ReadVec3(*SL, TEXT("spin_stbd_block"), Out.SpinStbdBlock)
				&& ReadVec3(*SL, TEXT("spin_port_winch"), Out.SpinPortWinch)
				&& ReadVec3(*SL, TEXT("spin_stbd_winch"), Out.SpinStbdWinch);
			Out.bSheetLeadsValid = bJ || bS;
			// Same port/stbd Y convention as tracks: sail_geom +Y = "port"; UE +Y = stbd.
			if (Out.bSheetLeadsValid)
			{
				if (Out.JibPortWinch.Y > Out.JibStbdWinch.Y)
				{
					Swap(Out.JibPortWinch, Out.JibStbdWinch);
				}
				if (Out.SpinPortBlock.Y > Out.SpinStbdBlock.Y)
				{
					Swap(Out.SpinPortBlock, Out.SpinStbdBlock);
					Swap(Out.SpinPortWinch, Out.SpinStbdWinch);
				}
			}
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
	BoatMeshJsonPrivate::FillRigging(Root, OutResult.Rigging);
	OutResult.bOk = true;
	return true;
}

void FBoatMeshFromJson::CopyProceduralMesh(UProceduralMeshComponent* Src, UProceduralMeshComponent* Dst)
{
	if (!Src || !Dst || Src == Dst) return;
	Dst->ClearAllMeshSections();
	Dst->bUseComplexAsSimpleCollision = false;
	Dst->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Dst->SetCastShadow(true);
	Dst->SetVisibility(true);
	Dst->SetHiddenInGame(false);
	const int32 Num = Src->GetNumSections();
	for (int32 Si = 0; Si < Num; ++Si)
	{
		FProcMeshSection* Sec = Src->GetProcMeshSection(Si);
		if (!Sec || Sec->ProcVertexBuffer.Num() == 0) continue;

		TArray<FVector> Pos;
		TArray<FVector> Norm;
		TArray<FVector2D> UV;
		TArray<FLinearColor> Col;
		TArray<FProcMeshTangent> Tan;
		TArray<int32> Idx;
		Pos.Reserve(Sec->ProcVertexBuffer.Num());
		Norm.Reserve(Sec->ProcVertexBuffer.Num());
		UV.Reserve(Sec->ProcVertexBuffer.Num());
		Col.Reserve(Sec->ProcVertexBuffer.Num());
		Tan.Reserve(Sec->ProcVertexBuffer.Num());
		for (const FProcMeshVertex& V : Sec->ProcVertexBuffer)
		{
			Pos.Add(V.Position);
			Norm.Add(V.Normal);
			UV.Add(V.UV0);
			Col.Add(V.Color);
			Tan.Add(V.Tangent);
		}
		Idx.Reserve(Sec->ProcIndexBuffer.Num());
		for (uint32 I : Sec->ProcIndexBuffer)
		{
			Idx.Add(static_cast<int32>(I));
		}
		Dst->CreateMeshSection_LinearColor(
			Si, Pos, Idx, Norm, UV, Col, Tan, /*bCreateCollision*/ false);
		if (UMaterialInterface* Mat = Src->GetMaterial(Si))
		{
			Dst->SetMaterial(Si, Mat);
		}
	}
	Dst->MarkRenderStateDirty();
}

bool FBoatMeshFromJson::LoadIntoProceduralMesh(
	UProceduralMeshComponent* HullOrCombinedMesh,
	const FString& JsonPathOrContentRelative,
	UMaterialInterface* DefaultMaterial,
	FBoatJsonLoadResult* OutResult,
	UProceduralMeshComponent* MainSailMesh,
	UProceduralMeshComponent* JibSailMesh,
	bool bSkipSailSections,
	UProceduralMeshComponent* AppendagesMesh)
{
	if (OutResult)
	{
		*OutResult = FBoatJsonLoadResult();
	}
	if (!HullOrCombinedMesh)
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
	FBoatJsonRunningRigging Rigging;
	BoatMeshJsonPrivate::FillSailing(Root, Sailing);
	BoatMeshJsonPrivate::FillSpars(Root, Spars);
	BoatMeshJsonPrivate::FillRigging(Root, Rigging);

	const TArray<TSharedPtr<FJsonValue>>* MeshArr = nullptr;
	if (!Root->TryGetArrayField(TEXT("meshes"), MeshArr) || !MeshArr)
	{
		return false;
	}

	UMaterialInterface* BaseMat = DefaultMaterial;
	auto PrepMesh = [](UProceduralMeshComponent* Mesh, bool bCastShadow)
	{
		if (!Mesh) return;
		Mesh->ClearAllMeshSections();
		Mesh->bUseComplexAsSimpleCollision = false;
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// Fully kill every cast path (VSM / contact / static / dynamic) — SetCastShadow(false)
		// alone is not always enough on Mac Metal + VSM.
		Mesh->SetCastShadow(bCastShadow);
		Mesh->bCastContactShadow = bCastShadow;
		Mesh->bCastDynamicShadow = bCastShadow;
		Mesh->bCastStaticShadow = bCastShadow;
		Mesh->bCastVolumetricTranslucentShadow = bCastShadow;
		Mesh->bCastInsetShadow = bCastShadow;
		Mesh->bSelfShadowOnly = false;
		Mesh->SetVisibility(true);
		Mesh->SetHiddenInGame(false);
	};
	PrepMesh(HullOrCombinedMesh, true);
	PrepMesh(MainSailMesh, true);
	PrepMesh(JibSailMesh, true);
	// Underwater fins: visible but never cast (would darken topsides through the water)
	PrepMesh(AppendagesMesh, false);

	const FVector SailPivot = Spars.bMastValid ? Spars.MastBase : FVector::ZeroVector;
	const bool bSplitSails = (MainSailMesh != nullptr) || (JibSailMesh != nullptr);

	int32 HullSection = 0;
	int32 MainSection = 0;
	int32 JibSection = 0;
	int32 AppendageSection = 0;
	int32 TotalSections = 0;
	// Stern rebuild: deck aft edge + hull aft extent → solid reverse-transom plate.
	TArray<FVector> DeckAftEdge; // deck edge samples sorted by Y
	float HullAftX = 0.f;
	float HullAftBottomZ = 0.f;
	bool bHaveHullAft = false;
	bool bHadJsonTransom = false;

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

		FString SectionName;
		M->TryGetStringField(TEXT("name"), SectionName);
		const bool bIsMain = SectionName.Contains(TEXT("sail_main"), ESearchCase::IgnoreCase)
			|| SectionName.Equals(TEXT("main"), ESearchCase::IgnoreCase);
		const bool bIsJib = SectionName.Contains(TEXT("sail_jib"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("jib"), ESearchCase::IgnoreCase);
		const bool bIsSail = SectionName.Contains(TEXT("sail"), ESearchCase::IgnoreCase) || bIsMain || bIsJib;
		const bool bIsAppendage = SectionName.Contains(TEXT("keel"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("rudder"), ESearchCase::IgnoreCase);
		if (bSkipSailSections && bIsSail)
		{
			continue;
		}

		UProceduralMeshComponent* Target = HullOrCombinedMesh;
		int32* SectionCounter = &HullSection;
		bool bPivotToSail = false;
		if (bIsAppendage && AppendagesMesh)
		{
			// Separate mesh, no shadow cast — keeps underwater fins from shading the hull
			Target = AppendagesMesh;
			SectionCounter = &AppendageSection;
		}
		else if (bSplitSails && bIsMain && MainSailMesh)
		{
			Target = MainSailMesh;
			SectionCounter = &MainSection;
			bPivotToSail = true;
		}
		else if (bSplitSails && bIsJib && JibSailMesh)
		{
			Target = JibSailMesh;
			SectionCounter = &JibSection;
			bPivotToSail = true;
		}
		else if (bSplitSails && bIsSail && MainSailMesh)
		{
			// Other sails (spinnaker etc.) → main mesh
			Target = MainSailMesh;
			SectionCounter = &MainSection;
			bPivotToSail = true;
		}

		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FProcMeshTangent> Tangents;
		TArray<FLinearColor> Colors;

		Vertices.Reserve(VertsJ->Num() / 3);
		for (int32 I = 0; I + 2 < VertsJ->Num(); I += 3)
		{
			FVector V(
				(*VertsJ)[I]->AsNumber(),
				(*VertsJ)[I + 1]->AsNumber(),
				(*VertsJ)[I + 2]->AsNumber());
			if (bPivotToSail)
			{
				V -= SailPivot;
			}
			Vertices.Add(V);
		}

		// Pre-classify for stern rebuild samples
		const bool bNameDeck = SectionName.Equals(TEXT("deck"), ESearchCase::IgnoreCase)
			|| (SectionName.Contains(TEXT("deck"), ESearchCase::IgnoreCase)
				&& !SectionName.Contains(TEXT("transom"), ESearchCase::IgnoreCase));
		const bool bNameHull = SectionName.Equals(TEXT("hull"), ESearchCase::IgnoreCase);
		const bool bNameTransom = SectionName.Equals(TEXT("transom"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("transom"), ESearchCase::IgnoreCase);

		// Deck aft edge samples (for reverse-transom top chord)
		if (bNameDeck && Vertices.Num() > 0)
		{
			float MinX = Vertices[0].X;
			for (const FVector& V : Vertices) MinX = FMath::Min(MinX, V.X);
			TMap<int32, FVector> EdgeByYBin;
			for (const FVector& V : Vertices)
			{
				if (V.X > MinX + 8.f) continue;
				const int32 Bin = FMath::RoundToInt(V.Y / 5.f);
				FVector* Existing = EdgeByYBin.Find(Bin);
				if (!Existing || V.X < Existing->X)
				{
					EdgeByYBin.Add(Bin, V);
				}
			}
			DeckAftEdge.Reset();
			EdgeByYBin.KeySort([](int32 A, int32 B) { return A < B; });
			for (const TPair<int32, FVector>& P : EdgeByYBin)
			{
				DeckAftEdge.Add(P.Value);
			}
		}

		// Hull aft extent (bottom of reverse transom)
		if (bNameHull && Vertices.Num() > 0)
		{
			float MinX = Vertices[0].X;
			float MinZ = Vertices[0].Z;
			for (const FVector& V : Vertices)
			{
				MinX = FMath::Min(MinX, V.X);
				MinZ = FMath::Min(MinZ, V.Z);
			}
			HullAftX = MinX;
			// Bottom of immersed reverse face sits near waterline, not keel tip
			HullAftBottomZ = FMath::Max(MinZ, 0.f) + 2.f;
			// Prefer lowest Z among aft-most verts (true stern bottom)
			float SternBottomZ = 1.e6f;
			for (const FVector& V : Vertices)
			{
				if (V.X < MinX + 20.f)
				{
					SternBottomZ = FMath::Min(SternBottomZ, V.Z);
				}
			}
			if (SternBottomZ < 1.e5f)
			{
				HullAftBottomZ = SternBottomZ;
			}
			bHaveHullAft = true;
		}

		// JSON transom is often thin/one-sided and was getting culled or morphed away —
		// skip the source mesh; we rebuild a solid plate after the loop.
		if (bNameTransom)
		{
			bHadJsonTransom = true;
			continue;
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

		const bool bIsKeel = SectionName.Contains(TEXT("keel"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("rudder"), ESearchCase::IgnoreCase);
		const bool bIsWindow = SectionName.Contains(TEXT("window"), ESearchCase::IgnoreCase);
		const bool bIsWinch = SectionName.Contains(TEXT("winch"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("sheet_block"), ESearchCase::IgnoreCase);
		const bool bIsHull = SectionName.Equals(TEXT("hull"), ESearchCase::IgnoreCase);
		const bool bIsDeck = SectionName.Equals(TEXT("deck"), ESearchCase::IgnoreCase)
			|| SectionName.Contains(TEXT("deck"), ESearchCase::IgnoreCase);
		const bool bIsCockpit = SectionName.Equals(TEXT("cockpit"), ESearchCase::IgnoreCase);
		const bool bIsCabin = SectionName.Contains(TEXT("cabin"), ESearchCase::IgnoreCase);
		// Open topsides / thin surfaces that often export with inverted winding.
		const bool bIsTopside = bIsDeck || bIsCockpit || bIsCabin;

		// Average normal (before double-sided index append) — used to flip inverted shells.
		FVector AvgN = FVector::ZeroVector;
		for (const FVector& N : Normals)
		{
			AvgN += N;
		}
		if (!AvgN.Normalize())
		{
			AvgN = FVector::UpVector;
		}

		// IMPORTANT: do NOT duplicate deck/hull/cabin triangles.
		// Coplanar front+back faces z-fight → shimmering / "glitchy" decks and dull hulls.
		// Flip normals / winding instead; use two-sided *materials* only for thin shells
		// that are still visible from both sides (sails, optional cabin).
		// Sails genuinely need dual faces (thin cloth, no thickness).
		const bool bDouble = bIsSail
			|| (M->HasField(TEXT("double_sided")) && M->GetBoolField(TEXT("double_sided"))
				&& !bIsDeck && !bIsCockpit && !bIsHull);
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

		// Orient exterior normals:
		//  - hull: systematically inward in export → flip all
		//  - deck / cockpit: should face +Z (up); j105 deck avgN.Z ≈ −0.8 when inverted
		//  - cabin / hatch: if average faces into the hull volume, flip
		bool bFlipNormals = bIsHull;
		if (bIsDeck || bIsCockpit)
		{
			// Top of boat must face the sky.
			bFlipNormals = (AvgN.Z < 0.f);
		}
		else if (bIsCabin && !bIsWindow)
		{
			// Cabin shell often inverted like hull — flip if mostly pointing down/in.
			bFlipNormals = (AvgN.Z < -0.15f);
		}
		if (bFlipNormals)
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
			const float Boost = bIsSail ? 1.05f
				: (bIsKeel || bIsWindow || bIsWinch ? 1.0f : 1.25f);
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
		else if (bIsWinch)
		{
			// Black alloy ST winch — keep dark, don't boost toward white.
			Col = FLinearColor(0.10f, 0.11f, 0.12f, 1.f);
		}
		else if (bIsHull)
		{
			// Glossy white gelcoat base — per-vert boot/cove stripes painted below.
			Col = FLinearColor(0.96f, 0.97f, 0.99f, 1.f);
		}
		else if (bIsDeck || bIsCockpit || (bIsCabin && !bIsWindow))
		{
			// Deck non-skid white — cabin top matches deck
			Col = FLinearColor(0.97f, 0.97f, 0.98f, 1.f);
		}

		UV0.Init(FVector2D(0.5f, 0.5f), Vertices.Num());
		for (int32 Vi = 0; Vi < Vertices.Num(); ++Vi)
		{
			UV0[Vi] = FVector2D(Vertices[Vi].X * 0.001f, Vertices[Vi].Y * 0.001f);
		}
		// Vertex colors unused for paint — hull uses MI_Yacht_HullPaint (local-Z hard bands).
		// Per-vert stripe paint on this coarse loft (~1.5k verts) looks stair-stepped/pixelated.
		Colors.Init(Col, Vertices.Num());
		// Tangents from normals so lighting/specular doesn't look broken on flipped shells
		Tangents.SetNum(Vertices.Num());
		for (int32 Vi = 0; Vi < Vertices.Num(); ++Vi)
		{
			const FVector Nrm = Normals.IsValidIndex(Vi) ? Normals[Vi] : FVector::UpVector;
			FVector T = FVector::CrossProduct(FVector::UpVector, Nrm).GetSafeNormal();
			if (T.IsNearlyZero())
			{
				T = FVector::CrossProduct(FVector::ForwardVector, Nrm).GetSafeNormal();
			}
			if (T.IsNearlyZero())
			{
				T = FVector(1.f, 0.f, 0.f);
			}
			Tangents[Vi] = FProcMeshTangent(T, false);
		}

		// Never cook complex collision from loft meshes:
		// double-sided / near-degenerate grid tris produce Chaos "bad triangles" spam.
		// SailBoatPawn uses a simple box collider instead.
		const int32 SectionIdx = *SectionCounter;
		Target->CreateMeshSection_LinearColor(
			SectionIdx, Vertices, Triangles, Normals, UV0, Colors, Tangents, /*bCreateCollision*/ false);

		if (BaseMat)
		{
			// Prefer authored /Game/Materials/Yacht/* DefaultLit instances when present.
			auto LoadYacht = [](const TCHAR* Path) -> UMaterialInterface*
			{
				return LoadObject<UMaterialInterface>(nullptr, Path);
			};
			UMaterialInterface* YachtPick = nullptr;
			if (bIsSail)
			{
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Sail.MI_Yacht_Sail"));
			}
			else if (bIsWindow)
			{
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Glass.MI_Yacht_Glass"));
			}
			else if (bIsKeel)
			{
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Keel.MI_Yacht_Keel"));
			}
			else if (bIsWinch)
			{
				// Reuse keel MI as dark metal; fall through to vertex color if missing.
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Keel.MI_Yacht_Keel"));
			}
			else if (bIsDeck || bIsCockpit || (bIsCabin && !bIsWindow))
			{
				// Cabin house / top uses same non-skid as deck (not separate glossy cabin MI)
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Deck.MI_Yacht_Deck"));
			}
			else if (bIsHull)
			{
				// Local-Z hard-band paint (crisp boot/cove). Fall back to solid gelcoat.
				YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_HullPaint.MI_Yacht_HullPaint"));
				if (!YachtPick)
				{
					YachtPick = LoadYacht(TEXT("/Game/Materials/Yacht/MI_Yacht_Gelcoat.MI_Yacht_Gelcoat"));
				}
			}

			if (YachtPick)
			{
				// HullPaint / Deck MIs already authored; deck is one-sided white non-skid.
				Target->SetMaterial(SectionIdx, YachtPick);
			}
			else
			{
				// Sails only: two-sided material (deck/hull use single-sided after normal flip).
				UMaterialInterface* MatSource = BaseMat;
				if (bIsSail)
				{
					static TWeakObjectPtr<UMaterial> SailTwoSidedMat;
					if (!SailTwoSidedMat.IsValid())
					{
						if (UMaterial* SrcMat = BaseMat->GetMaterial())
						{
							UMaterial* Dup = Cast<UMaterial>(StaticDuplicateObject(
								SrcMat, GetTransientPackage(), NAME_None, RF_Transient));
							if (Dup)
							{
								Dup->TwoSided = true;
#if WITH_EDITOR
								Dup->PostEditChange();
#endif
								SailTwoSidedMat = Dup;
							}
						}
					}
					if (SailTwoSidedMat.IsValid())
					{
						MatSource = SailTwoSidedMat.Get();
					}
				}

				UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(MatSource, Target);
				if (Mid)
				{
					const FLinearColor UseCol = bIsSail
						? FLinearColor(0.98f, 0.97f, 0.94f, 1.f)
						: (bIsHull ? FLinearColor(0.96f, 0.97f, 0.99f, 1.f)
							: ((bIsDeck || bIsCockpit || (bIsCabin && !bIsWindow))
								? FLinearColor(0.97f, 0.97f, 0.98f, 1.f) : Col));
					Mid->SetVectorParameterValue(TEXT("Color"), UseCol);
					Mid->SetVectorParameterValue(TEXT("BaseColor"), UseCol);
					if (bIsHull)
					{
						Mid->SetScalarParameterValue(TEXT("Roughness"), 0.15f);
						Mid->SetScalarParameterValue(TEXT("RoughTopsides"), 0.14f);
						Mid->SetScalarParameterValue(TEXT("ClearCoat"), 0.f);
						Mid->SetScalarParameterValue(TEXT("ClearCoatRoughness"), 1.f);
						Mid->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
						Mid->SetScalarParameterValue(TEXT("Specular"), 0.42f);
					}
					else if (bIsDeck || bIsCockpit)
					{
						Mid->SetScalarParameterValue(TEXT("Roughness"), 0.62f);
						Mid->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
						Mid->SetScalarParameterValue(TEXT("Specular"), 0.4f);
					}
					Target->SetMaterial(SectionIdx, Mid);
				}
			}
		}

		++(*SectionCounter);
		++TotalSections;
	}

	// Rebuild a solid reverse-transom plate (deck sheer → aft bottom). The JSON
	// "transom" fan is thin, often backface-culled, and was easy to break with
	// morphs — this grid is double-sided and always closes the stern.
	if (HullOrCombinedMesh && DeckAftEdge.Num() >= 3 && bHaveHullAft)
	{
		const int32 Nu = DeckAftEdge.Num(); // across (port→stbd)
		const int32 Nv = 8;                 // down the reverse face
		const float BottomX = HullAftX;
		const float BottomZ = HullAftBottomZ;
		// Bottom half-width ~ 30% of deck sheer (trapezoid reverse transom)
		float DeckHalfY = 1.f;
		for (const FVector& E : DeckAftEdge)
		{
			DeckHalfY = FMath::Max(DeckHalfY, FMath::Abs(E.Y));
		}
		const float BottomHalfY = DeckHalfY * 0.32f;

		TArray<FVector> TV;
		TArray<int32> TT;
		TArray<FVector> TN;
		TArray<FVector2D> TUV;
		TArray<FLinearColor> TC;
		TArray<FProcMeshTangent> TTan;
		TV.Reserve(Nu * Nv * 2);

		auto SampleTop = [&](int32 I) -> FVector
		{
			return DeckAftEdge[FMath::Clamp(I, 0, Nu - 1)];
		};

		for (int32 J = 0; J < Nv; ++J)
		{
			const float V01 = float(J) / float(Nv - 1); // 0 = deck, 1 = bottom
			// Reverse rake: top forward (deck X), bottom further aft (more −X)
			for (int32 I = 0; I < Nu; ++I)
			{
				const FVector Top = SampleTop(I);
				const float Y01 = (DeckHalfY > 1.f) ? (Top.Y / DeckHalfY) : 0.f;
				const float BotY = Y01 * BottomHalfY;
				const FVector Bot(BottomX, BotY, BottomZ);
				// Slight belly so the face isn't perfectly planar
				const float Belly = 4.f * FMath::Sin(V01 * PI);
				FVector P = FMath::Lerp(Top, Bot, V01);
				P.X -= Belly; // push face slightly aft mid-height
				TV.Add(P);
			}
		}

		// Front faces (outward / aft): wind CCW when viewing from stern (+ looking −X? 
		// from outside stern looking forward = looking +X, so CW in Y-up… use double sided)
		for (int32 J = 0; J + 1 < Nv; ++J)
		{
			for (int32 I = 0; I + 1 < Nu; ++I)
			{
				const int32 I00 = J * Nu + I;
				const int32 I10 = J * Nu + I + 1;
				const int32 I01 = (J + 1) * Nu + I;
				const int32 I11 = (J + 1) * Nu + I + 1;
				// Outward (aft, −X)
				TT.Add(I00); TT.Add(I01); TT.Add(I10);
				TT.Add(I10); TT.Add(I01); TT.Add(I11);
				// Inward (double-sided, no culling holes)
				TT.Add(I00); TT.Add(I10); TT.Add(I01);
				TT.Add(I10); TT.Add(I11); TT.Add(I01);
			}
		}

		TN.Init(FVector(-1.f, 0.f, 0.1f).GetSafeNormal(), TV.Num());
		// Better normals from geometry
		for (int32 J = 0; J < Nv; ++J)
		{
			for (int32 I = 0; I < Nu; ++I)
			{
				const int32 Idx = J * Nu + I;
				const FVector C = TV[Idx];
				const FVector Dx = TV[J * Nu + FMath::Min(I + 1, Nu - 1)] - TV[J * Nu + FMath::Max(I - 1, 0)];
				const FVector Dy = TV[FMath::Min(J + 1, Nv - 1) * Nu + I] - TV[FMath::Max(J - 1, 0) * Nu + I];
				FVector N = FVector::CrossProduct(Dy, Dx).GetSafeNormal(); // prefer −X
				if (N.X > 0.f) N = -N;
				if (N.IsNearlyZero()) N = FVector(-1.f, 0.f, 0.f);
				TN[Idx] = N;
			}
		}
		TUV.Init(FVector2D(0.5f, 0.5f), TV.Num());
		for (int32 I = 0; I < TV.Num(); ++I)
		{
			TUV[I] = FVector2D(TV[I].Y * 0.01f, TV[I].Z * 0.01f);
		}
		TC.Init(FLinearColor(0.04f, 0.10f, 0.28f, 1.f), TV.Num());
		TTan.Init(FProcMeshTangent(FVector(0.f, 1.f, 0.f), false), TV.Num());

		const int32 Sec = HullSection;
		HullOrCombinedMesh->CreateMeshSection_LinearColor(
			Sec, TV, TT, TN, TUV, TC, TTan, false);
		if (UMaterialInterface* HullPaint = LoadObject<UMaterialInterface>(
				nullptr, TEXT("/Game/Materials/Yacht/MI_Yacht_HullPaint.MI_Yacht_HullPaint")))
		{
			HullOrCombinedMesh->SetMaterial(Sec, HullPaint);
		}
		++HullSection;
		++TotalSections;
		UE_LOG(LogSailSim, Log,
			TEXT("BoatMeshFromJson: rebuilt reverse-transom plate (%d×%d, deckEdge=%d, jsonTransom=%s)"),
			Nu, Nv, DeckAftEdge.Num(), bHadJsonTransom ? TEXT("skipped") : TEXT("absent"));
	}
	else if (bHadJsonTransom)
	{
		UE_LOG(LogSailSim, Warning,
			TEXT("BoatMeshFromJson: could not rebuild transom (deckEdge=%d haveHullAft=%d)"),
			DeckAftEdge.Num(), bHaveHullAft ? 1 : 0);
	}

	if (MainSailMesh && MainSection > 0)
	{
		MainSailMesh->SetRelativeLocation(SailPivot);
		MainSailMesh->SetRelativeRotation(FRotator::ZeroRotator);
	}
	if (JibSailMesh && JibSection > 0)
	{
		JibSailMesh->SetRelativeLocation(SailPivot);
		JibSailMesh->SetRelativeRotation(FRotator::ZeroRotator);
	}

	if (OutResult)
	{
		OutResult->bOk = TotalSections > 0;
		OutResult->SectionCount = TotalSections;
		OutResult->Sailing = Sailing;
		OutResult->Spars = Spars;
		OutResult->Rigging = Rigging;
		OutResult->ResolvedPath = Path;
	}

	UE_LOG(LogSailSim, Log,
		TEXT("BoatMeshFromJson: loaded %d sections (hull=%d main=%d jib=%d) from %s"),
		TotalSections, HullSection, MainSection, JibSection, *Path);
	return TotalSections > 0;
}
