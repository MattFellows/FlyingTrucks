// Skyward - energy shield and particle guns for flying vehicles.

#include "FlyingCombatComponent.h"

#include "FlyingTrucks.h"
#include "FlyingTruckComponent.h"
#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Creature/FGCreature.h"
#include "DamageTypes/FGPointDamageType.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGVehicle.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "Patching/NativeHookManager.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "FlyingTrucks"

const FName UFlyingCombatComponent::MeshTag( TEXT( "SkywardCombatMesh" ) );

namespace
{
	UStaticMeshComponent* MakeCombatMesh( AActor* Owner, UStaticMesh* Mesh, UMaterialInterface* Material )
	{
		if( !Mesh || !Owner || !Owner->GetRootComponent() )
		{
			UE_LOG( LogFlyingTrucks, Warning, TEXT( "%s: combat mesh missing." ), *GetNameSafe( Owner ) );
			return nullptr;
		}
		UStaticMeshComponent* Comp = NewObject< UStaticMeshComponent >( Owner );
		Comp->ComponentTags.Add( UFlyingCombatComponent::MeshTag );
		Comp->SetupAttachment( Owner->GetRootComponent() );
		Comp->SetStaticMesh( Mesh );
		Comp->SetCollisionEnabled( ECollisionEnabled::NoCollision );
		Comp->SetCanEverAffectNavigation( false );
		Comp->SetCastShadow( false );
		if( Material )
		{
			Comp->SetMaterial( 0, Material );
		}
		return Comp;
	}
}

UFlyingCombatComponent::UFlyingCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	SetIsReplicatedByDefault( true );
}

void UFlyingCombatComponent::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( UFlyingCombatComponent, ShieldLevel );
}

void UFlyingCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	Flight = GetOwner()->FindComponentByClass< UFlyingTruckComponent >();
	BoltDamageType = LoadClass< UDamageType >( nullptr, TEXT( "/Game/FactoryGame/-Shared/Blueprint/DamageTypes/BP_PointDamageType_Energy.BP_PointDamageType_Energy_C" ) );
	if( !BoltDamageType )
	{
		BoltDamageType = UFGPointDamageType::StaticClass();
	}
	if( GetNetMode() == NM_DedicatedServer )
	{
		return;
	}

	ShieldComponent = MakeCombatMesh( GetOwner(), ShieldMesh.LoadSynchronous(), ShieldMaterial.LoadSynchronous() );
	if( ShieldComponent )
	{
		ShieldComponent->SetRelativeLocation( ShieldCenter );
		ShieldComponent->SetRelativeScale3D( ShieldRadii / 100.f );
		ShieldMID = ShieldComponent->CreateDynamicMaterialInstance( 0 );
		ShieldComponent->SetVisibility( false );
		ShieldComponent->RegisterComponent();
	}

	UStaticMesh* Ring = ReticleMesh.LoadSynchronous();
	ReticleComponent = MakeCombatMesh( GetOwner(), Ring, GlowMaterial.LoadSynchronous() );
	if( ReticleComponent )
	{
		// A ring well ahead of the nose, facing the pilot.
		const float Size = 260.f / FMath::Max( 1.f, float( Ring->GetBounds().BoxExtent.X * 2.0 ) );
		ReticleComponent->SetRelativeLocationAndRotation( FVector( ReticleDistance, 0.f, ShieldCenter.Z ), FRotator( 90.f, 0.f, 0.f ) );
		ReticleComponent->SetRelativeScale3D( FVector( Size ) );
		if( UMaterialInstanceDynamic* MID = ReticleComponent->CreateDynamicMaterialInstance( 0 ) )
		{
			MID->SetVectorParameterValue( TEXT( "Color" ), BoltColor );
			MID->SetScalarParameterValue( TEXT( "Intensity" ), 4.f );
		}
		ReticleComponent->SetVisibility( false );
		ReticleComponent->RegisterComponent();
	}
}

void UFlyingCombatComponent::EndPlay( const EEndPlayReason::Type EndPlayReason )
{
	Bolts.Reset();
	Super::EndPlay( EndPlayReason );
}

