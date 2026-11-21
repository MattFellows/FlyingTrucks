// FlyingTrucks - a flyable variant of the Satisfactory truck.

#include "FlyingTrucksSubsystem.h"

#include "FlyingTrucks.h"
#include "FGSchematic.h"
#include "FGSchematicManager.h"
#include "Resources/FGResourceNode.h"
#include "Resources/FGResourceNodeBase.h"
#include "Resources/FGResourceDescriptor.h"
#include "Components/PointLightComponent.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

AFlyingTrucksSubsystem::AFlyingTrucksSubsystem()
{
	// Replicated so clients get BeginPlay too (for the node glows); the server does the real work.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
	AutoGrantSchematics.Add( TSoftClassPtr< UFGSchematic >( FSoftObjectPath( TEXT( "/FlyingTrucks/Schematic_FlyingTruck.Schematic_FlyingTruck_C" ) ) ) );
	AutoGrantSchematics.Add( TSoftClassPtr< UFGSchematic >( FSoftObjectPath( TEXT( "/FlyingTrucks/Schematic_AcclivityStone.Schematic_AcclivityStone_C" ) ) ) );
	AcclivityResource = TSoftClassPtr< UObject >( FSoftObjectPath( TEXT( "/FlyingTrucks/Resources/Desc_AcclivityStone.Desc_AcclivityStone_C" ) ) );
	ConvertibleResourceClassNames = { TEXT( "Desc_OreIron_C" ), TEXT( "Desc_Stone_C" ) };
}

void AFlyingTrucksSubsystem::BeginPlay()
{
	Super::BeginPlay();

	AcclivityClass = AcclivityResource.LoadSynchronous();
	if( !AcclivityClass )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "Acclivity Stone resource %s not found." ), *AcclivityResource.ToString() );
	}

	FTimerManager& Timers = GetWorldTimerManager();
	if( HasAuthority() )
	{
		// The schematic manager registers mod schematics during world init; retry for a bit in case we're early.
		Timers.SetTimer( GrantTimer, this, &AFlyingTrucksSubsystem::TryGrantSchematics, 1.0f, true, 0.5f );
		// Give the game's own node setup (and any node randomiser) time to run first.
		Timers.SetTimer( ConvertTimer, this, &AFlyingTrucksSubsystem::TryConvertNodes, 2.0f, true, 3.0f );
	}
	if( GetNetMode() != NM_DedicatedServer )
	{
		// Clients learn about converted nodes as they replicate in, so keep checking now and then.
		Timers.SetTimer( GlowTimer, this, &AFlyingTrucksSubsystem::RefreshNodeGlows, 5.0f, true, 4.0f );
	}
}

void AFlyingTrucksSubsystem::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	FTimerManager& Timers = GetWorldTimerManager();
	Timers.ClearTimer( GrantTimer );
	Timers.ClearTimer( ConvertTimer );
	Timers.ClearTimer( GlowTimer );
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

bool AFlyingTrucksSubsystem::SetNodeResourceOverride( AFGResourceNodeBase* Node, UClass* Resource ) const
{
	// The override is private in the game's headers; set it through reflection, then run its OnRep
	// so the node refreshes here as it would on a client.
	static FClassProperty* OverrideProp = CastField< FClassProperty >( AFGResourceNodeBase::StaticClass()->FindPropertyByName( TEXT( "mResourceClassOverride" ) ) );
	if( !OverrideProp )
	{
		return false;
	}
	OverrideProp->SetObjectPropertyValue_InContainer( Node, Resource );
	if( UFunction* OnRep = Node->FindFunction( TEXT( "OnRep_ResourceClassOverride" ) ) )
	{
		Node->ProcessEvent( OnRep, nullptr );
	}
	Node->FlushNetDormancy();
	Node->ForceNetUpdate();
	return true;
}

