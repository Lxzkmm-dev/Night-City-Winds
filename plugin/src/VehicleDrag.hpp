#pragma once

#include <RED4ext/RED4ext.hpp>

namespace CWF::VehicleDrag
{
// Detours vehicle::WheeledPhysics::ApplyAirResistance so the game's own per-car drag acts on
// the airspeed (velocity - wind) instead of the ground speed. Returns false, and leaves the
// game untouched, if the hash doesn't resolve or the hook fails.
bool Attach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
void Detach(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk);
bool IsAttached();
} // namespace CWF::VehicleDrag
