#!/bin/bash
# The core's test script. The sandboxed core (core.wbx) must give the same
# picture, sound, lag count and memory as the native reference build of the
# same sources, and must not change when the whole machine is saved and
# loaded before every frame. It runs on the three machines Gearsystem is,
# with test cartridges this repository writes itself (tests/make-roms.py),
# so it needs no file from anywhere else.
#
# Usage: ./waterbox/run-gate.sh [-n <native build dir>] [-g <guest build dir>]
# CHIMERA_ROOT=<a Chimera checkout with this package installed> adds the
# test that goes through Chimera's engine.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
while getopts "n:g:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "sandboxed build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/tests/make-roms.py" "$work/roms" || { echo "could not write the test cartridges" >&2; exit 1; }

digests() { grep -E '^(frames|vsync|picture|videoHash|audioHash|audioSamples|lagFrames|domain\[)'; }
# what a turbo run can be held to: everything but the whole-run picture hash,
# which a run that drew nothing for half its frames cannot match
turboDigests() { grep -E '^(frames|vsync|tailVideoHash|audioHash|lagFrames|domain\[)'; }
field() { sed -n "s/^$1=//p" "$2"; }

ok=0
failed=0
skipped=0
report() {
	printf "%-28s %-6s %s\n" "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok+1)) ;; SKIP) skipped=$((skipped+1)) ;; *) failed=$((failed+1)) ;; esac
}
printf "%-28s %-6s %s\n" "Test" "Result" "Detail"
printf "%-28s %-6s %s\n" "----" "------" "------"

# a work folder: the machine's cartridge, its slot map, and settings (JSON)
workdir() { # name sys settings-json [extra slot entries]
	local wd="$work/$1" sys="$2" settings="$3" extra="${4:-}"
	rm -rf "$wd"; mkdir -p "$wd"
	cp "$work/roms/gstest.$sys" "$wd/"
	printf '{"cart_%s":["gstest.%s"]%s}' "$sys" "$sys" "$extra" > "$wd/slots"
	printf '%s' "$settings" > "$wd/settings"
	echo "$wd"
}
system_of() { case "$1" in sg) echo sg1000 ;; *) echo "$1" ;; esac; }

# native and sandbox over one folder and one schedule; the sandbox's digests
# are left in $work/<tag>.box
equal_runs() { # tag wd args...
	local tag="$1" wd="$2"; shift 2
	if ! "$nat/run-native" "$wd" "$@" 2>"$work/err" | digests > "$work/$tag.nat"; then
		echo "native run failed: $(head -1 "$work/err")"; return 1; fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" 2>"$work/err" | digests > "$work/$tag.box"; then
		echo "sandboxed run failed: $(head -1 "$work/err")"; return 1; fi
	if ! grep -q '^frames=' "$work/$tag.box"; then
		echo "the sandboxed run reported no frame"; return 1; fi
	if ! cmp -s "$work/$tag.nat" "$work/$tag.box"; then
		echo "native and sandbox differ: $(diff "$work/$tag.nat" "$work/$tag.box" | grep '^[<>]' | head -4 | tr '\n' ' ')"; return 1; fi
	return 0
}

