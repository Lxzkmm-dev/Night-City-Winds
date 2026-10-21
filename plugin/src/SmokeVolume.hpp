#pragma once

// 0.9.0: volumetric smoke through the game's own fog, by way of REDGraph (an optional dependency).
//
// The volumetric fog's lighting compute shader has a variant that blends GPU "volumetric
// particles" (position, radius, colour, density, falloff) into each froxel before lighting it -
// so they get the fog's sun, shadows, local lights and GI - but 2.31 only ever runs the
// _NoParticles variants. We register the with-particles bytecode as a patch of each of them
// (81702d32 raster, 61f9ce9c RT / PT), its particle reads moved to a cbuffer leased at b3, and
// fill that cbuffer every frame with the puffs of tagged smoke nearest the player: their
// positions come from SmokeWind's modifier inside the engine's particle simulation, so the
// volume moves exactly as the sprite smoke does, wind included. The sprites stay for detail.
//
// Without REDGraph nothing registers and one log line says so.

#include <RED4ext/RED4ext.hpp>

#include <atomic>
#include <cstdint>

namespace NCW::SmokeVolume
{
struct Settings
{
    std::atomic<bool> enabled{true};
    std::atomic<float> density{1.0f};    // multiplies each class's density
    std::atomic<float> radius{1.0f};     // multiplies each class's radius
    std::atomic<std::uint32_t> maxPuffs{128};
};

struct Stats
{
    std::atomic<std::uint32_t> state{0};   // 0 no host, 1 pending, 2 running, 3 refused
    std::atomic<std::uint64_t> swaps{0};   // fog dispatches that ran our patch (both variants)
    std::atomic<std::uint32_t> recorded{0}; // puffs recorded by the simulation last drain
    std::atomic<std::uint32_t> sent{0};     // puffs sent to the fog last frame
};

void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
// from SmokeWind's modifier, on the particle threads, for each tagged smoke puff each step:
// cheap and lock-free. aParticle is the engine's particle (position at +0, age +0xC, seed +0x94),
// aClass a SmokeTag::Class, aLife the puff's life fraction 0..1.
void Record(const std::uint8_t* aParticle, std::uint8_t aClass, float aLife);
// main thread, every frame: gathers the puffs, picks the nearest, writes the cbuffer
void Update();
Settings& GetSettings();
Stats& GetStats();
} // namespace NCW::SmokeVolume
