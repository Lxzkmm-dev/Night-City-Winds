import re, sys
exe = r"G:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
data = open(exe, "rb").read()
out = open(sys.argv[1], "w", encoding="utf-8")
n = 0
for m in re.finditer(rb'[\x20-\x7e]{5,}', data):
    out.write("%08X %s\n" % (m.start(), m.group().decode("ascii")))
    n += 1
print(n)
