-- =============================================================================
-- NIGHT CITY WINDS - RE PROBE (dev only, read-mostly)
--
-- Answers the open items in F:\2077 Wind Dev\docs\PHYSICS_RE_FINDINGS.md section 12.
-- Run from the "Wind Probe" window in the CET overlay (buttons), the hotkeys under
-- Bindings > WindProbe, or the CET console via GetMod: GetMod("WindProbe").All()
-- (CET sandboxes each mod, so a bare WindProbe.All() can't see it.)
--   WindProbe.Weather()   the active weather state and its WindAreaSettings curves, if any
--   WindProbe.Bodies()    every component on your car (or what you look at) that hands out a
--                         PhysicalBodyInterface, and what the hidden natives return
--                         (GetLinearVelocity, GetAngularVelocity, GetMass, GetLocalCenterOfMass, ...)
--   WindProbe.Push()      ONE upward PhysicalImpulseEvent on your car (2 m/s worth); the peak
--                         climb rate afterwards tells the units
--   WindProbe.All()       Weather + Bodies
-- Everything goes to the CET console and to WindProbe.log next to this file.
-- Remove this folder when the framework ships; it changes nothing on its own.
-- =============================================================================

local WindProbe = { log = nil, overlay = false, lines = {} }

local function out(s)
  print("[WindProbe] " .. s)
  table.insert(WindProbe.lines, s)
  if #WindProbe.lines > 400 then
    table.remove(WindProbe.lines, 1)
  end
  if WindProbe.log then
    WindProbe.log:write(s .. "\n")
    WindProbe.log:flush()
  end
end

local function try(label, fn)
  local ok, a, b, c = pcall(fn)
  if ok then
    return a, b, c
  end
  out("  " .. label .. " FAILED: " .. tostring(a))
  return nil
end

local function v(x)
  if x == nil then return "nil" end
  local ok, s = pcall(function()
    return string.format("(%.3f, %.3f, %.3f)", x.x or x.X, x.y or x.Y, x.z or x.Z)
  end)
  return ok and s or tostring(x)
end

local function cls(o)
  local ok, n = pcall(function() return NameToString(o:GetClassName()) end)
  return ok and n or "?"
end

-- ---- weather ---------------------------------------------------------------------
-- CET can't hand struct fields to script_ref parameters, so the resource path and the
-- curves are read by the redscript helper WindProbeRS (r6\scripts\WindProbe, needs a
-- game restart once). The loads are async: Weather() starts them, onUpdate reports each
-- state when its parameters have loaded.
WindProbe.pending = {}

function WindProbe.Weather()
  out("== weather")
  if not WindProbeRS then
    out("  WindProbeRS (redscript helper) not found: restart the game once so redscript compiles it")
    return
  end
  local ws = Game.GetWeatherSystem()
  local st = try("GetWeatherState", function() return ws:GetWeatherState() end)
  if st then
    out("  active state: " .. NameToString(st.name))
  end
  local def = try("GetEnvironmentDefinition", function() return ws:GetEnvironmentDefinition() end)
  if not def then
    return
  end
  for _, s in ipairs(def.weatherStates or {}) do
    local name = NameToString(s.name)
    local tok = try(name .. " ParamsToken", function() return WindProbeRS.ParamsToken(s) end)
    if tok then
      table.insert(WindProbe.pending, { name = name, tok = tok, at = os.clock() })
    end
  end
  out("  loading the parameters of " .. #WindProbe.pending .. " weather states...")
end

local function pollWeather()
  for i = #WindProbe.pending, 1, -1 do
    local p = WindProbe.pending[i]
    local done = try("token", function() return p.tok:IsFinished() end)
    if done or os.clock() - p.at > 10.0 then
      table.remove(WindProbe.pending, i)
      out("  state " .. p.name .. ":")
      local res = done and try("GetResource", function() return p.tok:GetResource() end) or nil
      if res then
        local rep = try("WindReport", function() return WindProbeRS.WindReport(res) end) or "?"
        for line in string.gmatch(rep, "[^\n]+") do
          out("    " .. line)
        end
      else
        out("    parameters failed to load")
      end
    end
  end
end

-- ---- bodies ----------------------------------------------------------------------
local HIDDEN = { "GetLinearVelocity", "GetAngularVelocity", "GetLinearSpeed", "GetMass",
  "GetLocalCenterOfMass", "GetBoundsCenter", "GetDimensions", "GetBodyIndex", "IsSimulated", "IsQueryable" }

local function target()
  local player = Game.GetPlayer()
  local veh = try("GetMountedVehicle", function() return Game.GetMountedVehicle(player) end)
  if veh then return veh, "mounted vehicle" end
  local look = try("look-at", function() return Game.GetTargetingSystem():GetLookAtObject(player, false, false) end)
  return look, "looked-at object"
end

function WindProbe.Bodies()
  out("== bodies")
  local e, what = target()
  if not e then
    out("  get in a car or look at a physics prop first")
    return
  end
  out("  " .. what .. ": " .. cls(e))
  if e.GetLinearVelocity then
    out("  entity GetLinearVelocity: " .. v(try("entity vel", function() return e:GetLinearVelocity() end)))
  end
  if e.GetTotalMass then
    out("  GetTotalMass: " .. tostring(try("GetTotalMass", function() return e:GetTotalMass() end)))
  end
  -- one line per component that hands out a body; the getters on the first two only
  local comps = try("GetComponents", function() return e:GetComponents() end) or {}
  local shown = 0
  for _, c in ipairs(comps) do
    local n = cls(c)
    if c.CreatePhysicalBodyInterface then
      local body = try(n .. ".CreatePhysicalBodyInterface", function() return c:CreatePhysicalBodyInterface() end)
      if body then
        local sim = try("IsSimulated", function() return body:IsSimulated() end)
        out(string.format("  %s '%s' simulated=%s", n, NameToString(c:GetName()), tostring(sim)))
        if shown < 2 and sim then
          shown = shown + 1
          for _, f in ipairs(HIDDEN) do
            if body[f] then
              local r = { pcall(function() return body[f](body) end) }
              local parts = {}
              for k = 2, #r do
                parts[#parts + 1] = type(r[k]) == "userdata" and v(r[k]) or tostring(r[k])
              end
              out(string.format("      %s -> ok=%s n=%d %s", f, tostring(r[1]), #r - 1, table.concat(parts, ", ")))
            else
              out("      " .. f .. " not bound")
            end
          end
        end
      end
    end
  end
end

-- ---- one impulse on the car --------------------------------------------------------
-- Straight up through the car's position, worth 2 m/s: tyres don't resist an upward push,
-- so the peak climb rate over the next 0.4 s tells the units (2.0 = N s, applied once).
function WindProbe.Push()
  out("== push (up, 2 m/s worth)")
  local player = Game.GetPlayer()
  local veh = try("GetMountedVehicle", function() return Game.GetMountedVehicle(player) end)
  if not veh then
    out("  get in a car first")
    return
  end
  local mass = try("GetTotalMass", function() return veh:GetTotalMass() end) or 1500.0
  local pos = veh:GetWorldPosition()
  local ev = PhysicalImpulseEvent.new()
  ev.worldPosition = Vector3.new(pos.x, pos.y, pos.z)
  ev.worldImpulse = Vector3.new(0.0, 0.0, mass * 2.0)
  veh:QueueEvent(ev)
  out(string.format("  mass %.1f kg, queued %s N s, velocity before %s", mass, v(ev.worldImpulse), v(veh:GetLinearVelocity())))
  WindProbe.pushAt = os.clock()
  WindProbe.pushVeh = veh
  WindProbe.pushPeak = -1e9
  WindProbe.pushFrames = 0
end

-- ---- vehicle wind (the plugin's car drag hook) ------------------------------------------
-- Sets one world wind vector, relative to the car's heading at the moment it's pressed:
-- "head" blows against the car, "tail" pushes it, "cross" blows from its left.
WindProbe.windSpeed = 30.0

local function cwf(name, ...)
  local args = { ... }
  local ok, r = pcall(function() return Game[name](table.unpack(args)) end)
  if not ok then
    out("  " .. name .. " FAILED: " .. tostring(r))
    return nil
  end
  return r
end

local function windSystem()
  local ok, sys = pcall(function()
    return Game.GetScriptableSystemsContainer():Get("NightCityWinds.NCWWind")
  end)
  return ok and sys or nil
end

-- "weather": back to the live wind field; anything else pins the wind (an override in the
-- field, so its per-frame push doesn't undo it)
function WindProbe.VehicleWind(mode)
  local sys = windSystem()
  if mode == "weather" then
    if sys then sys:ClearOverride() end
    out("== wind: back to the weather's field")
    return
  end
  local s = WindProbe.windSpeed
  local w = Vector4.new(0.0, 0.0, 0.0, 0.0)
  if mode ~= "off" then
    -- relative to the car, or to V on foot (headwind = blowing into V's face)
    local player = Game.GetPlayer()
    local veh = try("GetMountedVehicle", function() return Game.GetMountedVehicle(player) end)
    local ref = veh or player
    local f = ref:GetWorldForward()
    local r = ref:GetWorldRight()
    if mode == "head" then
      w = Vector4.new(-f.x * s, -f.y * s, 0.0, 0.0)
    elseif mode == "tail" then
      w = Vector4.new(f.x * s, f.y * s, 0.0, 0.0)
    elseif mode == "cross" then
      w = Vector4.new(r.x * s, r.y * s, 0.0, 0.0)
    end
  end
  if sys then
    sys:SetOverride(w)
  else
    cwf("NCW_SetWind", w)
  end
  out(string.format("== wind pinned (%s): %s m/s, hook active=%s", mode, v(w), tostring(cwf("NCW_IsVehicleHookActive"))))
end

function WindProbe.DragStats()
  local ok, s = pcall(function() return Game.NCW_GetVehicleDragStats() end)
  local line = "plugin not loaded"
  if ok and s then
    local okv, ver = pcall(function() return Game.NCW_Version() end)
    local okh1, carHook = pcall(function() return Game.NCW_IsVehicleHookActive() end)
    local okh2, smokeHook = pcall(function() return Game.NCW_IsSmokeHookActive() end)
    line = string.format("Night City Winds %s | hooks: cars %s, smoke %s", okv and ver or "?",
      tostring(okh1 and carHook), tostring(okh2 and smokeHook))
    line = line .. string.format("\ndrag calls %d | ground %.1f m/s (%.0f km/h) | airspeed %.1f m/s | airResistanceFactor %.3f",
      s.x, s.y, s.y * 3.6, s.z, s.w)
    local oka, ae = pcall(function() return Game.NCW_GetVehicleAeroStats() end)
    if oka and ae then
      line = line .. string.format("\naero: side force %.0f N | yaw torque %.0f N m | lift %.0f N", ae.x, ae.y, ae.z)
    end
  end
  local ok3, ps = pcall(function() return Game.NCW_GetPropWindStats() end)
  local ok4, hooked = pcall(function() return Game.NCW_IsPropHookActive() end)
  if ok3 and ps then
    line = line .. string.format("\nprops: hooked %s | PhysX steps %d | dynamic actors %d | pushed %d | max force %.0f N",
      tostring(ok4 and hooked), ps.x, ps.y, ps.z, ps.w)
  end
  local ok7, sm = pcall(function() return Game.NCW_GetSmokeWindStats() end)
  if ok7 and sm then
    line = line .. string.format("\nsmoke (engine): %d emitters set up | %d with our modifier | %.0fk push steps | %d tagged late",
      sm.x, sm.y, sm.z, sm.w)
  end
  local ok8, tg = pcall(function() return Game.NCW_GetSmokeTagStats() end)
  if ok8 and tg then
    line = line .. string.format("\nsmoke tags (load): %d effects | %d emitters | %d physics pools cleared | %d setups matched (%03d weak)",
      tg.x, tg.y, tg.z, math.floor(tg.w), math.floor((tg.w - math.floor(tg.w)) * 1000 + 0.5))
  end
  local ok9, ry = pcall(function() return Game.NCW_GetSmokeRayStats() end)
  if ok9 and ry then
    line = line .. string.format("\nsmoke rays: %d puffs checked | %d retired on a hit | survey entries %d", ry.x, ry.y, ry.z)
  end
  local sys = windSystem()
  if sys then
    local ok2, field = pcall(function()
      local p = sys:Pushed()
      return string.format("weather kind: %s | wind curves driven for %d states\n", sys:WeatherKind(), sys:DrivenStates()) ..
        string.format("field: %s | state %s | mean %.1f m/s | heading %.0f deg | gust %.2f | shelter %.2f | curves %s | visual strength %.3f | at car %s (%.1f m/s)",
        NameToString(sys:Weather()), NameToString(sys:State()), sys:Mean(), sys:Heading(), sys:Gust(), sys:Sheltered(),
        tostring(sys:HasWeatherCurves()), sys:VisualStrength(), v(p), math.sqrt(p.x * p.x + p.y * p.y + p.z * p.z))
    end)
    line = line .. "\n" .. (ok2 and field or ("field: " .. tostring(field)))
    local ok5, pl = pcall(function()
      local d = sys:PlayerPush()
      return string.format("player: airborne %s | wind push %.3f m/s this frame",
        tostring(sys:PlayerAirborne()), math.sqrt(d.x * d.x + d.y * d.y + d.z * d.z))
    end)
    line = line .. "\n" .. (ok5 and pl or ("player: " .. tostring(pl)))
  else
    line = line .. "\nfield: NCWWind system not found"
  end
  return line
end

function WindProbe.All()
  WindProbe.Weather()
  WindProbe.Bodies()
end

registerForEvent("onInit", function()
  WindProbe.log = io.open("WindProbe.log", "a")
  out("---- loaded " .. os.date())
end)

registerForEvent("onUpdate", function()
  pollWeather()
  -- track the car's climb rate for 0.4 s after a push
  if WindProbe.pushAt then
    local t = os.clock() - WindProbe.pushAt
    local vz = WindProbe.pushVeh:GetLinearVelocity().z
    WindProbe.pushFrames = WindProbe.pushFrames + 1
    if vz > WindProbe.pushPeak then
      WindProbe.pushPeak = vz
      WindProbe.pushPeakFrame = WindProbe.pushFrames
    end
    if t > 0.4 then
      out(string.format("  peak climb %.3f m/s at frame %d of %d (expect ~2.0 if the impulse is N s)", WindProbe.pushPeak, WindProbe.pushPeakFrame or 0, WindProbe.pushFrames))
      WindProbe.pushAt = nil
    end
  end
end)

registerForEvent("onOverlayOpen", function() WindProbe.overlay = true end)
registerForEvent("onOverlayClose", function() WindProbe.overlay = false end)

registerForEvent("onDraw", function()
  if not WindProbe.overlay then
    return
  end
  if ImGui.Begin("Wind Probe") then
    if ImGui.Button("Weather + bodies") then WindProbe.All() end
    ImGui.SameLine()
    if ImGui.Button("Weather") then WindProbe.Weather() end
    ImGui.SameLine()
    if ImGui.Button("Bodies") then WindProbe.Bodies() end
    ImGui.SameLine()
    if ImGui.Button("Push my car") then WindProbe.Push() end
    ImGui.SameLine()
    if ImGui.Button("Clear") then WindProbe.lines = {} end
    ImGui.Separator()
    ImGui.Text("Vehicle wind (plugin)")
    WindProbe.windSpeed = ImGui.SliderFloat("m/s", WindProbe.windSpeed, 0.0, 60.0, "%.0f")
    if ImGui.Button("Headwind") then WindProbe.VehicleWind("head") end
    ImGui.SameLine()
    if ImGui.Button("Tailwind") then WindProbe.VehicleWind("tail") end
    ImGui.SameLine()
    if ImGui.Button("Crosswind") then WindProbe.VehicleWind("cross") end
    ImGui.SameLine()
    if ImGui.Button("Still air") then WindProbe.VehicleWind("off") end
    ImGui.SameLine()
    if ImGui.Button("Weather wind") then WindProbe.VehicleWind("weather") end
    ImGui.Text("Wind state")
    for _, st in ipairs({ "calm", "breeze", "windy", "gale", "storm" }) do
      if ImGui.Button(st) then
        local sys = windSystem()
        if sys then sys:ForceState(CName.new(st)) end
        out("== wind state forced: " .. st)
      end
      ImGui.SameLine()
    end
    if ImGui.Button("weather picks") then
      local sys = windSystem()
      if sys then sys:ForceState(CName.new("")) end
      out("== wind state: back to the weather's odds")
    end
    ImGui.Text("Car aero (plugin): crosswind side force, yaw, lift")
    WindProbe.aeroOn = ImGui.Checkbox("car aero", WindProbe.aeroOn ~= false)
    WindProbe.aeroSide = ImGui.SliderFloat("side gain", WindProbe.aeroSide or 1.0, 0.0, 3.0, "%.2f")
    WindProbe.aeroLever = ImGui.SliderFloat("yaw lever (m ahead of the centre of mass)", WindProbe.aeroLever or 0.5, -1.0, 2.0, "%.2f")
    WindProbe.aeroLift = ImGui.SliderFloat("lift Cl x area (m2)", WindProbe.aeroLift or 2.4, 0.0, 6.0, "%.2f")
    if WindProbe.aeroSent ~= (tostring(WindProbe.aeroOn) .. WindProbe.aeroSide .. WindProbe.aeroLever .. WindProbe.aeroLift) then
      local ok = pcall(function() Game.NCW_SetVehicleAero(WindProbe.aeroOn, WindProbe.aeroSide, WindProbe.aeroLever, WindProbe.aeroLift) end)
      if ok then
        WindProbe.aeroSent = tostring(WindProbe.aeroOn) .. WindProbe.aeroSide .. WindProbe.aeroLever .. WindProbe.aeroLift
      end
    end
    ImGui.Text("Smoke (plugin)")
    WindProbe.raysOn = ImGui.Checkbox("overpass rays (smoke dies on what it hits)", WindProbe.raysOn ~= false)
    if WindProbe.raysSent ~= WindProbe.raysOn then
      local ok = pcall(function()
        Game.GetScriptableServiceContainer():GetService("NightCityWinds.NCWParticles"):SetRays(WindProbe.raysOn)
      end)
      if ok then WindProbe.raysSent = WindProbe.raysOn end
    end
    WindProbe.surveyOn = ImGui.Checkbox("record untagged smoke (survey)", WindProbe.surveyOn == true)
    if WindProbe.surveySent ~= WindProbe.surveyOn then
      local ok = pcall(function() Game.NCW_SetSmokeSurvey(WindProbe.surveyOn) end)
      if ok then WindProbe.surveySent = WindProbe.surveyOn end
    end
    ImGui.SameLine()
    if ImGui.Button("Dump survey to the plugin log") then
      local ok, n = pcall(function() return Game.NCW_DumpSmokeSurvey() end)
      out(ok and ("== survey: " .. tostring(n) .. " untagged particle systems logged") or ("survey failed: " .. tostring(n)))
    end
    ImGui.Text("Visuals (foliage, cloth, smoke, fire)")
    WindProbe.visualOn = ImGui.Checkbox("write wind into the weather curves", WindProbe.visualOn ~= false)
    WindProbe.visualGain = ImGui.SliderFloat("visual gain", WindProbe.visualGain or 1.0, 0.0, 50.0, "%.2f")
    WindProbe.playerOn = ImGui.Checkbox("push V while airborne", WindProbe.playerOn == true)
    WindProbe.smokeOn = ImGui.Checkbox("smoke wind in the engine (plugin push, off = game's own wind)", WindProbe.smokeOn == true)
    WindProbe.smokeGain = ImGui.SliderFloat("smoke wind gain", WindProbe.smokeGain or 1.0, 0.0, 5.0, "%.2f")
    if WindProbe.smokeOnSent ~= WindProbe.smokeOn or WindProbe.smokeGainSent ~= WindProbe.smokeGain then
      local ok = pcall(function() Game.NCW_SetSmokeWind(WindProbe.smokeOn, WindProbe.smokeGain) end)
      if ok then
        WindProbe.smokeOnSent = WindProbe.smokeOn
        WindProbe.smokeGainSent = WindProbe.smokeGain
      end
    end
    do
      local sys = windSystem()
      if sys then
        pcall(function()
          if WindProbe.playerSent ~= WindProbe.playerOn then
            sys:SetPlayerWind(WindProbe.playerOn)
            WindProbe.playerSent = WindProbe.playerOn
          end
        end)
      end
    end
    do
      local sys = windSystem()
      if sys then
        pcall(function()
          if WindProbe.visualOnSent ~= WindProbe.visualOn then
            sys:SetVisualSync(WindProbe.visualOn)
            WindProbe.visualOnSent = WindProbe.visualOn
          end
          if math.abs(sys:VisualGain() - WindProbe.visualGain) > 0.001 then
            sys:SetVisualGain(WindProbe.visualGain)
          end
        end)
      end
    end
    if WindProbe.statsAt == nil or os.clock() - WindProbe.statsAt > 0.25 then
      WindProbe.statsAt = os.clock()
      WindProbe.stats = WindProbe.DragStats()
    end
    ImGui.TextWrapped(WindProbe.stats or "")
    ImGui.Separator()
    ImGui.BeginChild("WindProbeLog", 0, 0, false)
    for _, l in ipairs(WindProbe.lines) do
      ImGui.TextWrapped(l)
    end
    ImGui.EndChild()
  end
  ImGui.End()
end)

registerHotkey("WindProbeAll", "Probe weather + bodies", function() WindProbe.All() end)
registerHotkey("WindProbePush", "Push my car up once", function() WindProbe.Push() end)

return WindProbe
