#include "VehicleDrag.hpp"

#include <cmath>

#include "Addresses.hpp"
#include "Wind.hpp"

namespace
{
// The engine's Vector3 (12 bytes, x y z).
struct Vector3
{
    float x, y, z;
};

// From the disassembly of 2.31 (hash 3489929719): rcx = WheeledPhysics*, rdx = const Vector3*
// (a copy of physicsData.velocity on the caller's stack), xmm2 = dt. Returns nothing.
using ApplyAirResistance_t = void (*)(void* aThis, const Vector3* aVelocity, float aDeltaTime);

// WheeledPhysics + 0xC90: the car's airResistanceFactor (TweakDB VehicleDriveModelData), read
// only for the diagnostics.
constexpr std::ptrdiff_t kAirResistanceFactorOffset = 0xC90;

ApplyAirResistance_t g_original = nullptr;
void* g_target = nullptr;

void ApplyAirResistance_Detour(void* aThis, const Vector3* aVelocity, float aDeltaTime)
{
    if (!CWF::Wind::IsEnabled() || !aVelocity)
    {
        g_original(aThis, aVelocity, aDeltaTime);
        return;
    }

    const auto wind = CWF::Wind::Get();
    const Vector3 air{aVelocity->x - wind.x, aVelocity->y - wind.y, aVelocity->z - wind.z};

    CWF::Wind::dragCalls.fetch_add(1, std::memory_order_relaxed);
    CWF::Wind::lastGroundSpeed.store(
        std::sqrt(aVelocity->x * aVelocity->x + aVelocity->y * aVelocity->y + aVelocity->z * aVelocity->z),
        std::memory_order_relaxed);
    CWF::Wind::lastAirSpeed.store(std::sqrt(air.x * air.x + air.y * air.y + air.z * air.z),
                                  std::memory_order_relaxed);
    if (aThis)
    {
        CWF::Wind::lastResistanceFactor.store(
            *reinterpret_cast<const float*>(static_cast<const char*>(aThis) + kAirResistanceFactorOffset),
            std::memory_order_relaxed);
    }

    g_original(aThis, &air, aDeltaTime);
}
} // namespace

namespace CWF::VehicleDrag
{
bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    const auto address = ResolveOrZero(Hashes::WheeledPhysics_ApplyAirResistance);
    if (!address)
    {
        aSdk->logger->Warn(aHandle, "ApplyAirResistance hash did not resolve; vehicle wind is off for this game version");
        return false;
    }

    g_target = reinterpret_cast<void*>(address);
    if (!aSdk->hooking->Attach(aHandle, g_target, reinterpret_cast<void*>(&ApplyAirResistance_Detour),
                               reinterpret_cast<void**>(&g_original)))
    {
        aSdk->logger->Warn(aHandle, "could not hook ApplyAirResistance; vehicle wind is off");
        g_target = nullptr;
        g_original = nullptr;
        return false;
    }

    aSdk->logger->InfoF(aHandle, "vehicle drag hook attached at %p", g_target);
    return true;
}

void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    if (g_target)
    {
        aSdk->hooking->Detach(aHandle, g_target);
        g_target = nullptr;
    }
}

bool IsAttached()
{
    return g_target != nullptr;
}
} // namespace CWF::VehicleDrag
