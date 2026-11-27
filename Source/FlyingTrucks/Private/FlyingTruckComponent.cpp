// FlyingTrucks - a flyable variant of the Satisfactory truck.

#include "FlyingTruckComponent.h"

#include "FlyingTrucks.h"
#include "FlyingCombatComponent.h"
#include "ChaosVehicleMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "FGChatManager.h"
#include "FGInventoryComponent.h"
#include "Resources/FGItemDescriptor.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "FGDriveablePawn.h"
#include "FGCharacterPlayer.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#define LOCTEXT_NAMESPACE "FlyingTrucks"

UFlyingTruckComponent::UFlyingTruckComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetIsReplicatedByDefault( true );
}

void UFlyingTruckComponent::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( UFlyingTruckComponent, bIsFlying );
	// The driver uses its own live input, so skip sending it back to them.
	DOREPLIFETIME_CONDITION( UFlyingTruckComponent, ReplicatedInput, COND_SkipOwner );
}

void UFlyingTruckComponent::BeginPlay()
{
	Super::BeginPlay();

	Combat = GetOwner()->FindComponentByClass< UFlyingCombatComponent >();

	VehicleMovement = GetOwner()->FindComponentByClass< UChaosVehicleMovementComponent >();
	if( !VehicleMovement )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: no Chaos vehicle movement component found; throttle/steering won't affect flight." ), *GetOwner()->GetName() );
	}

	if( APawn* Pawn = Cast< APawn >( GetOwner() ) )
	{
		Pawn->ReceiveControllerChangedDelegate.AddDynamic( this, &UFlyingTruckComponent::HandleControllerChanged );
		// Already possessed (e.g. component added late)?
		if( APlayerController* PC = Cast< APlayerController >( Pawn->GetController() ) )
		{
			BindToController( PC );
		}
	}
	else
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "UFlyingTruckComponent must be added to a pawn (vehicle); owner is %s." ), *GetOwner()->GetName() );
	}

	if( !FlightFuelItem.IsNull() )
	{
		FuelItemClass = FlightFuelItem.LoadSynchronous();
		if( !FuelItemClass )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "Flight fuel item %s not found; flight will not need fuel." ), *FlightFuelItem.ToString() );
		}
	}

	FitToVehicle();
	WingScale *= WingScaleAdjust;
	WingPivot += WingPivotAdjust;
	CreateBodyParts();
	CreateGlowLights();
	CreateRing();
	CreateWings();
	// Late joiners whose truck is already flying start with the wings out.
	WingDeployAlpha = bIsFlying ? 1.f : 0.f;
	UpdateWings( 0.f );
	// A late-joining client may already have bIsFlying = true replicated before BeginPlay.
	UpdateGlow();

	if( bApplyDefaultPaint && GetOwner()->HasAuthority() )
	{
		// A freshly built vehicle gets its customization from the build gun after BeginPlay, so wait a moment.
		GetWorld()->GetTimerManager().SetTimer( PaintTimer, this, &UFlyingTruckComponent::ApplyDefaultPaint, 1.0f, false );
	}
}

void UFlyingTruckComponent::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	ShowHiddenDriver();
	UnbindFromController();
	if( UWorld* World = GetWorld() )
	{
		World->GetTimerManager().ClearTimer( PaintTimer );
		World->GetTimerManager().ClearTimer( HideMeshTimer );
	}
	if( APawn* Pawn = Cast< APawn >( GetOwner() ) )
	{
		Pawn->ReceiveControllerChangedDelegate.RemoveDynamic( this, &UFlyingTruckComponent::HandleControllerChanged );
	}
	Super::EndPlay( EndPlayReason );
}

void UFlyingTruckComponent::HandleControllerChanged( APawn* Pawn, AController* OldController, AController* NewController )
{
	UnbindFromController();
	if( APlayerController* PC = Cast< APlayerController >( NewController ) )
	{
		BindToController( PC );
	}
	else
	{
		// Driver left: stop flying (the truck will glide/fall).
		RemoteInput = FFlyingTruckInput();
		if( GetOwner()->HasAuthority() )
		{
			SetFlying( false );
		}
	}
}

void UFlyingTruckComponent::CreateInputObjects()
{
	if( ToggleContext )
	{
		return;
	}

	ToggleAction = NewObject< UInputAction >( this, TEXT( "IA_FlyingTruck_ToggleFlight" ) );
	PitchUpAction = NewObject< UInputAction >( this, TEXT( "IA_FlyingTruck_PitchUp" ) );
	PitchDownAction = NewObject< UInputAction >( this, TEXT( "IA_FlyingTruck_PitchDown" ) );

	ToggleContext = NewObject< UInputMappingContext >( this, TEXT( "IMC_FlyingTruck_Toggle" ) );
	ToggleContext->MapKey( ToggleAction, EKeys::F );
	ToggleContext->MapKey( ToggleAction, EKeys::Gamepad_FaceButton_Top );       // Y on Xbox / Steam Deck

	auto MakePassThroughAxis = [this]( const TCHAR* Name )
	{
		UInputAction* Action = NewObject< UInputAction >( this, Name );
		Action->ValueType = EInputActionValueType::Axis1D;
		Action->bConsumeInput = false; // the truck still gets these keys (ground driving)
		return Action;
	};
	ThrottleAction = MakePassThroughAxis( TEXT( "IA_FlyingTruck_Throttle" ) );
	BrakeAction = MakePassThroughAxis( TEXT( "IA_FlyingTruck_Brake" ) );
	SteerLeftAction = MakePassThroughAxis( TEXT( "IA_FlyingTruck_SteerLeft" ) );
	SteerRightAction = MakePassThroughAxis( TEXT( "IA_FlyingTruck_SteerRight" ) );
	SteerAxisAction = MakePassThroughAxis( TEXT( "IA_FlyingTruck_SteerAxis" ) );

	FlightContext = NewObject< UInputMappingContext >( this, TEXT( "IMC_FlyingTruck_Flight" ) );
	FlightContext->MapKey( ThrottleAction, EKeys::W );
	FlightContext->MapKey( ThrottleAction, EKeys::Gamepad_RightTriggerAxis );
	FlightContext->MapKey( BrakeAction, EKeys::S );
	FlightContext->MapKey( BrakeAction, EKeys::Gamepad_LeftTriggerAxis );
	FlightContext->MapKey( SteerLeftAction, EKeys::A );
	FlightContext->MapKey( SteerRightAction, EKeys::D );
	FlightContext->MapKey( SteerAxisAction, EKeys::Gamepad_LeftX );
	FlightContext->MapKey( PitchUpAction, EKeys::SpaceBar );
	FlightContext->MapKey( PitchUpAction, EKeys::Gamepad_FaceButton_Bottom );   // A
	FlightContext->MapKey( PitchDownAction, EKeys::LeftControl );
	FlightContext->MapKey( PitchDownAction, EKeys::C );                         // matches the game's "down" habit
	FlightContext->MapKey( PitchDownAction, EKeys::Gamepad_FaceButton_Right );  // B
	FireAction = NewObject< UInputAction >( this, TEXT( "IA_FlyingTruck_Fire" ) );
	IMPAction = NewObject< UInputAction >( this, TEXT( "IA_FlyingTruck_IMP" ) );
	if( Combat )
	{
		FlightContext->MapKey( FireAction, EKeys::LeftMouseButton );
		FlightContext->MapKey( FireAction, EKeys::Gamepad_RightShoulder );      // RB / R1
		FlightContext->MapKey( IMPAction, EKeys::RightMouseButton );
		FlightContext->MapKey( IMPAction, EKeys::Gamepad_LeftShoulder );        // LB / L1
	}
}

