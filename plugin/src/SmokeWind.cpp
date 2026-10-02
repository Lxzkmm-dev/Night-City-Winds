#include "SmokeWind.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "Addresses.hpp"
#include "Wind.hpp"

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

CWF::SmokeWind::Settings g_settings;
CWF::SmokeWind::Stats g_stats;

// ---- engine pieces (2.31 disassembly; resolved by hash) ------------------------------------
// void* EmitterSetup(?, const Blob* desc, ?, ?): builds a runtime emitter from the cooked
// render blob and calls the modifier-list builder (0x41826C) on it. Only rdx is read.
using Setup_t = void* (*)(void*, void*, void*, void*);
// the allocator the list builder uses: (bytes) -> zeroed block
using Alloc_t = void* (*)(std::uint32_t);
// the matching free, called with {pointer, 0}
using Free_t = void (*)(void*);

Setup_t g_setupOriginal = nullptr;
void* g_setupTarget = nullptr;
Alloc_t g_alloc = nullptr;
Free_t g_free = nullptr;

// runtime emitter (layout confirmed by the 2026-10-02 dumps, docs/SMOKE_WIND_ANALYSIS.md 5f)
constexpr std::ptrdiff_t kEmitterList = 0x190;     // modifier function pointers
constexpr std::ptrdiff_t kEmitterCount = 0x198;    // how many (= cooked numModifiers)
constexpr std::ptrdiff_t kEmitterNoList = 0xDC;    // simulation type; non-zero: no CPU list
// the cooked blob: emitterInfo at +0x40 (the setup reads modifierSetMask at +0x88)
constexpr std::ptrdiff_t kDescMask = 0x88;
// simulation context passed to every modifier: dt at +0x54
constexpr std::ptrdiff_t kContextDt = 0x54;

// ---- RTTI layout of the cooked emitter info --------------------------------------------------
struct Layout
{
    bool ok = false;
    std::ptrdiff_t mask = 0, wind = 0, local = 0;
};
Layout g_layout;
std::once_flag g_layoutOnce;

void ResolveLayout()
{
    auto rtti = RED4ext::CRTTISystem::Get();
    auto cls = rtti ? rtti->GetClass("rendRenderParticleBlobEmitterInfo") : nullptr;
    auto mask = cls ? cls->GetProperty("modifierSetMask") : nullptr;
    auto count = cls ? cls->GetProperty("numModifiers") : nullptr;
    auto inits = cls ? cls->GetProperty("initializerSetMask") : nullptr;
    auto wind = cls ? cls->GetProperty("windInfluence") : nullptr;
    auto local = cls ? cls->GetProperty("keepSimulationLocal") : nullptr;
    if (!mask || !count || !wind || !local)
    {
        g_sdk->logger->Warn(g_handle, "smoke wind: emitter info layout not found in RTTI; smoke wind is off");
        return;
    }
    // the setup reads numModifiers 8 bytes before the mask and initializerSetMask 8 after it;
    // if RTTI disagrees, this game version changed the struct and we stay out
    const bool matches = count->valueOffset + 8 == mask->valueOffset &&
                         (!inits || inits->valueOffset == mask->valueOffset + 8);
    g_sdk->logger->InfoF(g_handle, "smoke wind: emitter info offsets mask %u count %u inits %d wind %u local %u -> %s",
                         mask->valueOffset, count->valueOffset, inits ? static_cast<int>(inits->valueOffset) : -1,
                         wind->valueOffset, local->valueOffset, matches ? "ok" : "MISMATCH, off");
    if (!matches)
    {
        return;
    }
    g_layout.mask = mask->valueOffset;
    g_layout.wind = wind->valueOffset;
    g_layout.local = local->valueOffset;
    g_layout.ok = true;
}

// ---- per-emitter influence, read by the modifier on worker threads ---------------------------
// A direct-mapped table of (emitter pointer << 16 | influence in 1/1000), one atomic word per
// slot: a collision overwrites, and the loser falls back to the default influence.
constexpr std::size_t kSlots = 1u << 16;
std::atomic<std::uint64_t> g_table[kSlots];

