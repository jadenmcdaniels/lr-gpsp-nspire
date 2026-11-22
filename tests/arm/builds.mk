# run.sh's builds (the tests' own, and the check builds of the same core with
# other switches: each one a folder of its own), all side by side under one
# make's jobs:  make -f builds.mk -j N
# Each build's log is build/built-NAME.log, its exit status build/built-NAME.rc
# (run.sh reads them); a failed one doesn't stop the others.
NAMES := main pcword idlecheck linecheck keepcheck keepcheckx ktr ktrn ktr16 ktr16n ktr32 ktr32n ktrx ktrxn

ARGS_main :=
ARGS_pcword := BUILD=build/pcword PCWORD_CHECK=1 CALC_ELF=- build/pcword/dynarec_test build/pcword/ram_code.gba
ARGS_idlecheck := BUILD=build/idlecheck IDLE_CHECK=1 CALC_ELF=- build/idlecheck/perf
ARGS_linecheck := BUILD=build/linecheck LINE_CHECK=1 CALC_ELF=- build/linecheck/perf
# speed10b's kept RAM blocks (see run.sh): the check builds, with a small PC table and ROM cache, and the trace
# builds with the blocks kept and without, in 16 and 32 KB RAM caches too; dynarec_test's ROM in three of them
ARGS_keepcheck := BUILD=build/keepcheck KEEP_CHECK=1 CALC_ELF=- build/keepcheck/perf build/keepcheck/dynarec_test build/keepcheck/ram_code.gba
ARGS_keepcheckx := BUILD=build/keepcheckx KEEP_CHECK=1 RAMPCS=1 ROMCACHE_KB=256 CALC_ELF=- build/keepcheckx/perf
ARGS_ktr := BUILD=build/ktr KEEP_TRACE=1 CALC_ELF=- build/ktr/perf build/ktr/dynarec_test build/ktr/ram_code.gba
ARGS_ktrn := BUILD=build/ktrn KEEP_TRACE=1 NOKEEP=1 CALC_ELF=- build/ktrn/perf build/ktrn/dynarec_test build/ktrn/ram_code.gba
ARGS_ktr16 := BUILD=build/ktr16 KEEP_TRACE=1 RAMCACHE_KB=16 CALC_ELF=- build/ktr16/perf
ARGS_ktr16n := BUILD=build/ktr16n KEEP_TRACE=1 RAMCACHE_KB=16 NOKEEP=1 CALC_ELF=- build/ktr16n/perf
ARGS_ktr32 := BUILD=build/ktr32 KEEP_TRACE=1 RAMCACHE_KB=32 CALC_ELF=- build/ktr32/perf
ARGS_ktr32n := BUILD=build/ktr32n KEEP_TRACE=1 RAMCACHE_KB=32 NOKEEP=1 CALC_ELF=- build/ktr32n/perf
ARGS_ktrx := BUILD=build/ktrx KEEP_TRACE=1 RAMPCS=1 ROMCACHE_KB=256 CALC_ELF=- build/ktrx/perf
ARGS_ktrxn := BUILD=build/ktrxn KEEP_TRACE=1 RAMPCS=1 ROMCACHE_KB=256 NOKEEP=1 CALC_ELF=- build/ktrxn/perf

all: $(addprefix built-,$(NAMES))
.PHONY: all $(addprefix built-,$(NAMES))

$(addprefix built-,$(NAMES)): built-%:
	@mkdir -p build
	+@$(MAKE) -s $(ARGS_$*) > build/built-$*.log 2>&1; echo $$? > build/built-$*.rc