void UFlyingTruckComponent::BindToController( APlayerController* PlayerController )
{
	if( !PlayerController || !PlayerController->IsLocalController() || BoundController == PlayerController )
	{
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem< UEnhancedInputLocalPlayerSubsystem >( PlayerController->GetLocalPlayer() );
	if( !Subsystem )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "No Enhanced Input subsystem for %s." ), *PlayerController->GetName() );
		return;
	}

	CreateInputObjects();

	if( !FlightInputComponent )
	{
		FlightInputComponent = NewObject< UEnhancedInputComponent >( this, TEXT( "FlyingTruckInput" ) );
		FlightInputComponent->BindAction( ToggleAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_ToggleFlight );
		FlightInputComponent->BindAction( ToggleAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_ToggleReleased );
		FlightInputComponent->BindAction( ThrottleAction, ETriggerEvent::Triggered, this, &UFlyingTruckComponent::Input_Throttle );
		FlightInputComponent->BindAction( ThrottleAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_ThrottleReleased );
		FlightInputComponent->BindAction( BrakeAction, ETriggerEvent::Triggered, this, &UFlyingTruckComponent::Input_Brake );
		FlightInputComponent->BindAction( BrakeAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_BrakeReleased );
		FlightInputComponent->BindAction( SteerLeftAction, ETriggerEvent::Triggered, this, &UFlyingTruckComponent::Input_SteerLeft );
		FlightInputComponent->BindAction( SteerLeftAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_SteerLeftReleased );
		FlightInputComponent->BindAction( SteerRightAction, ETriggerEvent::Triggered, this, &UFlyingTruckComponent::Input_SteerRight );
		FlightInputComponent->BindAction( SteerRightAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_SteerRightReleased );
		FlightInputComponent->BindAction( SteerAxisAction, ETriggerEvent::Triggered, this, &UFlyingTruckComponent::Input_SteerAxis );
		FlightInputComponent->BindAction( SteerAxisAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_SteerAxisReleased );
		FlightInputComponent->BindAction( PitchUpAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_PitchUpPressed );
		FlightInputComponent->BindAction( PitchUpAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_PitchUpReleased );
		FlightInputComponent->BindAction( PitchDownAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_PitchDownPressed );
		FlightInputComponent->BindAction( PitchDownAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_PitchDownReleased );
		FlightInputComponent->BindAction( IMPAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_IMP );
		FlightInputComponent->BindAction( FireAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_FirePressed );
		FlightInputComponent->BindAction( FireAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_FireReleased );
		FlightInputComponent->BindAction( FireAction, ETriggerEvent::Canceled, this, &UFlyingTruckComponent::Input_FireReleased );
	}

	BoundController = PlayerController;
	PlayerController->PushInputComponent( FlightInputComponent );
	Subsystem->AddMappingContext( ToggleContext, InputPriority );
	UpdateFlightContext();

	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: flight controls bound to %s." ), *GetOwner()->GetName(), *PlayerController->GetName() );
}

void UFlyingTruckComponent::UnbindFromController()
{
	if( !BoundController )
	{
		return;
	}

	if( UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem< UEnhancedInputLocalPlayerSubsystem >( BoundController->GetLocalPlayer() ) )
	{
		Subsystem->RemoveMappingContext( ToggleContext );
		Subsystem->RemoveMappingContext( FlightContext );
	}
	if( FlightInputComponent )
	{
		BoundController->PopInputComponent( FlightInputComponent );
	}
	BoundController = nullptr;
	bToggleKeyHeld = false;
	ResetFlightInputs();
}

void UFlyingTruckComponent::UpdateFlightContext()
{
	if( !BoundController || !FlightContext )
	{
		return;
	}
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem< UEnhancedInputLocalPlayerSubsystem >( BoundController->GetLocalPlayer() );
	if( !Subsystem )
	{
		return;
	}
	// Pitch keys only exist while flying, so Space is still the handbrake on the ground.
	if( bIsFlying )
	{
		Subsystem->AddMappingContext( FlightContext, InputPriority );
	}
	else
	{
		Subsystem->RemoveMappingContext( FlightContext );
		ResetFlightInputs();
	}
}

void UFlyingTruckComponent::ResetFlightInputs()
{
	bPitchUpHeld = bPitchDownHeld = false;
	if( Combat )
	{
		Combat->SetFiring( false );
	}
	ThrottleInput = BrakeInput = SteerLeftInput = SteerRightInput = SteerAxisInput = 0.f;
}

void UFlyingTruckComponent::Input_FirePressed( const FInputActionValue& Value )
{
	if( Combat && bIsFlying )
	{
		Combat->SetFiring( true );
	}
}

void UFlyingTruckComponent::Input_IMP( const FInputActionValue& Value )
{
	if( Combat && bIsFlying )
	{
		Combat->RequestIMP();
	}
}

void UFlyingTruckComponent::Input_FireReleased( const FInputActionValue& Value )
{
	if( Combat )
	{
		Combat->SetFiring( false );
	}
}

void UFlyingTruckComponent::Input_Throttle( const FInputActionValue& Value ) { ThrottleInput = FMath::Clamp( Value.Get< float >(), 0.f, 1.f ); }
void UFlyingTruckComponent::Input_Brake( const FInputActionValue& Value ) { BrakeInput = FMath::Clamp( Value.Get< float >(), 0.f, 1.f ); }
void UFlyingTruckComponent::Input_SteerLeft( const FInputActionValue& Value ) { SteerLeftInput = FMath::Clamp( Value.Get< float >(), 0.f, 1.f ); }
void UFlyingTruckComponent::Input_SteerRight( const FInputActionValue& Value ) { SteerRightInput = FMath::Clamp( Value.Get< float >(), 0.f, 1.f ); }
void UFlyingTruckComponent::Input_SteerAxis( const FInputActionValue& Value ) { SteerAxisInput = FMath::Clamp( Value.Get< float >(), -1.f, 1.f ); }

void UFlyingTruckComponent::Input_ToggleFlight( const FInputActionValue& Value )
{
	// One key press can arrive several times: twice in the same frame, and again when toggling flight
	// adds/removes the flight mapping context (Enhanced Input rebuilds and re-sees the held key).
	// So require the key to be released and some time to pass between toggles.
	const double Now = GetWorld()->GetRealTimeSeconds();
	if( bToggleKeyHeld || Now - LastToggleTime < ToggleCooldownSeconds )
	{
		UE_LOG( LogFlyingTrucks, Log, TEXT( "Ignoring repeated toggle (held=%d, %.2fs since last)." ), bToggleKeyHeld ? 1 : 0, Now - LastToggleTime );
		return;
	}
	bToggleKeyHeld = true;
	LastToggleTime = Now;
	ToggleFlying();
}

void UFlyingTruckComponent::Input_ToggleReleased( const FInputActionValue& Value )
{
	bToggleKeyHeld = false;
}

