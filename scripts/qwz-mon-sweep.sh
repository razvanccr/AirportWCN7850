#!/bin/bash
# qwz-mon-sweep.sh [channel] [seconds] [bssid]: try monitor-mode flag
# combinations one after another and report what each one delivered
# (needs Wi-Fi off). With a BSSID the sweep runs the BSSID-based sets.
# Flags (qwz/qwz_mon.h): 1 offsets, 2 EN_RXMON, 4 FP only, 8 keep filter,
# 0x20 delete station vdev, 0x40 promiscuous, 0x80 WMI monitor filter,
# 0x100 REO tap, 0x200 vdev MAC = bssid, 0x400 AP vdev, 0x800 neighbour-rx
# filter (AP BSSID), 0x1000 neighbour-rx filter (station MAC).
CH=${1:-6}; T=${2:-15}; BSSID=${3:-}
if [ -n "$BSSID" ]; then
  SETS=${SETS:-"0x103 0x303 0x503 0x703 0x903 0x1103 0x1f03"}
else
  SETS=${SETS:-"0x103 0x102 0x183 0x143 0x123 0x1c3 0x1e3"}
fi
counts() { sudo sysctl debug.qwz.publish=1 >/dev/null; ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | grep "mon: stopped" | tail -1; }
sudo sysctl debug.qwz.monbssid="$BSSID" >/dev/null || exit 1
for f in $SETS; do
  sudo sysctl debug.qwz.mon=0 >/dev/null 2>&1
  sudo sysctl debug.qwz.monflags=$f >/dev/null || exit 1
  if ! sudo sysctl debug.qwz.mon=$CH >/dev/null 2>&1; then echo "flags $f: start failed"; continue; fi
  sleep "$T"
  sysctl -n debug.qwz.moncap > /dev/null   # discard what the decoder would read
  sudo sysctl debug.qwz.mon=0 >/dev/null
  sleep 1
  echo "flags $f bssid ${BSSID:-own}: $(counts | sed 's/.*stopped: //')"
done
sudo sysctl debug.qwz.monflags=0x103 debug.qwz.monbssid="" >/dev/null