for sys in sms gg sg; do
	wd="$(workdir "$sys" "$sys" "{\"system\":\"$(system_of "$sys")\"}")"
	args=(--sys "$sys" --frames 600 --exercise)
	[ "$sys" = sms ] && args+=(--wiggle-axes)

	if msg="$(equal_runs "$sys" "$wd" "${args[@]}")"; then
		report "$sys:equivalence" PASS "600 frames, native == sandbox ($(field picture "$work/$sys.box"), $(field audioSamples "$work/$sys.box") sound samples)"
	else
		report "$sys:equivalence" FAIL "$msg"; continue
	fi

	# the controls must have shaped the machine: the same run with nothing
	# pressed must end elsewhere
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys "$sys" --frames 600 2>/dev/null | digests > "$work/$sys.idle"
	if cmp -s "$work/$sys.box" "$work/$sys.idle"; then
		report "$sys:input-shaped" FAIL "pressing the controls changed nothing"
	else
		report "$sys:input-shaped" PASS "the pressed controls changed the machine"
	fi

	# lag frames: the test cartridge reads its controls on every second
	# frame only, so about half the frames are lag frames. A machine whose
	# reads were never reported would count every frame.
	lag="$(field lagFrames "$work/$sys.box")"
	if [ "${lag:-600}" -ge 290 ] && [ "${lag:-0}" -le 310 ]; then
		report "$sys:lag" PASS "$lag of 600 frames are lag frames (the cartridge reads every second frame)"
	else
		report "$sys:lag" FAIL "$lag of 600 frames are lag frames; about 300 expected"
	fi

	# turbo: nothing drawn for the first half of the run; the machine, the
	# sound and the second half's pictures must be what they would have been,
	# and the whole-run picture hash must show that drawing really stopped
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>/dev/null > "$work/tn.raw"
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --turbo 2>/dev/null > "$work/tt.raw"; then
		turboDigests < "$work/tn.raw" > "$work/tn.txt"
		turboDigests < "$work/tt.raw" > "$work/tt.txt"
		if ! cmp -s "$work/tn.txt" "$work/tt.txt"; then
			report "$sys:turbo" FAIL "$(diff "$work/tn.txt" "$work/tt.txt" | grep '^[<>]' | head -2 | tr '\n' ' ')"
		elif [ "$(field videoHash "$work/tn.raw")" = "$(field videoHash "$work/tt.raw")" ]; then
			report "$sys:turbo" FAIL "the turbo run drew every frame"
		else
			report "$sys:turbo" PASS "300 frames undrawn, then the same machine and pictures"
		fi
	else
		report "$sys:turbo" FAIL "the turbo run failed"
	fi

	# save and load the whole machine before every frame
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/$sys.rr" \
		&& cmp -s "$work/$sys.box" "$work/$sys.rr"; then
		report "$sys:savestate" PASS "a save and a load before every frame change nothing"
	else
		report "$sys:savestate" FAIL "$(diff "$work/$sys.box" "$work/$sys.rr" | grep '^[<>]' | head -2 | tr '\n' ' ')"
	fi

	# Power: the console switched off and on part way through
	if msg="$(equal_runs "$sys.power" "$wd" "${args[@]}" --power-at 300)"; then
		if cmp -s "$work/$sys.box" "$work/$sys.power.box"; then
			report "$sys:power" FAIL "pressing Power changed nothing"
		else
			report "$sys:power" PASS "Power at frame 300 restarts the machine, native == sandbox"
		fi
	else
		report "$sys:power" FAIL "$msg"
	fi
done

# ---- a setting must reach the machine and build the machine it names ----
# (each: the machine with the setting, native == sandbox, and not the machine
# the default builds)
setting_test() { # name sys settings-json what-must-differ(grep pattern) args...
	local name="$1" sys="$2" settings="$3" pattern="$4"; shift 4
	local wd
	wd="$(workdir "set.$name" "$sys" "$settings")"
	local msg
	if ! msg="$(equal_runs "set.$name" "$wd" --sys "$sys" "$@")"; then
		report "setting:$name" FAIL "$msg"; return; fi
	local dwd
	dwd="$(workdir "set.$name.dflt" "$sys" "{\"system\":\"$(system_of "$sys")\"}")"
	"$nat/run-wbx" "$gst/core.wbx" "$dwd" --sys "$sys" "$@" 2>/dev/null | digests > "$work/set.$name.default"
	if ! grep -q '^frames=' "$work/set.$name.default"; then
		report "setting:$name" FAIL "the default machine did not run"
	elif [ "$(grep -E "$pattern" "$work/set.$name.box")" = "$(grep -E "$pattern" "$work/set.$name.default")" ]; then
		report "setting:$name" FAIL "the machine is the default's ($pattern unchanged)"
	else
		report "setting:$name" PASS "native == sandbox, and $(diff <(grep -E "$pattern" "$work/set.$name.default") <(grep -E "$pattern" "$work/set.$name.box") | sed -n 's/^> //p' | head -1)"
	fi
}
setting_test timing-pal sms '{"system":"sms","timing":"pal"}' '^vsync=' --frames 300 --exercise
setting_test ym2413-off sms '{"system":"sms","ym2413":"off"}' '^audioHash=' --frames 300 --exercise
setting_test region-japan sms '{"system":"sms","region":"japan"}' '^(audioHash|domain\[Main RAM\])=' --frames 300 --exercise
setting_test overscan sms '{"system":"sms","overscan":"full320"}' '^picture=' --frames 120
setting_test psg-volume sg '{"system":"sg1000","psgVolume":50}' '^audioHash=' --frames 300 --exercise
setting_test gg-1asic gg '{"system":"gg","ggModel":"1asic"}' '^(videoHash|audioHash|domain)' --frames 300 --exercise
setting_test gg-smsmode gg '{"system":"gg","ggModel":"2asicSmsMode"}' '^picture=' --frames 120
setting_test sg1000ii sg '{"system":"sg1000","sgModel":"sg1000ii"}' '^videoHash=' --frames 120
setting_test no-sprite-limit sms '{"system":"sms","noSpriteLimit":true,"allowUpDown":true}' '^(videoHash|domain\[Main RAM\])=' --frames 300 --exercise

