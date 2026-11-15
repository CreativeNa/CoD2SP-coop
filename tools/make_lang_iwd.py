r"""make_lang_iwd.py - dev builds: write <gamedir>\\main\\zz_coop_lang.iwd, the archive that carries localizedstrings/coop.str in the language of the game.

The language comes from <gamedir>\\coop_lang.txt (a row number 0..5, a test aid) or else from <gamedir>\\localization.txt (the engine's own
language source). The text of each language is built by tools/make_coop_str.py (run it first when the table changed).
Usage: python tools/make_lang_iwd.py <gamedir> [<outdir>]   (the archive goes to <outdir>\main, default <gamedir>)
"""
import os, re, sys, zipfile

ROWS = ["english", "french", "german", "italian", "spanish", "russian"]
game = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)

row = 0
p = os.path.join(game, "coop_lang.txt")
if os.path.exists(p):
    m = re.search(r"\d+", open(p).read())
    if m and 0 <= int(m.group()) <= 5:
        row = int(m.group())
else:
    p = os.path.join(game, "localization.txt")
    if os.path.exists(p):
        w = re.search(r"[A-Za-z]+", open(p, errors="replace").read())
        if w and w.group().lower() in ROWS:
            row = ROWS.index(w.group().lower())

# build the file of that language with the generator's own function
import importlib.util
spec = importlib.util.spec_from_file_location("coopstr", os.path.join(here, "make_coop_str.py"))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)          # also regenerates launcher/menu/coop.str and bridge/coop_text.h (harmless, same table)
data = mod.build_str([mod.LANGS[row]])

out = os.path.join(sys.argv[2] if len(sys.argv) > 2 else game, "main", "zz_coop_lang.iwd")
with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
    z.writestr("localizedstrings/coop.str", data)
print("zz_coop_lang.iwd: text set %d (%s), %d bytes" % (row, ROWS[row], os.path.getsize(out)))
