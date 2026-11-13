#!/bin/bash
# SPDX-FileCopyrightText: 2026 Charis
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the folder that is deployed to charis-conta.netlify.app: the account server and web
# panel from this directory, plus the newest PC installer and Switch app under /atualizacoes/
# (the apps check pc.txt and switch.txt there). The binaries are not kept in git.
#
#   stage-release.sh CharisSwitch-X.Y.Z-instalador.exe charis-stream.nro <output folder>
set -euo pipefail
installer="$1"
nro="$2"
out="$3"
here="$(cd "$(dirname "$0")" && pwd)"
pc_version="$(basename "$installer" | sed -n 's/^CharisSwitch-\([0-9.]*\)-instalador\.exe$/\1/p')"
switch_version="$(sed -n 's/.*CHARIS_APP_VERSION "\(.*\)"/\1/p' "$here/../charis-stream-switch/source/version.h")"
[ -n "$pc_version" ] || { echo "installer name must be CharisSwitch-X.Y.Z-instalador.exe"; exit 1; }
[ -n "$switch_version" ] || { echo "no Switch app version"; exit 1; }

rm -rf "${out:?}"
mkdir -p "$out"
(cd "$here" && git ls-files | grep -v -e '^netlify.toml$' -e '^stage-release.sh$') | while read -r f; do
  mkdir -p "$out/$(dirname "$f")"
  cp "$here/$f" "$out/$f"
done
cat > "$out/netlify.toml" <<'TOML'
"$schema" = "https://open-api.netlify.com/netlify.schema.json"

[build]
  publish = "public"
  command = "npm install --no-audit --no-fund"

[functions]
  node_bundler = "esbuild"
TOML
mkdir -p "$out/public/atualizacoes"
cp "$installer" "$out/public/atualizacoes/CharisSwitch-$pc_version-instalador.exe"
cp "$nro" "$out/public/atualizacoes/charis-stream-$switch_version.nro"
printf 'versao=%s\narquivo=CharisSwitch-%s-instalador.exe\n' "$pc_version" "$pc_version" > "$out/public/atualizacoes/pc.txt"
printf 'versao=%s\narquivo=charis-stream-%s.nro\n' "$switch_version" "$switch_version" > "$out/public/atualizacoes/switch.txt"
echo "PC $pc_version, Switch $switch_version -> $out"
