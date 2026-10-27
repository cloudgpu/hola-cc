#!/usr/bin/env sh
# Installs the Hola Agent Toolchain from this unpacked directory.
#   ./install.sh [PREFIX]      default: ~/.local  (use /usr/local with sudo)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
prefix="${1:-$HOME/.local}"
mkdir -p "$prefix/bin" "$prefix/lib" "$prefix/share"
rm -rf "$prefix/lib/holacc" "$prefix/lib/hola-adk" "$prefix/share/hola-agent-toolchain"
cp "$here/bin/holacc" "$prefix/bin/holacc"
cp -R "$here/lib/holacc" "$here/lib/hola-adk" "$prefix/lib/"
cp -R "$here/share/hola-agent-toolchain" "$prefix/share/"
echo "installed holacc $(cat "$here/VERSION") to $prefix"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "add $prefix/bin to your PATH";; esac
"$prefix/bin/holacc" doctor || true
