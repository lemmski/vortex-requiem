// DayNightCycle.cpp
#include "DayNightCycle.h"
#include "Kismet/GameplayStatics.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h" // Ensure this is included if using ASkyAtmosphere directly
#include "Components/VolumetricCloudComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Logging/LogMacros.h"
// #include "Async/Async.h" // Not strictly needed for this logic
// #include "ProfilingDebugging/ScopedTimers.h" // Not strictly needed for this logic
#include "Math/UnrealMathUtility.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/DirectionalLight.h" // Ensure included


DEFINE_LOG_CATEGORY(LogDayNightCycle);

ADayNightCycle::ADayNightCycle()
{
    PrimaryActorTick.bCanEverTick = true;
    // Default values are set in the header
}

void ADayNightCycle::BeginPlay() {
    Super::BeginPlay();

    // --- Initial Setup ---
    // Ensure references are valid (optional logging)
    if (!DirectionalLightRef) UE_LOG(LogDayNightCycle, Warning, TEXT("DirectionalLightRef (Sun) is not set!"));
    if (!MoonLightRef) UE_LOG(LogDayNightCycle, Warning, TEXT("MoonLightRef (Moon) is not set!"));
    if (!SkyLightRef) UE_LOG(LogDayNightCycle, Warning, TEXT("SkyLightRef is not set!"));
    if (!ExponentialHeightFogRef) UE_LOG(LogDayNightCycle, Warning, TEXT("ExponentialHeightFogRef is not set!"));

    // Create MID for SkySphere material once
    if (SkySphereRef) {
        UStaticMeshComponent* SkyMesh = SkySphereRef->FindComponentByClass<UStaticMeshComponent>(); // Find the component
        if (SkyMesh && SkyMesh->GetMaterial(0)) {
            SkyMID = SkyMesh->CreateAndSetMaterialInstanceDynamic(0);
            if (!SkyMID) {
                UE_LOG(LogDayNightCycle, Error, TEXT("Failed to create Dynamic Material Instance for SkySphere!"));
            }
        }
        else {
            UE_LOG(LogDayNightCycle, Warning, TEXT("SkySphereRef does not have a valid StaticMeshComponent or material slot 0!"));
        }
    }

    // Initialize state: Ensure first recapture happens soon after start
    TimeSinceLastRecapture = RecaptureInterval;
    // Estimate initial state for bWasDay based on starting TimeOfDay
    bWasDay = TimeOfDay >= 6.0f && TimeOfDay < 18.0f;
}

void ADayNightCycle::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Prevent updates if duration is invalid
    if (CycleDuration <= 0.0f) return;

    // --- Advance Time of Day ---
    // Calculate time step in hours (24h cycle mapped to CycleDuration seconds)
    float TimeStepHours = (DeltaTime / CycleDuration) * 24.0f;
    TimeOfDay += TimeStepHours;
    TimeOfDay = FMath::Fmod(TimeOfDay, 24.0f); // Wrap around 24 hours
    if (TimeOfDay < 0.0f) { // Ensure positive time
        TimeOfDay += 24.0f;
    }

    // --- Update Core Components ---
    UpdateSunAndMoon();        // Calculates positions and intensities
    UpdateSkyAtmosphere();     // Optional: Adjust SkyAtmosphere properties if needed
    UpdateFog();               // Adjust fog based on time/light
    UpdateClouds();            // Adjust clouds based on time/light
    UpdateSkySphere();         // Update sky sphere material parameters

    // --- Handle Sky Light Recapture ---
    TimeSinceLastRecapture += DeltaTime;
    // Update SkyLight intensity every tick, but recapture less frequently
    UpdateSkyLight(); // Update intensity first
    if (TimeSinceLastRecapture >= RecaptureInterval) {
        if (SkyLightRef) {
            USkyLightComponent* SkyComp = SkyLightRef->FindComponentByClass<USkyLightComponent>();
            if (SkyComp) {
                SkyComp->RecaptureSky();
                UE_LOG(LogDayNightCycle, Log, TEXT("Recapturing SkyLight. Time: %.2f"), TimeOfDay);
            }
        }
        TimeSinceLastRecapture = 0.0f; // Reset the timer
    }
}