std::size_t SlotOf(const void* aEmitter)
{
    const auto p = reinterpret_cast<std::uintptr_t>(aEmitter) >> 4;
    return static_cast<std::size_t>((p * 0x9E3779B97F4A7C15ull) >> 48) & (kSlots - 1);
}

void Remember(const void* aEmitter, float aInfluence)
{
    const auto q = static_cast<std::uint64_t>(std::clamp(aInfluence, 0.0f, 65.0f) * 1000.0f);
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(aEmitter)) << 16;
    g_table[SlotOf(aEmitter)].store(key | (q & 0xFFFF), std::memory_order_release);
}

float Recall(const void* aEmitter)
{
    const auto v = g_table[SlotOf(aEmitter)].load(std::memory_order_acquire);
    if ((v >> 16) == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(aEmitter)))
    {
        return static_cast<float>(v & 0xFFFF) / 1000.0f;
    }
    return g_settings.defaultInfluence.load(std::memory_order_relaxed);
}

// ---- our modifier ---------------------------------------------------------------------------
// Called like the engine's own: rcx particle, rdx emitter + 0x10, r8 sim context, xmm3 the
// particle's life fraction 0..1 (stack args unused). Particle: position +0x0, working velocity
// +0x14, base velocity +0x20. Runs last in the list, after the effect's own drag.
// Only CPU-simulated emitters reach here; one with a Collision module is simulated in PhysX
// and never runs its list, which is why the smoke archive strips that module from smoke.
void SmokeModifier(std::uint8_t* aParticle, std::uint8_t* aEmitter10, std::uint8_t* aContext, float aLife)
{
    if (!g_settings.enabled.load(std::memory_order_relaxed) || !CWF::Wind::IsEnabled() || !aParticle ||
        !aContext)
    {
        return;
    }
    const float dt = *reinterpret_cast<const float*>(aContext + kContextDt);
    if (!(dt > 0.0f && dt < 0.25f))
    {
        return;
    }
    const float influence = Recall(aEmitter10 - 0x10);
    // a fresh particle is still in its source's jet; the wind takes it over in its first 10%
    const float ramp = std::clamp(aLife / 0.1f, 0.0f, 1.0f);
    const float scale = influence * g_settings.gain.load(std::memory_order_relaxed);
    const auto w = CWF::Wind::Get();
    // Set the horizontal velocity outright once the particle has left its jet: wind x
    // influence x gain, blended in over the jet phase. The engine's drag damps the velocity
    // toward zero each frame, so easing toward the wind lost to it. Vertical motion stays the
    // effect's own. Both the working velocity (+0x14) and the base one (+0x20) are written, as
    // the engine's drag does: the loop restores +0x14 from +0x20 each frame. The engine adds its
    // own wind on top (position += wind x influence x dt, the context's +0x90 vector).
    auto* vel = reinterpret_cast<float*>(aParticle + 0x14);
    auto* keep = reinterpret_cast<float*>(aParticle + 0x20);
    const float own = 1.0f - ramp;
    vel[0] = w.x * scale * ramp + vel[0] * own;
    vel[1] = w.y * scale * ramp + vel[1] * own;
    keep[0] = vel[0];
    keep[1] = vel[1];

    g_stats.particleSteps.fetch_add(1, std::memory_order_relaxed);
    g_stats.lastDt.store(dt, std::memory_order_relaxed);
}

// ---- setup detour -----------------------------------------------------------------------------
std::atomic<std::uint32_t> g_hookedLogged{0};