bool UFlyingCombatComponent::IsFlying() const
{
	return Flight && Flight->IsFlying();
}

bool UFlyingCombatComponent::IsKnockedOut() const
{
	return GetWorld()->GetTimeSeconds() < KnockedOutUntil;
}

void UFlyingCombatComponent::SetFiring( bool bNewFiring )
{
	if( bLocalFiring == bNewFiring )
	{
		return;
	}
	bLocalFiring = bNewFiring;
	if( GetOwner()->HasAuthority() )
	{
		bFiring = bNewFiring;
	}
	else
	{
		Server_SetFiring( bNewFiring );
	}
}

void UFlyingCombatComponent::Server_SetFiring_Implementation( bool bNewFiring )
{
	bFiring = bNewFiring;
}

void UFlyingCombatComponent::TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction )
{
	Super::TickComponent( DeltaTime, TickType, ThisTickFunction );
	if( DeltaTime <= 0.f )
	{
		return;
	}
	if( GetOwner()->HasAuthority() )
	{
		if( !IsFlying() )
		{
			bFiring = false;
		}
		ShotTimer = FMath::Max( 0.f, ShotTimer - DeltaTime );
		if( bFiring && ShotTimer <= 0.f )
		{
			ShotTimer = 1.f / FMath::Max( ShotsPerSecond, 0.1f );
			FireOnce();
		}
	}
	UpdateBolts( DeltaTime );
	UpdateShield( DeltaTime );
	UpdateReticle();
}

FVector UFlyingCombatComponent::AimDirection( const FVector& Start ) const
{
	const FTransform Ship = GetOwner()->GetActorTransform();
	const FVector Forward = GetOwner()->GetActorForwardVector();
	// The guns converge on the reticle.
	FVector Best = ( Ship.TransformPositionNoScale( FVector( ReticleDistance, 0.f, ShieldCenter.Z ) ) - Start ).GetSafeNormal();
	double BestDot = FMath::Cos( FMath::DegreesToRadians( AimAssistDegrees ) );
	const double Range = BoltSpeed * BoltSeconds;

	auto Consider = [&]( const FVector& Position, const FVector& Velocity )
	{
		const double Distance = FVector::Dist( Position, Start );
		if( Distance > Range || Distance < 300.0 )
		{
			return;
		}
		const double Dot = FVector::DotProduct( ( Position - Start ).GetSafeNormal(), Forward );
		if( Dot > BestDot )
		{
			BestDot = Dot;
			Best = ( Position + Velocity * ( Distance / BoltSpeed ) - Start ).GetSafeNormal(); // lead the target
		}
	};
	for( TObjectIterator< UFlyingCombatComponent > It; It; ++It )
	{
		if( *It != this && It->GetWorld() == GetWorld() && It->HasBegunPlay() && It->IsFlying() )
		{
			Consider( It->GetOwner()->GetActorTransform().TransformPositionNoScale( It->ShieldCenter ), It->GetOwner()->GetVelocity() );
		}
	}
	for( TActorIterator< AFGCreature > It( GetWorld() ); It; ++It )
	{
		if( It->IsAliveAndWell() )
		{
			Consider( It->GetActorLocation(), It->GetVelocity() );
		}
	}
	return Best;
}

void UFlyingCombatComponent::FireOnce()
{
	if( Muzzles.Num() == 0 )
	{
		return;
	}
	NextMuzzle = ( NextMuzzle + 1 ) % Muzzles.Num();
	const FVector Start = GetOwner()->GetActorTransform().TransformPositionNoScale( Muzzles[ NextMuzzle ] );
	Multicast_Fire( Start, AimDirection( Start ) * BoltSpeed );
}

void UFlyingCombatComponent::Multicast_Fire_Implementation( FVector_NetQuantize Start, FVector_NetQuantize Velocity )
{
	FBolt& Bolt = Bolts.AddDefaulted_GetRef();
	Bolt.Position = Start;
	Bolt.Velocity = Velocity;
	Bolt.Life = BoltSeconds;
	if( UStaticMeshComponent* Mesh = TakeBoltMesh() )
	{
		// A long thin streak: the mesh is a sphere of radius 100.
		Mesh->SetWorldLocationAndRotation( Bolt.Position, Bolt.Velocity.Rotation() );
		Mesh->SetWorldScale3D( FVector( 1.8f, 0.1f, 0.1f ) );
		Bolt.Mesh = Mesh;
	}
}

