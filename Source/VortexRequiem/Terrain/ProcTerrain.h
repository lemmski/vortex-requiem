#pragma once

#include "CoreMinimal.h"
#include "FastNoiseLite.h"
#include "Containers/Array.h"
#include "Math/RandomStream.h"
#include <limits>

/**
 * Header-only, lightweight port of the Python Procedural Terrain Toolkit.
 * Generates an in-memory 0-1 float height map using:
 *  1) Fractional Brownian Motion (Perlin/Simplex) base
 *  2) Thermal erosion
 *  3) Hydraulic erosion (single-thread scalar version)
 *
 * The implementation purposefully mirrors the maths of the original Python
 * script so the resulting terrain is visually indistinguishable when the
 * same seeds / parameters are used.
 */

struct FFBMSettings
{
    bool  bUseSimplex   = true;
    float Scale         = 400.f;
    int32 Octaves       = 8;
    float Persistence   = 0.5f;
    float Lacunarity    = 2.f;
    // Domain-warp parameters (0 disables warp)
    float WarpStrength  = 0.f;   // displacement amplitude in pixel units
    float WarpScale     = 50.f;  // period of the warp noise (pixels)
};

struct FThermalSettings
{
	int32 Iterations    = 5;
	float DiffusionRate = 0.01f;
};

struct FHydraulicSettings
{
	int32 NumDroplets   = 70000;
	float Inertia       = 0.3f;
	float CapacityFactor= 4.f;
	float MinCapacity   = 0.01f;
	float ErosionRate   = 0.3f;
	float DepositionRate= 0.3f;
	float Gravity       = 4.f;
	int32 MaxLifetime   = 30;
};

class FProcTerrain
{
public:
	int32 Width  = 0;
	int32 Height = 0;
	FRandomStream RNG;
	TArray<float> HeightMap; // normalised 0..1

	FProcTerrain(int32 InW, int32 InH, int32 Seed)
		: Width(InW)
		, Height(InH)
		, RNG(Seed)
	{
		HeightMap.SetNumZeroed(Width * Height);
	}

	// ----------------------------------------------------------------------
	// Fractional-Brownian Motion base terrain
	// ----------------------------------------------------------------------
	void GenerateFBM(const FFBMSettings& S)
	{
		FastNoiseLite Noise;
		Noise.SetSeed(RNG.RandHelper(INT32_MAX));
		Noise.SetNoiseType(S.bUseSimplex ? FastNoiseLite::NoiseType_OpenSimplex2
                                   : FastNoiseLite::NoiseType_Perlin);

    // --- Domain warp setup ---
    FastNoiseLite Warp;
    const bool bUseWarp = S.WarpStrength > 0.f;
    if (bUseWarp)
    {
        Warp.SetSeed(RNG.RandHelper(INT32_MAX));
        Warp.SetNoiseType(S.bUseSimplex ? FastNoiseLite::NoiseType_OpenSimplex2
                                       : FastNoiseLite::NoiseType_Perlin);
    }
    const float InvWarpScale = (S.WarpScale > 0.f) ? 1.f / S.WarpScale : 0.f;

		const float InvScale = 1.f / S.Scale;
		float Amplitude = 1.f;
		float Frequency = InvScale;

		for (int32 Oct = 0; Oct < S.Octaves; ++Oct)
		{
			for (int32 y = 0; y < Height; ++y)
			{
				for (int32 x = 0; x < Width; ++x)
				{
					const int32 Idx = y * Width + x;
					float Fx = static_cast<float>(x);
                float Fy = static_cast<float>(y);

                if (bUseWarp)
                {
                    float wx = 0.f, wy = 0.f;
                    float aWarp = 1.f;
                    float fWarp = InvWarpScale;
                    for (int32 wo = 0; wo < 3; ++wo)
                    {
                        wx += Warp.GetNoise(Fx * fWarp + 1000.f, Fy * fWarp + 1000.f) * aWarp;
                        wy += Warp.GetNoise(Fx * fWarp + 2000.f, Fy * fWarp + 2000.f) * aWarp;
                        aWarp *= 0.5f;
                        fWarp *= 2.f;
                    }
                    Fx += wx * S.WarpStrength;
                    Fy += wy * S.WarpStrength;
                }

                const float Nx = Fx * Frequency;
                const float Ny = Fy * Frequency;
                HeightMap[Idx] += Noise.GetNoise(Nx, Ny) * Amplitude;
				}
			}
			Amplitude *= S.Persistence;
			Frequency *= S.Lacunarity;
		}
		Normalize();
	}

