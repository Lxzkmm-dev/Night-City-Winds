#include "Wind.hpp"

namespace
{
std::atomic<float> g_x{0.0f};
std::atomic<float> g_y{0.0f};
std::atomic<float> g_z{0.0f};
std::atomic<bool> g_enabled{true};
std::atomic<float> g_ox{0.0f};
std::atomic<float> g_oy{0.0f};
std::atomic<float> g_oz{0.0f};
} // namespace

namespace NCW
{
std::atomic<std::uint32_t> Wind::dragCalls{0};
std::atomic<float> Wind::lastGroundSpeed{0.0f};
std::atomic<float> Wind::lastAirSpeed{0.0f};
std::atomic<float> Wind::lastResistanceFactor{0.0f};

void Wind::Set(Vec3 aWind)
{
    g_x.store(aWind.x, std::memory_order_relaxed);
    g_y.store(aWind.y, std::memory_order_relaxed);
    g_z.store(aWind.z, std::memory_order_relaxed);
}

Vec3 Wind::Get()
{
    return {g_x.load(std::memory_order_relaxed), g_y.load(std::memory_order_relaxed),
            g_z.load(std::memory_order_relaxed)};
}

void Wind::SetOrigin(Vec3 aOrigin)
{
    g_ox.store(aOrigin.x, std::memory_order_relaxed);
    g_oy.store(aOrigin.y, std::memory_order_relaxed);
    g_oz.store(aOrigin.z, std::memory_order_relaxed);
}

Vec3 Wind::GetOrigin()
{
    return {g_ox.load(std::memory_order_relaxed), g_oy.load(std::memory_order_relaxed),
            g_oz.load(std::memory_order_relaxed)};
}

void Wind::SetEnabled(bool aEnabled)
{
    g_enabled.store(aEnabled, std::memory_order_relaxed);
}

bool Wind::IsEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}
} // namespace NCW
