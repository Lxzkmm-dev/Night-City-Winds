#pragma once

// Engine functions this plugin touches, by RED4ext address-library hash (stable across patches;
// RED4ext maps each to this build's address). See docs/PHYSICS_RE_FINDINGS.md section 4.1.

#include <cstdint>

namespace NCW::Hashes
{
// void vehicle::WheeledPhysics::ApplyAirResistance(const Vector3& velocity, float dt)
//   F = -1.2 * airResistanceFactor(this+0xC90) * |v|^2 * v^ ; physicsData.force += F * dt
constexpr std::uint32_t WheeledPhysics_ApplyAirResistance = 3489929719;

// CPU particles (docs/SMOKE_WIND_ANALYSIS.md 5f). 2.31 addresses in comments.
// runtime emitter setup from the cooked blob; ends by building the modifier list (0x417F6C)
constexpr std::uint32_t Particle_EmitterSetup = 2428183128;
// the allocator and free the modifier-list builder (0x41826C) uses (0xB04CA8, 0x14C19C)
constexpr std::uint32_t Particle_ListAlloc = 2087392471;
constexpr std::uint32_t Particle_ListFree = 2786008195;
// the per-frame CPU particle passes (0x1395B4, 0x13A394) and the spawn pass (0x13326C): r9 points
// at the runtime emitter pointer, the 5th argument is the owner whose +0x38 is the PhysX particle
// pool; a null pool makes the pass simulate the particles on the CPU
constexpr std::uint32_t Particle_SimA = 485192079;
constexpr std::uint32_t Particle_SimB = 524907113;
constexpr std::uint32_t Particle_Spawn = 513826673;
// CParticleModificatorVelocityOverLife's modifier function (0x13A28C): multiplies the working
// velocity by its curve each frame; replaced by a record-skipping stub on tagged smoke
constexpr std::uint32_t Particle_VelocityOverLife = 3859429499;
} // namespace NCW::Hashes

namespace NCW
{
// Resolves a hash through RED4ext without RED4ext's terminate-on-miss behaviour:
// returns 0 when the hash is unknown (e.g. after a game patch), so a feature can switch
// itself off instead of crashing the game.
std::uintptr_t ResolveOrZero(std::uint32_t aHash);
} // namespace NCW