void UFlyingTruckComponent::SetFlying( bool bNewFlying )
{
	if( bIsFlying == bNewFlying )
	{
		return;
	}
	const bool bAuthority = GetOwner()->HasAuthority();
	if( bNewFlying && !bAuthority )
	{
		// Only the server can see the cargo, so it decides whether we may take off; flight mode
		// switches on here when its answer replicates back.
		Server_SetFlying( true );
		return;
	}
	if( bNewFlying && Combat && Combat->IsGrounded() )
	{
		NotifyDriver( LOCTEXT( "HullTooLow", "Hull too damaged to fly - still repairing." ) );
		return;
	}
	if( bNewFlying && FuelItemClass && GetFuelCount() <= 0 )
	{
		NotifyDriver( FText::Format( LOCTEXT( "NoFuel", "Flight needs {0} in the vehicle's cargo or your inventory." ),
			UFGItemDescriptor::GetItemName( TSubclassOf< UFGItemDescriptor >( FuelItemClass.Get() ) ) ) );
		return;
	}
	// Landing is applied immediately here (no lag for the driver); the server's value then replicates to everyone.
	bIsFlying = bNewFlying;
	ApplyFlightModeLocally();
	if( !bAuthority )
	{
		Server_SetFlying( bNewFlying );
	}
	else if( bNewFlying )
	{
		bLowFuelWarned = false;
		UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: taking off with %d fuel item(s)." ), *GetOwner()->GetName(), GetFuelCount() );
	}
}

void UFlyingTruckComponent::GetFuelInventories( TArray< UFGInventoryComponent* >& OutInventories ) const
{
	if( const AFGWheeledVehicle* Vehicle = Cast< AFGWheeledVehicle >( GetOwner() ) )
	{
		if( UFGInventoryComponent* Cargo = Vehicle->GetStorageInventory() )
		{
			OutInventories.Add( Cargo );
		}
	}
	// The Factory Cart has no cargo, and it's handy on the others too.
	if( const AFGDriveablePawn* Driveable = Cast< AFGDriveablePawn >( GetOwner() ) )
	{
		if( const AFGCharacterPlayer* Driver = Driveable->GetDriver() )
		{
			if( UFGInventoryComponent* Pockets = Driver->GetInventory() )
			{
				OutInventories.Add( Pockets );
			}
		}
	}
}

int32 UFlyingTruckComponent::GetFuelCount() const
{
	if( !FuelItemClass )
	{
		return 0;
	}
	TArray< UFGInventoryComponent* > Inventories;
	GetFuelInventories( Inventories );
	int32 Count = 0;
	for( UFGInventoryComponent* Inventory : Inventories )
	{
		Count += Inventory->GetNumItems( TSubclassOf< UFGItemDescriptor >( FuelItemClass.Get() ) );
	}
	return Count;
}

namespace
{
	/** Bounds of an actor's main (root) skeletal mesh asset, in actor space. */
	bool GetVehicleMeshBox( const AActor* Actor, FBox& OutBox )
	{
		const USkeletalMeshComponent* Mesh = Actor ? Cast< USkeletalMeshComponent >( Actor->GetRootComponent() ) : nullptr;
		if( !Mesh && Actor )
		{
			Mesh = Actor->FindComponentByClass< USkeletalMeshComponent >();
		}
		const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
		if( !Asset )
		{
			return false;
		}
		OutBox = Asset->GetImportedBounds().GetBox();
		return OutBox.IsValid && !OutBox.GetExtent().IsNearlyZero();
	}
}

namespace
{
	/**
	 * How a vehicle relates to the reference (Truck) the ring/wing settings were tuned on:
	 * positions map as OwnC + ( P - RefC ) * S. False when no fitting is needed or possible.
	 */
	bool ComputeVehicleFit( const AActor* Vehicle, const TSoftClassPtr< AActor >& ReferenceVehicle, FVector& OwnC, FVector& RefC, FVector& S )
	{
		UClass* RefClass = ReferenceVehicle.LoadSynchronous();
		if( !Vehicle || !RefClass || Vehicle->IsA( RefClass ) )
		{
			return false; // the Truck: values are tuned for it
		}
		FBox Own, Ref;
		if( !GetVehicleMeshBox( Vehicle, Own ) || !GetVehicleMeshBox( RefClass->GetDefaultObject< AActor >(), Ref ) )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: couldn't measure the vehicle; using Truck-sized ring/wings." ), *Vehicle->GetName() );
			return false;
		}
		OwnC = Own.GetCenter();
		RefC = Ref.GetCenter();
		S = Own.GetExtent() / Ref.GetExtent().ComponentMax( FVector( 1.f ) );
		return true;
	}

	/** Swing that folds a wing mesh's tip in until its leading edge lies along the vehicle. */
	float WingFoldYawFor( const FBoxSphereBounds& Bounds, float Scale, float Override, float Extra )
	{
		if( Override > 0.f )
		{
			return Override;
		}
		const float Chord = Bounds.BoxExtent.X * 2.f * Scale;
		const float Span = Bounds.BoxExtent.Y * 2.f * Scale;
		return FMath::RadiansToDegrees( FMath::Atan2( Span, FMath::Max( Chord, 1.f ) ) ) + Extra;
	}
}

void UFlyingTruckComponent::FitToVehicle()
{
	if( !bScaleToVehicle || ReferenceVehicle.IsNull() )
	{
		return;
	}
	FVector OwnC, RefC, S;
	if( !ComputeVehicleFit( GetOwner(), ReferenceVehicle, OwnC, RefC, S ) )
	{
		return;
	}
	auto Map = [&]( const FVector& P ) { return OwnC + ( P - RefC ) * S; };

	RingDiameter *= S.Y;
	RingOffset = Map( RingOffset );
	GlowOffset = Map( GlowOffset );
	GlowRadius *= S.GetMax();
	if( bFitWings )
	{
		WingPivot = Map( WingPivot );
		WingScale *= FMath::Min( S.X, S.Y );
	}
	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: scale %s vs truck; ring %.0f at %s, wings x%.2f at %s" ),
		*GetOwner()->GetName(), *S.ToCompactString(), RingDiameter, *RingOffset.ToCompactString(),
		WingScale, *WingPivot.ToCompactString() );
}

void UFlyingTruckComponent::TickFuel( float DeltaTime )
{
	if( !FuelItemClass )
	{
		return;
	}
	FlightSecondsSinceBurn += DeltaTime;

	// No need to count the cargo every frame.
	FuelCheckTimer -= DeltaTime;
	const bool bBurnNow = FlightSecondsSinceBurn >= FlightSecondsPerFuelItem;
	if( FuelCheckTimer > 0.f && !bBurnNow )
	{
		return;
	}
	FuelCheckTimer = 0.25f;

	const FText ItemName = UFGItemDescriptor::GetItemName( TSubclassOf< UFGItemDescriptor >( FuelItemClass.Get() ) );
	int32 Count = GetFuelCount();
	if( Count > 0 && bBurnNow )
	{
		TArray< UFGInventoryComponent* > Inventories;
		GetFuelInventories( Inventories );
		for( UFGInventoryComponent* Inventory : Inventories )
		{
			if( Inventory->HasItems( TSubclassOf< UFGItemDescriptor >( FuelItemClass.Get() ), 1 ) )
			{
				Inventory->Remove( TSubclassOf< UFGItemDescriptor >( FuelItemClass.Get() ), 1 );
				break;
			}
		}
		FlightSecondsSinceBurn -= FlightSecondsPerFuelItem;
		--Count;
		UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: burned 1 fuel item, %d left." ), *GetOwner()->GetName(), Count );
	}
	if( Count <= 0 )
	{
		NotifyDriver( FText::Format( LOCTEXT( "OutOfFuel", "Out of {0} - flight mode off!" ), ItemName ) );
		SetFlying( false );
		return;
	}
	const float SecondsLeft = Count * FlightSecondsPerFuelItem - FlightSecondsSinceBurn;
	if( SecondsLeft <= LowFuelWarningSeconds )
	{
		if( !bLowFuelWarned )
		{
			bLowFuelWarned = true;
			NotifyDriver( FText::Format( LOCTEXT( "LowFuel", "Last {0} nearly used up - {1} seconds of flight left!" ),
				ItemName, FText::AsNumber( FMath::CeilToInt( SecondsLeft ) ) ) );
		}
	}
	else
	{
		bLowFuelWarned = false; // more stone was added
	}
}

