# Cyberpunk Wind Framework: physics engine RE findings

Thread 1 of the plan. Static analysis of the installed game plus the installed modlist, done on 2026-10-01.
Nothing in the game install was modified; every step below only read files.

- **Game:** Cyberpunk 2077 **2.31**, exe file version 3.0.80.51928 (linker timestamp 2025-08-27), Steam, `G:\SteamLibrary\steamapps\common\Cyberpunk 2077`.
- **Framework mods present:** RED4ext 1.30.0, Cyber Engine Tweaks 1.37.1, Codeware 1.20.5, redscript, TweakXL 1.11.4, ArchiveXL 1.27.3, RedHotTools, RedFunctions, REDscope, RedProfiler.
- **RE tools present:** Cheat Engine 7.5, Python 3.11, Java 25, Visual Studio 2019 (MSVC 14.29) and VS 18, .NET 8/9.
  - **Not present:** IDA, Ghidra, x64dbg, ReClass, Binary Ninja, git, WolvenKit.
  - **Installed for this work (with Omar's OK, 2026-10-01):** the Python packages `capstone` 5.0.9 (x86-64 disassembler) and `pefile`.
  - The function-level findings come from a small toolkit written for this job (`tools/re/`, see section 9): string xrefs, `.pdata` function bounds, address-library hashes and capstone disassembly. Items still marked **[needs disassembly]** need a deeper pass, such as vtable or indirect-call tracing.

Confidence tags used below:
- **[verified]**: read directly from the binary, the RTTI dump, or code that runs in game today.
- **[inferred]**: follows from the evidence but was not observed at runtime.
- **[needs runtime check]** or **[needs disassembly]**: an open item, with the check to run.

---

## 1. Headline findings

1. **The physics engine is NVIDIA PhysX 3.4.2, not Havok.** [verified]
   - It ships as separate DLLs in `bin\x64`: `PhysX3_x64.dll` (file version 3.4.2.0), `PhysX3Common_x64.dll`, `PhysX3Cooking_x64.dll`, `PhysX3CharacterKinematic_x64.dll`, `PxFoundation_x64.dll` and `PxPvdSDK_x64.dll`.
   - The exe has zero Havok strings. It does have CDPR's PhysX build paths (`d:\r6.main\dev\external\physx342\...`).
   - Cloth is **NvCloth 1.1.5**, statically linked into the exe (`z:\...\nvcloth_1_1_5\...`, and the symbol string `cloth::SwSolverKernel::applyWind`).
   - The player and NPCs move on a PhysX character controller (`PxCreateControllerManager` is imported from `PhysX3CharacterKinematic`). They are not rigid bodies.
2. **The game already has a wind model, but only for visuals.** [verified for the data, inferred for the consumers]
   - Each weather state's environment parameters can carry a `WindAreaSettings { strength: CurveDataFloat; direction: CurveDataVector4 }` (section 5).
   - It drives foliage (SpeedTree wind, render "AdvanceWind"), cloth (NvCloth `applyWind`, `worldClothMeshNode.affectedByWind`), particles (`windInfluence`), water (`CRenderSimWaterFFT.windDir/windSpeed`) and volumetric clouds (`vol*WindInfluence`).
   - **Rigid bodies, vehicles, ragdolls and the player get no wind force at all.** No string, RTTI property or TweakDB field ties wind to a rigid-body force. The framework has to add it.
3. **There are three working ways to push things today, all usable from redscript with no native code:** [verified, used by installed mods]
   - `PhysicalImpulseEvent` queued on an entity, which works on vehicles (Nitrous, Time Dilation Overhaul) and on physics props.
   - `PhysicalBodyInterface.AddLinearImpulse` from `ColliderComponent` / `PhysicalSkinnedMeshComponent.CreatePhysicalBodyInterface()` (MNC's physics spike).
   - `RagdollApplyImpulseEvent` (`CreateRagdollApplyImpulseEvent(pos, impulse, radius)`) for ragdolls, and `PSMImpulse` for the player's locomotion.
4. **The game ships a symbol-hash address library** (`bin\x64\cyberpunk2077_addresses.json`, 987,479 entries, 84 with names). Every function has a stable hash that RED4ext resolves per patch. This is the version-robust way to name a native hook (section 8).
5. **MNC's drones are simulated in script (`CMFlight`), not by PhysX.** They already read wind from `CMWind.At()`. The drone integration is a drop-in replacement of that query by the framework's wind field (section 10).
6. **Cars have their own drag function, and it is the best hook for wind on vehicles.** `WheeledPhysics::ApplyAirResistance` (hash 3489929719) computes `F = -1.2 · airResistanceFactor · |v|² · v̂` and adds `F·dt` to the car's own rigid body. Feeding it `v - wind` makes every car feel wind with its vanilla tuning (section 4.1). [verified by disassembly]

---

## 2. Frame and physics pipeline

### 2.1 Update tick groups [verified]

These are the `UpdateTickGroup` names in the exe, in table order. A RED4ext game system can register a callback on any of them through `IGameSystem::OnRegisterUpdates`.

| Group | Use for wind |
|---|---|
| `EntityUpdateState`, `Entities_PreTick` | too early |
| `UpdateTransformPrePhysics` | |
| **`PrePhysicsTick`** | **apply wind forces and impulses here, once per frame, before PhysX steps** |
| `PhysicsExecuteAsyncQueries`, `PhysicsFlushBufferedState` | buffered body writes are flushed into PhysX here [inferred] |
| `PostPhysicsSyncResults`, `UpdateTransformPostPhysics` | |
| `PostPhysicsTick` | read back velocities for the next frame's drag |
| `AnimationUpdate`, `PlayerAimUpdate`, `CameraUpdate`, `Entities_PostTick`, ... | |

Scripts have no tick-group access. Redscript systems tick from `DelayCallback(…, 0.0)` or a per-frame event, which lands outside the physics step (MNC does this today).

### 2.2 Physics step markers [verified]

The profiler marker table built by the function with hash **1028072074** (RVA `011F3A28` in 2.31) names the physics step stages in this order:

```
Physics/UpdateInBucket
Physics/ExecuteAsyncQueries
CollisionNodeManager/TickKick
CollisionNodeManager/TickFinish
ApplyWind                         <- the engine's own wind pass (cloth/visual), right before:
Physics/KickAndWaitSimulation     <- PxScene::simulate / fetchResults
Physics/PostSimulate, PhysX/PostSimulationFlush, NvCloth Begin Simulate, NvCloth_SimulateChunk
```

`ApplyWind` running just before the simulation kick is the natural place for a native wind pass, and it is the one the engine itself uses. Finding the function that opens the `ApplyWind` scope needs a disassembler [needs disassembly]: the marker objects are referenced through registered pointers, not direct references.

---

## 3. Rigid bodies: what scripts and natives can reach

### 3.1 RTTI layer (from Codeware's full type dump, 6,822 types) [verified]

- `physicsSystemBody` (`physicsISystemObject`): `params: physicsSystemBodyParams`, `localToModel`, `collisionShapes: array<ref<physicsICollider>>`, `mappedBoneName`, `isQueryBodyOnly`.
- `physicsSystemBodyParams`: `simulationType`, `linearDamping`, `angularDamping`, `solverIterationsCountPosition/Velocity`, `maxDepenetrationVelocity`, `maxAngularVelocity`, `maxContactImpulse`, **`mass`**, **`inertia: Vector3`**, **`comOffset: Transform`**.
  - This is everything a drag model needs (mass, inertia, centre of mass). It is the authored value in `physicsSystemResource` (`.phys`).
- `physicsSimulationType`: `Static`, `Dynamic`, `Kinematic`, `Invalid`.
- `physicsProxyType`: `PhysicalSystem`, `CharacterController`, `Destruction`, `ParticleSystem`, `Trigger`, `Cloth`, `WorldCollision`, `Terrain`, `SimpleCollider`, `AggregateSystem`, `CharacterObstacle`, `Ragdoll`, `FoliageDestruction`.
- `physicsPhysicalSystemOwner` lists every owner of a PhysX body. This is the full list of things wind could act on:
  - world nodes: `BakedDestructionNode`, `ClothMeshNode`, `CollisionAreaNode`, `DecorationMeshNode`, `DynamicMeshNode`, `InstancedDestructibleNode`, `PhysicalDestructionNode`, `PhysicalTriggerNode`, `StaticMeshNode`, `TerrainCollisionNode`, `WaterPatchNode`, `WorldCollisionNode`, `FoliageDestruction`;
  - components: `BakedDestructionComponent`, `ClothComponent`, `ColliderComponent`, `PhysicalDestructionComponent`, `PhysicalMeshComponent`, `PhysicalSkinnedMeshComponent`, `PhysicalTriggerComponent`, `SimpleColliderComponent`, `SkinnedClothComponent`, `StateMachineComponent`, **`VehicleChassisComponent`**, `PhysicalParticleSystem`, `RagdollBinder`, `EntityProxy`, `PhotoModeSystem`.
  - **Important for props:** world-node bodies (`DecorationMeshNode`, `DynamicMeshNode`) have no entity, so script events cannot reach them. VAXIS's Physics Overhaul turns about 3,370 decoration meshes dynamic, so most loose props in this modlist are of this kind. Only the native layer (section 7) can push them.
- `vehicleChassisComponent` (`IPlacedComponent`): `collisionResource: rRef<physicsSystemResource>`, `optionalPlayerOnlyCollisionResource`.

### 3.2 `PhysicalBodyInterface` (script class, native methods) [verified names; signatures partly verified]

Natives registered on the class, from the exe's registration table: `SetIsQueryable`, `SetQueryFilter`, `SetSimulationFilter`, `SetIsKinematic`, `SetIsSleeping`, **`SetMass`**, **`AddLinearImpulse`**, `SetDisplacement`, **`SetAngularVelocity`**, **`SetLinearVelocity`**, `SetTransform`, `ToggleKinematic`, `IsQueryable`, **`GetMass`**, **`GetLocalCenterOfMass`**, `GetBoundsCenter`, `GetDimensions`, `GetBounds`, `GetDisplacement`, `GetLinearSpeed`, **`GetAngularVelocity`**, **`GetLinearVelocity`**, `GetTransform`, `GetBodyIndex`.

- The vanilla 2.31 scripts declare only `AddLinearImpulse(impulse: Vector4, originInCOM: Bool, opt offset: Vector4)`, `IsSimulated`, `IsKinematic`, `IsQueryable`, `SetIsKinematic`, `ToggleKinematic`, `SetIsQueryable`, `GetBodyIndex`, `GetTransform` and `SetTransform`. Sources: the decompiled scripts and MNC's compiler probe at `CMPhysSpike.reds:13`.
- The rest exist in RTTI but are not declared to redscript. **[needs runtime check]** In CET, call `body:GetLinearVelocity()`, `body:GetMass()` and `body:GetLocalCenterOfMass()` on a body from `CreatePhysicalBodyInterface()`. CET binds every RTTI function, so this confirms each signature. A RED4ext plugin can then expose them to redscript.
- Obtained from: `ColliderComponent.CreatePhysicalBodyInterface()` (proven by MNC) and `PhysicalSkinnedMeshComponent.CreatePhysicalBodyInterface()`. `PhysicalMeshComponent` likely has it too [needs runtime check].

**How the natives work** (capstone disassembly of 2.31) [verified]:

- `PhysicalBodyInterface` holds a physics **proxy ID** at `this+0x40` and a **body index** at `this+0x44`.
- Each setter does the same three steps:
  1. Lock the proxy: hash **1210388152** (`002405EC`), through the global physics proxy registry at `03467A50` (hash **37956006**).
  2. Write one field of the body's **buffered state**: hash **1403400784** (`00240998`), args `(proxyId, …, bodyIndex, stateId, data*, size, …)`.
  3. Unlock: hash **200045** (`00240720`).
- The buffered state is flushed into PhysX in the `PhysicsFlushBufferedState` tick group [inferred from the group name]. When no buffer is attached, the write goes straight to the proxy through `proxy->vtbl[0x58/8]` [verified].
- Getters read through hash **3901166127** (`002406A8`).

Buffered-state IDs, read from the thunks' immediates:

| State ID | Size | Written by |
|---|---|---|
| `0x04` | 12 (Vector3) | `SetLinearVelocity` |
| `0x05` | 12 | `SetAngularVelocity` |
| `0x09` | 12 | `SetDisplacement` |
| `0x0C` | 32 | **`AddLinearImpulse`** (pending impulse accumulator) |
| `0x0D` | 1 | `SetIsSleeping` |
| `0x15` | 1 | `SetIsKinematic`, `ToggleKinematic` |
| `0x20` | 16 | `SetSimulationFilter` |
| `0x21` | 16 | `SetQueryFilter` |
| `0x29` | 1 | `SetIsQueryable` |

**`AddLinearImpulse(impulse: Vector4, localToCoM: Bool, point: Vector4)`** [verified from code; the parameter names are mine]:

- The engine function is hash **827464466** (`008E74FC`). It reads the pending accumulator (state `0x0C`, via hash **2678271271**), adds the impulse, and writes the slot back.
- The slot is `{ Σ(w·point): Vector3, Σ impulse: Vector3, Σ w: Float, … }` with `w = |impulse|²`. So several impulses in one frame merge into one impulse at a magnitude-weighted point, and that point gives the torque.
- When the Bool is **true**, `point` is an offset from the body's **centre of mass** in body space. The code reads the transform (hash 391385890) and the local centre of mass (hash 3158578683) and transforms to world. When it is **false**, `point` is a world position.
- Consequences [inferred]:
  - An impulse is cheap, deferred, and applied by the engine at its flush.
  - A wind "force" is `F * dt` per frame. Several wind contributions (drag, gust, lift) can be added separately in one frame and they merge correctly.
  - It doesn't fight `SetLinearVelocity` from the game's own logic, because it is a separate state slot.

Native thunks (2.31 RVAs; **the hashes are the version-robust names**). "Engine fn" is what the thunk calls between lock and unlock.

| Native | Thunk hash | Engine fn hash |
|---|---|---|
| `AddLinearImpulse` | **789586891** | **827464466** (accumulator, above) |
| `SetLinearVelocity` | 1720329310 | 1403400784 (state `0x04`) |
| `SetAngularVelocity` | 2443584717 | 1403400784 (state `0x05`) |
| `SetMass` | 3814858824 | 2719813719 |
| `SetTransform` | 2845647472 | 924129163 |
| `SetIsKinematic` / `ToggleKinematic` | 3827376901 / 305079167 | 1403400784 (state `0x15`) |
| `SetIsSleeping` | 3226018471 | 1403400784 (state `0x0D`) |
| `GetLinearVelocity` | 1645618258 | 1360335866 |
| `GetAngularVelocity` | 2368087233 | 1763775593 |
| `GetMass`, `GetLinearSpeed` | 3748012092, 3734774516 | 1689001670 |
| `GetLocalCenterOfMass` | 3287950653 | 3158578683 |
| `GetTransform` | 2774868580 | 391385890 |
| `GetBounds` / `GetBoundsCenter` / `GetDimensions` | 728703251 / 229712756 / 3358204609 | 1696796478 / 582488860 / 4236317289 |
| `IsSimulated` | 2050564588 | 3327270886 |
| `CreatePhysicalBodyInterface` | registered on **4 component classes** (thunks 1127754533, 108669017, 1392061280, 1495475901) | which 4 classes: [needs runtime check, the probe lists them] |

### 3.3 Script events [verified]

| Event | Fields | Works on | Used by |
|---|---|---|---|
| `PhysicalImpulseEvent` (`enteventsPhysicalImpulseEvent`) | `worldPosition: Vector3`, `worldImpulse: Vector3`, `radius`, `bodyIndex`, `shapeIndex` | any entity whose physical component listens, **including vehicles** | Nitrous (`nitro.reds:1788`), Time Dilation Overhaul (`Herbie.reds:173`) |
| `RagdollApplyImpulseEvent` | `worldImpulsePos`, `worldImpulseValue`, `influenceRadius`; built by `CreateRagdollApplyImpulseEvent(pos, impulse, radius)` | ragdolled NPCs | Explosion Knockback, Reduced Ragdoll Force, Zeusico quickhacks |
| `PSMImpulse` | `id: CName`, `impulse: Vector4` | the player's locomotion state machine | Reflex Engine (`slide.lua:231`) |
| `ForceRagdollEvent` (`CreateForceRagdollEvent(reason)`) | | NPCs | several |

`PhysicalImpulseEvent` with an off-centre `worldPosition` gives torque (TDO applies equal and opposite impulses fore and aft to yaw a car). That is enough to model weathervaning and crosswind roll.

**Runtime confirmation from MNC** (MNC Physics v2, 2.31, a 20 kg proxy box, 1,463 frames):
- **Writes work as described:** Lock (1210388152) → Write (1403400784) → Unlock (200045), with state 5 (AngularVelocity) and state 4 (LinearVelocity).
  - A P-controller on angular velocity held a 20.0° lean with 0.00° average error.
  - A height hold through state 4 sagged a steady 5.6 cm. That is g·dt acting after the write, ≈ g·dt / gain.
- **The getters return the buffered value written this frame, not the post-step velocity.** GetLinearVelocity (1360335866) read vz 0.2 while the measured vz was 0, and GetAngularVelocity (1763775593) read 0 at equilibrium. Measure velocity from pose deltas instead.

### 3.5 From a body handle to its `PxRigidDynamic` [verified statically, then in game by MNC Physics v3, 2026-10-01]

**In game (MNC Physics v3, 2.31, a 20 kg entColliderComponent box, 1,200+ physics steps):**
- **The resolve works:** the raw proxy-table lookup (generation check, vtable 4234546548, actors at +0x50 and count at +0x5C, `is<PxRigidDynamic>`) succeeded on every step.
- **Gravity off holds:** `setActorFlag(eDISABLE_GRAVITY)` was never restored by the engine.
- **Forces work:** `addForce(eFORCE)` inside the simulate/collide detour held height with 0.000 m error.
- **Scenes:** `PxGetPhysics` reports **7 scenes** sharing one NpScene vtable. Night City Winds' and MNC's vtable patches chain cleanly, whichever patches first.

1. **ProxyManager:** hash 37956006 is a pointer variable (RVA `03467A50`); dereference it to get `mgr`.
2. **Proxy from proxyId:** hash **4224462602** `ProxyRef* GetProxy(mgr, ProxyRef* out{proxy, refBlock}, u32 proxyId)`; release with hash **919150295**.
   - The id is `index | generation << 16`, checked against a u16 table at `mgr+0x102010`.
   - Raw layout: `proxy = *(void**)(mgr + 0x2018 + index*16)`.
3. **Physical-system proxy:** vtable hash **4234546548** (RVA `02B0A2E0`). Its slot 11 (+0x58) is the state writer, hash 594155312.
   - **`proxy+0x50` = `PxRigidActor**` (one per body), `proxy+0x5C` = body count.**
   - The writer indexes the array by bodyIndex and calls PhysX virtuals on the actor directly. `actor->isKindOf("PxRigidBody")` is hash 4101641921.
4. **ImpulseAccumulator (state 12) apply:** skips kinematic actors, takes the weighted point `Σ(w·p)/Σw`, and applies one `eIMPULSE` at it through hash **2696552864**, a statically linked `PxRigidBodyExt::addForceAtPos`-style helper. So `AddLinearImpulse` does produce torque, but impulses in the same frame merge, and equal and opposite ones cancel.
5. **PxRigidDynamic vtable** (confirmed in PhysX3_x64.dll: slot 5 `getType` returns 1 for NpRigidDynamic and 0 for NpRigidStatic; `userData` is at +0x10, name at +0x18):

| Method | Offset | Method | Offset |
|---|---|---|---|
| setActorFlag | +0x50 | getActorFlags | +0x60 |
| getGlobalPose | +0xA0 | setGlobalPose | +0xA8 |
| setMass | +0xF8 | getMass | +0x100 |
| getLinearVelocity | +0x128 | setLinearVelocity | +0x130 |
| getAngularVelocity | +0x138 | setAngularVelocity | +0x140 |
| addForce | +0x148 | addTorque | +0x150 |
| setRigidBodyFlag | +0x168 | getRigidBodyFlags | +0x178 |
| isSleeping | +0x1F0 | wakeUp | +0x240 |

   - A naive count of the 3.4 headers gives an extra slot, because PxActor's `isKindOf` override reuses PxBase's slot 4.
   - Gravity off: `setActorFlag(eDISABLE_GRAVITY=2, true)`.

### 3.4 Native impulse volumes (candidate ready-made wind zones) [verified types; behaviour needs runtime check]

- `entPhysicalImpulseAreaComponent` extends `PhysicalTriggerComponent` (so it has a `shape: physicsTriggerShape`, `filterData` and `simulationType`). It adds `impulse: Vector3` and `impulseRadius: Float`.
- `worldPhysicalImpulseAreaNode` is the same as a world node. Its debug draw is `Physics/Areas/TriggerAreas/ImpulseArea`.
- Codeware's `Entity.AddComponent(component)` can attach one to a spawned entity at runtime. Whether it pushes bodies once on entering or every frame decides if it can serve as a wind volume for world props without native code.
- Related: `gameEffectExecutor_PhysicalImpulseFromInstigator(_Value) { magnitude, forceUseHitPosition }`, an effect-system impulse used by weapons and explosions. Props also have `turnDynamicOnImpulse` (static until hit), which matters for VAXIS props: wind alone may not wake them.

---

## 4. Vehicles

### 4.1 The vanilla air-drag function: the best vehicle wind hook [verified by disassembly]

`vehicle::WheeledPhysics::ApplyAirResistance(const Vector3& velocity, float dt)` is hash **3489929719** (`008B469C` in 2.31). Its symbol name comes from Let There Be Flight's comments (section 11).

```
if |v|² > 0.01 and <global enable flag, on by default> {     // flag byte at 032FE130 = 1
    dir = normalize(v)
    F   = -1.2 * airResistanceFactor * |v|² * dir             // 1.2 = air density (kg/m³)
    physicsData.force += F * dt                               // physicsData = this->vehicle(+0x60)->physicsData(+0x2D0), force @+0x00
}
```

- `airResistanceFactor` is read from `this + 0xC90`. It is the TweakDB `VehicleDriveModelData.airResistanceFactor`, so it plays the role of ½·Cd·A in m².
- The global flag is almost certainly the `EnableAirResistance` vehicle config switch [inferred].
- **There is one caller**: `WheelSuspensionBase::ApplyAllResistances(float dt)`, hash **2526549425**.
  - It reads `velocity` from `physicsData + 0x18`.
  - It skips both air and low-speed resistance at 100 m/s and above (|v|² ≥ 10000) or when the speed isn't finite.
  - Otherwise it calls `ApplyAirResistance(&v, dt)` and then `ApplyLowSpeedResistances(&v, dt)` (hash 1695226878).
- **Units settled:** vanilla writes `F * dt` into `physicsData.force`. So that field accumulates **impulse (N·s) for the current step**, which answers the open question in LTBF's notes.

**Wind for cars and bikes with one detour:** hook `ApplyAirResistance` and call the original with `v_air = v - w(position)` in place of `v`.
- The game's own drag, with each car's own tuned factor, then acts on airspeed. That gives headwind, tailwind and crosswind push with no new tuning.
- Rolling resistance keeps using ground speed, because `ApplyLowSpeedResistances` is called separately with the untouched `v`.
- What it doesn't give: side-area differences (drag is isotropic here) and moments. For crosswind yaw and roll, add a lateral `F*dt` at the centre of pressure through `vehicle::RigidBody::ApplyImpulse(offset, impulse)` (hash **611586815**, which adds the force and computes the torque) or `ApplyAngularImpulse` (hash **3303544265**).
- Because the factor is isotropic and the threshold is 100 m/s, cars going over 360 km/h get no wind. That's fine.

### 4.2 Vehicle physics structure

- **Not PhysX Vehicle SDK.** The exe has no `PxVehicle*` strings. The tyre, suspension and drivetrain model is CDPR's own (`Vehicle/Suspension`, `Vehicle/Physics` markers; the curves `long_speed_diff_to_traction`, `lat_to_long_speed_coeff`, ...). [verified]
- **The chassis is integrated by CDPR's own `vehicle::RigidBody`**, not by PhysX: `vehicle::PhysicsData` with `force`@0x00, `torque`@0x0C, `velocity`@0x18, `angularVelocity`@0x24, `orientation`@0x30, `inverseMass`@0x40, then inertia tensors, gravity flag, masses and centre of mass. LTBF's struct, and the drag function above, read it the same way.
  - The PhysX actor for the chassis is therefore probably kinematic or driven [inferred]. **A `PxScene::simulate` hook would not move cars.** Vehicles must take the path in 4.1.
- **Other vanilla aero data:** TweakDB `VehicleDriveModelData_Record` (`airResistanceFactor`, `momentOfInertia`, `momentOfInertiaScale`), `DynamicDownforceHelper_Record`, `VehicleAirControl_Record` / `VehicleAirControlAxis_Record` (airborne control), and `BikeDriveModelData` / `TankDriveModelData`.
  - The engine config group `EnableAirResistance`, `UseDifferential`, `WeightTransferMode`, `EnableSmoothWheelContacts`, `EnableLowVelStoppingResistance`, `VelocitySmoothingTime` sits next to the vehicle physics code.
- **Read access from script** [verified]: `VehicleObject.GetLinearVelocity()` (used by Gone in 2077 Seconds, Much Better AI, TDO), `GetTotalMass()`, `IsInAir()`, `GetWorldForward/Right/Up()`, `GetWorldPosition()`.
- **RED4ext SDK layout** (`vehicleBaseObject`, size `0xBA0`): `isOnGround @0x25C`, `acceleration @0x264`, `deceleration @0x268`, `isReversing @0x2A3`, `burnout @0x2BC`, `archetype @0x3A0`, `isVehicleOnStateLocked @0x6D2`. LTBF adds `physics @0x2C8` and `physicsData @0x2D0`; the second is confirmed by the drag function.

### 4.3 Wind on a car without native code (phase 1 fallback)

```
v_air  = v_vehicle - w(position)
F_wind = 1.2 * k * (|v_vehicle| v_vehicle  -  |v_air| v_air)      k = airResistanceFactor (or a script estimate)
```

- This adds exactly what the 4.1 hook would change: the vanilla drag on airspeed minus the vanilla drag on ground speed.
- Apply `F_wind * dt` each frame as a `PhysicalImpulseEvent` at the centre of pressure, a little above and behind the centre of mass, which gives the yaw and roll moments.
- It works today from redscript. Its drawback is the frame order: script runs outside the physics step, so a native hook is smoother.

---

## 5. Weather and environment (where the game's own wind lives)

[verified for the RTTI; the curve contents per weather need a WolvenKit look at the `.envparam` files]

```
WeatherSystem (gameWeatherSystem, script)
  GetWeatherState() -> worldWeatherState            (Codeware addMethod)
  GetEnvironmentDefinition() -> worldEnvironmentDefinition
  SetWeather(name, blendTime, priority), ResetWeather(...)
worldEnvironmentDefinition
  weatherStates: array<ref<worldWeatherState>>, weatherStateTransitions, areaEnvironmentParameterLayers
worldWeatherState
  name, minDuration, maxDuration, probability, transitionDuration (CurveDataFloat)
  environmentAreaParameters: rRef<worldEnvironmentAreaParameters>
  effect: raRef<worldEffect>
worldEnvironmentAreaParameters
  renderAreaSettings: WorldRenderAreaSettings { areaParameters: array<ref<IAreaSettings>> }
WindAreaSettings extends IAreaSettings
  enable, disabledIndexedProperties
  strength: CurveDataFloat        (curves are keyed by time of day)
  direction: CurveDataVector4
worldWeatherAreaNotifier (trigger-area notifier)
  weatherStateNames, weatherStateValues, horizontalFadeDistance, verticalFadeDistance
```

- `CurveDataFloat` / `CurveDataVector4` expose `GetSize`, `GetPoint` and `GetPointValue` to script through Codeware. So a redscript system can load the current state's `environmentAreaParameters` (Codeware's `ResourceDepot` / `ResourceReference`), find the `WindAreaSettings` entry and sample `strength` and `direction` at the current game hour. The framework's mean wind can then follow the same numbers the trees and cloth use.
- Other wind consumers, which the framework can leave alone or match:
  - `meshMeshParamSpeedTreeWind`, `CSpeedTreeWindDataUpdater`, render `AdvanceWind` (3 functions; hashes 1614685340, 1615739069, 2607819952) and `WindUpdateAndRainMap` (hash 1563505581);
  - `worldMeshNode.windImpulseEnabled`, `CWindImpulseCollector`, `AddWindImpulseShapes` (hash 2650750825) and `Rendering/WindImpulse`, which is local foliage bending;
  - `entDynamicActorRepellingComponent { type: entRepellingType (Debris | BigObjects | WindImpulse | WaterImpulse), shape (Sphere | Capsule), magnitude, bendIntensity, radius, capsuleRadius, capsuleHeight }`. A component with `type = WindImpulse` bends foliage and grass around itself. **A drone or car could carry one to blow grass with its downwash or wake** [inferred, needs runtime check].
  - NvCloth `applyWind` with `PxCloth.DragCoefficient`, `m_dragCoefficient`, `m_liftCoefficient` (cloth params), and `cloth_wind` (3 users).
  - Particles: `windInfluence` per emitter.
- Weather names in the modlist's own scripts (CMWind) are matched by substring today: `sandstorm`, `storm`, `rain`, `pollution`, `toxic`, `fog`, `heavy_cloud`, `cloud`, else clear.
- Installed weather mods (Nova City 2 Weather, Weather Probability Rebalance, Auto Weather Scheduler, Weather Switcher, Disable Quest Weather) change which state is active. They do not change the wind curves, as far as the file lists show.

---

## 6. Player, NPCs, ragdolls, cloth

- **Player:** a PhysX character controller. Wind is a locomotion impulse, not a force:
  - `PSMImpulse` per frame (`impulse = F/m * dt` as a velocity change), with a bigger effect while airborne;
  - or a tweak to air control while falling or gliding (`DefaultTransition.GetLinearVelocity(scriptInterface)` reads it).
  - Ground walking should get at most a gentle drift. A strong push feels like a bug.
- **NPCs alive:** also character controllers. Leave them alone, or only nudge them in extreme storms.
- **Ragdolls:** PhysX articulations or bodies (`RagdollComponent`, `physicsRagdollBodyInfo`, `Components/Ragdoll/CenterOfMass`). `RagdollApplyImpulseEvent` with a large `influenceRadius` pushes the whole body. Throttle it: one impulse every 0.1 to 0.2 s, scaled by dt.
- **Cloth and garments:** already wind-driven by the engine (NvCloth). Garment physics mods (GarmentSupport, the "with physics" jewellery) follow the engine wind. Matching them to the framework's wind needs the native `ApplyWind` hook (section 7.3).

---

## 7. Native (RED4ext) layer options

### 7.1 PhysX directly (most version-robust native route) [verified feasibility; details inferred]

- The game imports `PxCreateBasePhysics` and `PxGetPhysics` from `PhysX3_x64.dll` (IAT RVAs `02A8D7C0` and `02A8D7C8` in 2.31). Both are **named DLL exports**, unchanged by game patches as long as CDPR keeps PhysX 3.4.2.
- A plugin calls `PxGetPhysics()` → `getScenes()` → the scene, and hooks **`PxScene::simulate`** through the vtable. The PhysX 3.4 public headers fix the vtable layout. [verified against the shipped DLL]
  - In `PhysX3_x64.dll` 3.4.2, the `NpScene` vtable is at RVA `0x20CAF0`. Slot 1 `release` is at `0x51D40`, **slot 56 `simulate`** at `0x53760` and slot 61 `fetchResults` at `0x53980`.
  - These indices match PhysX 3.4's `PxScene.h` declaration order exactly (destructor 0, release 1, …, simulate 56, fetchResults 61). Each was confirmed through that function's own error string.
  - So the plugin can be built against the open-source PhysX 3.4 headers and call `PxRigidDynamic::addForce` and the others as ordinary virtual calls. Only the `simulate` detour needs a vtable patch, and that index is fixed by the header, not by a game patch.
- Inside the hook, before calling the original:
  - iterate `getActors(PxActorTypeFlag::eRIGID_DYNAMIC)`;
  - skip kinematic and sleeping-and-sheltered bodies;
  - compute drag from the body's velocity, bounds and mass;
  - `addForce(F, PxForceMode::eFORCE, autowake)` at the centre of mass, or `PxRigidBodyExt::addForceAtPos` for torque.
- That reaches **every PhysX-simulated** dynamic body, including world-node props with no entity, debris and destruction chunks. It does **not** reach cars: their chassis is integrated by `vehicle::RigidBody` (section 4.2), so skip kinematic actors.
- **Classifying a body:** shape `PxFilterData` (the game's collision groups: `Dynamic`, `Vehicle`, `Destructible`, `Debris`, `FoliageDestructible`, `Ragdoll`, ...; RedHotTools has the names). `PxActor::userData` points back to the game's physics proxy [needs disassembly] to tell vehicles and ragdolls apart, so the framework doesn't double-apply where the script layer already handles them.
- **Risks:**
  - The game writes velocities through its own buffered state (section 3.2), so the hook must run after `PhysicsFlushBufferedState` and inside `simulate` [inferred].
  - PhysX is not thread-safe while simulating, but the `simulate` call itself is the safe window.
  - Cost: thousands of actors. Use the shape bounds and a spatial cell grid, and only process actors near the player.

### 7.2 Engine physics proxy (by address hash)

- The simplest native route for any body the game knows by proxy ID: call the script native through RTTI (`PhysicalBodyInterface::AddLinearImpulse`), or call the engine accumulator directly through `UniversalRelocFunc`.
- Lock: hash 1210388152 `(out*, proxyId)` returns an accessor. Add impulse: hash 827464466 `(accessor, {point: Vector3, impulse: Vector3}*, bodyIndex, localToCoM: bool)`. Unlock: hash 200045 `(accessor)`. [verified from the call site in the `AddLinearImpulse` thunk]
- It is cleaner than raw PhysX for entity-owned bodies, because the impulse rides the game's own buffered state and flush.
- What's missing is a way to go from an entity or component to its proxy ID without the script `CreatePhysicalBodyInterface` (thunk hash 1127754533 and siblings show how the game does it) [needs disassembly].

### 7.3 Engine wind (visual)

- Hook the `ApplyWind` stage of the physics step and the render `AdvanceWind` to feed the framework's wind vector into cloth and foliage, so visuals and forces agree.
- Needs disassembly to find where the global wind vector and strength are stored (`CSpeedTreeWindDataUpdater`, `CWindImpulseCollector` are the RTTI handles).

### 7.4 Game system tick

- A RED4ext `IGameSystem` registered on `PrePhysicsTick` runs the framework's per-frame force pass for vehicles and entity props (section 2.1).
- This is also where a redscript-facing `WindSystem` would sample the field once per frame and cache it for scripts.

---

## 8. Version-robust addressing

In order of preference:

1. **RTTI names.** Classes, properties and script natives are found at runtime through `CRTTISystem::Get()->GetClass("PhysicalBodyInterface")->GetFunction("AddLinearImpulse")`, which gives the native's function pointer. They survive patches unless CDPR renames them.
2. **PhysX exports and vtables** (section 7.1). They are stable while PhysX 3.4.2 ships.
3. **Address-library hashes** (`cyberpunk2077_addresses.json`, loaded by RED4ext): `RED4ext::UniversalRelocFunc<Fn>(hash)` or `RelocPtr`.
   - The hash is CDPR's stable ID for a function across patches. The file maps it to that build's RVA.
   - Format: `{"hash": "<u32>", "secondary hash": "<sha256>", "symbol": "<name or empty>", "offset": "<section>:<offset>"}`. Section 1 is `.text` at `+0x1000`.
   - The hashes in this document were mapped from 2.31 RVAs. They should resolve on later patches, but each must be re-checked on update.
4. **Byte patterns.** Last resort, only where no hash exists, such as statically linked NvCloth (`cloth::SwSolverKernel::applyWind` is used in function `01963400` in 2.31 and has no hash).

Named symbols in the address library that matter here: `rtti::Function::InternalCallNative` (2744524784), `rtti::ClassType::FindFunctionByHash` (2381714723), `rtti::NativeMemberFunction::NativeMemberFunction` (1613572595), `UpdatableSystemRegistrar::RegisterInternal` (4254156293), `CBaseEngine::ProcessBaseLoopFrame` (1852772247) and `game::data::GetTweakDB` (914361828).

---

## 9. Tools written for this (in `tools/re/`)

All read-only; they only open files.

| Script | What it does |
|---|---|
| `pe.py` | Minimal PE parser: sections, exports, imports. `python pe.py <dll> exports` |
| `strings.py` | ASCII string dump of the exe with file offsets |
| `addr.py` | `build` turns the address library into `symbols.tsv` (hash, RVA, name); `grep <regex>` searches names |
| `xref.py` | For a string: where it is referenced, the containing function (from `.pdata`) and that function's address-library hash. `--near` shows code pointers loaded next to it (native registrations) |
| `fn.py` | Lists direct calls, string refs and data refs in a function, in order, with callee hashes |
| `parse_cw.py` | Parses Codeware's `Codeware.Global.reds` into a JSON type table |
| `disasm.py` | capstone disassembly of a function, with callee hashes and string refs annotated |
| `callers.py` | Every direct caller of a function, with the instructions leading up to each call (used to tabulate buffered-state IDs) |
| `natives.py` | Script native name → its registration function → native thunk → what the thunk calls, with the immediates it passes |
| `dllxref.py` | The same string → function → vtable-slot trace for any DLL (used on `PhysX3_x64.dll`) |

Run them from `tools/re/`. The first run of `addr.py build` and `xref.py` builds `symbols.tsv` and `riprefs.bin` caches there.

**Still worth adding later:**
- **Ghidra**, for vtable and indirect-call tracing: the `ApplyWind` caller, the global wind storage, and how `PhysicalImpulseEvent` and the impulse area are handled.
- **WolvenKit**, to read the weather `.envparam` wind curves and the vehicle `.phys` masses (the CET probe in section 12 reads the curves at runtime instead).

---

## 10. Recommendation: where to inject wind

| Target | Best injection point | Layer | Why |
|---|---|---|---|
| **MNC drones** | Replace `CMWind.At(p, h)` with the framework's `Wind.At(p, h)`. `CMFlight.wind` already feeds airspeed into drag and blade flapping (`CMFlight.reds:248`) | redscript only | The drones are script-integrated rigid bodies (`CMFlight`), not PhysX. The flight model already does the aerodynamics right; it only needs a better field. No native code. |
| **Vehicles** | **Native (preferred):** detour `WheeledPhysics::ApplyAirResistance` (hash 3489929719) and pass `v - wind`, so the game's own per-car drag acts on airspeed. Add a lateral impulse at the centre of pressure through `RigidBody::ApplyImpulse` (611586815) for crosswind yaw and roll. **Script fallback:** a per-frame `PhysicalImpulseEvent` (section 4.3) | native, with a script fallback | One small, hash-addressed detour reuses every car's tuned `airResistanceFactor`. Rolling resistance stays on ground speed. Cars are integrated by CDPR's `vehicle::RigidBody`, so the PhysX hook doesn't reach them. |
| **Player** | `PSMImpulse` per frame, scaled down on the ground and full in the air | redscript | The player is a character controller, so no rigid-body force applies. This is how existing movement mods push V. |
| **Entity props** (spawned objects, `PhysicalMeshComponent` or `ColliderComponent`) | `PhysicalBodyInterface.AddLinearImpulse(F * dt)` or `PhysicalImpulseEvent`, only within some radius of the player | redscript | Proven in MNC's spike R2/R3. |
| **World props and debris** (decoration and dynamic mesh nodes, destruction chunks, the VAXIS dynamic props) | RED4ext hook of `PxScene::simulate`: drag force on dynamic actors near the player, classified by filter data | native | They have no entity, so this is the only layer that reaches them. Most version-robust native hook (DLL exports and PhysX vtable). |
| **Ragdolls** | Throttled `RagdollApplyImpulseEvent` with a body-wide radius | redscript | Proven by several installed mods |
| **Cloth and foliage** (visual) | Phase 3: drive the engine's own wind (`ApplyWind` and `AdvanceWind`) from the framework's field, or at least align with `WindAreaSettings` | native, after disassembly | Already wind-driven, so only alignment is needed |

**Shared core, which all of the above read:**
- A `WindField` with `At(position, height) -> Vector4`: mean from the weather (the `WindAreaSettings` curves when present, CMWind's table as the fallback), gusts, turbulence, the boundary layer and urban shelter.
- MNC's `CMWind` already has a good model of the field. It should move into the framework as the reference implementation, with Omar's MNC then depending on it.

---

## 11. Community prior art

The full report, with a source URL for every fact and a staleness checklist, is in `docs/PRIOR_ART.md`. What matters for the framework:

- **Let There Be Flight** (jackhumbert, last commit 2026-09-13, targets 2.30/2.31) is the reference for native vehicle forces.
  - A native game system registers `RegisterUpdate(UpdateBucketMask::Vehicle, UpdateBucketStage::PhysicsExecuteAsyncQueries, …)`. Each frame it calls `vehicle::BaseObject::ForceEnablePhysics()` (hash 2793083181), runs script, and adds to `physicsData->force` and `->torque` (`BaseObject+0x2D0`).
  - It is all hash-based. It hooks `RigidBody::ApplyImpulse` 611586815, `ApplyAngularImpulse` 3303544265, `ApplyAllResistances` 2526549425, `FixedUpdate_PostSolve` 3281786499, `DrivingForcesHelper::Apply` 1989218322, `ApplyAntiSwaybarForce` 2268865059 and `ApplyAirResistance` 3489929719.
  - **Every one of these hashes resolves in the installed 2.31 address library** [verified].
  - Its `RED4ext.SDK@new-types` fork has the `vehicle::PhysicsData` and `WheeledPhysics` layouts. Old `1.52/1.6x RVA` and `@pattern` comments in those headers are stale; the hashes are current.
- **The reflected `physicsStateValue` enum** (RED4ext SDK) names the buffered-state IDs found in section 3.2: LinearVelocity=4, AngularVelocity=5, **ImpulseAccumulator=12**, IsSleeping=13, Mass=16, IsKinematic=21, SimulationFilter=32.
  - That matches the immediates read from the disassembly. Hash 37956006 is `physics::ProxyManager::Get()`, the global at `03467A50`.
- **RED4ext addressing:** `UniversalRelocFunc` / `UniversalRelocPtr` **terminate the game if a hash is missing**. For optional features, resolve with the exported `RED4ext_ResolveAddress(hash)` and check for 0. That way a patch that drops a hash disables wind on that layer instead of crashing.
- **Vanilla 2.31 scripts** (decompiled, codeberg adamsmasher/cyberpunk):
  - `PhysicalMeshComponent.CreatePhysicalBodyInterface(opt bodyIndex)` exists alongside `ColliderComponent`'s. Vanilla devices use `AddLinearImpulse(…, false)` (e.g. the disposal device).
  - `VehicleObject` has `GetTotalMass()`, `PhysicsWakeUp()`, `AddCollisionForce(Vector4)`, `IsInAir()` and `EnableAirControl`.
  - `PhysicalImpulseEvent` also has a `radius` field. Vanilla uses it to knock bikes over.
- **Nobody has published RE of how NvCloth or foliage get their wind** in this game, and LTBF is the only physics-driven flight mod found.
- **Vanilla flying drones are animation-driven NPC puppets** until they die. That matches MNC's choice to simulate its drones in script.

---

## 12a. Runtime results (WindProbe, 2026-10-01, Omar in a Chevalier Thrax-based armed car)

- **Classes that hand out a `PhysicalBodyInterface`** (3 of the 4 registrations seen): `entColliderComponent`, `entPhysicalMeshComponent`, `entAppearanceProxyMeshComponent`. A car exposes about 90 of them, including doors, hood, bumpers, wheels, windows and the weapon parts.
- **The car's chassis collider (`entColliderComponent 'ChassisMesh'`) reports `IsSimulated = false`.** This is consistent with section 4.2: PhysX doesn't simulate the chassis, CDPR's `vehicle::RigidBody` does. The detachable parts (bumpers, doors, hood) are PhysX-simulated.
- **The hidden getters (`GetLinearVelocity`, `GetMass`, `GetLocalCenterOfMass`, …) return nil in CET**, although the native code writes a value to the result (disassembly, section 3.2). `GetBodyIndex` and `IsSimulated` work. So CET doesn't marshal those returns, possibly because of how their return type is registered [open]. The framework should read velocity and mass natively (engine getter hashes 1360335866 and 1689001670) or from `vehicle::PhysicsData`, not through these script getters.
- **`PhysicalImpulseEvent` units, settled** (second run): an upward impulse of `mass × 2` (3,400 N·s on a car whose `GetTotalMass()` returns 1,700 kg) gave a peak climb of **exactly 2.000 m/s, two frames later**. So `worldImpulse` is in **N·s**, is applied **once**, and is applied in full. Wind through this route is `F × dt` per frame.
  - A sideways push on a parked car barely moves it (0.005 to 0.04 m/s for 1 m/s worth), because parked tyre grip absorbs it. A crosswind will mostly show while driving.
- **Vanilla wind curves** (`WindAreaSettings` per weather state, read through the redscript helper `r6\scripts\WindProbe\WindProbe.reds`; t is the hour of day):

  | Weather state | strength | direction (x, y) |
  |---|---|---|
  | `24h_weather_sunny`, `_cloudy`, `_light_clouds` | 0.06 constant | (0, 1), toward +Y |
  | `24h_weather_fog` | 0.05 constant | (1, 0), toward +X |
  | `24h_weather_rain`, `_pollution`, `_heavy_clouds` | 14-key daily curve, 0.00 to 0.07 (lows around 07:00 and 22:00) | (0, 1), swinging to (1, -1.02) around 10:40, back by 13:15 |
  | `24h_weather_toxic_rain` | the same shape, raised: 0.07 to 0.14 | same as rain |
  | `24h_weather_sandstorm` | **20** constant | (0, 1) |
  | `sa_courier_clouds` | 0.05 | (1, 0) |
  | `q306_epilogue_cloudy_morning` | 0 | none |
  | `q302_light_rain`, `q306_rainy_night`, `q302_deeb_blue`, `q302_squat_morning` | no wind settings | |

  - Rain and heavy clouds also carry a second, zero-strength `WindAreaSettings` layer.
  - **These are not m/s.** Most are a 0 to 0.14 visual scale for foliage and cloth. Sandstorm's 20 is an outlier, probably tuned for the dust effect.
  - The framework should take its **direction** and **time-of-day shape** from these curves and its **magnitude** from its own m/s table (CMWind's numbers: 3.5 m/s clear, 7 rain, 12 storm, 14 sandstorm, 1.5 fog/smog). Then foliage and physics agree on where the wind comes from and when it picks up.

## 12b. Framework build status (2026-10-01)

- **Plugin** `NightCityWinds.dll` 0.2.0. Source: `F:\2077 Wind Dev\plugin`; `build.ps1` builds with VS 18 + CMake + Ninja and installs into the MO2 mod.
  - `VehicleDrag.cpp`: the `ApplyAirResistance` detour (hash 3489929719), fed `v - wind`. **Verified in game**: a 30 m/s headwind cut the top speed from 358 to 253 km/h, the same 361 km/h airspeed.
  - `PhysXWind.cpp`: patches NpScene vtable slots 56 (simulate) and 58 (collide) once a scene exists (`PxGetPhysics()` → `getScenes`). Before each step it adds drag `0.5·1.2·Cd·A·|v_rel|·v_rel` to non-kinematic dynamics under 800 kg within 50 m of the wind origin. A sleeping body is woken only above 0.15·m·g. Bodies inside `NCW_IgnoreNear` spheres are skipped. **Verified in game** (Omar, 2026-10-01): the plugin log shows the scene hooked at `PhysX3_x64.dll+0x20CAF0`, the predicted vtable address, and props moved downwind under a pinned 30 m/s wind.
  - `Main.cpp`: the natives `NCW_SetWind/GetWind/SetEnabled/IsVehicleHookActive/GetVehicleDragStats/SetWindOrigin/SetPropWind/IgnoreNear/IsPropHookActive/GetPropWindStats`. Hashes resolve through `RED4ext_ResolveAddress` with a 0 check, and the runtime is version-independent.
- **Scripts** (mod `r6\scripts\NightCityWinds`): `Natives.reds` (bindings) and `NCWWind.reds`, the wind field.
  - `NCWWind` is a ScriptableSystem: `At(pos, h)`, `Exposure`, `Mean`, `Heading`, `Gust`, `SetOverride`, `SetScale`.
  - Every frame it pushes the wind at the player or their car, with shelter, to the plugin. **Verified in game**: it finds the weather's curves.
- **Player layer** (in `NCWWind.Tick`, on foot only):
  - V's velocity comes from frame-to-frame position. V counts as airborne when no Static, Vehicle or Dynamic hit is found within 0.6 m below the feet.
  - While airborne, drag on a person (Cd 1.0, 0.7 m², 80 kg) is applied as `PSMImpulse { id = n"impulse", impulse = Δv }` each frame.
  - Nothing is applied while grounded, where the locomotion state machine owns the velocity. [built; not yet tested in game]
- **Wind states and visual sync** (NCWWind rewrite, 2026-10-01) [built; not yet tested in game]:
  - **States:** calm about 1 m/s, breeze 4, windy 8, gale 14, storm 21 (±20%), drawn from per-weather odds, each lasting 90 to 300 s. The mean eases over about 25 s.
  - **Curves:** every weather state's environment parameters are loaded at start. The original curves of each state's primary `WindAreaSettings` are copied into script arrays, which drive direction and the daily shape.
  - **Visual sync:** every 0.1 s the live wind is written back into every state's curves. Strength = the state's original average × (current speed with gusts ÷ that weather's design speed) × gain; direction = the current heading. The point count is never changed, and the originals are restored on disable.
  - **Open question:** whether the renderer, cloth and particles read these curves live, or copied them at load.
- **Dev probe:** the CET `WindProbe` window (weather and body dumps, impulse test, wind pinning, live stats), plus the redscript helper `WindProbeRS`.

## 12d. How foliage reads the wind [verified by disassembly, 2026-10-01]

- **Render path:** the render frame's `AdvanceWind` stage (function hash 1614685340) queues a command whose vtable is hash 869338778. Its execute (hash 2276729450) calls **hash 1291658409**, the SpeedTree wind update. For each SpeedTree wind object (`+0x4528` holds its current strength), it reads the blended environment:
  - strength `env+0xB20` → `clamp(strength × 0.05, 0, 1)`, applied when it changes (hash 2493452431);
  - direction `env+0xB30..0xB38` (hash 2840991125);
  - time `env+0x1B0` (advance, hash 1555304324).
- **So trees need a WindAreaSettings strength of about 20 for full sway.** Vanilla sandstorm uses 20; sunny's 0.06 gives 0.003, which is still trees. That's why the first visual sync, scaled from each weather's own average, moved smoke slightly and trees not at all.
- **NCWWind's visual strength is now** `max(art, physical) × gain`:
  - physical = `20 × (v / 21 m/s)²`, i.e. breeze 4 m/s ≈ 0.7, windy 8 ≈ 2.9, gale 14 ≈ 8.9, storm 21 = 20;
  - art = the weather's curve average × (v / design)², which keeps a weather authored windier than that.
- **Verified in game** (Omar's clip, 2026-10-01; Nova City 2 active, 63 states driven): with a pinned 40 m/s wind at gain 1.0, a palm goes from still to fronds whipping and bent hard downwind within the 10 s clip.
- **Ceiling:** a pinned 40 m/s wind (strength about 72) sheared a tyre fire's flames off their source. The visual strength is now capped at 25. The particle floor now applies only to smoke-like paths (smoke, steam, exhaust, fume, vapor, dust); flames keep their authored influence.
- **Particles:** prop smoke follows the same blended strength (seen in game). Explosion and vehicle-fire smoke were authored with little or no `windInfluence`; NCWWind raises it to a floor on smoke/fire/steam/explosion effects at `Resource/PostLoad`, in both `CParticleEmitter.windInfluence` and the cooked `rendRenderParticleBlob.header.emitterInfo.windInfluence`.
- `wind_intensity` (CName `0x78178CFC0099FB69`, registered by hash 3185054827) is a separate parameter. It's probably the ambient-audio wind RTPC [unverified]; a candidate for driving wind sound later.

## 12c. Nova City 2 integration (2026-10-01)

**How Nova City 2 hooks in:**
- It replaces `base\worlds\03_night_city\_compiled\default\03_night_city.streamingworld` (archive hash `611101B4BB2A5E9C`) so its `environmentDefinition` points to `base\weather\24h_basic\nova_city_master_env_v002.env` (hash `DC7FC79AFF65E11E`).
- No other mod in the modlist overrides either file (all 2,686 archives scanned).
- Its definition lists **70 weather states**, the vanilla ones plus Nova City's, with envparams under `base\weather\nova_city_weather\`, `nova_city_alpha\`, `nova_city_weather_old\`, `vanilla_states\` and `test_states\`. Most carry a `WindAreaSettings`.
- Read with `tools\re\archive.py`, an RDAR reader that decompresses through the game's own `oo2ext_7_win64.dll`.

**Framework support:** no hard dependency.
- NCWWind loads whatever definition `WeatherSystem.GetEnvironmentDefinition()` returns, plus any active state missing from it. So Nova City's curves are copied and driven automatically when it's installed, and nothing changes when it isn't.
- `NCWWind.KindOf` names every vanilla and Nova City state explicitly, with keyword fallback for other weather mods:

| Kind | States |
|---|---|
| clear | sunny, sunny_old, clear, sunny_sunset, sunn_e3, vanilla_sunny, default, sky_softbox(_clear), blackout, test |
| smog | pollution, vanilla_pollution, smog, haze_pollution, haze_smog |
| fog | fog, vanilla_fog, mist, fog_heavy, fog_dense, fog_thick, fog_dark_dense, fog_haze, silent_hill, pollution_fog |
| clouds | light_clouds, cloudy, vanilla_light_clouds, vanilla_cloudy, overcast, overcast_light, overcast_broken, courier_clouds, gloomy, meme_clouds, sa_courier_clouds, q306_epilogue_cloudy_morning |
| heavy clouds | heavy_clouds, vanilla_heavy_clouds, heavy_clouds_dense |
| rain | rain, vanilla_rain, rain_alt_1, rain_q, rain_q_alt, rain_3q, rain_wip, q306_rainy_night |
| toxic rain | toxic_rain, vanilla_toxic_rain |
| storm | storm |
| sandstorm | sandstorm, sandstorm_old, vanilla_sandstorm |
| hot and still | drought, arid, humid, muggy, haze, haze_heavy, dew |
| windy | windy, sunny_windy |
| drizzle | drizzle, drizzle_light, light_rain, distant_rain, q302_light_rain |
| heavy rain | rain_alt_2, drizzle_heavy, downpour |
| wet fog | fog_wet, fog_rain, haze_rain |

Each kind has a design speed (the m/s its art was made for) and its own odds over calm, breeze, windy, gale and storm.

**Other weather and visual mods in the list** (scanned 2026-10-01; scan script in the scratchpad, archives read with `archive.py`):
- **State switchers (compatible as is):**
  - Weather Switcher and Auto Weather Scheduler (CET) call `WeatherSystem.SetWeather/ResetWeather` with a priority.
  - Disable Quest Weather (reds) blanks `questPlayEnv_SetWeather` nodes.
  - NCWWind follows the current state within 2 s, so these just change which state it reads.
- **LUT Switcher 3** edits loaded env resources live, which is evidence that runtime writes take effect. It uses Codeware `CallbackSystem` `Resource/PostLoad`/`Resource/Ready` with `ResourceTarget.Type(n"worldEnvironmentDefinition" | n"worldEnvironmentAreaParameters" | n"worldStreamingSector")`.
  - It names the master envs: `base\weather\24h_basic\cp2077_master_env_nge_v002.env` (base), `…\cp2077_master_env_ep1_v006.env` (EP1), `ep1\weather\cp2077_ep1_master_env_ep1_v005.env` (Dogtown), and `…\nova_city_master_env_v002.env` / `…\nova_city_master_env_ncr_v002.env` (Nova City).
  - Zone overrides: Dogtown trigger areas carry inline env params (`worldTriggerAreaNode.notifiers` → `worldEnvAreaNotifier.params.areaParameters`). These are local art overrides, which NCWWind deliberately leaves alone.
- **Wind-reactive particles** (`windInfluence` in their particle resources): Ultra Fog Steam Smoke and Bloom (165 files), Nova City 2 (15), Exploded Vehicles Smoke Overhaul (5), (Less) Perfect Rain (2). They follow whatever wind the weather curves carry.
- **NCWWind now:**
  - registers `Resource/PostLoad` for every `worldEnvironmentDefinition` and queues its states' envparams, de-duplicated by path hash;
  - also queues the active state if it isn't in any definition (quest weathers);
  - writes curves by state name, so base, EP1 and Nova City files for the same state are all kept in step.
- **Note:** in Omar's first probe runs, `GetEnvironmentDefinition()` returned the 15 vanilla states. Either Nova City 2 wasn't enabled yet at that point, or the definition the weather system uses differs from the world's [check with the probe after a restart].

## 12. Open items, in priority order

A CET probe for items 1, 2 and 4 is installed, disabled until the mod is enabled in MO2: `F:\Cyberpunk 2077\mods\Cyberpunk Wind Framework\bin\x64\plugins\cyber_engine_tweaks\mods\WindProbe\init.lua`.
- In the CET console, `WindProbe.All()` dumps the weather and bodies, and `WindProbe.Push()` gives one sideways impulse to your car. There are hotkeys too.
- Output goes to the console and to `WindProbe.log` in that folder.
- It reads only. The one exception is `Push()`, a single impulse on the car you're in.

Done (section 12a): the classes that hand out a body, the hidden getters (they don't marshal in CET), the vanilla wind curves, and the `PhysicalImpulseEvent` units (N·s, applied once).

1. **[disassembly]** Find the `ApplyWind` caller and the global wind storage (to drive cloth and foliage), and the PxActor `userData` → game proxy link (to classify actors in the PhysX hook).
2. **[runtime]** Does an off-centre `PhysicalImpulseEvent` produce the expected torque (yaw a moving car)?
3. **[runtime]** How `entPhysicalImpulseAreaComponent` applies its impulse (once on entering, or every frame).
4. **[runtime, native]** Confirm the `ApplyAirResistance` detour in a minimal RED4ext plugin: the first build step of the framework.
5. **[WolvenKit]** Vehicle `.phys` masses and inertia.
