// Copyright Epic Games, Inc. All Rights Reserved.


#include "VortexRequiemPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "VortexRequiemCameraManager.h"

AVortexRequiemPlayerController::AVortexRequiemPlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = AVortexRequiemCameraManager::StaticClass();
}

void AVortexRequiemPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Add Input Mapping Context
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}
	}
}
