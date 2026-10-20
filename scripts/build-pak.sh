#!/bin/sh
# Builds PlexMusic.pak.zip: the UI (plexmusic.elf, libmsettings.so) and
# ffmpeg in the tg5040 toolchain container, plexd with the host's Go.
#
# Usage: scripts/build-pak.sh <version>
#
# Expects this repo at $WS/opencode/git/paks, because the UI Makefile finds
# NextUI at ../../../NextUI (cloned to $WS/opencode/NextUI if missing).
# Set FFMPEG_CACHE to a directory to reuse an ffmpeg built from the same
# scripts/build-ffmpeg.sh.
set -eu

VERSION="${1:?usage: build-pak.sh <version>}"
WS="${WS:-${GITHUB_WORKSPACE:?set WS or GITHUB_WORKSPACE}}"
PAKS="$WS/opencode/git/paks"
TOOLCHAIN="${TOOLCHAIN:-ghcr.io/loveretro/tg5040-toolchain:latest}"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.2}"
OUT="$PAKS/bin/tg5040"

mkdir -p "$OUT"
[ -d "$WS/opencode/NextUI" ] ||
	git clone --depth 1 https://github.com/LoveRetro/NextUI.git "$WS/opencode/NextUI"

echo "== libmsettings.so"
docker run --rm -v "$WS:$WS" "$TOOLCHAIN" sh -c '
	cd '"$WS"'/opencode/NextUI/workspace/tg5040/libmsettings
	${CROSS_COMPILE}gcc -c -fpic msettings.c
	${CROSS_COMPILE}gcc -shared -o libmsettings.so msettings.o -ltinyalsa -ldl -lrt
	cp libmsettings.so '"$OUT"'/
'

echo "== plexmusic.elf"
docker run --rm -v "$WS:$WS" "$TOOLCHAIN" sh -c 'cd '"$PAKS"'/src && make 2>&1'

echo "== ffmpeg"
if [ -n "${FFMPEG_CACHE:-}" ] && [ -x "$FFMPEG_CACHE/ffmpeg" ]; then
	cp "$FFMPEG_CACHE/ffmpeg" "$OUT/ffmpeg"
else
	WORK="$WS/build/ffmpeg-work"
	mkdir -p "$WORK"
	[ -f "$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz" ] ||
		curl -fsSL -o "$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz" \
			"https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
	docker run --rm -v "$WS:$WS" -e WORK="$WORK" -e FFMPEG_VERSION="$FFMPEG_VERSION" \
		"$TOOLCHAIN" sh "$PAKS/scripts/build-ffmpeg.sh" "$OUT"
	if [ -n "${FFMPEG_CACHE:-}" ]; then
		mkdir -p "$FFMPEG_CACHE"
		cp "$OUT/ffmpeg" "$FFMPEG_CACHE/ffmpeg"
	fi
fi

echo "== plexd"
(cd "$PAKS/plexd" && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath \
	-ldflags "-s -w -X github.com/ExpandedPlum/PlexMusicPak/plexd/internal/service.Version=$VERSION" \
	-o "$OUT/plexd" .)

echo "== package"
STAGE="$WS/staging/PlexMusic.pak"
rm -rf "$WS/staging" "$WS/PlexMusic.pak.zip"
mkdir -p "$STAGE/bin/tg5040"
cp "$PAKS/launch.sh" "$PAKS/pak.json" "$STAGE/"
cp -r "$PAKS/res" "$STAGE/res"
cp -r "$PAKS/licenses" "$STAGE/licenses"
for f in plexmusic.elf libmsettings.so plexd ffmpeg; do
	cp "$OUT/$f" "$STAGE/bin/tg5040/"
done
chmod +x "$STAGE/launch.sh" "$STAGE"/bin/tg5040/plexmusic.elf "$STAGE"/bin/tg5040/plexd "$STAGE"/bin/tg5040/ffmpeg
jq --arg v "$VERSION" '.version = $v' "$STAGE/pak.json" > "$WS/pak.json.tmp" && mv "$WS/pak.json.tmp" "$STAGE/pak.json"
# No top-level directory: the pak store unzips straight into the pak folder.
(cd "$STAGE" && zip -qr "$WS/PlexMusic.pak.zip" .)
ls -la "$WS/PlexMusic.pak.zip"
