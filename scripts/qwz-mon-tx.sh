#!/bin/bash
# qwz-mon-tx.sh <ops>: M1 transmit while a capture with an AP-type vdev runs
# (scripts/qwz-mon-bssid.sh awdl first, iPhone share sheet open so a PSF has
# been heard). ops bits: 1 send one broadcast copy of the last PSF heard,
# 4 send one unicast copy to that PSF's sender, 2 auto mode: answer every
# PSF heard with a unicast copy while the sender is still on our channel.
#   scripts/qwz-mon-tx.sh 1     broadcast once -> completion status
#   scripts/qwz-mon-tx.sh 2     auto unicast replies -> "compl ok" counts ACKs
#   scripts/qwz-mon-tx.sh 0     off
OPS=${1:?usage: $0 <ops>}
sudo sysctl debug.qwz.montx="$OPS"   # on failure the log tail below says why
sleep 2
sudo sysctl debug.qwz.publish=1 >/dev/null
ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | sed 's/^.*"QwzMacLog" = "//' | grep -E "mon: (tx|transmit|self peer|vdev .* up)" | tail -8