UStaticMeshComponent* UFlyingCombatComponent::TakeBoltMesh()
{
	if( GetNetMode() == NM_DedicatedServer )
	{
		return nullptr;
	}
	UStaticMeshComponent* Mesh = nullptr;
	if( FreeBoltMeshes.Num() > 0 )
	{
		Mesh = FreeBoltMeshes.Pop();
	}
	else
	{
		Mesh = MakeCombatMesh( GetOwner(), ShieldMesh.LoadSynchronous(), GlowMaterial.LoadSynchronous() );
		if( !Mesh )
		{
			return nullptr;
		}
		Mesh->SetUsingAbsoluteLocation( true );
		Mesh->SetUsingAbsoluteRotation( true );
		Mesh->SetUsingAbsoluteScale( true );
		if( UMaterialInstanceDynamic* MID = Mesh->CreateDynamicMaterialInstance( 0 ) )
		{
			MID->SetVectorParameterValue( TEXT( "Color" ), BoltColor );
			MID->SetScalarParameterValue( TEXT( "Intensity" ), 30.f );
		}
		Mesh->RegisterComponent();
	}
	Mesh->SetVisibility( true );
	return Mesh;
}

void UFlyingCombatComponent::ReturnBoltMesh( UStaticMeshComponent* Mesh )
{
	if( Mesh )
	{
		Mesh->SetVisibility( false );
		FreeBoltMeshes.Add( Mesh );
	}
}

void UFlyingCombatComponent::UpdateBolts( float DeltaTime )
{
	constexpr float BurstSeconds = 0.2f;
	// Hits can run game code, so never hold a reference into the array across TraceBolt.
	for( int32 i = Bolts.Num() - 1; i >= 0; --i )
	{
		UStaticMeshComponent* Mesh = Bolts[ i ].Mesh.Get();
		if( Bolts[ i ].BurstAge >= 0.f )
		{
			Bolts[ i ].BurstAge += DeltaTime;
			const float T = Bolts[ i ].BurstAge / BurstSeconds;
			if( T < 1.f && Mesh )
			{
				Mesh->SetWorldScale3D( FVector( 0.2f + 0.8f * FMath::Sin( T * UE_PI ) ) );
			}
			else
			{
				ReturnBoltMesh( Mesh );
				Bolts.RemoveAtSwap( i );
			}
			continue;
		}

		const FVector Start = Bolts[ i ].Position;
		const FVector End = Start + Bolts[ i ].Velocity * DeltaTime;
		FVector Hit = End;
		const bool bHit = TraceBolt( Start, End, Hit );
		Bolts[ i ].Position = End;
		Bolts[ i ].Life -= DeltaTime;
		if( bHit && Mesh )
		{
			Mesh->SetWorldLocationAndRotation( Hit, FRotator::ZeroRotator );
			Mesh->SetWorldScale3D( FVector( 0.2f ) );
			Bolts[ i ].BurstAge = 0.f;
		}
		else if( bHit || Bolts[ i ].Life <= 0.f )
		{
			ReturnBoltMesh( Mesh );
			Bolts.RemoveAtSwap( i );
		}
		else if( Mesh )
		{
			Mesh->SetWorldLocation( End );
		}
	}
}

float UFlyingCombatComponent::IntersectShield( const FVector& A, const FVector& B ) const
{
	// Work in a space where the bubble is a unit sphere. With the shield down only the ship itself can be hit.
	const FTransform Ship = GetOwner()->GetActorTransform();
	const FVector Radii = ShieldLevel > 0 ? ShieldRadii : ShieldRadii * 0.7f;
	const FVector P = ( Ship.InverseTransformPositionNoScale( A ) - ShieldCenter ) / Radii;
	const FVector D = ( Ship.InverseTransformPositionNoScale( B ) - ShieldCenter ) / Radii - P;
	const double Qa = D.SizeSquared();
	const double Qb = FVector::DotProduct( P, D );
	const double Qc = P.SizeSquared() - 1.0;
	if( Qc <= 0.0 )
	{
		return 0.f; // already inside
	}
	const double Disc = Qb * Qb - Qa * Qc;
	if( Qa < UE_SMALL_NUMBER || Disc < 0.0 )
	{
		return -1.f;
	}
	const double Time = ( -Qb - FMath::Sqrt( Disc ) ) / Qa;
	return Time >= 0.0 && Time <= 1.0 ? float( Time ) : -1.f;
}

