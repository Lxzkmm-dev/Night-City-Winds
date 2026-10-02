"""Build the Wind Framework's smoke patch archive.

Cyberpunk's renderer takes each particle emitter's wind influence from the effect file when it
loads it (runtime edits never reach it; see docs/SMOKE_WIND_ANALYSIS.md). So smoke can only follow
the wind if the files carry a wind influence. This script:
  1. extracts the vanilla effects in the smoke-relevant fx folders (WolvenKit CLI),
  2. extracts the same folders from every *enabled* mod archive (MO2 profile), so effects that a
     mod replaces or adds (Ultra Fog, Exploded Vehicles Smoke Overhaul, ...) are patched from
     that mod's version, picking the archive that wins in game (see archive_load_order),
  3. serializes them to JSON, raises windInfluence on emitters whose editor name says smoke,
     steam, dust and the like (both the emitter and the cooked render blob's emitterInfo),
     leaving flames, sparks and debris alone; gives those without one a Drag module (the only
     way wind reaches a particle), and takes VelocityOverLife off columns that have no drag of
     their own (it overwrites the velocity drag adds),
  4. deserializes the patched files and packs them into an archive that must load before the
     mods it patches, installed into the Wind Framework mod.
Read-only on the game and on other mods. Re-run whenever a smoke mod updates.
When a modlist.txt sets the archive order, our archive has to be its first line (the script
warns if it isn't); otherwise every mod it lists loads first and wins.

Usage: python build_smoke.py [--floor 0.2] [--drag 0.15] [--profile "<MO2 profile name>"] [--dry-run]
"""
import argparse, base64, json, os, re, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WK = os.path.join(HERE, "..", "deps", "wolvenkit", "WolvenKit.CLI.exe")
GAME = r"G:\SteamLibrary\steamapps\common\Cyberpunk 2077"
MO2 = r"F:\Cyberpunk 2077"
MOD_DIR = os.path.join(MO2, "mods", "Cyberpunk Wind Framework", "archive", "pc", "mod")
# first loaded wins a file conflict; without a modlist.txt archives load alphabetically, and
# "!!!!!" sorts before "!!!_Ultra_Fog_Lite" and every other "!!!"/"#"/letter-named archive
ARCHIVE_NAME = "!!!!!NightCityWinds_SmokeWind"

# the fx folders whose effects can carry wind-blown smoke
PATH_RX = (r"^(base|ep1)\\fx\\("
           r"environment\\(smoke|pyro|dust|steam)|"
           r"vehicles\\(_damage|_exhaust|car|bike)|"
           r"_library\\(fire|smoke|explosion)|"
           r"weapons\\(explosives|throwables|grenades)"
           r")\\.*\.particle$")
# emitters (by editor name) that are smoke-like, and ones never to touch
SMOKE_RX = re.compile(r"smoke|steam|dust|fume|vapo|cloud|haze|mist|fog|soot|ash", re.I)
SKIP_RX = re.compile(r"fire|flame|spark|ember|glow|light|debris|chunk|flash|blast|shock|heat|distort", re.I)


def run(args):
    print("  $", " ".join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args))[:300])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
        raise SystemExit("WolvenKit failed")
    return r.stdout


def enabled_mods(profile):
    path = os.path.join(MO2, "profiles", profile, "modlist.txt")
    mods = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("+"):
            mods.append(line[1:])
    return mods


def archive_load_order(profile):
    """Enabled mod archives in the order the game loads them; the first one holding a file wins.
    A modlist.txt in archive\\pc\\mod (CyberVision ships one) lists archives that load first, in
    its order; unlisted ones follow alphabetically. MO2 writes the profile's modlist.txt highest
    priority first, so the first mod supplying a file is the one the game sees.
    Returns ([(file name, mod, path)], (mod, modlist path) or None, listed names lowercased)."""
    arcs, modlist = {}, None
    for m in enabled_mods(profile):
        if m == "Cyberpunk Wind Framework":
            continue
        d = os.path.join(MO2, "mods", m, "archive", "pc", "mod")
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            lf = f.lower()
            if lf.endswith(".archive") and lf not in arcs:
                arcs[lf] = (f, m, os.path.join(d, f))
            elif lf == "modlist.txt" and modlist is None:
                modlist = (m, os.path.join(d, f))
    listed = []
    if modlist:
        listed = [l.strip().lower() for l in open(modlist[1], encoding="utf-8-sig", errors="replace") if l.strip()]
    rank = {}
    for i, n in enumerate(listed):
        rank.setdefault(n, i)
    order = sorted(arcs, key=lambda n: (0, rank[n]) if n in rank else (1, n))
    return [arcs[n] for n in order], modlist, listed


