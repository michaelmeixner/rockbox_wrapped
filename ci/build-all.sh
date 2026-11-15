#!/bin/bash
# Builds wrapped.rock for every player that uses the given toolchains.
#
#   ci/build-all.sh ROCKBOX_TREE LABEL ARCH_LETTERS OUTDIR [TARGET...]
#
# LABEL names the Rockbox version in the file names (e.g. "4.0", "dev").
# Each player gets OUTDIR/wrapped-LABEL-TARGET.rock, and every result is
# recorded in OUTDIR/results-LABEL-ARCH.tsv as
#   label <TAB> target <TAB> ok|noclock|failed <TAB> full name
# Players without a clock never build the plugin (SOURCES has #if CONFIG_RTC)
# and are recorded as "noclock". Exits 1 if any player failed to compile.
# Pass TARGET names to build only those.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# pwd -P: the real (short) path, not a symlink's. Rockbox's dependency step
# fails with "Argument list too long" when the tree's path is long.
tree="$(cd "${1:?usage: build-all.sh ROCKBOX_TREE LABEL ARCH_LETTERS OUTDIR [TARGET...]}" && pwd -P)"
label="${2:?label}"; arch="${3:?arch letters}"
mkdir -p "${4:?outdir}"; out="$(cd "$4" && pwd)"; shift 4
only=" $* "

bash "$here/prepare-tree.sh" "$tree" || exit 1

results="$out/results-$label-$arch.tsv"
: > "$results"
failed=0
while read -r target a name; do
  [ "$only" = "  " ] || [[ "$only" == *" $target "* ]] || continue
  dir="$tree/b"                      # short on purpose (see above)
  rock="$dir/apps/plugins/wrapped.rock"
  log="$dir/build.log"
  rm -rf "$dir"; mkdir -p "$dir"
  if ! (cd "$dir" && ../tools/configure --target="$target" --type=n < /dev/null > "$log" 2>&1); then
    echo "FAILED  $target ($name): configure"; tail -5 "$log" | sed 's/^/    /'
    mkdir -p "$out/logs"; cp "$log" "$out/logs/configure-$label-$target.log"
    printf '%s\t%s\tfailed\t%s\n' "$label" "$target" "$name" >> "$results"
    failed=1; continue
  fi
  # Ask the build for its plugin list (SOURCES run through the target's own
  # config). Wrapped is only listed on players with a clock (#if CONFIG_RTC).
  plugins=$(cd "$dir" && make --no-print-directory -s -f Makefile \
              -f "$here/print-plugins.mk" print-plugins 2>> "$log")
  if [[ " $plugins " != *"/wrapped.c "* ]]; then
    echo "noclock $target ($name)"
    printf '%s\t%s\tnoclock\t%s\n' "$label" "$target" "$name" >> "$results"
    rm -rf "$dir"; continue
  fi
  if (cd "$dir" && make -j"$(nproc)" "$dir/apps/plugins/wrapped.rock" >> "$log" 2>&1); then
    cp "$rock" "$out/wrapped-$label-$target.rock"
    warn=$(grep -c 'wrapped\.c.*warning' "$log" || true)
    echo "ok      $target ($name)$( [ "$warn" -gt 0 ] && echo ", $warn warnings")"
    grep 'wrapped\.c.*warning' "$log" | head -5 | sed 's/^/    /'
    printf '%s\t%s\tok\t%s\n' "$label" "$target" "$name" >> "$results"
  else
    echo "FAILED  $target ($name)"
    grep -E 'error|Error' "$log" | head -8 | sed 's/^/    /'
    mkdir -p "$out/logs"; cp "$log" "$out/logs/build-$label-$target.log"
    printf '%s\t%s\tfailed\t%s\n' "$label" "$target" "$name" >> "$results"
    failed=1
  fi
  rm -rf "$dir"                      # keep CI disk use down
done < <(bash "$here/list-targets.sh" "$tree" "$arch")

echo "---"
for s in ok noclock failed; do
  printf '%-8s %s\n' "$s" "$(grep -c $'\t'"$s"$'\t' "$results" || true)"
done
exit $failed