bool UFlyingCombatComponent::TraceBolt( const FVector& A, const FVector& B, FVector& OutHit )
{
	float BestTime = 2.f;
	UFlyingCombatComponent* HitShip = nullptr;
	for( TObjectIterator< UFlyingCombatComponent > It; It; ++It )
	{
		if( *It == this || It->GetWorld() != GetWorld() || !It->HasBegunPlay() || !It->GetOwner() )
		{
			continue;
		}
		const float Time = It->IntersectShield( A, B );
		if( Time >= 0.f && Time < BestTime )
		{
			BestTime = Time;
			HitShip = *It;
		}
	}

	// Object queries find everything, trigger volumes included, so keep only creatures and solid things.
	FCollisionQueryParams Params( SCENE_QUERY_STAT( SkywardBolt ), false, GetOwner() );
	TArray< FHitResult > Hits;
	GetWorld()->LineTraceMultiByObjectType( Hits, A, B, FCollisionObjectQueryParams( FCollisionObjectQueryParams::AllObjects ), Params );
	const FHitResult* Best = nullptr;
	for( const FHitResult& Hit : Hits )
	{
		const AActor* Actor = Hit.GetActor();
		const UPrimitiveComponent* Comp = Hit.GetComponent();
		if( !Comp || Hit.Time >= BestTime || ( Actor && Actor->FindComponentByClass< UFlyingCombatComponent >() ) )
		{
			continue;
		}
		const bool bSolid = Comp->GetCollisionResponseToChannel( ECC_Visibility ) == ECR_Block
			&& Comp->GetCollisionResponseToChannel( ECC_Pawn ) == ECR_Block;
		const bool bCreatureBody = Actor && Actor->IsA< AFGCreature >() && Comp->GetCollisionResponseToChannel( ECC_Pawn ) == ECR_Block;
		if( bSolid || bCreatureBody )
		{
			Best = &Hit;
			BestTime = Hit.Time;
		}
	}

	const bool bAuthority = GetOwner()->HasAuthority();
	if( Best )
	{
		OutHit = Best->ImpactPoint;
		AActor* Victim = Best->GetActor();
		if( bAuthority && Victim && Victim->IsA< AFGCreature >() )
		{
			const APawn* Pawn = Cast< APawn >( GetOwner() );
			UGameplayStatics::ApplyPointDamage( Victim, CreatureDamage, ( B - A ).GetSafeNormal(), *Best,
				Pawn ? Pawn->GetController() : nullptr, GetOwner(), TSubclassOf< UDamageType >( BoltDamageType.Get() ) );
		}
		return true;
	}
	if( HitShip )
	{
		OutHit = FMath::Lerp( A, B, double( BestTime ) );
		if( bAuthority )
		{
			HitShip->TakeBoltHit( ShieldDamage );
		}
		return true;
	}
	return false;
}

void UFlyingCombatComponent::SetShield( float NewCharge )
{
	ShieldCharge = FMath::Clamp( NewCharge, 0.f, ShieldMax );
	// Round up, so clients see some shield until it is really empty.
	const uint8 NewLevel = uint8( FMath::Clamp( FMath::CeilToInt( ShieldCharge / FMath::Max( ShieldMax, 1.f ) * 255.f ), 0, 255 ) );
	if( NewLevel != ShieldLevel )
	{
		const uint8 OldLevel = ShieldLevel;
		ShieldLevel = NewLevel;
		OnRep_ShieldLevel( OldLevel ); // the server's own visuals
	}
}

