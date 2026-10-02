"""Put the smoke archive on the first line of every enabled modlist.txt (CyberVision ships one;
see build_smoke.archive_load_order), backing each file up first. --undo restores the backups.
The modpack's own updates overwrite modlist.txt, so re-run this after updating it."""
import os, shutil, sys
from build_smoke import ARCHIVE_NAME, MO2, enabled_mods

PROFILE = "04 - CyberVision - PATH TRACING Very High - RTX 5070 TI"
BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backups", "modlist")
LINE = ARCHIVE_NAME + ".archive"

for m in enabled_mods(PROFILE):
    p = os.path.join(MO2, "mods", m, "archive", "pc", "mod", "modlist.txt")
    if not os.path.isfile(p):
        continue
    bak = os.path.join(BACKUP, m + ".modlist.txt")
    if "--undo" in sys.argv:
        if os.path.isfile(bak):
            shutil.copyfile(bak, p)
            print("restored", p)
        continue
    raw = open(p, "rb").read()
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    lines = raw.split(nl)
    if lines and lines[0].strip().lower() == LINE.lower().encode():
        print("already first:", p)
        continue
    os.makedirs(BACKUP, exist_ok=True)
    if not os.path.isfile(bak):
        shutil.copyfile(p, bak)
    lines = [l for l in lines if l.strip().lower() != LINE.lower().encode()]
    open(p, "wb").write(nl.join([LINE.encode()] + lines))
    print("listed first in", p, "(backup:", os.path.normpath(bak) + ")")
