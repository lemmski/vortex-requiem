// Copyright Epic Games, Inc. All Rights Reserved.


#include "ShooterGameState.h"
#include "Net/UnrealNetwork.h"

void AShooterGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AShooterGameState, TeamScores);
}

void AShooterGameState::OnRep_TeamScores()
{
    OnTeamScoresChangedDelegate.Broadcast();
}
