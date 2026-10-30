#!/usr/bin/env bash
# Builds a Sierra-III release from this checkout on a clean Linux machine: the
# EBOOT, the two helper PRX modules and a zip laid out for the Memory Stick.
#
#   tools/build_release.sh <output folder>
#
# Needs: the pspdev toolchain, git, Python 3, ninja, cmake, make
# (docs/toolchain.md says how to get each). The engine and the Media Engine
# library are fetched at the commits upstream/pins.env pins, built once into a
# cache, and reused by later builds.
#
# environment:
#   PSPDEV   the pspdev toolchain (required, e.g. /opt/pspdev)
#   CACHE    where the engine and me-core-mapper builds are kept
#            (default ${XDG_CACHE_HOME:-~/.cache}/sierra-iii)
#   JOBS     compile jobs (default: every CPU)
#   LABEL    the zip's name (default: the commit's short hash)
#   PSP_*    build switches, passed to psp_build.py (CLAUDE.md lists them)
#
# writes into the output folder:
#   halo_psp_<LABEL>.zip   PSP/GAME/halo/ (EBOOT.PBP, halo_crash.prx,
#                          halo_profile.prx, halo_sleep.prx,
#                          LICENSE-mecore.md, licenses/),
#                          halo/presets.txt when psp/presets.txt exists, and
#                          the campaign's all-levels profile in halo/save/
#   halo.elf, halo.map     the linked program and its link map
#   build.log              everything the build printed
#
# The same commit builds the same bytes on any machine: the compile records
# no paths or times, and the zip's files carry the commit's time. Reads this
# checkout and the network (the pinned upstreams); writes only under the
# output folder and CACHE.
set -euo pipefail

SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: tools/build_release.sh <output folder>}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
: "${PSPDEV:?PSPDEV must name the pspdev toolchain (docs/toolchain.md)}"
CACHE=${CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/sierra-iii}
JOBS=${JOBS:-$(nproc)}
LABEL=${LABEL:-$(git -C "$SRC" rev-parse --short=10 HEAD)}
export PATH="$PSPDEV/bin:$PATH"
mkdir -p "$CACHE"

# the pins: only the names this script uses are taken from the file
eval "$(cd "$SRC" && set -a && . upstream/pins.env && set +a && env | grep -E '^(ENGINE|MECORE)_')"

log() { echo "build: $*" | tee -a "$OUT/build.log"; }
: > "$OUT/build.log"

