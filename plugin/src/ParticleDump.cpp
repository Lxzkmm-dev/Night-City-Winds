// Read-only dump of a CParticleSystem's emitters for the smoke analysis: per emitter, the wind
// influence, local-space flag and the cooked render blob's simulation signature. Reads fields
// through the RTTI property table (offsets from the engine itself), never through script, so
// it is safe on ISerializable-only objects such as particle modules.

#include "ParticleDump.hpp"

#include <cstdio>
#include <string>

#include <RED4ext/RED4ext.hpp>

namespace
{
RED4ext::CClass* TypeOf(RED4ext::ISerializable* aObj)
{
    return aObj ? aObj->GetType() : nullptr;
}

RED4ext::CProperty* Prop(RED4ext::CClass* aClass, const char* aName)
{
    return aClass ? aClass->GetProperty(aName) : nullptr;
}

template<typename T>
T* Field(void* aInstance, RED4ext::CProperty* aProp)
{
    return (aInstance && aProp) ? reinterpret_cast<T*>(static_cast<char*>(aInstance) + aProp->valueOffset) : nullptr;
}

// a struct-typed property's own class (for nested structs)
RED4ext::CClass* StructClass(RED4ext::CProperty* aProp)
{
    if (!aProp || !aProp->type || aProp->type->GetType() != RED4ext::rtti::ERTTIType::Class)
    {
        return nullptr;
    }
    return static_cast<RED4ext::CClass*>(aProp->type);
}

std::string ClassName(RED4ext::ISerializable* aObj)
{
    auto cls = TypeOf(aObj);
    return cls ? cls->GetName().ToString() : std::string("?");
}
} // namespace

namespace NCW::ParticleDump
{
void Dump(RED4ext::ISerializable* aSystem, const char* aLabel, const LogFn& aLog)
{
    auto sysCls = TypeOf(aSystem);
    auto emittersProp = Prop(sysCls, "emitters");
    auto emitters = Field<RED4ext::DynArray<RED4ext::Handle<RED4ext::ISerializable>>>(aSystem, emittersProp);
    if (!emitters)
    {
        aLog(std::string("signatures: ") + aLabel + ": no emitters property");
        return;
    }
    aLog(std::string("signatures: ") + aLabel + " (" + std::to_string(emitters->Size()) + " emitters)");

    for (uint32_t i = 0; i < emitters->Size(); ++i)
    {
        auto emitter = (*emitters)[i].instance;
        if (!emitter)
        {
            continue;
        }
        auto ecls = TypeOf(emitter);
        auto wind = Field<float>(emitter, Prop(ecls, "windInfluence"));
        auto local = Field<bool>(emitter, Prop(ecls, "keepSimulationLocal"));

        char line[1024];
        int n = std::snprintf(line, sizeof(line), "  e%u wind %.2f %s", i, wind ? *wind : -1.0f,
                              (local && *local) ? "LOCAL" : "world");

        // cooked render blob: header.emitterInfo
        auto blobHandle = Field<RED4ext::Handle<RED4ext::ISerializable>>(emitter, Prop(ecls, "renderResourceBlob"));
        auto blob = blobHandle ? blobHandle->instance : nullptr;
        auto bcls = TypeOf(blob);
        auto headerProp = Prop(bcls, "header");
        auto header = Field<char>(blob, headerProp);
        auto hcls = StructClass(headerProp);
        auto infoProp = Prop(hcls, "emitterInfo");
        auto info = Field<char>(header, infoProp);
        auto icls = StructClass(infoProp);
        if (info && icls)
        {
            auto f = [&](const char* aName) { return Field<char>(info, Prop(icls, aName)); };
            auto bw = reinterpret_cast<float*>(f("windInfluence"));
            auto st = reinterpret_cast<uint32_t*>(f("simulationType"));
            auto pt = reinterpret_cast<uint32_t*>(f("particleType"));
            auto nm = reinterpret_cast<uint32_t*>(f("numModifiers"));
            auto mm = reinterpret_cast<uint64_t*>(f("modifierSetMask"));
            auto ni = reinterpret_cast<uint32_t*>(f("numInitializers"));
            auto im = reinterpret_cast<uint64_t*>(f("initializerSetMask"));
            auto sh = reinterpret_cast<uint64_t*>(f("simulationHash"));
            auto bl = reinterpret_cast<bool*>(f("keepSimulationLocal"));
            n += std::snprintf(line + n, sizeof(line) - n,
                               " | blob wind %.2f%s sim %u ptype %u mods %u modmask %016llX inits %u initmask %016llX simhash %016llX",
                               bw ? *bw : -1.0f, (bl && *bl) ? " LOCAL" : "", st ? *st : 0u, pt ? *pt : 0u,
                               nm ? *nm : 0u, mm ? static_cast<unsigned long long>(*mm) : 0ull, ni ? *ni : 0u,
                               im ? static_cast<unsigned long long>(*im) : 0ull,
                               sh ? static_cast<unsigned long long>(*sh) : 0ull);
        }
        else
        {
            n += std::snprintf(line + n, sizeof(line) - n, " | no blob info (%s)", blob ? ClassName(blob).c_str() : "null");
        }

        std::string text(line);
        auto modules = Field<RED4ext::DynArray<RED4ext::Handle<RED4ext::ISerializable>>>(emitter, Prop(ecls, "modules"));
        if (modules)
        {
            text += " | modules:";
            for (uint32_t m = 0; m < modules->Size(); ++m)
            {
                if ((*modules)[m].instance)
                {
                    text += " " + ClassName((*modules)[m].instance);
                }
            }
        }
        aLog(text);
    }
}
} // namespace NCW::ParticleDump
