// Copyright Epic Games, Inc. All Rights Reserved.


#include "Variant_Shooter/ShooterGameMode.h"
#include "ShooterUI.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "ShooterGameState.h"
#include "Terrain/TerrainGen.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Engine/EngineTypes.h"
#include "TimerManager.h"

AShooterGameMode::AShooterGameMode()
{
	GameStateClass = AShooterGameState::StaticClass();
}

void AShooterGameMode::BeginPlay()
{
	Super::BeginPlay();

	// create the UI
	//ShooterUI = CreateWidget<UShooterUI>(UGameplayStatics::GetPlayerController(GetWorld(), 0), ShooterUIClass);
	//ShooterUI->AddToViewport(0);

	// Start checking for terrain readiness if we're the server
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(TerrainCheckTimer, this, &AShooterGameMode::CheckTerrainAndSpawnPendingPlayers, 0.5f, true);
	}
}

void AShooterGameMode::RestartPlayer(AController* NewPlayer)
{
	if (NewPlayer == nullptr || NewPlayer->IsPendingKillPending())
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - NewPlayer is null or pending kill"));
		return;
	}

	// Get more detailed information about the player
	FString PlayerInfo = FString::Printf(TEXT("Name=%s, NetMode=%d"), 
		*NewPlayer->GetName(), 
		(int32)GetNetMode());
	
	if (NewPlayer->GetPawn())
	{
		PlayerInfo += FString::Printf(TEXT(", HasPawn=true, PawnLocation=%s"), 
			*NewPlayer->GetPawn()->GetActorLocation().ToString());
	}
	else
	{
		PlayerInfo += TEXT(", HasPawn=false");
	}
	
	UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Called for player: %s"), *PlayerInfo);

	// Check if this player was already spawned
	if (SpawnedPlayers.Contains(NewPlayer))
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Player %s was already spawned! Ignoring duplicate call."), *NewPlayer->GetName());
		return;
	}

	// First, check if we have a TerrainGen actor
	ATerrainGen* TerrainGen = Cast<ATerrainGen>(UGameplayStatics::GetActorOfClass(GetWorld(), ATerrainGen::StaticClass()));
	
	if (TerrainGen)
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - TerrainGen found. IsTerrainReady=%s, SpawnPoints=%d"), 
			TerrainGen->IsTerrainReady() ? TEXT("true") : TEXT("false"), TerrainGen->SpawnPoints.Num());
		
		if (TerrainGen->IsTerrainReady() && TerrainGen->SpawnPoints.Num() > 0)
		{
			            // Terrain is ready, spawn at terrain spawn point
            const int32 SpawnIndex = FMath::RandRange(0, TerrainGen->SpawnPoints.Num() - 1);
            FVector SpawnLocation = TerrainGen->SpawnPoints[SpawnIndex];
            
            // Get default pawn class to determine capsule height
            float CapsuleHalfHeight = 88.0f; // Default character capsule half height
            if (DefaultPawnClass)
            {
                const ACharacter* DefaultCharacter = DefaultPawnClass->GetDefaultObject<ACharacter>();
                if (DefaultCharacter && DefaultCharacter->GetCapsuleComponent())
                {
                    CapsuleHalfHeight = DefaultCharacter->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                }
            }
            
            // Spawn slightly above the terrain surface to ensure we're not stuck
            SpawnLocation.Z += CapsuleHalfHeight + 10.0f;

            UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Spawning player %s at TerrainGen point %d (%s) with capsule offset %.2f"), 
                *NewPlayer->GetName(), SpawnIndex, *SpawnLocation.ToString(), CapsuleHalfHeight + 10.0f);

            FRotator StartRotation(ForceInit);
            StartRotation.Yaw = FMath::RandRange(0.0f, 360.0f);
            StartRotation.Pitch = 0.f;
            StartRotation.Roll = 0.f;
            FTransform StartTransform(StartRotation, SpawnLocation);
			
			RestartPlayerAtTransform(NewPlayer, StartTransform);
			
			// Mark this player as spawned
			SpawnedPlayers.Add(NewPlayer);
			UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Player %s spawned successfully at terrain point"), *NewPlayer->GetName());
			return;
		}
		else
		{
			// TerrainGen exists but is not ready - add to pending players
			UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - TerrainGen not ready yet! Adding player to pending spawn list..."));
			
			// Check if player already has a pawn (shouldn't happen but let's be safe)
			if (NewPlayer->GetPawn())
			{
				UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Player %s already has a pawn! Not adding to pending list."), *NewPlayer->GetName());
				return;
			}
			
			if (!PendingSpawnPlayers.Contains(NewPlayer))
			{
				PendingSpawnPlayers.Add(NewPlayer);
				UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Added player %s to pending list. Total pending: %d"), 
					*NewPlayer->GetName(), PendingSpawnPlayers.Num());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Player %s already in pending list!"), *NewPlayer->GetName());
			}
			return;
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - No TerrainGen actor found, checking for PlayerStart actors..."));
	}

	// Fallback to PlayerStart actors if no TerrainGen
	AActor* StartSpot = FindPlayerStart(NewPlayer);

	if (StartSpot == nullptr)
	{
		TArray<AActor*> PlayerStarts;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(), APlayerStart::StaticClass(), PlayerStarts);

		if (PlayerStarts.Num() > 0)
		{
			StartSpot = PlayerStarts[FMath::RandRange(0, PlayerStarts.Num() - 1)];
			UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Found %d PlayerStart actors, using one"), PlayerStarts.Num());
		}
	}
	
	if (StartSpot)
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Using PlayerStart actor for spawn"));
		RestartPlayerAtPlayerStart(NewPlayer, StartSpot);
		
		// Mark this player as spawned
		SpawnedPlayers.Add(NewPlayer);
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::RestartPlayer - Player %s spawned successfully at PlayerStart"), *NewPlayer->GetName());
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ShooterGameMode::RestartPlayer - No spawn location found! Player will not spawn."));
	}
}