# ---- the Master System's other controllers ----
# Each must answer the program's reads (native == sandbox, through a save and
# a load before every frame) and must not be the machine a plain pad makes:
# an unknown name falls back to a pad, so agreeing alone would prove nothing.
device_test() { # name settings-json
	local name="$1" settings="$2"
	local wd msg
	wd="$(workdir "dev.$name" sms "$settings")"
	if ! msg="$(equal_runs "dev.$name" "$wd" --sys sms --frames 300 --exercise --wiggle-axes)"; then
		report "device:$name" FAIL "$msg"; return; fi
	local pwd_
	pwd_="$(workdir "dev.$name.padwd" sms '{"system":"sms"}')"
	"$nat/run-wbx" "$gst/core.wbx" "$pwd_" --sys sms --frames 300 --exercise --wiggle-axes 2>/dev/null | digests > "$work/dev.$name.pad"
	if ! grep -q '^frames=' "$work/dev.$name.pad"; then
		report "device:$name" FAIL "the plain pad's machine did not run"; return
	elif cmp -s "$work/dev.$name.box" "$work/dev.$name.pad"; then
		report "device:$name" FAIL "the machine is the one a plain pad makes"; return; fi
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 300 --exercise --wiggle-axes --rerecord 2>/dev/null \
		| digests > "$work/dev.$name.rr" && cmp -s "$work/dev.$name.box" "$work/dev.$name.rr"; then
		report "device:$name" PASS "native == sandbox == save and load every frame, not a plain pad's machine"
	else
		report "device:$name" FAIL "a save and a load before every frame changed the run"
	fi
}
device_test lightPhaser '{"system":"sms","port1":"lightPhaser"}'
device_test paddle '{"system":"sms","port1":"paddle"}'
device_test sportsPad1 '{"system":"sms","port1":"sportsPad"}'
device_test sportsPad2 '{"system":"sms","port2":"sportsPad"}'
device_test noPads '{"system":"sms","port1":"none","port2":"none"}'

# the axes must matter to the device that reads them: the same run with the
# axes left in the middle must end elsewhere
wd="$work/dev.paddle"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 300 --exercise 2>/dev/null | digests > "$work/paddle.still"
if cmp -s "$work/dev.paddle.box" "$work/paddle.still"; then
	report "device:paddle-knob" FAIL "turning the paddle's knob changed nothing"
else
	report "device:paddle-knob" PASS "the paddle's axis reaches the machine"
fi

# ---- save data: out of the cartridge's battery RAM, and back in ----
wd="$(workdir save sms '{"system":"sms"}')"
mkdir -p "$work/sd.nat" "$work/sd.box"
"$nat/run-native" "$wd" --sys sms --frames 120 --exercise --savedata-out "$work/sd.nat" >/dev/null 2>&1
"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 120 --exercise --savedata-out "$work/sd.box" >/dev/null 2>&1
if [ -f "$work/sd.box/battery.sav" ] && [ "$(stat -c %s "$work/sd.box/battery.sav")" = 32768 ] \
	&& diff -r "$work/sd.nat" "$work/sd.box" >/dev/null 2>&1; then
	report "savedata:export" PASS "battery.sav, 32768 bytes, native == sandbox"
