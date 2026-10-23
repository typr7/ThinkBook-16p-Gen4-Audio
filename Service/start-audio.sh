#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
base='/Library/Application Support/ThinkBookAudio'
status=$("$base/audioctl" status 2>/dev/null) || exit 0
case "$status" in
 READY_MANUAL_NO_AMP_ACCESS|READY_MANUAL_REARM_AFTER_WAKE) ;;
 *) exit 0 ;;
esac
console_user=$(/usr/bin/stat -f %Su /dev/console)
case "$console_user" in root|loginwindow|'') exit 0;; esac
console_uid=$(/usr/bin/id -u "$console_user")
[ "$console_uid" -ge 500 ] || exit 0
lock='/var/run/ThinkBookAudio.lock'
/bin/mkdir "$lock" 2>/dev/null || exit 0
clock_pid=''
clock_log=''

cleanup() {
 if [ -n "$clock_pid" ]; then /bin/kill "$clock_pid" 2>/dev/null || true; fi
 if [ -n "$clock_log" ]; then /bin/rm -f "$clock_log"; fi
 /bin/rmdir "$lock" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
umask 077
clock_log=$(/usr/bin/mktemp /var/run/ThinkBookAudio.clock.XXXXXX)
echo 'Starting protected ThinkBook speaker initialization.'
/bin/launchctl asuser "$console_uid" /usr/bin/sudo -u "$console_user" "$base/audio-clock" 30 >"$clock_log" 2>&1 &
clock_pid=$!
ready=0
for attempt in 1 2 3 4 5 6 7 8 9 10; do
 if /usr/bin/grep -qx 'AUDIO_CLOCK_READY' "$clock_log"; then ready=1;break;fi
 /bin/kill -0 "$clock_pid" 2>/dev/null || break
 /bin/sleep 0.2
done
if [ "$ready" -ne 1 ]; then
 echo 'Could not start the built-in output clock.'
 /bin/cat "$clock_log"
 exit 1
fi
if ! "$base/audioctl" arm; then
 "$base/audioctl" stop || true
 exit 1
fi
# Keep an initial clock briefly, then let applications open/close their own stream.
/bin/sleep 2
echo 'ThinkBook speakers initialized; playback monitor is active.'