// Helper to calculate twilight alpha (0 = night, 1 = day) based on current sun elevation
float ADayNightCycle::GetTwilightAlpha() const
{
    // If elevation is above the start twilight point, alpha is 1.0 (full day)
    if (CurrentSunElevation >= TwilightStartElevation) {
        return 1.0f;
    }
    // If elevation is below the end twilight point, alpha is 0.0 (full night)
    if (CurrentSunElevation <= TwilightEndElevation) {
        return 0.0f;
    }

    // Otherwise, linearly interpolate alpha based on elevation within the twilight range
    // Uses the range [TwilightEndElevation, TwilightStartElevation] mapped to [0.0f, 1.0f]
    return FMath::GetMappedRangeValueClamped(
        FVector2D(TwilightEndElevation, TwilightStartElevation),
        FVector2D(0.0f, 1.0f),
        CurrentSunElevation
    );
}


void ADayNightCycle::UpdateSunAndMoon()
{
    // Ensure references are valid before proceeding
    if (!DirectionalLightRef || !MoonLightRef) {
        return;
    }

    // --- Calculate Sun Position ---
    // Based on https://gml.noaa.gov/grad/gmatic/calculator.html or similar astronomical formulas
    // Convert time of day (hours) to angle (degrees). 0 = midnight, 12 = noon.
    float NormalizedTime = TimeOfDay;
    float HourAngle = (NormalizedTime - 12.0f) * 15.0f; // Angle relative to local noon in degrees (-180 to +180)

    // Simplified Declination calculation (approximation for Earth's axial tilt)
    // This is a rough approximation, more accurate calculations depend on the day of the year.
    float Declination = -23.45f * FMath::Cos(FMath::DegreesToRadians(360.0f / 365.25f * ((NormalizedTime / 24.0f * 365.25f) + 172.0f - 180.0f))); // Rough approximation

    // Convert Latitude, Declination, and Hour Angle to radians for calculations
    float LatRad = FMath::DegreesToRadians(Latitude);
    float DecRad = FMath::DegreesToRadians(Declination);
    float HourRad = FMath::DegreesToRadians(HourAngle);

    // Calculate Elevation (Altitude) using the standard formula
    float Elevation = FMath::RadiansToDegrees(FMath::Asin(
        FMath::Sin(DecRad) * FMath::Sin(LatRad) +
        FMath::Cos(DecRad) * FMath::Cos(LatRad) * FMath::Cos(HourRad)
    ));

    // Calculate Azimuth (Direction) using the standard formula (East positive from North)
    float Azimuth = FMath::Atan2(
        FMath::Sin(HourRad), // Numerator component related to hour angle
        FMath::Cos(HourRad) * FMath::Sin(LatRad) - FMath::Tan(DecRad) * FMath::Cos(LatRad) // Denominator
    );
    Azimuth = FMath::DegreesToRadians(Azimuth); // Convert Azimuth to degrees [-180, 180]

    // Store calculated values for use in other functions
    CurrentSunElevation = Elevation;
    CurrentSunAzimuth = Azimuth; // Store in radians for consistency

    // --- Apply Rotations to Actors ---
    // UE Rotator uses Pitch, Yaw, Roll.
    // Pitch = -Elevation (positive pitch looks up)
    // Yaw = Azimuth + Longitude (adjusting for world rotation and time zone)
    // Roll = 0
    DirectionalLightRef->SetActorRotation(FRotator(-Elevation, Azimuth + FMath::DegreesToRadians(Longitude), 0.0f));
    // Moon is opposite the sun in elevation and rotated 180 degrees in Yaw
    MoonLightRef->SetActorRotation(FRotator(Elevation, Azimuth + FMath::DegreesToRadians(Longitude) + 180.0f, 0.0f));

    // --- Update Light Component Properties ---
    UDirectionalLightComponent* SunLightComp = DirectionalLightRef->GetLightComponent() ? Cast<UDirectionalLightComponent>(DirectionalLightRef->GetLightComponent()) : nullptr;
    UDirectionalLightComponent* MoonLightComp = MoonLightRef->GetLightComponent() ? Cast<UDirectionalLightComponent>(MoonLightRef->GetLightComponent()) : nullptr;

    if (SunLightComp && MoonLightComp)
    {
        // --- Atmosphere Sun Light Switch ---
        // Determine which light should primarily influence Sky Atmosphere scattering
        const float AtmosphereSunSwitchElevation = -0.5f; // Sun stops influencing atmosphere slightly below horizon
        bool bIsDay = Elevation > AtmosphereSunSwitchElevation;

        SunLightComp->SetAtmosphereSunLight(bIsDay);
        MoonLightComp->SetAtmosphereSunLight(!bIsDay); // Moon affects atmosphere when sun doesn't

        // Trigger immediate recapture if the primary atmosphere light source changed
        if (bIsDay != bWasDay)
        {
            UE_LOG(LogDayNightCycle, Log, TEXT("Atmosphere light source changed. SunActive: %s"), bIsDay ? TEXT("true") : TEXT("false"));
            // Force immediate recapture to prevent visual gaps during the transition
            if (SkyLightRef) {
                USkyLightComponent* SkyComp = SkyLightRef->FindComponentByClass<USkyLightComponent>();
                if (SkyComp) SkyComp->RecaptureSky();
            }
            // Reset timer to ensure next periodic recapture happens after a short delay
            TimeSinceLastRecapture = 0.0f;
        }
        bWasDay = bIsDay; // Update the state tracker

        // --- Calculate Smooth Twilight Factor ---
        // Get alpha based on elevation, smoothed using SmoothStep for a nicer curve
        float TwilightAlpha = GetTwilightAlpha();
        float SmoothedTwilightAlpha = FMath::SmoothStep(0.0f, 1.0f, TwilightAlpha);

        // --- Sun Intensity & Color ---
        // Intensity ramps FROM 0 (at night) TO MaxSunIntensity (at day) using SmoothedTwilightAlpha
        float SunIntensity = FMath::Lerp(0.0f, MaxSunIntensity, SmoothedTwilightAlpha);
        SunLightComp->SetIntensity(SunIntensity);

        // Interpolate sun color between a warm twilight color and the main daylight color
        FLinearColor TwilightSunColor = FMath::Lerp(FLinearColor(1.0f, 0.7f, 0.4f), SunLightColor, SmoothedTwilightAlpha);
        SunLightComp->SetLightColor(TwilightSunColor);


        // --- Moon Intensity & Color ---
        // Intensity ramps FROM MaxMoonIntensity (at night) TO 0 (at day) using the SAME SmoothedTwilightAlpha
        // This inverted Lerp ensures continuity: As SunIntensity decreases, MoonIntensity increases.
        float MoonIntensity = FMath::Lerp(MaxMoonIntensity, 0.0f, SmoothedTwilightAlpha);
        MoonLightComp->SetIntensity(MoonIntensity);
        MoonLightComp->SetLightColor(MoonLightColor);
    }
}

