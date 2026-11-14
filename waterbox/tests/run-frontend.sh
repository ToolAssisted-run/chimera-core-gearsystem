#!/bin/bash
# The frontend test script: the Gearsystem package inside Chimera's window,
# run headless (no display). For each machine it opens a project of the test
# cartridge, runs a fixed number of frames with nothing pressed, and requires
# the console's whole RAM to be byte for byte the native reference's. Then it
# holds buttons through the window (input reaches the machine), unplugs the
# pad with a setting (a setting reaches the machine), and checks that the
# package's key bindings are the window's when nobody changed them.
#
# Usage: ./waterbox/tests/run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=300
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	[ -d "$root/../chimera" ] && chimera_root="$root/../chimera"
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "Chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
crun="$chimera_root/build/dll/chimera-run"
package="$chimera_root/build/Cores/gearsystem.chimeraCore"
rn="$root/build/meson-native/run-native"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
[ -x "$crun" ] || { echo "chimera-run not installed: $crun" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run waterbox/build-package.sh)" >&2; exit 1; }
[ -x "$rn" ] || { echo "native reference not built" >&2; exit 1; }

work="$here/work"
rm -rf "$work"
mkdir -p "$work"
python3 "$root/tests/make-roms.py" "$work/roms" || { echo "could not write the test cartridges" >&2; exit 1; }

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"
python3 "$here/settings-config.py" "$work/window-settings.json"

ok=0
failed=0
report() { printf "%-26s %-6s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-26s %-6s %s\n" "Test" "Result" "Detail"
printf "%-26s %-6s %s\n" "----" "------" "------"

# a project of one machine's test cartridge, with these settings (JSON); a
# link machine (ext gglink or m3link) gets the link cartridge in both slots
make_project() { # tag ext settings: prints the project's path
	local dir="$work/project.$1"
	rm -rf "$dir" && mkdir -p "$dir"
	case "$2" in
	gglink|m3link)
		local e=gg; [ "$2" = m3link ] && e=sms
		# P2's slot left empty: P2's console runs its own copy of P1's
		# cartridge (the native reference puts the file in both slots)
		cp "$work/roms/gslink.$e" "$dir/"
		"$crun" --new-project "$dir/$1.chimeraProject" "$package" --file "cart_$e=$dir/gslink.$e" \
			--settings "$3" > "$dir/make.log" 2>&1 && echo "$dir/$1.chimeraProject" ;;
	*)
		cp "$work/roms/gstest.$2" "$dir/"
		"$crun" --new-project "$dir/$1.chimeraProject" "$package" "$dir/gstest.$2" \
			--settings "$3" > "$dir/make.log" 2>&1 && echo "$dir/$1.chimeraProject" ;;
	esac
}

# the window: the project, a job for frontend-ram.lua, its RAM in $work/<tag>.ram
run_window() { # tag ext settings system-id hold lua
	local tag="$1" ext="$2" settings="$3" sysid="$4" hold="$5" lua="${6:-frontend-ram.lua}"
	local job="$work/job.$tag.txt" project
	local out="$work/$tag.ram"
	[ "$lua" = dump-controls.lua ] && out="$work/$tag.controls.json"
	printf 'frames=%s\nout=%s\nmeta=%s\nsystem=%s\nhold=%s\n' "$frames" "$out" "$work/$tag.meta" "$sysid" "$hold" > "$job"
	project="$(make_project "$tag" "$ext" "$settings")" || { echo "no project (see tests/work/project.$tag/make.log)" > "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout -s KILL 900 env -u DISPLAY QT_QPA_PLATFORM=offscreen "$chimera_exe" --headless \
		"--config=$work/window-settings.json" "--core=$package" \
		"--lua=$here/$lua" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta" ] && grep -q "^status=OK" "$work/$tag.meta"
}

# the native reference over the same cartridge, settings and held buttons
native_ram() { # tag ext settings sys hold-indices
	local tag="$1" ext="$2" settings="$3" sys="$4" hold="$5"
	local wd="$work/native.$tag"
	rm -rf "$wd"; mkdir -p "$wd"
	case "$ext" in
	gglink|m3link)
		local e=gg; [ "$ext" = m3link ] && e=sms
		cp "$work/roms/gslink.$e" "$wd/"
		printf '{"cart_%s":["gslink.%s"],"cart_%s2":["gslink.%s"]}' "$e" "$e" "$e" "$e" > "$wd/slots" ;;
	*)
		cp "$work/roms/gstest.$ext" "$wd/"
		printf '{"cart_%s":["gstest.%s"]}' "$sys" "$ext" > "$wd/slots" ;;
	esac
	printf '%s' "$settings" > "$wd/settings"
	"$rn" "$wd" --sys "$sys" --frames "$frames" ${hold:+--hold "$hold"} \
		--dump-domain "Main RAM" "$work/native.$tag.ram" > "$work/native.$tag.txt" 2>&1
}

