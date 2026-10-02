#pragma once

// The wind the physics hooks read. For now one world-space vector set from redscript
// (CWF_SetWind); the field model (gusts, height, shelter, weather) lives in redscript and
// pushes its result here each frame. Read from the physics jobs, written from scripts, so
// every value is atomic.

#include <atomic>
#include <cstdint>

namespace CWF
{
struct Vec3
{
    float x, y, z;
};

class Wind
{
public:
    static void Set(Vec3 aWind);
    static Vec3 Get();
    // where the wind was sampled (the player or their car): the prop layer works around it
    static void SetOrigin(Vec3 aOrigin);
    static Vec3 GetOrigin();
    static void SetEnabled(bool aEnabled);
    static bool IsEnabled();

    // vehicle drag hook diagnostics
    static std::atomic<std::uint32_t> dragCalls;
    static std::atomic<float> lastGroundSpeed;
    static std::atomic<float> lastAirSpeed;
    static std::atomic<float> lastResistanceFactor;
};
} // namespace CWF
