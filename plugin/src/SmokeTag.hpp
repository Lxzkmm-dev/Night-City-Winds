#pragma once

// Smoke classification at load, so the plugin can do what the smoke archive did without files:
// as each particle system loads (a script callback hands it over), its emitters whose effect path
// and emitter name say smoke, steam or dust (and not fire, sparks or debris) are
//   - fingerprinted from their cooked render blob and remembered with a class and a wind
//     influence floor, so the emitter setup hook can set the influence the engine's own wind
//     advects by and the drag our modifier applies,
//   - given the floor on the script-visible emitter and blob too, for copies made later.
// (Their Collision module stays: the PhysX decision is made before this callback, so the
// passes in SmokeWind.cpp keep tagged smoke out of PhysX instead.)
// See docs/SMOKE_WIND_ANALYSIS.md.

#include <atomic>
#include <cstdint>

#include <RED4ext/RED4ext.hpp>

namespace NCW::SmokeTag
{
// what kind of smoke an emitter is: its feel (influence floor, sideways drag) follows from it
enum class Class : std::uint8_t
{
    None = 0,
    Smoke,   // body smoke: the designers' 0.2
    Steam,   // steam, vapour, mist: thin, takes the wind readily
    Dust,    // dust, ash, soot: light but gritty
    Column,  // tall columns and plumes: heavy, keeps its lean longer
    Exhaust, // vehicle exhaust: short-lived, close to the body
};

struct Stats
{
    std::atomic<std::uint32_t> systemsSeen{0};
    std::atomic<std::uint32_t> systemsTagged{0};
    std::atomic<std::uint32_t> emittersTagged{0};
    std::atomic<std::uint32_t> collisions{0};      // tagged emitters with a Collision module
    std::atomic<std::uint32_t> setupHits{0};       // emitter setups matched by the full fingerprint
    std::atomic<std::uint32_t> setupHitsWeak{0};   // matched only without the cooked data bytes
    std::atomic<std::uint32_t> surveyEntries{0};   // untagged systems recorded for the survey
};

void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
// the script callback's entry: classify and tag one loaded CParticleSystem
void TagSystem(RED4ext::ISerializable* aSystem, const char* aPath);
// for the emitter setup hook: the class and influence floor remembered for this cooked blob
// (floor 0 = not smoke)
float FloorFor(const std::uint8_t* aBlob, Class& aClass);
// the same lookup from the runtime emitter's own copies of the cooked fields, for emitters set
// up before their effect was tagged (the render side can be a few ms ahead of the load callback)
float FloorForFields(std::uint64_t aModMask, std::uint64_t aInitMask, std::uint32_t aNumMod, std::uint32_t aNumInit,
                     std::uint32_t aMaxParticles, std::uint64_t aSimHash, Class& aClass);
// the sideways drag coefficient our modifier uses for a class (quadratic, like the engine's)
float DragFor(Class aClass);
Stats& GetStats();
// the floor for body smoke; the other classes scale with it (default 0.2)
std::atomic<float>& Floor();
// the coverage survey: while on, every loaded particle system that got no tag is recorded
// (path, emitter names, wind influence, whether its folder was a smoke folder); DumpSurvey
// logs and clears the record, on the main thread
void SetSurvey(bool aOn);
std::uint32_t DumpSurvey();
} // namespace NCW::SmokeTag
