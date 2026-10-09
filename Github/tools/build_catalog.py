"""Scan RoA workshop content and write characters/catalog.json.

Every playable character (stock or workshop) becomes one catalog entry; the in-game
character switcher and the RoA bridge both read this file, so adding a character never
needs code changes. Stock characters are listed in characters/stock.json.
"""
import json
import re
from pathlib import Path

WORKSHOP = Path(r"G:\games\steamapps\workshop\content\383980")
ROOT = Path(__file__).resolve().parent.parent


def read_general(path):
    """Tolerant [general] reader: values are key="..." and may span several lines."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    m = re.search(r"\[general\](.*?)(?=^\[|\Z)", text, re.S | re.M)
    body = m.group(1) if m else ""
    return dict(re.findall(r'^([\w ]+)="(.*?)"\s*$', body, re.S | re.M))


def scan_workshop(root):
    out = []
    for d in sorted(root.iterdir()):
        ini = d / "config.ini"
        if not ini.is_file():
            continue
        g = read_general(ini)
        if g.get("type", "").strip() != "0":      # 0 = character, 2 = stage
            continue
        scripts = d / "scripts"
        out.append({
            "id": f"workshop:{d.name}",
            "source": "workshop",
            "workshop_id": d.name,
            "name": g.get("name", d.name).strip(),
            "author": g.get("author", "").strip(),
            "path": str(d),
            "portrait": "charselect.png" if (d / "charselect.png").is_file() else None,
            "icon": "icon.png" if (d / "icon.png").is_file() else None,
            "has_init": (scripts / "init.gml").is_file(),
            "has_attacks": (scripts / "attacks").is_dir(),
        })
    return out


def main():
    stock = json.loads((ROOT / "characters" / "stock.json").read_text())
    ws = scan_workshop(WORKSHOP)
    bad = [c["name"] for c in ws if not c["has_init"]]
    cat = {"version": 1, "characters": stock + ws}
    (ROOT / "characters" / "catalog.json").write_text(json.dumps(cat, indent=1))
    print(f"{len(stock)} stock + {len(ws)} workshop characters; {len(bad)} missing init.gml: {bad}")


if __name__ == "__main__":
    main()
