// DayNightCycle.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Logging/LogMacros.h"
#include "DayNightCycle.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogDayNightCycle, Log, All);

UCLASS()
class VORTEXREQUIEM_API ADayNightCycle : public AActor
{
    GENERATED_BODY()

public:
    ADayNightCycle();

    virtual void BeginPlay() override;

    virtual void Tick(float DeltaTime) override;

    // Configurable cycle speed (seconds for full 24h cycle)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight")
    float CycleDuration = 600.0f; // 10 minutes

    // Current time of day (0-24 hours)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight")
    float TimeOfDay = 14.0f; // Start mid-day

    // Actor references
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    ADirectionalLight* DirectionalLightRef; // Sun

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    AExponentialHeightFog* ExponentialHeightFogRef;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    ASkyAtmosphere* SkyAtmosphereRef; // Note: Component reference might be better if known

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    ASkyLight* SkyLightRef;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    ADirectionalLight* MoonLightRef; // Moon

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    AActor* SkySphereRef; // Actor containing the SkySphere StaticMeshComponent

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Actors")
    AVolumetricCloud* VolumetricCloudRef;

    // Location settings
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Location")
    float Latitude = 60.17f; // Degrees, Helsinki/Southern Finland

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Location")
    float Longitude = 24.94f; // Degrees, Helsinki

    // Lighting parameters
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float MaxSunIntensity = 5.0f; // Max intensity for the sun light

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    FLinearColor SunLightColor = FLinearColor(1.0f, 0.95f, 0.85f); // Warm daylight color

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float MaxMoonIntensity = 0.015f; // Max intensity for the moon light (as suggested)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    FLinearColor MoonLightColor = FLinearColor(0.5f, 0.6f, 0.8f); // Cool moonlight color

    // Define the elevation range (in degrees) for the smooth twilight transition
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float TwilightStartElevation = 5.0f; // Sun elevation where twilight fully begins (sun is bright)

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float TwilightEndElevation = -15.0f; // Sun elevation where twilight fully ends (night begins)

    // Skylight intensities
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float DaySkyLightIntensity = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float NightSkyLightIntensity = 0.02f;

    // SkyLight recapture settings
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DayNight|Lighting")
    float RecaptureInterval = 5.0f; // How often to recapture skylight (seconds)

protected:
    // Internal state
    bool bWasDay = true; // Tracks the previous state for recapture triggers
    float CurrentSunElevation = 0.0f; // Stores the calculated sun elevation
    float CurrentSunAzimuth = 0.0f; // Stores the calculated sun azimuth (in radians)
    class UMaterialInstanceDynamic* SkyMID = nullptr; // Dynamic material instance for the sky sphere
    float TimeSinceLastRecapture = 0.0f; // Timer for sky light recapture

    // --- Core Update Functions ---
    void UpdateSunAndMoon();
    void UpdateSkyAtmosphere(); // Note: Usually automatic based on Directional Light
    void UpdateSkyLight();
    void UpdateFog();
    void UpdateClouds();
    void UpdateSkySphere();

    // Helper to calculate twilight alpha (0 = night, 1 = day) based on current sun elevation
    float GetTwilightAlpha() const;
};