void UFlyingTruckComponent::NotifyDriver( const FText& Message )
{
	if( BoundController )
	{
		ShowLocalMessage( Message );
		return;
	}
	const APawn* Pawn = Cast< APawn >( GetOwner() );
	if( GetOwner()->HasAuthority() && Pawn && Pawn->GetController() )
	{
		Client_Notify( Message );
	}
}

void UFlyingTruckComponent::Client_Notify_Implementation( const FText& Message )
{
	ShowLocalMessage( Message );
}

void UFlyingTruckComponent::ShowLocalMessage( const FText& Message ) const
{
	UE_LOG( LogFlyingTrucks, Log, TEXT( "Message: %s" ), *Message.ToString() );
	if( AFGChatManager* Chat = AFGChatManager::Get( GetWorld() ) )
	{
		FChatMessageStruct Msg;
		Msg.MessageText = Message;
		Msg.MessageType = EFGChatMessageType::CMT_SystemMessage;
		Chat->AddChatMessageToReceived( Msg );
	}
}

void UFlyingTruckComponent::ApplyFlightModeLocally()
{
	if( !bIsFlying && GetOwner()->HasAuthority() )
	{
		ReplicatedInput = FFlyingTruckInput();
	}
	UpdateFlightContext();
	UpdateGlow();
	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: flight mode %s (%s)." ), *GetOwner()->GetName(), bIsFlying ? TEXT( "ON" ) : TEXT( "OFF" ),
		GetOwner()->HasAuthority() ? TEXT( "server" ) : TEXT( "client" ) );
	OnFlightModeChanged.Broadcast( bIsFlying );
}

void UFlyingTruckComponent::OnRep_IsFlying()
{
	ApplyFlightModeLocally();
}

void UFlyingTruckComponent::Server_SetFlying_Implementation( bool bNewFlying )
{
	SetFlying( bNewFlying );
}

void UFlyingTruckComponent::Server_SetFlightInput_Implementation( FFlyingTruckInput NewInput )
{
	NewInput.Throttle = FMath::Clamp( NewInput.Throttle, 0.f, 1.f );
	NewInput.Brake = FMath::Clamp( NewInput.Brake, 0.f, 1.f );
	NewInput.Steer = FMath::Clamp( NewInput.Steer, -1.f, 1.f );
	NewInput.Pitch = FMath::Clamp( NewInput.Pitch, -1.f, 1.f );
	RemoteInput = NewInput;
}

FFlyingTruckInput UFlyingTruckComponent::GatherLocalInput() const
{
	FFlyingTruckInput In;
	In.Throttle = ThrottleInput;
	In.Brake = BrakeInput;
	In.Steer = FMath::Clamp( SteerRightInput - SteerLeftInput + SteerAxisInput, -1.f, 1.f );
	In.Pitch = ( bPitchUpHeld ? 1.f : 0.f ) - ( bPitchDownHeld ? 1.f : 0.f );
	return In;
}

void UFlyingTruckComponent::SendInputToServer( float DeltaTime )
{
	// Send on change (at most 30/s) plus a 5/s heartbeat, since these RPCs are unreliable.
	const double Now = GetWorld()->GetTimeSeconds();
	const FFlyingTruckInput Current = GatherLocalInput();
	const double SinceLast = Now - LastInputSendTime;
	const bool bChanged = !Current.IsNearlyEqual( LastSentInput );
	if( ( bChanged && SinceLast >= 1.0 / 30.0 ) || SinceLast >= 0.2 )
	{
		Server_SetFlightInput( Current );
		LastSentInput = Current;
		LastInputSendTime = Now;
	}
}

void UFlyingTruckComponent::TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction )
{
	Super::TickComponent( DeltaTime, TickType, ThisTickFunction );

	UpdateWings( DeltaTime );
	UpdateRing( DeltaTime );
	UpdateDriverVisibility();

	if( !bIsFlying || DeltaTime <= 0.f )
	{
		return;
	}

	const bool bAuthority = GetOwner()->HasAuthority();
	if( BoundController && !bAuthority )
	{
		SendInputToServer( DeltaTime );
	}
	if( bAuthority )
	{
		TickFuel( DeltaTime );
		if( !bIsFlying )
		{
			return; // just ran out
		}
		// Share the driver's controls with onlookers so their copy of the truck flies the same way.
		const FFlyingTruckInput Current = BoundController ? GatherLocalInput() : RemoteInput;
		if( !Current.IsNearlyEqual( ReplicatedInput, 0.01f ) )
		{
			ReplicatedInput = Current;
		}
	}
	// Forces run on every machine: without them an onlooker's copy only has gravity between
	// network updates, sags, and gets yanked back (the "janky" look).

	UPrimitiveComponent* Body = Cast< UPrimitiveComponent >( GetOwner()->GetRootComponent() );
	if( !Body || !Body->IsSimulatingPhysics() )
	{
		return;
	}

	ApplyFlightForces( Body, DeltaTime );
}

bool UFlyingTruckComponent::IsAirborne( const UPrimitiveComponent* Body ) const
{
	const FVector Start = Body->GetComponentLocation();
	const FVector End = Start - FVector::UpVector * GroundCheckDistance;
	FCollisionQueryParams Params( SCENE_QUERY_STAT( FlyingTruckGround ), false, GetOwner() );
	FHitResult Hit;
	return !GetWorld()->LineTraceSingleByChannel( Hit, Start, End, ECC_Visibility, Params );
}

