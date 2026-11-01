#!/bin/bash
# qwz-mon.sh <channel>|off|status: monitor-mode capture (qwz/qwz_mon.h).
#   scripts/qwz-mon.sh 6        park the radio on channel 6 and capture
#   scripts/qwz-moncap.py       in another terminal: decode the frames
#   scripts/qwz-mon.sh off      back to the station's receive filter
# Wi-Fi must be off in the menu bar first: the capture needs the interface
# down, and switching Wi-Fi on again stops the capture by itself.
set -u
case "${1:-status}" in
  status)
    ch=$(sysctl -n debug.qwz.mon 2>/dev/null) || { echo "debug.qwz.mon missing: kext without monitor support"; exit 1; }
    [ "$ch" = 0 ] && echo "monitor: off" || echo "monitor: channel $ch"
    echo "flags: $(sysctl -n debug.qwz.monflags)"
    echo "pending capture bytes: $(sysctl -n debug.qwz.moncap 2>/dev/null | wc -c | tr -d ' ')"
    ;;
  off|stop|0)
    sudo sysctl debug.qwz.mon=0
    ;;
  *)
    ch=$1
    if [ "$(sysctl -n debug.qwz.mon 2>/dev/null)" = "" ]; then
      echo "debug.qwz.mon missing: kext without monitor support"; exit 1
    fi
    if ifconfig | grep -q "Wi-Fi\|awdl0" 2>/dev/null; then :; fi
    sudo sysctl debug.qwz.mon="$ch" || { echo "start failed (is Wi-Fi off?)"; }
    ;;
esac
sleep 1
sudo sysctl debug.qwz.publish=1 >/dev/null 2>&1
echo "--- log tail"
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | grep -i "mon" | tail -12
