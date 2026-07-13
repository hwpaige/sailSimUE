#include "Sailing/BoatLoftOracle.h"
#include "SailSimUE.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

bool FBoatLoftOracle::LiveLoftExists()
{
	const FString Path = FPaths::ProjectContentDir() / LiveBoatJsonRelative();
	return FPaths::FileExists(Path);
}

bool FBoatLoftOracle::RebuildLiveLoft(const FBoatSpec& Spec, FString* OutError)
{
	const FString ContentData = FPaths::ProjectContentDir() / TEXT("Data");
	IFileManager::Get().MakeDirectory(*ContentData, true);

	const FString SpecPath = ContentData / TEXT("live_spec.json");
	const FString OutPath = ContentData / TEXT("live_boat3d.json");
	const FString ScriptPath = FPaths::ProjectDir() / TEXT("Scripts/export_boat3d_live.py");

	if (!FPaths::FileExists(ScriptPath))
	{
		const FString Err = FString::Printf(TEXT("Oracle script missing: %s"), *ScriptPath);
		UE_LOG(LogSailSim, Error, TEXT("%s"), *Err);
		if (OutError) *OutError = Err;
		return false;
	}

	// Write effective imperial dims for Python
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("preset_id"), Spec.Id.IsEmpty() ? TEXT("j105") : Spec.Id);
	Root->SetStringField(TEXT("name"), Spec.Name);
	Root->SetNumberField(TEXT("loa"), Spec.EffectiveLoa());
	Root->SetNumberField(TEXT("lwl"), Spec.EffectiveLwl());
	Root->SetNumberField(TEXT("beam"), Spec.EffectiveBeam());
	Root->SetNumberField(TEXT("draft"), Spec.EffectiveDraft());
	Root->SetNumberField(TEXT("disp_lb"), Spec.EffectiveDisp());
	Root->SetNumberField(TEXT("ballast_lb"), Spec.BallastLb * Spec.ScaleLoa * Spec.ScaleBeam * Spec.ScaleDraft);
	Root->SetNumberField(TEXT("sail_area"), Spec.EffectiveSail());
	Root->SetNumberField(TEXT("I"), Spec.I * Spec.ScaleLoa);
	Root->SetNumberField(TEXT("J"), Spec.J * Spec.ScaleBeam);
	Root->SetNumberField(TEXT("P"), Spec.P * Spec.ScaleLoa);
	Root->SetNumberField(TEXT("E"), Spec.E * Spec.ScaleLoa);
	Root->SetNumberField(TEXT("scale_loa"), Spec.ScaleLoa);
	Root->SetNumberField(TEXT("scale_beam"), Spec.ScaleBeam);
	Root->SetNumberField(TEXT("scale_draft"), Spec.ScaleDraft);

	FString SpecJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SpecJson);
	FJsonSerializer::Serialize(Root, Writer);
	if (!FFileHelper::SaveStringToFile(SpecJson, *SpecPath))
	{
		const FString Err = FString::Printf(TEXT("Failed to write %s"), *SpecPath);
		if (OutError) *OutError = Err;
		return false;
	}

	// Prefer python3 on PATH
	FString PythonBin = TEXT("python3");
	FString Args = FString::Printf(TEXT("\"%s\" \"%s\""), *ScriptPath, *SpecPath);

	// Working directory = project root so sail_geom path resolution matches manual runs
	const FString WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	int32 ReturnCode = -1;
	FString StdOut, StdErr;
	const bool bLaunched = FPlatformProcess::ExecProcess(
		*PythonBin, *Args, &ReturnCode, &StdOut, &StdErr, *WorkDir);

	if (!bLaunched)
	{
		const FString Err = TEXT("Failed to launch python3 (is it on PATH?)");
		UE_LOG(LogSailSim, Error, TEXT("%s"), *Err);
		if (OutError) *OutError = Err;
		return false;
	}

	UE_LOG(LogSailSim, Log, TEXT("Oracle python exit=%d\n%s\n%s"), ReturnCode, *StdOut, *StdErr);

	if (ReturnCode != 0 || !FPaths::FileExists(OutPath))
	{
		const FString Err = FString::Printf(
			TEXT("Oracle re-loft failed (code %d). stderr: %s"), ReturnCode, *StdErr);
		if (OutError) *OutError = Err;
		return false;
	}

	UE_LOG(LogSailSim, Log, TEXT("Oracle re-loft OK -> %s"), *OutPath);
	return true;
}
