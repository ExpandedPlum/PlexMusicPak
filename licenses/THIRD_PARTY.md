# Third-party software in the pak

Besides the libraries compiled into `plexmusic.elf` (see `src/include/` and
`src/audio/` for their sources and license headers), the pak bundles:

| Component | Used for | License |
|-----------|----------|---------|
| [FFmpeg](https://ffmpeg.org) 7.1.2 | Audio decoding and output for plexd (`bin/tg5040/ffmpeg`) | LGPL-2.1-or-later |
| [Go x/crypto x509roots/fallback](https://pkg.go.dev/golang.org/x/crypto/x509roots/fallback) | Mozilla CA bundle for HTTPS in plexd when the device has none | BSD-3-Clause |

FFmpeg is built from the unmodified upstream source release
(`https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz`) without `--enable-gpl` or
`--enable-nonfree`, using the configuration in `scripts/build-ffmpeg.sh` of this
repository. That script, together with the upstream source, is everything
needed to rebuild the bundled binary.