# --- the machine the window builds must be the one the native build is ---
for m in "sms sms SMS sms" "gg gg GG gg" "sg sg SG sg1000" "gglink gglink GGL ggLink" "m3link m3link M3L markIIILink"; do
	read -r tag ext sysid system <<< "$m"
	settings="{\"system\":\"$system\"}"
	if ! native_ram "$tag" "$ext" "$settings" "$tag" ""; then
		report "$tag:frontend" FAIL "native run failed (see tests/work/native.$tag.txt)"
	elif ! run_window "$tag" "$ext" "$settings" "$sysid" ""; then
		report "$tag:frontend" FAIL "the window's run did not finish (see tests/work/$tag.log)"
	elif cmp -s "$work/native.$tag.ram" "$work/$tag.ram"; then
		report "$tag:frontend" PASS "$frames frames in the window, Main RAM identical to the native reference"
		# a link machine: P1's RAM must also show what came over the cable
		case "$tag" in gglink|m3link)
			n="$(python3 -c "import sys; print(open(sys.argv[1],'rb').read()[0x114])" "$work/$tag.ram")"
			if [ "${n:-0}" -gt 0 ]; then
				report "$tag:cable" PASS "P1 received $n times from P2 over the cable in the window"
			else
				report "$tag:cable" FAIL "P1 received nothing over the cable in the window"
			fi ;;
		esac
	else
		report "$tag:frontend" FAIL "Main RAM differs from the native reference"
	fi
done

# --- input through the window: hold P1 Right and P1 Button 1 on a Master
# System (wire 6 and 7); the native twin holds the same two ---
settings='{"system":"sms"}'
if ! native_ram "hold" sms "$settings" sms "6,7"; then
	report "input:frontend" FAIL "native run failed (see tests/work/native.hold.txt)"
elif ! run_window "hold" sms "$settings" SMS "P1 Right,P1 Button 1"; then
	report "input:frontend" FAIL "the window's run did not finish (see tests/work/hold.log)"
elif ! cmp -s "$work/native.hold.ram" "$work/hold.ram"; then
	report "input:frontend" FAIL "with the buttons held, Main RAM differs from the native reference"
elif cmp -s "$work/hold.ram" "$work/sms.ram"; then
	report "input:frontend" FAIL "holding the buttons changed nothing"
else
	report "input:frontend" PASS "P1 Right and Button 1 held in the window reach the machine as in the native run"
fi

# --- a setting through the project: with port 1 empty the same held
# buttons must go dead, and the machine still match its native reference ---
settings='{"system":"sms","port1":"none"}'
if ! native_ram "noport" sms "$settings" sms "6,7"; then
	report "setting:port1" FAIL "native run failed (see tests/work/native.noport.txt)"
elif ! run_window "noport" sms "$settings" SMS "P1 Right,P1 Button 1"; then
	report "setting:port1" FAIL "the window's run did not finish (see tests/work/noport.log)"
elif ! cmp -s "$work/native.noport.ram" "$work/noport.ram"; then
	report "setting:port1" FAIL "with port 1 empty, Main RAM differs from the native reference"
elif cmp -s "$work/noport.ram" "$work/hold.ram"; then
	report "setting:port1" FAIL "port1=none built the same machine as a plugged pad"
else
	report "setting:port1" PASS "port1=none reached the machine and the held buttons went dead"
fi

# --- the bindings the package ships are the window's when nobody changed them ---
for m in "sms:Master System Controller:sms:SMS" "gg:Game Gear Controller:gg:GG" "sg:SG-1000 Controller:sg1000:SG" \
	"gglink:Game Gear Link Controls:ggLink:GGL" "m3link:Mark III Link Controls:markIIILink:M3L"; do
	IFS=: read -r ext name system sysid <<< "$m"
	tag="keys.$ext"
	if run_window "$tag" "$ext" "{\"system\":\"$system\"}" "$sysid" "" dump-controls.lua \
		&& [ -s "$work/$tag.controls.json" ]; then
		if python3 "$here/check-keybinds.py" "$work/$tag.controls.json" "$wb/default_keybinds.json" "$name" > "$work/$tag.txt" 2>&1; then
			report "keybinds:$ext" PASS "$(cat "$work/$tag.txt")"
		else
			report "keybinds:$ext" FAIL "$(head -1 "$work/$tag.txt")"
		fi
	else
		report "keybinds:$ext" FAIL "the window's run did not finish (see tests/work/$tag.log)"
	fi
done

echo
echo "$ok passed, $failed failed"
[ "$failed" -eq 0 ]