else
	report "savedata:export" FAIL "no battery.sav of 32768 bytes, or native and sandbox differ"
fi
# a save put back: the cartridge copies its first two bytes into the
# console's RAM at power-on (0xC111 and 0xC112)
python3 -c "import sys; open(sys.argv[1],'wb').write(bytes([0x5A,0xA5])+bytes(32766))" "$work/in.sav"
wd="$(workdir load sms '{"system":"sms"}' ',"savedata":["in.sav"]')"
cp "$work/in.sav" "$wd/"
if "$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 2 --dump-domain "Main RAM" "$work/load.ram" >/dev/null 2>&1 \
	&& [ "$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('%02x%02x' % (d[0x111], d[0x112]))" "$work/load.ram")" = 5aa5 ]; then
	report "savedata:import" PASS "the save in the project is the battery RAM the cartridge finds"
else
	report "savedata:import" FAIL "the cartridge did not find the save's bytes"
fi
# a save the machine cannot take is refused, not ignored
python3 -c "import sys; open(sys.argv[1],'wb').write(bytes(100))" "$wd/in.sav"
if "$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 2 2>"$work/err" >/dev/null; then
	report "savedata:refused" FAIL "a save of 100 bytes was taken"
elif grep -q 'error.save_refused' "$work/err"; then
	report "savedata:refused" PASS "a save of the wrong size stops the load and says why"
else
	report "savedata:refused" FAIL "the load failed for another reason: $(head -1 "$work/err")"
fi

