#!/bin/sh
# Copy the MSYS2-provided DLLs an exe needs into its build directory.
#
#   stage_dlls.sh <exe> <outdir> <dll-source-dir>
#
# objdump only reports direct imports, so this iterates: scan everything staged
# so far, copy any new import found in the source dir, repeat until stable.
set -e

exe="$1"
out="$2"
src="$3"

[ -n "$exe" ] && [ -n "$out" ] && [ -n "$src" ] || {
    echo "usage: stage_dlls.sh <exe> <outdir> <dll-source-dir>" >&2
    exit 2
}

count_before=-1
while :; do
    count=$(ls "$out" 2>/dev/null | grep -ci '\.dll$' || true)
    [ "$count" = "$count_before" ] && break
    count_before=$count
    for f in "$exe" "$out"/*.dll; do
        [ -f "$f" ] || continue
        objdump -p "$f" 2>/dev/null | sed -n 's/^\s*DLL Name:\s*//p' | while read -r dll; do
            dll=$(echo "$dll" | tr -d '\r')
            [ -f "$out/$dll" ] && continue
            [ -f "$src/$dll" ] || continue
            cp "$src/$dll" "$out/"
            echo "  staged $dll"
        done
    done
done

echo "runtime DLLs staged in $out/"
