#!/bin/ash

set -u

work=/tmp/busybox-runtime

fail()
{
	echo "BUSYBOX_RUNTIME_FAIL $1"
	exit 1
}

rm -rf "$work" || fail cleanup
mkdir -p "$work" || fail mkdir
trap 'rm -rf "$work"' EXIT

busybox --list | LC_ALL=C sort > "$work/applets" || fail manifest-list
cmp /etc/busybox.applets "$work/applets" || fail manifest-dynamic
busybox-static --list | LC_ALL=C sort > "$work/applets-static" ||
	fail manifest-static-list
cmp /etc/busybox.applets "$work/applets-static" || fail manifest-static
printf '%s\n' BUSYBOX_MANIFEST_OK

shell_function()
{
	printf '%s' "$(( $1 * 2 ))"
}

alias shell_alias='printf alias-ok'
shell_value=$(printf '%s' command)
test "$shell_value" = command || fail shell-substitution
test "$(shell_function 21)" = 42 || fail shell-function
test "$(shell_alias)" = alias-ok || fail shell-alias
case alpha in
	a*) shell_case=yes ;;
	*) shell_case=no ;;
esac
test "$shell_case" = yes || fail shell-case
shell_total=0
for shell_number in 1 2 3 4; do
	shell_total=$((shell_total + shell_number))
done
test "$shell_total" = 10 || fail shell-loop
set -- -a value -b
OPTIND=1
shell_options=
while getopts a:b shell_option; do
	shell_options=$shell_options$shell_option
done
test "$shell_options" = ab || fail shell-getopts
printf '%s\n' BUSYBOX_ASH_LANGUAGE_OK

mkdir "$work/text" || fail text-mkdir
printf '%s\n' gamma alpha beta beta > "$work/text/input" ||
	fail text-input
test "$(awk 'NR == 2 { print $1 }' "$work/text/input")" = alpha ||
	fail text-awk
sed 's/beta/delta/' "$work/text/input" > "$work/text/sed" ||
	fail text-sed
grep -q delta "$work/text/sed" || fail text-grep
test "$(printf 'left:right\n' | cut -d: -f2)" = right ||
	fail text-cut
test "$(printf 'lower\n' | tr 'a-z' 'A-Z')" = LOWER ||
	fail text-tr
sort "$work/text/input" | uniq -c > "$work/text/counts" ||
	fail text-sort
grep -Eq '2[[:space:]]+beta' "$work/text/counts" || fail text-uniq
test "$(head -n 1 "$work/text/input")" = gamma || fail text-head
test "$(tail -n 1 "$work/text/input")" = beta || fail text-tail
test "$(wc -l < "$work/text/input")" = 4 || fail text-wc
printf '%s\n' one two | xargs printf '%s-' > "$work/text/xargs" ||
	fail text-xargs
test "$(cat "$work/text/xargs")" = one-two- || fail text-xargs-data
find "$work/text" -type f -name input | grep -q '/input$' ||
	fail text-find
cp "$work/text/input" "$work/text/copy" || fail text-copy
cmp "$work/text/input" "$work/text/copy" || fail text-cmp
diff -u "$work/text/input" "$work/text/copy" > "$work/text/diff" ||
	fail text-diff
printf '%s\n' BUSYBOX_TEXT_TOOLS_OK

mkdir "$work/archive-source" "$work/archive-output" || fail archive-mkdir
printf '%s\n' archive-payload > "$work/archive-source/payload" ||
	fail archive-input
tar -cf "$work/payload.tar" -C "$work/archive-source" payload ||
	fail archive-tar-create
tar -xf "$work/payload.tar" -C "$work/archive-output" ||
	fail archive-tar-extract
cmp "$work/archive-source/payload" "$work/archive-output/payload" ||
	fail archive-tar-data
gzip -c "$work/archive-source/payload" > "$work/payload.gz" ||
	fail archive-gzip
zcat "$work/payload.gz" > "$work/gzip-output" || fail archive-zcat
cmp "$work/archive-source/payload" "$work/gzip-output" ||
	fail archive-gzip-data
bzip2 -c "$work/archive-source/payload" > "$work/payload.bz2" ||
	fail archive-bzip2
bzcat "$work/payload.bz2" > "$work/bzip-output" || fail archive-bzcat
cmp "$work/archive-source/payload" "$work/bzip-output" ||
	fail archive-bzip-data
printf '%s' \
	'/Td6WFoAAATm1rRGBMAUECEBFgAAAAAAAAAAAEEgRR8BAA9hcmNoaXZlLXBheWxv' \
	'YWQKAOKJb0Cx2SoeAAEwELyTd+IftvN9AQAAAAAEWVo=' |
	base64 -d > "$work/payload.xz" || fail archive-xz-fixture