void AppendIfWindy(std::uint8_t* aEmitter, const std::uint8_t* aDesc)
{
    std::call_once(g_layoutOnce, ResolveLayout);
    if (!g_layout.ok || !g_alloc || !g_free)
    {
        return;
    }
    const auto* info = aDesc + kDescMask - g_layout.mask;
    const float influence = *reinterpret_cast<const float*>(info + g_layout.wind);
    const bool local = *(info + g_layout.local) != 0;
    if (!(influence > 0.0f) || local || *reinterpret_cast<const std::uint32_t*>(aEmitter + kEmitterNoList) != 0)
    {
        return;
    }
    auto** oldList = *reinterpret_cast<void***>(aEmitter + kEmitterList);
    const auto count = *reinterpret_cast<const std::uint32_t*>(aEmitter + kEmitterCount);
    if (!oldList || count == 0 || count > 64)
    {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (!oldList[i])
        {
            return; // the builder left a hole; not a list we understand
        }
    }
    auto** newList = static_cast<void**>(g_alloc((count + 1) * sizeof(void*)));
    if (!newList)
    {
        return;
    }
    std::memcpy(newList, oldList, count * sizeof(void*));
    newList[count] = reinterpret_cast<void*>(&SmokeModifier);

    Remember(aEmitter, influence);
    *reinterpret_cast<void***>(aEmitter + kEmitterList) = newList;
    *reinterpret_cast<std::uint32_t*>(aEmitter + kEmitterCount) = count + 1;

    struct
    {
        void* ptr;
        std::uint64_t zero;
    } block{oldList, 0};
    g_free(&block);
    g_stats.emittersWindy.fetch_add(1, std::memory_order_relaxed);
    if (g_hookedLogged.fetch_add(1, std::memory_order_relaxed) < 60)
    {
        g_sdk->logger->InfoF(g_handle, "smoke wind: hooked emitter %p, influence %.2f, %u modifiers", aEmitter,
                             influence, count);
    }
}

void* Setup_Detour(void* a1, void* aDesc, void* a3, void* a4)
{
    void* emitter = g_setupOriginal(a1, aDesc, a3, a4);
    if (emitter && aDesc)
    {
        g_stats.emittersSeen.fetch_add(1, std::memory_order_relaxed);
        AppendIfWindy(static_cast<std::uint8_t*>(emitter), static_cast<const std::uint8_t*>(aDesc));
    }
    return emitter;
}
} // namespace

namespace CWF::SmokeWind
{
bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    g_handle = aHandle;
    g_sdk = aSdk;
    const auto setup = ResolveOrZero(Hashes::Particle_EmitterSetup);
    g_alloc = reinterpret_cast<Alloc_t>(ResolveOrZero(Hashes::Particle_ListAlloc));
    g_free = reinterpret_cast<Free_t>(ResolveOrZero(Hashes::Particle_ListFree));
    if (!setup || !g_alloc || !g_free)
    {
        aSdk->logger->Warn(aHandle, "particle emitter setup hashes did not resolve; smoke wind is off for this game version");
        return false;
    }
    g_setupTarget = reinterpret_cast<void*>(setup);
    if (!aSdk->hooking->Attach(aHandle, g_setupTarget, reinterpret_cast<void*>(&Setup_Detour),
                               reinterpret_cast<void**>(&g_setupOriginal)))
    {
        aSdk->logger->Warn(aHandle, "could not hook the particle emitter setup; smoke wind is off");
        g_setupTarget = nullptr;
        return false;
    }
    aSdk->logger->InfoF(aHandle, "smoke wind: emitter setup hooked at %p", g_setupTarget);
    return true;
}

void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    if (g_setupTarget)
    {
        aSdk->hooking->Detach(aHandle, g_setupTarget);
        g_setupTarget = nullptr;
    }
}

bool IsAttached()
{
    return g_setupTarget != nullptr;
}

Settings& GetSettings()
{
    return g_settings;
}

Stats& GetStats()
{
    return g_stats;
}

void LogSettingChange(bool aEnabled, float aGain)
{
    if (g_sdk)
    {
        g_sdk->logger->InfoF(g_handle, "smoke wind setting: enabled %d gain %.2f", aEnabled ? 1 : 0, aGain);
    }
}
} // namespace CWF::SmokeWind
