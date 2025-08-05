#pragma once

#include "CoreMinimal.h"
#include "TerrainTypes.generated.h"

UENUM(BlueprintType)
enum class ETerrainPreset : uint8
{
    None                     UMETA(DisplayName="None"),
    DowntownRuins            UMETA(DisplayName="Downtown Ruins"),
    CrystallineBloomfallZone UMETA(DisplayName="Crystalline Bloomfall Zone"),
    MutatedSwamplands        UMETA(DisplayName="Mutated Swamplands"),
    IrradiatedBadlands       UMETA(DisplayName="Irradiated Badlands"),
    OldWorldAnomaly          UMETA(DisplayName="Old World Anomaly"),
    GothicCathedralApproach  UMETA(DisplayName="Gothic Cathedral Approach"),
    MangroveDeltaFull        UMETA(DisplayName="Mangrove Delta Full"),
    ProvingGroundsSmall      UMETA(DisplayName="Proving Grounds Small"),
    ArenaTiny513             UMETA(DisplayName="Arena Tiny 513")
};
