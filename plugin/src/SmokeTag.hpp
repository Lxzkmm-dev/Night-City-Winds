#pragma once

// Smoke classification at load, so the plugin can do what the smoke archive did without files:
// as each particle system loads (a script callback hands it over), its emitters whose effect path
// and emitter name say smoke, steam or dust (and not fire, sparks or debris) are
//   - fingerprinted from their cooked render blob and remembered with a wind influence floor,
//     so the emitter setup hook can set the influence the engine's own wind advects by,
//   - given the floor on the script-visible emitter and blob too, for copies made later.
// (Their Collision module stays: the PhysX decision is made before this callback, so the
// passes in SmokeWind.cpp keep tagged smoke out of PhysX instead.)
// See docs/SMOKE_WIND_ANALYSIS.md.

#include <atomic>
#include <cstdint>

#include <RED4ext/RED4ext.hpp>

namespace NCW::SmokeTag
{
struct Stats
{
    std::atomic<std::uint32_t> systemsSeen{0};
    std::atomic<std::uint32_t> systemsTagged{0};
    std::atomic<std::uint32_t> emittersTagged{0};
    std::atomic<std::uint32_t> collisions{0};       // tagged emitters with a Collision module
    std::atomic<std::uint32_t> setupHits{0};     // emitter setups matched by the full fingerprint
    std::atomic<std::uint32_t> setupHitsWeak{0}; // matched only without the cooked data bytes
};

void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
// the script callback's entry: classify and tag one loaded CParticleSystem
void TagSystem(RED4ext::ISerializable* aSystem, const char* aPath);
// for the emitter setup hook: the influence floor remembered for this cooked blob, or 0
float FloorFor(const std::uint8_t* aBlob);
// the same lookup from the runtime emitter's own copies of the cooked fields, for emitters set
// up before their effect was tagged (the render side can be a few ms ahead of the load callback)
float FloorForFields(std::uint64_t aModMask, std::uint64_t aInitMask, std::uint32_t aNumMod, std::uint32_t aNumInit,
                     std::uint32_t aMaxParticles, std::uint64_t aSimHash);
Stats& GetStats();
std::atomic<float>& Floor();
} // namespace NCW::SmokeTag
