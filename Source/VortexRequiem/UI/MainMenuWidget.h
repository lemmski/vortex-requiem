// Copyright VortexRequiem. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Terrain/TerrainTypes.h" // For ETerrainPreset
#include "MainMenuWidget.generated.h"

class UButton;
class UWidgetSwitcher;
class UPanelWidget;
class UBiomeButtonWidget;
class UAudioComponent;
class USoundBase;
class UTextBlock;

UCLASS()
class VORTEXREQUIEM_API UMainMenuWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "UI")
    void ShowBiomeSelectionScreen();

    UFUNCTION(BlueprintCallable, Category = "UI")
    void ShowMainMenuScreen();

    void StartGameWithPreset(ETerrainPreset Preset);

protected:
    virtual void NativeConstruct() override;
    
    UFUNCTION()
    void DelayedStartGeneration();

    UFUNCTION(BlueprintCallable, Category = "UI")
    void PopulateBiomeButtons();

    UFUNCTION()
    void HandleGenerationProgress(const FText& ProgressText);

    UFUNCTION()
    void HandleGenerationComplete();

    UPROPERTY(meta = (BindWidget))
    UWidgetSwitcher* MainWidgetSwitcher;

    UPROPERTY(meta = (BindWidget))
    UWidget* MainMenuScreen;

    UPROPERTY(meta = (BindWidget))
    UWidget* BiomeSelectionScreen;
    
    UPROPERTY(meta = (BindWidget))
    UWidget* LoadingScreen;

    UPROPERTY(meta = (BindWidget))
    UTextBlock* LoadingScreenText;

    UPROPERTY(meta = (BindWidget))
    UButton* SinglePlayerButton;
    
    UPROPERTY(meta = (BindWidget))
    UPanelWidget* BiomeButtonsContainer;

    UPROPERTY(EditDefaultsOnly, Category = "UI")
    TSubclassOf<UBiomeButtonWidget> BiomeButtonWidgetClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio")
    USoundBase* MusicTrack;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Audio")
    float MusicFadeInDuration = 2.0f;

private:
    UPROPERTY()
    UAudioComponent* AudioComponent;
    
    ETerrainPreset PresetToGenerate;
    FTimerHandle GenerationTimerHandle;
};
