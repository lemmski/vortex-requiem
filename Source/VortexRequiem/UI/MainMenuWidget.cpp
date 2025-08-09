// Copyright VortexRequiem. All Rights Reserved.

#include "UI/MainMenuWidget.h"
#include "Components/Button.h"
#include "Components/WidgetSwitcher.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Kismet/GameplayStatics.h"
#include "UI/BiomeButtonWidget.h"
#include "Terrain/ProcTerrainPreset.h"
#include "UObject/UObjectIterator.h"
#include "VortexRequiemGameInstance.h"
#include "EngineUtils.h"
#include "Terrain/TerrainGen.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

void UMainMenuWidget::NativeConstruct()
{
    Super::NativeConstruct();
    bIsShuttingDown = false;

    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::NativeConstruct - Menu widget created. NetMode=%d"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"),
        (int32)GetWorld()->GetNetMode());

    // Always bind to terrain readiness so clients auto-close the menu when the server signals ready
    if (UWorld* World = GetWorld())
    {
        ATerrainGen* TerrainActor = nullptr;
        for (TActorIterator<ATerrainGen> It(World); It; ++It)
        {
            TerrainActor = *It;
            break;
        }

        if (TerrainActor)
        {
            // Ensure we don't bind multiple times if the widget is reconstructed
            TerrainActor->OnAllClientsReady.RemoveDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
            TerrainActor->OnAllClientsReady.AddUniqueDynamic(this, &UMainMenuWidget::HandleGenerationComplete);

            // If terrain is already ready when this widget appears, close immediately
            if (TerrainActor->IsTerrainReady())
            {
                HandleGenerationComplete();
                return;
            }
        }
    }

    if (SinglePlayerButton)
    {
        SinglePlayerButton->OnClicked.AddDynamic(this, &UMainMenuWidget::ShowBiomeSelectionScreen);
    }
    
    PopulateBiomeButtons();
    PlayRandomMusicTrack();
}

void UMainMenuWidget::NativeDestruct()
{
    if (AudioComponent)
    {
        AudioComponent->OnAudioFinished.RemoveAll(this);
        AudioComponent->Stop();
    }
    Super::NativeDestruct();
}

void UMainMenuWidget::PlayRandomMusicTrack()
{
    if (MusicTracks.Num() > 0)
    {
        int32 RandomIndex = FMath::RandRange(0, MusicTracks.Num() - 1);
        USoundBase* SelectedTrack = MusicTracks[RandomIndex];

        if (SelectedTrack)
        {
            if (!AudioComponent)
            {
                AudioComponent = UGameplayStatics::CreateSound2D(this, SelectedTrack);
            }
            
            if (AudioComponent)
            {
                // Ensure we are in a clean state and bind finish delegate BEFORE starting playback
                AudioComponent->OnAudioFinished.RemoveAll(this);
                AudioComponent->OnAudioFinished.AddDynamic(this, &UMainMenuWidget::OnMusicTrackFinished);
                AudioComponent->Stop();
                AudioComponent->SetSound(SelectedTrack);
                AudioComponent->FadeIn(MusicFadeInDuration);
            }
        }
    }
}

void UMainMenuWidget::OnMusicTrackFinished()
{
    // Don't start a new track if we're in the process of shutting down
    if (bIsShuttingDown)
    {
        return;
    }
    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::OnMusicTrackFinished - Selecting next random track"),
        GetWorld() && GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
    PlayRandomMusicTrack();
}

void UMainMenuWidget::PopulateBiomeButtons()
{
    if (!BiomeButtonWidgetClass || !BiomeButtonsContainer)
    {
        return;
    }

    BiomeButtonsContainer->ClearChildren();

    const UEnum* EnumPtr = FindObject<UEnum>(nullptr, TEXT("/Script/VortexRequiem.ETerrainPreset"), true);
    if (!EnumPtr) return;

    for (int32 i = 0; i < EnumPtr->NumEnums() - 1; ++i)
    {
        ETerrainPreset Preset = (ETerrainPreset)EnumPtr->GetValueByIndex(i);
        if (Preset == ETerrainPreset::None) continue;

        FProcTerrainPresetDefinition PresetDef;
        if (ProcTerrainPresets::GetPreset(Preset, PresetDef))
        {
            UBiomeButtonWidget* BiomeButton = CreateWidget<UBiomeButtonWidget>(this, BiomeButtonWidgetClass);
            if (BiomeButton)
            {
                FString DisplayName = EnumPtr->GetDisplayNameTextByIndex(i).ToString();
                BiomeButton->SetBiomeName(DisplayName);
                BiomeButton->SetBiomePreset(Preset);
                BiomeButton->SetOwningMenu(this);
                BiomeButtonsContainer->AddChild(BiomeButton);
            }
        }
    }
}

