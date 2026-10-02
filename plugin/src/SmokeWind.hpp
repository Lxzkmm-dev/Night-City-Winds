#pragma once

// The smoke layer: wind on smoke inside the engine's own CPU particle simulation, so smoke
// follows the wind whatever mod or effect it comes from, with no patched particle files.
//
// How: the engine sets up each emitter once at load (emitter setup, hash 2428183128) and builds
// a list of modifier functions from its cooked modifier mask; every frame it calls that list
// for each CPU-simulated particle. The setup is detoured and, for emitters the tagger
// (SmokeTag) classed as smoke, the runtime emitter gets a wind influence floor (the engine's
// own advection multiplies the wind by it), its VelocityOverLife curve stubbed, a birth point
// no higher than 2.5 m, and one modifier of ours appended that damps sideways motion. The
// per-frame passes are detoured too, to catch emitters set up before their effect was tagged
// and to keep tagged smoke out of PhysX (a PhysX-simulated emitter never runs the list).
// Other world-space emitters with a wind influence get the modifier for the optional push.
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
    // the push's influence when an emitter's own was not remembered
    std::atomic<float> defaultInfluence{0.2f};
};

struct Stats
{
    std::atomic<std::uint32_t> emittersSeen{0};   // emitter setups seen
    std::atomic<std::uint32_t> emittersWindy{0};  // ones our modifier was appended to
    std::atomic<std::uint64_t> particleSteps{0};  // push steps (the optional push only)
    std::atomic<std::uint32_t> poolsCleared{0};   // PhysX pools cleared off tagged smoke
    std::atomic<std::uint32_t> lateMatches{0};    // emitters tagged from a pass, not the setup
    std::atomic<std::uint32_t> curvesStubbed{0};  // VelocityOverLife slots replaced
    std::atomic<std::uint32_t> spawnsLowered{0};  // birth points brought down
    std::atomic<std::uint32_t> raysCast{0};       // puff samples handed to the script's rays
    std::atomic<std::uint32_t> puffsKilled{0};    // puffs retired on a hit
};

// one puff sampled for an overpass ray: where it is and how it moves, plus the handle a hit
// is reported with
struct RaySampleOut
{
    std::uint64_t key;
    std::uint32_t slot;
    float pos[3];
    float vel[3];
};

bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
bool IsAttached();
Settings& GetSettings();
Stats& GetStats();
void LogSettingChange(bool aEnabled, float aGain);
// overpass rays (on by default): the script tick drains the puff samples each frame, casts the
// rays on the main thread and reports hits back
void SetRays(bool aOn);
std::uint32_t DrainRaySamples(RaySampleOut* aOut, std::uint32_t aMax);
void RequestKill(std::uint64_t aKey, std::uint32_t aSlot);
} // namespace NCW::SmokeWind
