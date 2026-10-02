#include "SmokeWind.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "Addresses.hpp"
#include "SmokeTag.hpp"
#include "Wind.hpp"

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

NCW::SmokeWind::Settings g_settings;
NCW::SmokeWind::Stats g_stats;

// ---- engine pieces (2.31 disassembly; resolved by hash) ------------------------------------
// void* EmitterSetup(?, const Blob* desc, ?, ?): builds a runtime emitter from the cooked
// render blob and calls the modifier-list builder on it. Only rdx is read.
using Setup_t = void* (*)(void*, void*, void*, void*);
// the allocator the list builder uses: (bytes) -> zeroed block
using Alloc_t = void* (*)(std::uint32_t);
// the matching free, called with {pointer, 0}
using Free_t = void (*)(void*);
// the per-frame particle passes: r9 points at the runtime emitter pointer, the 5th argument is
// the owner whose +0x38 is the PhysX particle pool
using Pass_t = void* (*)(void*, void*, void*, void**, std::uint8_t*, void*, void*);

Setup_t g_setupOriginal = nullptr;
Alloc_t g_alloc = nullptr;
Free_t g_free = nullptr;
Pass_t g_simA = nullptr;
Pass_t g_simB = nullptr;
Pass_t g_spawn = nullptr;
void* g_volFunction = nullptr;
void* g_targets[4] = {};

// runtime emitter (layout from the 2026-10-02 dumps, docs/SMOKE_WIND_ANALYSIS.md 5f)
constexpr std::ptrdiff_t kEmitterRecords = 0x18;      // its copy of the cooked updater records
constexpr std::ptrdiff_t kEmitterModifOffset = 0x40;  // where the modifier records start in them
constexpr std::ptrdiff_t kEmitterMaxParticles = 0xB8;
constexpr std::ptrdiff_t kEmitterInfluence = 0xD8;    // windInfluence, read by the engine's advection
constexpr std::ptrdiff_t kEmitterNoList = 0xDC;       // simulation type; non-zero: no CPU list
constexpr std::ptrdiff_t kEmitterFlags = 0xE0;        // bit 1: keepSimulationLocal
constexpr std::ptrdiff_t kEmitterList = 0x190;        // modifier function pointers
constexpr std::ptrdiff_t kEmitterCount = 0x198;       // how many (= cooked numModifiers)
constexpr std::ptrdiff_t kEmitterModMask = 0x1A0;
constexpr std::ptrdiff_t kEmitterNumInit = 0x1B0;
constexpr std::ptrdiff_t kEmitterInitMask = 0x1B8;
constexpr std::ptrdiff_t kEmitterSimHash = 0x1D0;
// the cooked blob: emitterInfo at +0x40 (the setup reads modifierSetMask at +0x88)
constexpr std::ptrdiff_t kDescMask = 0x88;
// the owner of a per-frame pass: its PhysX particle pool
constexpr std::ptrdiff_t kOwnerPool = 0x38;
// simulation context passed to every modifier: dt at +0x54
constexpr std::ptrdiff_t kContextDt = 0x54;
constexpr std::uint32_t kMaxModifiers = 64;

template<typename T>
T& At(std::uint8_t* aBase, std::ptrdiff_t aOffset)
{
    return *reinterpret_cast<T*>(aBase + aOffset);
}

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

// ---- per-emitter tables, read by the modifier and the passes on worker threads ---------------
// Open-addressed, lock-free, keyed by the runtime emitter pointer. An entry is never removed;
// emitters are few (hundreds) and the tables are sized well beyond that.
constexpr std::size_t kSlots = 1u << 12;
// tagged smoke: emitter pointer | class in the low 4 bits (emitters are 16-byte aligned)
std::atomic<std::uintptr_t> g_smokeEmitters[kSlots];
std::atomic<std::uint64_t> g_influence[kSlots]; // emitter pointer << 16 | influence in 1/1000
// emitters the passes checked against the tag table without a match: pointer << 16 | tries.
// After kLateTries the check stops (the tag would have come within a few frames of setup).
std::atomic<std::uint64_t> g_lateTried[kSlots];
constexpr std::uint32_t kLateTries = 600;

