// =============================================================================
// Night City Winds - RED4ext plugin (0.5.1, 2026-10-02; Cyberpunk Wind Framework until 0.5.0)
//
// 0.1.0: the vehicle layer. Detours the game's own car air-drag function so it acts on the
// airspeed (velocity - wind), and gives redscript a handful of natives to drive and inspect it:
//   NCW_SetWind(wind: Vector4)          world wind velocity, m/s (W ignored)
//   NCW_GetWind() -> Vector4
//   NCW_SetEnabled(enabled: Bool)       off = vanilla drag
//   NCW_IsVehicleHookActive() -> Bool   false if the hash didn't resolve for this game version
//   NCW_GetVehicleDragStats() -> Vector4  (drag calls so far, last ground speed, last airspeed,
//                                          last airResistanceFactor)
// 0.2.0: the prop layer. Drag on loose PhysX bodies near the wind origin, before each step:
//   NCW_SetWindOrigin(origin: Vector4)  where the wind was sampled (the player / their car)
//   NCW_SetPropWind(enabled: Bool, radius: Float, scale: Float)
//   NCW_IgnoreNear(id: Int32, position: Vector4, radius: Float)  skip bodies there for 0.5 s
//                                       (for mods that apply the wind to their own bodies)
//   NCW_IsPropHookActive() -> Bool
//   NCW_Log(message: String)            a line in red4ext\logs\nightcitywinds-*.log
//   NCW_GetPropWindStats() -> Vector4   (PhysX steps seen, dynamic actors, pushed last step,
//                                        largest force last step in N)
// 0.3.0: the smoke layer. Our own wind step appended to the engine's CPU particle modifiers:
//   NCW_SetSmokeWind(enabled: Bool, gain: Float)
//   NCW_GetSmokeWindStats() -> Vector4  (emitter setups seen, emitters given wind,
//                                        particle steps in thousands, last dt)
// =============================================================================

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>

#include <Windows.h>

#include "Addresses.hpp"
#include "ParticleDump.hpp"
#include "PhysXWind.hpp"
#include "SmokeTag.hpp"
#include "SmokeWind.hpp"
#include "VehicleDrag.hpp"
#include "Wind.hpp"

namespace
{
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
} // namespace

std::uintptr_t NCW::ResolveOrZero(std::uint32_t aHash)
{
    using Resolve_t = std::uintptr_t (*)(std::uint32_t);
    static Resolve_t resolve = []() -> Resolve_t
    {
        const auto module = GetModuleHandleW(L"RED4ext.dll");
        return module ? reinterpret_cast<Resolve_t>(GetProcAddress(module, "RED4ext_ResolveAddress")) : nullptr;
    }();
    return resolve ? resolve(aHash) : 0;
}

// ---- natives ---------------------------------------------------------------------------
namespace
{
void NCW_SetWind(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    RED4ext::Vector4 wind{};
    RED4ext::GetParameter(aFrame, &wind);
    aFrame->code++; // ParamEnd
    NCW::Wind::Set({wind.X, wind.Y, wind.Z});
    // the script tick calls this every frame: install the PhysX hook once a scene exists
    NCW::PhysXWind::EnsureHooked();
}

void NCW_SetWindOrigin(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    RED4ext::Vector4 origin{};
    RED4ext::GetParameter(aFrame, &origin);
    aFrame->code++;
    NCW::Wind::SetOrigin({origin.X, origin.Y, origin.Z});
}

void NCW_SetPropWind(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool enabled = true;
    float radius = 50.0f;
    float scale = 1.0f;
    RED4ext::GetParameter(aFrame, &enabled);
    RED4ext::GetParameter(aFrame, &radius);
    RED4ext::GetParameter(aFrame, &scale);
    aFrame->code++;
    auto& s = NCW::PhysXWind::GetSettings();
    s.enabled.store(enabled);
    s.radius.store(radius);
    s.scale.store(scale);
}

void NCW_IgnoreNear(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    int32_t id = 0;
    RED4ext::Vector4 position{};
    float radius = 1.0f;
    RED4ext::GetParameter(aFrame, &id);
    RED4ext::GetParameter(aFrame, &position);
    RED4ext::GetParameter(aFrame, &radius);
    aFrame->code++;
    NCW::PhysXWind::IgnoreNear(id, position.X, position.Y, position.Z, radius);
}

void NCW_Log(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    RED4ext::CString message;
    RED4ext::GetParameter(aFrame, &message);
    aFrame->code++;
    if (g_sdk)
    {
        g_sdk->logger->Info(g_handle, message.c_str());
    }
}

void NCW_DumpParticles(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    RED4ext::Handle<RED4ext::ISerializable> system;
    RED4ext::CString label;
    RED4ext::GetParameter(aFrame, &system);
    RED4ext::GetParameter(aFrame, &label);
    aFrame->code++;
    if (!g_sdk)
    {
        return;
    }
    if (!system.instance)
    {
        g_sdk->logger->InfoF(g_handle, "signatures: %s did not load", label.c_str());
        return;
    }
    NCW::ParticleDump::Dump(system.instance, label.c_str(),
                            [](const std::string& aLine) { g_sdk->logger->Info(g_handle, aLine.c_str()); });
}

void NCW_IsPropHookActive(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, bool* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        *aOut = NCW::PhysXWind::IsHooked();
    }
}