void UFlyingTruckComponent::ApplyFlightForces( UPrimitiveComponent* Body, float DeltaTime )
{
	const FTransform Xf = Body->GetComponentTransform();
	const FVector Fwd = Xf.GetUnitAxis( EAxis::X );
	const FVector Right = Xf.GetUnitAxis( EAxis::Y );
	const FVector Up = Xf.GetUnitAxis( EAxis::Z );

	const FVector Velocity = Body->GetPhysicsLinearVelocity();
	const float FwdSpeed = FVector::DotProduct( Velocity, Fwd );
	const bool bAirborne = IsAirborne( Body );

	// Local driver: our own flight-mode bindings (the game's vehicle input freezes while airborne).
	// Server with a remote driver: the controls that driver sent us.
	// Onlookers: the driver's controls as replicated by the server.
	const FFlyingTruckInput In = BoundController ? GatherLocalInput() : ( GetOwner()->HasAuthority() ? RemoteInput : ReplicatedInput );
	const float Throttle = In.Throttle;
	const float Brake = In.Brake;
	const float Steer = In.Steer;
	const float Pitch = In.Pitch;

	// ---- Linear: thrust, lift, drag, slip alignment (all as accelerations, mass-independent) ----
	const float Gravity = FMath::Abs( GetWorld()->GetGravityZ() );
	FVector Accel = FVector::ZeroVector;

	// The wheels still drive on the ground; extra thrust in the air keeps you moving.
	Accel += Fwd * ( Throttle - Brake * ReverseThrustFraction ) * ThrustAcceleration;

	// Hover (M-Bot): 1 when slow, fading to 0 by HoverSpeed.
	const float HoverBlend = bHover ? 1.f - FMath::Clamp( FMath::Abs( FwdSpeed ) / FMath::Max( HoverSpeed, 1.f ), 0.f, 1.f ) : 0.f;

	// Lift ~ v^2, equals gravity at TakeoffSpeed, along the truck's up axis (so banking turns).
	const float SpeedRatio = FMath::Max( FwdSpeed, 0.f ) / FMath::Max( TakeoffSpeed, 1.f );
	Accel += Up * FMath::Min( SpeedRatio * SpeedRatio, MaxLiftG ) * Gravity * ( 1.f - HoverBlend );
	if( HoverBlend > 0.f )
	{
		// Hold height against gravity, rise/sink on the pitch keys, and let drift die away.
		Accel += FVector::UpVector * ( Gravity + Pitch * HoverClimbAcceleration ) * HoverBlend;
		Accel -= Velocity * HoverDamping * HoverBlend * ( 1.f - Throttle );
	}

	if( bAirborne )
	{
		Accel -= Velocity * LinearDrag;
		// Bleed off sideways and vertical slip so the truck flies where it points (not while hovering).
		const float Align = FMath::Min( VelocityAlignment, 1.f / DeltaTime ) * ( 1.f - HoverBlend );
		const FVector Slip = Right * FVector::DotProduct( Velocity, Right ) + Up * FVector::DotProduct( Velocity, Up );
		Accel -= Slip * Align;
		Accel += Fwd * Slip.Size() * Align * 0.5f;
	}

	Body->AddForce( Accel, NAME_None, /*bAccelChange*/ true );

	DebugLogTimer -= DeltaTime;
	if( DebugLogTimer <= 0.f )
	{
		DebugLogTimer = 2.f;
		UE_LOG( LogFlyingTrucks, Log, TEXT( "Flight: speed %.0f km/h, fwd %.0f km/h, alt-check %s, throttle %.2f brake %.2f steer %.2f pitch %.0f, lift %.2fg" ),
			Velocity.Size() * 0.036f, FwdSpeed * 0.036f, bAirborne ? TEXT( "air" ) : TEXT( "ground" ),
			Throttle, Brake, Steer, Pitch, FMath::Min( SpeedRatio * SpeedRatio, MaxLiftG ) );
	}

	// ---- Angular: rate control around the truck's own axes ----
	// Using FVector::CrossProduct conventions (X fwd, Y right, Z up):
	//   nose up    = rotation about -Right
	//   turn right = rotation about +Up
	//   bank right = rotation about -Fwd
	const FVector AngVel = FMath::DegreesToRadians( Body->GetPhysicsAngularVelocityInDegrees() );
	// While hovering the pitch keys move it up/down instead, and it levels itself.
	FVector DesiredAngVel = -Right * FMath::DegreesToRadians( Pitch * PitchRateDegrees ) * ( 1.f - HoverBlend );
	if( HoverBlend > 0.f )
	{
		const float NoseUpDegrees = FMath::RadiansToDegrees( FMath::Asin( FMath::Clamp( Fwd.Z, -1.f, 1.f ) ) );
		DesiredAngVel += Right * FMath::DegreesToRadians( NoseUpDegrees * HoverLevelGain ) * HoverBlend;
	}

	if( bAirborne || HoverBlend > 0.f )
	{
		const float CurrentBank = FMath::RadiansToDegrees( FMath::Asin( FMath::Clamp( -Right.Z, -1.f, 1.f ) ) );
		const float TargetBank = Steer * MaxBankDegrees * ( 1.f - HoverBlend );
		const float RollRate = ( TargetBank - CurrentBank ) * RollGain;
		DesiredAngVel += Up * FMath::DegreesToRadians( Steer * YawRateDegrees );
		DesiredAngVel += -Fwd * FMath::DegreesToRadians( RollRate );

		const FVector AngAccel = ( DesiredAngVel - AngVel ) * AngularResponse;
		Body->AddTorqueInRadians( AngAccel, NAME_None, /*bAccelChange*/ true );
	}
	else if( Pitch != 0.f )
	{
		// On the ground only help the nose up/down for take-off; leave steering to the wheels.
		const float PitchError = FVector::DotProduct( DesiredAngVel - AngVel, Right );
		Body->AddTorqueInRadians( Right * PitchError * AngularResponse, NAME_None, true );
	}
}

namespace
{
	/** Calls a reflected function on Target with a properly initialised parameter buffer. */
	struct FReflectedCall
	{
		UFunction* Function = nullptr;
		uint8* Params = nullptr;

		FReflectedCall( UObject* Target, FName Name )
		{
			Function = Target ? Target->FindFunction( Name ) : nullptr;
			if( Function )
			{
				Params = static_cast< uint8* >( FMemory::Malloc( FMath::Max< int32 >( Function->ParmsSize, 1 ), Function->GetMinAlignment() ) );
				FMemory::Memzero( Params, Function->ParmsSize );
				for( TFieldIterator< FProperty > It( Function ); It && It->HasAnyPropertyFlags( CPF_Parm ); ++It )
				{
					It->InitializeValue_InContainer( Params );
				}
			}
		}
		~FReflectedCall()
		{
			if( Params )
			{
				for( TFieldIterator< FProperty > It( Function ); It && It->HasAnyPropertyFlags( CPF_Parm ); ++It )
				{
					It->DestroyValue_InContainer( Params );
				}
				FMemory::Free( Params );
			}
		}
		FStructProperty* FirstStructParam() const
		{
			for( TFieldIterator< FProperty > It( Function ); It && It->HasAnyPropertyFlags( CPF_Parm ); ++It )
			{
				if( FStructProperty* Prop = CastField< FStructProperty >( *It ) )
				{
					return Prop;
				}
			}
			return nullptr;
		}
	};

	void SetLinearColorField( const UScriptStruct* Struct, void* StructData, FName Field, const FLinearColor& Color )
	{
		if( FStructProperty* Prop = CastField< FStructProperty >( Struct->FindPropertyByName( Field ) ) )
		{
			if( Prop->Struct == TBaseStructure< FLinearColor >::Get() )
			{
				*Prop->ContainerPtrToValuePtr< FLinearColor >( StructData ) = Color;
			}
		}
	}
}

