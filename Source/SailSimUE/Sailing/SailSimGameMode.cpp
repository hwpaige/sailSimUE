#include "Sailing/SailSimGameMode.h"
#include "Sailing/SailBoatPawn.h"

ASailSimGameMode::ASailSimGameMode()
{
	DefaultPawnClass = ASailBoatPawn::StaticClass();
}