std::size_t SlotOf(const void* aEmitter)
{
    const auto p = reinterpret_cast<std::uintptr_t>(aEmitter) >> 4;
    return static_cast<std::size_t>((p * 0x9E3779B97F4A7C15ull) >> 52) & (kSlots - 1);
}

void MarkSmoke(const void* aEmitter, NCW::SmokeTag::Class aClass)
{
    const auto p = reinterpret_cast<std::uintptr_t>(aEmitter) & ~std::uintptr_t(0xF);
    const auto v = p | (static_cast<std::uintptr_t>(aClass) & 0xF);
    auto i = SlotOf(aEmitter);
    for (std::size_t n = 0; n < 32; ++n, i = (i + 1) & (kSlots - 1))
    {
        std::uintptr_t expected = 0;
        const auto cur = g_smokeEmitters[i].load(std::memory_order_acquire);
        if ((cur & ~std::uintptr_t(0xF)) == p ||
            (cur == 0 && g_smokeEmitters[i].compare_exchange_strong(expected, v, std::memory_order_acq_rel)))
        {
            return;
        }
    }
}

// the class of a tagged smoke emitter, None for anything else
NCW::SmokeTag::Class SmokeClass(const void* aEmitter)
{
    const auto p = reinterpret_cast<std::uintptr_t>(aEmitter) & ~std::uintptr_t(0xF);
    auto i = SlotOf(aEmitter);
    for (std::size_t n = 0; n < 32; ++n, i = (i + 1) & (kSlots - 1))
    {
        const auto cur = g_smokeEmitters[i].load(std::memory_order_acquire);
        if (cur == 0)
        {
            return NCW::SmokeTag::Class::None;
        }
        if ((cur & ~std::uintptr_t(0xF)) == p)
        {
            return static_cast<NCW::SmokeTag::Class>(cur & 0xF);
        }
    }
    return NCW::SmokeTag::Class::None;
}

bool IsSmoke(const void* aEmitter)
{
    return SmokeClass(aEmitter) != NCW::SmokeTag::Class::None;
}

// true while this emitter is still worth checking against the tag table
bool LateTry(const void* aEmitter)
{
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(aEmitter)) << 16;
    auto& slot = g_lateTried[SlotOf(aEmitter)];
    auto v = slot.load(std::memory_order_relaxed);
    if ((v & ~std::uint64_t(0xFFFF)) != key)
    {
        slot.store(key | 1, std::memory_order_relaxed); // another emitter had the slot: start over
        return true;
    }
    const auto tries = static_cast<std::uint32_t>(v & 0xFFFF);
    if (tries >= kLateTries)
    {
        return false;
    }
    slot.store(key | (tries + 1), std::memory_order_relaxed);
    return true;
}

// ---- overpass rays ---------------------------------------------------------------------------
// Tagged smoke runs on the CPU path, where nothing collides, so a column would rise through an
// overpass. The modifier samples each puff once per quarter second of its life into a ring; the
// script tick drains it (NCW_SmokeRaySamples), casts a short ray along each puff's motion on
// the main thread, and reports the hits (NCW_SmokeRayHit). A hit goes into a second ring keyed
// by the particle; the modifier kills the puff on its next step, from the particle thread, so no
// particle memory is touched from outside the simulation.
struct RaySample
{
    std::atomic<std::uint32_t> ready{0};
    std::uintptr_t particle = 0;
    std::uint32_t seed = 0;
    float pos[3] = {};
    float vel[3] = {};
};
constexpr std::size_t kRaySamples = 256;
RaySample g_raySamples[kRaySamples];
std::atomic<std::uint32_t> g_rayCount{0};
// kill requests: particle pointer | seed-derived low bits, looked up per particle step
constexpr std::size_t kKillSlots = 256;
std::atomic<std::uint64_t> g_kills[kKillSlots]; // particle pointer ^ (seed << 48)
std::atomic<std::uint32_t> g_raysOn{1};

