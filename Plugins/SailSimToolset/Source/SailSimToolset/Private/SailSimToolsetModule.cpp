// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolsetModule.h"
#include "SailSimToolset.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#define LOCTEXT_NAMESPACE "FSailSimToolsetModule"

namespace
{
	IConsoleObject* GSailSimRunPreferOnGateCmd = nullptr;

	void ExecSailSimRunPreferOnGate()
	{
		const FString Json = USailSimToolset::RunPreferOnGate();
		// PersistPreferOnGateJson already ran inside RunPreferOnGate (Saved/SailSim/last_prefer_on_gate.json).
		UE_LOG(LogTemp, Display, TEXT("SailSim.RunPreferOnGate: %s"), *Json);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				INDEX_NONE, 8.f, FColor::Green,
				FString::Printf(TEXT("SailSim.RunPreferOnGate → Saved/SailSim/last_prefer_on_gate.json (%d chars)"), Json.Len()));
		}
	}
}

void FSailSimToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(USailSimToolset::StaticClass());

	if (!GSailSimRunPreferOnGateCmd)
	{
		GSailSimRunPreferOnGateCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SailSim.RunPreferOnGate"),
			TEXT("Ops Prefer-ON gate: EnsurePIE + DSF2 Prefer-ON stick + moored==16 assert + midHarborMoored CPV. Writes Saved/SailSim/last_prefer_on_gate.json. Does not change MaxBoats / moored strip. ProfileGPUDump stays parked."),
			FConsoleCommandDelegate::CreateStatic(&ExecSailSimRunPreferOnGate),
			ECVF_Default);
	}
}

void FSailSimToolsetModule::ShutdownModule()
{
	if (GSailSimRunPreferOnGateCmd)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSailSimRunPreferOnGateCmd, false);
		GSailSimRunPreferOnGateCmd = nullptr;
	}
	UToolsetRegistry::UnregisterToolsetClass(USailSimToolset::StaticClass());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSailSimToolsetModule, SailSimToolset)
