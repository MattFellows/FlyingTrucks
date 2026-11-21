// FlyingTrucks - a flyable variant of the Satisfactory truck.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FlyingTruckComponent.generated.h"

class APawn;
class AController;
class APlayerController;
class UPrimitiveComponent;
class UPointLightComponent;
class USpotLightComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UChaosVehicleMovementComponent;
class UEnhancedInputComponent;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FOnFlightModeChanged, bool, bIsFlying );

/** The driver's flight controls, sent from the driving client to the server. */
USTRUCT()
struct FFlyingTruckInput
{
	GENERATED_BODY()

	UPROPERTY()
	float Throttle = 0.f;

	UPROPERTY()
	float Brake = 0.f;

	/** -1 = left, +1 = right. */
	UPROPERTY()
	float Steer = 0.f;

	/** -1 = nose down, +1 = nose up. */
	UPROPERTY()
	float Pitch = 0.f;

	bool IsNearlyEqual( const FFlyingTruckInput& Other, float Tolerance = 0.05f ) const
	{
		return FMath::IsNearlyEqual( Throttle, Other.Throttle, Tolerance ) && FMath::IsNearlyEqual( Brake, Other.Brake, Tolerance )
			&& FMath::IsNearlyEqual( Steer, Other.Steer, Tolerance ) && FMath::IsNearlyEqual( Pitch, Other.Pitch, Tolerance );
	}
};

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
 * Flight needs Acclivity Stone in the truck's cargo: one stone is burned per minute of flight, and the
 * truck drops out of flight mode when the last one is used up. The server decides; the driver gets a
 * chat message when flight is refused or the stone runs out.
 *
 * Multiplayer: flight mode is replicated (so everyone sees the ring/glow). The driver's controls are
 * sent to the server and replicated on to the other players, and the flight forces run on every machine
 * (server, driver, onlookers) so each copy of the truck flies the same way between network corrections.
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
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

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

	/** Paint new Flying Trucks in their own colours (only while they still have the default paint). */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Paint" )
	bool bApplyDefaultPaint = true;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Paint" )
	FLinearColor DefaultPaintPrimary = FLinearColor( 0.02f, 0.30f, 0.95f );   // bright sky blue

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Paint" )
	FLinearColor DefaultPaintSecondary = FLinearColor( 0.90f, 0.90f, 0.90f ); // white trim

	/** Glow underneath the truck while flight mode is on. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	bool bFlightGlow = true;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	FLinearColor GlowColor = FLinearColor( 0.1f, 0.45f, 1.0f );

	/** Offset of the glow lights from the truck's origin, cm (truck-local). */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	FVector GlowOffset = FVector( 0.f, 0.f, 20.f );

	/** Underside glow (point light), unitless intensity. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	float GlowIntensity = 30000.f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	float GlowRadius = 900.f;

	/** Downward pool of light on the ground (spot light), unitless intensity. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	float GroundGlowIntensity = 80000.f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Glow" )
	float GroundGlowRange = 4000.f;

	/** Glowing ring under the truck, shown only in flight mode. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	bool bFlightRing = true;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	TSoftObjectPtr< UStaticMesh > RingMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath( TEXT( "/FlyingTrucks/Ring/SM_FlyingRing.SM_FlyingRing" ) ) );

	/** Outer size of the ring, cm (the truck is roughly 3.5-4 m wide). */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	float RingDiameter = 400.f;

	/** Ring centre relative to the truck's origin, cm (X forward, Z up). Back by ~1/4 radius, up by one tube thickness. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	FVector RingOffset = FVector( -50.f, 0.f, 63.f );

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	float RingSpinDegreesPerSecond = 40.f;

	/** Emissive strength of the ring's material (it glows by itself; bloom does the halo). */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Ring" )
	float RingGlowIntensity = 20.f;

	/** Delta wings that swing out of the truck's sides in flight mode. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	bool bFlightWings = true;

	/** Triangle with its pivot (front point) at the mesh origin, extending backwards and to one side. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	TSoftObjectPtr< UStaticMesh > WingMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath( TEXT( "/FlyingTrucks/Wings/SM_FlyingWing.SM_FlyingWing" ) ) );

	/** Right wing's pivot (its front point), truck-local cm; the left wing mirrors it. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	FVector WingPivot = FVector( 150.f, 130.f, 110.f );

	/** Extra inward swing when folded, degrees, so the tip ends up well inside the body. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	float WingExtraFoldDegrees = 3.f;

	/** Time to fully extend or fold, seconds. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	float WingDeploySeconds = 0.8f;

	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Wings" )
	FLinearColor WingColor = FLinearColor( 0.035f, 0.035f, 0.04f );

	/** Item that must be in the truck's cargo to fly (Acclivity Stone). Empty = no fuel needed. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Fuel" )
	TSoftClassPtr< UObject > FlightFuelItem = TSoftClassPtr< UObject >( FSoftObjectPath( TEXT( "/FlyingTrucks/Resources/Desc_AcclivityStone.Desc_AcclivityStone_C" ) ) );

	/** Seconds of flight per stone. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Fuel" )
	float FlightSecondsPerFuelItem = 60.f;

	/** Warn the driver this many seconds before the last stone is used up. */
	UPROPERTY( EditAnywhere, BlueprintReadWrite, Category = "Flight|Fuel" )
	float LowFuelWarningSeconds = 10.f;

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

	void CreateGlowLights();
	void CreateRing();
	void UpdateGlow();
	void CreateWings();
	void UpdateWings( float DeltaTime );

	/** Sets the custom paint via reflection (avoids depending on FFactoryCustomizationData's C++ layout). */
	void ApplyDefaultPaint();
	void ApplyFlightForces( UPrimitiveComponent* Body, float DeltaTime );

	// ---- Multiplayer ----
	UFUNCTION()
	void OnRep_IsFlying();

	/** Local effects of a flight-mode change (input context, glow/ring, log, event). */
	void ApplyFlightModeLocally();

	UFUNCTION( Server, Reliable )
	void Server_SetFlying( bool bNewFlying );

	UFUNCTION( Server, Unreliable )
	void Server_SetFlightInput( FFlyingTruckInput NewInput );

	FFlyingTruckInput GatherLocalInput() const;
	void SendInputToServer( float DeltaTime );

	// ---- Acclivity Stone (server decides) ----
	/** Stones in the truck's cargo (server). */
	int32 GetFuelCount() const;
	/** Server: burn fuel while flying, land when it runs out. */
	void TickFuel( float DeltaTime );
	/** Shows a chat line to whoever is driving (locally, or via a client RPC). */
	void NotifyDriver( const FText& Message );
	/** Adds a local system chat line on this machine. */
	void ShowLocalMessage( const FText& Message ) const;

	UFUNCTION( Client, Reliable )
	void Client_Notify( const FText& Message );