void AFlyingTrucksSubsystem::TryConvertNodes()
{
	++ConvertAttempts;
	if( !AcclivityClass )
	{
		GetWorldTimerManager().ClearTimer( ConvertTimer );
		return;
	}

	TArray< AFGResourceNode* > Candidates;
	int32 NumNodes = 0;
	int32 NumAlreadyConverted = 0;
	for( TActorIterator< AFGResourceNode > It( GetWorld() ); It; ++It )
	{
		AFGResourceNode* Node = *It;
		if( Node->GetResourceNodeType() != EResourceNodeType::Node )
		{
			continue; // leave deposits, geysers and fracking wells alone
		}
		++NumNodes;
		UClass* Resource = Node->GetResourceClass();
		if( Resource == AcclivityClass )
		{
			++NumAlreadyConverted;
		}
		else if( Resource && ConvertibleResourceClassNames.Contains( Resource->GetName() ) )
		{
			Candidates.Add( Node );
		}
	}

	if( NumNodes == 0 )
	{
		if( ConvertAttempts >= 30 )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "No resource nodes found; no Acclivity Stone nodes created." ) );
			GetWorldTimerManager().ClearTimer( ConvertTimer );
		}
		return;
	}
	GetWorldTimerManager().ClearTimer( ConvertTimer );

	if( NumAlreadyConverted > 0 )
	{
		UE_LOG( LogFlyingTrucks, Display, TEXT( "This save already has %d Acclivity Stone nodes." ), NumAlreadyConverted );
		RefreshNodeGlows();
		return;
	}

	// Pick by a hash of the node's (stable, level-placed) name: spread across the map, same choice every time.
	int32 NumConverted = 0;
	for( AFGResourceNode* Node : Candidates )
	{
		const uint32 Hash = FCrc::StrCrc32( *Node->GetName() );
		if( Hash % FMath::Max( ConvertOneNodeIn, 1 ) != 0 )
		{
			continue;
		}
		const FString From = Node->GetResourceClass()->GetName();
		if( SetNodeResourceOverride( Node, AcclivityClass ) )
		{
			++NumConverted;
			UE_LOG( LogFlyingTrucks, Display, TEXT( "Acclivity Stone node: %s (was %s) at %s" ), *Node->GetName(), *From, *Node->GetActorLocation().ToCompactString() );
		}
		else
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "Could not set the resource override on nodes; game layout changed?" ) );
			break;
		}
	}
	UE_LOG( LogFlyingTrucks, Display, TEXT( "Converted %d of %d iron/limestone nodes (%d nodes total) to Acclivity Stone." ), NumConverted, Candidates.Num(), NumNodes );
	RefreshNodeGlows();
}

void AFlyingTrucksSubsystem::RefreshNodeGlows()
{
	if( !AcclivityClass || GetNetMode() == NM_DedicatedServer )
	{
		return;
	}
	for( TActorIterator< AFGResourceNode > It( GetWorld() ); It; ++It )
	{
		AFGResourceNode* Node = *It;
		const bool bIsStone = Node->GetResourceClass() == AcclivityClass;
		const TWeakObjectPtr< UPointLightComponent >* Existing = NodeGlows.Find( Node );
		if( bIsStone && !( Existing && Existing->IsValid() ) )
		{
			USceneComponent* Root = Node->GetRootComponent();
			if( !Root )
			{
				continue;
			}
			UPointLightComponent* Light = NewObject< UPointLightComponent >( Node, TEXT( "AcclivityGlow" ) );
			Light->SetupAttachment( Root );
			Light->SetRelativeLocation( FVector( 0.f, 0.f, 200.f ) );
			Light->SetIntensityUnits( ELightUnits::Unitless );
			Light->SetIntensity( NodeGlowIntensity );
			Light->SetAttenuationRadius( NodeGlowRadius );
			Light->SetLightColor( NodeGlowColor );
			Light->SetCastShadows( false );
			Light->RegisterComponent();
			NodeGlows.Add( Node, Light );
		}
		else if( !bIsStone && Existing && Existing->IsValid() )
		{
			( *Existing )->DestroyComponent();
			NodeGlows.Remove( Node );
		}
	}
}