# ---- the link machines: two consoles in one, joined by their cable ----
# gslink.gg talks over the Gear-to-Gear cable's serial port, gslink.sms over
# the Mark III's Joy-Joy cable (tests/make-roms.py). Byte 0x114 of a
# console's RAM counts what it received from the other.
linkdir() { # name ext system
	local wd="$work/$1"
	rm -rf "$wd"; mkdir -p "$wd"
	cp "$work/roms/gslink.$2" "$wd/"
	printf '{"cart_%s":["gslink.%s"],"cart_%s2":["gslink.%s"]}' "$2" "$2" "$2" "$2" > "$wd/slots"
	printf '{"system":"%s"}' "$3" > "$wd/settings"
	echo "$wd"
}
ram_byte() { python3 -c "import sys; print(open(sys.argv[1],'rb').read()[int(sys.argv[2],0)])" "$1" "$2"; }
for m in "gglink gg ggLink 12" "m3link sms markIIILink 19"; do
	read -r sys ext system p2button <<< "$m"
	wd="$(linkdir "$sys" "$ext" "$system")"
	args=(--sys "$sys" --frames 600 --exercise)
	if msg="$(equal_runs "$sys" "$wd" "${args[@]}")"; then
		report "$sys:equivalence" PASS "600 frames, native == sandbox ($(field picture "$work/$sys.box"), two consoles)"
	else
		report "$sys:equivalence" FAIL "$msg"; continue
	fi
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/$sys.rr" \
		&& cmp -s "$work/$sys.box" "$work/$sys.rr"; then
		report "$sys:savestate" PASS "a save and a load before every frame change nothing"
	else
		report "$sys:savestate" FAIL "$(diff "$work/$sys.box" "$work/$sys.rr" | grep '^[<>]' | head -2 | tr '\n' ' ')"
	fi
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>/dev/null > "$work/tn.raw"
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --turbo 2>/dev/null > "$work/tt.raw"
	turboDigests < "$work/tn.raw" > "$work/tn.txt"; turboDigests < "$work/tt.raw" > "$work/tt.txt"
	if cmp -s "$work/tn.txt" "$work/tt.txt" && [ "$(field videoHash "$work/tn.raw")" != "$(field videoHash "$work/tt.raw")" ]; then
		report "$sys:turbo" PASS "300 frames undrawn, then the same machine and pictures"
	else
		report "$sys:turbo" FAIL "turbo changed the machine, or drew every frame"
	fi
	# each console received something from the other; the same cartridge on
	# one console alone receives nothing
	n1="" n2="" alone=""
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys "$sys" --frames 300 --dump-domain "Main RAM" "$work/l1.ram" >/dev/null 2>&1 && n1="$(ram_byte "$work/l1.ram" 0x114)"
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys "$sys" --frames 300 --dump-domain "P2 Main RAM" "$work/l2.ram" >/dev/null 2>&1 && n2="$(ram_byte "$work/l2.ram" 0x114)"
	single="$ext"; [ "$ext" = sms ] && settings='{"system":"sms","region":"japan"}' || settings='{"system":"gg"}'
	swd="$work/$sys.alone"; rm -rf "$swd"; mkdir -p "$swd"; cp "$work/roms/gslink.$ext" "$swd/"
	printf '{"cart_%s":["gslink.%s"]}' "$ext" "$ext" > "$swd/slots"; printf '%s' "$settings" > "$swd/settings"
	"$nat/run-wbx" "$gst/core.wbx" "$swd" --sys "$ext" --frames 300 --dump-domain "Main RAM" "$work/l0.ram" >/dev/null 2>&1 && alone="$(ram_byte "$work/l0.ram" 0x114)"
	if [ "${n1:-0}" -gt 0 ] && [ "${n2:-0}" -gt 0 ] && [ "${alone:-1}" = 0 ]; then
		report "$sys:exchange" PASS "in 300 frames P1 received $n1 times and P2 $n2; one console alone, 0"
	else
		report "$sys:exchange" FAIL "received: P1 ${n1:-?}, P2 ${n2:-?}, alone ${alone:-?}"
	fi
	# P2's button reaches P1 only through the cable: held, it changes P1's RAM
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --sys "$sys" --frames 300 --hold "$p2button" --dump-domain "Main RAM" "$work/lh.ram" >/dev/null 2>&1
	if [ -s "$work/lh.ram" ] && ! cmp -s "$work/l1.ram" "$work/lh.ram"; then
		report "$sys:crossing" PASS "P2's button, held, changes P1's RAM through the cable"
	else
		report "$sys:crossing" FAIL "P2's button did not reach P1"
	fi
	if msg="$(equal_runs "$sys.power" "$wd" "${args[@]}" --power-at 300)" && ! cmp -s "$work/$sys.box" "$work/$sys.power.box"; then
		report "$sys:power" PASS "Power at frame 300 restarts both consoles, native == sandbox"
	else
		report "$sys:power" FAIL "${msg:-pressing Power changed nothing}"
	fi
	# a frame is a lag frame when neither console read its controls; each
	# reads on every second frame of its own, and the two need not be in
	# step, so at most about half (a read never reported would make all 600)
	lag="$("$nat/run-wbx" "$gst/core.wbx" "$wd" --sys "$sys" --frames 600 2>/dev/null | sed -n 's/^lagFrames=//p')"
	if [ "${lag:-600}" -gt 0 ] && [ "${lag:-600}" -le 310 ]; then
		report "$sys:lag" PASS "$lag of 600 frames are lag frames (each cartridge reads every second frame)"
	else
		report "$sys:lag" FAIL "$lag of 600 frames are lag frames; between 1 and about 300 expected"
	fi
done
# a Game Gear Link takes Game Gear cartridges only
wd="$work/gglink.refused"; rm -rf "$wd"; mkdir -p "$wd"
cp "$work/roms/gslink.gg" "$work/roms/gstest.sms" "$wd/"
printf '{"cart_gg":["gslink.gg"],"cart_gg2":["gstest.sms"]}' > "$wd/slots"
printf '{"system":"ggLink"}' > "$wd/settings"
if "$nat/run-wbx" "$gst/core.wbx" "$wd" --sys gglink --frames 2 2>"$work/err" >/dev/null; then
	report "gglink:refused" FAIL "a Master System cartridge was linked to a Game Gear"
elif grep -q 'error.link_refused' "$work/err"; then
	report "gglink:refused" PASS "a Master System cartridge in a Game Gear Link stops the load and says why"
else
	report "gglink:refused" FAIL "the load failed for another reason: $(head -1 "$work/err")"
fi

# ---- the boot ROM ----
wd="$(workdir bios.missing sms '{"system":"sms","bios":true}')"
if "$nat/run-wbx" "$gst/core.wbx" "$wd" --sys sms --frames 2 2>"$work/err" >/dev/null; then
	report "bios:missing" FAIL "the machine started without the boot ROM it was told to run"