private:
	UPROPERTY( Transient, ReplicatedUsing = OnRep_IsFlying )
	bool bIsFlying = false;

	/** Server only: latest controls received from a remote driver. */
	FFlyingTruckInput RemoteInput;

	/** The driver's controls as seen by everyone except the driver (set by the server). */
	UPROPERTY( Transient, Replicated )
	FFlyingTruckInput ReplicatedInput;

	/** Driving client only: what was last sent to the server, and when. */
	FFlyingTruckInput LastSentInput;
	double LastInputSendTime = -1000.0;

	/** Server: flight time since the last stone was burned (kept across toggles so hopping is not free). */
	float FlightSecondsSinceBurn = 0.f;
	bool bLowFuelWarned = false;
	float FuelCheckTimer = 0.f;

	UPROPERTY( Transient )
	TObjectPtr< UClass > FuelItemClass;

	double LastToggleTime = -1000.0;
	bool bToggleKeyHeld = false;
	float DebugLogTimer = 0.f;
	FTimerHandle PaintTimer;

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
	TObjectPtr< UPointLightComponent > GlowLight;

	UPROPERTY( Transient )
	TObjectPtr< USpotLightComponent > GroundGlowLight;

	UPROPERTY( Transient )
	TObjectPtr< UStaticMeshComponent > RingComponent;

	UPROPERTY( Transient )
	TObjectPtr< UStaticMeshComponent > LeftWing;

	UPROPERTY( Transient )
	TObjectPtr< UStaticMeshComponent > RightWing;

	/** 0 = folded into the body, 1 = fully out. */
	float WingDeployAlpha = 0.f;
	/** Yaw that folds the right wing in (left uses the negative); from the mesh's shape. */
	float WingFoldYaw = 0.f;

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
