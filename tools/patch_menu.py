"""patch_menu.py - build the menu override for the co-op mod from the game's OWN files.

    python tools\\patch_menu.py <GameDir> <OutDir> [--mainonly]   (default: the CO-OP button is also shown in the pause menu; --mainonly hides it there)

Reads the stock ui/main.menu and ui/menus.txt out of the game's own archives (main\\iw_*.iwd only: a zz_coop archive that
is already installed is never read back), and writes into OutDir:
  ui/main.menu   stock + a CO-OP button in the main screen (menuDef main_text), the buttons below it moved down 30
  ui/menus.txt   stock + loadMenu { "ui/coop.menu" }
  ui/coop.menu   launcher/menu/coop.menu, copied as it is
Every anchor is asserted: if the stock files are not what this expects, it fails loudly instead of writing a broken menu.
"""

import glob
import os
import re
import shutil
import sys
import zipfile


def read_stock(game, name):
    found = []
    for f in sorted(glob.glob(os.path.join(game, "main", "iw_*.iwd"))):
        z = zipfile.ZipFile(f)
        if name in z.namelist():
            found.append((f, z.read(name).decode("latin-1")))
    if len(found) != 1:
        sys.exit(f"expected {name} in exactly one stock archive, found {len(found)}")
    return found[0][1]


