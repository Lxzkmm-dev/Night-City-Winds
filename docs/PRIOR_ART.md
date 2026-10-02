# Prior art: Cyberpunk 2077 physics RE for a wind-force framework (target: game 2.31, RED4ext 1.30)

Researched 2026-10-01. Every fact has a source URL. Tags:
- **[CURRENT]**: the source states it was updated for 2.30/2.31.
- **[STALE?]**: an RVA, byte pattern or layout taken from 1.52/1.6/1.61, or not checked against 2.3x.
- **[UNVERIFIED]**: my inference, not stated in the source.

---

## 0. Key takeaways

1. **Let There Be Flight (LTBF) is current and is the main reference.** The repo was last committed on 2026-09-13. Its CLAUDE.md says: game 2.31, last release v0.3.17 targeting 2.30, RED4ext 1.27.0+. It uses RED4ext universal hashes, not signature scanning. Its SDK fork `jackhumbert/RED4ext.SDK@new-types` had "add 2.31 to GameVersions.json" committed on 2026-09-13 and "Add support for patch 2.31" on 2025-09-11.
   - https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/CLAUDE.md
   - https://api.github.com/repos/jackhumbert/RED4ext.SDK/commits?sha=new-types
2. **How LTBF applies force.** A per-frame bucket update, `UpdateBucketMask::Vehicle` at stage `UpdateBucketStage::PhysicsExecuteAsyncQueries`, adds to `vehicle::PhysicsData::force` and `::torque`. The fields are at `+0x00` and `+0x0C`, and `PhysicsData` is reached through `vehicle::BaseObject + 0x2D0`. LTBF calls `ForceEnablePhysics()` first. No PhysX calls are made directly.
3. **Wind data exists only as authored weather data.** `WindAreaSettings { CurveData<float> strength @0x48; CurveData<Vector4> direction @0x80 }` sits inside `worldEnvironmentAreaParameters.renderAreaSettings.areaParameters[]`. That is reached from `worldWeatherState.environmentAreaParameters`, and the state comes from Codeware's `WeatherSystem.GetWeatherState()`. No vanilla script reads wind. Search of the 2.31 decompiled scripts for "Wind" found nothing.
4. **Script-only impulse routes (no plugin needed):**
   - `PhysicalImpulseEvent` queued on a vehicle. Vanilla does this to knock over bikes.
   - `PhysicalBodyInterface.AddLinearImpulse(impulse: Vector4, originInCOM: Bool, opt offset: Vector4)`, from `ColliderComponent` or `PhysicalMeshComponent.CreatePhysicalBodyInterface()`.
   - `RagdollApplyImpulseEvent` for ragdolls.

---

## 1. jackhumbert/let_there_be_flight

Repo: https://github.com/jackhumbert/let_there_be_flight (branch `main`)

- **Submodules** (https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/.gitmodules):
  - `deps/red4ext.sdk` → `git@github.com:jackhumbert/RED4ext.SDK.git`, `branch = new-types`
  - `deps/red_lib` → `https://github.com/jackhumbert/cp2077-red-lib.git`, `branch = jack`
  - `src/redscript/codeware` → `jackhumbert/cp2077-codeware`
  - Also: `cyberpunk_cmake`, `mod_settings`, `input_loader`, `spdlog`, `Detours`, `archive_xl`, `tweak_xl`.
- **Address strategy** [CURRENT] (CLAUDE.md): "universal address hashing via RED4ext's `UniversalRelocBase::Resolve` rather than signature scanning".
  - The older tooling is still in the repo. `cmake/FindIndividualRED4extAddresses.cmake` runs `zoltan-clang.exe` over SDK headers, reading their `/// @pattern` comments against `Cyberpunk2077.exe`, and generates `zoltan/Addresses.hpp`.
  - The readme still describes the old manual workflow ("Update all addresses with '1.61hf1 RVA:'") [STALE].
  - https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/cmake/FindIndividualRED4extAddresses.cmake

### 1.1 Hooks in `src/red4ext/Physics/VehiclePhysicsUpdate.cpp` [CURRENT, hash-based]

Source: https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/red4ext/Physics/VehiclePhysicsUpdate.cpp

All hooks use `REGISTER_FLIGHT_HOOK_HASH(ret, HASH, Name, args...)`. Each hash is a RED4ext universal address ID. The comment above each hook gives the real engine symbol.

