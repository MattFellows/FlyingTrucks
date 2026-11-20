// FlyingTrucks - a flyable variant of the Satisfactory truck.

#include "FlyingTrucksSubsystem.h"

#include "FlyingTrucks.h"
#include "FGSchematic.h"
#include "FGSchematicManager.h"
#include "TimerManager.h"
#include "Engine/World.h"

AFlyingTrucksSubsystem::AFlyingTrucksSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	AutoGrantSchematics.Add( TSoftClassPtr< UFGSchematic >( FSoftObjectPath( TEXT( "/FlyingTrucks/Schematic_FlyingTruck.Schematic_FlyingTruck_C" ) ) ) );
}

void AFlyingTrucksSubsystem::BeginPlay()
{
	Super::BeginPlay();
	if( HasAuthority() )
	{
		// The schematic manager registers mod schematics during world init; retry for a bit in case we're early.
		GetWorldTimerManager().SetTimer( GrantTimer, this, &AFlyingTrucksSubsystem::TryGrantSchematics, 1.0f, true, 0.5f );
	}
}

void AFlyingTrucksSubsystem::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	GetWorldTimerManager().ClearTimer( GrantTimer );
	Super::EndPlay( EndPlayReason );
}

void AFlyingTrucksSubsystem::TryGrantSchematics()
{
	++GrantAttempts;

	AFGSchematicManager* SchematicManager = AFGSchematicManager::Get( GetWorld() );
	if( !SchematicManager )
	{
		if( GrantAttempts >= 30 )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "Schematic manager never appeared; Flying Truck not unlocked." ) );
			GetWorldTimerManager().ClearTimer( GrantTimer );
		}
		return;
	}

	bool bAllGranted = true;
	for( const TSoftClassPtr< UFGSchematic >& SoftSchematic : AutoGrantSchematics )
	{
		TSubclassOf< UFGSchematic > Schematic = SoftSchematic.LoadSynchronous();
		if( !Schematic )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "Could not load schematic %s." ), *SoftSchematic.ToString() );
			continue;
		}
		if( SchematicManager->IsSchematicPurchased( Schematic ) )
		{
			continue;
		}
		if( !SchematicManager->CanGiveAccessToSchematic( Schematic ) )
		{
			bAllGranted = false;
			continue;
		}
		SchematicManager->GiveAccessToSchematic( Schematic, nullptr );
		UE_LOG( LogFlyingTrucks, Display, TEXT( "Granted %s." ), *Schematic->GetName() );
	}

	if( bAllGranted || GrantAttempts >= 30 )
	{
		if( !bAllGranted )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "Gave up granting FlyingTrucks schematics after %d attempts." ), GrantAttempts );
		}
		GetWorldTimerManager().ClearTimer( GrantTimer );
	}
}