elif grep -q 'error.bios_not_read' "$work/err"; then
	report "bios:missing" PASS "asked for a boot ROM that is not there, the load stops and says why"
else
	report "bios:missing" FAIL "the load failed for another reason: $(head -1 "$work/err")"
fi
# A real boot ROM is the user's own and is never in this repository; put a
# copy in tests/firmware-local (smsBios, ggBios) to run these.
for b in sms:smsBios gg:ggBios; do
	sys="${b%%:*}"; id="${b##*:}"
	fw="$root/tests/firmware-local/$id"
	if [ ! -f "$fw" ]; then
		report "bios:$sys" SKIP "no tests/firmware-local/$id (the user's own boot ROM)"
		continue
	fi
	wd="$(workdir "bios.$sys" "$sys" "{\"system\":\"$sys\",\"bios\":true}")"
	cp "$fw" "$wd/$id"
	if msg="$(equal_runs "bios.$sys" "$wd" --sys "$sys" --frames 900 --exercise)"; then
		report "bios:$sys" PASS "900 frames through the boot ROM, native == sandbox (sha1 $(sha1sum "$fw" | cut -c1-12))"
	else
		report "bios:$sys" FAIL "$msg"
	fi
done

# ------------------------------------------------- Chimera's engine
# Each read of the controls is reported to Chimera as it happens, so a Lua
# script's event.oninputpoll runs there (chimera#344): every frame the game
# read them has at least one, and counting them moves nothing. Needs
# CHIMERA_ROOT, a Chimera checkout with this core's package installed in it.
runner="${CHIMERA_ROOT:-}/build/dll/chimera-run"
package="${CHIMERA_ROOT:-}/build/Cores/gearsystem.chimeraCore"
if [ -n "${CHIMERA_ROOT:-}" ] && [ -x "$runner" ] && [ -f "$package" ]; then
	cart="$work/roms/gstest.sms"
	"$runner" --new-project "$work/none.chimeraProject" "$package" "$cart" --settings '{"system":"sms"}' > "$work/none.make.log" 2>&1
	"$runner" --project "$work/none.chimeraProject" "$package" --files "$(dirname "$cart")" \
		--frames 300 --record "$work/idle.txt" > "$work/idle.log" 2>&1
	"$runner" --new-project "$work/idle.chimeraProject" "$package" "$cart" "$work/idle.txt" --settings '{"system":"sms"}' > "$work/idle.make.log" 2>&1
	"$runner" --project "$work/idle.chimeraProject" "$package" --files "$(dirname "$cart")" \
		--dump "Main RAM=$work/plain.ram" > "$work/plain.log" 2>&1
	"$runner" --project "$work/idle.chimeraProject" "$package" --files "$(dirname "$cart")" --count-input-polls \
		--dump "Main RAM=$work/polls.ram" --meta "$work/polls.meta" > "$work/polls.log" 2>&1
	polls=$(sed -n 's/^inputPolls=//p' "$work/polls.meta" 2> /dev/null)
	lags=$(sed -n 's/^lagFrames=//p' "$work/polls.meta" 2> /dev/null)
	frames=$(sed -n 's/^frames=//p' "$work/polls.meta" 2> /dev/null)
	if [ "${polls:-none}" != none ] && [ "${frames:-0}" = 300 ] && [ "${lags:-300}" -lt 300 ] \
		&& [ "${polls:-0}" -ge $(( 300 - ${lags:-300} )) ] && [ -s "$work/plain.ram" ] && cmp -s "$work/plain.ram" "$work/polls.ram"; then
		report "engine:input-polls" PASS "$polls reads of the controls in 300 frames reach Chimera as they happen, $lags lag frames; RAM as without counting"
	else
		report "engine:input-polls" FAIL "inputPolls=${polls:-missing} frames=${frames:-?} lagFrames=${lags:-?}, or counting moved RAM: $(tail -1 "$work/polls.log" 2> /dev/null)"
	fi
else
	report "engine:input-polls" SKIP "needs CHIMERA_ROOT with build/dll/chimera-run and build/Cores/gearsystem.chimeraCore"
fi

echo ""
echo "$ok passed, $failed failed, $skipped skipped"
[ "$failed" -gt 0 ] && exit 1
exit 0
