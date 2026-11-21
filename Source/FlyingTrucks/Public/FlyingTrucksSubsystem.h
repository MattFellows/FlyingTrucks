// FlyingTrucks - a flyable variant of the Satisfactory truck.

#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "FlyingTrucksSubsystem.generated.h"

class UFGSchematic;
class AFGResourceNodeBase;
class UPointLightComponent;

/**
 * World subsystem (registered in RootGameWorld_FlyingTrucks), spawned on the server and replicated.
 *
 * Server:
 *  - grants the mod's schematics as soon as a world is loaded (no unlock step needed);
 *  - once per save, turns some iron ore and limestone nodes into Acclivity Stone nodes. This uses the
 *    game's own per-node resource override, which is saved with the game and replicated to clients.
 * Every machine with a screen: puts a soft blue light on Acclivity Stone nodes so they can be found.
 */
UCLASS()
class FLYINGTRUCKS_API AFlyingTrucksSubsystem : public AModSubsystem
{
	GENERATED_BODY()

public:
	AFlyingTrucksSubsystem();

	/** Schematics to grant automatically. */
	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks" )
	TArray< TSoftClassPtr< UFGSchematic > > AutoGrantSchematics;

	/** The resource some nodes are converted to. */
	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	TSoftClassPtr< UObject > AcclivityResource;

	/** Class names of the resources whose nodes may be converted. */
	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	TArray< FString > ConvertibleResourceClassNames;

	/** Roughly one in this many convertible nodes becomes Acclivity Stone. */
	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	int32 ConvertOneNodeIn = 8;

	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	FLinearColor NodeGlowColor = FLinearColor( 0.1f, 0.45f, 1.0f );

	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	float NodeGlowIntensity = 40000.f;

	UPROPERTY( EditDefaultsOnly, Category = "FlyingTrucks|Acclivity Stone" )
	float NodeGlowRadius = 1500.f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type EndPlayReason ) override;

private:
	void TryGrantSchematics();
	void TryConvertNodes();
	void RefreshNodeGlows();
	bool SetNodeResourceOverride( AFGResourceNodeBase* Node, UClass* Resource ) const;

	FTimerHandle GrantTimer;
	int32 GrantAttempts = 0;

	FTimerHandle ConvertTimer;
	int32 ConvertAttempts = 0;

	FTimerHandle GlowTimer;

	UPROPERTY( Transient )
	TObjectPtr< UClass > AcclivityClass;

	TMap< TWeakObjectPtr< AFGResourceNodeBase >, TWeakObjectPtr< UPointLightComponent > > NodeGlows;
};
