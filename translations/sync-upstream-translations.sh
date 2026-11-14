#!/bin/bash
# Pull finished translations from upstream's deskflow_*.ts into the autodeskflow_*.ts files we ship.
#
# Run after merging upstream. Upstream wins where both files translate the same message;
# messages only we have are kept. Afterwards rebuild, deleting
# <build>/translations/.lupdate/*.stamp first: the .qm files depend on lupdate's stamp,
# not on the .ts, so hand-edited .ts files are not recompiled otherwise.
set -euo pipefail
cd "$(dirname "$0")"

LCONVERT="${LCONVERT:-$(command -v lconvert || echo /opt/homebrew/opt/qt/bin/lconvert)}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for upstream in deskflow_*.ts; do
  lang=${upstream#deskflow_}
  ours=autodeskflow_$lang
  [ -f "$ours" ] || continue
  "$LCONVERT" -no-obsolete -no-untranslated -i "$upstream" -o "$tmp/$lang"
  "$LCONVERT" -i "$ours" -i "$tmp/$lang" -o "$ours"
  echo "merged $upstream -> $ours"
done
