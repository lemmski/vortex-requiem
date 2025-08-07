// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "ShooterGameState.generated.h"

USTRUCT(BlueprintType)
struct FTeamScore
{
    GENERATED_BODY()

    UPROPERTY()
    uint8 TeamID;

    UPROPERTY()
    int32 Score;
};


/**
 * 
 */
UCLASS()
class VORTEXREQUIEM_API AShooterGameState : public AGameStateBase
{
	GENERATED_BODY()

public:

	UPROPERTY(ReplicatedUsing=OnRep_TeamScores)
	TArray<FTeamScore> TeamScores;

    UFUNCTION()
    void OnRep_TeamScores();

    DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnTeamScoresChanged);
    UPROPERTY(BlueprintAssignable, Category = "Scores")
    FOnTeamScoresChanged OnTeamScoresChangedDelegate;
};
