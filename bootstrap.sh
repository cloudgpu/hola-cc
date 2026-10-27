#!/usr/bin/env bash
# Bootstrap script for hola-cc: install the build dependencies, find or build
# a Hola ADK, build holacc and its runtime, and install them.
#
# usage: ./bootstrap.sh
#
# Environment:
#   PREFIX        install prefix (default /usr/local; sudo is used only if it is not writable)
#   HOLA_ADK_DIR  a Hola ADK to use instead of searching for one
#   HOLA_ADK_VERSION  download this ADK version (default: the newest published)
#   HOLA_ADK_URL      download the ADK archive from this URL instead (with URL.sha256 beside it)
#   SKIP_DEPS=1   do not install packages
#   RUN_TESTS=1   run `make test` before installing
#
# A usable ADK has lib/libhola.a, lib/libhola_tools.a and
# include/hola/builtin_tools.h. The script uses the first of:
#   1. HOLA_ADK_DIR;
#   2. an installed Hola that has all three (/opt/hola/hola_core,
#      ~/.local/hola/hola_core; released packages up to 1.31 do not);
#   3. an ADK downloaded earlier into .hola-adk;
#   4. the Hola source tree this directory belongs to (../scripts/build-adk.sh);
#   5. the ADK published with Hola's releases on GitHub (cloudgpu/hola-releases),
#      downloaded and checked against its SHA-256.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"
PREFIX="${PREFIX:-/usr/local}"

say() { printf '%s\n' "$*"; }
die() { printf 'bootstrap: %s\n' "$*" >&2; exit 1; }

# ---- 1. build dependencies ---------------------------------------------

if [ "${SKIP_DEPS:-0}" != 1 ]; then
    if command -v pacman >/dev/null; then
        sudo pacman -S --needed --noconfirm base-devel flex bison clang curl sqlite
    elif command -v apt-get >/dev/null; then
        sudo apt-get update
        sudo apt-get install -y build-essential flex bison clang libcurl4-openssl-dev libsqlite3-dev
    elif command -v dnf >/dev/null; then
        sudo dnf install -y gcc make flex bison clang libcurl-devel sqlite-devel
    else
        die "unsupported package manager; install a C compiler, make, flex, bison, clang, and the libcurl and sqlite3 development packages, then run with SKIP_DEPS=1"
    fi
fi

for tool in cc make flex bison clang curl tar sha256sum; do
    command -v "$tool" >/dev/null || die "$tool is not installed"
done

# ---- 2. the Hola ADK ---------------------------------------------------

is_adk() {
    [ -f "$1/lib/libhola.a" ] && [ -f "$1/lib/libhola_tools.a" ] && [ -f "$1/include/hola/builtin_tools.h" ]
}

adk=""
if [ -n "${HOLA_ADK_DIR:-}" ]; then
    is_adk "$HOLA_ADK_DIR" || die "HOLA_ADK_DIR=$HOLA_ADK_DIR is not a complete Hola ADK (needs lib/libhola.a, lib/libhola_tools.a, include/hola/builtin_tools.h)"
    adk="$HOLA_ADK_DIR"
else
    for cand in /opt/hola/hola_core "$HOME/.local/hola/hola_core" "$here/.hola-adk"; do
        if is_adk "$cand"; then adk="$cand"; break; fi
    done
fi

releases="https://github.com/cloudgpu/hola-releases"

download_adk() {
    [ "$(uname -m)" = x86_64 ] || die "the published Hola ADK is for x86_64 Linux only"
    local url
    if [ -n "${HOLA_ADK_URL:-}" ]; then
        url="$HOLA_ADK_URL"          # a mirror; <url>.sha256 must sit next to it
    elif [ -n "${HOLA_ADK_VERSION:-}" ]; then
        url="$releases/releases/download/v$HOLA_ADK_VERSION/hola-adk-$HOLA_ADK_VERSION-linux-x86_64.tar.gz"
    else
        # releases are listed newest first; take the first that has an ADK
        url="$(curl -fsSL "https://api.github.com/repos/cloudgpu/hola-releases/releases?per_page=30" |
               grep -o '"browser_download_url": *"[^"]*/hola-adk-[^"]*-linux-x86_64\.tar\.gz"' |
               head -1 | sed 's/.*"\(https[^"]*\)"$/\1/')" || true
        [ -n "$url" ] || die "no Hola ADK is published yet at $releases; set HOLA_ADK_DIR or HOLA_ADK_VERSION"
    fi
    local tmp
    tmp="$(mktemp -d)"
    say "downloading $url"
    curl -fsSL -o "$tmp/adk.tar.gz" "$url" || die "download failed: $url"
    curl -fsSL -o "$tmp/adk.tar.gz.sha256" "$url.sha256" || die "download failed: $url.sha256"
    local want have
    want="$(cut -d' ' -f1 "$tmp/adk.tar.gz.sha256")"
    have="$(sha256sum "$tmp/adk.tar.gz" | cut -d' ' -f1)"
    [ "$want" = "$have" ] || die "the downloaded ADK does not match its SHA-256; not using it"
    mkdir -p "$tmp/x"
    tar -C "$tmp/x" -xzf "$tmp/adk.tar.gz"
    rm -rf "$here/.hola-adk"
    mv "$tmp"/x/hola-adk-* "$here/.hola-adk"
    rm -rf "$tmp"
    is_adk "$here/.hola-adk" || die "the downloaded archive is not a complete Hola ADK"
}

if [ -z "$adk" ]; then
    if [ -f "$here/../scripts/build-adk.sh" ] && [ -d "$here/../hola_core" ]; then
        say "building the Hola ADK from $(cd "$here/.." && pwd)"
        bash "$here/../scripts/build-adk.sh" "$here/.hola-adk" >/dev/null
        rm -f "$here/.hola-adk.tar.gz"
        is_adk "$here/.hola-adk" || die "building the Hola ADK failed"
    else
        download_adk
    fi
    adk="$here/.hola-adk"
fi

adk="$(cd "$adk" && pwd)"
export HOLA_ADK_DIR="$adk"
say "using the Hola ADK at $adk ($(cat "$adk/VERSION" 2>/dev/null || echo unknown version))"

# ---- 3. build ----------------------------------------------------------

make clean >/dev/null
make all

if [ "${RUN_TESTS:-0}" = 1 ]; then
    make test
fi

# ---- 4. install --------------------------------------------------------

mkdir -p "$PREFIX" 2>/dev/null || true
if [ -w "$PREFIX" ]; then
    make install PREFIX="$PREFIX" HOLA_ADK_DIR="$adk"
else
    say "installing to $PREFIX with sudo"
    sudo make install PREFIX="$PREFIX" HOLA_ADK_DIR="$adk"
fi

say "installed holacc to $PREFIX/bin/holacc"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "note: $PREFIX/bin is not in PATH" ;;
esac
env -u HOLA_ADK_DIR -u HOLACC_RUNTIME_DIR "$PREFIX/bin/holacc" doctor