std::size_t KillSlot(std::uintptr_t aParticle)
{
    return static_cast<std::size_t>((aParticle >> 4) * 0x9E3779B97F4A7C15ull >> 56) & (kKillSlots - 1);
}

std::uint64_t KillKey(std::uintptr_t aParticle, std::uint32_t aSeed)
{
    return static_cast<std::uint64_t>(aParticle) ^ (static_cast<std::uint64_t>(aSeed & 0xFFFF) << 48);
}

void SampleForRay(std::uint8_t* aParticle, float aDt)
{
    const float age = *reinterpret_cast<const float*>(aParticle + 0xC);
    // once per 0.25 s of life, after the first quarter second (the jet is the source's business)
    const float phase = std::fmod(age, 0.25f);
    if (age < 0.25f || phase >= aDt)
    {
        return;
    }
    const auto i = g_rayCount.fetch_add(1, std::memory_order_relaxed);
    if (i >= kRaySamples)
    {
        return;
    }
    auto& s = g_raySamples[i];
    s.particle = reinterpret_cast<std::uintptr_t>(aParticle);
    s.seed = *reinterpret_cast<const std::uint32_t*>(aParticle + 0x94);
    std::memcpy(s.pos, aParticle, sizeof(s.pos));
    std::memcpy(s.vel, aParticle + 0x14, sizeof(s.vel));
    s.ready.store(1, std::memory_order_release);
}

// true (and the request consumed) when this puff was reported as hitting something
bool KillRequested(std::uint8_t* aParticle)
{
    auto& slot = g_kills[KillSlot(reinterpret_cast<std::uintptr_t>(aParticle))];
    const auto v = slot.load(std::memory_order_acquire);
    if (v == 0)
    {
        return false;
    }
    const auto seed = *reinterpret_cast<const std::uint32_t*>(aParticle + 0x94);
    if (v != KillKey(reinterpret_cast<std::uintptr_t>(aParticle), seed))
    {
        return false;
    }
    slot.store(0, std::memory_order_release);
    return true;
}

void Remember(const void* aEmitter, float aInfluence)
{
    const auto q = static_cast<std::uint64_t>(std::clamp(aInfluence, 0.0f, 65.0f) * 1000.0f);
    const auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(aEmitter)) << 16;
    g_influence[SlotOf(aEmitter)].store(key | (q & 0xFFFF), std::memory_order_release);
}

float Recall(const void* aEmitter)
{
    const auto v = g_influence[SlotOf(aEmitter)].load(std::memory_order_acquire);
    if ((v >> 16) == static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(aEmitter)))
    {
        return static_cast<float>(v & 0xFFFF) / 1000.0f;
    }
    return g_settings.defaultInfluence.load(std::memory_order_relaxed);
}

