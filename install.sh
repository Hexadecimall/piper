#!/bin/sh
set -eu

prefix=
prefix_set=0
base=${PIPER_RELEASE_BASE_URL:-}
linkage=static
stage=release

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || { echo "install.sh: --prefix needs a directory" >&2; exit 2; }; prefix=$2; prefix_set=1; shift 2 ;;
        --base-url) [ "$#" -ge 2 ] || { echo "install.sh: --base-url needs a URL" >&2; exit 2; }; base=$2; shift 2 ;;
        --dynamic) linkage=dynamic; shift ;;
        --static) linkage=static; shift ;;
        --dev) stage=dev; shift ;;
        --release) stage=release; shift ;;
        -h|--help)
            echo "usage: install.sh [--prefix DIR] [--base-url URL] [--static|--dynamic] [--release|--dev]"
            echo ""
            echo "Defaults to /usr/local for root and a user-local bin directory otherwise."
            echo "--prefix DIR always installs into DIR/bin."
            exit 0 ;;
        *) echo "install.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done

if [ "$prefix_set" -eq 0 ]; then
    if [ "$(id -u)" -eq 0 ]; then
        prefix=/usr/local
    elif [ -n "${XDG_BIN_HOME:-}" ]; then
        prefix=${XDG_BIN_HOME%/}
    else
        [ -n "${HOME:-}" ] || { echo "install.sh: HOME is not set; pass --prefix" >&2; exit 2; }
        prefix=${HOME%/}/.local
    fi
fi

[ -n "$base" ] || { echo "install.sh: set PIPER_RELEASE_BASE_URL or pass --base-url" >&2; exit 2; }
base=${base%/}
case "$base" in http://*|https://*) ;; *) echo "install.sh: release URL must use HTTP or HTTPS" >&2; exit 2 ;; esac

case "$(uname -s)" in Darwin) os=macos ;; Linux) os=linux ;; MINGW*|MSYS*|CYGWIN*) os=windows ;; *) echo "install.sh: unsupported operating system" >&2; exit 1 ;; esac
case "$(uname -m)" in arm64|aarch64) arch=aarch64 ;; x86_64|amd64) arch=x86_64 ;; *) echo "install.sh: unsupported architecture" >&2; exit 1 ;; esac

suffix=
[ "$os" = windows ] && suffix=.exe
asset="piper-$arch-$os-$linkage-$stage$suffix"
work=$(mktemp -d "${TMPDIR:-/tmp}/piper-install.XXXXXX")
trap 'rm -f "$work/checksums.txt" "$work/$asset"; rmdir "$work" 2>/dev/null || true' EXIT HUP INT TERM
mkdir -p "$work"
curl --fail --location --silent --show-error "$base/checksums.txt" -o "$work/checksums.txt"
curl --fail --location --silent --show-error "$base/$asset" -o "$work/$asset"
expected=$(awk -v name="$asset" '$2 == name || $2 == "*" name { print $1; exit }' "$work/checksums.txt")
[ -n "$expected" ] || { echo "install.sh: checksum missing for $asset" >&2; exit 1; }
if command -v sha256sum >/dev/null 2>&1; then actual=$(sha256sum "$work/$asset" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then actual=$(shasum -a 256 "$work/$asset" | awk '{print $1}')
else actual=$(openssl dgst -sha256 "$work/$asset" | awk '{print $NF}'); fi
[ "$actual" = "$expected" ] || { echo "install.sh: SHA-256 verification failed" >&2; exit 1; }
if [ "$prefix_set" -eq 0 ] && [ -n "${XDG_BIN_HOME:-}" ] && [ "$(id -u)" -ne 0 ]; then
    bindir=$prefix
else
    bindir=$prefix/bin
fi
mkdir -p "$bindir"
chmod 755 "$work/$asset"
mv "$work/$asset" "$bindir/piper$suffix"
echo "installed piper to $bindir/piper$suffix"
