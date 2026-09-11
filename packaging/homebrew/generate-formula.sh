#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
    echo "usage: generate-formula.sh VERSION HOMEPAGE BASE_URL CHECKSUMS OUTPUT" >&2
    exit 2
fi

version=$1
homepage=${2%/}
base=${3%/}
checksums=$4
output=$5
template=$(dirname "$0")/piper.rb.in

case "$version" in *[!0-9A-Za-z._-]*|'') echo "generate-formula.sh: invalid version" >&2; exit 2 ;; esac
case "$homepage" in http://*|https://*) ;; *) echo "generate-formula.sh: homepage must use HTTP or HTTPS" >&2; exit 2 ;; esac
case "$base" in http://*|https://*) ;; *) echo "generate-formula.sh: release URL must use HTTP or HTTPS" >&2; exit 2 ;; esac
[ -f "$checksums" ] || { echo "generate-formula.sh: checksums file not found" >&2; exit 2; }
[ -f "$template" ] || { echo "generate-formula.sh: formula template not found" >&2; exit 2; }

checksum() {
    value=$(awk -v name="$1" '$2 == name || $2 == "*" name { print $1; exit }' "$checksums")
    case "$value" in
        *[!0-9A-Fa-f]*|'') echo "generate-formula.sh: invalid or missing checksum for $1" >&2; exit 1 ;;
    esac
    [ "${#value}" -eq 64 ] || { echo "generate-formula.sh: invalid checksum length for $1" >&2; exit 1; }
    printf '%s' "$value"
}

macos_arm64=$(checksum piper-aarch64-macos-static-release)
macos_x86_64=$(checksum piper-x86_64-macos-static-release)
linux_x86_64=$(checksum piper-x86_64-linux-static-release)

escaped_homepage=$(printf '%s' "$homepage" | sed 's/[&|]/\\&/g')
escaped_base=$(printf '%s' "$base" | sed 's/[&|]/\\&/g')
temporary=$output.tmp.$$
trap 'rm -f "$temporary"' EXIT HUP INT TERM

sed \
    -e "s|@VERSION@|$version|g" \
    -e "s|@HOMEPAGE@|$escaped_homepage|g" \
    -e "s|@BASE_URL@|$escaped_base|g" \
    -e "s|@MACOS_ARM64_SHA256@|$macos_arm64|g" \
    -e "s|@MACOS_X86_64_SHA256@|$macos_x86_64|g" \
    -e "s|@LINUX_X86_64_SHA256@|$linux_x86_64|g" \
    "$template" > "$temporary"
mv "$temporary" "$output"
trap - EXIT HUP INT TERM
