#pragma once

// 0.8.0: the foliage layer, through REDGraph (an optional dependency).
//
// The game bends trees and grass near the camera with a 64^3 "wind impulse" volume (0.5 m cells,
// 32 m across, centred on cb12 register 53) that the SpeedTree vertex shaders sample. The engine
// fills it every frame from impulse shapes (explosions, cars, ...) in the compute shader
// m_windImpulseVolumeUpdate_Shapes, then runs a small fluid simulation over it. We patch that one
// shader (REDGraph patch + a leased cbuffer at b3) so it also adds Night City Winds' local wind:
// a 16 x 16 grid of 2 m columns holding the field's deviation from what the weather curves already
// show everywhere (gust fronts arriving at different trees at different times, eddies).
//
// Far trees keep following the weather curves NCWWind writes (the fallback). Without REDGraph, or
// with one older than API 1 / too small a table, nothing registers, one log line says so, and the
// physics layers are untouched.

#include <RED4ext/RED4ext.hpp>

#include <atomic>
#include <cstdint>

namespace NCW::FoliageWind
{
// what NCWWind.reds' field is right now (its GustAt / AtTurb inputs), pushed every frame
struct FieldState
{
    float clock;      // engine time, s
    float seed;
    float mean;       // m/s at 10 m, 0 = nothing to add (disabled, pinned or curves not driven)
    float gust;       // gust fraction of the mean
    float turb;       // turbulence fraction of the mean
    float heading;    // degrees, 0 = +Y
    float frontSpeed; // m/s the gust fronts travel at
};

struct Settings
{
    std::atomic<bool> enabled{true};
    std::atomic<float> gain{0.01f}; // volume units added per frame per m/s of deviation
};

struct Stats
{
    std::atomic<std::uint32_t> state{0}; // 0 no host, 1 pending, 2 running, 3 refused
    std::atomic<std::uint64_t> swaps{0}; // dispatches of the shapes shader that ran our patch
    std::atomic<std::uint32_t> writes{0};
    std::atomic<float> lastMax{0.0f};    // largest grid entry written last time (xyz length)
};

// in Load: hands our registration to REDGraph whenever it loads (rg_host_when_ready)
void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
// main thread, every frame (NCW_SetFieldState)
void Update(const FieldState& aField);
Settings& GetSettings();
Stats& GetStats();
} // namespace NCW::FoliageWind
