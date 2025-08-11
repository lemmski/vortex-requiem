#pragma once

#include "CoreMinimal.h"

class AActor;

namespace TerrainPhysicsUtils
{
    // Temporarily disable physics simulation on all primitive components in the world except the excluded actor.
    void DisableActorPhysicsTemporarily(UWorld* World, AActor* ExcludeActor, TArray<TWeakObjectPtr<AActor>>& OutActorsToRestore);

    // Restore physics simulation on previously disabled actors.
    void RestoreActorPhysics(TArray<TWeakObjectPtr<AActor>>& ActorsToRestore);
}


