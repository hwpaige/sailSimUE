// Copyright Sail Buddy. All Rights Reserved.

#include "SailSimToolsetModule.h"
#include "SailSimToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#define LOCTEXT_NAMESPACE "FSailSimToolsetModule"

void FSailSimToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(USailSimToolset::StaticClass());
}

void FSailSimToolsetModule::ShutdownModule()
{
	UToolsetRegistry::UnregisterToolsetClass(USailSimToolset::StaticClass());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSailSimToolsetModule, SailSimToolset)
