#include "TerrainSplatUtils.h"
#include "Engine/Texture2D.h"

namespace
{
    static FORCEINLINE float SmoothStep(float Edge0, float Edge1, float X)
    {
        const float T = FMath::Clamp((X - Edge0) / FMath::Max(Edge1 - Edge0, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T);
    }
}

UTexture2D* TerrainSplatUtils::CreateTextureRGBA8(int32 W, int32 H, const TArray<FColor>& Pixels, const FString& DebugName)
{
    if (W <= 0 || H <= 0 || Pixels.Num() != W * H) return nullptr;

    UTexture2D* NewTex = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8, DebugName.IsEmpty() ? NAME_None : FName(*DebugName));
    if (!NewTex || !NewTex->GetPlatformData() || NewTex->GetPlatformData()->Mips.Num() == 0) return nullptr;

    FTexture2DMipMap& Mip = NewTex->GetPlatformData()->Mips[0];
    const int64 Size = int64(Pixels.Num()) * sizeof(FColor);
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    Data = Mip.BulkData.Realloc(Size);
    FMemory::Memcpy(Data, Pixels.GetData(), Size);
    Mip.BulkData.Unlock();

    NewTex->SRGB = false;
    NewTex->CompressionSettings = TC_Masks;
    NewTex->Filter = TF_Bilinear;
    NewTex->UpdateResource();
    return NewTex;
}

