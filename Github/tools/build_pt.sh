#!/bin/bash
# Build the patched Pizza Tower working copy (C:\pt) into C:\ptbuild\out.
#   build_pt.sh Fallback for others      compile only ("Package" does not refresh the .win, so we use Run and let it exit)
#   build_pt.sh run Fallback for others  compile and launch (Igor "Run"); blocks until the game exits
# Bridge files are staged BEFORE Igor so a launched game finds them.
set -e
R=/c/ProgramData/GameMakerStudio2-LTS/Cache/runtimes/runtime-2022.0.3.99
# GameMaker user folder (the one holding your licence / local settings): set GMS_USER, e.g. C:/Users/<you>/AppData/Roaming/GameMakerStudio2-LTS/<account>
GMS_USER="${GMS_USER:?set GMS_USER to your GameMaker user folder}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/c/ptbuild/out
mkdir -p $OUT
python "$HERE/pt-bridge/install.py" C:/pt
cp "$HERE/pt-bridge/extension/pizzarivals_pt.dll" $OUT/
[ -f "$HERE/characters/catalog.json" ] || python "$HERE/tools/build_catalog.py"      # (lists your Workshop characters)
cp "$HERE/characters/catalog.json" $OUT/pizzarivals_catalog.json
[ -f $OUT/pizzarivals_music.cfg ] || cp "$HERE/characters/pizzarivals_music.cfg" $OUT/
"$R/bin/igor/windows/x64/Igor.exe" --project=C:/pt/PizzaTower_GM2.yyp \
  --user="$GMS_USER" \
  --runtimePath=$R --cache=C:/ptbuild/cache --temp=C:/ptbuild/temp --of=$OUT/PizzaTower_GM2.win \
  windows Run > /c/ptbuild/log.txt 2>&1 || true
grep -n -i -E "error|rivals" /c/ptbuild/log.txt | head -20
