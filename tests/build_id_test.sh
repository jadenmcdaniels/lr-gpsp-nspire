#!/bin/sh
# tools/build_id.sh: a clean tree's build id takes the commit's date, not the
# clock, so two builds of one commit are byte-identical (the release md5 is
# checked by a rebuild); a dirty tree takes the clock. A shim `date` gives a
# clock far from the commit's date. Prints "build_id: N checks, F failed".
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
mkdir "$t/bin" "$t/repo"
printf '#!/bin/sh\necho "2099-12-31 23:59"\n' > "$t/bin/date"
chmod +x "$t/bin/date"
n=0 f=0
check() { n=$((n + 1)); if [ "$1" = "$2" ]; then :; else f=$((f + 1)); echo "FAIL: $3: got '$1' want '$2'"; fi; }
date_of() { sed -n 's/.*NSPIRE_BUILD_DATE "\(.*\)"/\1/p' "$1"; }
cd "$t/repo"
git init -q . && echo a > a
git add a
GIT_COMMITTER_DATE="2026-01-02T03:04:00+01:00" GIT_AUTHOR_DATE="2026-01-02T03:04:00+01:00" \
    git -c user.name=t -c user.email=t@t commit -q -m a
run() { PATH="$t/bin:$PATH" sh "$here/tools/build_id.sh" "$1" /nonexistent 0 "$t/repo"; }
run "$t/h1"
check "$(date_of "$t/h1")" "2026-01-02 03:04" "clean tree: the commit's date"
run "$t/h2"
check "$(cmp -s "$t/h1" "$t/h2" && echo same)" same "clean tree: two runs, same file"
# A header left by an older build (clock date, same id) is rewritten.
sed 's/2026-01-02 03:04/2025-05-05 05:05/' "$t/h1" > "$t/h3"
run "$t/h3"
check "$(date_of "$t/h3")" "2026-01-02 03:04" "stale date rewritten"
echo b > a
run "$t/h4"
check "$(date_of "$t/h4")" "2099-12-31 23:59" "dirty tree: the clock"
check "$(grep -c -- '-dirty' "$t/h4")" 1 "dirty tree: -dirty in the id"
echo "build_id: $n checks, $f failed"
[ "$f" = 0 ]
