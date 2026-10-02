#pragma once

// The smoke layer (option B of docs/OWN_SMOKE_RESEARCH): our own wind step inside the engine's
// CPU particle simulation, so smoke follows the wind whatever mod or effect it comes from,
// with no patched particle files.
//
// How: the engine sets up each emitter once at load (emitter setup, hash 2428183128) and builds
// a list of modifier functions from its cooked modifier mask; every frame it calls that list
// for each CPU-simulated particle. We detour the setup and, for world-space emitters with a
// wind influence, append one function of ours to the list. It steers the particle's horizontal
// velocity to wind x influence x gain once the particle has left its source's jet, on top of
// the engine's own wind advection. Emitters with a Collision module are simulated in PhysX
// and never run the list; the smoke archive strips that module from smoke.
// See docs/SMOKE_WIND_ANALYSIS.md 5f for the engine layout this relies on.

#include <atomic>
#include <cstdint>

#include <RED4ext/RED4ext.hpp>

namespace NCW::SmokeWind
{
struct Settings
{
    // off by default: with influence, no collision and the weather curves driven, the engine's
    // own additive wind moves smoke more naturally than this override (Omar, 2026-10-02)
    std::atomic<bool> enabled{false};
    std::atomic<float> gain{1.0f};
    // used when an emitter's own influence is 0 but it was classed as smoke (later phases)
    std::atomic<float> defaultInfluence{0.2f};
};

struct Stats
{
    std::atomic<std::uint32_t> emittersSeen{0};   // emitter setups seen
    std::atomic<std::uint32_t> emittersWindy{0};  // ones we appended our modifier to
    std::atomic<std::uint64_t> particleSteps{0};  // our modifier calls
    std::atomic<float> lastDt{0.0f};
};

bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
bool IsAttached();
Settings& GetSettings();
Stats& GetStats();
void LogSettingChange(bool aEnabled, float aGain);
} // namespace NCW::SmokeWind