void UFlyingCombatComponent::OnRep_ShieldLevel( uint8 OldLevel )
{
	if( ShieldLevel < OldLevel )
	{
		HitFlash = 1.f;
	}
}

bool UFlyingCombatComponent::AbsorbDamage( float Damage )
{
	if( Damage <= 0.f || Damage >= ShieldMaxAbsorb || ShieldCharge <= 0.f || !IsFlying() )
	{
		return false;
	}
	LastHitTime = GetWorld()->GetTimeSeconds();
	SetShield( ShieldCharge - Damage );
	if( ShieldCharge <= 0.f )
	{
		Flight->NotifyDriver( LOCTEXT( "ShieldDown", "Shield down!" ) );
	}
	return true;
}

void UFlyingCombatComponent::TakeBoltHit( float Damage )
{
	if( !IsFlying() || IsKnockedOut() || AbsorbDamage( Damage ) )
	{
		return; // parked and knocked-out ships can't be hurt
	}
	LastHitTime = GetWorld()->GetTimeSeconds();
	KnockedOutUntil = LastHitTime + KnockoutSeconds;
	Flight->NotifyDriver( FText::Format( LOCTEXT( "KnockedOut", "Hit! Engines out for {0} seconds." ), FText::AsNumber( FMath::CeilToInt( KnockoutSeconds ) ) ) );
	Flight->SetFlying( false );
}

void UFlyingCombatComponent::UpdateShield( float DeltaTime )
{
	if( GetOwner()->HasAuthority() && IsFlying() && ShieldCharge < ShieldMax && GetWorld()->GetTimeSeconds() - LastHitTime >= ShieldHitDelay )
	{
		SetShield( ShieldCharge + ShieldMax * DeltaTime / FMath::Max( ShieldChargeSeconds, 0.1f ) );
		if( ShieldCharge >= ShieldMax )
		{
			Flight->NotifyDriver( LOCTEXT( "ShieldFull", "Shield fully charged." ) );
		}
	}
	HitFlash = FMath::Max( 0.f, HitFlash - DeltaTime / 0.35f );
	if( !ShieldComponent )
	{
		return;
	}
	const bool bShow = IsFlying() && ( ShieldLevel > 0 || HitFlash > 0.f );
	if( ShieldComponent->GetVisibleFlag() != bShow )
	{
		ShieldComponent->SetVisibility( bShow );
	}
	if( bShow && ShieldMID )
	{
		// Charge shows as colour and strength; hits flare it up.
		const float Level = ShieldLevel / 255.f;
		ShieldMID->SetVectorParameterValue( TEXT( "Color" ), FMath::Lerp( ShieldLowColor, ShieldFullColor, Level ) );
		ShieldMID->SetScalarParameterValue( TEXT( "Opacity" ), 0.03f + 0.12f * Level + 0.3f * HitFlash );
	}
}

void UFlyingCombatComponent::UpdateReticle()
{
	if( !ReticleComponent )
	{
		return;
	}
	const APawn* Pawn = Cast< APawn >( GetOwner() );
	const APlayerController* PC = Pawn ? Cast< APlayerController >( Pawn->GetController() ) : nullptr;
	const bool bShow = IsFlying() && PC && PC->IsLocalController();
	if( ReticleComponent->GetVisibleFlag() != bShow )
	{
		ReticleComponent->SetVisibility( bShow );
	}
}

void FlyingTrucks_RegisterShieldHook()
{
#if !WITH_EDITOR
	AFGVehicle* Sample = GetMutableDefault< AFGVehicle >();
	SUBSCRIBE_METHOD_VIRTUAL( AFGVehicle::TakeDamage, Sample,
		[]( auto& Scope, AFGVehicle* Self, float Damage, FDamageEvent const& Event, AController* Instigator, AActor* Causer )
		{
			UFlyingCombatComponent* Combat = Self && Self->HasAuthority() ? Self->FindComponentByClass< UFlyingCombatComponent >() : nullptr;
			if( Combat && Combat->AbsorbDamage( Damage ) )
			{
				Scope.Override( 0.f );
			}
		} );
#endif
}

#undef LOCTEXT_NAMESPACE
