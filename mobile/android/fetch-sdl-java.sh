#!/bin/sh
# Copies SDL's Android Java glue (org.libsdl.app) for the SDL version the native build uses.
set -eu
SDL_TAG=release-3.4.8
DEST="$(cd "$(dirname "$0")" && pwd)/app/src/main/sdl-java"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -sL "https://github.com/libsdl-org/SDL/archive/refs/tags/$SDL_TAG.tar.gz" | tar -xz -C "$TMP"
rm -rf "$DEST"
mkdir -p "$DEST/org/libsdl"
cp -R "$TMP/SDL-$SDL_TAG/android-project/app/src/main/java/org/libsdl/app" "$DEST/org/libsdl/"
echo "SDL Java sources copied to $DEST"