void NCW_GetPropWindStats(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::Vector4* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        auto& s = NCW::PhysXWind::GetStats();
        *aOut = RED4ext::Vector4{static_cast<float>(s.steps.load()), static_cast<float>(s.scanned.load()),
                                 static_cast<float>(s.pushed.load()), s.maxForce.load()};
    }
}

void NCW_SetSmokeWind(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool enabled = true;
    float gain = 1.0f;
    RED4ext::GetParameter(aFrame, &enabled);
    RED4ext::GetParameter(aFrame, &gain);
    aFrame->code++;
    auto& s = NCW::SmokeWind::GetSettings();
    if (s.enabled.load() != enabled || s.gain.load() != gain)
    {
        NCW::SmokeWind::LogSettingChange(enabled, gain);
    }
    s.enabled.store(enabled);
    s.gain.store(gain);
}

void NCW_GetSmokeWindStats(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::Vector4* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        auto& s = NCW::SmokeWind::GetStats();
        *aOut = RED4ext::Vector4{static_cast<float>(s.emittersSeen.load()), static_cast<float>(s.emittersWindy.load()),
                                 static_cast<float>(s.particleSteps.load()) / 1000.0f, s.lastDt.load()};
    }
}

// 0.6.0: smoke tagging at load (replaces the smoke archive): called from a Resource/PostLoad
// callback on CParticleSystem, on the loading threads, so it does nothing but the tagging
void NCW_TagSmokeSystem(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    RED4ext::Handle<RED4ext::ISerializable> system;
    RED4ext::CString path;
    RED4ext::GetParameter(aFrame, &system);
    RED4ext::GetParameter(aFrame, &path);
    aFrame->code++;
    if (system.instance)
    {
        NCW::SmokeTag::TagSystem(system.instance, path.c_str());
    }
}

void NCW_SetSmokeTagFloor(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    float floor = 0.2f;
    RED4ext::GetParameter(aFrame, &floor);
    aFrame->code++;
    NCW::SmokeTag::Floor().store(floor);
}

void NCW_GetSmokeTagStats(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::Vector4* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        auto& s = NCW::SmokeTag::GetStats();
        *aOut = RED4ext::Vector4{static_cast<float>(s.systemsTagged.load()), static_cast<float>(s.emittersTagged.load()),
                                 static_cast<float>(s.collisionsStripped.load()),
                                 static_cast<float>(s.setupHits.load()) + static_cast<float>(s.setupHitsWeak.load()) / 1000.0f};
    }
}

void NCW_GetWind(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::Vector4* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        const auto w = NCW::Wind::Get();
        *aOut = RED4ext::Vector4{w.x, w.y, w.z, 0.0f};
    }
}

void NCW_SetEnabled(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool enabled = true;
    RED4ext::GetParameter(aFrame, &enabled);
    aFrame->code++;
    NCW::Wind::SetEnabled(enabled);
}

void NCW_IsVehicleHookActive(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, bool* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        *aOut = NCW::VehicleDrag::IsAttached();
    }
}

void NCW_GetVehicleDragStats(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::Vector4* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
    {
        *aOut = RED4ext::Vector4{static_cast<float>(NCW::Wind::dragCalls.load(std::memory_order_relaxed)),
                                 NCW::Wind::lastGroundSpeed.load(std::memory_order_relaxed),
                                 NCW::Wind::lastAirSpeed.load(std::memory_order_relaxed),
                                 NCW::Wind::lastResistanceFactor.load(std::memory_order_relaxed)};
    }
}

template<typename T>
RED4ext::CGlobalFunction* Register(const char* aName, RED4ext::ScriptingFunction_t<T> aFunc, const char* aReturn)
{
    auto func = RED4ext::CGlobalFunction::Create(aName, aName, aFunc);
    func->flags = {.isNative = true, .isStatic = true};
    if (aReturn)
    {
        func->SetReturnType(aReturn);
    }
    return func;
}

