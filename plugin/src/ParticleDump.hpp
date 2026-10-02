#pragma once

#include <functional>
#include <string>

namespace RED4ext
{
struct ISerializable;
}

namespace CWF::ParticleDump
{
using LogFn = std::function<void(const std::string&)>;

// Logs a CParticleSystem's emitters (wind influence, local space, cooked simulation signature,
// module classes). Read-only; main thread.
void Dump(RED4ext::ISerializable* aSystem, const char* aLabel, const LogFn& aLog);
} // namespace CWF::ParticleDump
