// Copyright VortexRequiem. All Rights Reserved.

#include "UI/BiomeButtonWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "UI/MainMenuWidget.h"

void UBiomeButtonWidget::SetOwningMenu(UMainMenuWidget* Menu)
{
    OwningMenu = Menu;
}

void UBiomeButtonWidget::SetBiomeName(const FString& Name)
{
    if (BiomeNameText)
    {
        BiomeNameText->SetText(FText::FromString(Name));
    }
}

void UBiomeButtonWidget::SetBiomePreset(ETerrainPreset Preset)
{
    BiomePreset = Preset;
}

void UBiomeButtonWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (BiomeButton)
    {
        BiomeButton->OnClicked.AddDynamic(this, &UBiomeButtonWidget::OnBiomeButtonClicked);
    }
}

void UBiomeButtonWidget::OnBiomeButtonClicked()
{
    if (OwningMenu)
    {
        OwningMenu->StartGameWithPreset(BiomePreset);
    }
}