void UFlyingTruckComponent::ApplyDefaultPaint()
{
	AActor* Owner = GetOwner();
	if( !Owner )
	{
		return;
	}

	FReflectedCall Get( Owner, TEXT( "GetCustomizationData" ) );
	FStructProperty* DataProp = Get.Function ? Get.FirstStructParam() : nullptr;
	if( !DataProp )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: GetCustomizationData not found; can't paint." ), *Owner->GetName() );
		return;
	}
	Owner->ProcessEvent( Get.Function, Get.Params );

	const UScriptStruct* DataStruct = DataProp->Struct;
	void* Data = DataProp->ContainerPtrToValuePtr< void >( Get.Params );
	FClassProperty* SwatchProp = CastField< FClassProperty >( DataStruct->FindPropertyByName( TEXT( "SwatchDesc" ) ) );
	FStructProperty* ColorProp = CastField< FStructProperty >( DataStruct->FindPropertyByName( TEXT( "OverrideColorData" ) ) );
	if( !SwatchProp || !ColorProp )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: customization data has no SwatchDesc/OverrideColorData; can't paint." ), *Owner->GetName() );
		return;
	}

	UClass* CurrentSwatch = Cast< UClass >( SwatchProp->GetObjectPropertyValue_InContainer( Data ) );
	const FString CurrentName = CurrentSwatch ? CurrentSwatch->GetName() : TEXT( "none" );
	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: current swatch %s." ), *Owner->GetName(), *CurrentName );

	// Only repaint trucks still wearing the default paint; anything the player painted is kept.
	const bool bIsDefaultPaint = !CurrentSwatch || CurrentName == TEXT( "SwatchDesc_Slot0_C" );
	if( !bIsDefaultPaint )
	{
		return;
	}

	UClass* CustomSwatch = LoadClass< UObject >( nullptr, TEXT( "/Game/FactoryGame/Buildable/-Shared/Customization/Swatches/SwatchDesc_Custom.SwatchDesc_Custom_C" ) );
	if( !CustomSwatch )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "SwatchDesc_Custom not found; can't paint." ) );
		return;
	}
	SwatchProp->SetObjectPropertyValue_InContainer( Data, CustomSwatch );
	void* ColorData = ColorProp->ContainerPtrToValuePtr< void >( Data );
	SetLinearColorField( ColorProp->Struct, ColorData, TEXT( "PrimaryColor" ), DefaultPaintPrimary );
	SetLinearColorField( ColorProp->Struct, ColorData, TEXT( "SecondaryColor" ), DefaultPaintSecondary );

	FReflectedCall Set( Owner, TEXT( "SetCustomizationData" ) );
	FStructProperty* SetParam = Set.Function ? Set.FirstStructParam() : nullptr;
	if( !SetParam || SetParam->Struct != DataStruct )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: SetCustomizationData not found; can't paint." ), *Owner->GetName() );
		return;
	}
	DataStruct->CopyScriptStruct( SetParam->ContainerPtrToValuePtr< void >( Set.Params ), Data );
	Owner->ProcessEvent( Set.Function, Set.Params );
	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: applied Flying Truck paint." ), *Owner->GetName() );
}

void UFlyingTruckComponent::CreateGlowLights()
{
	if( !bFlightGlow || GetNetMode() == NM_DedicatedServer || GlowLight )
	{
		return;
	}
	USceneComponent* Root = GetOwner()->GetRootComponent();
	if( !Root )
	{
		return;
	}

	GlowLight = NewObject< UPointLightComponent >( GetOwner(), TEXT( "FlyingTruckGlow" ) );
	GlowLight->SetupAttachment( Root );
	GlowLight->SetRelativeLocation( GlowOffset - FVector( 0.f, 0.f, 40.f ) );
	GlowLight->SetIntensityUnits( ELightUnits::Unitless );
	GlowLight->SetIntensity( GlowIntensity );
	GlowLight->SetAttenuationRadius( GlowRadius );
	GlowLight->SetLightColor( GlowColor );
	GlowLight->SetCastShadows( false );
	GlowLight->SetVisibility( false );
	GlowLight->RegisterComponent();

	GroundGlowLight = NewObject< USpotLightComponent >( GetOwner(), TEXT( "FlyingTruckGroundGlow" ) );
	GroundGlowLight->SetupAttachment( Root );
	GroundGlowLight->SetRelativeLocation( GlowOffset );
	GroundGlowLight->SetRelativeRotation( FRotator( -90.f, 0.f, 0.f ) ); // straight down
	GroundGlowLight->SetIntensityUnits( ELightUnits::Unitless );
	GroundGlowLight->SetIntensity( GroundGlowIntensity );
	GroundGlowLight->SetAttenuationRadius( GroundGlowRange );
	GroundGlowLight->SetInnerConeAngle( 15.f );
	GroundGlowLight->SetOuterConeAngle( 40.f );
	GroundGlowLight->SetLightColor( GlowColor );
	GroundGlowLight->SetCastShadows( false );
	GroundGlowLight->SetVisibility( false );
	GroundGlowLight->RegisterComponent();
}

void UFlyingTruckComponent::UpdateGlow()
{
	const bool bOn = bFlightGlow && bIsFlying;
	if( GlowLight )
	{
		GlowLight->SetVisibility( bOn );
	}
	if( GroundGlowLight )
	{
		GroundGlowLight->SetVisibility( bOn );
	}
	if( RingComponent )
	{
		RingComponent->SetVisibility( bFlightRing && ( bIsFlying || bRingAlwaysVisible ) );
	}
}

void UFlyingTruckComponent::CreateRing()
{
	if( !bFlightRing || GetNetMode() == NM_DedicatedServer || RingComponent )
	{
		return;
	}
	USceneComponent* Root = GetOwner()->GetRootComponent();
	UStaticMesh* Mesh = RingMesh.LoadSynchronous();
	if( !Root || !Mesh )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: ring mesh %s not found." ), *GetOwner()->GetName(), *RingMesh.ToString() );
		return;
	}

	RingComponent = NewObject< UStaticMeshComponent >( GetOwner(), TEXT( "FlyingTruckRing" ) );
	RingComponent->SetupAttachment( Root );
	RingComponent->SetStaticMesh( Mesh );
	RingComponent->SetCollisionEnabled( ECollisionEnabled::NoCollision );
	RingComponent->SetCanEverAffectNavigation( false );
	RingComponent->SetCastShadow( false );
	RingComponent->SetRelativeLocation( RingOffset );

	// The mesh's outer diameter is its bounding box width; scale to RingDiameter.
	const float MeshDiameter = Mesh->GetBounds().BoxExtent.X * 2.f;
	const float Scale = MeshDiameter > KINDA_SMALL_NUMBER ? RingDiameter / MeshDiameter : 1.f;
	RingComponent->SetRelativeScale3D( FVector( Scale ) );

	RingMaterial = RingComponent->CreateDynamicMaterialInstance( 0 );
	RingGlowAlpha = -1.f; // force the first UpdateRing to set the colour
	RingComponent->SetVisibility( bRingAlwaysVisible );
	RingComponent->RegisterComponent();
}