fetch_at() {   # fetch_at <url> <full commit> <dir>: a shallow checkout of one commit
    local url=$1 commit=$2 dir=$3
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

# Two builds sharing a CACHE can want the same entry at once: the second waits
# for the first, then finds the entry built. Entries are built in <dir>.part
# and renamed into place, so a half-built one is never used.
lock_entry() {
    exec 8>"$(dirname "$1")/.$(basename "$1").lock"
    flock -n 8 || { log "another build is preparing $(basename "$1"); waiting for it"; flock 8; }
}
unlock_entry() { exec 8>&-; }

# --- the Media Engine library (me-core-mapper), once per pin -----------------
MECORE_DIR=$CACHE/mecore-$MECORE_COMMIT
lock_entry "$MECORE_DIR"
if [[ ! -f $MECORE_DIR/lib/libme-core.a ]]; then
    log "me-core-mapper $MECORE_COMMIT: building into $MECORE_DIR"
    tmp=$(mktemp -d "$OUT/mecore-src.XXXXXX")
    fetch_at "$MECORE_URL" "$MECORE_COMMIT" "$tmp"
    cmake -S "$tmp" -B "$tmp/build" -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" >> "$OUT/build.log" 2>&1
    cmake --build "$tmp/build" -j "$JOBS" >> "$OUT/build.log" 2>&1
    rm -rf "$MECORE_DIR.part"; mkdir -p "$MECORE_DIR.part/lib" "$MECORE_DIR.part/include/me-core-mapper/kernel"
    cp "$tmp"/build/libme-core*.a "$MECORE_DIR.part/lib/"
    for h in "$tmp"/*.h; do [[ $h == *me-core-mapping.def.h ]] || cp "$h" "$MECORE_DIR.part/include/me-core-mapper/"; done
    cp "$tmp"/kernel/*.h "$MECORE_DIR.part/include/me-core-mapper/kernel/"
    cp "$tmp/LICENSE.md" "$MECORE_DIR.part/LICENSE.md"
    rm -rf "$MECORE_DIR"; mv "$MECORE_DIR.part" "$MECORE_DIR"; rm -rf "$tmp"
fi
unlock_entry

# --- the engine, checked out and configured once per pin ---------------------
url=$ENGINE_CHUPA_URL; commit=$ENGINE_CHUPA_COMMIT
ENGINE_DIR=$CACHE/engine-chupa-$commit
lock_entry "$ENGINE_DIR"
if [[ ! -f $ENGINE_DIR/build/linux/halo_msvc_semantics.h ]]; then
    log "engine $commit: checking out into $ENGINE_DIR"
    rm -rf "$ENGINE_DIR.part"
    fetch_at "$url" "$commit" "$ENGINE_DIR.part"
    (cd "$ENGINE_DIR.part" && python3 configure.py --no-game-browser >> "$OUT/build.log" 2>&1 \
        && ninja build/linux/halo_msvc_semantics.h build/linux/platform_msvc_semantics.h >> "$OUT/build.log" 2>&1)
    rm -rf "$ENGINE_DIR"; mv "$ENGINE_DIR.part" "$ENGINE_DIR"
fi
unlock_entry

# --- the port --------------------------------------------------------------------
log "Sierra-III $(git -C "$SRC" rev-parse HEAD), $JOBS jobs"
(cd "$SRC" && HALO_REPO="$ENGINE_DIR" PSP_OUT="$OUT/build" PSP_MECORE="$MECORE_DIR" \
    python3 psp_build.py --jobs "$JOBS") >> "$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
# the three helper modules, built from copies so the checkout stays untouched
# (the same bytes as built in place)
for module in crash profile sleep; do
    rm -rf "$OUT/$module"; cp -r "$SRC/psp/$module" "$OUT/$module"
    make -C "$OUT/$module" clean >> "$OUT/build.log" 2>&1
    make -C "$OUT/$module" >> "$OUT/build.log" 2>&1
done

# --- the release zip -----------------------------------------------------------
pkg=$OUT/pkg; rm -rf "$pkg"; mkdir -p "$pkg/PSP/GAME/halo"
cp "$OUT/build/EBOOT.PBP" "$OUT/crash/halo_crash.prx" "$OUT/profile/halo_profile.prx" "$OUT/sleep/halo_sleep.prx" \
    "$pkg/PSP/GAME/halo/"
cp "$MECORE_DIR/LICENSE.md" "$pkg/PSP/GAME/halo/LICENSE-mecore.md"
# the toolchain's libraries linked into the EBOOT, whose licences ask for
# their notices to travel with the binary (pspsdk BSD, libpspvram MIT; newlib
# and pthread-embedded's LGPL, met by the port's source being public, so
# anyone can relink against a changed library)
lic=$PSPDEV/psp/share/licenses
mkdir -p "$pkg/PSP/GAME/halo/licenses"
for pair in pspsdk/LICENSE:pspsdk.txt libpspvram/LICENSE:libpspvram.txt \
            newlib/COPYING.NEWLIB:newlib.txt pthread-embedded/COPYING.LIB:pthread-embedded.txt; do
    [[ -f $lic/${pair%%:*} ]] || { echo "build_release: no $lic/${pair%%:*} in the toolchain" >&2; exit 1; }
    cp "$lic/${pair%%:*}" "$pkg/PSP/GAME/halo/licenses/${pair#*:}"
done
cat > "$pkg/PSP/GAME/halo/licenses/README.txt" << 'EOF'
EBOOT.PBP is linked with these libraries from the PSP toolchain (pspdev):
  pspsdk            BSD licence           pspsdk.txt
  libpspvram        MIT licence           libpspvram.txt
  newlib            its licences          newlib.txt
  pthread-embedded  LGPL 2.1              pthread-embedded.txt
  me-core           MIT licence           ../LICENSE-mecore.md
The game's own source, with which the EBOOT can be rebuilt and relinked
against changed copies of these libraries, is at
https://github.com/Sheepdog-01/Sierra-III-Release (docs/toolchain.md).
EOF
top=(PSP)
if [[ -f $SRC/psp/presets.txt ]]; then
    # the launch menu takes MAXIMUM_PRESETS (psp_menu.c); one past it would be
    # a test the menu can't start
    most=$(sed -n 's/^#define MAXIMUM_PRESETS \([0-9]*\).*/\1/p' "$SRC/psp/src/psp_menu.c")
    count=$(grep -v '^#' "$SRC/psp/presets.txt" | grep -c ':' || true)
    if [[ -z $most || $count -gt $most ]]; then
        echo "build_release: psp/presets.txt has $count presets; the launch menu holds ${most:-?}" >&2
        exit 1
    fi
    mkdir -p "$pkg/halo"; cp "$SRC/psp/presets.txt" "$pkg/halo/presets.txt"
fi
# the stick's own files psp_build.py made beside the EBOOT: the campaign's
# ready-made profile with every level open, in a slot of its own, so
# unzipping a new build never touches a player's profiles
if [[ -d $OUT/build/stick ]]; then cp -r "$OUT/build/stick/." "$pkg/"; fi
[[ -d $pkg/halo ]] && top+=(halo)
# every file and folder carries the commit's time and fixed permissions, so
# the zip's bytes depend only on the commit (Python's zip tool also sorts)
stamp=$(git -C "$SRC" log -1 --format=%ct)
find "$pkg" -type d -exec chmod 755 {} +
find "$pkg" -type f -exec chmod 644 {} +
find "$pkg" -exec touch -h -d "@$stamp" {} +
rm -f "$OUT/halo_psp_$LABEL.zip"
# (a zip stores local times: UTC, so the time zone of the machine doesn't show)
(cd "$pkg" && TZ=UTC python3 -m zipfile -c "$OUT/halo_psp_$LABEL.zip" "${top[@]}")
cp "$OUT/build/halo.elf" "$OUT/build/halo.map" "$OUT/"
# each text section's own alignment, which the link map doesn't print, so a
# code layout can be placed where the link will really put it
find "$OUT/build/obj" -name '*.o' -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' o; do
    psp-readelf -SW "$o" | awk -v rel="${o#"$OUT/build/obj/"}" '
        sub(/^ *\[ *[0-9]+\] /, "") && $1 ~ /^\.text/ && $2 == "PROGBITS" { print $1 "\t" rel "\t" $NF }'
done > "$OUT/halo.secalign.txt" || true
log "$(grep -E '^program ' "$OUT/build.log" | tail -1)"
log "wrote $OUT/halo_psp_$LABEL.zip ($(sha256sum "$OUT/build/EBOOT.PBP" | cut -c1-16) EBOOT)"
