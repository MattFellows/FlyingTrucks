// FlyingTrucks - a flyable variant of the Satisfactory truck.

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FlyingTrucksSubsystem.generated.h"

class UFGSchematic;

/**
 * Server-side subsystem (registered in RootGameWorld_FlyingTrucks) that grants the mod's schematics
 * as soon as a world is loaded, so the Flying Truck is buildable without any unlock step.
 */
UCLASS()
class FLYINGTRUCKS_API AFlyingTrucksSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AFlyingTrucksSubsystem();

	/** Schematics to grant automatically. Defaults to Schematic_FlyingTruck. */
	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks" )
	TArray< TSoftClassPtr< UFGSchematic > > AutoGrantSchematics;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;

private:
	void TryGrantSchematics();

	FTimerHandle GrantTimer;
	int32 GrantAttempts = 0;
};
