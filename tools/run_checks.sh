#!/bin/bash
# Все проверки движка Arranger перед сборкой (правило «ничего не возвращается», sound 30.10).
# ./tools/run_checks.sh <DZODArrangerTool> <rigor-suno-versions-2026-10-25> <папка «аудит» с русской копией 04B> <song-sections-2026-10-25> <кэш> <рабочая папка>
set -u
T="$1"; R="$2"; RU="$3"; SONGS="$4"; C="$5"; W="$6"
rm -rf "$W"; mkdir -p "$W"
"$T" selftest > "$W/selftest.txt" 2>&1;            s1=$?
"$T" regress "$R" "$C" "$W/regress" > "$W/regress.txt" 2>&1; s2=$?
"$T" audit "$R" "$RU" "$SONGS" "$C" "$W/audit" > "$W/audit.txt" 2>&1; s3=$?
"$T" stress "$R" "$C" "$W/stress" 4 > "$W/stress.txt" 2>&1; s4=$?
"$T" phasetest "$W/phase" "$W/phase-cache" > "$W/phase.txt" 2>&1; s5=$?
grep -E "^selftest" "$W/selftest.txt"; grep -E "^regressions" "$W/regress.txt"; grep -E "^audit" "$W/audit.txt"; grep -cE "^seed .*: OK" "$W/stress.txt" | sed 's/^/stress OK: /'; grep -E "^phasetest" "$W/phase.txt"
grep -hE "FAIL" "$W"/*.txt
[ $s1 -eq 0 ] && [ $s2 -eq 0 ] && [ $s3 -eq 0 ] && [ $s4 -eq 0 ] && [ $s5 -eq 0 ] && echo "ALL CHECKS PASSED" || { echo "CHECKS FAILED - do not ship"; exit 1; }
