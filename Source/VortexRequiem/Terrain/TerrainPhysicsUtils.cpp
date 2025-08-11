#include "TerrainPhysicsUtils.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"

void TerrainPhysicsUtils::DisableActorPhysicsTemporarily(UWorld* World, AActor* ExcludeActor, TArray<TWeakObjectPtr<AActor>>& OutActorsToRestore)
{
    if (!World) return;
    OutActorsToRestore.Empty();
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || Actor == ExcludeActor) continue;
        TArray<UPrimitiveComponent*> PrimitiveComponents;
        Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
        bool bHadPhysics = false;
        for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
        {
            if (PrimComp && PrimComp->IsSimulatingPhysics())
            {
                PrimComp->SetSimulatePhysics(false);
                bHadPhysics = true;
            }
        }
        if (bHadPhysics)
        {
            OutActorsToRestore.Add(Actor);
        }
    }
}

void TerrainPhysicsUtils::RestoreActorPhysics(TArray<TWeakObjectPtr<AActor>>& ActorsToRestore)
{
    for (const TWeakObjectPtr<AActor>& WeakActor : ActorsToRestore)
    {
        if (AActor* Actor = WeakActor.Get())
        {
            TArray<UPrimitiveComponent*> PrimitiveComponents;
            Actor->GetComponents<UPrimitiveComponent>(PrimitiveComponents);
            for (UPrimitiveComponent* PrimComp : PrimitiveComponents)
            {
                if (PrimComp)
                {
                    PrimComp->SetSimulatePhysics(true);
                }
            }
        }
    }
    ActorsToRestore.Empty();
}


