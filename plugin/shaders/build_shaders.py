"""Builds the foliage-wind shader patch the plugin embeds (REDGraph client, 0.8.0).

The patch is the game's own wind-impulse "shapes" compute shader (m_windImpulseVolumeUpdate_Shapes,
HASH b88af93e421f88ee278f2ba00f214d6d) with one edit: where it stores each cell of the 64^3 wind
impulse volume, it also adds a cell of Night City Winds' local wind, read from a 16 x 16 grid in a
constant buffer at b3 (REDGraph leases b3 into the engine's own descriptor table). Everything else
is the vanilla shader's bytecode, so its resource layout and behaviour stay the engine's.

Nothing of CDPR's is kept in the repository: the vanilla bytecode is extracted from the installed
game's shader cache at build time, edited, assembled and validated, and written as a C++ header
into plugin/build/generated (ignored by git).

    python -I build_shaders.py --game "G:\\...\\Cyberpunk 2077" --out ..\\build\\generated

Needs REDGraph's SDK tools (deps/REDGraph/sdk/tools) and a DXC with dxcompiler.dll + dxil.dll
(the Windows SDK's bin/<ver>/x64 works). Standard library only.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent.parent / "deps" / "REDGraph" / "sdk" / "tools"
SHAPES = "b88af93e421f88ee278f2ba00f214d6d"

# --- the edit ------------------------------------------------------------------------------------
# Cell (x, y, z) of the volume is thread (%24, %25, %26); the shader has just summed the impulse
# shapes into (%296..%299), faded them toward the volume's edges by %295, added the previous
# frame's cell (%301..%304) as (%305..%308) and stores that. We add grid[(x >> 2) + (y >> 2) * 16]
# x %295 (the same edge fade): one entry per 2 m x 2 m column, the plugin's wind deviation there.

TYPE_ANCHOR = "%hostlayout.SharedPixelConsts = type"
TYPE_LINE = "%hostlayout.NCWGrid = type { [256 x <4 x float>] }\n"

HANDLE_ANCHOR = ("  %7 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 12, i1 false)"
                 "  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)\n")
HANDLE_LINE = "  %ncw.h = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 1, i32 3, i1 false)\n"

STORE_OLD = ("  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %1, i32 %24, i32 %25, i32 %26, "
             "float %305, float %306, float %307, float %308, i8 15)")
STORE_NEW = """  %ncw.ix = lshr i32 %24, 2
  %ncw.iy = lshr i32 %25, 2
  %ncw.iys = shl i32 %ncw.iy, 4
  %ncw.ixy = or i32 %ncw.ix, %ncw.iys
  %ncw.i = and i32 %ncw.ixy, 255
  %ncw.r = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %ncw.h, i32 %ncw.i)
  %ncw.x = extractvalue %dx.types.CBufRet.f32 %ncw.r, 0
  %ncw.y = extractvalue %dx.types.CBufRet.f32 %ncw.r, 1
  %ncw.z = extractvalue %dx.types.CBufRet.f32 %ncw.r, 2
  %ncw.w = extractvalue %dx.types.CBufRet.f32 %ncw.r, 3
  %ncw.fx = fmul fast float %ncw.x, %295
  %ncw.fy = fmul fast float %ncw.y, %295
  %ncw.fz = fmul fast float %ncw.z, %295
  %ncw.fw = fmul fast float %ncw.w, %295
  %ncw.ox = fadd fast float %305, %ncw.fx
  %ncw.oy = fadd fast float %306, %ncw.fy
  %ncw.oz = fadd fast float %307, %ncw.fz
  %ncw.ow = fadd fast float %308, %ncw.fw
  call void @dx.op.textureStore.f32(i32 67, %dx.types.Handle %1, i32 %24, i32 %25, i32 %26, float %ncw.ox, float %ncw.oy, float %ncw.oz, float %ncw.ow, i8 15)"""

CB_LIST_OLD = "!17 = !{!18}\n"
CB_LIST_NEW = "!17 = !{!18, !100}\n"
CB_META = "!100 = !{i32 1, %hostlayout.NCWGrid* undef, !\"\", i32 0, i32 3, i32 1, i32 4096, null}\n"


def replace_once(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit(f"edit anchor '{what}' found {text.count(old)} times (the game's shader changed?)")
    return text.replace(old, new)


def edit(ll: str) -> str:
    if f"; shader hash: {SHAPES}" not in ll:
        raise SystemExit("not the shapes shader")
    ll = replace_once(ll, TYPE_ANCHOR, TYPE_LINE + TYPE_ANCHOR, "type")
    ll = replace_once(ll, HANDLE_ANCHOR, HANDLE_ANCHOR + HANDLE_LINE, "handle")
    ll = replace_once(ll, STORE_OLD, STORE_NEW, "store")
    ll = replace_once(ll, CB_LIST_OLD, CB_LIST_NEW, "cbuffer list")
    return ll.rstrip("\n") + "\n" + CB_META


def run(*args):
    r = subprocess.run([sys.executable, "-I", *map(str, args)], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"{args[0]} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="the Cyberpunk 2077 install (holds engine/)")
    ap.add_argument("--dxc", required=True, help="folder with dxc.exe, dxcompiler.dll, dxil.dll")
    ap.add_argument("--out", required=True, help="folder for NCWFoliagePatch.hpp")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        run(TOOLS / "rg_shader_cache.py", "extract", SHAPES, "--game", a.game, "--out", t)
        vanilla = t / f"{SHAPES}.dxil"
        run(TOOLS / "rg_dxil.py", "dis", vanilla, t / "vanilla.ll", "--dxc", a.dxc)
        ll = (t / "vanilla.ll").read_text(encoding="utf-8")
        (t / "patched.ll").write_text(edit(ll), encoding="utf-8")
        print(run(TOOLS / "rg_dxil.py", "asm", t / "patched.ll", t / "patched.dxil", "--dxc", a.dxc).strip())
        print(run(TOOLS / "rg_dxil.py", "header", t / "patched.dxil", out / "NCWFoliagePatch.hpp",
                  "--symbol", "FoliagePatch", "--namespace", "NCW::Shaders").strip())


if __name__ == "__main__":
    main()
