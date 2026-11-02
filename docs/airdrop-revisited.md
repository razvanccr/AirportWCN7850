# AirDrop on the WCN7850

State on 2026-11-02. AirDrop works in both directions on this Hackintosh,
from one command:

```
sudo owlymac up --wifi-on --receive ~/Downloads --accept    # from Terminal.app
```

macOS's own AirDrop window sends to the iPhone; owly's receiver saves what
the iPhone sends into Downloads, single or multiple files. While it runs,
Wi-Fi shows on but joins no network: the cell has the radio. Ctrl-C gives
Wi-Fi back.

The earlier note, `awdl-feasibility.md`, said no-go. What changed is the
model: instead of a real `awdl0` with channel hopping, the card sits on one
social channel as a stationary AWDL node ("exclusive" mode in owly's terms),
and the AWDL protocol runs in user space.

## How it works

```
iPhone ──AWDL on channel 6──▶ WCN7850 firmware (AP-type capture vdev,
                               BSSID 00:25:00:ff:94:73, flags 0x503)
   ▲                                   │ debug.qwz.moncap (frames in)
   │                                   ▼ debug.qwz.montxframe (frames out)
   │                            owlymac (root): owly's AWDL node
   │                                   │ feth0/feth1 pair + BPF
   │                                   ▼
   └─ mDNS, TCP ───────────── feth0: owly receive (as the user)
                               sharingd (native send, over the same node)
```

- **The cell.** `qwz/qwz_mon.c`: a second vdev of type AP, started on the
  social channel with the AWDL BSSID. Management frames arrive through
  `WMI_MGMT_RX`, data frames through the RXDMA error destination ring.
  Transmit goes through the firmware's management-transmit command, which
  carries data frames fine once the vdev has a self peer. Sysctls:
  `debug.qwz.mon` (channel, 0 = off), `monflags`, `monbssid`, `moncap`
  (ring out), `montxframe` (frames in), `monpeer`, `monmac`.
- **Hold mode.** `debug.qwz.monhold=1`: turning Wi-Fi on keeps the cell
  instead of starting the station; scans complete empty, joins are refused.
  macOS then shows Wi-Fi powered on, which sharingd needs before it sends
  the AirDrop Bluetooth advert that wakes the iPhone. Native sending from
  the Finder window follows from that alone.
- **The node.** `owly-macos/owly`, branch `macos`: crate
  `crates/owly-radio-macos` (the radio over the sysctls, a feth pair with
  BPF on a blocking thread, the `owlymac` daemon) and the macOS gating of
  owly's Linux modules. `owlymac up` brings the cell up, runs the node,
  optionally holds the cell with Wi-Fi on (`--wifi-on`) and runs
  `owly receive` on the link as the sudo user (`--receive DIR`, `--accept`).
- **Firmware after a cell.** The firmware stops delivering the station's
  replies after any cell. `qwz_mon_stop` marks it (`mon_fw_dirty`) and
  `qwz_init` restarts it with the full bring-up before the station comes
  up; a wedged firmware (WMI timeouts at teardown) restarts on the spot.
  Artist5 joins about nine seconds after Wi-Fi comes on.

## Traps

- The cell's BSSID must be the AWDL one with flags `0x503`; a beaconing
  cell on the card's own address (`0x4503`) hears no AWDL frames.
- macOS Local Network privacy: a binary launched from another app, the
  Claude app and its terminal tabs included, gets no multicast or
  link-local traffic on feth0. Run the receiver from Terminal.app.
- Wi-Fi must be off before `owlymac up`; the kext refuses a cell under a
  running station (`Resource busy`).
- The kext's messages reach neither the unified log nor dmesg: read its
  ring with `scripts/qwz-log.sh`.
- One daemon at a time (`/var/run/owlymac.lock`). Closing its tab tears
  down through SIGHUP; leftovers clear with
  `sudo sysctl debug.qwz.monhold=0 debug.qwz.mon=0 debug.qwz.awdl0=down`.
- An iPhone names each file of a multi-file share
  `./NSIRD_AirDrop_<6 chars>/<FileName>`; owly's receiver accepts and
  flattens that (spec ASK-5a). Its idle connections linger between
  transfers; the per-address bound evicts them rather than refusing.
- `networksetup -getairportnetwork` says "not associated" even when
  joined; check with `ping -b en3 <router>`.

## Native receiving: blocked

Finder's AirDrop receiving needs an `awdl0` the system recognises.
`qwz/qwz_awdl.c` creates one (`ifnet_allocate_extended`, subfamily Wi-Fi,
`IFEF_AWDL`, no Skywalk nexus; sysctls `debug.qwz.awdl0/awdltx/awdlrx`,
daemon `--awdl0`). With it the iPhone lists the Mac, discovery and the
iOS 26 pairing with rapportd complete, but the transfer endpoint sharingd
hands over (`_asquic._udp`, QUIC) is published only on interfaces with a
Skywalk nexus, and attaching the nexus (`--nexus`) garbles the link. A
Skywalk-native `awdl0` is the remaining step; parked.

Two other dead ends: raw HCI through IOBluetooth's private calls returns
success and transmits nothing on macOS 26 (hold mode replaced it), and
marking feth0 as AWDL (`IFEF_AWDL`, `debug.qwz.awdlif`) does not make
mDNSResponder treat it as one (the functional type needs the Wi-Fi
subfamily).

## Timeline

- 2026-11-01: M0, monitor-mode capture; management frames need the
  AP-type vdev.
- 2026-11-02: M1, transmit with hardware ACKs from the iPhone. M2,
  `owly receive` gets a photo. M3, hold mode and native sending. M4,
  the kext's own awdl0 up to the transfer stage. Firmware restart after a
  cell. M5, the hybrid command, multi-file receive.

## Next

- A launchd service so AirDrop is on without a terminal (groundwork in
  owly: `crates/owly-radio-macos/launchd/`, untested). Needs an on/off
  switch, since the cell takes Wi-Fi.
- The Wi-Fi menu keeps its scan spinner while the cell is held; cosmetic.
- Skywalk-native awdl0 for native receiving.
