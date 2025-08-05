// Copyright VortexRequiem. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Terrain/TerrainGen.h" // For ETerrainPreset
#include "VortexRequiemGameInstance.generated.h"

UCLASS()
class VORTEXREQUIEM_API UVortexRequiemGameInstance : public UGameInstance
{
    GENERATED_BODY()

public:
    UVortexRequiemGameInstance();

    UPROPERTY(BlueprintReadWrite, Category = "Game")
    ETerrainPreset SelectedPreset;
};