xzcat "$work/payload.xz" > "$work/xz-output" || fail archive-xzcat
cmp "$work/archive-source/payload" "$work/xz-output" ||
	fail archive-xz-data
mkdir "$work/cpio-source" "$work/cpio-output" || fail archive-cpio-mkdir
printf '%s\n' cpio-payload > "$work/cpio-source/payload" ||
	fail archive-cpio-input
(cd "$work/cpio-source" && printf '%s\n' payload | cpio -o -H newc) \
	> "$work/payload.cpio" 2>/dev/null || fail archive-cpio-create
(cd "$work/cpio-output" && cpio -id < "$work/payload.cpio") \
	2>/dev/null || fail archive-cpio-extract
cmp "$work/cpio-source/payload" "$work/cpio-output/payload" ||
	fail archive-cpio-data
printf '%s\n' BUSYBOX_ARCHIVE_TOOLS_OK

mkdir "$work/files" || fail file-mkdir
printf '%s\n' file-payload > "$work/files/source" || fail file-input
install -m 0600 "$work/files/source" "$work/files/installed" ||
	fail file-install
test "$(stat -c %a "$work/files/installed")" = 600 || fail file-mode
ln "$work/files/source" "$work/files/hard" || fail file-hardlink
ln -s source "$work/files/symbolic" || fail file-symlink
test "$(readlink "$work/files/symbolic")" = source || fail file-readlink
test "$(realpath "$work/files/symbolic")" = "$work/files/source" ||
	fail file-realpath
dd if="$work/files/source" of="$work/files/dd" bs=1 status=none ||
	fail file-dd
cmp "$work/files/source" "$work/files/dd" || fail file-dd-data
truncate -s 8192 "$work/files/truncated" || fail file-truncate
test "$(stat -c %s "$work/files/truncated")" = 8192 ||
	fail file-truncate-size
fsync "$work/files/truncated" || fail file-fsync
split -b 4 "$work/files/source" "$work/files/split-" || fail file-split
cat "$work/files"/split-* > "$work/files/joined" || fail file-join
cmp "$work/files/source" "$work/files/joined" || fail file-split-data
TZ=UTC touch -d '2024-01-02 03:04:05' "$work/files/timestamp" ||
	fail file-touch-date
test "$(stat -c %Y "$work/files/timestamp")" = 1704164645 ||
	fail file-timestamp
printf '%s\n' BUSYBOX_FILE_TOOLS_OK

test "$(id -u)" = 0 || fail account-id
test "$(whoami)" = root || fail account-whoami
groups | grep -qw root || fail account-groups
su -s /bin/sh nobody -c 'test "$(id -u)" = 65534' || fail account-su
cryptpw -m sha256 -S caffeinix secret | grep -q '^\$5\$caffeinix\$' ||
	fail account-cryptpw
if nologin > "$work/nologin" 2>&1; then
	fail account-nologin-status
fi
grep -q 'account is not available' "$work/nologin" ||
	fail account-nologin-output
printf '%s\n' BUSYBOX_ACCOUNT_TOOLS_OK

nice -n 1 true || fail process-nice
setsid true || fail process-setsid
sh -c 'renice 1 $$ >/dev/null' || fail process-renice
time -o "$work/time" sleep 0.01 || fail process-time
grep -q real "$work/time" || fail process-time-output
pgrep -x sh >/dev/null || fail process-pgrep
pstree | grep -q sh || fail process-pstree
printf '%s\n' BUSYBOX_PROCESS_TOOLS_OK

printf '2 + 3\n' | bc | grep -qx 5 || fail utility-bc
printf '2 3 + p\n' | dc | grep -qx 5 || fail utility-dc
cal 1 2024 | grep -q January || fail utility-cal
uuidgen | grep -Eq \
	'^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$' ||
	fail utility-uuidgen
tree "$work/files" | grep -q installed || fail utility-tree
printf 'timestamped\n' | ts | grep -q timestamped || fail utility-ts
root_device=
while read -r source target rest; do
	test "$target" = / && root_device=/dev/$source
done < /proc/mounts
test -n "$root_device" || fail utility-root-device
blkid "$root_device" | grep -q 'TYPE="ext4"' || fail utility-blkid
mountpoint -q /tmp || fail utility-mountpoint
printf '%s\n' BUSYBOX_SYSTEM_TOOLS_OK

printf '%s\n' BUSYBOX_RUNTIME_OK
