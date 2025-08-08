#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "ProcTerrain.h"
#include "TerrainTypes.h"
#include "ProcTerrainPreset.h"
#include "Delegates/DelegateCombinations.h"
#include "TerrainGen.generated.h"

class UMaterialInterface;
class UMaterialInstance;

UENUM()
enum class EGenerationState : uint8
{
    Idle,
    LoadHeightmap,
    GenerateProcedural,
    CreateMesh,
    UploadMesh,
    WaitForCollision,
    CalculateSpawnPoints,
    BuildNavigation,
    Finalize
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGenerationProgress, const FText&, ProgressText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGenerationComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAllClientsReady);

UCLASS()
class VORTEXREQUIEM_API ATerrainGen : public AActor
{
    GENERATED_BODY()

public:
    ATerrainGen();

    UFUNCTION(BlueprintCallable, Category="Terrain")
    void GenerateTerrainFromPreset(ETerrainPreset NewPreset);

    UFUNCTION(BlueprintCallable, Category="Terrain")
    bool IsTerrainReady() const { return bTerrainReady; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    bool bGenerateOnBeginPlay;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    FString PngPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    UTexture2D* HeightmapTexture;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", ReplicatedUsing=OnRep_Preset)
    ETerrainPreset Preset;

    UFUNCTION()
    void OnRep_Preset();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain", ReplicatedUsing=OnRep_Seed)
	int32 Seed;

    UFUNCTION()
    void OnRep_Seed();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    UMaterialInterface* TerrainMaterial;

    // Flat map: Key = "Preset.Layer" -> Material Instance (editable, supports drag & drop)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain|Splat", meta=(AllowedClasses="/Script/Engine.MaterialInstanceConstant", DisplayThumbnail="true"))
    TMap<FName, UMaterialInstance*> AllPresetLayerMaterials;

    // If true, generated splat maps will be assigned to the mesh material instance using parameter names: Splat_<GroupName>
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain|Splat")
    bool bApplySplatToMaterial = true;

    // Convenience: list of detected splat groups and layers for the current preset
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Splat")
    TArray<FName> AvailableSplatGroups;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain|Splat")
    TArray<FName> AvailableSplatLayers;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", Replicated)
    float XYScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain", Replicated)
    float ZScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    int32 TileQuads;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Terrain")
    float HeightTolerance;

    UPROPERTY(EditAnywhere, Category="Terrain|UI")
    TSubclassOf<class UUserWidget> LoadingWidgetClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
    int32 NumPlayerStarts;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0", ClampMax = "90.0"))
    float MaxSpawnSlopeInDegrees;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0"))
    float MinSpawnSeparation;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning", meta = (ClampMin = "0.0"))
    float SpawnClearanceRadius;
    
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spawning", ReplicatedUsing=OnRep_SpawnPoints)
    TArray<FVector> SpawnPoints;

    UFUNCTION()
    void OnRep_SpawnPoints();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
    bool bUseLargeSpawnSpheres;

    UPROPERTY(BlueprintAssignable, Category = "Terrain")
    FOnGenerationProgress OnGenerationProgress;

    UPROPERTY(BlueprintAssignable, Category = "Terrain")
    FOnGenerationComplete OnGenerationComplete;

    UPROPERTY(BlueprintAssignable, Category = "Terrain")
    FOnAllClientsReady OnAllClientsReady;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Terrain", ReplicatedUsing=OnRep_TerrainReady)
    bool bTerrainReady;

    UFUNCTION()
    void OnRep_TerrainReady();

    UFUNCTION(NetMulticast, Reliable)
    void Multicast_NotifyClientsReady();

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    virtual void OnConstruction(const FTransform& Transform) override;

protected:
    virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY()
	USceneComponent* DefaultSceneRoot;

    UPROPERTY(VisibleAnywhere, Transient)
    UStaticMeshComponent* Mesh;

    UPROPERTY(Transient)
    class UStaticMesh* GeneratedMesh;

    UPROPERTY(Transient)
    class UUserWidget* ActiveLoadingWidget;

    // Runtime material instance bound to the mesh
    UPROPERTY(Transient)
    class UMaterialInstanceDynamic* RuntimeMID;

    // Generated splat textures per group name (e.g. "urban_decay")
    UPROPERTY(Transient)
    TMap<FName, UTexture2D*> SplatGroupTextures;

    // For each group, map LayerName -> ChannelIndex (0=R,1=G,2=B,3=A). Internal only (not exposed to UHT).
    TMap<FName, TMap<FName, int32>> SplatGroupChannelMap;

    FTimerHandle CollisionReadyTimer;
    TArray<TWeakObjectPtr<AActor>> ActorsToReenablePhysics;

    UFUNCTION(CallInEditor, Category="Terrain")
    void Regenerate();

    //~ Begin Async Terrain Generation
    void StartAsyncGeneration();
    void ProcessGenerationStep();

    EGenerationState CurrentState;
    FTimerHandle GenerationProcessTimer;
    
    // Data passed between states
    TArray<uint8> HeightData;
    int32 HeightmapWidth;
    int32 HeightmapHeight;
    FString CurrentCacheKey;
    FString EditorLastCacheKey;
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector2D> UVs;
    
    void GenerateTerrain_Editor();
    
    void Step_LoadHeightmap();
    void Step_GenerateProcedural();
    void Step_CreateMesh();
    void Step_UploadMesh();
    void Step_WaitForCollision();
    void Step_CalculateSpawnPoints();
    void Step_BuildNavigation();
    void Step_Finalize();
    //~ End Async Terrain Generation

    void CalculateSpawnPoints();
    void CheckCollisionReady();
    void DisableActorPhysicsTemporarily();
    void RestoreActorPhysics();
    static bool LoadHeightMapRaw(const FString& FilePath, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutData);

    // --- Splat generation helpers ---
    void GenerateSplatMaps(const FProcTerrainPresetDefinition* OptionalPresetDef);
    void ApplyMaterialBindings(const FProcTerrainPresetDefinition* OptionalPresetDef);
    static UTexture2D* CreateTextureRGBA8(int32 InWidth, int32 InHeight, const TArray<FColor>& Pixels, const FString& DebugName);

    // Populate layer material slots and available names from preset
    void UpdateLayerSlotsFromPreset(const FProcTerrainPresetDefinition& Def);
    void UpdateAllPresetLayerSlots();
    static FName GetPresetDisplayName(ETerrainPreset InPreset);
};
