#!/bin/bash
# Gathers the build results into release files.
#
#   ci/make-release.sh BUILDS_DIR DIST_DIR [VERSION]
#
# BUILDS_DIR holds wrapped-*.rock and results-*.tsv files from ci/build-all.sh.
# DIST_DIR gets:
#   wrapped-<label>-<target>.rock          every successful build
#   rockbox-wrapped-<label>-all-players.zip  one per Rockbox version
#   release-notes.md                       a table of every player and file
set -euo pipefail
src="$(cd "${1:?usage: make-release.sh BUILDS_DIR DIST_DIR [VERSION]}" && pwd)"
mkdir -p "${2:?dist dir}"; dist="$(cd "$2" && pwd)"
version="${3:-}"
cat "$src"/results-*.tsv | sort -t$'\t' -k4,4f -k1,1 > "$dist/results.tsv"

cp "$src"/wrapped-*.rock "$dist/"
labels=$(cut -f1 "$dist/results.tsv" | sort -u)

# per-version zips, each with a list of players
for l in $labels; do
  stage=$(mktemp -d)
  {
    echo "Rockbox Wrapped${version:+ $version} for Rockbox $l"
    echo
    echo "Find your player below, rename its file to wrapped.rock and copy it"
    echo "to /.rockbox/rocks/apps/ on the player."
    echo
    awk -F'\t' -v l="$l" '$1 == l && $3 == "ok" { printf "%-45s wrapped-%s-%s.rock\n", $4, l, $2 }' "$dist/results.tsv"
  } > "$stage/PLAYERS.txt"
  cp "$dist"/wrapped-"$l"-*.rock "$stage/"
  (cd "$stage" && zip -q -9 "$dist/rockbox-wrapped-$l-all-players.zip" ./*)
  rm -rf "$stage"
done

# release notes: one row per player, one column per Rockbox version
{
  echo "## Downloads"
  echo
  echo "Check your Rockbox version under **System → Rockbox Info**, then download the file for your player."
  echo "Rename it to \`wrapped.rock\`, copy it to \`/.rockbox/rocks/apps/\`, and open **Plugins → Applications → wrapped**."
  echo "Each zip below has every player's file for one Rockbox version."
  echo
  printf '| Player |'; for l in $labels; do printf ' Rockbox %s |' "$l"; done; echo
  printf '|---|';      for l in $labels; do printf '%s' '---|'; done; echo
  cut -f2,4 "$dist/results.tsv" | sort -u -t$'\t' -k2,2f -k1,1 | while IFS=$'\t' read -r target name; do
    built=0; row="| $name |"
    for l in $labels; do
      if grep -q "^$l"$'\t'"$target"$'\tok\t' "$dist/results.tsv"; then
        row="$row \`wrapped-$l-$target.rock\` |"; built=1
      else
        row="$row – |"
      fi
    done
    if [ $built = 1 ]; then echo "$row"; fi   # (an `&&` here would end the
  done                                       #  loop "failed" under set -e)
  noclock=$(awk -F'\t' '$3 == "noclock" { print $4 }' "$dist/results.tsv" | sort -u | paste -sd, - | sed 's/,/, /g')
  failed=$(awk -F'\t' '$3 == "failed" { print $4 " (" $1 ")" }' "$dist/results.tsv" | sort -u | paste -sd, - | sed 's/,/, /g')
  echo
  if [ -n "$noclock" ]; then
    echo "Not available (no clock, so no dates to count by): $noclock."
  fi
  if [ -n "$failed" ]; then
    echo "Not built this time (build failed): $failed."
  fi
} > "$dist/release-notes.md"

echo "$(ls "$dist"/wrapped-*.rock | wc -l) plugin files, $(ls "$dist"/*.zip | wc -l) zips"
