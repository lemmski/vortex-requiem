// Copyright VortexRequiem. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Terrain/TerrainGen.h" // For ETerrainPreset
#include "BiomeButtonWidget.generated.h"

class UButton;
class UTextBlock;
class UMainMenuWidget;

UCLASS()
class VORTEXREQUIEM_API UBiomeButtonWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    void SetOwningMenu(UMainMenuWidget* Menu);
    void SetBiomeName(const FString& Name);
    void SetBiomePreset(ETerrainPreset Preset);

protected:
    virtual void NativeConstruct() override;

    UFUNCTION()
    void OnBiomeButtonClicked();

    UPROPERTY(meta = (BindWidget))
    UButton* BiomeButton;

    UPROPERTY(meta = (BindWidget))
    UTextBlock* BiomeNameText;
    
    UPROPERTY()
    ETerrainPreset BiomePreset;

private:
    UPROPERTY()
    UMainMenuWidget* OwningMenu;
};
