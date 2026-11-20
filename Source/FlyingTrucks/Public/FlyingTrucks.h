// FlyingTrucks - a flyable variant of the Satisfactory truck.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

FLYINGTRUCKS_API DECLARE_LOG_CATEGORY_EXTERN(LogFlyingTrucks, Log, All);

class FFlyingTrucksModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
