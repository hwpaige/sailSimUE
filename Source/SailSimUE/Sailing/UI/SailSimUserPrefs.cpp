#include "Sailing/UI/SailSimUserPrefs.h"
#include "Sailing/BoatDynamics.h"
#include "Sailing/SailBoatPawn.h"
#include "Sailing/Ocean/SailOceanSubsystem.h"
#include "Sailing/Wind/WindFieldSubsystem.h"
#include "SailSimUE.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"

FString FSailSimUserPrefs::ConfigPath()
{
	// Absolute path — relative ProjectSavedDir can confuse GConfig / FConfigFile writes.
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Config"), TEXT("SailSimUserPrefs.ini")));
}

void FSailSimUserPrefs::Load()
{
	const FString Path = ConfigPath();
	if (!FPaths::FileExists(Path))
	{
		UE_LOG(LogSailSim, Log, TEXT("Prefs: no file at %s (using defaults)"), *Path);
		return;
	}

	// Read the file directly — do not rely on GConfig custom-path branches
	// (those were never writing SailSimUserPrefs.ini to disk).
	FConfigFile File;
	File.Read(Path);

	const TCHAR* Sec = TEXT("SailSim.User");

	// Prefer TwsDisplayKn; legacy TwsKn may have been physics-scale.
	float DispTws = TwsKn;
	if (File.GetFloat(Sec, TEXT("TwsDisplayKn"), DispTws))
	{
		TwsKn = DispTws;
	}
	else
	{
		float Legacy = 18.f;
		if (File.GetFloat(Sec, TEXT("TwsKn"), Legacy))
		{
			// Heuristic: old files stored display if ≤ WindDisplayMaxKn, else physics.
			if (Legacy > FBoatDynamics::WindDisplayMaxKn + 0.5f)
			{
				TwsKn = FBoatDynamics::WindDisplayFromPhysics(Legacy);
			}
			else
			{
				TwsKn = Legacy;
			}
		}
	}

	File.GetFloat(Sec, TEXT("TwdDeg"), TwdDeg);
	File.GetFloat(Sec, TEXT("Outhaul01"), Outhaul01);
	File.GetFloat(Sec, TEXT("Vang01"), Vang01);
	File.GetFloat(Sec, TEXT("SheetEase01"), SheetEase01);
	File.GetFloat(Sec, TEXT("SpinSheet01"), SpinSheet01);
	File.GetFloat(Sec, TEXT("JibCar01"), JibCar01);
	File.GetBool(Sec, TEXT("JibSet"), bJibSet);
	File.GetBool(Sec, TEXT("KiteSet"), bKiteSet);
	File.GetFloat(Sec, TEXT("Cloud01"), Cloud01);
	File.GetFloat(Sec, TEXT("Fog01"), Fog01);
	File.GetFloat(Sec, TEXT("TimeOfDayHours"), TimeOfDayHours);
	File.GetFloat(Sec, TEXT("Season01"), Season01);
	File.GetFloat(Sec, TEXT("MapPanelW"), MapPanelW);
	File.GetFloat(Sec, TEXT("MapPanelH"), MapPanelH);
	File.GetInt(Sec, TEXT("EnvPreset"), EnvPreset);
	File.GetBool(Sec, TEXT("LightsPanelOpen"), bLightsPanelOpen);

	TwsKn = FMath::Clamp(TwsKn, 0.f, FBoatDynamics::WindDisplayMaxKn);
	TwdDeg = FMath::Fmod(TwdDeg + 360.f, 360.f);
	Outhaul01 = FMath::Clamp(Outhaul01, 0.f, 1.f);
	Vang01 = FMath::Clamp(Vang01, 0.f, 1.f);
	SheetEase01 = FMath::Clamp(SheetEase01, 0.f, 1.f);
	SpinSheet01 = FMath::Clamp(SpinSheet01, 0.f, 1.f);
	JibCar01 = FMath::Clamp(JibCar01, 0.f, 1.f);
	Cloud01 = FMath::Clamp(Cloud01, 0.f, 1.f);
	Fog01 = FMath::Clamp(Fog01, 0.f, 1.f);
	TimeOfDayHours = FMath::Fmod(FMath::Max(0.f, TimeOfDayHours), 24.f);
	Season01 = FMath::Clamp(Season01, 0.f, 1.f);
	MapPanelW = FMath::Clamp(MapPanelW, 220.f, 720.f);
	MapPanelH = FMath::Clamp(MapPanelH, 160.f, 560.f);
	EnvPreset = FMath::Clamp(EnvPreset, 0, 6);

	UE_LOG(LogSailSim, Log,
		TEXT("Prefs loaded: %s  sheet=%.2f out=%.2f vang=%.2f jib=%d kite=%d tws=%.1f twd=%.0f cloud=%.2f env=%d"),
		*Path, SheetEase01, Outhaul01, Vang01, bJibSet ? 1 : 0, bKiteSet ? 1 : 0,
		TwsKn, TwdDeg, Cloud01, EnvPreset);
}