| Hash (dec) | Engine symbol (from comment) | LTBF hook signature | What LTBF does |
|---|---|---|---|
| 2526549425 | `void vehicle::WheelSuspensionBase::ApplyAllResistances(float)` | `(vehicle::WheeledPhysics* self, float dt)` | Applies air and low-speed resistance when speed² ≥ 10000 (≥ 100 m/s), then calls the original |
| 3303544265 | `void vehicle::RigidBody::ApplyAngularImpulse(Vector3 const&, Vector3 const&)` | `(vehicle::PhysicsData*, Vector3* offset, Vector3* torque)` | Blocks it while flying |
| 611586815 | `void vehicle::RigidBody::ApplyImpulse(Vector3 const&, Vector3 const&)` | `(vehicle::PhysicsData*, Vector3* offset, Vector3* force)` | Blocks it while flying ("adds to force & computes torque") |
| 3281786499 | `void vehicle::WheelSuspensionBase::FixedUpdate_PostSolve(float)` (vtable `sub_58`) | `uintptr_t (vehicle::WheeledPhysics*, float)` | Sets `driveHelpers.size = 0` while flying |
| 1414536155 | `void vehicle::CarBaseObject::AdjustSplineTransformToRoad(Transform&, float, float)` | `(CarBaseObject*, Transform*, float, float)` | Disables the road-align PID |
| 2879787320 | `vehicle::CarSuspension::UpdateAnimVars(float)` | `(vehicle::CarPhysics*, float)` | Animation input |
| 3191280029 | `vehicle::BikeSuspension::UpdateAnimVars(float)` | `(vehicle::BikePhysics*, float)` | `tiltControlEnabled` |
| 1989218322 | `void vehicle::DrivingForcesHelper::Apply(float)` | `(uint64_t* a1, float)`; `physics = (WheeledPhysics*)a1[2]` | Disables tire torque and friction |
| 2268865059 | `vehicle::WheelSuspensionBase::ApplyAntiSwaybarForce(CarWheel, CarWheel, float, math::Transform const&)` | `(WheeledPhysics*, u8 rear, u8 front, float, Transform*)` | Disables anti-sway and calls `vehicle::PhysicsStructUpdate(physicsData)` |
| 3489929719 | `WheeledPhysics::ApplyAirResistance(Vector3 const& vel, float dt)` | Called via `IHookable::StaticHook<void, 3489929719>` | Engine function, called directly |
| 1695226878 | `WheeledPhysics::ApplyLowSpeedResistances(Vector3 const&, float)` | `StaticHook<void, 1695226878>` | Engine function |
| 2463305925 | `GetGameSystemsData` hook (adds LTBF's game system) | | |
| 2793083181 | `vehicle::BaseObject::ForceEnablePhysics()` | `StaticHook<void, 2793083181>(this)` | Wakes or enables vehicle physics |

`ApplyImpulse` (611586815) and `ApplyAngularImpulse` (3303544265) are the engine's own entry points for adding an impulse at a point on a vehicle. A wind plugin could call them through `UniversalRelocFunc` instead of writing the fields directly [UNVERIFIED that calling them from a bucket update is safe; LTBF only hooks them].

### 1.2 Where and when LTBF applies force [CURRENT]

- **`src/red4ext/FlightSystem.cpp`**, in `OnRegisterUpdates(UpdateRegistrar*)`:
  ```cpp
  aRegistrar->RegisterUpdate(UpdateBucketMask::Vehicle, UpdateBucketStage::PrePhysicsTick, this, "FlightSystem/PrePhysics", &PrePhysics);           // audio listener only
  aRegistrar->RegisterUpdate(UpdateBucketMask::Vehicle, UpdateBucketStage::PhysicsExecuteAsyncQueries, this, "FlightSystem/UpdateComponents", &UpdateComponents);
  ```
  `UpdateComponents(UpdateBucketEnum, FrameInfo& frame, JobQueue&)` iterates registered `FlightComponent`s and calls `OnUpdate(frame.deltaTime)`.
  https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/red4ext/FlightSystem.cpp
- **`src/red4ext/Flight/Component.cpp`**, `FlightComponent::OnUpdate(float dt)`:
  ```cpp
  vehicle = reinterpret_cast<vehicle::BaseObject*>(this->entity);   // after IsOfClass("vehicleBaseObject")
  if (this->hasUpdate) {
    vehicle->ForceEnablePhysics();
    ExecuteFunction(this, this->nativeType->GetFunction("OnUpdate"), nullptr, deltaTime); // redscript computes this.force/this.torque
    vehicle->physicsData->force  += this->force.AsVector3();
    vehicle->physicsData->torque += this->torque.AsVector3();
    this->force = Vector4(); this->torque = Vector4();
  }
  ```
  https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/red4ext/Flight/Component.cpp
- **`FlightComponent` layout** (`Flight/Component.hpp`). It is a custom `game::Component` subclass:
  - `sys` @0xA8, `active` @0xB8, `hasUpdate` @0xB9, `Vector4 force` @0xC0, `Vector4 torque` @0xD0, `configuration` @0xE0, `linearDamp` @0xF8, `angularDamp` @0xFC.
  - The redscript side declares the same fields with `@runtimeProperty("offset", "0xC0") public native let force: Vector4;`. This red_lib/Codeware technique exposes C++ fields to script at fixed offsets.
  - `FlightComponent::Get(vehicle)` scans `v->componentsStorage.components` for the RTTI type.
  - https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/red4ext/Flight/Component.hpp
  - https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/redscript/Flight/FlightComponent.reds
- **Units, as LTBF uses them** (`FlightComponent.reds`):
  ```
  force *= timeDelta; force *= this.stats.s_mass; force = this.stats.d_orientation * force;   // accel -> m*a*dt, local->world
  torque *= timeDelta; torque *= this.stats.s_mass; torque.X *= thrusterTensor.X ...; torque = d_orientation * torque;
  this.force += force; this.torque += torque;
  ```
  The force added to `PhysicsData::force` is therefore mass × acceleration × frame dt, in world space. That is an impulse-like quantity. Whether the engine treats `force` as an impulse or a force (integrated again with the fixed dt) is **not documented** [UNVERIFIED]. Calibrate empirically, for example by checking that 9.81·m·dt cancels gravity in hover mode.

### 1.3 Script natives LTBF adds to `VehicleObject` (`Extensions/VehicleObject.cpp`)

These are good models for a wind plugin's script API.
https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/src/red4ext/Extensions/VehicleObject.cpp

```cpp
GetCenterOfMass()    -> physicsData->centerOfMass
GetAngularVelocity() -> physicsData->angularVelocity
EnableGravity(bool)  -> physicsData->unk1B0 = gravity      // unk1B0 = gravity flag
HasGravity()         -> physicsData->unk1B0
GetInertiaTensor()   -> physicsData->localInertiaTensor ; GetGlobalInertiaTensor() -> worldInertiaTensor
UsesInertiaTensor(), GetMomentOfInertiaScale(), ForceEnablePhysics(), TurnOffAirControl() (zeros airControl PIDs)
```

Registered with `aType->AddFunction<&VehicleObject::X>("X")` in `OnExpand`. `WheeledObject.cpp` adds `GetDampedSpringForce(i)`, which reads `((WheeledPhysics*)physics)->insert2[i].dampedSpringForce`.

---

## 2. Physics structs in jackhumbert/RED4ext.SDK (`new-types`)

### 2.1 `vehicle::PhysicsData` (engine name `vehicle::RigidBody`)

Source: https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/vehiclePhysicsData.hpp. `RED4EXT_ASSERT_SIZE(PhysicsData, 0x1E0)`.

```
Vector3 force;            // 00   accumulated force/impulse
Vector3 torque;           // 0C
Vector3 velocity;         // 18   linear velocity (world)
Vector3 angularVelocity;  // 24
Quaternion orientation;   // 30
float inverseMass;        // 40
uint32_t unk48[3];
Matrix localInertiaTensor; Matrix worldInertiaTensor; Matrix invertedLocalInertiaTensor; Matrix invertedWorldInertiaTensor;
WorldTransform currentTransform; Vector4 worldPosition;
BaseObject* vehicle; uint64_t unk188; Box bounds;
uint8_t unk1B0 (gravity enabled per LTBF), unk1B1..3;
float alternativeChassisMass; // 1B4
uint8_t usesAlternativeChassisMass; ...
float total_mass; float chassis_mass; // 1C0
uint8_t usesInertiaTensor; ...
Vector3 momentOfInertiaScale; Vector3 centerOfMass;
```

Methods:

| Method | ID | Note |
|---|---|---|
| `ApplyForceAtPosition(Vector3* pos, Vector3* force)` | `/// @hash 611586815` | "adds to force & computes torque" |
| `ApplyTorqueAtPosition(Vector3* pos, Vector3* torque)` | `/// @hash 3303544265` | |
| `AddTorque(Vector3*)` | Pattern `F3 0F 10 41 0C F3 0F 58 02 F3 0F 11 41 0C F3 0F 10 4A 04 F3 0F 58 49 10 ...` | [STALE? 1.61] |
| `ApplyForceTorque()` | Pattern `40 53 48 83 EC 50 F3 0F 10 41 40 48 8B D9 F3 0F 10 51 08 0F 28 C8 F3 0F 59 09 ...` | Integrator step. No hash given [STALE?] |
| `UsesAlternativeChassisMass`, `SetUsesNormalChassisMass`, `UpdateChassis` | | Only 1.52 RVAs and patterns [STALE] |

The field layout up to `orientation` is very likely stable, since LTBF uses it on 2.31. Offsets after `unk48` are not asserted individually [STALE? check the later fields before use].

### 2.2 `vehicle::BaseObject` (fork: `vehicleBaseObject.hpp`)

Source: https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/vehicleBaseObject.hpp

- `bool isOnGround` @0x25C (asserted)
- `struct Input input` @0x264 (asserted): `acceleration`, `deceleration`, `handbrake`, `strafeY/X`, `turnInput`, `leanFB`, `rockFB`, ...
- `Physics* physics` @0x2C8 (asserted)
- `PhysicsData* physicsData` @0x2D0 (follows `physics`)
- `Handle<ChassisComponent> chassis`, `WorldTransform worldTransform`, plus `airControl` and `componentsStorage`, which LTBF uses.
- `RED4EXT_ASSERT_SIZE(BaseObject, 0xBA0)`; `VFT` hash `1274679101`
- `ForceEnablePhysics()` = `StaticHook<void, 2793083181>(this)`
- Other methods are commented with 1.52/1.6 RVAs only [STALE]: `SetPhysicsState(vehicle::PhysicsState, bool)`, `GetTotalMass`, `GetInverseMass`, `GetGravitationalForce`, `UpdatePhysicsSleepState(float)`, `PreUpdatePreMovePhysicsState*`, `PostMovePhysicsState*`.
- Upstream WopsS SDK only knows `isOnGround`@0x25C, `acceleration`@0x264, `deceleration`@0x268, `isReversing`@0x2A3, `burnout`@0x2BC, `archetype`@0x3A0, `isVehicleOnStateLocked`@0x6D2, and size 0xBA0. Its 0x2C0..0x3A0 range is padding.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/vehicleBaseObject.hpp

### 2.3 `vehicle::Physics` / `WheeledPhysics` / `CarPhysics` / `BikePhysics` / `TankPhysics`

Source: https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/vehiclePhysics.hpp

- **`vehicle::Physics`** (engine name `vehicle::SuspensionBase`). VFT hash `3054439199`, size `0xE0`.
  - Fields: `Vector3 velocity`@0x10, `WorldTransform worldTransform`@0x30, `BaseObject* parent`@0x60, `WaterParams* waterParams`@0x68, `worldTransform2`@0x80, `float sleepTimer`@0xA0 ("1.0 when awake, counts down ... -1.0 asleep"), `int32 unkB0` (physics proxy state; comment says "B0 in 2.0"), `bool isMoving`@0xB8, `UnkC8* physicsBaseStruct2`@0xD0 (holds `linearVelocity`/`angularVelocity` Vector4s).
  - Vtable slots:

    | Offset | Slot |
    |---|---|
    | 0x38 | `sub_38(dt)`: fall-under-world teleport |
    | 0x48 | `FixedUpdate_PreSolve(uint64, float)` |
    | 0x50 | `sub_50(dt)`: sets velocity from `physicsData`, water resistance |
    | 0x58 | `sub_58(dt)` = `FixedUpdate_PostSolve` (hash 3281786499 is the WheelSuspensionBase override) |
    | 0x140 | `LoadSomeVehiclePhysicsStuff(Handle<VehicleDriveModelData_Record>*)` |
- **`WheeledPhysics : Physics`** (engine name `vehicle::WheelSuspensionBase`). VFT `4127657236`, size `0xDE0` (asserted).
  - `insert2[4]`@0x5E0 (wheel runtime data), `driveHelpers` (DynArray) at ~0xDA8 in 2.x.
  - Tunables (offsets from comments): `airResistanceFactor`@~0xC50, `turningRollFactor`, `antiSwaybarDampingScalor`, `brakingFrictionFactor`, slip curves.
  - Static `FixedUpdate(...)` has pattern `48 89 5C 24 10 57 48 83 EC 30 48 8B F9 41 0F B6 49 32 E8 ? ? ? 00 48 8B 4F 08 80 B9 B4 00 00` [STALE 1.61hf1].
  - `ProcessAirControl(float)` has pattern `40 53 48 83 EC 30 48 8B D9 0F 29 74 24 20 48 8B 49 60 0F 28 F1 E8 ? ? F6 FF 84 C0 74 45 48 8B` [STALE 1.61hf1].
- **`CarPhysics`**: VFT `2824866490`, size `0xF50`, `AnimationUpdate` hash 2879787320.
- **`BikePhysics`**: VFT `3020491551`, size `0xE80`, `AnimationUpdate` hash 3191280029, `tiltControlEnabled`.
- **`TankPhysics : Physics`**: VFT `3053062962`, size `0x3E0`.
- **AV/flying vehicles** have no dedicated physics class in this header. LTBF makes ordinary cars fly through the `PhysicsData` force/torque path.

### 2.4 `vehicle::Collisions` (`vehicleCollisions.hpp`)

https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/vehicleCollisions.hpp

`WorldTransform`@0x10, `Vector4 linearVelocity`@0x30, `Vector3 acceleration`@0x40, `currentForce`@0x90, `currentForceVector`@0x94, `collisionForce`@0xA0, `collisionForceVector`@0xA4, `downforceMaybe`@0x104, `gravityScalar`@0x108 [names are guesses; version unknown].

### 2.5 PhysX proxy layer (fork)

These are not used by LTBF at runtime, but they are the documented way into PhysX actors for non-vehicle bodies.

- **`physics::ProxyManager`** (`physicsProxyManager.hpp`):
  - `Get()` = `UniversalRelocPtr<ProxyManager*>(37956006)`
  - Size `0x2E4068`; `systemKeys` (`StaticArray<Handle<BaseProxy>,0xFFFF>`) @0x2018, indexed by `ProxyID.index`; `proxyCacheIDs`@0x2A2054
  - `GetProxyHandle(Handle<BaseProxy>*, ProxyID)` has a 2.0 pattern `48 89 5C 24 08 48 89 74 24 10 55 57 41 56 48 8B EC 48 83 EC 50 41 8B D8 4C 8B F2 48 8B F9 41 83`
  - https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/physicsProxyManager.hpp
- **`physics::BaseProxy`** (`physicsBaseProxy.hpp`). VFT `/// @hash 1619396603:idata`. Fields: `Handle<ent::Entity> entity`, `Handle<ent::IComponent> component`, `ProxyID proxyID`, `ProxyType type`, ...
  - `virtual bool sub_58(uint32_t bodyIndex, uint32_t shapeIndex, StateValue updateType, void* data, size_t dataSize, bool wakeOption)` is the generic setter. Per the comment: "12: `PxRigidBodyExt::addForce`; 15: `setLinearVelocity & setAngularVelocity`; 73: `setRigidBodyFlag`".
  - `virtual bool sub_50(...)` is the getter.
  - `pxRigidBody_Update(void* pxRigidBody, StateValue, void*, __int64, uint shapeIndex, bool isAsleep)`, 1.6 pattern `48 8B C4 55 56 57 41 56 48 8D 68 B1 48 81 EC D8 00 00 00 44 0F 29 48 98 49 8B F0 44 0F 29 50 88` [STALE]. Comment cases:

    | Case | PhysX call |
    |---|---|
    | 1 | `setKinematicTarget` |
    | 2/3 | `setGlobalPose` |
    | 4 | `setLinearVelocity` |
    | 5 | `setAngularVelocity` |
    | 7/8 | linear/angular damping |
    | 13 | sleep/wake |
    | 16 | `setMassAndUpdateInertia` |
    | 17 | `setMassSpaceInertiaTensor` |
    | 38 | `setCMassLocalPose` |
  - `ProxyID_GetGlobalPose(Transform*, ProxyID*, uint bodyIndex)` [STALE 1.6].
  - https://raw.githubusercontent.com/jackhumbert/RED4ext.SDK/new-types/include/RED4ext/Scripting/Natives/physicsBaseProxy.hpp
- **`physicsStateValue`** enum, from reflection, so it is authoritative for the current game:
  - Position=1, Rotation=2, Transform=3, LinearVelocity=4, AngularVelocity=5, LinearSpeed=6, TouchesGround=10, TouchesWalls=11, **ImpulseAccumulator=12**, IsSleeping=13, Mass=16, Volume=18, IsSimulated=20, IsKinematic=21, TimeDeltaOverride=27, Radius=30, SimulationFilter=32.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/physics/StateValue.hpp
  - Jack's per-case comments above use slightly different numbering in places (e.g. 15 vs 4/5), so the reflection enum is the reference.
- **`physicsSimulationType`**: Static=0, Dynamic=1, Kinematic=2, Invalid=3.
- **PhysX**: LTBF links the game's `bin/x64/PhysX3_x64.dll` (`cmake/FindPhysX3.cmake`, `IMPORTED_IMPLIB deps/physx/PhysX3_x64.lib`), but no `Px*` calls appear in `FlightSystem.cpp`. I found no community code that calls PhysX 3.4 or NvCloth directly.
  - https://raw.githubusercontent.com/jackhumbert/let_there_be_flight/main/cmake/FindPhysX3.cmake

---

## 3. WopsS/RED4ext.SDK master: relevant generated headers (reflection-derived, current)

- **`WindAreaSettings : IAreaSettings`**: `CurveData<float> strength` @0x48, `CurveData<Vector4> direction` @0x80, size 0xB8.
  - `IAreaSettings : ISerializable`: `bool enable`@0x30, `uint64 disabledIndexedProperties`@0x38, size 0x48.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/WindAreaSettings.hpp
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/IAreaSettings.hpp
- **`world::WeatherState`** (`worldWeatherState : ISerializable`): `minDuration`@0x30, `maxDuration`@0x68, `Ref<world::EnvironmentAreaParameters> environmentAreaParameters`@0xA0, `RaRef<world::Effect> effect`@0xB8, `CName name`@0xC0, `probability`@0xC8, `transitionDuration`@0x100. Size 0x138.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/world/WeatherState.hpp
- **`worldEnvironmentAreaParameters : CResource`**: `WorldRenderAreaSettings renderAreaSettings`@0x40, which holds `DynArray<Handle<IAreaSettings>> areaParameters`@0x00. Find the `WindAreaSettings` entry by RTTI type.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/world/EnvironmentAreaParameters.hpp
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/WorldRenderAreaSettings.hpp
  - The `CurveData` keys are most likely time of day [UNVERIFIED]. You need a curve evaluator: RED4ext.SDK `CurveData` or your own sampling.
- **`ent::IPlacedComponent`** (hand-written): `parentTransform`@0x90, `localTransform`@0xC0, `worldTransform`@0xE0, size 0x120.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/entIPlacedComponent.hpp
- **`ent::ColliderComponent : IPlacedComponent`**: `colliders`@0x138, `simulationType`@0x148, `startInactive`@0x149, `useCCD`@0x14A, `massOverride`@0x14C, `mass`@0x150, `volume`@0x154, `Vector3 inertia`@0x158, `Transform comOffset`@0x170, `filterData`@0x190. Size 0x1C0.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/ent/ColliderComponent.hpp
- **`ent::PhysicalMeshComponent : MeshComponent`**: `filterData`@0x210, `filterDataSource`@0x230, `simulationType`@0x231, `startInactive`@0x232, `useResourceSimulationType`@0x233. Size 0x240.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/ent/PhysicalMeshComponent.hpp
- **`ent::PhysicalBodyInterface : IScriptable`** (`entPhysicalBodyInterface`): one opaque 8-byte field @0x40, probably the proxy ID/body. Size 0x48.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/ent/PhysicalBodyInterface.hpp
- **`ent::RagdollApplyImpulseEvent : red::Event`**: `Vector4 worldImpulsePos`@0x40, `Vector4 worldImpulseValue`@0x50, `float influenceRadius`@0x60. Size 0x70.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/ent/RagdollApplyImpulseEvent.hpp
- **`physics::SystemBody : physics::ISystemObject`** (`physicsSystemBody`): `collisionShapes`@0x38, `isQueryBodyOnly`@0x49, `SystemBodyParams params`@0x50, `localToModel`@0xB0, `mappedBoneName`@0xD0. Size 0x100.
  - **`physicsSystemBodyParams`**: `simulationType`@0, `mass`@0x08, `Vector3 inertia`@0x0C, `Transform comOffset`@0x20, `linearDamping`@0x40, `angularDamping`@0x44, solver iterations @0x48/0x4C, `maxDepenetrationVelocity`@0x50, `maxAngularVelocity`@0x54, `maxContactImpulse`@0x58.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/physics/SystemBody.hpp
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/physics/SystemBodyParams.hpp
- **`vehicle::ChassisComponent : IPlacedComponent`** (`vehicleChassisComponent`): `Ref<physics::SystemResource> collisionResource`@0x128, `optionalPlayerOnlyCollisionResource`@0x140. Size 0x190.
  - LTBF had a commented-out `GetComOffset()` reading `collisionResource.Fetch()->bodies[0]->params.comOffset`.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Scripting/Natives/Generated/vehicle/ChassisComponent.hpp
- The `Generated/physics/` directory also holds `PhysicalSystemOwner`, `ProxyType`, `SystemJoint`, `SystemResource`, `RagdollBodyInfo`, joint/drive types, `FilterData`, `QueryFilter`, `TraceResult`. Upstream has no vehicle-physics hand-written headers; those exist only in Jack's fork.

### 3.1 Update stages (`include/RED4ext/SystemUpdate.hpp`)

https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/SystemUpdate.hpp

- `UpdateBucketStage`: Entities_PreTick, Entities_ServiceEvents, **PrePhysicsTick**, UpdateTransformPrePhysics, PhysicsFlushBufferedState, **PhysicsExecuteAsyncQueries**, PostPhysicsSyncResults, UpdateTransformPostPhysics, AnimationUpdate, PostPhysicsTick, Entities_PostTick, Entities_PostServiceEvents.
- `UpdateBucketMask`: Vehicle=1, Character=2, AttachedObject=4, Everything=7.
- `UpdateTickGroup`: FrameBegin, …, PreBuckets, Buckets, PostBuckets, CameraUpdate, …
- `FrameInfo { float deltaTime; FrameDetailedInfo* details; uint8_t unk10; }`
- `UpdateRegistrar::RegisterUpdate(UpdateBucketMask, UpdateBucketStage, IScriptable*, const char*, BucketUpdateCallback&&)` and `RegisterUpdate(UpdateTickGroup, IScriptable*, const char*, GroupUpdateCallback&&)`.
- Hook into it by implementing `IUpdatableSystem::OnRegisterUpdates(UpdateRegistrar*)` on a native game system.
- Address hashes: `UpdateRegistrar_RegisterGroupUpdate = 0xFD914605`, `UpdateRegistrar_RegisterBucketUpdate = 0x192F4EA2`, `CRTTISystem_Get = 0x4A610F64`. `Detail/AddressHashes.hpp` has about 140 entries and **none for physics, vehicle or weather**.
  - https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Detail/AddressHashes.hpp

### 3.2 How RED4ext address hashes work (version-robust addressing)

- **Loading the table.** `RED4ext/src/dll/Addresses.cpp` loads `<game>/bin/x64/cyberpunk2077_addresses.json`. The format is `{"Addresses":[{"hash":"<uint64 as string>","offset":"<segment>:0x<hex>"}...]}`.
  - Segment 1 is added to `.text` VirtualAddress, 2 to `.rdata`, 3 to `.data`. Then the image base is added.
  - The map is keyed by `static_cast<uint32_t>(hash)`.
  - The resolver is exported as `RED4EXT_C_EXPORT std::uintptr_t RED4EXT_CALL RED4ext_ResolveAddress(const std::uint32_t aHash)`.
  - https://raw.githubusercontent.com/WopsS/RED4ext/master/src/dll/Addresses.cpp
- **SDK wrappers** (https://raw.githubusercontent.com/WopsS/RED4ext.SDK/master/include/RED4ext/Relocation.hpp and `Relocation-inl.hpp`):
  - `UniversalRelocBase::Resolve(uint32_t)` gets the resolver from the RED4ext module.
  - If the result is 0, it shows "Failed to find the address for the hash (...) ... likely caused by the mod using an incorrect or outdated hash" and **terminates the process**.
  - `UniversalRelocFunc<T>(hash)` and `UniversalRelocPtr<T>(hash)` wrap functions and globals.
  - `RelocFunc` and `RelocPtr` are legacy RVA + image-base wrappers.
  - `Detail::AddressResolverOverride` can replace resolution at compile time.
- **Why hashes survive patches.** The hash is a stable 32-bit ID per engine symbol. The JSON's offsets are regenerated for each game build, so the same hash resolves on 2.30 and 2.31 as long as the function still exists. LTBF's comments show demangled names next to the hashes. The hash-to-name mapping is not public in these repos; Jack and psiberx derive it themselves. That the JSON is generated and shipped alongside the game by CDPR is [UNVERIFIED].
- **Design implication.** A missing hash kills the game at plugin load. Validate optional hashes yourself: call the exported `RED4ext_ResolveAddress` and check for 0 rather than using `UniversalRelocFunc`. Fail soft per feature.

---

## 4. psiberx/cp2077-codeware

- **WeatherSystem extensions** [CURRENT]. C++ in `src/App/World/WeatherSystemEx.{hpp,cpp}` adds four script methods to `WeatherSystem`:

  | Method | Notes |
  |---|---|
  | `SetWeather(CName weather, opt blendTime: Float, opt priority: Uint32) -> Bool` | Calls `RuntimeSystemWeather::SetWeatherByName(system, weather, "", blend, 0, priority)` and sets `CycleWeather=false` |
  | `ResetWeather(opt forceRestore: Bool, opt blendTime: Float) -> Bool` | |
  | `GetEnvironmentDefinition() -> ref<worldEnvironmentDefinition>` | |
  | `GetWeatherState() -> ref<worldWeatherState>` | |

  - Internals (`src/Red/RuntimeScene.hpp`):
    - `WeatherScriptInterface + 0x40` → `worldRuntimeSystemWeather*`
    - `RuntimeSystemWeather`: `CycleWeather`@0x8C (bool), `CurrentStateIndex`@0x90, `CurrentSource`@0x98 (CName), `PreviousStateIndex`@0xB8
  - Address IDs (`src/Red/Addresses/Library.hpp`): `RuntimeSystemWeather_GetEnvironmentDefinition = 1471948702`, `RuntimeSystemWeather_GetWeatherState = 2679119876`, `RuntimeSystemWeather_SetWeatherByName = 2334794340`, `RuntimeSystemWeather_SetWeatherByIndex = 1821516328`, `PhysicsTraceResult_GetHitObject = 2394822845`.
  - Wiki usage: `GameInstance.GetWeatherSystem(GetGameInstance()).SetWeather(n"24h_weather_rain", 10.0, 5); ... ResetWeather(true);`
  - Sources:
    - https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/src/App/World/WeatherSystemEx.cpp
    - https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/src/Red/RuntimeScene.hpp
    - https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/src/Red/Addresses/Library.hpp
    - https://github.com/psiberx/cp2077-codeware/wiki
- **Physics.** Codeware has only `TraceResultEx` (`Raw::PhysicsTraceResult::ResultID` @0x48, `GetHitObject`). It adds no force or impulse API.
  - `scripts/Base/Addons/ColliderComponent.reds` exposes native fields: `colliders`, `simulationType`, `startInactive`, `useCCD`, `massOverride`, `volume`, `mass`, `inertia`, `comOffset`, `filterData`, `isEnabled`, `dynamicTrafficSetting`.
  - `VehicleObject.reds` adds only `archetype` and `isVehicleOnStateLocked`. `Raw::Vehicle::RecordID` is @0x638.
  - https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/scripts/Base/Addons/ColliderComponent.reds
  - https://raw.githubusercontent.com/psiberx/cp2077-codeware/main/src/Red/Vehicle.hpp
- **Reflection** (wiki): `Reflection.GetClass(n"X")`, `cls.GetProperty(n"p").GetValue(obj)` / `SetValue`, `cls.GetFunction(n"f").Call(obj, [args])`. This works for reading `worldWeatherState` and `WindAreaSettings` from script, but evaluating `CurveData` from script is likely impossible [UNVERIFIED].

---

## 5. Vanilla scripts, 2.31 decompile (codeberg adamsmasher/cyberpunk)

The repo's latest commit is "2.31 (#27)" on 2025-11-12, so these signatures are current.
Repo: https://codeberg.org/adamsmasher/cyberpunk. Native declarations are in `orphans.swift`; line numbers come from repo search.

- **`public native class PhysicalBodyInterface extends IScriptable`** (orphans.swift ~L35919–35940). Methods seen:
  - `GetBodyIndex() -> Int32`
  - `IsKinematic() -> Bool`, `IsQueryable() -> Bool`
  - `ToggleKinematic(flag: Bool)`
  - **`AddLinearImpulse(impulse: Vector4, originInCOM: Bool, opt offset: Vector4) -> Void`** (L35935)
  - `SetIsQueryable(enable: Bool)`, `SetIsKinematic(enable: Bool)`
  - No `SetLinearVelocity`, `GetLinearVelocity`, `AddAngularImpulse` or `GetCenterOfMass` exists in 2.31 scripts (searched).
- **Factories**:
  - `ColliderComponent` (`public final importonly class ColliderComponent extends IPlacedComponent`, L33746): `CreatePhysicalBodyInterface(opt bodyIndex: Uint32) -> ref<PhysicalBodyInterface>` (L33749)
  - `PhysicalMeshComponent extends MeshComponent` (L35880): `CreatePhysicalBodyInterface(opt bodyIndex: Int32)` (L35882)
  - Vanilla usage: `m_physicalMeshes[i].CreatePhysicalBodyInterface().SetIsKinematic(false); ...AddLinearImpulse(new Vector4(0.0, -100.0, 0.0, 0.0), false);` in `cyberpunk/devices/disposal/disposalDevice.swift` L358–359. Similar code is in `activatedDeviceTrapDestruction.swift`, `baseDestructibleDevice.swift` and `trafficLight.swift`.
  - `vehicleChassisComponent` extends `IPlacedComponent`, not `ColliderComponent`, so vehicles do not get a `PhysicalBodyInterface` this way [UNVERIFIED in-game].
- **`public importonly class PhysicalImpulseEvent extends Event`** (L46864). Fields: `bodyIndex: Uint32` (L46866) and `worldImpulse: Vector3` (L46870). Usage shows `worldPosition: Vector3` and `radius: Float`.
  - Vanilla use on a vehicle (`core/components/scriptComponents/motorcycleComponent.swift` ~L96–121, also `cyberpunk/player/psm/vehicleTransition.swift` ~L2858–2878):
    ```
    bikeImpulseEvent = new PhysicalImpulseEvent();
    bikeImpulseEvent.radius = 1.00;
    bikeImpulseEvent.worldPosition.X/Y/Z = vehiclePos (+0.5 Z);
    tempVec4 = WorldTransform.GetRight(vehicle.GetWorldTransform()) * (vehicle.GetTotalMass() * 3.80);
    bikeImpulseEvent.worldImpulse = Vector4.Vector4To3(tempVec4);
    this.GetVehicle().QueueEvent(bikeImpulseEvent);
    ```
    This is a **script-only way to push a vehicle**. The impulse is mass × Δv, so 3.8 m/s here. The event is processed asynchronously through the entity event queue, so per-frame cost and latency are untested [UNVERIFIED].
  - The RED4ext SDK has no `Generated/PhysicalImpulseEvent.hpp` at the root, `ent/` or `game/`; the native RTTI name is unknown.
- **`VehicleObject` natives** (`core/gameplay/vehicles.swift`):
  - `PhysicsWakeUp()`, `IsInTrafficPhysicsState() -> Bool`, `GetLinearVelocity() -> Vector4` (L240), `GetTotalMass() -> Float`
  - `AddCollisionForce(force: Vector4)`, `GetCollisionForce() -> Vector4`
  - `IsInAir()`, `IsLeaningOnOneWheel()`, `IsFlippedOver()`, `IsSkidding(wheelSlipThreshold: Float)`
  - `EnableAirControl(toggle: Bool)`, `IsAirControlEnabled()`, `DetermineCoolExitImpulseLevel(...)`
  - `public native class AVObject extends VehicleObject {}` adds nothing.
  - Not present: `GetAngularVelocity`, `ApplyImpulse` (LTBF adds `GetAngularVelocity`).
- **Ragdoll**: `core/animation/animCommunication.swift` L30 has a static helper `CreateRagdollApplyImpulseEvent(worldPos: Vector4, imuplseVal: Vector4, influenceRadius: Float)` (sic). It builds `RagdollApplyImpulseEvent` (fields `worldImpulsePos`, `worldImpulseValue`: Vector4 at orphans L30063/30065; `influenceRadius`). Send it with `QueueEvent` to an NPC.
- **Weather**: `GameInstance.GetWeatherSystem(self) -> ref<WeatherSystem>` (L11517). `public importonly class WeatherSystem extends IScriptable` (L43599) has `GetRainIntensity() -> Float` (L43609) and `GetRainIntensityType() -> worldRainIntensity` (L43611). Vanilla has no `SetWeather` or `GetWeatherState` in the script dump; Codeware adds them. **No wind API anywhere in vanilla scripts.**
- **Drones** (`core/components/scriptComponents/droneComponent.swift`, `DroneComponent extends ScriptableComponent`):
  - Flying NPC drones are `NPCPuppet`s moved by **animation-driven locomotion** (`AnimationControllerComponent.SetAnimWrapperWeight(owner, n"DroneLocomotion_" + movementType, 1.0)`, `SendStaticDataToAnimgraph()` with mass, size and tilt coefficients). They are not rigid bodies.
  - On death: `UncontrolledMovementStartEvent { ragdollOnCollision = true; ragdollNoGroundThreshold = -1.0 }`, then `ReenableColliderEvent` after 0.2 s.
  - Wind on live drones therefore needs movement-component/AI offsets or an animgraph tilt, not forces.

---

## 6. NativeDB and other mods

- **NativeDB** (https://nativedb.red4ext.com) is a JavaScript single-page app. Direct class URLs return 404 to non-browser fetches, so I took signatures from the 2.31 script dump instead.
- **Other mods**: searches for other flying-vehicle or drone RED4ext mods with physics code found nothing beyond LTBF. Its own modes include `FlightModeDrone`, `FlightModeDroneAntiGravity`, `FlightModeHover`, `FlightModeFly` and `FlightModeAutomatic`; see `src/redscript/Flight/`.
  - https://api.github.com/repos/jackhumbert/let_there_be_flight/contents/src/redscript/Flight
- **No public community RE of NvCloth wind input** was found. Cloth, vegetation and particle wind are presumably driven by the render-side environment (`WindAreaSettings` in environment area parameters) [UNVERIFIED].

---

## 7. Staleness checklist

| Item | Status |
|---|---|
| LTBF hash IDs (611586815, 3303544265, 2526549425, 3281786499, 1414536155, 2879787320, 3191280029, 1989218322, 2268865059, 3489929719, 1695226878, 2793083181, 2463305925) | CURRENT (2.30/2.31 builds) |
| `physics::ProxyManager` hash 37956006, BaseProxy VFT 1619396603 | Probably current (hash-based), unverified |
| Any `1.52 RVA` / `1.6 RVA` / `1.61hf1 RVA` and `@pattern` in Jack's headers | STALE: do not use on 2.31 without re-finding |
| `PhysicsData` field layout (force/torque/velocity/angVel/orientation/inverseMass) | Used live by LTBF on 2.31: good |
| `PhysicsData` tail (masses, `centerOfMass`, `unk1B0`) | Used by LTBF script natives; not individually asserted |
| `BaseObject.physics`@0x2C8 asserted; `physicsData`@0x2D0 | 2.3-era fork ("update vehicle stuff", 2025-07-17) |
| `WheeledPhysics` size 0xDE0, `insert2`@0x5E0 | Asserted in 2.3-era fork |
| Generated reflection headers (WindAreaSettings, WeatherState, ColliderComponent, PhysicalMeshComponent, SystemBody, StateValue) | Reflection-dumped; current with SDK master |
| Codeware RuntimeSystemWeather offsets 0x8C/0x90/0x98/0xB8, `WeatherScriptInterface`+0x40 | Maintained by psiberx; check against your installed Codeware version |
| Units of `PhysicsData::force` (impulse vs force) | UNVERIFIED; calibrate |
