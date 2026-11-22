#!/bin/sh
# Writes nspire_build_id.h (argument 1; arguments 2, 3, 4: the usbpad
# driver's directory, whether it's built in (1/0), and the code's root):
# the build id the link log's first line shows (nspire_link.c). The file is
# rewritten only when the id changes, so nothing rebuilds for nothing.
out=$1; pad=$2; padon=$3; root=${4:-.}
g=$(cd "$root" && git describe --always --dirty 2>/dev/null || echo nogit)
d=unknown
[ "$padon" = 1 ] && d=$(cd "$pad" && git describe --always --dirty 2>/dev/null || echo nogit)
new="#define NSPIRE_BUILD_GIT \"$g\"
#define NSPIRE_BUILD_PADS $padon
#define NSPIRE_BUILD_DRIVER \"$d\""
# The date is the commit's (in its own time zone), so a clean tree at one
# commit always builds the same bytes (a release's md5 can be checked by a
# rebuild); only an uncommitted ("-dirty") or git-less tree takes the clock.
when=
case $g in *-dirty|nogit) ;; *) when=$(cd "$root" && git log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M' 2>/dev/null) ;; esac
if [ -z "$when" ]; then
    # Dirty: kept while the id is the same (its date is the first build's).
    if [ -f "$out" ] && [ "$(sed '/NSPIRE_BUILD_DATE/d' "$out")" = "$new" ]; then exit 0; fi
    when=$(date '+%Y-%m-%d %H:%M')
fi
full=$(printf '%s\n#define NSPIRE_BUILD_DATE "%s"' "$new" "$when")
[ -f "$out" ] && [ "$(cat "$out")" = "$full" ] && exit 0
printf '%s\n' "$full" > "$out"
