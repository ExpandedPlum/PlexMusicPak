#!/bin/sh
# Builds a small, audio-only ffmpeg for the TrimUI Brick / Smart Pro (tg5040).
# Runs inside ghcr.io/loveretro/tg5040-toolchain (see scripts/build-pak.sh).
#
# Only what plexd needs is enabled: the codecs and containers Plex serves
# music in, the http protocol (to reach plexd's loopback proxy), the volume
# filter for USB/Bluetooth software volume, and ALSA output.
#
# From Audiobookshelf.pak, which bundles the same build.
#
# Usage: build-ffmpeg.sh <output-dir> [native]
#   native: build for the host instead (used for testing the same config).
set -eu

FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.2}"
OUT="${1:?output dir}"
MODE="${2:-cross}"
WORK="${WORK:-/tmp/ffmpeg-build}"

mkdir -p "$WORK" "$OUT"
cd "$WORK"
if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
	if [ ! -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ]; then
		curl -fsSLO "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
	fi
	tar xf "ffmpeg-$FFMPEG_VERSION.tar.xz"
fi
cd "ffmpeg-$FFMPEG_VERSION"

DECODERS="aac,aac_latm,mp3,mp3float,mp2,mp2float,flac,alac,opus,vorbis,ac3,eac3,wmav1,wmav2,wmapro,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8,pcm_alaw,pcm_mulaw"
DEMUXERS="mov,mp3,aac,flac,ogg,matroska,wav,asf,ac3,eac3,aiff,w64"
PARSERS="aac,aac_latm,mpegaudio,flac,opus,vorbis,ac3"
FILTERS="atempo,aresample,aformat,anull,volume,afade"

set -- \
	--disable-everything \
	--disable-autodetect \
	--disable-doc \
	--disable-debug \
	--disable-ffplay \
	--disable-ffprobe \
	--disable-swscale \
	--disable-postproc \
	--enable-small \
	--enable-ffmpeg \
	--enable-network \
	--enable-protocol=file,pipe,http,tcp \
	--enable-decoder="$DECODERS" \
	--enable-demuxer="$DEMUXERS" \
	--enable-parser="$PARSERS" \
	--enable-filter="$FILTERS" \
	--enable-encoder=pcm_s16le \
	--enable-muxer=null,wav,pcm_s16le

case "$MODE" in
native)
	set -- "$@" --enable-alsa --enable-outdev=alsa --prefix="$WORK/native-install"
	;;
*)
	set -- "$@" \
		--enable-cross-compile \
		--cross-prefix="${CROSS_COMPILE:-aarch64-nextui-linux-gnu-}" \
		--arch=aarch64 --target-os=linux \
		--cpu=cortex-a53 \
		--sysroot="${SYSROOT:?SYSROOT not set; run inside the tg5040 toolchain}" \
		--pkg-config=pkg-config \
		--enable-alsa --enable-outdev=alsa \
		--extra-cflags="-O2" \
		--prefix="$WORK/install"
	;;
esac

./configure "$@"

make -j"$(nproc)" ffmpeg
if [ "$MODE" != native ]; then
	"${CROSS_COMPILE:-aarch64-nextui-linux-gnu-}strip" ffmpeg
else
	strip ffmpeg
fi
cp ffmpeg "$OUT/ffmpeg"
echo "ffmpeg built: $OUT/ffmpeg ($(du -h "$OUT/ffmpeg" | cut -f1))"