void UFlyingTruckComponent::CreateWings()
{
	if( !bFlightWings || GetNetMode() == NM_DedicatedServer || WingPlates.Num() > 0 )
	{
		return;
	}
	USceneComponent* Root = GetOwner()->GetRootComponent();
	UStaticMesh* Mesh = WingMesh.LoadSynchronous();
	if( !Root || !Mesh )
	{
		UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: wing mesh %s not found." ), *GetOwner()->GetName(), *WingMesh.ToString() );
		return;
	}

	// The mesh's pivot is its front point; work out which way it extends (importers may mirror axes)
	// so the right wing goes back and to +Y, the left back and to -Y.
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const float SignX = Bounds.Origin.X <= 0.f ? 1.f : -1.f;
	const float SignY = Bounds.Origin.Y >= 0.f ? 1.f : -1.f;
	const float Chord = Bounds.BoxExtent.X * 2.f * WingScale;
	const float Span = Bounds.BoxExtent.Y * 2.f * WingScale;
	WingPlateThickness = Bounds.BoxExtent.Z * 2.f * WingScale;
	// Folding swings the tip in until the leading edge lies along the truck, plus a little extra.
	WingFoldYaw = WingFoldYawFor( Bounds, WingScale, WingFoldYawOverride, WingExtraFoldDegrees );
	WingLeadingEdgeLength = FMath::Sqrt( Chord * Chord + Span * Span );
	WingLeadingEdgeDir = FVector( -Chord, Span, 0.f ).GetSafeNormal();

	const int32 Plates = FMath::Clamp( WingPlateCount, 1, 6 );
	for( float Side : { 1.f, -1.f } )
	{
		for( int32 Index = 0; Index < Plates; ++Index )
		{
			const FName Name( *FString::Printf( TEXT( "FlyingTruckWing%s%d" ), Side > 0.f ? TEXT( "R" ) : TEXT( "L" ), Index ) );
			UStaticMeshComponent* Plate = NewObject< UStaticMeshComponent >( GetOwner(), Name );
			Plate->SetupAttachment( Root );
			Plate->SetStaticMesh( Mesh );
			Plate->SetCollisionEnabled( ECollisionEnabled::NoCollision );
			Plate->SetCanEverAffectNavigation( false );
			Plate->SetRelativeScale3D( FVector( SignX, SignY * Side, 1.f ) * WingScale );
			if( UMaterialInstanceDynamic* MID = Plate->CreateDynamicMaterialInstance( 0 ) )
			{
				MID->SetVectorParameterValue( TEXT( "Color" ), WingColor );
			}
			Plate->RegisterComponent();
			WingPlates.Add( Plate );
		}
	}
}

void UFlyingTruckComponent::UpdateWings( float DeltaTime )
{
	if( WingPlates.Num() == 0 )
	{
		return;
	}
	const float Target = bIsFlying ? 1.f : 0.f;
	if( DeltaTime > 0.f )
	{
		if( WingDeployAlpha == Target )
		{
			return;
		}
		const float Step = DeltaTime / FMath::Max( WingDeploySeconds, 0.01f );
		WingDeployAlpha = Target > WingDeployAlpha ? FMath::Min( WingDeployAlpha + Step, Target ) : FMath::Max( WingDeployAlpha - Step, Target );
	}
	// First half: the stacked plates swing out about the front point. Second half: they telescope
	// out along the leading edge. Folding plays it backwards.
	const float Swing = FMath::SmoothStep( 0.f, 0.5f, WingDeployAlpha );
	const float Slide = FMath::SmoothStep( 0.5f, 1.f, WingDeployAlpha );
	const float Yaw = WingFoldYaw * ( 1.f - Swing );
	const int32 PlatesPerSide = WingPlates.Num() / 2;

	for( int32 i = 0; i < WingPlates.Num(); ++i )
	{
		UStaticMeshComponent* Plate = WingPlates[ i ];
		if( !Plate )
		{
			continue;
		}
		const float Side = i < PlatesPerSide ? 1.f : -1.f;
		const int32 Index = i % PlatesPerSide;
		// Right wing extends to +Y; folding it inwards turns its tip towards -Y, which is a positive yaw.
		const FRotator Rot( 0.f, Yaw * Side, 0.f );
		const FVector SlideLocal = FVector( WingLeadingEdgeDir.X, WingLeadingEdgeDir.Y * Side, 0.f ) * ( WingLeadingEdgeLength * WingPlateSlideFraction * Index * Slide );
		// Each further plate sits just under the previous one so they nest when stacked.
		const FVector Pivot( WingPivot.X, WingPivot.Y * Side, WingPivot.Z - Index * WingPlateThickness * 1.05f );
		Plate->SetRelativeLocationAndRotation( Pivot + Rot.RotateVector( SlideLocal ), Rot );
	}
}

void UFlyingTruckComponent::UpdateRing( float DeltaTime )
{
	if( !RingComponent && FlightGlowParts.Num() == 0 )
	{
		return;
	}
	const float Target = bIsFlying ? 1.f : 0.f;
	const float Previous = RingGlowAlpha;
	if( RingGlowAlpha < 0.f || DeltaTime <= 0.f )
	{
		RingGlowAlpha = Target;
	}
	else
	{
		const float Step = DeltaTime / FMath::Max( RingFadeSeconds, 0.01f );
		RingGlowAlpha = Target > RingGlowAlpha ? FMath::Min( RingGlowAlpha + Step, Target ) : FMath::Max( RingGlowAlpha - Step, Target );
	}
	if( RingGlowAlpha != Previous )
	{
		const float A = FMath::SmoothStep( 0.f, 1.f, RingGlowAlpha );
		for( const TPair< int32, TWeakObjectPtr< UMaterialInstanceDynamic > >& Glow : FlightGlowParts )
		{
			if( UMaterialInstanceDynamic* MID = Glow.Value.Get() )
			{
				const FFlyingBodyPart& Part = BodyParts[ Glow.Key ];
				MID->SetVectorParameterValue( TEXT( "Color" ), FMath::Lerp( Part.IdleColor, Part.Color, A ) );
				MID->SetScalarParameterValue( TEXT( "Intensity" ), FMath::Lerp( 1.f, Part.GlowIntensity, A ) );
			}
		}
	}
	if( !RingComponent )
	{
		return;
	}
	if( RingGlowAlpha > 0.f )
	{
		// Spins up and down with the glow.
		RingComponent->AddLocalRotation( FRotator( 0.f, RingSpinDegreesPerSecond * RingGlowAlpha * DeltaTime, 0.f ) );
	}
	if( RingMaterial && RingGlowAlpha != Previous )
	{
		const float A = FMath::SmoothStep( 0.f, 1.f, RingGlowAlpha );
		RingMaterial->SetVectorParameterValue( TEXT( "Color" ), FMath::Lerp( RingIdleColor, GlowColor, A ) );
		RingMaterial->SetScalarParameterValue( TEXT( "Intensity" ), FMath::Lerp( 1.f, RingGlowIntensity, A ) );
	}
}

void UFlyingTruckComponent::HideVehicleMeshes()
{
	TArray< UMeshComponent* > Meshes;
	GetOwner()->GetComponents< UMeshComponent >( Meshes );
	for( UMeshComponent* Mesh : Meshes )
	{
		const bool bOurs = Mesh == RingComponent || WingPlates.Contains( Mesh ) || BodyPartComponents.Contains( Mesh )
			|| Mesh->ComponentHasTag( UFlyingCombatComponent::MeshTag );
		if( !bOurs && !Mesh->bHiddenInGame )
		{
			// Hidden only: the base vehicle's physics body and wheels keep working.
			Mesh->SetHiddenInGame( true );
		}
	}
}

