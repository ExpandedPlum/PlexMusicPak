# Plex Music+ for NextUI

A Plex music player for the TrimUI Brick (`tg5040`) running
[NextUI](https://nextui.loveretro.games/). Browse and stream your Plex music
library, download albums for offline listening, and **keep listening while
you play games**.

This is a personal fork of
[thwonp/PlexMusicPak](https://github.com/thwonp/PlexMusicPak). On top of the
original it adds a background player service (`plexd`), Plex Relay support
for networks that can't reach your server directly, playlists, search,
favorites synced with Plex ratings, and in-game controls. It installs as its
own pak, **Plex Music+**, so it can't be mixed up with the original.

> Transcoding (the default, 128 kbps Opus) needs Plex Pass on the server. If
> you don't have it, set Streaming Quality and Download Quality to
> *Original* in Settings.

## Features

**Connecting**
- Sign in with a PIN at [plex.tv/link](https://plex.tv/link): no typing passwords on the handheld.
- Pick from every Plex server on your account.
- Reaches your server however it can. The app tries the home network address
  first, then the internet address, then **Plex Relay**. Relay works from
  networks that can't see your server, like an IoT VLAN or a phone hotspot.
  Settings shows which kind of connection is in use.
- On relay, playback switches to 128 kbps Opus, because the relay is too slow
  for original FLAC.
- If the connection drops mid-session, the app looks for a working connection
  again by itself.
- All network traffic goes through `plexd`, which checks HTTPS certificates.
- Each device gets its own Plex client ID, so it shows up as its own player in
  Plex.

**Browsing**
- Artists → Albums → Tracks, or a flat Albums list sorted by release year.
- **Playlists**: your Plex music playlists, regular and smart.
- **Search** for artists, albums and tracks with the on-screen keyboard.
- Album art next to every list. L1/R1 jump by letter or year.
- B cancels any load that's in progress.

**Playback**
- Plays MP3, FLAC, WAV, M4A/AAC, OGG and Opus, either as the original file or
  transcoded to Opus at the quality you choose.
- Starts in a couple of seconds; the next track is fetched in advance so the
  change between tracks is quick.
- Shuffle, repeat (All / One / Off), tap to skip, hold to seek.
- Reports what you play to Plex: Now Playing in Plex Web and Plexamp,
  play counts and history.

**Keeps playing while you game**
- Music plays in a small background service, `plexd`. It keeps going after
  you leave the app, in the NextUI menu and inside games.
- Games are muted while music plays and get their sound back when you pause.
  You can turn this off in Settings.
- **In-game controls**: play/pause and skip tracks without leaving the game.
  Earphone media buttons work too.
- Music pauses for a few seconds whenever an app starts or exits, to skip
  NextUI's full-volume blip. It also pauses before the device sleeps.
- Plugging in or unplugging USB-C or Bluetooth audio moves the music to the
  new output.

**Offline**
- Download albums and your favorites to the SD card, as original files or
  Opus.
- Offline mode (SELECT) browses and plays only what's downloaded, with the
  same Artists and Albums views.

**Favorites**
- Y on any track toggles it as a favorite (♥).
- *Favorite Tracks* on the home screen lists them all.
- Favorites are mirrored to Plex as five-star ratings, which Plexamp calls
  "loved". Changes made offline are queued and sent later.
- Y on *Favorite Tracks* syncs both ways: tracks you loved in Plexamp or Plex
  Web are added, and missing ones are downloaded.

**Other**
- Screen timeout and a pocket button lock (MENU+SELECT wakes the screen).
- Settings, sign-in and downloads are kept on the SD card and survive
  reboots and reinstalls.

## Install

1. Download `PlexMusic-install.zip` from
   [Releases](https://github.com/ExpandedPlum/PlexMusicPak/releases), or take
   the *PlexMusic-install* artifact from a
   [CI run](https://github.com/ExpandedPlum/PlexMusicPak/actions/workflows/ci.yml).
   GitHub wraps artifacts in a second zip, so unzip that one first.
2. Unzip `PlexMusic-install.zip` at the **root of the SD card**. It creates:

   ```
   Tools/tg5040/Plex Music+.pak/
     launch.sh  pak.json  res/  licenses/
     bin/tg5040/  plexmusic.elf  plexd  ffmpeg  libmsettings.so
   ```

3. If you have the original **Plex Music** from the Pak Store, uninstall it
   there or delete `Tools/tg5040/PlexMusic.pak`. Both use the same settings
   folder, so your sign-in, favorites and downloads carry over. Keeping both
   installed would make them overwrite each other's settings.

To update, unzip a newer `PlexMusic-install.zip` over the old one.

## First launch

1. Open **Tools → Plex Music+**.
2. A 4-character PIN appears. On a phone or computer, go to
   [plex.tv/link](https://plex.tv/link) and enter it.
3. Pick your server, then your music library (skipped if you only have one).

Next time it goes straight to your library.

## Controls

**Home and lists**

| Button | Action |
|---|---|
| D-pad | Move |
| A | Open / play |
| B | Back |
| L1 / R1 | Previous / next letter or year |
| SELECT | Toggle offline mode |
| Y on an album | Download it |
| Y on a track | Toggle favorite |
| Y on *Favorite Tracks* (online) | Sync favorites with Plex and download them |
| Y on search results | New search |

**On-screen keyboard**

| Button | Action |
|---|---|
| D-pad | Move |
| A | Type the key (or SPACE / DEL / OK) |
| B | Delete (cancel when empty) |
| X | Space |
| Y | Search |

**Now Playing**

| Button | Action |
|---|---|
| A | Play / pause |
| LEFT / RIGHT (tap) | Previous / next track |
| LEFT / RIGHT (hold) | Seek ±5 s |
| UP / DOWN | Seek ±30 s |
| L2 | Shuffle |
| R2 | Repeat: Off → All → One |
| Y | Toggle favorite |
| X | Stop playback |
| B | Back to browsing (keeps playing) |

**In a game or the NextUI menu**, while music is loaded:

| Button | Action |
|---|---|
| FN1, or SELECT+START, or earphone play button | Play / pause |
| SELECT+L1, or earphone previous | Previous track |
| SELECT+R1, or earphone next | Next track |

The Audiobookshelf pak uses the same buttons. Each one only responds while it
has something loaded, so stop one before using the other.

**Anywhere**

| Button | Action |
|---|---|
| START (short press) | Help |
| START (hold) | Quit, with confirmation |
| MENU+START | Quit right away |

Quitting leaves the music playing; press X on Now Playing first to stop it.

## Settings

| Setting | What it does |
|---|---|
| Server | Switch server. Also shows the connection in use: local, remote or relay. |
| Library | Switch music library. |
| Screen timeout | When the screen turns off. |
| Streaming Quality | Original, or Opus at a bitrate. On relay, Original plays as 128 kbps Opus. |
| Download Quality | The same choice for downloads. |
| Pocket Button Lock | While the screen is off, only MENU+SELECT wakes it, so buttons pressed in a pocket do nothing. |
| Mute Games While Playing | Silence games while music plays. |
| In-Game Controls | Turn the in-game buttons on or off. |
| Sign Out | Forget the account. Your other settings stay. |

## Troubleshooting

- **"Failed to create PIN"**: the device can't reach plex.tv. Check that
  Wi-Fi is connected and has internet access.
- **Phone hotspot won't connect**: the connection problem is on the device,
  not in the app. With an iPhone, turn on *Maximize Compatibility*, keep the
  Personal Hotspot screen open while the Brick connects, and rename the phone
  if its name has an apostrophe (Settings → General → About → Name). Updating
  to the latest
  [TrimUI firmware](https://trimui.com/pages/trimui-brick-firmware) also
  helps Wi-Fi stability.
- **Signs in but the library won't load**: the server isn't reachable by any
  route. Check that Remote Access is enabled on the server. The log lists
  each address that was tried.
- **Original quality stutters**: you're probably on relay; check the
  connection shown next to Server in Settings, and pick an Opus quality.
- **No sound in games**: pause the music, or turn off *Mute Games While
  Playing*.

## Files

| What | Where |
|---|---|
| App log | `.userdata/tg5040/logs/plexmusic.txt` |
| Player service log | `.userdata/tg5040/logs/plexd.txt` |
| Sign-in and app settings | `.userdata/shared/plexmusic/config.json` |
| Player service settings | `.userdata/shared/plexmusic/plexd.json` |
| Favorites | `.userdata/shared/plexmusic/favorites.json` |
| Downloads | `.userdata/shared/plexmusic/downloads/` |
| Album art cache | `.userdata/shared/plexmusic/art/` (trimmed automatically) |

All paths are relative to the SD card root.

In `plexd.json` you can change the in-game buttons, using names like `FN1`
or `SELECT+R1`, with commas between alternatives.

## How it works

- **`plexmusic.elf`** is the user interface, written in C with SDL2 and the
  NextUI API. It draws the screens and asks `plexd` to play things.
- **`plexd`** is a Go service that keeps running after the UI quits. It
  handles the play queue and playback (through `ffmpeg`, one process per
  track), connecting to Plex and reporting to it, game muting, and the
  in-game buttons. It listens on `/tmp/plexd.sock`. All of the UI's network
  requests go through `plexd` too, on `/tmp/plexd-net.sock`.
- `launch.sh` starts `plexd` and installs small NextUI hooks, so music pauses
  when the device sleeps and around app switches. `plexd` exits on its own a
  few minutes after it goes idle.

## Building

CI (`.github/workflows/ci.yml`) runs the `plexd` tests and builds the whole
pak on every push, then uploads both zips as artifacts. To publish a release,
run the *Build & Release* workflow (`.github/workflows/build-release.yml`) by
hand with a version number; it attaches both zips to a GitHub release.

To build locally you need Docker, Go and jq:

```sh
WS=/path/to/workspace scripts/build-pak.sh 1.0.0
```

- The repo has to be at `$WS/opencode/git/paks`, because the UI's Makefile
  finds NextUI at `../../../NextUI`. The script clones NextUI if it's missing.
- The UI, `libmsettings.so` and `ffmpeg` (`scripts/build-ffmpeg.sh`) are built
  in the `ghcr.io/loveretro/tg5040-toolchain` image; `plexd` with the host's
  Go.
- Set `FFMPEG_CACHE` to a directory to reuse an earlier ffmpeg build.
- The output is `$WS/PlexMusic-install.zip`, plus `$WS/PlexMusic.pak.zip`
  (the same files without the folder).

To run the `plexd` tests (they need `ffmpeg` installed on the host):

```sh
cd plexd && go test ./...
```

## Credits

Based on [PlexMusicPak](https://github.com/thwonp/PlexMusicPak) by Thwonp.
The background service is modeled on the
[Audiobookshelf pak](https://github.com/ExpandedPlum/ABS-NextUi) for NextUI.
Third-party licenses are in `licenses/`.
