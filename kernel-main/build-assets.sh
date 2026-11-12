#!/usr/bin/env bash
#
# Build the asset archive "boot install" / "boot repair" extract
#
#   ./build-assets.sh [manifest] [output]
#   ./build-assets.sh assets.txt bin/assets.tar
#
# The manifest (assets.txt) has one file per line:
#
#   <path under home>  <source>  [mode]
#
# Sources are relative to the manifest's folder, mode is octal (default
# 644; 755 makes the file executable on disk). A path ending in '/' with no
# source is an empty folder. '#' starts a comment.
#
# A folder line can name a group policy instead of a source:
#
#   sys/    policy=admin
#
# The folder and everything in it are put in that policy on install and on
# every repair (user, admin, system or network: the kernel's table in
# kernel/security/policy.h). The list goes into the archive as one file,
# ".policy" at its root ("<folder> <policy>" per line, parents first),
# which the kernel reads and does not write to disk.
#
# The output is plain POSIX ustar, built reproducibly (sorted, owner 0,
# mtime 0, 512-byte blocks), and only replaced when its content changes, so
# an unchanged archive does not rebuild the disk image. List it with
#   tar -tvf bin/assets.tar
#
# Environment:
#   ASSETS_STRICT   1: a missing source is an error. Default: a warning and
#                   the file is left out, so the kernel builds before the
#                   programs do (compile_cabinate_binaries.sh)
#   TAR             GNU tar to use (default tar; on macOS: TAR=gtar)
#
set -euo pipefail

MANIFEST=${1:-assets.txt}
OUT=${2:-bin/assets.tar}
STRICT=${ASSETS_STRICT:-0}
TAR=${TAR:-tar}

die() { echo "build-assets: $*" >&2; exit 1; }

[ -f "$MANIFEST" ] || die "manifest not found: $MANIFEST"
"$TAR" --version 2>/dev/null | grep -q "GNU tar" || die "GNU tar is required (set TAR=gtar)"

base=$(cd "$(dirname "$MANIFEST")" && pwd)
mkdir -p "$(dirname "$OUT")"

stage=$(mktemp -d "${TMPDIR:-/tmp}/assets.XXXXXX")
trap 'rm -rf "$stage"' EXIT

files=0
missing=0
line_no=0
declare -A seen=()
policies=()

# Must match the names in kernel/security/policy.c
POLICY_NAMES="user admin system network"

while IFS= read -r line || [ -n "$line" ]; do
    line_no=$((line_no + 1))
    line=${line%%#*}
    read -r dest src mode extra <<< "$line" || true
    [ -n "${dest:-}" ] || continue
    [ -z "${extra:-}" ] || die "$MANIFEST:$line_no: too many fields"

    # Paths stay inside home: relative, no "..", short enough for ustar
    case "/$dest/" in
        */../*|*/./*) die "$MANIFEST:$line_no: '.' or '..' in $dest" ;;
    esac
    [ "${dest#/}" = "$dest" ] || die "$MANIFEST:$line_no: $dest must be relative to home"
    (( ${#dest} <= 240 )) || die "$MANIFEST:$line_no: path too long for ustar: $dest"
    [ -z "${seen[$dest]:-}" ] || die "$MANIFEST:$line_no: $dest is listed twice"
    seen[$dest]=1

    [ "$dest" != ".policy" ] || die "$MANIFEST:$line_no: .policy is reserved for the policy list"

    # Folder, maybe with a policy
    if [ "${dest%/}" != "$dest" ]; then
        if [ -n "${src:-}" ]; then
            [ "${src#policy=}" != "$src" ] || die "$MANIFEST:$line_no: a folder takes no source (only policy=<name> [write=<name>])"
            policy=${src#policy=}
            write=
            if [ -n "${mode:-}" ]; then
                [ "${mode#write=}" != "$mode" ] || die "$MANIFEST:$line_no: after policy=<name> only write=<name>"
                write=${mode#write=}
            fi
            for p in "$policy" ${write:+"$write"}; do
                case " $POLICY_NAMES " in
                    *" $p "*) ;;
                    *) die "$MANIFEST:$line_no: unknown policy '$p' (one of: $POLICY_NAMES)" ;;
                esac
            done
            folder=${dest%/}
            [ -n "$folder" ] || die "$MANIFEST:$line_no: the home folder itself can't take a policy"
            policies+=("$folder $policy${write:+ $write}")
        fi
        mkdir -p "$stage/$dest"
        continue
    fi

    [ -n "${src:-}" ] || die "$MANIFEST:$line_no: no source for $dest"
    mode=${mode:-644}
    [[ "$mode" =~ ^[0-7]{3,4}$ ]] || die "$MANIFEST:$line_no: bad mode $mode"

    case "$src" in /*) path=$src ;; *) path=$base/$src ;; esac
    if [ ! -f "$path" ]; then
        [ "$STRICT" = 1 ] && die "$MANIFEST:$line_no: missing $src"
        echo "build-assets: warning: missing $src, $dest left out" >&2
        missing=$((missing + 1))
        continue
    fi

    install -D -m "$mode" "$path" "$stage/$dest"
    files=$((files + 1))
done < "$MANIFEST"

# The policy list, parents before the folders inside them (sorted by path,
# so "sys" comes before "sys/services" and the inner one wins)
if (( ${#policies[@]} > 0 )); then
    printf '%s\n' "${policies[@]}" | LC_ALL=C sort > "$stage/.policy"
    chmod 644 "$stage/.policy"
fi

# Folders the files land in get the usual mode whatever the umask
find "$stage" -mindepth 1 -type d -exec chmod 755 {} +

# Top-level entries by name (no "./" prefix; an empty list is a valid,
# empty archive)
entries=()
while IFS= read -r -d '' e; do
    entries+=("${e#"$stage"/}")
done < <(find "$stage" -mindepth 1 -maxdepth 1 -print0 | sort -z)

tmp="$OUT.tmp"
"$TAR" --format=ustar --blocking-factor=1 --sort=name \
       --owner=0 --group=0 --numeric-owner --mtime=@0 \
       -C "$stage" -cf "$tmp" ${entries[@]+"${entries[@]}"}

if [ -f "$OUT" ] && cmp -s "$tmp" "$OUT"; then
    rm -f "$tmp"
else
    mv -f "$tmp" "$OUT"
fi

bytes=$(stat -c %s "$OUT")
echo "built $OUT: $files files$( (( ${#policies[@]} > 0 )) && echo ", ${#policies[@]} folder policies"), $(( (bytes + 1023) / 1024 )) KB$( (( missing > 0 )) && echo ", $missing missing")"