void UFlyingTruckComponent::UpdateDriverVisibility()
{
	if( !bHideDriver || GetNetMode() == NM_DedicatedServer )
	{
		return;
	}
	const AFGDriveablePawn* Driveable = Cast< AFGDriveablePawn >( GetOwner() );
	AActor* Driver = Driveable ? Driveable->GetDriver() : nullptr;
	if( Driver != HiddenDriver.Get() )
	{
		ShowHiddenDriver();
		HiddenDriver = Driver;
	}
	if( !Driver )
	{
		return;
	}
	// Only this machine's view, nothing replicated; re-checked every frame in case the game shows them again.
	TArray< UMeshComponent* > Meshes;
	Driver->GetComponents< UMeshComponent >( Meshes );
	for( UMeshComponent* Mesh : Meshes )
	{
		if( !Mesh->bHiddenInGame )
		{
			Mesh->SetHiddenInGame( true );
			HiddenDriverMeshes.AddUnique( Mesh );
		}
	}
}

void UFlyingTruckComponent::ShowHiddenDriver()
{
	for( const TWeakObjectPtr< UMeshComponent >& Mesh : HiddenDriverMeshes )
	{
		if( Mesh.IsValid() )
		{
			Mesh->SetHiddenInGame( false );
		}
	}
	HiddenDriverMeshes.Reset();
	HiddenDriver = nullptr;
}

void UFlyingTruckComponent::CreateBodyParts()
{
	if( GetNetMode() == NM_DedicatedServer )
	{
		return;
	}
	if( bHideVehicleMesh )
	{
		HideVehicleMeshes();
		// The game can switch parts back on (e.g. when the vehicle comes back into view), so keep checking.
		GetWorld()->GetTimerManager().SetTimer( HideMeshTimer, this, &UFlyingTruckComponent::HideVehicleMeshes, 2.0f, true );
	}
	if( BodyParts.Num() == 0 || BodyPartComponents.Num() > 0 )
	{
		return;
	}
	USceneComponent* Root = GetOwner()->GetRootComponent();
	for( int32 i = 0; i < BodyParts.Num(); ++i )
	{
		const FFlyingBodyPart& Part = BodyParts[ i ];
		UStaticMesh* Mesh = Part.Mesh.LoadSynchronous();
		if( !Mesh || !Root )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: body part mesh %s not found." ), *GetOwner()->GetName(), *Part.Mesh.ToString() );
			continue;
		}
		UStaticMeshComponent* Comp = NewObject< UStaticMeshComponent >( GetOwner(), *FString::Printf( TEXT( "FlyingBodyPart%d" ), i ) );
		Comp->SetupAttachment( Root );
		Comp->SetStaticMesh( Mesh );
		Comp->SetCollisionEnabled( ECollisionEnabled::NoCollision );
		Comp->SetCanEverAffectNavigation( false );
		Comp->SetRelativeTransform( FTransform( Part.Rotation, Part.Location, Part.Scale ) );
		if( UMaterialInterface* Material = Part.Material.LoadSynchronous() )
		{
			Comp->SetMaterial( 0, Material );
		}
		if( UMaterialInstanceDynamic* MID = Comp->CreateDynamicMaterialInstance( 0 ) )
		{
			MID->SetVectorParameterValue( TEXT( "Color" ), Part.Color );
			if( Part.GlowIntensity > 0.f )
			{
				MID->SetScalarParameterValue( TEXT( "Intensity" ), Part.GlowIntensity );
			}
			if( Part.bGlowOnlyInFlight )
			{
				FlightGlowParts.Emplace( i, MID );
				RingGlowAlpha = -1.f; // force UpdateRing to set the starting colour
			}
		}
		Comp->RegisterComponent();
		BodyPartComponents.Add( Comp );
	}
}

const UFlyingTruckComponent* UFlyingTruckComponent::FindTemplate( const UClass* VehicleClass )
{
	for( const UClass* Class = VehicleClass; Class; Class = Class->GetSuperClass() )
	{
		const UBlueprintGeneratedClass* BPClass = Cast< UBlueprintGeneratedClass >( Class );
		if( !BPClass || !BPClass->SimpleConstructionScript )
		{
			continue;
		}
		for( const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes() )
		{
			if( const UFlyingTruckComponent* Template = Node ? Cast< UFlyingTruckComponent >( Node->ComponentTemplate ) : nullptr )
			{
				return Template;
			}
		}
	}
	return nullptr;
}

void UFlyingTruckComponent::GetParkedVisuals( const AActor* Vehicle, TArray< FFlyingParkedVisual >& OutVisuals ) const
{
	for( const FFlyingBodyPart& Part : BodyParts )
	{
		if( UStaticMesh* Mesh = Part.Mesh.LoadSynchronous() )
		{
			OutVisuals.Add( { Mesh, FTransform( Part.Rotation, Part.Location, Part.Scale ) } );
		}
	}

	// The same fitting BeginPlay applies, on copies.
	float FitRingDiameter = RingDiameter, FitWingScale = WingScale;
	FVector FitRingOffset = RingOffset, FitWingPivot = WingPivot;
	FVector OwnC, RefC, S;
	if( bScaleToVehicle && !ReferenceVehicle.IsNull() && ComputeVehicleFit( Vehicle, ReferenceVehicle, OwnC, RefC, S ) )
	{
		auto Map = [&]( const FVector& P ) { return OwnC + ( P - RefC ) * S; };
		FitRingDiameter *= S.Y;
		FitRingOffset = Map( FitRingOffset );
		if( bFitWings )
		{
			FitWingPivot = Map( FitWingPivot );
			FitWingScale *= FMath::Min( S.X, S.Y );
		}
	}
	FitWingScale *= WingScaleAdjust;
	FitWingPivot += WingPivotAdjust;

	if( bFlightRing && bRingAlwaysVisible )
	{
		if( UStaticMesh* Mesh = RingMesh.LoadSynchronous() )
		{
			const float MeshDiameter = Mesh->GetBounds().BoxExtent.X * 2.f;
			const float Scale = MeshDiameter > KINDA_SMALL_NUMBER ? FitRingDiameter / MeshDiameter : 1.f;
			OutVisuals.Add( { Mesh, FTransform( FRotator::ZeroRotator, FitRingOffset, FVector( Scale ) ) } );
		}
	}

	if( bFlightWings )
	{
		if( UStaticMesh* Mesh = WingMesh.LoadSynchronous() )
		{
			// Folded: every plate stacked at the pivot, swung in (matches UpdateWings at deploy 0).
			const FBoxSphereBounds Bounds = Mesh->GetBounds();
			const float SignX = Bounds.Origin.X <= 0.f ? 1.f : -1.f;
			const float SignY = Bounds.Origin.Y >= 0.f ? 1.f : -1.f;
			const float Thickness = Bounds.BoxExtent.Z * 2.f * FitWingScale;
			const float Yaw = WingFoldYawFor( Bounds, FitWingScale, WingFoldYawOverride, WingExtraFoldDegrees );
			const int32 Plates = FMath::Clamp( WingPlateCount, 1, 6 );
			for( float Side : { 1.f, -1.f } )
			{
				for( int32 Index = 0; Index < Plates; ++Index )
				{
					const FVector Pivot( FitWingPivot.X, FitWingPivot.Y * Side, FitWingPivot.Z - Index * Thickness * 1.05f );
					OutVisuals.Add( { Mesh, FTransform( FRotator( 0.f, Yaw * Side, 0.f ), Pivot, FVector( SignX, SignY * Side, 1.f ) * FitWingScale ) } );
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
