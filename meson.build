# chimera-core-gearsystem: Gearsystem in miniBox's sandbox, as a Chimera core.
#
# Native reference build (run-native, and run-wbx, the program that runs the
# sandboxed core the way Chimera does):
#     meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
# Guest (core.wbx) build, through the cross file waterbox/setup-guest.sh
# writes (needs miniBox's C++ guest toolchain, build/meson-cpp):
#     sh waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest core.wbx
#
# The source list is upstream's libretro core's (platforms/libretro/
# Makefile.common) without libretro.cpp and libretro_link.cpp: the emulator
# and nothing of the desktop program, its debugger windows or its MCP server.
# Both builds compile the SAME waterbox/cinterface.cpp; only emulibc.h
# differs.
project('chimera-core-gearsystem', ['c', 'cpp'],
  default_options: ['c_std=gnu99', 'cpp_std=c++17', 'optimization=2', 'debug=false', 'warning_level=0'])

gs = 'extern/Gearsystem'

# The submodule is upstream as released; the patches in patches/ are applied
# to its working tree here (each one once).
run_command(files('waterbox/apply-patches.sh'), check: true)

mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/chimera-common-minibox)')
  endif
endif

defs = [
  '-DNDEBUG',
  '-DMINIZ_NO_TIME',
  '-DEMULATOR_BUILD="3.9.24"',
  # Upstream's debugger (disassembly records, call stack, profiler, trace
  # log, breakpoints) is not built: it is the desktop program's, and it
  # alone would take 64 MiB of pointers at start (one for each byte a
  # cartridge may have). The emulation does not depend on it.
  '-DGS_DISABLE_DISASSEMBLER',
  # patches/0001: each read of the controller ports is reported to the adapter
  '-DCHIMERA_INPUT_HOOK',
]
quiet = [
  '-fno-strict-aliasing',
  '-Wno-deprecated-declarations',
]
add_project_arguments(defs + quiet, language: ['c', 'cpp'])
# On aarch64 GCC fuses a multiply and an add into one instruction unless told
# not to, which rounds once where the guest (built with -ffp-contract=off)
# rounds twice. Both builds keep them apart; x86-64 is unchanged.
if not meson.is_cross_build() and host_machine.cpu_family() == 'aarch64'
  add_project_arguments('-ffp-contract=off', language: ['c', 'cpp'])
endif

inc = include_directories(
  'waterbox',
  gs / 'src',
  gs / 'platforms/shared/dependencies/miniz',
)

core_sources = files(
  gs / 'src/audio/emu2413/emu2413.c',
  gs / 'platforms/shared/dependencies/miniz/miniz.c',
  gs / 'src/Audio.cpp',
  gs / 'src/Cartridge.cpp',
  gs / 'src/CodemastersMemoryRule.cpp',
  gs / 'src/GameGearIOPorts.cpp',
  gs / 'src/MarkIIILink.cpp',
  gs / 'src/GearsystemCore.cpp',
  gs / 'src/Input.cpp',
  gs / 'src/KoreanMemoryRule.cpp',
  gs / 'src/KoreanMSXSMS8000MemoryRule.cpp',
  gs / 'src/KoreanSMS32KB2000MemoryRule.cpp',
  gs / 'src/KoreanMSX32KB2000MemoryRule.cpp',
  gs / 'src/Korean2000XOR1FMemoryRule.cpp',
  gs / 'src/KoreanMSX8KB0300MemoryRule.cpp',
  gs / 'src/Korean0000XORFFMemoryRule.cpp',
  gs / 'src/KoreanFFFFHiComMemoryRule.cpp',
  gs / 'src/KoreanFFFEMemoryRule.cpp',
  gs / 'src/KoreanBFFCMemoryRule.cpp',
  gs / 'src/KoreanFFF3FFFCMemoryRule.cpp',
  gs / 'src/KoreanMDFFF5MemoryRule.cpp',
  gs / 'src/KoreanMDFFF0MemoryRule.cpp',
  gs / 'src/Multi4PAKAllActionMemoryRule.cpp',
  gs / 'src/IratahackMemoryRule.cpp',
  gs / 'src/JumboDahjeeMemoryRule.cpp',
  gs / 'src/Eeprom93C46MemoryRule.cpp',
  gs / 'src/Memory.cpp',
  gs / 'src/MemoryRule.cpp',
  gs / 'src/MSXMemoryRule.cpp',
  gs / 'src/opcodes.cpp',
  gs / 'src/opcodes_cb.cpp',
  gs / 'src/opcodes_ed.cpp',
  gs / 'src/Processor.cpp',
  gs / 'src/RomOnlyMemoryRule.cpp',
  gs / 'src/SegaMemoryRule.cpp',
  gs / 'src/SG1000MemoryRule.cpp',
  gs / 'src/SmsIOPorts.cpp',
  gs / 'src/Video.cpp',
  gs / 'src/BootromMemoryRule.cpp',
  gs / 'src/JanggunMemoryRule.cpp',
  gs / 'src/YM2413.cpp',
  gs / 'src/VgmRecorder.cpp',
  gs / 'src/TraceLogger.cpp',
  gs / 'src/Profiler.cpp',
  gs / 'src/audio/Blip_Buffer.cpp',
  gs / 'src/audio/Sms_Apu.cpp',
  gs / 'src/audio/Stereo_Buffer.cpp',
  'waterbox/cinterface.cpp',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl/libstdc++ toolchain arrives through the cross file; the
  # link recipe is the C++ guest kit's (cxxglue for the unwinder, weak
  # pthread pulls for libgcc_eh), as in chimera-core-snes9x.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax', '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference -------------------------------------------------
  m = meson.get_compiler('c').find_library('m', required: false)
  executable('run-native', core_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    cpp_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    dependencies: [m])

  # The host library comes from the same miniBox build the guest toolchain
  # does: meson-cpp (-Dguest_cpp=true), as Gearsystem is C++.
  mbhost = mb / 'build/meson-cpp/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
endif
