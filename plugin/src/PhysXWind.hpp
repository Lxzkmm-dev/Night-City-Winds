#pragma once

#include <RED4ext/RED4ext.hpp>

#include <atomic>
#include <cstdint>

namespace NCW::PhysXWind
{
// Wind on loose PhysX bodies (props, debris, VAXIS's dynamic decorations): aerodynamic drag on
// every dynamic, non-kinematic actor within a radius of the wind origin (the player), added
// right before each PhysX step. Installs itself by patching NpScene's vtable (simulate, slot 56;
// collide, slot 58; fixed by PhysX 3.4's PxScene.h) the first time a scene exists.
void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void EnsureHooked();
void Shutdown();
bool IsHooked();

// Bodies inside this sphere are left out of the prop layer for the next half second: for mods
// that fly their own bodies and apply the wind themselves. Call it every frame per body with a
// stable id (it refreshes that id's sphere); up to 32 live spheres.
void IgnoreNear(std::int32_t aId, float aX, float aY, float aZ, float aRadius);

struct Settings
{
    std::atomic<bool> enabled{true};
    std::atomic<float> radius{50.0f};  // m around the origin
    std::atomic<float> scale{1.0f};    // force multiplier
    std::atomic<float> maxMass{800.0f}; // kg; heavier bodies are left alone
};
Settings& GetSettings();

struct Stats
{
    std::atomic<std::uint32_t> steps{0};   // simulate/collide calls seen
    std::atomic<std::uint32_t> scanned{0}; // dynamic actors in the scene, last step
    std::atomic<std::uint32_t> pushed{0};  // actors given a wind force, last step
    std::atomic<float> maxForce{0.0f};     // largest force applied, last step (N)
};
Stats& GetStats();
} // namespace NCW::PhysXWind
