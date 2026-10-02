#include "SmokeTag.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
NCW::SmokeTag::Stats g_stats;
std::atomic<float> g_floor{0.2f}; // the game's own top value for body smoke (survey.py)
std::atomic<std::uint32_t> g_logged{0};

// ---- the same rules as smoke/build_smoke.py --------------------------------------------------
const char* const kPaths[] = {
    "\\fx\\environment\\smoke", "\\fx\\environment\\pyro",   "\\fx\\environment\\dust",  "\\fx\\environment\\steam",
    "\\fx\\vehicles\\_damage",   "\\fx\\vehicles\\_exhaust", "\\fx\\vehicles\\car",      "\\fx\\vehicles\\bike",
    "\\fx\\_library\\fire",      "\\fx\\_library\\smoke",    "\\fx\\_library\\explosion", "\\fx\\weapons\\explosives",
    "\\fx\\weapons\\throwables", "\\fx\\weapons\\grenades"};
const char* const kSmoke[] = {"smoke", "steam", "dust", "fume", "vapo", "cloud", "haze", "mist", "fog", "soot", "ash"};
const char* const kSkip[] = {"fire",  "flame", "spark", "ember", "glow",  "light", "debris",
                             "chunk", "flash", "blast", "shock", "heat",  "distort"};

std::string Lower(const char* aText)
{
    std::string s = aText ? aText : "";
    for (auto& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool ContainsAny(const std::string& aText, const char* const* aWords, std::size_t aCount)
{
    for (std::size_t i = 0; i < aCount; ++i)
    {
        if (aText.find(aWords[i]) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

// ---- RTTI offsets, resolved once ------------------------------------------------------------
struct Layout
{
    bool ok = false;
    RED4ext::CProperty* sysEmitters = nullptr;
    RED4ext::CProperty* emName = nullptr;
    bool emNameIsString = false;
    RED4ext::CProperty* emWind = nullptr;
    RED4ext::CProperty* emBlob = nullptr;
    RED4ext::CProperty* emModules = nullptr;
    // inside rendRenderParticleBlob
    std::ptrdiff_t info = -1, infoWind = -1, infoModMask = -1, infoInitMask = -1, infoNumMod = -1, infoNumInit = -1,
                   infoMaxParticles = -1, infoSimHash = -1, data = -1, modifOffset = -1;
};
Layout g_layout;
std::once_flag g_layoutOnce;

RED4ext::CClass* ClassOf(RED4ext::CProperty* aProp)
{
    if (!aProp || !aProp->type || aProp->type->GetType() != RED4ext::ERTTIType::Class)
    {
        return nullptr;
    }
    return static_cast<RED4ext::CClass*>(aProp->type);
}

void ResolveLayout()
{
    auto rtti = RED4ext::CRTTISystem::Get();
    auto sys = rtti->GetClass("CParticleSystem");
    auto em = rtti->GetClass("CParticleEmitter");
    auto blob = rtti->GetClass("rendRenderParticleBlob");
    g_layout.sysEmitters = sys ? sys->GetProperty("emitters") : nullptr;
    g_layout.emName = em ? em->GetProperty("editorName") : nullptr;
    g_layout.emNameIsString = g_layout.emName && g_layout.emName->type &&
                              !std::strcmp(g_layout.emName->type->GetName().ToString(), "String");
    g_layout.emWind = em ? em->GetProperty("windInfluence") : nullptr;
    g_layout.emBlob = em ? em->GetProperty("renderResourceBlob") : nullptr;
    g_layout.emModules = em ? em->GetProperty("modules") : nullptr;
    auto header = blob ? blob->GetProperty("header") : nullptr;
    auto headerCls = ClassOf(header);
    auto info = headerCls ? headerCls->GetProperty("emitterInfo") : nullptr;
    auto infoCls = ClassOf(info);
    auto updater = blob ? blob->GetProperty("updaterData") : nullptr;
    auto updaterCls = ClassOf(updater);
    auto data = updaterCls ? updaterCls->GetProperty("data") : nullptr;
    auto modif = updaterCls ? updaterCls->GetProperty("modifOffset") : nullptr;
    auto field = [&](const char* aName) -> std::ptrdiff_t
    {
        auto p = infoCls ? infoCls->GetProperty(aName) : nullptr;
        return p ? static_cast<std::ptrdiff_t>(header->valueOffset + info->valueOffset + p->valueOffset) : -1;
    };
    if (!g_layout.sysEmitters || !g_layout.emName || !g_layout.emWind || !g_layout.emBlob || !g_layout.emModules ||
        !header || !info || !infoCls || !updater || !data)
    {
        g_sdk->logger->Warn(g_handle, "smoke tag: particle RTTI layout not found; smoke tagging is off");
        return;
    }
    g_layout.info = header->valueOffset + info->valueOffset;
    g_layout.infoWind = field("windInfluence");
    g_layout.infoModMask = field("modifierSetMask");
    g_layout.infoInitMask = field("initializerSetMask");
    g_layout.infoNumMod = field("numModifiers");
    g_layout.infoNumInit = field("numInitializers");
    g_layout.infoMaxParticles = field("maxParticles");
    g_layout.infoSimHash = field("simulationHash");
    g_layout.data = updater->valueOffset + data->valueOffset;
    g_layout.modifOffset = modif ? static_cast<std::ptrdiff_t>(updater->valueOffset + modif->valueOffset) : -1;
    g_layout.ok = g_layout.infoWind >= 0 && g_layout.infoModMask >= 0 && g_layout.infoNumMod >= 0 &&
                  g_layout.infoMaxParticles >= 0;
    g_sdk->logger->InfoF(g_handle,
                         "smoke tag: editorName is %s; blob offsets info %d wind %d masks %d/%d counts %d/%d max %d simhash %d data %d modif %d -> %s",
                         g_layout.emName && g_layout.emName->type ? g_layout.emName->type->GetName().ToString() : "?",
                         static_cast<int>(g_layout.info), static_cast<int>(g_layout.infoWind),
                         static_cast<int>(g_layout.infoModMask), static_cast<int>(g_layout.infoInitMask),
                         static_cast<int>(g_layout.infoNumMod), static_cast<int>(g_layout.infoNumInit),
                         static_cast<int>(g_layout.infoMaxParticles), static_cast<int>(g_layout.infoSimHash),
                         static_cast<int>(g_layout.data), static_cast<int>(g_layout.modifOffset),
                         g_layout.ok ? "ok" : "incomplete, off");
}

// ---- fingerprints -------------------------------------------------------------------------
std::uint64_t Fnv(std::uint64_t aHash, const void* aData, std::size_t aLen)
{
    const auto* p = static_cast<const std::uint8_t*>(aData);
    for (std::size_t i = 0; i < aLen; ++i)
    {
        aHash = (aHash ^ p[i]) * 0x100000001B3ull;
    }
    return aHash;
}

template<typename T>
T Read(const std::uint8_t* aBase, std::ptrdiff_t aOffset)
{
    T v{};
    if (aOffset >= 0)
    {
        std::memcpy(&v, aBase + aOffset, sizeof(T));
    }
    return v;
}

// the cooked emitter without its wind influence: masks, counts, max particles, simulation hash
std::uint64_t WeakFromFields(std::uint64_t mm, std::uint64_t im, std::uint32_t nm, std::uint32_t ni, std::uint32_t mp,
                             std::uint64_t sh)
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    h = Fnv(h, &mm, sizeof(mm));
    h = Fnv(h, &im, sizeof(im));
    h = Fnv(h, &nm, sizeof(nm));
    h = Fnv(h, &ni, sizeof(ni));
    h = Fnv(h, &mp, sizeof(mp));
    h = Fnv(h, &sh, sizeof(sh));
    return h | 1; // never 0
}

std::uint64_t WeakFingerprint(const std::uint8_t* aBlob)
{
    return WeakFromFields(Read<std::uint64_t>(aBlob, g_layout.infoModMask), Read<std::uint64_t>(aBlob, g_layout.infoInitMask),
                          Read<std::uint32_t>(aBlob, g_layout.infoNumMod), Read<std::uint32_t>(aBlob, g_layout.infoNumInit),
                          Read<std::uint32_t>(aBlob, g_layout.infoMaxParticles), Read<std::uint64_t>(aBlob, g_layout.infoSimHash));
}

// the weak one plus the cooked updater records (the initializer and modifier parameters)
std::uint64_t StrongFingerprint(const std::uint8_t* aBlob)
{
    std::uint64_t h = WeakFingerprint(aBlob);
    const auto* buffer = reinterpret_cast<const RED4ext::RawBuffer*>(aBlob + g_layout.data);
    if (buffer->data && buffer->size > 0 && buffer->size < (1u << 20))
    {
        h = Fnv(h, buffer->data, buffer->size);
    }
    const std::uint32_t mo = Read<std::uint32_t>(aBlob, g_layout.modifOffset);
    h = Fnv(h, &mo, sizeof(mo));
    return (h | 1) ^ 0x8000000000000000ull;
}

// ---- the table: fingerprint -> influence floor (in 1/1000), lock-free -------------------------
constexpr std::size_t kSlots = 1u << 12;
struct Slot
{
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint32_t> value{0};
};
Slot g_table[kSlots];

void Store(std::uint64_t aKey, float aFloor)
{
    const auto v = static_cast<std::uint32_t>(std::clamp(aFloor, 0.0f, 10.0f) * 1000.0f);
    auto start = static_cast<std::size_t>((aKey * 0x9E3779B97F4A7C15ull) >> 52);
    for (std::size_t i = 0; i < 16; ++i)
    {
        auto& s = g_table[(start + i) & (kSlots - 1)];
        std::uint64_t expected = 0;
        const auto k = s.key.load(std::memory_order_acquire);
        if (k == aKey || (k == 0 && s.key.compare_exchange_strong(expected, aKey, std::memory_order_acq_rel)))
        {
            // keep the larger floor if two emitters share a fingerprint
            std::uint32_t cur = s.value.load(std::memory_order_relaxed);
            while (cur < v && !s.value.compare_exchange_weak(cur, v, std::memory_order_release))
            {
            }
            return;
        }
    }
}

float Load(std::uint64_t aKey)
{
    auto start = static_cast<std::size_t>((aKey * 0x9E3779B97F4A7C15ull) >> 52);
    for (std::size_t i = 0; i < 16; ++i)
    {
        auto& s = g_table[(start + i) & (kSlots - 1)];
        const auto k = s.key.load(std::memory_order_acquire);
        if (k == 0)
        {
            return 0.0f;
        }
        if (k == aKey)
        {
            return static_cast<float>(s.value.load(std::memory_order_acquire)) / 1000.0f;
        }
    }
    return 0.0f;
}

template<typename T>
T* Field(void* aInstance, RED4ext::CProperty* aProp)
{
    return (aInstance && aProp) ? reinterpret_cast<T*>(static_cast<char*>(aInstance) + aProp->valueOffset) : nullptr;
}
} // namespace

namespace NCW::SmokeTag
{
void Init(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk)
{
    g_handle = aHandle;
    g_sdk = aSdk;
}

void TagSystem(RED4ext::ISerializable* aSystem, const char* aPath)
{
    g_stats.systemsSeen.fetch_add(1, std::memory_order_relaxed);
    std::call_once(g_layoutOnce, ResolveLayout);
    if (!g_layout.ok || !aSystem)
    {
        return;
    }
    std::string path = Lower(aPath);
    std::replace(path.begin(), path.end(), '/', '\\');
    const bool wanted = path.find(".particle") != std::string::npos && ContainsAny(path, kPaths, std::size(kPaths));
    auto emitters = Field<RED4ext::DynArray<RED4ext::Handle<RED4ext::ISerializable>>>(aSystem, g_layout.sysEmitters);
    if (!wanted || !emitters)
    {
        return;
    }
    const float floor = g_floor.load(std::memory_order_relaxed);
    std::uint32_t tagged = 0, collisions = 0;
    for (std::uint32_t i = 0; i < emitters->Size(); ++i)
    {
        auto* emitter = (*emitters)[i].instance;
        if (!emitter)
        {
            continue;
        }
        // editorName is a String on CParticleEmitter (a CName elsewhere): read it by its type
        std::string editorName;
        if (g_layout.emNameIsString)
        {
            auto* s = Field<RED4ext::CString>(emitter, g_layout.emName);
            editorName = Lower(s ? s->c_str() : "");
        }
        else
        {
            auto* n = Field<RED4ext::CName>(emitter, g_layout.emName);
            editorName = Lower(n ? n->ToString() : "");
        }
        if (!ContainsAny(editorName, kSmoke, std::size(kSmoke)) || ContainsAny(editorName, kSkip, std::size(kSkip)))
        {
            continue;
        }
        auto* blobHandle = Field<RED4ext::Handle<RED4ext::ISerializable>>(emitter, g_layout.emBlob);
        auto* blob = reinterpret_cast<std::uint8_t*>(blobHandle ? blobHandle->instance : nullptr);
        if (!blob)
        {
            continue;
        }
        ++tagged;
        // remember the cooked emitter, with and without its record bytes
        Store(StrongFingerprint(blob), floor);
        Store(WeakFingerprint(blob), floor);
        // the script-visible values too, in case a render copy is made after this point
        auto* wind = Field<float>(emitter, g_layout.emWind);
        if (wind && *wind < floor)
        {
            *wind = floor;
        }
        auto* blobWind = reinterpret_cast<float*>(blob + g_layout.infoWind);
        if (*blobWind < floor)
        {
            *blobWind = floor;
        }
        // Emitters with a Collision module are counted, not changed: the game decides on PhysX
        // for the emitter before this callback and reads the module's settings after it.
        // Removing the module dropped the column like a crate (gravity), disabling it killed
        // the puffs at once (2026-10-02). The plugin keeps smoke out of PhysX at the particle
        // passes instead (SmokeWind.cpp).
        auto modules = Field<RED4ext::DynArray<RED4ext::Handle<RED4ext::ISerializable>>>(emitter, g_layout.emModules);
        for (std::uint32_t m = 0; modules && m < modules->Size(); ++m)
        {
            auto* module = (*modules)[m].instance;
            auto cls = module ? module->GetType() : nullptr;
            if (cls && !std::strcmp(cls->GetName().ToString(), "CParticleModificatorCollision"))
            {
                ++collisions;
            }
        }
    }
    if (tagged)
    {
        g_stats.systemsTagged.fetch_add(1, std::memory_order_relaxed);
        g_stats.emittersTagged.fetch_add(tagged, std::memory_order_relaxed);
        g_stats.collisions.fetch_add(collisions, std::memory_order_relaxed);
        if (g_logged.fetch_add(1, std::memory_order_relaxed) < 100)
        {
            g_sdk->logger->InfoF(g_handle, "smoke tag: %s: %u smoke emitters, %u with a collision module", aPath, tagged,
                                 collisions);
        }
    }
}

float FloorFor(const std::uint8_t* aBlob)
{
    if (!g_layout.ok || !aBlob)
    {
        return 0.0f;
    }
    float f = Load(StrongFingerprint(aBlob));
    if (f > 0.0f)
    {
        g_stats.setupHits.fetch_add(1, std::memory_order_relaxed);
        return f;
    }
    f = Load(WeakFingerprint(aBlob));
    if (f > 0.0f)
    {
        g_stats.setupHitsWeak.fetch_add(1, std::memory_order_relaxed);
    }
    return f;
}

Stats& GetStats()
{
    return g_stats;
}

float FloorForFields(std::uint64_t aModMask, std::uint64_t aInitMask, std::uint32_t aNumMod, std::uint32_t aNumInit,
                     std::uint32_t aMaxParticles, std::uint64_t aSimHash)
{
    if (!g_layout.ok)
    {
        return 0.0f;
    }
    const float f = Load(WeakFromFields(aModMask, aInitMask, aNumMod, aNumInit, aMaxParticles, aSimHash));
    if (f > 0.0f)
    {
        g_stats.setupHitsWeak.fetch_add(1, std::memory_order_relaxed);
    }
    return f;
}

std::atomic<float>& Floor()
{
    return g_floor;
}
} // namespace NCW::SmokeTag