def fnv1a64(s):
    h = 0xCBF29CE484222325
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def archive_hashes(path):
    """The path hashes an archive holds, read from its index only (mods can be gigabytes)."""
    with open(path, "rb") as f:
        magic, ver, idx_pos, idx_size = struct.unpack("<IIQI", f.read(20))
        if magic != 0x52414452:  # 'RDAR'
            return set()
        f.seek(idx_pos)
        idx = f.read(idx_size)
    n = struct.unpack_from("<I", idx, 16)[0]
    return {struct.unpack_from("<Q", idx, 28 + 56 * i)[0] for i in range(n)}


def rel_files(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.endswith(".particle"):
                p = os.path.join(dp, f)
                out[os.path.relpath(p, root).lower()] = p
    return out


DRAG_BIT = 1 << 32  # CParticleModificatorDrag's bit in modifierSetMask (bitmap.py, 1,717 emitters)
VOL_BIT = 1 << 1    # CParticleModificatorVelocityOverLife's bit
# drag for columns whose curve is replaced by buoyancy: the game's own buoyant smoke uses 0.25-1.5
COLUMN_DRAG = 1.0


def enabled_module(e, cls):
    return next((((m or {}).get("Data") or {}) for m in e.get("modules") or []
                 if ((m or {}).get("Data") or {}).get("$type") == cls and ((m or {}).get("Data") or {}).get("isEnabled", 1)), None)


ACC_BIT = 1 << 3    # CParticleModificatorAcceleration's bit


def set_mask(info, mask):
    info["modifierSetMask"] = str(mask) if isinstance(info.get("modifierSetMask"), str) else mask


def vol_to_buoyancy(e, k, next_id):
    """Replace VelocityOverLife with buoyancy, for an emitter that will get drag k.
    The curve scales a particle's velocity component-wise every frame (0x13A28C), so it owns the
    column's motion. In its place goes a constant upward acceleration, the way the game's own
    smoke rises (Acceleration up + Drag ~1). The engine's drag is quadratic (dv/dt = -k |v| v),
    so a particle settles where a = k rise^2: acceleration k x rise x |rise| keeps the curve's
    mean climb.
    Cooked layout: VOL is mask bit 1 and its record is the first modifier record, u16 samples,
    u16 size = 4 + 12 samples (volrec.py, all 455 emitters with VOL). Acceleration is bit 3: a
    direction record 02 00 1C 00 + (min xyz, max xyz) and a scale record 02 00 0C 00 +
    (min, max), nothing else (accrec.py, 615 emitters). Bits 0 and 2 must be clear so the
    records stay first; an existing Acceleration is rewritten in place.
    Returns the rise speed, the handle ids used, or None (and changes nothing)."""
    blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
    info = ((blob.get("header") or {}).get("emitterInfo")) or {}
    upd = blob.get("updaterData") or {}
    data = upd.get("data") or {}
    mask = int(info.get("modifierSetMask") or 0)
    vol = enabled_module(e, "CParticleModificatorVelocityOverLife")
    if not vol or not mask & VOL_BIT or mask & 0b101 or "Bytes" not in data:
        return None
    raw = base64.b64decode(data["Bytes"] or "")
    off = int(upd.get("modifOffset") or 0)
    if off + 4 > len(raw):
        return None
    count, size = struct.unpack_from("<HH", raw, off)
    if count < 1 or size != 4 + 12 * count or off + size > len(raw):
        return None
    zs = struct.unpack_from("<%df" % (3 * count), raw, off + 4)[2::3]
    rise = mean_rise(e, sum(zs) / len(zs))
    if rise is None:
        return None
    az = k * rise * abs(rise)
    acc = (bytes.fromhex("02001c00") + struct.pack("<6f", 0, 0, az, 0, 0, az)
           + bytes.fromhex("02000c00") + struct.pack("<2f", 1, 1))
    old = enabled_module(e, "CParticleModificatorAcceleration")
    if mask & ACC_BIT:
        a = off + size
        if not old or a + 40 > len(raw) or raw[a:a + 4] != bytes.fromhex("02001c00") \
                or raw[a + 28:a + 32] != bytes.fromhex("02000c00"):
            return None
        raw = raw[:off] + acc + raw[a + 40:]
        old["direction"] = {"HandleId": str(next_id), "Data": {
            "$type": "CEvaluatorVectorConst", "freeAxes": "FVA_Three", "spill": 1,
            "value": {"$type": "Vector4", "W": 0, "X": 0, "Y": 0, "Z": az}}}
        old["scale"] = {"HandleId": str(next_id + 1), "Data": {"$type": "CEvaluatorFloatConst", "value": 1}}
        old["worldSpace"] = 1
        used = 2
        info["numModifiers"] = int(info.get("numModifiers") or 0) - 1
    else:
        raw = raw[:off] + acc + raw[off + size:]
        (e.setdefault("modules", [])).append({
            "HandleId": str(next_id),
            "Data": {
                "$type": "CParticleModificatorAcceleration",
                "direction": {"HandleId": str(next_id + 1), "Data": {
                    "$type": "CEvaluatorVectorConst", "freeAxes": "FVA_Three", "spill": 1,
                    "value": {"$type": "Vector4", "W": 0, "X": 0, "Y": 0, "Z": az}}},
                "editorGroup": "Velocity", "editorName": "Acceleration (buoyancy)", "isEnabled": 1,
                "scale": {"HandleId": str(next_id + 2), "Data": {"$type": "CEvaluatorFloatConst", "value": 1}},
                "seed": 0, "worldSpace": 1,
            },
        })
        used = 3
    data["Bytes"] = base64.b64encode(raw).decode()
    set_mask(info, (mask & ~VOL_BIT) | ACC_BIT)
    vol["isEnabled"] = 0
    return rise, used


SPAWN_CAP = 2.5  # highest birth point (m above the effect) a wind-carried column may keep


def lower_spawn(e, cap=SPAWN_CAP):
    """A column born at a fixed point high above its source (Exploded Vehicles' car column starts
    10 m up) keeps restarting from that spot however the wind carries it, so it reads as pinned
    in the sky while the plume below bends away. Lower a constant InitializerPosition Z to cap.
    Its cooked record is 02 00 1C 00 + (x y z, x y z) among the initializer records (before
    modifOffset); it is found by its exact bytes and must be unique. Returns the old Z or None."""
    ini = enabled_module(e, "CParticleInitializerPosition")
    ev = ((ini or {}).get("position") or {}).get("Data") or {}
    v = ev.get("value") if ev.get("$type") == "CEvaluatorVectorConst" else None
    if not isinstance(v, dict) or float(v.get("Z") or 0) <= cap:
        return None
    x, y, z = float(v.get("X") or 0), float(v.get("Y") or 0), float(v["Z"])
    blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
    upd = blob.get("updaterData") or {}
    data = upd.get("data") or {}
    raw = base64.b64decode(data.get("Bytes") or "")
    head = raw[:int(upd.get("modifOffset") or 0)]
    old = bytes.fromhex("02001c00") + struct.pack("<6f", x, y, z, x, y, z)
    if head.count(old) != 1:
        return None
    i = head.index(old)
    raw = raw[:i] + bytes.fromhex("02001c00") + struct.pack("<6f", x, y, cap, x, y, cap) + raw[i + len(old):]
    data["Bytes"] = base64.b64encode(raw).decode()
    v["Z"] = cap
    return z


def mean_rise(e, z_mult):
    """Mean vertical speed the removed curve gave a particle: initial Z velocity x curve's mean."""
    ini = enabled_module(e, "CParticleInitializerVelocity") or {}
    ev = (ini.get("velocity") or {}).get("Data") or {}
    try:
        if isinstance(ev.get("value"), dict):
            z = float(ev["value"].get("Z") or 0)
        elif isinstance(ev.get("min"), dict):
            z = (float(ev["min"].get("Z") or 0) + float(ev["max"].get("Z") or 0)) / 2
        else:
            return None
        return z * float(ini.get("scale") or 1) * z_mult
    except (TypeError, ValueError):
        return None


def max_handle_id(node):
    best = 0
    if isinstance(node, dict):
        for k, v in node.items():
            if k in ("HandleId", "HandleRefId") and str(v).isdigit():
                best = max(best, int(v))
            else:
                best = max(best, max_handle_id(v))
    elif isinstance(node, list):
        for v in node:
            best = max(best, max_handle_id(v))
    return best


def can_add_drag(e):
    blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
    info = ((blob.get("header") or {}).get("emitterInfo")) or {}
    data = (blob.get("updaterData") or {}).get("data") or {}
    return "Bytes" in data and not int(info.get("modifierSetMask") or 0) >> 32


def add_drag(e, coef, next_id):
    """Give an emitter a Drag modifier. In the cooked updater data, drag is mask bit 32 and its
    data is the module's scale as a float, then the coefficient evaluator record 02 00 0C 00 +
    (min, max). The engine's drag (0x13F65C in 2.31, picked by bit 32 in the modifier list
    builder at 0x41826C) reads the float first; without it (as built before 2026-10-02) it took
    the record header as the scale, got ~0 and skipped the drag, so every drag we added did
    nothing. It is quadratic: v' = v / (1 + k dt |v|) with k = scale x coefficient.
    Drag is the top bit here, so it can only be appended when no higher bit is set."""
    blob = (e.get("renderResourceBlob") or {}).get("Data") or {}
    info = ((blob.get("header") or {}).get("emitterInfo")) or {}
    upd = blob.get("updaterData") or {}
    data = upd.get("data") or {}
    mask = int(info.get("modifierSetMask") or 0)
    if mask & DRAG_BIT or mask >> 32 or "Bytes" not in data:
        return False
    raw = base64.b64decode(data["Bytes"] or "")
    raw += struct.pack("<f", coef) + bytes.fromhex("02000c00") + struct.pack("<2f", 1, 1)
    data["Bytes"] = base64.b64encode(raw).decode()
    info["modifierSetMask"] = str(mask | DRAG_BIT) if isinstance(info.get("modifierSetMask"), str) else (mask | DRAG_BIT)
    info["numModifiers"] = int(info.get("numModifiers") or 0) + 1
    # the editable module list too, so the asset and its cooked data agree
    (e.setdefault("modules", [])).append({
        "HandleId": str(next_id),
        "Data": {
            "$type": "CParticleModificatorDrag",
            "dragCoefficient": {"HandleId": str(next_id + 1), "Data": {"$type": "CEvaluatorFloatConst", "value": 1}},
            "editorGroup": "", "editorName": "Drag (wind)", "isEnabled": 1, "scale": coef, "seed": 0,
        },
    })
    return True


def set_wind(e, w):
    e["windInfluence"] = w
    info = (((e.get("renderResourceBlob") or {}).get("Data") or {}).get("header") or {}).get("emitterInfo")
    if isinstance(info, dict):
        info["windInfluence"] = w


def patch_json(path, floor, drag_coef, column_wind=None):
    d = json.load(open(path, encoding="utf-8"))
    root = d["Data"]["RootChunk"]
    patched, names = 0, []
    next_id = max_handle_id(d) + 1
    for h in root.get("emitters") or []:
        e = (h or {}).get("Data") or {}
        name = str(e.get("editorName") or "")
        if not SMOKE_RX.search(name) or SKIP_RX.search(name):
            continue
        changed = False
        # A Collision module makes the game simulate the emitter's particles in PhysX, and the
        # CPU modifiers (the engine's own drag, the plugin's wind) never reach those: in the
        # decisive run of 2026-10-02 the car plume's tall column, the one emitter with Collision,
        # was the one that ignored the wind while its collision-free twin drifted (docs 5f).
        # Smoke loses killOnCollision with it: it passes through overpasses instead of dying.
        mods = e.get("modules") or []
        kept = [m for m in mods if ((m or {}).get("Data") or {}).get("$type") != "CParticleModificatorCollision"]
        if len(kept) != len(mods):
            e["modules"] = kept
            name += " -collision"
            changed = True
        # only enabled modules count: an emitter can carry a switched-off Drag
        has_drag = enabled_module(e, "CParticleModificatorDrag") is not None
        coef = drag_coef
        if drag_coef > 0 and not has_drag and can_add_drag(e) and enabled_module(e, "CParticleModificatorVelocityOverLife"):
            # hand a column's motion from its curve to buoyancy plus drag, the way the game's own
            # smoke is built (Acceleration up + Drag ~1): it rises at the curve's mean speed and
            # takes the wind's speed across within a second or so. Emitters with a designer drag
            # keep their curve, since their drag was tuned for it; so do ones whose cooked layout
            # leaves no room, rather than losing their climb.
            r = vol_to_buoyancy(e, COLUMN_DRAG, next_id)
            if r is not None:
                rise, used = r
                next_id += used
                coef = COLUMN_DRAG
                name += " -vol +rise %.2g m/s" % rise
                changed = True
                z = lower_spawn(e)
                if z is not None:
                    name += " spawn %gm->%gm" % (z, SPAWN_CAP)
                if column_wind is not None:
                    set_wind(e, column_wind)
                    name += " wind %g" % column_wind
        if drag_coef > 0 and not has_drag and add_drag(e, coef, next_id):
            next_id += 2
            name += " +drag %.3g" % coef
            changed = True
        if float(e.get("windInfluence") or 0) < floor:
            e["windInfluence"] = floor
            changed = True
        blob = ((e.get("renderResourceBlob") or {}).get("Data") or {})
        info = ((blob.get("header") or {}).get("emitterInfo")) if isinstance(blob.get("header"), dict) else None
        if isinstance(info, dict) and float(info.get("windInfluence") or 0) < floor:
            info["windInfluence"] = floor
            changed = True
        if changed:
            patched += 1
            names.append(name)
    if patched:
        json.dump(d, open(path, "w", encoding="utf-8"), indent=2)
    return patched, names


def main():
    ap = argparse.ArgumentParser()
    # the game's own smoke uses 0.1-0.2 (survey.py), tuned for its windiest weather (sandstorm,
    # strength 20); 0.5 blew car and tire smoke away faster than it was emitted (Omar, 2026-10-01)
    ap.add_argument("--floor", type=float, default=0.2)
    ap.add_argument("--column-wind", type=float, default=None,
                    help="wind influence for columns moved from their curve to buoyancy (default: the floor)")
    ap.add_argument("--drag", type=float, default=0.15,
                    help="drag coefficient given to smoke emitters that have none (0 = don't add)")
    ap.add_argument("--profile", default="04 - CyberVision - PATH TRACING Very High - RTX 5070 TI")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--vanilla-only", action="store_true",
                    help="patch only the game's own effects (no mod versions); the shippable base patch")
    a = ap.parse_args()

    work = os.path.join(HERE, "work")
    vanilla = os.path.join(HERE, "src_vanilla")
    mods_out = os.path.join(HERE, "src_mods")
    for p in (work,):
        shutil.rmtree(p, ignore_errors=True)
    os.makedirs(work)

    # 1. vanilla (cached between runs)
    if not os.path.isdir(vanilla):
        arcs = [os.path.join(dp, f) for dp, dn, fn in os.walk(os.path.join(GAME, "archive", "pc"))
                for f in fn if f.endswith(".archive") and not f.startswith(("audio", "lang"))]
        run([WK, "extract", *arcs, "-o", vanilla, "-r", PATH_RX, "-v", "Minimal"])
    sources = rel_files(vanilla)
    origin = {k: "vanilla" for k in sources}
    print("vanilla effects:", len(sources))

    # 2. enabled mods' versions, each file taken from the archive that wins it in game
    shutil.rmtree(mods_out, ignore_errors=True)
    load, modlist, listed = ([], None, []) if a.vanilla_only else archive_load_order(a.profile)
    if modlist:
        print("archive order set by modlist.txt from mod:", modlist[0])
        if not listed or listed[0] != ARCHIVE_NAME.lower() + ".archive":
            print("  WARNING: %s.archive is not the first line of %s;\n"
                  "  every archive listed there loads first and overrides our patched copies." % (ARCHIVE_NAME, modlist[1]))
    print("enabled mod archives:", len(load))
    # one extraction over all of them, in reverse load order, so the winner is written last ...
    rev = [p for f, m, p in reversed(load)]
    for i in range(0, len(rev), 200):
        subprocess.run([WK, "extract", *rev[i:i + 200], "-o", mods_out, "-r", PATH_RX, "-v", "Minimal"],
                       capture_output=True, text=True)
    got = rel_files(mods_out) if os.path.isdir(mods_out) else {}
    # ... then, without trusting the extractor's write order, re-take every file that more than
    # one mod archive holds from the archive that loads first
    want = {fnv1a64(k): k for k in got}
    holders = {}
    for f, m, p in load:
        for h in archive_hashes(p) & want.keys():
            holders.setdefault(want[h], []).append((f, m, p))
    redo = {}
    for k, hs in holders.items():
        if len(hs) > 1:
            redo.setdefault(hs[0][2], []).append(k)
            print("  conflict: %s -> %s (over %s)" % (k, hs[0][0], ", ".join(h[0] for h in hs[1:])))
    for p, ks in redo.items():
        tmp = os.path.join(work, "winner")
        shutil.rmtree(tmp, ignore_errors=True)
        rx = "^(" + "|".join(re.escape(k) for k in ks) + ")$"
        run([WK, "extract", p, "-o", tmp, "-r", rx, "-v", "Minimal"])
        for k in ks:
            shutil.copyfile(os.path.join(tmp, k), got[k])
    added = 0
    for k, p in got.items():
        if k not in sources:
            added += 1
            origin[k] = "mod (added)"
        elif open(p, "rb").read() != open(sources[k], "rb").read():
            origin[k] = "mod (replaced)"
        else:
            continue
        sources[k] = p
    print("mod effects in these folders: %d (%d added, %d replaced)" % (len(got), added,
          sum(1 for v in origin.values() if v == "mod (replaced)")))
    print("effects to consider:", len(sources))

    # 3. copy, serialize, patch
    raw = os.path.join(work, "raw")
    for k, p in sources.items():
        dst = os.path.join(raw, k)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(p, dst)
    run([WK, "convert", "s", raw, "-v", "Minimal"])
    report = []
    keep = set()
    for k in sources:
        j = os.path.join(raw, k) + ".json"
        if not os.path.exists(j):
            report.append("NO JSON  " + k)
            continue
        n, names = patch_json(j, a.floor, a.drag, a.column_wind)
        if n:
            keep.add(k)
            report.append("%2d  %-90s %s  [%s]" % (n, k, origin[k], ", ".join(sorted(set(names)))[:120]))
    print("effects patched:", len(keep))
    open(os.path.join(HERE, "smoke_patch_report.txt"), "w", encoding="utf-8").write("\n".join(sorted(report)))
    if a.dry_run:
        return

    # 4. deserialize the patched ones into a clean pack folder, pack, install
    pack = os.path.join(work, ARCHIVE_NAME)
    for k in keep:
        dst = os.path.join(pack, k) + ".json"
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(raw, k) + ".json", dst)
    run([WK, "convert", "d", pack, "-v", "Minimal"])
    for dp, dn, fn in os.walk(pack):
        for f in fn:
            if f.endswith(".json"):
                os.remove(os.path.join(dp, f))
    run([WK, "pack", pack, "-o", work, "-v", "Minimal"])
    built = os.path.join(work, ARCHIVE_NAME + ".archive")
    os.makedirs(MOD_DIR, exist_ok=True)
    try:
        shutil.copyfile(built, os.path.join(MOD_DIR, ARCHIVE_NAME + ".archive"))
    except PermissionError:
        # the game holds its archives open; install by hand once it's closed
        print("NOT INSTALLED (file locked, game running?). Built:", built, os.path.getsize(built), "bytes")
        return
    print("installed", os.path.join(MOD_DIR, ARCHIVE_NAME + ".archive"), os.path.getsize(built), "bytes")


if __name__ == "__main__":
    main()