void AShooterGameMode::CheckTerrainAndSpawnPendingPlayers()
{
	if (PendingSpawnPlayers.Num() == 0)
	{
		// No pending players, we can stop checking
		GetWorldTimerManager().ClearTimer(TerrainCheckTimer);
		return;
	}

	ATerrainGen* TerrainGen = Cast<ATerrainGen>(UGameplayStatics::GetActorOfClass(GetWorld(), ATerrainGen::StaticClass()));
	if (TerrainGen && TerrainGen->IsTerrainReady() && TerrainGen->SpawnPoints.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::CheckTerrainAndSpawnPendingPlayers - Terrain is ready! Spawning %d pending players"), PendingSpawnPlayers.Num());
		
		// List pending players before spawning
		for (int32 i = 0; i < PendingSpawnPlayers.Num(); i++)
		{
			if (IsValid(PendingSpawnPlayers[i]))
			{
				UE_LOG(LogTemp, Warning, TEXT("  Pending[%d]: %s (HasPawn=%s)"), 
					i, 
					*PendingSpawnPlayers[i]->GetName(),
					PendingSpawnPlayers[i]->GetPawn() ? TEXT("true") : TEXT("false"));
			}
		}
		
		// Spawn all pending players
		TArray<AController*> PlayersToSpawn = PendingSpawnPlayers;
		PendingSpawnPlayers.Empty();
		
		for (AController* PendingPlayer : PlayersToSpawn)
		{
			if (IsValid(PendingPlayer))
			{
				// Double-check they don't already have a pawn
				if (PendingPlayer->GetPawn())
				{
					UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::CheckTerrainAndSpawnPendingPlayers - Player %s already has a pawn, skipping spawn"), 
						*PendingPlayer->GetName());
					continue;
				}
				
				UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::CheckTerrainAndSpawnPendingPlayers - Calling RestartPlayer for %s"), 
					*PendingPlayer->GetName());
				RestartPlayer(PendingPlayer);
			}
		}
		
		// Stop checking
		GetWorldTimerManager().ClearTimer(TerrainCheckTimer);
	}
	else
	{
		// Log current status while waiting
		static int32 WaitCounter = 0;
		if (WaitCounter++ % 10 == 0) // Log every 5 seconds (0.5s * 10)
		{
			UE_LOG(LogTemp, Warning, TEXT("ShooterGameMode::CheckTerrainAndSpawnPendingPlayers - Still waiting. TerrainGen=%s, IsReady=%s, SpawnPoints=%d, PendingPlayers=%d"), 
				TerrainGen ? TEXT("Found") : TEXT("NotFound"),
				TerrainGen && TerrainGen->IsTerrainReady() ? TEXT("true") : TEXT("false"),
				TerrainGen ? TerrainGen->SpawnPoints.Num() : 0,
				PendingSpawnPlayers.Num());
		}
	}
}

void AShooterGameMode::IncrementTeamScore(uint8 TeamByte)
{
	AShooterGameState* const MyGameState = GetGameState<AShooterGameState>();
	if (MyGameState)
	{
		FTeamScore* TeamScore = MyGameState->TeamScores.FindByPredicate([&](const FTeamScore& Score)
		{
			return Score.TeamID == TeamByte;
		});

		if (TeamScore)
		{
			TeamScore->Score++;
		}
		else
		{
			FTeamScore NewTeamScore;
			NewTeamScore.TeamID = TeamByte;
			NewTeamScore.Score = 1;
			MyGameState->TeamScores.Add(NewTeamScore);
		}
        MyGameState->OnRep_TeamScores();
	}
}
