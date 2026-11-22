# The frontend's code that no game frame runs and no load or save waits
# on (menus and their pages, the managers, dialogs, logs, newlib's qsort and
# strstr, the settings kept in the program file, a USB drive's folders, the
# link speed's marker, the controller side's power cycle): built -Os without LTO
# (Makefile COLD_OBJS; nspire_lineio.ld; r16's
# shrink); so is the drivers' code that runs at a device's attach and in
# menus and loads (COLD_DRIVERS: USB drives, the USB audio descriptors).
# The rest of the frontend is -O2, a few files -O3 (FRONT_O3_C below).
# tests/arm builds its copies of these files for ui_bench and libbench the
# same way. Bluetooth's glue and page are cold too (its per-frame sites
# call nothing without a dongle: nspire_bt_glue.h NSPIRE_BT_IDLE).
COLD_C := gui nspire_ui_screens nspire_dialog nspire_controls nspire_bind \
  nspire_save_manager nspire_rom_manager nspire_loading nspire_padpage nspire_drive_page \
  nspire_audio_page nspire_exit nspire_padlog nspire_devices nspire_drive_saves \
  nspire_bt_glue nspire_bt_page newlib_qsort newlib_strstr nspire_usbcycle nspire_link_speed \
  nspire_selfstore nspire_drive
COLD_DRIVERS := drive_msc drive_usbms drive_os audio_uac

# The rest of the frontend is -O2 (no LTO either; r16's
# shrink). Kept -O3: what tests/libbench.sh times slower at -O2 (the
# states' packer nspire_fastdeflate.c: a save +2.6%; the ROM manager's
# conversions), libretro-common and savestate.c (paths and strings of every
# load; nothing to gain), what reads ROMs (nspire_zip.c, nspire_gbz.c), the
# sound's per-sample loops (nspire_audio.c, the driver's pcm.c: audio_pcm),
# nspire_video_ui.c (the speed model's frame), and the menus' and the OSD's
# drawing (nspire_ui.c, nspire_osd.c: final/ui7 rewrites them; their flags
# then, measured with tests/arm/ui_bench.sh).
FRONT_O3_C := nspire_fastdeflate nspire_rom_convert savestate nspire_zip nspire_gbz nspire_audio audio_pcm \
  nspire_video_ui nspire_ui nspire_osd \
  compat_posix_string compat_strl fopen_utf8 encoding_utf file_path file_path_io file_stream stdstring \
  rtime vfs_implementation

# r16: the frontend files whose data gets its own sections (-fdata-sections),
# so --gc-sections drops the data of functions nothing calls: UI7 left
# nspire_ui.c's big-word drawing (ui_big_number, ui_giant, ui_backdrop_word...)
# unused, and with it three fonts (d62, d100, d125: 22.6 KB raw). Measured
# with tests/arm/ui_bench.sh (r16).
FRONT_DATA_SECTIONS_C := nspire_ui

# zlib: the SDK's own sources (thirdparty/zlib, 1.2.11: what its lib/libz.a
# was built from, nspire-gcc -O3 -DHAVE_HIDDEN, the same instructions), built
# by the Makefile with -ffunction-sections (--gc-sections drops what nothing
# calls: deflateParams, inflateSync, the crc32 combines...) and
# DYNAMIC_CRC_TABLE (crc32's 8 KB of tables made in .bss at its first call,
# ~0.1 ms once, instead of in the program file). Linked instead of libz.a's;
# tests/arm's libbench links the same (r16's shrink).
ZLIB_C := adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil
ZLIB_FLAGS := -O3 -DHAVE_HIDDEN -DDYNAMIC_CRC_TABLE -ffunction-sections

# newlib's printf family as the program links it. R6b: without its
# floating-point formatting (no %f/%e/%g/%a anywhere in what's linked: our
# code, the libretro glue, nspire-usbpad, Ndless's libs); its whole-number
# versions are the same code built INTEGER_ONLY (%d %u %x %s %c %p, l, ll,
# widths: checked equal under qemu). -15,485 bytes of the .tns (after sscanf
# went). r16: the sprintf family through the stream one too (newlib's
# __sfvwrite_r writes a string FILE as its string code does: snprintf's
# truncation and count, sprintf, vsnprintf, snprintf(NULL, 0)), so its second
# copy of the formatting code (svfiprintf.o) isn't linked: -1,910 bytes; +54
# instructions a call, the same cycles (r16's shrink).
# tests/arm/printf_test.c checks 725 calls both ways (run.sh).
PRINTF_LDFLAGS := -Wl,--defsym=_vfprintf_r=_vfiprintf_r -Wl,--defsym=_svfprintf_r=_vfiprintf_r \
  -Wl,--defsym=_svfiprintf_r=_vfiprintf_r
