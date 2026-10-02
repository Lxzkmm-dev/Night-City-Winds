#pragma once

// The vehicle layer: the game's own car air drag (WheeledPhysics::ApplyAirResistance, hash
// 3489929719) detoured to act on the airspeed (velocity - wind), so every car feels the wind
// with its vanilla tuning. 0.7.0 adds what the isotropic drag can't give: a crosswind side
// force (a car shows more area sideways than head-on), the yaw it causes (the centre of
// pressure sits ahead of the centre of mass, so the nose is pushed downwind) and lift (the
// wind's change to the airflow over the body lightens or plants the car).
// docs/PHYSICS_RE_FINDINGS.md 4.1 and 4.2.

#include <atomic>
#include <cstdint>

#include <RED4ext/RED4ext.hpp>

namespace NCW::VehicleDrag
{
struct Settings
{
    std::atomic<bool> aero{true};
    // extra lateral drag as a fraction of the game's isotropic drag (side area / frontal - 1)
    std::atomic<float> sideGain{1.0f};
    // metres the centre of pressure sits ahead of the centre of mass (yaw lever)
    std::atomic<float> yawLever{0.5f};
    // lift coefficient x top area, m2 (0.3 x 8 for a car); only the wind's share is applied
    std::atomic<float> liftArea{2.4f};
};

struct Stats
{
    std::atomic<float> lastSideForce{0.0f}; // N
    std::atomic<float> lastYawTorque{0.0f}; // N m
    std::atomic<float> lastLift{0.0f};      // N, up
};

bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
bool IsAttached();
Settings& GetSettings();
Stats& GetStats();
} // namespace NCW::VehicleDrag