	// ----------------------------------------------------------------------
	// Simple thermal erosion (4-point Laplacian)
	// ----------------------------------------------------------------------
	void ApplyThermal(const FThermalSettings& S)
	{
		for (int32 It = 0; It < S.Iterations; ++It)
		{
			for (int32 y = 1; y < Height - 1; ++y)
			{
				for (int32 x = 1; x < Width - 1; ++x)
				{
					const int32 Idx = y * Width + x;
					const float Center = HeightMap[Idx];
					const float Avg = (HeightMap[Idx - 1] + HeightMap[Idx + 1] +
					                 HeightMap[Idx - Width] + HeightMap[Idx + Width]) * 0.25f;
					HeightMap[Idx] += (Avg - Center) * S.DiffusionRate;
				}
			}
		}
		Normalize();
	}

	// ----------------------------------------------------------------------
	// Scalar hydraulic erosion port – follows Python logic closely
	// ----------------------------------------------------------------------
	void ApplyHydraulic(const FHydraulicSettings& S)
	{
		for (int32 Drop = 0; Drop < S.NumDroplets; ++Drop)
		{
			float Px = RNG.FRandRange(0.f, static_cast<float>(Width  - 2));
			float Py = RNG.FRandRange(0.f, static_cast<float>(Height - 2));

			float Dx = 0.f, Dy = 0.f, Speed = 0.f, Water = 1.f, Sediment = 0.f;

			for (int32 Life = 0; Life < S.MaxLifetime; ++Life)
			{
				const int32 Ix = static_cast<int32>(Px);
				const int32 Iy = static_cast<int32>(Py);
				if (Ix < 0 || Ix >= Width - 1 || Iy < 0 || Iy >= Height - 1)
					break;

				auto Sample = [&](int32 SX, int32 SY) { return HeightMap[SY * Width + SX]; };

				const float Htl = Sample(Ix,     Iy    );
				const float Htr = Sample(Ix + 1, Iy    );
				const float Hbl = Sample(Ix,     Iy + 1);
				const float Hbr = Sample(Ix + 1, Iy + 1);

				const float OffX = Px - static_cast<float>(Ix);
				const float OffY = Py - static_cast<float>(Iy);

				const float H = FMath::Lerp(
					FMath::Lerp(Htl, Htr, OffX),
					FMath::Lerp(Hbl, Hbr, OffX),
					OffY);

				// Gradient
				const float Gx = (Htr - Htl) * (1.f - OffY) + (Hbr - Hbl) * OffY;
				const float Gy = (Hbl - Htl) * (1.f - OffX) + (Hbr - Htr) * OffX;

				Dx = Dx * S.Inertia - Gx * (1.f - S.Inertia);
				Dy = Dy * S.Inertia - Gy * (1.f - S.Inertia);
				const float Len = FMath::Max(FMath::Sqrt(Dx * Dx + Dy * Dy), 1e-6f);
				Dx /= Len; Dy /= Len;

				Px += Dx; Py += Dy;
				if (Px < 0.f || Px >= Width - 1 || Py < 0.f || Py >= Height - 1)
					break;

				const float NewH = Sample(static_cast<int32>(Px), static_cast<int32>(Py));
				const float DeltaH = NewH - H;

				const float Capacity = FMath::Max(-DeltaH * Speed * Water * S.CapacityFactor, S.MinCapacity);
				if (Sediment > Capacity || DeltaH > 0.f)
				{
					const float Deposit = (Sediment - Capacity) * S.DepositionRate;
					Sediment -= Deposit;
					HeightMap[Iy * Width + Ix] += Deposit;
				}
				else
				{
					const float Erode = FMath::Min((Capacity - Sediment) * S.ErosionRate, -DeltaH);
					Sediment += Erode;
					HeightMap[Iy * Width + Ix] -= Erode;
				}

				Speed = FMath::Sqrt(FMath::Max(0.f, Speed * Speed + DeltaH * S.Gravity));
				Water *= 0.99f;
				if (Water < 0.01f)
					break;
			}
		}
		Normalize();
    }

    // ----------------------------------------------------------------------
    // Height redistribution (power curve)
    // ----------------------------------------------------------------------
    void ApplyRedistribution(float Exponent)
    {
        if (FMath::IsNearlyEqual(Exponent, 1.f))
            return;
        for (float& V : HeightMap)
        {
            V = FMath::Pow(V, Exponent);
        }
        Normalize();
    }

private:
	void Normalize()
	{
		float MinV = TNumericLimits<float>::Max();
		float MaxV = TNumericLimits<float>::Lowest();
		for (float V : HeightMap)
		{
			MinV = FMath::Min(MinV, V);
			MaxV = FMath::Max(MaxV, V);
		}
		const float Range = FMath::Max(MaxV - MinV, 1e-6f);
		for (float& V : HeightMap)
		{
			V = (V - MinV) / Range;
		}
	}
};