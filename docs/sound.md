# Sound mixer notes (branch final/sound)

The calculators have no speaker, so every build is compiled with
`-DNSPIRE_NO_AUDIO` (sound.c keeps only what the game can see). The aim of this
lane: sound through a USB sound adapter. The emulator side first (this file),
the USB side is separate (nspire-usbaudio, `src/pcm.c` is the consumer).

## 2026-11-12  Ground (no mixer code changed)

What and why: make the mixer buildable and testable again and measure it,
before changing anything.

- `make -C tests/arm AUDIO=1 build/perf_audio`: the speed harness without
  `-DNSPIRE_NO_AUDIO` (own objects in `tests/arm/build/audio`). Each frame ends
  by draining the mixer with `sound_read_samples`; hashes of the audio
  (`audio FRAME HASH SAMPLES`) and `wav=PATH wavfrom= wavto=`. The default
  `build/perf` is byte for byte the trunk's. `PERF_AUDIO=1 tests/calc_speed.sh`
  runs the audio harness under the model; "audio" part = sound_timer +
  render_gbc_sound + sound_read_samples; the harness's own hashing is a region
  "harness" the totals leave out.
- `tests/audio/run.sh` is the gate (73 s): (1) default build untouched (binary
  md5 and exact model numbers for aw and emerald, 600 frames), (2) pictures equal
  with the mixer on, (3) audio hashes equal `tests/audio/ref/`, (4) unit tests
  (`tone_test`, 264 checks; `unit_*.sh` files in tests/audio are run too).
  `--rebase` only where a task says the output may change.
  Mixer settings of later steps: one line each in `tests/audio/settings.txt`
  (name, then `perf_audio` key=value arguments); run.sh runs them all.
- `tests/audio/speed.sh [--calc cx2|cx192]`: the cost table. `wav.sh SCENE
  OUT.wav [k=v]`: a clip with its peak/RMS. `stats/run.sh`: call counts (a
  measuring build with counters, never committed).
- The model's cycles depend on the host environment and the tree's path length
  (both are on qemu-user's guest stack): `lib.sh model_env` pins them. Always
  compare numbers produced through tests/audio.

Numbers (cx2, ms per frame, audio part = timer + render + read): aw 0.86 (5.7%
of the frame), emerald 0.92 (7.2%), fe 1.11 (5.6%), gs 0.77 (5.0%), mc 0.81
(6.6%); cx192: 1.47 to 2.05 ms (5.3 to 7.7%). Costliest: sound_timer (448-704
calls a frame, ~270 cycles each), render_gbc_sound (tone channels on with
volume 0 are rendered), sound_read_samples (24.6 cycles per s16, flat 0.14 ms).

The original recordings included routing crosstalk and lost wave RAM writes;
these are corrected below. Their measured DC offset was -3 to -6% of full scale.

## 2026-11-14  Integer steps and timer boundaries

The recovered integer-step work now checks every square/sweep/wave rate, all
128 noise combinations, and all 65,536 timer reloads at four prescalers, with
both clock bases. Tone/noise steps exactly match their old floating formulas.
The timer formula is the exact rational floor: at the normal clock 1,198 inputs
differ from float (maximum 43 low bits, at reload 3); at the 60 Hz clock 1,075
differ (maximum 13). The previous unfinished test excluded those valid short
periods from its claimed one-bit bound. It no longer does.

Reload 1 cannot fit in 8.24: it now saturates at UINT32_MAX instead of wrapping
to zero. Zero and near-UINT32_MAX steps can no longer hang either mixer path.
Synthetic cases cover zero, one, the 1.0 boundary, and additions that wrap u32.
Existing five-scene and synthetic recordings are unchanged; only the
no-audio binary/model references change for this deliberate bug fix.

Sound-state loading now validates all six channel documents, exact FIFO/wave
lengths, table indices, routing, volume and shifts before changing anything.
The old checker reused its channel index for its field loop and skipped
`ds1` and `gs1..3`. Loading clears queued audio from the old timeline; changing
noise modes normalizes the phase before indexing the smaller noise table.
The sound tone suite has 369 assertions, including rejection without partial
mutation. The recovered FIFO batch/DMA optimizations keep their original
state-equivalence and 20,000 DMA comparison tests; their word stores now use
`memcpy` so the FIFO's byte-array type does not violate C alias rules.

The no-audio build now advances PSG phase, length, envelope and sweep state in
runs ending at the next 256 Hz counter edge. Previously it froze all of these
while the PCM build advanced them, so the game could distinguish the builds
through SOUNDCNT_X. Unrouted and zero-volume PCM channels use the same cheap
state-only path. `unit_state.sh` compares 240 complete serialized sound states
and status registers across the two builds, including routing/volume changes,
length expiry, sweeps, envelopes, noise widths and extreme FIFO steps.

## 2026-11-14  Routing, wave banks and noise

Corrected the left-routing mask that also enabled the next channel's right
output. Wave writes now target the opposite playback bank, survive consecutive
writes/bank switches with no elapsed time, work while master sound is off, and
restore the correct CPU-visible bank on a switch. Two-bank playback starts at
the selected bank. These rules are described in
[GBATEK](https://problemkaputt.de/gbatek.htm#gbasoundchannel3waveoutput) and
[gbadoc's wave example](https://gbadev.net/gbadoc/audio/sound3.html).

The 7-bit noise table now wraps at its full 127-state period, not 126. Channel
status clears in the same render that expires its length, as documented by
[SOUNDCNT_X](https://problemkaputt.de/gbatek.htm#gbasoundcontrolregisters).
The old tone test incorrectly required a 1/256-second sound to remain on after
a whole frame; it now requires that short length to have expired.

The audio references are deliberately regenerated for these fixes. Targeted
assertions cover crosstalk, both wave banks at zero elapsed time and with sound
disabled, the selected 64-sample starting bank, a 127-step noise cycle and width
changes without retriggering. This is emulated-mixer testing; USB playback has
not been validated on the calculators. Runtime rates/mono/small-ring settings
and a gpSP USB sink are still outside these completed core fixes.

Final core checks: the five-scene audio gate passes all recording hashes and
388 picture comparisons; 378 tone assertions, 240 cross-build state snapshots,
both exhaustive integer-step tests, and 316 host core checks pass. The default
calculator build links with USB gamepads. Null/oversized PCM-read requests are
bounded; resetting the read cursor on state load also removes the old saved
cursor's stale silent prefix from the Advance Wars and Emerald recordings.