void UMainMenuWidget::ShowBiomeSelectionScreen()
{
    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::ShowBiomeSelectionScreen - Showing biome selection"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
        
    if (MainWidgetSwitcher)
    {
        MainWidgetSwitcher->SetActiveWidget(BiomeSelectionScreen);
    }
}

void UMainMenuWidget::ShowMainMenuScreen()
{
    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::ShowMainMenuScreen - Showing main menu screen"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
        
    if (MainWidgetSwitcher)
    {
        MainWidgetSwitcher->SetActiveWidget(MainMenuScreen);
    }
}

void UMainMenuWidget::StartGameWithPreset(ETerrainPreset Preset)
{
    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::StartGameWithPreset - Starting game with preset %d"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"),
        (int32)Preset);

    PresetToGenerate = Preset;

    if (LoadingScreenText)
    {
        LoadingScreenText->SetText(FText::FromString(TEXT("Initiating...")));
    }

    if (MainWidgetSwitcher && LoadingScreen)
    {
        MainWidgetSwitcher->SetActiveWidget(LoadingScreen);
    }
    
    GetWorld()->GetTimerManager().SetTimer(GenerationTimerHandle, this, &UMainMenuWidget::DelayedStartGeneration, 0.1f, false);
}

void UMainMenuWidget::DelayedStartGeneration()
{
    // Find the TerrainGen actor in the world
    ATerrainGen* TerrainActor = nullptr;
    for (TActorIterator<ATerrainGen> It(GetWorld()); It; ++It)
    {
        TerrainActor = *It;
        break;
    }

    if (TerrainActor)
    {
        // Bind to the new multicast delegate (guard against duplicates)
        TerrainActor->OnAllClientsReady.RemoveDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
        TerrainActor->OnAllClientsReady.AddUniqueDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
        TerrainActor->OnGenerationComplete.RemoveDynamic(this, &UMainMenuWidget::OnLocalGenerationComplete);
        TerrainActor->OnGenerationComplete.AddUniqueDynamic(this, &UMainMenuWidget::OnLocalGenerationComplete);
        
        // Only trigger generation on the server
        if (GetWorld()->GetNetMode() != NM_Client)
        {
            TerrainActor->GenerateTerrainFromPreset(PresetToGenerate);
        }
    }
    else
    {
        HandleGenerationComplete(); // Can't find actor, just close the menu
    }
}

void UMainMenuWidget::HandleGenerationProgress(const FText& ProgressText)
{
    if (LoadingScreenText)
    {
        LoadingScreenText->SetText(ProgressText);
    }
}

void UMainMenuWidget::HandleGenerationComplete()
{
    bIsShuttingDown = true;
    UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::HandleGenerationComplete - Called"),
        GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));

    // Clear any running timers
    GetWorld()->GetTimerManager().ClearTimer(TerrainReadyCheckTimer);

    // Fade out music
    if (AudioComponent)
    {
        AudioComponent->OnAudioFinished.RemoveAll(this);
        AudioComponent->FadeOut(1.0f, 0.0f);
    }

    // Unbind from the delegates
    ATerrainGen* TerrainActor = nullptr;
    for (TActorIterator<ATerrainGen> It(GetWorld()); It; ++It)
    {
        TerrainActor = *It;
        break;
    }
    if (TerrainActor)
    {
        TerrainActor->OnAllClientsReady.RemoveDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
        TerrainActor->OnGenerationComplete.RemoveDynamic(this, &UMainMenuWidget::OnLocalGenerationComplete);
    }

    // Hide the menu and return control to the player
    APlayerController* PlayerController = GetOwningPlayer();
    if (PlayerController)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::HandleGenerationComplete - Removing menu from parent and setting input mode to game"),
            GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
        RemoveFromParent();
        FInputModeGameOnly InputMode;
        PlayerController->SetInputMode(InputMode);
        PlayerController->SetShowMouseCursor(false);
    }
}

void UMainMenuWidget::OnLocalGenerationComplete()
{
    // Mirrors HandleGenerationComplete to ensure the UI closes even if we locally generated without receiving multicast
    HandleGenerationComplete();
}

void UMainMenuWidget::RemoveFromParent()
{
    if (GetWorld()) // Add null check for GetWorld() to prevent crash
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] MainMenuWidget::RemoveFromParent - Called. Widget will be removed from viewport"),
            GetWorld()->GetNetMode() == NM_Client ? TEXT("CLIENT") : TEXT("SERVER"));
        }

    Super::RemoveFromParent();
}

