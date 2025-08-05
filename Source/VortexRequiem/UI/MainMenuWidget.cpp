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

    if (SinglePlayerButton)
    {
        SinglePlayerButton->OnClicked.AddDynamic(this, &UMainMenuWidget::ShowBiomeSelectionScreen);
    }
    
    PopulateBiomeButtons();

    if (MusicTrack && !AudioComponent)
    {
        AudioComponent = UGameplayStatics::CreateSound2D(this, MusicTrack);
        if (AudioComponent)
        {
            AudioComponent->FadeIn(MusicFadeInDuration);
        }
    }
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
    if (MainWidgetSwitcher)
    {
        MainWidgetSwitcher->SetActiveWidget(BiomeSelectionScreen);
    }
}

void UMainMenuWidget::ShowMainMenuScreen()
{
    if (MainWidgetSwitcher)
    {
        MainWidgetSwitcher->SetActiveWidget(MainMenuScreen);
    }
}

void UMainMenuWidget::StartGameWithPreset(ETerrainPreset Preset)
{
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
        TerrainActor->OnGenerationProgress.AddDynamic(this, &UMainMenuWidget::HandleGenerationProgress);
        TerrainActor->OnGenerationComplete.AddDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
        TerrainActor->GenerateTerrainFromPreset(PresetToGenerate);
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
    // Fade out music
    if (AudioComponent)
    {
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
        TerrainActor->OnGenerationProgress.RemoveDynamic(this, &UMainMenuWidget::HandleGenerationProgress);
        TerrainActor->OnGenerationComplete.RemoveDynamic(this, &UMainMenuWidget::HandleGenerationComplete);
    }

    // Hide the menu and return control to the player
    APlayerController* PlayerController = GetOwningPlayer();
    if (PlayerController)
    {
        RemoveFromParent();
        FInputModeGameOnly InputMode;
        PlayerController->SetInputMode(InputMode);
        PlayerController->SetShowMouseCursor(false);
    }
}
