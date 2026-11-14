#!/bin/bash
# Runs the games kept in tests/roms-local (never in git: commercial games are
# not ours to pass on) through the same comparison as the core test script:
# native == sandbox, and native == a save and a load before every frame.
# Each game runs with the controls pressed in a fixed pseudo-random pattern
# (the harness's --exercise), from power-on, for the frames given. The machine
# comes from the extension: .gg is a Game Gear, .sg/.sc/.mv an SG-1000, the
# rest a Master System. With --link, each Game Gear game also runs on the
# Game Gear Link (two Game Gears with the same game, joined by the cable),
# and each Master System game whose name says "(J)" or "Japan" on the Mark
# III Link. A folder with no games says SKIP.
#
# Usage: ./waterbox/tests/run-local.sh [--frames N] [--shots <folder>] [--link]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
frames=3600
shots=""
link=0
while [ $# -gt 0 ]; do
	case "$1" in
		--frames) frames="$2"; shift ;;
		--shots) shots="$2"; shift ;;
		--link) link=1 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] && [ -f "$gst/core.wbx" ] || {
	echo "build the native reference and core.wbx first (docs/BUILDING.md)" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|picture|videoHash|audioHash|audioSamples|lagFrames|domain\[)'; }
ok=0; failed=0; ran=0

shopt -s nullglob nocaseglob
for game in "$root"/tests/roms-local/*.{sms,gg,sg,sc,mv}; do
	name="$(basename "$game")"
	ext="${name##*.}"; ext="${ext,,}"
	case "$ext" in
		gg) sys=gg; system=gg ;;
		sg|sc|mv) sys=sg; system=sg1000 ;;
		*) sys=sms; system=sms ;;
	esac
	sha="$(sha1sum "$game" | cut -c1-40)"
	runs=("$sys $system single")
	if [ "$link" = 1 ]; then
		[ "$sys" = gg ] && runs+=("gglink ggLink linked")
		[ "$sys" = sms ] && case "$name" in *"(J)"*|*Japan*) runs+=("m3link markIIILink linked") ;; esac
	fi
	for r in "${runs[@]}"; do
	read -r hsys system how <<< "$r"
	wd="$work/game"; rm -rf "$wd"; mkdir -p "$wd"
	cp "$game" "$wd/game.$ext"
	if [ "$how" = linked ]; then
		printf '{"cart_%s":["game.%s"],"cart_%s2":["game.%s"]}' "$sys" "$ext" "$sys" "$ext" > "$wd/slots"
	else
		printf '{"cart_%s":["game.%s"]}' "$sys" "$ext" > "$wd/slots"
	fi
	printf '{"system":"%s"}' "$system" > "$wd/settings"
	args=(--sys "$hsys" --frames "$frames" --exercise)
	name="$(basename "$game") [$how]"
	ran=$((ran+1))
	"$nat/run-native" "$wd" "${args[@]}" 2>/dev/null | digests > "$work/nat"
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" ${shots:+--screenshot "$shots/$(basename "$game").$how.tga"} 2>/dev/null | digests > "$work/box"
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/rr"
	if ! grep -q '^frames=' "$work/box"; then
		echo "FAIL  $name ($sha): the sandboxed run reported no frame"; failed=$((failed+1))
	elif ! cmp -s "$work/nat" "$work/box"; then
		echo "FAIL  $name ($sha): native and sandbox differ: $(diff "$work/nat" "$work/box" | grep '^[<>]' | head -2 | tr '\n' ' ')"; failed=$((failed+1))
	elif ! cmp -s "$work/box" "$work/rr"; then
		echo "FAIL  $name ($sha): a save and a load before every frame changed the run"; failed=$((failed+1))
	else
		echo "PASS  $name ($sha): $frames frames, native == sandbox == save and load every frame, $(sed -n 's/^lagFrames=//p' "$work/box") lag frames, $(sed -n 's/^picture=//p' "$work/box")"
		ok=$((ok+1))
	fi
	done
done
[ "$ran" -eq 0 ] && echo "SKIP  no game in tests/roms-local (.sms, .gg, .sg, .sc, .mv)"
echo "$ok passed, $failed failed"
[ "$failed" -eq 0 ]
