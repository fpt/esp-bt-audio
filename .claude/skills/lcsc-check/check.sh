#!/usr/bin/env bash
# Compare each symbol's LCSC part package against its KiCad footprint.
# Usage: check.sh [board.kicad_sch ...]   (default: both boards in this repo)
# Needs kicad-cli and curl. Queries the public LCSC product API, one request per part.
set -euo pipefail

K="${KICAD_CLI:-/c/Program Files/KiCad/10.0/bin/kicad-cli.exe}"
root="$(cd "$(dirname "$0")/../../.." && pwd)"
[ $# -gt 0 ] || set -- "$root/main/esp-bt-audio.kicad_sch" "$root/i2s/i2s-power.kicad_sch"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Normalise a package name so 0603 == C_0603_1608Metric, SO-8 == SOP-8 == SOIC-8.
norm() {
  tr '[:lower:]' '[:upper:]' <<<"$1" | sed -E 's/SOIC|SOP/SO/g; s/[^A-Z0-9]//g'
}

status=0
for sch in "$@"; do
  name="$(basename "$sch" .kicad_sch)"
  bom="$tmp/$name.csv"
  "$K" sch export bom --fields 'Reference,Value,Footprint,LCSC' \
    --group-by 'Value,Footprint,LCSC' --ref-range-delimiter '' \
    --field-delimiter ';' --string-delimiter '' -o "$bom" "$sch" >/dev/null 2>&1
  echo "== $name"
  printf '%-6s %-14s %-10s %-12s %-28s %s\n' RESULT REFS LCSC PACKAGE FOOTPRINT DESCRIPTION
  while IFS=';' read -r refs value fp lcsc; do
    fpname="${fp#*:}"
    if [ -z "$lcsc" ]; then
      printf '%-6s %-14s %-10s %-12s %-28s %s\n' NOLCSC "$refs" - - "$fpname" "$value"
      continue
    fi
    json="$(curl -s -m 20 "https://wmsc.lcsc.com/ftps/wm/product/detail?productCode=$lcsc" || true)"
    pkg="$(grep -o '"encapStandard":"[^"]*"' <<<"$json" | head -1 | cut -d'"' -f4 || true)"
    desc="$(grep -o '"productIntroEn":"[^"]*"' <<<"$json" | head -1 | cut -d'"' -f4 | cut -c1-70 || true)"
    if [ -z "$pkg" ]; then
      result=LOOKUP
      status=1
    elif [[ "$(norm "$fpname")" == *"$(norm "$pkg")"* ]]; then
      result=OK
    else
      result=CHECK
      status=1
    fi
    printf '%-6s %-14s %-10s %-12s %-28s %s\n' "$result" "$refs" "$lcsc" "${pkg:--}" "$fpname" "$desc"
  done < <(tail -n +2 "$bom")
  echo
done
exit $status
