// FlyingTrucks - a flyable variant of the Satisfactory truck.

#include "FlyingTrucks.h"
#include "FlyingVehicleHologram.h"

DEFINE_LOG_CATEGORY(LogFlyingTrucks);

#define LOCTEXT_NAMESPACE "FFlyingTrucksModule"

void FFlyingTrucksModule::StartupModule()
{
	UE_LOG(LogFlyingTrucks, Display, TEXT("FlyingTrucks module started"));
	FlyingTrucks_RegisterHologramHook();
}

void FFlyingTrucksModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FFlyingTrucksModule, FlyingTrucks)