void TerrainSplatUtils::GenerateSplatMaps(
    const TArray<uint8>& HeightData,
    int32 W,
    int32 H,
    const FProcTerrainPresetDefinition* OptionalPresetDef,
    TMap<FName, UTexture2D*>& OutGroupTextures,
    TMap<FName, TMap<FName, int32>>& OutChannelMap,
    TArray<FName>& OutGroups,
    TArray<FName>& OutLayers)
{
    OutGroupTextures.Empty();
    OutChannelMap.Empty();
    OutGroups.Empty();
    OutLayers.Empty();

    if (W <= 0 || H <= 0 || HeightData.Num() != W * H)
    {
        return;
    }

    FSplatMapRulesDefinition Rules;
    if (OptionalPresetDef && OptionalPresetDef->Splat.OutputGroups.Num() > 0)
    {
        Rules = OptionalPresetDef->Splat;
    }
    else
    {
        Rules.BlendDistance = 0.05f;
        Rules.bExportChannelsSeparately = false;
        FSplatMapGroupDefinition Group; Group.GroupName = TEXT("base");
        {
            FSplatLayerDef Base; Base.Name = TEXT("dirt"); Base.bIsBaseLayer = true; Group.Layers.Add(Base);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("grass"); L.bHasChannel = true; L.Channel = 'R';
            L.Rules.bHasMaxSlope = true; L.Rules.MaxSlope = 0.35f; Group.Layers.Add(L);
        }
        {
            FSplatLayerDef L; L.Name = TEXT("rock"); L.bHasChannel = true; L.Channel = 'G';
            L.Rules.bHasMinSlope = true; L.Rules.MinSlope = 0.5f; Group.Layers.Add(L);
        }
        Rules.OutputGroups.Add(Group);
    }

    TArray<float> Altitude; Altitude.SetNumUninitialized(W * H);
    for (int32 i = 0; i < W * H; ++i)
    {
        Altitude[i] = static_cast<float>(HeightData[i]) / 255.0f;
    }

    TArray<float> Slope; Slope.SetNumZeroed(W * H);
    float MaxSlope = 0.0f;
    auto SampleAlt = [&](int32 X, int32 Y) -> float
    {
        X = FMath::Clamp(X, 0, W - 1);
        Y = FMath::Clamp(Y, 0, H - 1);
        return Altitude[Y * W + X];
    };
    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            const float Dzdx = (SampleAlt(x + 1, y) - SampleAlt(x - 1, y)) * 0.5f;
            const float Dzdy = (SampleAlt(x, y + 1) - SampleAlt(x, y - 1)) * 0.5f;
            const float G = FMath::Sqrt(Dzdx * Dzdx + Dzdy * Dzdy);
            Slope[y * W + x] = G;
            MaxSlope = FMath::Max(MaxSlope, G);
        }
    }
    if (MaxSlope > SMALL_NUMBER)
    {
        const float Inv = 1.0f / MaxSlope;
        for (float& V : Slope) V *= Inv;
    }

    const float Blend = Rules.BlendDistance;

    for (const FSplatMapGroupDefinition& Group : Rules.OutputGroups)
    {
        TArray<const FSplatLayerDef*> ExplicitLayers;
        const FSplatLayerDef* BaseLayer = nullptr;
        for (const FSplatLayerDef& L : Group.Layers)
        {
            if (L.bIsBaseLayer)
            {
                if (!BaseLayer) BaseLayer = &L; else { BaseLayer = nullptr; break; }
            }
            else
            {
                ExplicitLayers.Add(&L);
            }
        }
        if (!BaseLayer)
        {
            UE_LOG(LogTemp, Warning, TEXT("Splat group '%s' must have exactly one base layer. Skipping."), *Group.GroupName.ToString());
            continue;
        }

        OutGroups.AddUnique(Group.GroupName);
        for (const FSplatLayerDef& L : Group.Layers)
        {
            OutLayers.AddUnique(L.Name);
        }

        const int32 NumExp = ExplicitLayers.Num();
        TArray<TArray<float>> LayerWeights; LayerWeights.SetNum(NumExp);
        for (int32 li = 0; li < NumExp; ++li) LayerWeights[li].SetNumUninitialized(W * H);
        TArray<float> SumExplicit; SumExplicit.SetNumZeroed(W * H);

        for (int32 li = 0; li < NumExp; ++li)
        {
            const FSplatLayerDef& L = *ExplicitLayers[li];
            const FSplatLayerRuleDef& R = L.Rules;
            float* RESTRICT LW = LayerWeights[li].GetData();
            for (int32 i = 0; i < W * H; ++i)
            {
                float Influence = 1.0f;
                const float A = Altitude[i];
                const float S = Slope[i];
                if (R.bHasMinAltitude) Influence *= SmoothStep(R.MinAltitude - Blend, R.MinAltitude + Blend, A);
                if (R.bHasMaxAltitude) Influence *= (1.0f - SmoothStep(R.MaxAltitude - Blend, R.MaxAltitude + Blend, A));
                if (R.bHasMinSlope)    Influence *= SmoothStep(R.MinSlope - Blend, R.MinSlope + Blend, S);
                if (R.bHasMaxSlope)    Influence *= (1.0f - SmoothStep(R.MaxSlope - Blend, R.MaxSlope + Blend, S));
                LW[i] = Influence;
                SumExplicit[i] += Influence;
            }
        }

        for (int32 i = 0; i < W * H; ++i)
        {
            const float Den = FMath::Max(1.0f, SumExplicit[i]);
            if (Den > 1.0f + KINDA_SMALL_NUMBER) SumExplicit[i] = 1.0f;
            for (int32 li = 0; li < NumExp; ++li) LayerWeights[li][i] /= Den;
        }

        TArray<float> BaseW; BaseW.SetNumUninitialized(W * H);
        for (int32 i = 0; i < W * H; ++i)
        {
            float FinalExp = 0.0f; for (int32 li = 0; li < NumExp; ++li) FinalExp += LayerWeights[li][i];
            BaseW[i] = FMath::Clamp(1.0f - FinalExp, 0.0f, 1.0f);
        }

        TArray<FColor> Pixels; Pixels.SetNumZeroed(W * H);
        bool Used[4] = { false, false, false, false };
        TMap<FName, int32> LayerToChannel;

        auto ChannelToIndex = [](TCHAR C) -> int32 {
            switch (C)
            {
                case 'R': case 'r': return 0;
                case 'G': case 'g': return 1;
                case 'B': case 'b': return 2;
                case 'A': case 'a': return 3;
                default: return -1;
            }
        };

        for (int32 li = 0; li < NumExp; ++li)
        {
            const FSplatLayerDef& L = *ExplicitLayers[li];
            const int32 Channel = L.bHasChannel ? ChannelToIndex(L.Channel) : -1;
            if (Channel < 0 || Channel > 3)
            {
                UE_LOG(LogTemp, Warning, TEXT("Layer '%s' in group '%s' has invalid channel. Skipping."), *L.Name.ToString(), *Group.GroupName.ToString());
                continue;
            }
            Used[Channel] = true;
            LayerToChannel.Add(L.Name, Channel);
            const float* LW = LayerWeights[li].GetData();
            for (int32 i = 0; i < W * H; ++i)
            {
                const uint8 V = static_cast<uint8>(FMath::Clamp(LW[i] * 255.0f, 0.0f, 255.0f));
                FColor& P = Pixels[i];
                switch (Channel)
                {
                    case 0: P.R = V; break;
                    case 1: P.G = V; break;
                    case 2: P.B = V; break;
                    case 3: P.A = V; break;
                }
            }
        }

        int32 BaseChannel = 0; while (BaseChannel < 4 && Used[BaseChannel]) ++BaseChannel;
        if (BaseChannel >= 4)
        {
            UE_LOG(LogTemp, Warning, TEXT("Splat group '%s' had no free channel for base layer '%s'. Overwriting alpha."), *Group.GroupName.ToString(), *BaseLayer->Name.ToString());
            BaseChannel = 3;
        }
        for (int32 i = 0; i < W * H; ++i)
        {
            const uint8 V = static_cast<uint8>(FMath::Clamp(BaseW[i] * 255.0f, 0.0f, 255.0f));
            FColor& P = Pixels[i];
            switch (BaseChannel)
            {
                case 0: P.R = V; break;
                case 1: P.G = V; break;
                case 2: P.B = V; break;
                case 3: P.A = V; break;
            }
        }
        LayerToChannel.Add(BaseLayer->Name, BaseChannel);

        UTexture2D* Tex = TerrainSplatUtils::CreateTextureRGBA8(W, H, Pixels, FString::Printf(TEXT("Splat_%s"), *Group.GroupName.ToString()));
        if (Tex)
        {
            OutGroupTextures.Add(Group.GroupName, Tex);
            OutChannelMap.Add(Group.GroupName, LayerToChannel);
        }
    }
}