def block_end(t, brace_open):
    depth = 0
    for i in range(brace_open, len(t)):
        if t[i] == "{":
            depth += 1
        elif t[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    sys.exit("unbalanced braces in the stock menu")


def item_spans(blk):
    """(start, end) of every itemDef { ... } in a menuDef block, end exclusive."""
    spans, i = [], 0
    while True:
        j = blk.find("itemDef", i)
        if j < 0:
            return spans
        k = blk.index("{", j)
        e = block_end(blk, k) + 1
        spans.append((j, e))
        i = e


def patch_main(t, ingame=False):
    m = re.search(r"name\s+main_text\b", t)
    if not m:
        sys.exit("menuDef main_text not found")
    ms = t.rfind("menuDef", 0, m.start())
    me = block_end(t, t.index("{", ms)) + 1
    blk = t[ms:me]
    spans = item_spans(blk)

    def name_of(it):
        mm = re.search(r'name\s+"?([A-Za-z_0-9]+)', it)
        return mm.group(1) if mm else ""

    opts = [(s, e) for s, e in spans if name_of(blk[s:e]) == "options"]
    if len(opts) != 1:
        sys.exit(f"expected one 'options' button in main_text, found {len(opts)}")
    s0, e0 = opts[0]
    tmpl = blk[s0:e0]
    y = re.search(r"(rect\s+MAIN_RECT_X\s+)(\d+)(\s)", tmpl)
    if not y:
        sys.exit("options button has no 'rect MAIN_RECT_X <y>'")
    base_y = int(y.group(2))

    # the new button: a clone of 'options' with its own name, text, action, shown only outside a running game
    new = tmpl
    new = re.sub(r"(name\s+)\"?options\"?", r"\1coop", new, count=1)
    new = re.sub(r'(\btext\s+)"[^"]*"', r'\1"CO-OP"', new, count=1)
    new, n = re.subn(r"action\s*\{[^}]*\}", 'action\n\t\t\t{\n\t\t\t\tplay "mouse_click";\n\t\t\t\topen "coop_menu";\n\t\t\t}', new, count=1)
    if n != 1:
        sys.exit("could not replace the action of the cloned button")
    if "dvarTest" in new:
        sys.exit("the 'options' button already has a dvarTest: the clone would need a different rule")
    if not ingame:   # default: only in the main menu; --ingame also shows it in the pause menu (the same 'main' menu while a mission runs)
        last = new.rindex("}")
        new = new[:last] + '\t\tdvarTest "sv_running"\n\t\tshowDvar { "0" }\n\t}' + new[last + 1:]

    # shift every main-screen button that sits at or below the options button down by 30, then put the clone in the gap
    shifted = []
    for s, e in spans:
        it = blk[s:e]
        mm = re.search(r"(rect\s+MAIN_RECT_X\s+)(\d+)(\s)", it)
        nm = name_of(it)
        if mm and nm in ("options", "playselection", "credit", "quit"):
            it = it[:mm.start(2)] + str(int(mm.group(2)) + 30) + it[mm.end(2):]
        shifted.append((s, e, it))
    out, pos = [], 0
    for s, e, it in shifted:
        out.append(blk[pos:s])
        if (s, e) == (s0, e0):
            out.append(new + "\n\t\t")
        out.append(it)
        pos = e
    out.append(blk[pos:])
    blk2 = "".join(out)
    if len(re.findall(r'name\s+"?coop"?\s', blk2)) != 1 or blk2.count('open "coop_menu"') != 1:
        sys.exit("the CO-OP button was not inserted exactly once")
    # the clone took the options button's y; the options button itself moved down by 30
    return t[:ms] + blk2 + t[me:], base_y



def patch_hud(t):
    """ui/hud.menu: the partner's state (label + value + red value) next to the compass, in the same menuDef so it hides with the compass.
    The values come from the dvars coop_partner / coop_partner_on / coop_partner_low that the mod script sets."""
    m = re.search(r'name\s+"Compass"', t)
    if not m:
        sys.exit('menuDef "Compass" not found in ui/hud.menu')
    ms = t.rfind("menuDef", 0, m.start())
    me = block_end(t, t.index("{", ms))

    def item(name, extra, rect):
        return (
            "\t\titemDef\n\t\t{\n"
            f'\t\t\tname "{name}"\n'
            f"\t\t\trect {rect}\n"
            "\t\t\ttextscale .32\n"
            "\t\t\ttextstyle ITEM_TEXTSTYLE_SHADOWED\n"
            "\t\t\ttextalign 0\n"
            "\t\t\ttextaligny 12\n"
            "\t\t\tdecoration\n"
            "\t\t\tvisible MENU_TRUE\n"
            f"{extra}"
            "\t\t}\n")

    on = '\t\t\tdvartest "coop_partner_on"\n\t\t\tshowdvar { "1" }\n'
    label = item("coop_partner_label", '\t\t\ttext "@COOP_PARTNER"\n\t\t\tforecolor .85 .85 .85 1\n' + on, "150 112 70 14")
    val = item("coop_partner_value", '\t\t\ttype ITEM_TYPE_TEXT\n\t\t\tdvar "coop_partner"\n\t\t\tforecolor 1 1 1 1\n\t\t\tdvartest "coop_partner_low"\n\t\t\tshowdvar { "0" }\n', "215 112 60 14")
    low = item("coop_partner_low", '\t\t\ttype ITEM_TYPE_TEXT\n\t\t\tdvar "coop_partner"\n\t\t\tforecolor 1 .3 .3 1\n\t\t\tdvartest "coop_partner_low"\n\t\t\tshowdvar { "1" }\n', "215 112 60 14")
    return t[:me] + "\n" + label + val + low + "\t" + t[me:]

def patch_menus_txt(t):
    anchor = 'loadMenu { "ui/main.menu" }'
    if t.count(anchor) != 1:
        sys.exit("ui/menus.txt: the main.menu anchor is not unique")
    return t.replace(anchor, anchor + '\n\tloadMenu { "ui/coop.menu" }')


def main():
    game, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "ui"), exist_ok=True)
    ingame = "--mainonly" not in sys.argv[3:]   # default: the CO-OP button is also in the pause menu
    main_t, y = patch_main(read_stock(game, "ui/main.menu"), ingame)
    with open(os.path.join(out, "ui", "main.menu"), "w", encoding="latin-1", newline="") as f:
        f.write(main_t)
    with open(os.path.join(out, "ui", "hud.menu"), "w", encoding="latin-1", newline="") as f:
        f.write(patch_hud(read_stock(game, "ui/hud.menu")))
    with open(os.path.join(out, "ui", "menus.txt"), "w", encoding="latin-1", newline="") as f:
        f.write(patch_menus_txt(read_stock(game, "ui/menus.txt")))
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launcher", "menu", "coop.menu")
    shutil.copyfile(src, os.path.join(out, "ui", "coop.menu"))
    print(f"menu patched: CO-OP button at y={y}, options/multiplayer/credits/quit moved down 30 -> {out}")


if __name__ == "__main__":
    main()
