// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ShooterGameMode.generated.h"

class UShooterUI;

/**
 *  Simple GameMode for a first person shooter game
 *  Manages game UI
 *  Keeps track of team scores
 */
UCLASS(abstract)
class VORTEXREQUIEM_API AShooterGameMode : public AGameModeBase
{
	GENERATED_BODY()
	
protected:

	/** Type of UI widget to spawn */
	UPROPERTY(EditAnywhere, Category="Shooter")
	TSubclassOf<UShooterUI> ShooterUIClass;

	/** Pointer to the UI widget */
	TObjectPtr<UShooterUI> ShooterUI;

	/** Players waiting to spawn until terrain is ready */
	UPROPERTY()
	TArray<AController*> PendingSpawnPlayers;

	/** Track players that have already been spawned to prevent double spawning */
	UPROPERTY()
	TArray<AController*> SpawnedPlayers;

	/** Timer handle for checking terrain ready status */
	FTimerHandle TerrainCheckTimer;

public:

	AShooterGameMode();

protected:

	/** Gameplay initialization */
	virtual void BeginPlay() override;
	virtual void RestartPlayer(AController* NewPlayer) override;

	/** Check if terrain is ready and spawn pending players */
	void CheckTerrainAndSpawnPendingPlayers();

public:

	/** Increases the score for the given team */
	void IncrementTeamScore(uint8 TeamByte);
};
