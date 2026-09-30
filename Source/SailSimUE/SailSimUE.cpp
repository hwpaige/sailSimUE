// Copyright Epic Games, Inc. All Rights Reserved.

#include "SailSimUE.h"
#include "Modules/ModuleManager.h"
#include "Sailing/SailSimPerf.h"

DEFINE_LOG_CATEGORY(LogSailSim);

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, SailSimUE, "SailSimUE" );

FSailSimPerf& SailSimGetPerf()
{
	// Defined in the game module so editor tools and the HUD share one set of
	// counters. FSailSimPerf::Get() is inline and would be a different copy in
	// another DLL.
	return FSailSimPerf::Get();
}
