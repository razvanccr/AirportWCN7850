#!/bin/bash
# qwz-mon-bssid.sh <bssid|awdl> [channel] [flags]: capture with an AP-type
# vdev whose BSSID is the given one (the firmware then forwards that BSSID's
# management frames to the host through WMI_MGMT_RX, which a monitor-type
# vdev never gets). "awdl" is 00:25:00:ff:94:73. Wi-Fi must be off.
#   scripts/qwz-mon-bssid.sh 62:49:6b:30:10:32      a neighbour: its beacons
#   scripts/qwz-mon-bssid.sh awdl                    then open the iPhone share sheet
#   scripts/qwz-mon-bssid.sh own 6 0x4503            AP on our MAC, beaconing (M1 transmit tests)
#   scripts/qwz-moncap.py                            in another terminal
#   scripts/qwz-mon.sh off
B=${1:?usage: $0 <bssid|awdl> [channel] [flags]}; CH=${2:-6}; F=${3:-0x503}
[ "$B" = awdl ] && B=00:25:00:ff:94:73
[ "$B" = own ] && B=""        # the vdev comes up on its own MAC (needed with the beacon flag 0x4000)
sudo sysctl debug.qwz.mon=0 >/dev/null 2>&1
sudo sysctl debug.qwz.monbssid="$B" debug.qwz.monflags="$F" debug.qwz.mon="$CH" || exit 1
sleep 1
sudo sysctl debug.qwz.publish=1 >/dev/null
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | grep "mon:" | tail -5
