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
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

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

	if( bApplyDefaultPaint && GetOwner()->HasAuthority() )
	{
		// A freshly built vehicle gets its customization from the build gun after BeginPlay, so wait a moment.
		GetWorld()->GetTimerManager().SetTimer( PaintTimer, this, &UFlyingTruckComponent::ApplyDefaultPaint, 1.0f, false );
	}
}

void UFlyingTruckComponent::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	UnbindFromController();
	if( UWorld* World = GetWorld() )
	{
		World->GetTimerManager().ClearTimer( PaintTimer );
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
	ThrottleInput = BrakeInput = SteerLeftInput = SteerRightInput = SteerAxisInput = 0.f;
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
	if( BoundController )
	{
		// Our own flight-mode bindings: the game's vehicle input freezes while airborne.
		Throttle = ThrottleInput;
		Brake = BrakeInput;
		Steer = FMath::Clamp( SteerRightInput - SteerLeftInput + SteerAxisInput, -1.f, 1.f );
	}
	else if( VehicleMovement )
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
