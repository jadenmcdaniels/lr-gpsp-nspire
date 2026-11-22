# Shared by run.sh, speed.sh and wav.sh (tests/audio): paths, the scenes, the
# builds and the speed model's controlled environment. Source it.

AUDIO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(dirname "$(dirname "$AUDIO_DIR")")
ARM=$ROOT/tests/arm
REF=$AUDIO_DIR/ref
GAMES=${GBA_GAMES:-}
QEMU=${QEMU:-$(command -v qemu-arm)}
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
export LC_ALL=C

# The scenes of tests/speed_scenes.sh, with its games, states, keys and
# frames: name | game | state ("-": power-on) | keys | frames | frames left
# out of the speed figures. The audio gate runs all the frames of each.
SCENES="
aw|Advance Wars (USA) (Rev 1).gba|aw_latest.svs.tns|-|600|30
emerald|Pokemon - Emerald Version (USA, Europe).gba|emerald_route_grass.svs.tns|tests/arm/replay/emerald_grass.keys|1900|30
fe|Fire Emblem - The Sacred Stones (USA, Australia).gba|-|tests/arm/replay/boot.keys|2400|1700
gs|Golden Sun (USA, Europe).gba|-|tests/arm/replay/boot.keys|2400|1700
mc|Legend of Zelda, The - The Minish Cap (USA).gba|-|tests/arm/replay/boot.keys|2400|1700
"
SCENE_NAMES="aw emerald fe gs mc"

# The listening clips (wav.sh): scene | first | last frame (about 10 s, 600
# frames, at the end of the scene's run).
CLIPS="
aw|31|630
emerald|1301|1900
fe|1801|2400
gs|1801|2400
mc|1801|2400
"

# scene_get NAME: sets SC_GAME SC_STATE SC_KEYS SC_FRAMES SC_SKIP (state and
# game as paths; "-" stays "-").
scene_get() {
  local line
  line=$(echo "$SCENES" | grep "^$1|") || { echo "no scene $1" >&2; return 1; }
  IFS='|' read -r _ SC_GAME SC_STATE SC_KEYS SC_FRAMES SC_SKIP <<<"$line"
  SC_GAME=$GAMES/$SC_GAME
  [ "$SC_STATE" = - ] || SC_STATE=$GAMES/$SC_STATE
  [ "$SC_KEYS" = - ] || SC_KEYS=$ROOT/$SC_KEYS
  [ -f "$SC_GAME" ] && { [ "$SC_STATE" = - ] || [ -f "$SC_STATE" ]; }
}

# Mixer settings of the audio harness (key=value arguments of perf_audio):
# one set per line, name then its arguments. The first is the default.
# Every set is run by run.sh (the pictures must not change in any of them,
# the audio must equal its reference ref/SCENE.NAME.txt).
mixer_settings() {
  grep -v '^#' "$AUDIO_DIR/settings.txt" | grep -v '^$'
}

build_harness() {   # the default and the audio speed harness
  make -s -C "$ARM" -j4 build/perf >/dev/null || return 1
  make -s -C "$ARM" -j4 AUDIO=1 build/perf_audio >/dev/null || return 1
}

build_tone() {
  make -s -C "$ARM" -j4 -f ../audio/tone.mk AUDIO=1 build/audio/tone_test 2>&1 | grep -v '^lto-wrapper' ; [ -x "$ARM/build/audio/tone_test" ]
}

# gate_links SCENE: semihosting splits the command line at spaces, so the
# game, state and keys are linked under plain names in tests/arm/build/gate;
# sets GL_ROM GL_BIOS GL_STATE GL_KEYS (relative to tests/arm).
gate_links() {
  local d=$ARM/build/gate
  mkdir -p "$d"
  ln -sfn "$SC_GAME" "$d/$1.rom"
  ln -sfn "$GAMES/gba_bios.bin" "$d/bios"
  GL_ROM=build/gate/$1.rom
  GL_BIOS=build/gate/bios
  GL_STATE=-
  GL_KEYS=-
  if [ "$SC_STATE" != - ]; then ln -sfn "$SC_STATE" "$d/$1.state"; GL_STATE=build/gate/$1.state; fi
  if [ "$SC_KEYS" != - ]; then ln -sfn "$SC_KEYS" "$d/$1.keys"; GL_KEYS=build/gate/$1.keys; fi
}

# perf_functional HARNESS SCENE FRAMES ARGS...: runs the harness under plain
# qemu-arm (no timing model), prints its output.
perf_functional() {
  local h=$1 scene=$2 frames=$3
  shift 3
  scene_get "$scene" || return 1
  gate_links "$scene"
  (cd "$ARM" && "$QEMU" -cpu arm926 "build/$h" "$GL_ROM" "$GL_BIOS" "$GL_STATE" "$frames" "$GL_KEYS" 1 "$@")
}

# The speed model's results depend on the host's environment (qemu-user puts
# it on the guest's stack, and the stack's address moves cache lines) and on
# the length of the tree's path (the program's path is on the stack too). So
# the model runs with an empty guest environment plus a padding variable that
# makes the path and the padding 300 characters together: the same numbers
# in any tree and any shell. model_env CMD... runs CMD that way.
model_env() {
  local n=$((300 - ${#ARM}))
  [ $n -lt 1 ] && n=1
  local pad
  pad=$(head -c "$n" /dev/zero | tr '\0' x)
  env -i PATH="$PATH" HOME="$HOME" QEMU="$QEMU" PERF_AUDIO="${PERF_AUDIO:-}" QEMU_SET_ENV=PAD="$pad" \
      QEMU_UNSET_ENV=PATH,HOME,QEMU,PERF_AUDIO,QEMU_UNSET_ENV,QEMU_SET_ENV "$@"
}

# model_run TAG SCENE FRAMES [CALC_SPEED ARGS...]: the speed model on a scene
# (PERF_AUDIO=1 in the environment: the audio harness). The tag has to be the
# same for runs that are compared (the arguments are on the stack too).
model_run() {
  local tag=$1 scene=$2 frames=$3
  shift 3
  scene_get "$scene" || return 1
  local keys=$SC_KEYS
  model_env python3 -I "$ROOT/tests/calc_speed.py" "$SC_STATE" --rom "$SC_GAME" --keys "$keys" \
      --frames "$frames" --skip "$SC_SKIP" --tag "$tag" "$@"
}

# model_ref_text TAG SCENE FRAMES: what the gate stores for the default build:
# the model's report and its exact totals line (cycles, instructions, misses).
model_ref_text() {
  model_run "$@" --calc cx2 2>&1 || return 1
  sed -n 2p "$ARM/build/speed/$1.model"
}
