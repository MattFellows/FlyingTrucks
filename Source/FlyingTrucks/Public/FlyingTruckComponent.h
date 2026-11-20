// FlyingTrucks - a flyable variant of the Satisfactory truck.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FlyingTruckComponent.generated.h"

class APawn;
class AController;
class APlayerController;
class UPrimitiveComponent;
class UChaosVehicleMovementComponent;
class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FOnFlightModeChanged, bool, bIsFlying );

/**
 * Add to a wheeled vehicle Blueprint (BP_FlyingTruck) to let the driver fly it like a plane.
 *
 * Only uses engine APIs (pawn, physics body, Chaos vehicle inputs, Enhanced Input), so it does not
 * depend on the layout of Satisfactory's own vehicle classes.
 *
 * Controls while driving:
 *   F / Gamepad Y            toggle flight mode
 *   Space / Gamepad A        pitch nose up   (flight mode only)
 *   C, Left Ctrl / Gamepad B pitch nose down (flight mode only)
 *   W / Right trigger        thrust;  S / Left trigger: air brake / reverse thrust
 *   A, D / Left stick        bank and turn
 *   (read by this component in flight mode, and still passed on to the truck)
 *
 * Single-player first: forces are applied on the machine controlling the vehicle.
 */
UCLASS( ClassGroup = ( FlyingTrucks ), meta = ( BlueprintSpawnableComponent ) )
class FLYINGTRUCKS_API UFlyingTruckComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFlyingTruckComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;
	virtual void TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction ) override;

	UFUNCTION( BlueprintPure, Category = "Flight" )
	bool IsFlying() const { return bIsFlying; }

	UFUNCTION( BlueprintCallable, Category = "Flight" )
	void SetFlying( bool bNewFlying );

	UFUNCTION( BlueprintCallable, Category = "Flight" )
	void ToggleFlying() { SetFlying( !bIsFlying ); }

	UPROPERTY( BlueprintAssignable, Category = "Flight" )
	FOnFlightModeChanged OnFlightModeChanged;

	/** Forward acceleration at full throttle, cm/s^2. Top speed is roughly ThrustAcceleration / LinearDrag. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float ThrustAcceleration = 1600.f;

	/** Fraction of thrust available when braking/reversing. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float ReverseThrustFraction = 0.5f;

	/** Linear air drag, 1/s. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float LinearDrag = 0.25f;

	/** Forward speed (cm/s) at which lift exactly cancels gravity. 2200 cm/s = ~80 km/h. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float TakeoffSpeed = 2200.f;

	/** Cap on lift, in multiples of gravity. 1 = level flight holds altitude above TakeoffSpeed; climb by pitching up. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float MaxLiftG = 1.0f;

	/** How quickly sideways / vertical slip is turned into forward motion, 1/s. Higher = more "on rails". */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float VelocityAlignment = 1.5f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float PitchRateDegrees = 45.f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float YawRateDegrees = 35.f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float MaxBankDegrees = 35.f;

	/** How hard the roll is pulled towards the target bank angle, 1/s. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float RollGain = 3.f;

	/** How quickly actual rotation rates follow the desired ones, 1/s. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float AngularResponse = 6.f;

	/** Ground probe length below the vehicle's root, cm. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Tuning" )
	float GroundCheckDistance = 300.f;

	/** Minimum time between flight toggles, seconds. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Input" )
	float ToggleCooldownSeconds = 0.5f;

	/** Priority of the mod's input mapping contexts (above the vehicle's own). */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Input" )
	int32 InputPriority = 1000;

protected:
	UFUNCTION()
	void HandleControllerChanged( APawn* Pawn, AController* OldController, AController* NewController );

	void BindToController( APlayerController* PlayerController );
	void UnbindFromController();
	void UpdateFlightContext();
	void CreateInputObjects();

	void Input_ToggleFlight( const FInputActionValue& Value );
	void Input_ToggleReleased( const FInputActionValue& Value );
	// Flight-mode copies of the driving controls (the game stops updating its own vehicle input in the air).
	void Input_Throttle( const FInputActionValue& Value );
	void Input_ThrottleReleased( const FInputActionValue& Value ) { ThrottleInput = 0.f; }
	void Input_Brake( const FInputActionValue& Value );
	void Input_BrakeReleased( const FInputActionValue& Value ) { BrakeInput = 0.f; }
	void Input_SteerLeft( const FInputActionValue& Value );
	void Input_SteerLeftReleased( const FInputActionValue& Value ) { SteerLeftInput = 0.f; }
	void Input_SteerRight( const FInputActionValue& Value );
	void Input_SteerRightReleased( const FInputActionValue& Value ) { SteerRightInput = 0.f; }
	void Input_SteerAxis( const FInputActionValue& Value );
	void Input_SteerAxisReleased( const FInputActionValue& Value ) { SteerAxisInput = 0.f; }
	void ResetFlightInputs();

	void Input_PitchUpPressed( const FInputActionValue& Value ) { bPitchUpHeld = true; }
	void Input_PitchUpReleased( const FInputActionValue& Value ) { bPitchUpHeld = false; }
	void Input_PitchDownPressed( const FInputActionValue& Value ) { bPitchDownHeld = true; }
	void Input_PitchDownReleased( const FInputActionValue& Value ) { bPitchDownHeld = false; }

	bool IsAirborne( const UPrimitiveComponent* Body ) const;
	void ApplyFlightForces( UPrimitiveComponent* Body, float DeltaTime );

private:
	UPROPERTY( Transient )
	bool bIsFlying = false;

	double LastToggleTime = -1000.0;
	bool bToggleKeyHeld = false;
	float DebugLogTimer = 0.f;

	float ThrottleInput = 0.f;
	float BrakeInput = 0.f;
	float SteerLeftInput = 0.f;
	float SteerRightInput = 0.f;
	float SteerAxisInput = 0.f;

	bool bPitchUpHeld = false;
	bool bPitchDownHeld = false;

	UPROPERTY( Transient )
	TObjectPtr< UChaosVehicleMovementComponent > VehicleMovement;

	UPROPERTY( Transient )
	TObjectPtr< UEnhancedInputComponent > FlightInputComponent;

	UPROPERTY( Transient )
	TObjectPtr< APlayerController > BoundController;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > ToggleAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > PitchUpAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > PitchDownAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > ThrottleAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > BrakeAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > SteerLeftAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > SteerRightAction;

	UPROPERTY( Transient )
	TObjectPtr< UInputAction > SteerAxisAction;

	/** Always active while driving: flight toggle. */
	UPROPERTY( Transient )
	TObjectPtr< UInputMappingContext > ToggleContext;

	/** Active only while flying: pitch keys (overrides handbrake etc.). */
	UPROPERTY( Transient )
	TObjectPtr< UInputMappingContext > FlightContext;
};
