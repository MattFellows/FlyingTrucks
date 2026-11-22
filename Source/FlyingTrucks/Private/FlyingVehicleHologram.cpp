// FlyingTrucks - build holograms for vehicles that bring their own look (M-Bot).
//
// The game builds a vehicle's hologram from the base vehicle's meshes, so M-Bot (an Explorer underneath)
// previewed as an Explorer. After any vehicle hologram sets itself up, this swaps those meshes for the
// vehicle's own parked look, using the hologram's material so it still turns red/blue for placement.

#include "FlyingVehicleHologram.h"

#include "FlyingTrucks.h"
#include "FlyingTruckComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Hologram/FGVehicleHologram.h"
#include "Patching/NativeHookManager.h"

namespace
{
	void DressVehicleHologram( AFGVehicleHologram* Hologram )
	{
		if( !Hologram || Hologram->GetNetMode() == NM_DedicatedServer )
		{
			return;
		}
		UClass* VehicleClass = Hologram->GetBuildClass();
		const UFlyingTruckComponent* Template = UFlyingTruckComponent::FindTemplate( VehicleClass );
		if( !Template || !Template->ReplacesVehicleMesh() )
		{
			return;
		}
		TArray< FFlyingParkedVisual > Visuals;
		Template->GetParkedVisuals( VehicleClass->GetDefaultObject< AActor >(), Visuals );
		if( Visuals.Num() == 0 )
		{
			return;
		}

		// The vehicle body the hologram copied: our parts are placed relative to it, like on the real vehicle.
		USkeletalMeshComponent* Body = Hologram->FindComponentByClass< USkeletalMeshComponent >();
		if( !Body )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: no vehicle mesh on the hologram to replace." ), *Hologram->GetName() );
			return;
		}
		UMaterialInterface* HologramMaterial = Body->GetMaterial( 0 );
		const TArray< FName > Tags = Body->ComponentTags;

		// Hide the base vehicle's look (its body and anything else copied with the same hologram tags).
		TArray< UMeshComponent* > Meshes;
		Hologram->GetComponents< UMeshComponent >( Meshes );
		for( UMeshComponent* Mesh : Meshes )
		{
			if( Mesh == Body || ( Tags.Num() > 0 && Mesh->ComponentTags == Tags ) )
			{
				Mesh->SetHiddenInGame( true );
			}
		}

		for( int32 i = 0; i < Visuals.Num(); ++i )
		{
			UStaticMeshComponent* Comp = NewObject< UStaticMeshComponent >( Hologram, *FString::Printf( TEXT( "FlyingHologramPart%d" ), i ) );
			Comp->SetupAttachment( Body );
			Comp->SetStaticMesh( Visuals[ i ].Mesh );
			Comp->SetRelativeTransform( Visuals[ i ].Transform );
			Comp->SetCollisionEnabled( ECollisionEnabled::NoCollision );
			Comp->SetCanEverAffectNavigation( false );
			Comp->SetCastShadow( false );
			Comp->ComponentTags = Tags; // so placement colour changes reach it too
			Comp->SetRenderCustomDepth( Body->bRenderCustomDepth );
			Comp->SetCustomDepthStencilValue( Body->CustomDepthStencilValue );
			if( HologramMaterial )
			{
				for( int32 Slot = 0; Slot < FMath::Max( 1, Comp->GetNumMaterials() ); ++Slot )
				{
					Comp->SetMaterial( Slot, HologramMaterial );
				}
			}
			Comp->RegisterComponent();
		}
		UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: hologram shows %d own parts for %s." ), *Hologram->GetName(), Visuals.Num(), *GetNameSafe( VehicleClass ) );
	}
}

void FlyingTrucks_RegisterHologramHook()
{
#if !WITH_EDITOR
	AFGVehicleHologram* Sample = GetMutableDefault< AFGVehicleHologram >();
	SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGVehicleHologram::BeginPlay, Sample, []( AFGVehicleHologram* Self )
	{
		DressVehicleHologram( Self );
	} );
#endif
}
