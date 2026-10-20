#!/bin/sh
# PlexMusic.pak — NextUI launcher entry point.
#
# Starts the plexd playback daemon (if it isn't already running) detached from
# this script, so music keeps playing after the UI exits and while games run,
# then shows the UI.
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# Use system PLATFORM variable, fallback to tg5040 if not set
[ -z "$PLATFORM" ] && PLATFORM="tg5040"

# DEVICE=brick shares the tg5040 binary (same chipset)
[ "$DEVICE" = "brick" ] && PLATFORM="tg5040"

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${USERDATA_PATH:=$SDCARD_PATH/.userdata/$PLATFORM}"
: "${SHARED_USERDATA_PATH:=$SDCARD_PATH/.userdata/shared}"
: "${LOGS_PATH:=$USERDATA_PATH/logs}"
export SDCARD_PATH PLATFORM USERDATA_PATH SHARED_USERDATA_PATH LOGS_PATH
mkdir -p "$LOGS_PATH"

BIN="$DIR/bin/$PLATFORM"
export LD_LIBRARY_PATH="$DIR:$DIR/bin:$BIN:$LD_LIBRARY_PATH"

PLEXD="$BIN/plexd"
DAEMON_LOG="$LOGS_PATH/plexd.txt"
# The UI uses these to restart the daemon if it isn't running.
export PLEXD_BIN="$PLEXD" PLEXD_LOG_FILE="$DAEMON_LOG"

if [ -x "$PLEXD" ]; then
	# Pause right before the device goes to sleep (NextUI runs *.sync.sh
	# hooks synchronously before suspending).
	HOOK_DIR="$USERDATA_PATH/.hooks/pre-sleep.d"
	mkdir -p "$HOOK_DIR"
	cat > "$HOOK_DIR/plexmusic.sync.sh" <<EOF
#!/bin/sh
[ -S /tmp/plexd.sock ] && "$PLEXD" ctl presleep >/dev/null 2>&1
exit 0
EOF
	chmod +x "$HOOK_DIR/plexmusic.sync.sh"

	# Every NextUI app (this one included) sets the speaker to full volume for
	# a moment as it starts, which is a loud blip with music playing. Hold the
	# music for a few seconds around each app switch; it resumes on its own.
	for phase in pre-launch.d post-launch.d; do
		HOOK_DIR="$USERDATA_PATH/.hooks/$phase"
		mkdir -p "$HOOK_DIR"
		cat > "$HOOK_DIR/plexmusic.sync.sh" <<EOF
#!/bin/sh
[ -S /tmp/plexd.sock ] || exit 0
"$PLEXD" ctl transition '{"seconds":3}' >/dev/null 2>&1
exit 0
EOF
		chmod +x "$HOOK_DIR/plexmusic.sync.sh"
	done

	# All daemon output (including Go runtime crashes) goes to its log.
	if ! "$PLEXD" ctl status >/dev/null 2>&1; then
		echo "launch.sh: starting $PLEXD ($(uname -m), kernel $(uname -r))" >"$DAEMON_LOG"
		if command -v setsid >/dev/null 2>&1; then
			setsid "$PLEXD" serve </dev/null >>"$DAEMON_LOG" 2>&1 &
		else
			nohup "$PLEXD" serve </dev/null >>"$DAEMON_LOG" 2>&1 &
		fi
	fi
fi

# Set CPU frequency for Plex music client (ondemand governor)
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 1200000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq

# Run the platform-specific binary
"$BIN/plexmusic.elf" &> /dev/null

# Restore default CPU settings on exit
echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
