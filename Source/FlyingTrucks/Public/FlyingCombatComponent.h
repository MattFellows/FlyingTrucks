// Skyward - energy shield and particle guns for flying vehicles.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "FlyingCombatComponent.generated.h"

class UFlyingTruckComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Add next to a UFlyingTruckComponent. The server decides every hit; other machines simulate the same bolts for show.
 *  - Shield: charges slowly in flight, keeps its charge when parked, and absorbs damage to the vehicle. A bolt that
 *    gets through an empty shield knocks the engines out for a while; bolts never destroy the vehicle.
 *  - Guns: hold fire in flight. Bolts hurt creatures and drain other shields, but not players on foot or buildings.
 */
UCLASS( ClassGroup = ( FlyingTrucks ), meta = ( BlueprintSpawnableComponent ) )
class FLYINGTRUCKS_API UFlyingCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFlyingCombatComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;
	virtual void TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction ) override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/** Local driver pressed/released the trigger. */
	void SetFiring( bool bNewFiring );
	/** Server: engines are out after a hit on an empty shield. */
	bool IsKnockedOut() const;
	/** Server: damage to the vehicle. True if the shield swallowed it. */
	bool AbsorbDamage( float Damage );
	/** Marks our own meshes, so the flight component doesn't hide them with the base vehicle's. */
	static const FName MeshTag;

	UPROPERTY( EditAnywhere, Category = "Shield" )
	float ShieldMax = 100.f;
	/** Seconds to charge from empty to full. */
	UPROPERTY( EditAnywhere, Category = "Shield" )
	float ShieldChargeSeconds = 60.f;
	/** Charging pauses this long after a hit. */
	UPROPERTY( EditAnywhere, Category = "Shield" )
	float ShieldHitDelay = 4.f;
	UPROPERTY( EditAnywhere, Category = "Shield" )
	float KnockoutSeconds = 5.f;
	/** Single hits at least this big go through (e.g. falling out of the world). */
	UPROPERTY( EditAnywhere, Category = "Shield" )
	float ShieldMaxAbsorb = 1000.f;
	UPROPERTY( EditAnywhere, Category = "Shield" )
	FVector ShieldCenter = FVector( -10.f, 0.f, 130.f );
	UPROPERTY( EditAnywhere, Category = "Shield" )
	FVector ShieldRadii = FVector( 480.f, 520.f, 200.f );
	/** Sphere of radius 100 cm. */
	UPROPERTY( EditAnywhere, Category = "Shield" )
	TSoftObjectPtr< UStaticMesh > ShieldMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath( TEXT( "/Skyward/Combat/SM_Shield.SM_Shield" ) ) );
	UPROPERTY( EditAnywhere, Category = "Shield" )
	TSoftObjectPtr< UMaterialInterface > ShieldMaterial = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath( TEXT( "/Skyward/Combat/M_Shield.M_Shield" ) ) );
	UPROPERTY( EditAnywhere, Category = "Shield" )
	FLinearColor ShieldFullColor = FLinearColor( 0.1f, 0.5f, 1.f );
	UPROPERTY( EditAnywhere, Category = "Shield" )
	FLinearColor ShieldLowColor = FLinearColor( 1.f, 0.12f, 0.05f );

	/** The guns, vehicle-local; they take turns. */
	UPROPERTY( EditAnywhere, Category = "Guns" )
	TArray< FVector > Muzzles = { FVector( 350.f, 131.f, 115.f ), FVector( 350.f, -131.f, 115.f ) };
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float ShotsPerSecond = 6.f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float BoltSpeed = 20000.f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float BoltSeconds = 1.75f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float ShieldDamage = 8.f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float CreatureDamage = 12.f;
	/** Shots bend towards a target within this angle of the nose, degrees. */
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float AimAssistDegrees = 4.f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	TSoftObjectPtr< UMaterialInterface > GlowMaterial = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath( TEXT( "/Skyward/Ring/M_FlyingRingGlow.M_FlyingRingGlow" ) ) );
	UPROPERTY( EditAnywhere, Category = "Guns" )
	TSoftObjectPtr< UStaticMesh > ReticleMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath( TEXT( "/Skyward/Ring/SM_FlyingRing.SM_FlyingRing" ) ) );
	UPROPERTY( EditAnywhere, Category = "Guns" )
	float ReticleDistance = 8000.f;
	UPROPERTY( EditAnywhere, Category = "Guns" )
	FLinearColor BoltColor = FLinearColor( 0.15f, 0.7f, 1.f );

protected:
	UFUNCTION( Server, Reliable )
	void Server_SetFiring( bool bNewFiring );
	UFUNCTION( NetMulticast, Unreliable )
	void Multicast_Fire( FVector_NetQuantize Start, FVector_NetQuantize Velocity );
	UFUNCTION()
	void OnRep_ShieldLevel( uint8 OldLevel );

private:
	/** A bolt in flight, or (Age >= 0) the short burst where it hit. */
	struct FBolt
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Life = 0.f;
		float BurstAge = -1.f;
		TWeakObjectPtr< UStaticMeshComponent > Mesh;
	};

	bool IsFlying() const;
	void FireOnce();
	FVector AimDirection( const FVector& Start ) const;
	void UpdateBolts( float DeltaTime );
	void UpdateShield( float DeltaTime );
	void UpdateReticle();
	/** Moves a bolt from A to B; true if it hit something. Applies damage on the server. */
	bool TraceBolt( const FVector& A, const FVector& B, FVector& OutHit );
	/** Where a segment enters this ship's bubble, 0..1; negative if it misses. */
	float IntersectShield( const FVector& A, const FVector& B ) const;
	void TakeBoltHit( float Damage );
	void SetShield( float NewCharge );
	UStaticMeshComponent* TakeBoltMesh();
	void ReturnBoltMesh( UStaticMeshComponent* Mesh );

	UPROPERTY( Transient )
	TObjectPtr< UFlyingTruckComponent > Flight;
	UPROPERTY( Transient )
	TObjectPtr< UStaticMeshComponent > ShieldComponent;
	UPROPERTY( Transient )
	TObjectPtr< UMaterialInstanceDynamic > ShieldMID;
	UPROPERTY( Transient )
	TObjectPtr< UStaticMeshComponent > ReticleComponent;
	UPROPERTY( Transient )
	TArray< TObjectPtr< UStaticMeshComponent > > FreeBoltMeshes;
	UPROPERTY( Transient )
	TObjectPtr< UClass > BoltDamageType;

	/** 0..255 of ShieldMax. */
	UPROPERTY( Transient, ReplicatedUsing = OnRep_ShieldLevel )
	uint8 ShieldLevel = 0;

	float ShieldCharge = 0.f; // server
	double LastHitTime = -1000.0;
	double KnockedOutUntil = -1000.0;
	bool bFiring = false;      // server
	bool bLocalFiring = false; // driver
	float ShotTimer = 0.f;
	int32 NextMuzzle = 0;
	float HitFlash = 0.f;
	TArray< FBolt > Bolts;
};

/** Hooks vehicle damage so shields can absorb it. Call once at startup. */
void FlyingTrucks_RegisterShieldHook();