void ADayNightCycle::UpdateSkyAtmosphere()
{
    // The SkyAtmosphere component automatically uses the Directional Light marked as 'Atmosphere Sun Light'.
    // We've set this property in UpdateSunAndMoon().
    // If you need to adjust other SkyAtmosphere parameters (e.g., scattering, absorption amounts) based on time of day,
    // you can do it here using the calculated TwilightAlpha.

    // Example: Adjust scattering parameters during twilight for a more dramatic effect
    if (SkyAtmosphereRef) {
        // Find the component if the reference is to the Actor
        USkyAtmosphereComponent* SkyAtmosphereComp = SkyAtmosphereRef->FindComponentByClass<USkyAtmosphereComponent>();
        if (!SkyAtmosphereComp) {
            // Maybe the reference is directly to the component? Check that too.
            SkyAtmosphereComp = Cast<USkyAtmosphereComponent>(SkyAtmosphereRef);
        }

        if (SkyAtmosphereComp) {
            float TwilightAlpha = GetTwilightAlpha();
            float SmoothedTwilightAlpha = FMath::SmoothStep(0.0f, 1.0f, TwilightAlpha);

            // Example: Make atmosphere slightly less dense/intense during twilight
            // float ScatteringFactor = FMath::Lerp(0.05f, 0.03f, SmoothedTwilightAlpha); // Daytime vs Nighttime scattering factor
            // SkyAtmosphereComp->SetAtmosphere斎Scattering(ScatteringFactor); // Replace with actual property name if needed

            // Example: Adjust absorption towards night
            // FLinearColor AbsorptionColor = FMath::Lerp(FLinearColor(0.01f, 0.01f, 0.02f), FLinearColor(0.005f, 0.005f, 0.01f), SmoothedTwilightAlpha);
            // SkyAtmosphereComp->SetAbsorptionCoefficient(AbsorptionColor); // Replace with actual property name if needed
        }
    }
}

void ADayNightCycle::UpdateSkyLight()
{
    // Update intensity based on twilight, recapture handled in Tick
    if (!SkyLightRef) return;

    USkyLightComponent* SkyComp = SkyLightRef->FindComponentByClass<USkyLightComponent>();
    if (SkyComp)
    {
        // Calculate twilight alpha based on the current sun elevation. Reuse the helper.
        float TwilightAlpha = GetTwilightAlpha();
        float SmoothedTwilightAlpha = FMath::SmoothStep(0.0f, 1.0f, TwilightAlpha);

        // Lerp between night and day intensities for the skylight
        float NewIntensity = FMath::Lerp(NightSkyLightIntensity, DaySkyLightIntensity, SmoothedTwilightAlpha);

        SkyComp->SetIntensity(NewIntensity);

        // Note: Recapture is now managed by the timer in Tick() for efficiency.
        // SkyComp->RecaptureSky(); // <-- Moved to Tick()
    }
}

