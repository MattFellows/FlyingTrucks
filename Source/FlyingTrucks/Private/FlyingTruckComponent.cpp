// FlyingTrucks - a flyable variant of the Satisfactory truck.

#include "FlyingTruckComponent.h"

#include "FlyingTrucks.h"
#include "ChaosVehicleMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"

UFlyingTruckComponent::UFlyingTruckComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UFlyingTruckComponent::BeginPlay()
{
	Super::BeginPlay();

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
}

void UFlyingTruckComponent::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	UnbindFromController();
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
		SetFlying( false );
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

	FlightContext = NewObject< UInputMappingContext >( this, TEXT( "IMC_FlyingTruck_Flight" ) );
	FlightContext->MapKey( PitchUpAction, EKeys::SpaceBar );
	FlightContext->MapKey( PitchUpAction, EKeys::Gamepad_FaceButton_Bottom );   // A
	FlightContext->MapKey( PitchDownAction, EKeys::LeftControl );
	FlightContext->MapKey( PitchDownAction, EKeys::Gamepad_FaceButton_Right );  // B
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
		FlightInputComponent->BindAction( PitchUpAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_PitchUpPressed );
		FlightInputComponent->BindAction( PitchUpAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_PitchUpReleased );
		FlightInputComponent->BindAction( PitchDownAction, ETriggerEvent::Started, this, &UFlyingTruckComponent::Input_PitchDownPressed );
		FlightInputComponent->BindAction( PitchDownAction, ETriggerEvent::Completed, this, &UFlyingTruckComponent::Input_PitchDownReleased );
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
	bPitchUpHeld = bPitchDownHeld = false;
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
		bPitchUpHeld = bPitchDownHeld = false;
	}
}

void UFlyingTruckComponent::Input_ToggleFlight( const FInputActionValue& Value )
{
	ToggleFlying();
}

void UFlyingTruckComponent::SetFlying( bool bNewFlying )
{
	if( bIsFlying == bNewFlying )
	{
		return;
	}
	bIsFlying = bNewFlying;
	UpdateFlightContext();
	UE_LOG( LogFlyingTrucks, Log, TEXT( "%s: flight mode %s." ), *GetOwner()->GetName(), bIsFlying ? TEXT( "ON" ) : TEXT( "OFF" ) );
	OnFlightModeChanged.Broadcast( bIsFlying );
}

void UFlyingTruckComponent::TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction )
{
	Super::TickComponent( DeltaTime, TickType, ThisTickFunction );

	if( !bIsFlying || DeltaTime <= 0.f )
	{
		return;
	}

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

	float Throttle = 0.f, Brake = 0.f, Steer = 0.f;
	if( VehicleMovement )
	{
		Throttle = VehicleMovement->GetThrottleInput();
		Brake = VehicleMovement->GetBrakeInput();
		Steer = VehicleMovement->GetSteeringInput();
	}
	const float Pitch = ( bPitchUpHeld ? 1.f : 0.f ) - ( bPitchDownHeld ? 1.f : 0.f );

	// ---- Linear: thrust, lift, drag, slip alignment (all as accelerations, mass-independent) ----
	const float Gravity = FMath::Abs( GetWorld()->GetGravityZ() );
	FVector Accel = FVector::ZeroVector;

	// The wheels still drive on the ground; extra thrust in the air keeps you moving.
	Accel += Fwd * ( Throttle - Brake * ReverseThrustFraction ) * ThrustAcceleration;

	// Lift ~ v^2, equals gravity at TakeoffSpeed, along the truck's up axis (so banking turns).
	const float SpeedRatio = FMath::Max( FwdSpeed, 0.f ) / FMath::Max( TakeoffSpeed, 1.f );
	Accel += Up * FMath::Min( SpeedRatio * SpeedRatio, MaxLiftG ) * Gravity;

	if( bAirborne )
	{
		Accel -= Velocity * LinearDrag;
		// Bleed off sideways and vertical slip so the truck flies where it points.
		const FVector Slip = Right * FVector::DotProduct( Velocity, Right ) + Up * FVector::DotProduct( Velocity, Up );
		Accel -= Slip * FMath::Min( VelocityAlignment, 1.f / DeltaTime );
		Accel += Fwd * Slip.Size() * FMath::Min( VelocityAlignment, 1.f / DeltaTime ) * 0.5f;
	}

	Body->AddForce( Accel, NAME_None, /*bAccelChange*/ true );

	// ---- Angular: rate control around the truck's own axes ----
	// Using FVector::CrossProduct conventions (X fwd, Y right, Z up):
	//   nose up    = rotation about -Right
	//   turn right = rotation about +Up
	//   bank right = rotation about -Fwd
	const FVector AngVel = FMath::DegreesToRadians( Body->GetPhysicsAngularVelocityInDegrees() );
	FVector DesiredAngVel = -Right * FMath::DegreesToRadians( Pitch * PitchRateDegrees );

	if( bAirborne )
	{
		const float CurrentBank = FMath::RadiansToDegrees( FMath::Asin( FMath::Clamp( -Right.Z, -1.f, 1.f ) ) );
		const float TargetBank = Steer * MaxBankDegrees;
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