// ---- our modifier ---------------------------------------------------------------------------
// Called like the engine's own: rcx particle, rdx emitter + 0x10, r8 sim context, xmm3 the
// particle's life fraction 0..1 (stack args unused). Particle: position +0x0, working velocity
// +0x14, base velocity +0x20. Runs last in the list.
void SmokeModifier(std::uint8_t* aParticle, std::uint8_t* aEmitter10, std::uint8_t* aContext, float aLife)
{
    if (!NCW::Wind::IsEnabled() || !aParticle || !aContext)
    {
        return;
    }
    const float dt = At<const float>(aContext, kContextDt);
    if (!(dt > 0.0f && dt < 0.25f))
    {
        return;
    }
    auto* emitter = aEmitter10 - 0x10;
    auto* vel = reinterpret_cast<float*>(aParticle + 0x14);
    auto* keep = reinterpret_cast<float*>(aParticle + 0x20);
    // Tagged smoke: bleed off the puff's own sideways motion, quadratically like the engine's
    // drag (the coefficient by class), on the base velocity too. Many columns are authored to
    // lean one fixed way; with that damped, the engine's wind advection (position += wind x
    // influence x dt, every frame) sets the drift. Vertical motion stays the effect's own.
    const auto cls = SmokeClass(emitter);
    if (cls != NCW::SmokeTag::Class::None)
    {
        const float h = std::sqrt(keep[0] * keep[0] + keep[1] * keep[1]);
        const float f = 1.0f / (1.0f + NCW::SmokeTag::DragFor(cls) * dt * h);
        keep[0] *= f;
        keep[1] *= f;
        vel[0] *= f;
        vel[1] *= f;
        if (g_raysOn.load(std::memory_order_relaxed))
        {
            if (KillRequested(aParticle))
            {
                // age past its lifetime: the pass retires it (what killOnCollision did)
                const float invLife = *reinterpret_cast<const float*>(aParticle + 0x10);
                if (invLife > 0.0f)
                {
                    *reinterpret_cast<float*>(aParticle + 0xC) = 1.0f / invLife + 1.0f;
                }
                g_stats.puffsKilled.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            SampleForRay(aParticle, dt);
        }
    }
    if (!g_settings.enabled.load(std::memory_order_relaxed))
    {
        return;
    }
    // The optional push (off by default: the game's own advection looks better): once a puff has
    // left its source's jet (its first 10% of life), its horizontal velocity is set outright to
    // wind x influence x gain, on the base velocity too.
    const float ramp = std::clamp(aLife / 0.1f, 0.0f, 1.0f);
    const float scale = Recall(emitter) * g_settings.gain.load(std::memory_order_relaxed);
    const auto w = NCW::Wind::Get();
    const float own = 1.0f - ramp;
    vel[0] = w.x * scale * ramp + vel[0] * own;
    vel[1] = w.y * scale * ramp + vel[1] * own;
    keep[0] = vel[0];
    keep[1] = vel[1];
    g_stats.particleSteps.fetch_add(1, std::memory_order_relaxed);
}

// ---- what a tagged smoke emitter gets --------------------------------------------------------
// VelocityOverLife off. The CPU passes commit a particle's working velocity back to its base
// velocity each frame, and VelocityOverLife multiplies the working velocity by its curve (3 at
// birth for a car column), so the two compounded: a puff left the car at 200 m/s sideways (the
// 2026-10-02 log: base X 1.1 at 0.03 s, 8.8 at 0.07 s, 87 at 0.15 s). The curve's slot in the
// modifier list gets a stub that only skips the curve's cooked record, so the cursor stays
// aligned for the modifiers after it.
void VolStub(std::uint8_t*, std::uint8_t* aEmitter10, std::uint8_t*, float, std::uint32_t, std::uint32_t, void*,
             std::uint32_t* aCursor, std::uint32_t)
{
    const auto* data = aEmitter10 ? *reinterpret_cast<std::uint8_t* const*>(aEmitter10 + 8) : nullptr;
    if (aCursor && data)
    {
        *aCursor += *reinterpret_cast<const std::uint16_t*>(data + *aCursor + 2);
    }
}

void NeutralizeVol(std::uint8_t* aEmitter)
{
    auto** list = At<void**>(aEmitter, kEmitterList);
    const auto count = At<const std::uint32_t>(aEmitter, kEmitterCount);
    if (!g_volFunction || !list || count == 0 || count > kMaxModifiers)
    {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (list[i] == g_volFunction)
        {
            list[i] = reinterpret_cast<void*>(&VolStub);
            g_stats.curvesStubbed.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Birth point lowered. A column born at a fixed point high above its source keeps restarting
// there however the wind carries it (Exploded Vehicles' car column is born 10 m up). The runtime
// emitter's copy of the cooked initializer records is patched: the position initializer is the
// first 28-byte constant record 02 00 1C 00 + (x y z, x y z) with x = y = 0 and z above the cap
// (velocity records of the same shape have x or y set). New puffs use it from then on.
constexpr float kSpawnCap = 2.5f;

void LowerSpawn(std::uint8_t* aEmitter)
{
    auto* data = At<std::uint8_t*>(aEmitter, kEmitterRecords);
    const auto len = At<const std::uint32_t>(aEmitter, kEmitterModifOffset);
    if (!data || len < 32 || len > 4096)
    {
        return;
    }
    for (std::uint32_t i = 0; i + 32 <= len; ++i)
    {
        if (data[i] != 0x02 || data[i + 1] != 0x00 || data[i + 2] != 0x1C || data[i + 3] != 0x00)
        {
            continue;
        }
        float v[6];
        std::memcpy(v, data + i + 4, sizeof(v));
        if (v[0] == 0.0f && v[1] == 0.0f && v[3] == 0.0f && v[4] == 0.0f && v[2] == v[5] && v[2] > kSpawnCap &&
            v[2] < 100.0f)
        {
            v[2] = v[5] = kSpawnCap;
            std::memcpy(data + i + 4, v, sizeof(v));
            g_stats.spawnsLowered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

// Our modifier appended to the emitter's list, once. Safe at setup and from the pass detours,
// which run before the pass reads the list.
void AppendModifier(std::uint8_t* aEmitter, float aInfluence)
{
    auto** oldList = At<void**>(aEmitter, kEmitterList);
    const auto count = At<const std::uint32_t>(aEmitter, kEmitterCount);
    if (!oldList || count == 0 || count > kMaxModifiers || oldList[count - 1] == reinterpret_cast<void*>(&SmokeModifier))
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

    Remember(aEmitter, aInfluence);
    At<void**>(aEmitter, kEmitterList) = newList;
    At<std::uint32_t>(aEmitter, kEmitterCount) = count + 1;

    struct
    {
        void* ptr;
        std::uint64_t zero;
    } block{oldList, 0};
    g_free(&block);
    g_stats.emittersWindy.fetch_add(1, std::memory_order_relaxed);
}

// Everything a tagged smoke emitter needs: the influence floor the engine's own advection
// multiplies the wind by, the curve stub, the lowered birth point, and our modifier.
void TagEmitter(std::uint8_t* aEmitter, float aFloor, NCW::SmokeTag::Class aClass)
{
    MarkSmoke(aEmitter, aClass);
    NeutralizeVol(aEmitter);
    LowerSpawn(aEmitter);
    auto& influence = At<float>(aEmitter, kEmitterInfluence);
    if (influence < aFloor)
    {
        influence = aFloor;
    }
    const bool local = (At<const std::uint8_t>(aEmitter, kEmitterFlags) & 2) != 0;
    if (!local && At<const std::uint32_t>(aEmitter, kEmitterNoList) == 0)
    {
        AppendModifier(aEmitter, influence);
    }
}

// ---- the emitter setup hook --------------------------------------------------------------------
void OnEmitterSetup(std::uint8_t* aEmitter, const std::uint8_t* aDesc)
{
    std::call_once(g_layoutOnce, ResolveLayout);
    if (!g_layout.ok || !g_alloc || !g_free)
    {
        return;
    }
    const auto* info = aDesc + kDescMask - g_layout.mask;
    const float influence = *reinterpret_cast<const float*>(info + g_layout.wind);
    const bool local = *(info + g_layout.local) != 0;
    NCW::SmokeTag::Class cls;
    const float floor = NCW::SmokeTag::FloorFor(aDesc, cls);
    if (floor > 0.0f)
    {
        TagEmitter(aEmitter, floor, cls);
        return;
    }
    // an emitter with its own wind influence gets the (optional) push
    if (influence > 0.0f && !local && At<const std::uint32_t>(aEmitter, kEmitterNoList) == 0)
    {
        AppendModifier(aEmitter, influence);
    }
}

void* Setup_Detour(void* a1, void* aDesc, void* a3, void* a4)
{
    void* emitter = g_setupOriginal(a1, aDesc, a3, a4);
    if (emitter && aDesc)
    {
        g_stats.emittersSeen.fetch_add(1, std::memory_order_relaxed);
        OnEmitterSetup(static_cast<std::uint8_t*>(emitter), static_cast<const std::uint8_t*>(aDesc));
    }
    return emitter;
}

// ---- the per-frame passes -----------------------------------------------------------------------
// An effect's render side can set its emitters up a few ms before the load callback tags the
// effect (the car plume: tagged at .514, set up just before), so every pass also checks an
// unmarked emitter against the tag table through the runtime emitter's own copies of the
// cooked fields. Then, for tagged smoke, the owner's PhysX pool pointer is cleared: an emitter
// with a Collision module is simulated in PhysX, the passes take its positions and velocities
// from that pool and never run the modifier list, so neither the engine's wind nor ours reaches
// it. Without the pool the pass takes the plain CPU path (no gravity, no collision deaths; the
// smoke archive made the same trade). The pool itself is leaked, which is harmless; a stand-in
// owner without the pool was tried instead and left the plume invisible, since the engine's
// PhysX write-back still found the real pool.
void EnsureSmoke(std::uint8_t* aEmitter)
{
    if (!aEmitter || IsSmoke(aEmitter) || !LateTry(aEmitter))
    {
        return;
    }
    // the cooked modifier count: one less than the list when our modifier is already on it
    std::uint32_t numMod = At<const std::uint32_t>(aEmitter, kEmitterCount);
    auto** list = At<void**>(aEmitter, kEmitterList);
    if (list && numMod > 0 && list[numMod - 1] == reinterpret_cast<void*>(&SmokeModifier))
    {
        --numMod;
    }
    NCW::SmokeTag::Class cls;
    const float floor = NCW::SmokeTag::FloorForFields(
        At<const std::uint64_t>(aEmitter, kEmitterModMask), At<const std::uint64_t>(aEmitter, kEmitterInitMask), numMod,
        At<const std::uint32_t>(aEmitter, kEmitterNumInit), At<const std::uint32_t>(aEmitter, kEmitterMaxParticles),
        At<const std::uint64_t>(aEmitter, kEmitterSimHash), cls);
    if (floor > 0.0f && g_alloc && g_free)
    {
        TagEmitter(aEmitter, floor, cls);
        g_stats.lateMatches.fetch_add(1, std::memory_order_relaxed);
    }
}

void BeforePass(void** aEmitterRef, std::uint8_t* aOwner)
{
    if (!aEmitterRef)
    {
        return;
    }
    auto* emitter = static_cast<std::uint8_t*>(*aEmitterRef);
    EnsureSmoke(emitter);
    if (aOwner && IsSmoke(emitter))
    {
        auto& pool = At<void*>(aOwner, kOwnerPool);
        if (pool)
        {
            pool = nullptr;
            g_stats.poolsCleared.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void* SimA_Detour(void* a1, void* a2, void* a3, void** a4, std::uint8_t* aOwner, void* a6, void* a7)
{
    BeforePass(a4, aOwner);
    return g_simA(a1, a2, a3, a4, aOwner, a6, a7);
}

void* SimB_Detour(void* a1, void* a2, void* a3, void** a4, std::uint8_t* aOwner, void* a6, void* a7)
{
    BeforePass(a4, aOwner);
    return g_simB(a1, a2, a3, a4, aOwner, a6, a7);
}

// the spawn pass too: left with its pool, PhysX particles piled up with nothing draining them
// and the game froze right after a car blew up (2026-10-02)
void* Spawn_Detour(void* a1, void* a2, void* a3, void** a4, std::uint8_t* aOwner, void* a6, void* a7)
{
    BeforePass(a4, aOwner);
    return g_spawn(a1, a2, a3, a4, aOwner, a6, a7);
}

bool Hook(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk, std::uint32_t aHash, void* aDetour,
          void** aOriginal, void** aTarget, const char* aName)
{
    const auto address = NCW::ResolveOrZero(aHash);
    if (!address)
    {
        aSdk->logger->WarnF(aHandle, "smoke wind: the %s hash did not resolve for this game version", aName);
        return false;
    }
    *aTarget = reinterpret_cast<void*>(address);
    if (!aSdk->hooking->Attach(aHandle, *aTarget, aDetour, aOriginal))
    {
        aSdk->logger->WarnF(aHandle, "smoke wind: could not hook %s", aName);
        *aTarget = nullptr;
        return false;
    }
    return true;
}
} // namespace

namespace NCW::SmokeWind
{
bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    g_handle = aHandle;
    g_sdk = aSdk;
    g_alloc = reinterpret_cast<Alloc_t>(ResolveOrZero(Hashes::Particle_ListAlloc));
    g_free = reinterpret_cast<Free_t>(ResolveOrZero(Hashes::Particle_ListFree));
    g_volFunction = reinterpret_cast<void*>(ResolveOrZero(Hashes::Particle_VelocityOverLife));
    if (!g_alloc || !g_free || !g_volFunction)
    {
        aSdk->logger->Warn(aHandle, "smoke wind: particle hashes did not resolve for this game version; smoke wind is off");
        return false;
    }
    if (!Hook(aHandle, aSdk, Hashes::Particle_EmitterSetup, reinterpret_cast<void*>(&Setup_Detour),
              reinterpret_cast<void**>(&g_setupOriginal), &g_targets[0], "the particle emitter setup"))
    {
        return false;
    }
    // the passes are needed only for smoke that would otherwise be simulated in PhysX
    Hook(aHandle, aSdk, Hashes::Particle_SimA, reinterpret_cast<void*>(&SimA_Detour), reinterpret_cast<void**>(&g_simA),
         &g_targets[1], "particle pass A");
    Hook(aHandle, aSdk, Hashes::Particle_SimB, reinterpret_cast<void*>(&SimB_Detour), reinterpret_cast<void**>(&g_simB),
         &g_targets[2], "particle pass B");
    Hook(aHandle, aSdk, Hashes::Particle_Spawn, reinterpret_cast<void*>(&Spawn_Detour), reinterpret_cast<void**>(&g_spawn),
         &g_targets[3], "the particle spawn pass");
    aSdk->logger->InfoF(aHandle, "smoke wind: emitter setup hooked at %p", g_targets[0]);
    return true;
}

void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    for (auto& target : g_targets)
    {
        if (target)
        {
            aSdk->hooking->Detach(aHandle, target);
            target = nullptr;
        }
    }
}

bool IsAttached()
{
    return g_targets[0] != nullptr;
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

void SetRays(bool aOn)
{
    g_raysOn.store(aOn ? 1u : 0u, std::memory_order_relaxed);
}

std::uint32_t DrainRaySamples(RaySampleOut* aOut, std::uint32_t aMax)
{
    const auto n = std::min<std::uint32_t>(g_rayCount.load(std::memory_order_acquire), kRaySamples);
    std::uint32_t out = 0;
    for (std::uint32_t i = 0; i < n && out < aMax; ++i)
    {
        auto& s = g_raySamples[i];
        if (!s.ready.load(std::memory_order_acquire))
        {
            continue;
        }
        aOut[out].key = KillKey(s.particle, s.seed);
        aOut[out].slot = static_cast<std::uint32_t>(KillSlot(s.particle));
        std::memcpy(aOut[out].pos, s.pos, sizeof(s.pos));
        std::memcpy(aOut[out].vel, s.vel, sizeof(s.vel));
        ++out;
    }
    for (std::uint32_t i = 0; i < n; ++i)
    {
        g_raySamples[i].ready.store(0, std::memory_order_relaxed);
    }
    g_rayCount.store(0, std::memory_order_release);
    g_stats.raysCast.fetch_add(out, std::memory_order_relaxed);
    return out;
}

void RequestKill(std::uint64_t aKey, std::uint32_t aSlot)
{
    if (aSlot < kKillSlots)
    {
        g_kills[aSlot].store(aKey, std::memory_order_release);
    }
}
} // namespace NCW::SmokeWind
