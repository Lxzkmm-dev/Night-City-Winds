#include "PhysXWind.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>

#include <PxPhysics.h>
#include <PxRigidDynamic.h>
#include <PxScene.h>
#include <PxSceneLock.h>

#include "Wind.hpp"

using namespace physx;

namespace
{
// PxScene vtable slots, in PhysX 3.4's PxScene.h declaration order (destructor 0, release 1, ...).
// Checked against the shipped PhysX3_x64.dll 3.4.2: slot 56 and 61 hold the functions that
// raise the simulate()/fetchResults() error strings.
constexpr std::size_t kSlotSimulate = 56;
constexpr std::size_t kSlotCollide = 58;

constexpr float kAirDensity = 1.2f; // kg/m^3, the game's own constant in the car drag
constexpr float kDragCoefficient = 1.0f;
constexpr float kMaxArea = 25.0f; // m^2, so a mis-sized body can't take a huge force
constexpr float kGravity = 9.81f;
constexpr PxU32 kBatch = 1024;

using Simulate_t = bool (*)(PxScene*, PxReal, PxBaseTask*, void*, PxU32, bool);
using Collide_t = void (*)(PxScene*, PxReal, PxBaseTask*, void*, PxU32, bool);
using PxGetPhysics_t = PxPhysics& (*)();

RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
void** g_vtable = nullptr;
Simulate_t g_simulate = nullptr;
Collide_t g_collide = nullptr;
std::atomic<bool> g_hooked{false};
PxGetPhysics_t g_getPhysics = nullptr;

CWF::PhysXWind::Settings g_settings;
CWF::PhysXWind::Stats g_stats;

// exclusion spheres (CWF_IgnoreNear), each alive for kIgnoreMs after its last refresh
constexpr std::size_t kMaxIgnore = 32;
constexpr ULONGLONG kIgnoreMs = 500;
struct Ignore
{
    PxVec3 center;
    float radius2;
    ULONGLONG until;
    std::int32_t id;
};
Ignore g_ignore[kMaxIgnore] = {};
SRWLOCK g_ignoreLock = SRWLOCK_INIT;

// copies the live exclusion spheres for this step
std::size_t SnapshotIgnores(Ignore* aOut)
{
    const auto now = GetTickCount64();
    std::size_t n = 0;
    AcquireSRWLockShared(&g_ignoreLock);
    for (const auto& e : g_ignore)
    {
        if (e.until > now)
        {
            aOut[n++] = e;
        }
    }
    ReleaseSRWLockShared(&g_ignoreLock);
    return n;
}

void PatchSlot(void** aVtable, std::size_t aSlot, void* aValue)
{
    DWORD old = 0;
    if (VirtualProtect(&aVtable[aSlot], sizeof(void*), PAGE_READWRITE, &old))
    {
        // one aligned pointer write: atomic for any thread calling through the vtable
        InterlockedExchangePointer(&aVtable[aSlot], aValue);
        VirtualProtect(&aVtable[aSlot], sizeof(void*), old, &old);
    }
}

// Drag on every loose dynamic body near the wind origin, applied as a force for this step.
void ApplyWind(PxScene* aScene)
{
    g_stats.steps.fetch_add(1, std::memory_order_relaxed);
    if (!g_settings.enabled.load(std::memory_order_relaxed) || !CWF::Wind::IsEnabled())
    {
        g_stats.pushed.store(0, std::memory_order_relaxed);
        return;
    }

    const auto w = CWF::Wind::Get();
    const PxVec3 wind(w.x, w.y, w.z);
    const auto o = CWF::Wind::GetOrigin();
    const PxVec3 origin(o.x, o.y, o.z);
    const float radius = g_settings.radius.load(std::memory_order_relaxed);
    const float radius2 = radius * radius;
    const float scale = g_settings.scale.load(std::memory_order_relaxed);
    const float maxMass = g_settings.maxMass.load(std::memory_order_relaxed);

    const PxU32 total = aScene->getNbActors(PxActorTypeFlag::eRIGID_DYNAMIC);
    g_stats.scanned.store(total, std::memory_order_relaxed);
    if (wind.magnitudeSquared() < 0.25f || scale <= 0.0f)
    {
        g_stats.pushed.store(0, std::memory_order_relaxed);
        return;
    }

    Ignore ignores[kMaxIgnore];
    const std::size_t nIgnore = SnapshotIgnores(ignores);

    PxActor* actors[kBatch];
    std::uint32_t pushed = 0;
    float maxForce = 0.0f;
    for (PxU32 start = 0; start < total; start += kBatch)
    {
        const PxU32 n = aScene->getActors(PxActorTypeFlag::eRIGID_DYNAMIC, actors, kBatch, start);
        for (PxU32 i = 0; i < n; ++i)
        {
            auto body = actors[i]->is<PxRigidDynamic>();
            if (!body || (body->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
            {
                continue;
            }
            if (body->getActorFlags() & PxActorFlag::eDISABLE_SIMULATION)
            {
                continue;
            }
            const PxVec3 p = body->getGlobalPose().p;
            if ((p - origin).magnitudeSquared() > radius2)
            {
                continue;
            }
            bool ignored = false;
            for (std::size_t k = 0; k < nIgnore && !ignored; ++k)
            {
                ignored = (p - ignores[k].center).magnitudeSquared() <= ignores[k].radius2;
            }
            if (ignored)
            {
                continue;
            }
            const float mass = body->getMass();
            if (mass <= 0.0f || mass > maxMass)
            {
                continue;
            }

            // the air past the body, and the area it shows to it (its world box projected)
            const PxVec3 rel = wind - body->getLinearVelocity();
            const float speed = rel.magnitude();
            if (speed < 0.3f)
            {
                continue;
            }
            const PxVec3 d = rel / speed;
            const PxVec3 e = body->getWorldBounds().getDimensions();
            const float area = std::min(kMaxArea, std::fabs(d.x) * e.y * e.z + std::fabs(d.y) * e.x * e.z +
                                                      std::fabs(d.z) * e.x * e.y);
            const PxVec3 force = rel * (0.5f * kAirDensity * kDragCoefficient * area * speed * scale);
            const float f = force.magnitude();

            // a body at rest stays put until the wind could plausibly slide or tip it
            if (body->isSleeping() && f < 0.15f * mass * kGravity)
            {
                continue;
            }
            body->addForce(force, PxForceMode::eFORCE, true);
            ++pushed;
            maxForce = std::max(maxForce, f);
        }
    }
    g_stats.pushed.store(pushed, std::memory_order_relaxed);
    g_stats.maxForce.store(maxForce, std::memory_order_relaxed);
}

// The caller of simulate()/collide() owns the scene for writing; if the scene enforces PhysX's
// read/write lock, take it (it is re-entrant for the thread that already holds it).
void ApplyWindLocked(PxScene* aScene)
{
    if (aScene->getFlags() & PxSceneFlag::eREQUIRE_RW_LOCK)
    {
        PxSceneWriteLock lock(*aScene, "CyberpunkWindFramework");
        ApplyWind(aScene);
    }
    else
    {
        ApplyWind(aScene);
    }
}

bool Simulate_Detour(PxScene* aThis, PxReal aDt, PxBaseTask* aTask, void* aMem, PxU32 aMemSize, bool aControl)
{
    ApplyWindLocked(aThis);
    return g_simulate(aThis, aDt, aTask, aMem, aMemSize, aControl);
}

void Collide_Detour(PxScene* aThis, PxReal aDt, PxBaseTask* aTask, void* aMem, PxU32 aMemSize, bool aControl)
{
    ApplyWindLocked(aThis);
    g_collide(aThis, aDt, aTask, aMem, aMemSize, aControl);
}
} // namespace

namespace CWF::PhysXWind
{
void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    g_handle = aHandle;
    g_sdk = aSdk;
    const auto module = GetModuleHandleW(L"PhysX3_x64.dll");
    if (module)
    {
        g_getPhysics = reinterpret_cast<PxGetPhysics_t>(GetProcAddress(module, "PxGetPhysics"));
    }
    if (!g_getPhysics)
    {
        aSdk->logger->Warn(aHandle, "PhysX3_x64.dll / PxGetPhysics not found; prop wind is off");
    }
}

// Called from the script tick (main thread) until a scene exists, then patches its vtable once.
void EnsureHooked()
{
    if (g_hooked.load(std::memory_order_acquire) || !g_getPhysics)
    {
        return;
    }
    PxPhysics& physics = g_getPhysics();
    if (physics.getNbScenes() == 0)
    {
        return;
    }
    PxScene* scene = nullptr;
    physics.getScenes(&scene, 1, 0);
    if (!scene)
    {
        return;
    }

    g_vtable = *reinterpret_cast<void***>(scene);
    g_simulate = reinterpret_cast<Simulate_t>(g_vtable[kSlotSimulate]);
    g_collide = reinterpret_cast<Collide_t>(g_vtable[kSlotCollide]);
    PatchSlot(g_vtable, kSlotSimulate, reinterpret_cast<void*>(&Simulate_Detour));
    PatchSlot(g_vtable, kSlotCollide, reinterpret_cast<void*>(&Collide_Detour));
    g_hooked.store(true, std::memory_order_release);

    if (g_sdk)
    {
        g_sdk->logger->InfoF(g_handle, "PhysX scene hooked (%u scene(s), vtable %p)", physics.getNbScenes(), g_vtable);
    }
}

void Shutdown()
{
    if (g_hooked.exchange(false))
    {
        PatchSlot(g_vtable, kSlotSimulate, reinterpret_cast<void*>(g_simulate));
        PatchSlot(g_vtable, kSlotCollide, reinterpret_cast<void*>(g_collide));
    }
}

bool IsHooked()
{
    return g_hooked.load(std::memory_order_acquire);
}

void IgnoreNear(std::int32_t aId, float aX, float aY, float aZ, float aRadius)
{
    const PxVec3 c(aX, aY, aZ);
    const auto now = GetTickCount64();
    AcquireSRWLockExclusive(&g_ignoreLock);
    // refresh this caller's sphere (same id), else take a free slot
    Ignore* slot = nullptr;
    for (auto& e : g_ignore)
    {
        if (e.until > now && e.id == aId)
        {
            slot = &e;
            break;
        }
    }
    for (std::size_t i = 0; !slot && i < kMaxIgnore; ++i)
    {
        if (g_ignore[i].until <= now)
        {
            slot = &g_ignore[i];
        }
    }
    if (slot)
    {
        *slot = {c, aRadius * aRadius, now + kIgnoreMs, aId};
    }
    ReleaseSRWLockExclusive(&g_ignoreLock);
}

Settings& GetSettings()
{
    return g_settings;
}

Stats& GetStats()
{
    return g_stats;
}
} // namespace CWF::PhysXWind