void PostRegisterTypes()
{
    auto rtti = RED4ext::CRTTISystem::Get();

    auto setWind = Register("NCW_SetWind", &NCW_SetWind, nullptr);
    setWind->AddParam("Vector4", "wind");
    rtti->RegisterFunction(setWind);

    rtti->RegisterFunction(Register("NCW_GetWind", &NCW_GetWind, "Vector4"));

    auto setEnabled = Register("NCW_SetEnabled", &NCW_SetEnabled, nullptr);
    setEnabled->AddParam("Bool", "enabled");
    rtti->RegisterFunction(setEnabled);

    rtti->RegisterFunction(Register("NCW_IsVehicleHookActive", &NCW_IsVehicleHookActive, "Bool"));
    rtti->RegisterFunction(Register("NCW_GetVehicleDragStats", &NCW_GetVehicleDragStats, "Vector4"));

    auto setOrigin = Register("NCW_SetWindOrigin", &NCW_SetWindOrigin, nullptr);
    setOrigin->AddParam("Vector4", "origin");
    rtti->RegisterFunction(setOrigin);

    auto setProp = Register("NCW_SetPropWind", &NCW_SetPropWind, nullptr);
    setProp->AddParam("Bool", "enabled");
    setProp->AddParam("Float", "radius");
    setProp->AddParam("Float", "scale");
    rtti->RegisterFunction(setProp);

    auto ignore = Register("NCW_IgnoreNear", &NCW_IgnoreNear, nullptr);
    ignore->AddParam("Int32", "id");
    ignore->AddParam("Vector4", "position");
    ignore->AddParam("Float", "radius");
    rtti->RegisterFunction(ignore);

    auto dump = Register("NCW_DumpParticles", &NCW_DumpParticles, nullptr);
    dump->AddParam("handle:CParticleSystem", "system");
    dump->AddParam("String", "label");
    rtti->RegisterFunction(dump);

    auto log = Register("NCW_Log", &NCW_Log, nullptr);
    log->AddParam("String", "message");
    rtti->RegisterFunction(log);

    rtti->RegisterFunction(Register("NCW_IsPropHookActive", &NCW_IsPropHookActive, "Bool"));
    rtti->RegisterFunction(Register("NCW_GetPropWindStats", &NCW_GetPropWindStats, "Vector4"));

    auto smoke = Register("NCW_SetSmokeWind", &NCW_SetSmokeWind, nullptr);
    smoke->AddParam("Bool", "enabled");
    smoke->AddParam("Float", "gain");
    rtti->RegisterFunction(smoke);
    rtti->RegisterFunction(Register("NCW_GetSmokeWindStats", &NCW_GetSmokeWindStats, "Vector4"));

    auto tag = Register("NCW_TagSmokeSystem", &NCW_TagSmokeSystem, nullptr);
    tag->AddParam("handle:CParticleSystem", "system");
    tag->AddParam("String", "path");
    rtti->RegisterFunction(tag);
    auto tagFloor = Register("NCW_SetSmokeTagFloor", &NCW_SetSmokeTagFloor, nullptr);
    tagFloor->AddParam("Float", "floor");
    rtti->RegisterFunction(tagFloor);
    rtti->RegisterFunction(Register("NCW_GetSmokeTagStats", &NCW_GetSmokeTagStats, "Vector4"));
}

void RegisterTypes()
{
}
} // namespace

// ---- plugin entry points ------------------------------------------------------------------
RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
    {
        g_handle = aHandle;
        g_sdk = aSdk;

        auto rtti = RED4ext::CRTTISystem::Get();
        rtti->AddRegisterCallback(RegisterTypes);
        rtti->AddPostRegisterCallback(PostRegisterTypes);

        NCW::VehicleDrag::Attach(aHandle, aSdk);
        NCW::PhysXWind::Init(aHandle, aSdk);
        NCW::SmokeTag::Init(aHandle, aSdk);
        NCW::SmokeWind::Attach(aHandle, aSdk);
        break;
    }
    case RED4ext::v1::EMainReason::Unload:
    {
        NCW::SmokeWind::Detach(aHandle, aSdk);
        NCW::PhysXWind::Shutdown();
        NCW::VehicleDrag::Detach(aHandle, aSdk);
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"Night City Winds";
    aInfo->author = L"Omar";
    aInfo->version = RED4EXT_V1_SEMVER(0, 6, 0);
    // Addresses come from hashes that resolve per game version, and a missing hash switches
    // the feature off instead of crashing, so the plugin doesn't pin a game version.
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
