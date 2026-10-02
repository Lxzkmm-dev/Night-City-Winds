#include "VehicleDrag.hpp"

#include <algorithm>
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

// From the disassembly of 2.31 (hash 3489929719, 0x8B469C): rcx = WheeledPhysics*, rdx = const
// Vector3* (a copy of physicsData.velocity on the caller's stack), xmm2 = dt. Returns nothing.
// It reads airResistanceFactor at this+0xC90, computes F = -1.2 k |v|^2 v^, and adds F dt to
// vehicle::PhysicsData at [[this+0x60]+0x2D0]: force +0x00, torque +0x0C, velocity +0x18,
// angularVelocity +0x24, orientation +0x30 (quaternion i j k r), inverseMass +0x40.
using ApplyAirResistance_t = void (*)(void* aThis, const Vector3* aVelocity, float aDeltaTime);

constexpr std::ptrdiff_t kAirResistanceFactorOffset = 0xC90;
constexpr std::ptrdiff_t kBaseObjectOffset = 0x60;
constexpr std::ptrdiff_t kPhysicsDataOffset = 0x2D0;
constexpr std::ptrdiff_t kForce = 0x00;
constexpr std::ptrdiff_t kTorque = 0x0C;
constexpr std::ptrdiff_t kOrientation = 0x30;
constexpr std::ptrdiff_t kInverseMass = 0x40;
constexpr float kAirDensity = 1.2f;
constexpr float kGravity = 9.81f;

ApplyAirResistance_t g_original = nullptr;
void* g_target = nullptr;
NCW::VehicleDrag::Settings g_settings;
NCW::VehicleDrag::Stats g_stats;

template<typename T>
T& At(void* aBase, std::ptrdiff_t aOffset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(aBase) + aOffset);
}

// the body's forward (+Y) and right (+X) axes in world space from its orientation quaternion
void Axes(const float* aQ, Vector3& aForward, Vector3& aRight)
{
    const float x = aQ[0], y = aQ[1], z = aQ[2], w = aQ[3];
    aRight = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w)};
    aForward = {2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w)};
}

// Side force, yaw and lift from the wind's share of the airflow, added to the chassis as F dt
// (the field accumulates impulse for the step, like the game's own drag).
void ApplyAero(void* aThis, const Vector3& aVel, const Vector3& aAir, float aDt)
{
    if (!g_settings.aero.load(std::memory_order_relaxed) || aDt <= 0.0f || aDt > 0.25f)
    {
        return;
    }
    auto* base = At<void*>(aThis, kBaseObjectOffset);
    auto* data = base ? At<void*>(base, kPhysicsDataOffset) : nullptr;
    if (!data)
    {
        return;
    }
    const float k = At<const float>(aThis, kAirResistanceFactorOffset);
    const float invMass = At<const float>(data, kInverseMass);
    if (!(k > 0.0f) || !(invMass > 0.0f) || !std::isfinite(k) || !std::isfinite(invMass))
    {
        return;
    }
    Vector3 forward, right;
    Axes(&At<const float>(data, kOrientation), forward, right);
    const float n2 = forward.x * forward.x + forward.y * forward.y + forward.z * forward.z;
    if (!(n2 > 0.9f && n2 < 1.1f))
    {
        return; // not a unit quaternion: not the layout we expect
    }
    const float airSpeed = std::sqrt(aAir.x * aAir.x + aAir.y * aAir.y + aAir.z * aAir.z);
    const float maxForce = 0.25f * kGravity / invMass; // a quarter of the car's weight

    // lateral airspeed (the game's drag already takes 1.2 k |v| v of it; the side gain adds
    // the rest of the side area), and the yaw it gives at the centre of pressure
    const float lat = aAir.x * right.x + aAir.y * right.y + aAir.z * right.z;
    float side = -kAirDensity * k * airSpeed * lat * g_settings.sideGain.load(std::memory_order_relaxed);
    side = std::clamp(side, -maxForce, maxForce);
    const Vector3 f{right.x * side, right.y * side, right.z * side};
    const float lever = g_settings.yawLever.load(std::memory_order_relaxed);
    const Vector3 r{forward.x * lever, forward.y * lever, forward.z * lever};
    const Vector3 torque{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};

    // lift: the wind's change to 0.5 rho Cl A |v_h|^2 (ground-speed lift is the game's own)
    const float vh2 = aVel.x * aVel.x + aVel.y * aVel.y;
    const float ah2 = aAir.x * aAir.x + aAir.y * aAir.y;
    float lift = 0.5f * kAirDensity * g_settings.liftArea.load(std::memory_order_relaxed) * (ah2 - vh2);
    lift = std::clamp(lift, -maxForce, maxForce);

    auto* force = &At<float>(data, kForce);
    force[0] += f.x * aDt;
    force[1] += f.y * aDt;
    force[2] += (f.z + lift) * aDt;
    auto* tq = &At<float>(data, kTorque);
    tq[0] += torque.x * aDt;
    tq[1] += torque.y * aDt;
    tq[2] += torque.z * aDt;

    g_stats.lastSideForce.store(side, std::memory_order_relaxed);
    g_stats.lastYawTorque.store(torque.z, std::memory_order_relaxed);
    g_stats.lastLift.store(lift, std::memory_order_relaxed);
}

void ApplyAirResistance_Detour(void* aThis, const Vector3* aVelocity, float aDeltaTime)
{
    if (!NCW::Wind::IsEnabled() || !aVelocity || !aThis)
    {
        g_original(aThis, aVelocity, aDeltaTime);
        return;
    }

    const auto wind = NCW::Wind::Get();
    const Vector3 air{aVelocity->x - wind.x, aVelocity->y - wind.y, aVelocity->z - wind.z};

    NCW::Wind::dragCalls.fetch_add(1, std::memory_order_relaxed);
    NCW::Wind::lastGroundSpeed.store(
        std::sqrt(aVelocity->x * aVelocity->x + aVelocity->y * aVelocity->y + aVelocity->z * aVelocity->z),
        std::memory_order_relaxed);
    NCW::Wind::lastAirSpeed.store(std::sqrt(air.x * air.x + air.y * air.y + air.z * air.z),
                                  std::memory_order_relaxed);
    NCW::Wind::lastResistanceFactor.store(At<const float>(aThis, kAirResistanceFactorOffset),
                                          std::memory_order_relaxed);

    g_original(aThis, &air, aDeltaTime);
    ApplyAero(aThis, *aVelocity, air, aDeltaTime);
}
} // namespace

namespace NCW::VehicleDrag
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

Settings& GetSettings()
{
    return g_settings;
}

Stats& GetStats()
{
    return g_stats;
}
} // namespace NCW::VehicleDrag