void ADayNightCycle::UpdateFog()
{
    if (!ExponentialHeightFogRef) return;

    // Get the components safely
    UExponentialHeightFogComponent* FogComp = ExponentialHeightFogRef->FindComponentByClass<UExponentialHeightFogComponent>();
    if (!FogComp) {
        // Maybe the reference is directly to the component?
        FogComp = Cast<UExponentialHeightFogComponent>(ExponentialHeightFogRef);
    }

    if (FogComp) {
        // Use SmoothedTwilightAlpha: 1 = day, 0 = night
        float TwilightAlpha = GetTwilightAlpha();
        float SmoothedTwilightAlpha = FMath::SmoothStep(0.0f, 1.0f, TwilightAlpha);

        // Example: Make fog slightly denser at night
        float TargetDensity = FMath::Lerp(0.05f, 0.01f, SmoothedTwilightAlpha); // Denser at night (alpha=0), less dense during day (alpha=1)
        FogComp->SetFogDensity(TargetDensity);

        // Optional: Adjust Fog Color based on time of day/sun position
        // Example: Make fog cooler/bluer at night, warmer/whiter during day
        FLinearColor TwilightFogColor = FMath::Lerp(FLinearColor(0.1f, 0.15f, 0.2f), FLinearColor(0.9f, 0.9f, 0.9f), SmoothedTwilightAlpha);
        FogComp->SetFogInscatteringColor(TwilightFogColor);
        // FogComp->SetFogOutscatteringColor(TwilightFogColor); // Often the same
    }
}

void ADayNightCycle::UpdateClouds()
{
    if (!VolumetricCloudRef) return;

    UVolumetricCloudComponent* CloudComp = VolumetricCloudRef->FindComponentByClass<UVolumetricCloudComponent>();
    if (!CloudComp) {
        // Maybe the reference is directly to the component?
        CloudComp = Cast<UVolumetricCloudComponent>(VolumetricCloudRef);
    }

    if (CloudComp) {
        // Use SmoothedTwilightAlpha: 1 = day, 0 = night
        float TwilightAlpha = GetTwilightAlpha();
        float SmoothedTwilightAlpha = FMath::SmoothStep(0.0f, 1.0f, TwilightAlpha);

        // Example: Vary cloud altitude slightly
        // Lower clouds at night, higher during the day. Adjust range as needed.
        float CloudBottomAltitude = FMath::Lerp(800.0f, 2000.0f, SmoothedTwilightAlpha);
        CloudComp->SetLayerBottomAltitude(CloudBottomAltitude);

        // Example: Vary Cloud Density/Coverage
        // Make clouds appear slightly denser/thicker during midday
        float CloudDensity = FMath::Lerp(0.2f, 0.6f, SmoothedTwilightAlpha);
        // Note: The actual property might vary (e.g., related to Scattering, Absorption, Slope). 
        // You might need to adjust specific parameters based on your cloud material setup.
        // For example, setting density might involve adjusting absorption or scattering coefficients.
        // Let's assume a generic density property exists or can be controlled via material params.
        // If using default volumetric clouds, you might control overall appearance via material parameters instead.
        // Example using a hypothetical SetCloudOverallDensity:
        // CloudComp->SetCloudOverallDensity(CloudDensity); 

        // If you are controlling clouds via material parameters, you would do it in UpdateSkySphere or a similar function
        // targeting the material instance.
    }
}

void ADayNightCycle::UpdateSkySphere()
{
    // Update the Dynamic Material Instance if it exists and has the "TimeOfDay" parameter
    if (SkyMID) {
        // Check if the parameter exists before setting value (good practice)
        // Note: Checking parameter presence can be slow if done every tick. Consider doing it once or relying on setup.
        // if (SkyMID->ScalarParameterValues.ContainsByPredicate([&](const FScalarParameterValue& Param){ return Param.ParameterInfo.Name == TEXT("TimeOfDay"); })) {
        SkyMID->SetScalarParameterValue(TEXT("TimeOfDay"), TimeOfDay);
        // }

        // Optionally pass other values if the material uses them:
        // SkyMID->SetScalarParameterValue(TEXT("SunElevation"), CurrentSunElevation);
        // SkyMID->SetVectorParameterValue(TEXT("SunDirection"), DirectionalLightRef->GetActorForwardVector());
    }
}