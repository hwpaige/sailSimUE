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
	IConsoleObject* GSailSimAssertModuleFreshCmd = nullptr;
	IConsoleObject* GSailSimEnsureTipInBinaryCmd = nullptr;

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

	bool IsHexShaArg(const FString& Arg)
	{
		if (Arg.Len() < 7 || Arg.Len() > 40)
		{
			return false;
		}
		for (const TCHAR Ch : Arg)
		{
			if (!FChar::IsHexDigit(Ch))
			{
				return false;
			}
		}
		return true;
	}

	void ParseFreshArgs(const TArray<FString>& Args, FString& OutSha, bool& bLiveCompile)
	{
		OutSha.Reset();
		bLiveCompile = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("LiveCompile"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("bLiveCompile=1"), ESearchCase::IgnoreCase)
				|| Arg.Equals(TEXT("1")))
			{
				bLiveCompile = true;
			}
			else if (IsHexShaArg(Arg))
			{
				OutSha = Arg;
			}
		}
	}

	void ExecAssertModuleFresh(const TArray<FString>& Args)
	{
		FString Sha;
		bool bLiveCompile = false;
		ParseFreshArgs(Args, Sha, bLiveCompile);
		const FString Json = USailSimToolset::AssertModuleFresh(Sha, bLiveCompile);
		UE_LOG(LogTemp, Display, TEXT("SailSim.AssertModuleFresh: %s"), *Json);
	}

	void ExecEnsureTipInBinary(const TArray<FString>& Args)
	{
		FString Sha;
		bool bLiveCompile = false;
		ParseFreshArgs(Args, Sha, bLiveCompile);
		const FString Json = USailSimToolset::EnsureTipInBinary(Sha, bLiveCompile);
		UE_LOG(LogTemp, Display, TEXT("SailSim.EnsureTipInBinary: %s"), *Json);
	}
}

void FSailSimToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(USailSimToolset::StaticClass());

	if (!GSailSimRunPreferOnGateCmd)
	{
		GSailSimRunPreferOnGateCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SailSim.RunPreferOnGate"),
			TEXT("Ops Prefer-ON gate. Fails closed (stale_binary / tip_not_in_binary) before HighResShot unless the game-module binary is newer than Source/SailSimUE and the log shows hull slot only + multi-slot mats=. Optional arg: expected git SHA. Writes Saved/SailSim/last_prefer_on_gate.json."),
			FConsoleCommandDelegate::CreateStatic(&ExecSailSimRunPreferOnGate),
			ECVF_Default);
	}
	if (!GSailSimAssertModuleFreshCmd)
	{
		GSailSimAssertModuleFreshCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SailSim.AssertModuleFresh"),
			TEXT("Compare git HEAD (optional SHA arg) to SailSimUE/SailSimToolset binary mtimes. Pass LiveCompile to queue LiveCoding.Compile without blocking. If LC cannot start, ubtHint is the editor-target rebuild. Writes Saved/SailSim/last_module_fresh.json."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ExecAssertModuleFresh),
			ECVF_Default);
	}
	if (!GSailSimEnsureTipInBinaryCmd)
	{
		GSailSimEnsureTipInBinaryCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SailSim.EnsureTipInBinary"),
			TEXT("Same as SailSim.AssertModuleFresh. Call before RunPreferOnGate. Optional SHA, optional LiveCompile."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ExecEnsureTipInBinary),
			ECVF_Default);
	}
}

void FSailSimToolsetModule::ShutdownModule()
{
	if (GSailSimEnsureTipInBinaryCmd)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSailSimEnsureTipInBinaryCmd, false);
		GSailSimEnsureTipInBinaryCmd = nullptr;
	}
	if (GSailSimAssertModuleFreshCmd)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSailSimAssertModuleFreshCmd, false);
		GSailSimAssertModuleFreshCmd = nullptr;
	}
	if (GSailSimRunPreferOnGateCmd)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GSailSimRunPreferOnGateCmd, false);
		GSailSimRunPreferOnGateCmd = nullptr;
	}
	UToolsetRegistry::UnregisterToolsetClass(USailSimToolset::StaticClass());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSailSimToolsetModule, SailSimToolset)