void FSailSimUserPrefs::Save() const
{
	const FString Path = ConfigPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);

	FConfigFile File;
	if (FPaths::FileExists(Path))
	{
		File.Read(Path);
	}

	const TCHAR* Sec = TEXT("SailSim.User");
	auto SetF = [&](const TCHAR* Key, float V)
	{
		File.SetString(Sec, Key, *FString::Printf(TEXT("%g"), V));
	};
	auto SetB = [&](const TCHAR* Key, bool V)
	{
		File.SetString(Sec, Key, V ? TEXT("True") : TEXT("False"));
	};
	auto SetI = [&](const TCHAR* Key, int32 V)
	{
		File.SetString(Sec, Key, *FString::FromInt(V));
	};

	// Display-scale wind (matches HUD / SetTrueWind).
	SetF(TEXT("TwsDisplayKn"), TwsKn);
	SetF(TEXT("TwsKn"), TwsKn); // same scale; kept for older readers
	SetF(TEXT("TwdDeg"), TwdDeg);
	SetF(TEXT("Outhaul01"), Outhaul01);
	SetF(TEXT("Vang01"), Vang01);
	SetF(TEXT("SheetEase01"), SheetEase01);
	SetF(TEXT("SpinSheet01"), SpinSheet01);
	SetF(TEXT("JibCar01"), JibCar01);
	SetB(TEXT("JibSet"), bJibSet);
	SetB(TEXT("KiteSet"), bKiteSet);
	SetF(TEXT("Cloud01"), Cloud01);
	SetF(TEXT("Fog01"), Fog01);
	SetF(TEXT("TimeOfDayHours"), TimeOfDayHours);
	SetF(TEXT("Season01"), Season01);
	SetF(TEXT("MapPanelW"), MapPanelW);
	SetF(TEXT("MapPanelH"), MapPanelH);
	SetI(TEXT("EnvPreset"), EnvPreset);
	SetB(TEXT("LightsPanelOpen"), bLightsPanelOpen);

	File.NoSave = false;
	File.Dirty = true;

	bool bOk = File.Write(Path);
	if (!bOk)
	{
		// Hard fallback: write the INI text ourselves.
		FString Text;
		File.WriteToString(Text, Path);
		bOk = FFileHelper::SaveStringToFile(
			Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	if (bOk)
	{
		UE_LOG(LogSailSim, Log,
			TEXT("Prefs saved: %s  sheet=%.2f out=%.2f vang=%.2f jib=%d kite=%d tws=%.1f twd=%.0f cloud=%.2f env=%d"),
			*Path, SheetEase01, Outhaul01, Vang01, bJibSet ? 1 : 0, bKiteSet ? 1 : 0,
			TwsKn, TwdDeg, Cloud01, EnvPreset);
	}
	else
	{
		UE_LOG(LogSailSim, Error, TEXT("Prefs SAVE FAILED: %s"), *Path);
	}
}

void FSailSimUserPrefs::Apply(ASailBoatPawn* Boat, UWorld* World) const
{
	if (Boat)
	{
		Boat->SetOuthaul(Outhaul01);
		Boat->SetVang(Vang01);
		Boat->SetSheetEase(SheetEase01);
		Boat->SetSpinSheetEase(SpinSheet01);
		Boat->SetJibCar(JibCar01);
		Boat->SetJibSet(bJibSet);
		Boat->SetKiteSet(bKiteSet);
		Boat->SetTrueWind(TwsKn, TwdDeg);
	}
	UWorld* W = World;
	if (!W && Boat) W = Boat->GetWorld();
	if (!W) return;

	if (USailOceanSubsystem* Ocean = W->GetSubsystem<USailOceanSubsystem>())
	{
		// Time of day first (Fair Day at noon = captured baseline). Mood presets
		// (Overcast/Storm/Fog) still available from Settings env buttons.
		Ocean->SetTimeOfDayHours(TimeOfDayHours);
		if (EnvPreset >= 4 && EnvPreset <= 6) // Overcast / Storm / FogBank
		{
			Ocean->ApplyEnvPreset(static_cast<uint8>(EnvPreset));
		}
		// Explicit fog/cloud sliders always win over any preset defaults.
		Ocean->SetVolumetricCloudIntensity(Cloud01);
		Ocean->SetFogIntensity(Fog01);
		Ocean->SetSeason01(Season01);
	}
	if (Boat)
	{
		// Ensure base field matches saved synoptic wind after preset apply.
		Boat->SetTrueWind(TwsKn, TwdDeg);
	}

	UE_LOG(LogSailSim, Log,
		TEXT("Prefs applied: sheet=%.2f out=%.2f tws=%.1f twd=%.0f cloud=%.2f env=%d"),
		SheetEase01, Outhaul01, TwsKn, TwdDeg, Cloud01, EnvPreset);
}
