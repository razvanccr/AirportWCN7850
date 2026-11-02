# AirDrop on the WCN7850, revisited: the stationary-radio model

Written 2026-11-01 against macOS 26.7.2, `AirportWCN7850.kext` 0.0.2 and
`BluetoothWCN7850.kext` 1.0.0 (both loaded on this machine), owly 0.4.2
(released 2026-10-31) and Linux 7.3-rc7 ath12k. Research only; no driver
code was written. Supersedes the verdict in [awdl-feasibility.md](awdl-feasibility.md)
without invalidating its analysis.

## Verdict: feasible as a userspace AirDrop, not (yet) as Finder's AirDrop

The October note was right about one thing and wrong about which problem
mattered. It is right that this host cannot hop the radio between channels
on AWDL's 64 TU grid within 3 ms; nothing in the WMI surface lets it. It was
wrong that AWDL requires hopping. A node that stays on one social channel and
advertises that channel in every slot it serves is a valid AWDL peer: the
phone does the hopping and comes to it. That is owly's "exclusive" mode, and
on 2026-10-31 it transferred files both ways between an iPhone and a WCN7850
(`17cb:1107`, the same device id as this card) on Linux, with patches to the
ath12k driver (not the firmware). The firmware does have what this model
needs: an AP vdev on a fixed channel, monitor receive, and raw frame
transmission. The timing problem the October note stopped at does not arise.

What this buys is `owly send` / `owly receive` on the Hackintosh (iPhone and
Mac peers, Everyone mode, Contacts Only with records from the keychain),
driven by a Rust daemon talking to the kext through a user client. The Wi-Fi
link to the AP goes down while AirDrop runs, which this machine can afford:
`en0` is wired. Finder's AirDrop through IO80211Family's `awdl0` is a
further step with its own unknowns (section 6); the radio work below is a
prerequisite for it either way.

## 1. What changed since 2026-10-17

| | October note | Now |
|---|---|---|
| Open implementations | owl (seemoo-lab, C, monitor + inject, ath9k) and OpenDrop (Python) | owl archived 2026-10-12, OpenDrop effectively unmaintained (no confirmed success on iOS 17+). owly (t4t5, Rust, MIT, 3.2k stars, 111 commits, release 0.4.2 on 2026-10-31) implements AWDL and AirDrop from scratch with an independent spec (`docs/SPEC.md`) and a per-card hardware table |
| Model | AWDL = channel hopping in sync with the peer | owly has three modes; two of them never hop (section 2) |
| WCN7850 | "no AWDL implementation exists for a fullMAC card of this kind" | owly issue 40: shared mode hears nothing from the phone; exclusive mode works with mal-cap's ath12k patches (comment of 2026-10-31, test JSON attached: start 377 ms, phone heard, shown on the phone, phone → computer and computer → phone both pass, channel 6). Patches not yet published; mal-cap's fork `ath12k-monitor` has only upstream commits as of today |
| Intel cards | "same architecture as WCN7850, AirportItlwm never managed it" | iwlwifi AX200/AX201/AX211/BE200 are `tested` in owly's exclusive mode. The fullMAC architecture is not the blocker; monitor injection and an address to ACK for are |

The shared-mode failure on the WCN7850 ("receives nothing from the phone")
matches what the October note found about ath12k: monitor receive is new
(6.16) and monitor transmit is not wired. mal-cap's fix is in the Linux
driver, which is the same layer as our kext.

## 2. owly's three modes, and which one fits

From `crates/owly-radio/src/injection/mod.rs` and `drivers.toml`:

- **Shared.** Station stays associated; the AP must be on 6, 44 or 149; a
  monitor interface with the station's MAC injects AWDL frames; the station
  ACKs the phone's unicast. Needs injection while associated. Fails on every
  Intel card and on the WCN7850.
- **Exclusive.** Station taken down. An ad-hoc cell on the AWDL BSSID, or on
  cards without ad-hoc mode a hidden AP run by hostapd, is created *with the
  station's MAC*, so the hardware ACKs frames the phone sends to that
  address. AWDL runs fixed on one channel (tries 44, 149, 6; Intel goes
  straight to 6). A monitor interface carries all receive and all transmit.
  This is the WCN7850's working mode. The hidden AP is WPA2 with a random
  SSID and passphrase, `ignore_broadcast_ssid=1`, HT, beacons every 1000 TU;
  nothing ever joins it. Its only job is the ACK.
- **Active monitor.** As exclusive, but the monitor itself ACKs. Needs a
  driver feature the WCN7850 does not have.

What the stationary model asks of the radio, in order of risk:

1. **Receive every frame on the channel**, including action frames and data
   frames whose BSSID is `00:25:00:ff:94:73`, with an RSSI and a host
   timestamp on delivery. Microsecond precision is not needed: owly's node
   reads no clock and is fed `Instant::now()` by the driver thread.
2. **Transmit host-built 802.11 frames**: broadcast action frames (PSF, MIF:
   category 127, OUI `00:17:f2`) and unicast QoS data frames to the phone
   with To/From DS = 0, addr3 = AWDL BSSID, LLC/SNAP `00:17:f2`, at a fixed
   rate, with the hardware waiting for ACKs and retrying unicast.
3. **ACK the phone's unicast** to our MAC, at HT rates.
4. **Sit on channel 6, 44 or 149** with the station down.
5. Optionally answer the phone's ADDBA so it aggregates (AWDL-15): several
   times faster uploads.

Items 2 and 3 are what the hidden AP plus monitor give owly on Linux. Item 1
is where the WCN7850 failed in shared mode and where mal-cap's patches
presumably live.

## 3. Mapping onto the WCN7850 as `qwz/` drives it

| Need | In the kext today | Gap |
|---|---|---|
| Station down | `qwz_mac_vdev_stop` / vdev down exist for the STA vdev | Teardown and restore around an AirDrop session; the firmware forgets its country when the station goes (Intel does; check whether the WCN7850's regulatory state survives, or re-send `WMI_SCAN_CHAN_LIST` / `WMI_PDEV_SET_REGDOMAIN`) |
| AP vdev on a fixed channel, our MAC | `qwz_mac_setup_vdev_create_params` and `qwz_mac_vdev_start` have `WMI_VDEV_TYPE_AP` cases; `beacon_tx_offload_max_vdev = 2` is in the resource config; the `NL80211_IFTYPE_AP` arm in `qwz_mac_op_add_interface` is `#if 0` (qwz.c:22822) | Enable the AP arm for a second vdev beside the (stopped) STA vdev; add `WMI_BCN_TMPL_CMDID` with a minimal hidden-SSID beacon (no `bcn_tmpl` code is in qwz today); `WMI_VDEV_UP` with our MAC as BSSID. Whether the firmware ACKs unicast to a vdev that is up without a beacon template is the first thing to measure |
| Monitor receive | The monitor rings are set up (`HAL_RXDMA_MONITOR_BUF/DST/DESC`, `HAL_TX_MONITOR_DST`, qwz.c:13507-13573); `WMI_VDEV_TYPE_MONITOR` is `#if 0` in add_interface; no `dp_mon.c` port (the status-ring parser that produces frames with `mactime` and RSSI) | Port ath12k's monitor vdev start (`ath12k_mac_monitor_vdev_start`) and the `dp_mon` receive path from Linux 6.16+, which is where WCN7850 monitor landed. This is the largest piece and the one owly's shared-mode failure points at. Alternative to try first: a monitor vdev may not be needed if the AP vdev's receive filter can be widened with `HTT_H2T_MSG_TYPE_RX_RING_SELECTION_CFG` to pass frames with foreign BSSIDs (ath12k's `ath12k_mac_config_mon_status_default` does this for the status ring) |
| Raw transmit of action frames | `qwz_mac_mgmt_tx_wmi` wraps `WMI_MGMT_TX_SEND_CMDID` for frames net80211 builds; the firmware sends the bytes it is given, addr3 included | Expose it for host-built frames on the AP vdev; broadcast needs no peer. Confirm the firmware does not rewrite addr3 or refuse category 127 |
| Raw transmit of data frames | The TCL raw-encapsulation path exists (`HAL_TCL_ENCAP_TYPE_RAW` in `qwz_dp_tx`, qwz.c:24001-24273), gated by the device-wide `ATH12K_FLAG_RAW_MODE`, which also switches receive decapsulation to raw | In exclusive mode the station is down, so raw mode for the whole device is acceptable; set the flag for the session. A unicast raw frame needs a peer entry for its destination: create a `WMI_PEER_CREATE` for the phone's MAC on the AP vdev when owly's `Netif::peer_up` fires, delete on `peer_down`. The hardware then retries and reports ACK status as for any peer |
| Rates | Linux ath12k sets rates per peer (`WMI_PEER_SET_PARAM`), not per frame | Fix a peer rate (owly uses 12 Mbit/s OFDM on 5 GHz; the MT7612U notes say 1 Mbit/s CCK on channel 6 fills most of a slot, so prefer 44 or 149 where the iPhone also spends more slots) |
| ADDBA response | `qwz` handles Block Ack only inside net80211's station path | owly answers ADDBA itself (AWDL-15) as a raw action frame; the firmware's aggregation-receive setup for the peer may additionally need `WMI_PEER_SET_PARAM` / HTT `RX_BA_SESSION` to reorder. Later |
| Timestamps | None needed from the chip | Host `mach_absolute_time` on delivery to the daemon, as owly does on Linux |

The WCN7850 vdev budget is 4 (`num_vdevs = 4` in `qwz_wmi_init_wcn7850`):
STA, AP and monitor fit.

## 4. The macOS side: a userspace port of owly

owly's layering was built for this. `owly-awdl` is `no_std` sans-IO: a
`Node` that is fed received frames, IP packets and timer expiries with the
time, and returns actions. Its `driver` feature runs it on tokio against two
traits:

- `Air`: `recv() -> Received { bytes, rssi }` and `send(Frame { bytes, to,
  rate, no_ack })`. On Linux this is a radiotap pcap on the monitor
  interface. On macOS it becomes the kext's user client.
- `Netif`: `recv()` / `deliver()` IPv6 packets plus `peer_up(mac)` /
  `peer_down(mac)`. On Linux it is a TAP named `awdl0` with no DAD and a
  link-local address derived from the station MAC.

`owly-airdrop` (9.5k lines: BLE advert format, mDNS over the link, TLS,
`/Discover` `/Ask` `/Upload`, cpio and gzip) and `owly-cli` are portable
Rust; the only `cfg(target_os = "linux")` in them is a staging-directory
detail. `owly-radio` (7.6k lines: nl80211, rtnl, hostapd, `/proc/sys`,
pcap) is the Linux backend and is replaced, not ported.

What the macOS backend needs:

1. **A raw-frame user client in the kext**: `awdl_start(channel)` (station
   down, AP vdev up with our MAC, monitor or widened filter on, raw mode on),
   `awdl_stop()` (undo, re-associate), `inject(frame, rate, no_ack)`,
   `peer_add(mac)` / `peer_del(mac)`, and a receive path (an async
   notification port plus a shared-memory ring, or `IOConnectCallStructMethod`
   polling with a 1 ms wait; frames arrive at tens per second, so either is
   fine). The existing `ItlNetworkUserClient` (the HeliPort interface) shows
   the pattern.
2. **`awdl0` as a network interface.** macOS has no TAP without a kext.
   Two options that need none: a `feth` pair (`ifconfig feth0 create`,
   `ifconfig feth1 create`, `ifconfig feth0 peer feth1`), with the daemon
   reading and writing Ethernet frames on `feth1` through BPF while the IP
   stack uses `feth0`; or a `utun` carrying IPv6 only, which is what AWDL's
   data path carries anyway, if `owly-airdrop`'s mDNS responder (its `mdns`
   feature, bound to the link with `socket2`) works on a point-to-point
   interface. `feth` is the safer first try because the Linux backend is
   also Ethernet-shaped. A third option is the kext publishing a second
   `IOEthernetInterface`, which is more work and only pays off for section 6.
3. **The BLE wake-up** (`owly-cli/src/bluez.rs`, 220 lines over D-Bus). A
   receiver needs none: owly keeps AWDL and its AirDrop server running, so an
   iPhone sending to the Hackintosh works without Bluetooth on our side. A
   sender must advertise Apple manufacturer data (`0x004C`, TLV `0x05`,
   version 3, 100 ms interval) or Everyone-mode phones never turn their AWDL
   on. CoreBluetooth's `CBPeripheralManager` on macOS accepts only a local
   name and service UUIDs, not manufacturer data. Options: the private
   `IOBluetoothHostController` raw HCI command path (`LE Set Advertising
   Data` / `LE Set Advertising Enable`), which bluetoothd allows for the
   controller it owns; or a second cheap BLE dongle driven from userspace.
   owly already degrades gracefully: "not advertising over Bluetooth:
   sending still works to receivers that are already listening" (a phone
   with its share sheet open has AWDL up).
4. **Contacts Only.** OpenDrop's `airdrop-keychain-extractor` reads the
   Apple-signed validation record and certificate from the macOS keychain of
   a signed-in Apple ID. This Hackintosh is such a Mac, so owly's
   `SenderRecordData` path can be fed real records, which Linux users cannot
   do. Everyone mode works without any of it (ID-6: iOS reverts Everyone to
   Contacts Only after 10 minutes; macOS peers do not).

Build: Rust 1.88+, `cargo build -p owly-cli -p owlyd` once a `macos`
backend crate replaces `owly-radio` in the workspace. The `fake` backend in
`owly-radio` (`fake.rs`, 208 lines) and the `tests/` conformance harness run
without hardware and are the way to bring the daemon up before the kext
path exists.

## 5. Milestones with stop conditions

Each step is measurable on its own and the early ones are cheap.

- **M0, hear the phone (1-2 weeks).** Add a debug user-client call that
  parks the radio on channel 6 with the station down, turns on monitor
  receive (or the widened filter) and streams frames to a userspace dumper.
  Open the share sheet on an iPhone next to it. Pass: action frames with
  BSSID `00:25:00:ff:94:73` and OUI `00:17:f2` arrive, with RSSI. Stop: if
  no combination of monitor vdev, `RX_RING_SELECTION_CFG` filter and raw
  decapsulation delivers them, this is owly issue 40's shared-mode failure
  and the project waits for mal-cap's patches to show what the firmware
  wanted.
- **M1, be heard and ACK (1-2 weeks).** Inject a PSF (owly-awdl can build
  one from a `Config` in a unit test) through `WMI_MGMT_TX_SEND` on an AP
  vdev carrying our MAC; verify on a Linux laptop with a monitor interface
  that it is on the air unmodified. Then bring the AP vdev up and verify the
  phone's unicast to our MAC is ACKed (watch its retry count on the laptop's
  capture). Stop: if the firmware rewrites addr3, refuses category 127, or
  will not ACK without a template beacon it cannot be given.
- **M2, `owly receive` (2-4 weeks).** The macOS backend over the user client
  and a `feth` pair; raw-mode data transmit with a peer per phone. Pass: the
  Hackintosh appears on the iPhone's AirDrop sheet and a photo arrives.
  This is the first useful artefact.
- **M3, sending, speed, identity (open-ended but incremental).** HCI
  advertising for the BLE wake-up; ADDBA so uploads aggregate; channel 44 or
  149 for 5 GHz rates; keychain records for Contacts Only; a menu-bar
  wrapper around the CLI. Mac peers (a real Mac sending to the Hackintosh)
  should work in principle, since the protocol is the same, but owly only
  documents iPhones; test early.

M0 through M2 is of the order of two months of part-time work, with the
decisive answer at M0 after two or three weeks.

## 6. Finder's AirDrop (native `awdl0`) stays a separate question

The October note's section 1 still describes what IO80211Family on Tahoe
expects from a vendor driver: `createVirtualInterface` for role
`APPLE80211_VIF_AWDL`, about 50 `get/setAWDL_*` handlers, an availability-
window event every 64 TU, a Skywalk data path, and the family building and
parsing the TLVs itself from a channel sequence and sync parameters that
wifip2pd sets. The Intel forks that tried (ReAWDL, AirportItlwm-Tahoe) got
an empty AirDrop window and a panic in `_outputStartGated`.

The stationary model could in principle be presented to the family: answer
the channel-sequence setter with one channel in every slot, post the AW tick
from a timer, run owly-awdl's sync and election inside the kext, and feed
the family's `ACTION_FRAME_RX` with what the monitor hears. Whether the
family tolerates a peer that never hops, and whether Apple's PSF template
(`SYNC_FRAME_TEMPLATE`) can be beaconed from a host timer with host
timestamps the phone accepts, is unknown and only testable once M1 exists.
Treat it as M4, and do not start it before M2 works: it needs every piece of
the radio path above plus the family's undocumented contract, and a working
`owly receive` is the only way to debug the radio half independently of the
family half.

The alternative for Finder's AirDrop today is unchanged: a Broadcom card
with Apple firmware, which on Tahoe means OpenCore Legacy Patcher's modern-
wireless root patches for the BCM4360/43602 generation (section 6 of the
October note).

## 7. Sources

- owly: <https://github.com/t4t5/owly> (README hardware table, `docs/SPEC.md`,
  `crates/owly-radio/drivers.toml`, `crates/owly-radio/src/injection/`,
  `crates/owly-awdl/src/driver.rs`, `crates/owly-cli/src/bluez.rs`), issue 40
  (WCN7850) and issue 45 (WCN6855), release 0.4.2 of 2026-10-31.
- mal-cap's WCN7850 test record `owly-test-ath12k_wifi7_pci-1793468053.json`
  (attached to issue 40): owly 0.4.2, CachyOS kernel 7.3.0, `17cb:1107`,
  exclusive mode, channel 6, all steps pass, computer → phone 7375 ms.
- seemoo-lab OpenDrop and owl: <https://github.com/seemoo-lab/opendrop>,
  <https://github.com/seemoo-lab/owl> (archived 2026-10-12). OpenDrop issues
  127 and 129 (no discovery, TLS rejected by current iOS); owl issue 96 (iOS
  26 discovery problems). `airdrop-keychain-extractor` for Contacts Only.
- Linux ath12k at 7.3-rc7: `mac.c` (`supports_monitor` gate, monitor vdev
  lifecycle), `dp_tx.c` (`HAL_TCL_ENCAP_TYPE_RAW` under `ATH12K_FLAG_RAW_MODE`).
- This repository: `qwz/qwz.c` (vdev types at 22810-22860, raw encap at
  24001-24273, monitor rings at 13507-13573, `qwz_mac_mgmt_tx_wmi` at 24413),
  `controller/ItlNetworkUserClient.cpp`, [awdl-feasibility.md](awdl-feasibility.md).

## 8. Addendum 2026-11-01: shairplay, AirDrop-for-Everything, BrowseAllInterfaces

- **juhovh/shairplay** is an AirPlay audio (RAOP, AirPort Express emulation)
  receiver over ordinary IP and mDNS, in C, last commit August 2018. It has
  nothing to do with AirDrop or AWDL. AirPlay discovery also exists on the
  infrastructure network, which is why the Intel `awdl0` stubs in section 6
  got AirPlay working and AirDrop not.
- **BenItBuhner/AirDrop-for-Everything** (`a4e`, Rust core plus Rust CLI,
  egui, SwiftUI and Kotlin shells; 6 stars, 5 commits) is a clean-room port
  of the AirDrop *protocol* layer (mDNS `_airdrop._tcp`, TLS, `/Discover`
  `/Ask` `/Upload`, DVZip cpio, BLE wake advert). It does not implement AWDL:
  on macOS it uses the system `awdl0`, on Linux it needs owl or
  `brcmfmac-awdl` to have created one, and on Windows and Android it has a
  "LAN mode" that only reaches other `a4e` instances and "Macs running
  `BrowseAllInterfaces`". Its BLE path is a stub on macOS. For this machine
  it adds nothing over owly, whose AWDL layer is the part we lack; its
  protocol crate is a second reference implementation at most.
- **`BrowseAllInterfaces`** (`defaults write com.apple.NetworkBrowser
  BrowseAllInterfaces 1`, then restart Finder) is the 2011-era switch that
  made Finder's AirDrop browse every interface, so Macs found each other over
  Ethernet. Tahoe's `/usr/libexec/sharingd` still contains the key, in the
  `com.apple.NetworkBrowser` domain next to `EnableDirectIP`,
  `AlternateService` and `DisableAirDrop`, so a direct-IP AirDrop path still
  exists in macOS 26. The key is not set on this machine. What it could give:
  Finder's own AirDrop window talking to another Mac on the wired LAN with
  the same key set, or to an `a4e` LAN-mode receiver, with no AWDL at all.
  What it cannot give: iPhones, which advertise AirDrop only on `awdl0`.
  Whether sharingd on this Hackintosh even opens the AirDrop window with the
  key set, given its `AirDrop not ready: wifi=...` gate, is a five-minute
  test: set the key, `killall Finder sharingd`, open AirDrop in Finder next
  to a Mac on the LAN with the same key, then `defaults delete
  com.apple.NetworkBrowser BrowseAllInterfaces` to undo.
- **parsakhaz/open-airdrop-windows** (October 2026, 5 commits in one day, 1
  star) contains no code: a README and a timeline page. Its AirDrop part is
  "research, currently parked": static analysis of the Windows MT7925 driver's
  transmit path, "no frame sent yet", with monitor receive while associated
  and raw transmit both marked not achieved. Its proposed fallback is a USB
  dongle passed into WSL2 running owl. It confirms the same diagnosis as
  this note (AWDL needs monitor receive plus raw injection from the driver)
  and offers nothing to reuse.
- **pfparsons/owl_openawdl** is a fork of seemoo-lab/owl with one commit of
  its own (December 2024, "Started updating and reorganizing thirdparty
  dependencies"), the upstream README unchanged, zero stars. Same AR9280-
  class active-monitor requirement, same 2022 code. Nothing beyond owl.

## 9. M0 implementation (2026-11-01): monitor-mode capture

Built, not yet run on hardware. `qwz/qwz_mon.c` adds a monitor vdev and a
frame capture path to `AirportWCN7850.kext`, following the Linux ath12k
WCN7850 model (no rxdma1: the RXDMA buffer rings get the monitor-destination
filter with filter-pass and monitor-other bits, and MPDUs arrive as link
descriptors on the RXDMA error destination ring, which a 10 ms timer reaps).
The monitor status ring (RSSI, rates) is not used yet.

- `sysctl debug.qwz.mon=<channel>` (Wi-Fi off first): creates the monitor
  vdev with the card's MAC, starts it on the channel (HT20), brings it up,
  switches the receive filter, starts the reap timer. `=0` undoes all of it
  and restores the station filter; switching Wi-Fi on does the same.
- `sysctl debug.qwz.monflags`: experiment bits (`QWZ_MON_F_*` in
  `qwz/qwz_mon.h`): explicit RX TLV offsets on or off, `EN_RXMON`, filter-pass
  only, keep the station filter, verbose.
- `sysctl debug.qwz.moncap`: read-only stream of `qwz_moncap_hdr` records;
  `scripts/qwz-moncap.py` drains it, prints AWDL frames (vendor action
  frames with OUI `00:17:f2`, or BSSID `00:25:00:ff:94:73`) and writes a
  radiotap pcap. `scripts/qwz-mon.sh` wraps the start, stop and log tail.
- Driver changes outside the new file: the HTT ring-selection command now
  carries the monitor fields (`info0` drop-threshold and `EN_RXMON` bits,
  `info1` conf lengths, `info2`, `info3`), zero for the station filter as
  before; `qwz_attach` registers the capture ring; `qwz_init` stops a
  running capture; the compat layer exposes the command gate so the sysctl
  can run the WMI sequence on it.

Test plan: Wi-Fi off in the menu bar; `scripts/qwz-mon.sh 6`; in another
terminal `scripts/qwz-moncap.py -v` (beacons on channel 6 prove the path
delivers frames at all); open the share sheet on an iPhone next to the
machine and watch for `AWDL PSF`/`MIF` lines; `scripts/qwz-mon.sh off`; Wi-Fi
on. Repeat on 44 and 149. Stop condition unchanged: no AWDL frames with any
`monflags` combination means the firmware filters them before the host, as
owly saw in shared mode on Linux, and the next step is mal-cap's ath12k
patches.

### M0 result (2026-11-01, evening): the WCN7850 hears an iPhone's AWDL

Three boots, three findings.

1. **The receive path works.** With Wi-Fi off, `debug.qwz.mon=6` created the
   monitor vdev, brought it up on 2437 MHz and switched the RXDMA filter;
   frames from four neighbouring networks on channel 6 (null frames, QoS
   data, multicast) arrived through the RXDMA error destination ring with
   correct addresses and lengths, 748 MPDUs in five minutes, no ring errors,
   and the stop path tore everything down and gave Wi-Fi back.
2. **AWDL data frames arrive.** With an iPhone's share sheet open next to the
   machine, 20 frames with BSSID `00:25:00:ff:94:73` from the phone's AWDL
   address `42:94:65:8a:04:3e` came in on channel 6: 802.11 data, To/From DS
   0, LLC/SNAP with OUI `00:17:f2`, the AWDL data header (`03 04` + sequence),
   then IPv6 from `fe80::4094:65ff:fe8a:43e` (the EUI-64 of that MAC, as
   NET-1 says) to `ff02::fb` and `ff02::16`: mDNS queries for
   `_applicationServicePairing._tcp.local`, 129 to 183 bytes, FCS included.
   This is the traffic owly's shared mode could not hear on Linux. The
   capture stopped when the share sheet closed, as expected.
3. **No management frames at all.** Not one beacon or probe in 1100 frames on
   a channel with several access points, and the WMI management-receive
   counter stayed at zero while a tap on that handler was in place. The
   firmware delivers foreign management frames neither through the RXDMA
   ring nor through `WMI_MGMT_RX_EVENTID` with this filter. AWDL's PSF and
   MIF are action frames, so this is the one gap left in M0: without them we
   cannot follow the phone's sync or run owly's election.

Next: a build with runtime-switchable experiments (`debug.qwz.monflags`:
drop the explicit TLV offsets, send `WMI_MNT_FILTER_CMDID` with
configure type "all" on the monitor vdev, `WMI_PDEV_SET_RX_FILTER_PROMISCUOUS`
on the pdev, delete the station vdev for the capture, and a tap plus
counters on the REO path and the two error rings), cycled by
`scripts/qwz-mon-sweep.sh` with beacons as the indicator. The two WMI
commands come from Qualcomm's fw-api `wmi_unified.h` (Linux ath12k never
sends them; the downstream driver uses the monitor filter for its monitor
mode). The `no-mpdu-start` flag on every frame means the MPDU-start tag is
not where `hal_rx_desc_wcn7850` puts it, although the payload offset is
right; informational for now.

### Management frames need an AP-type vdev (2026-11-02, after midnight)

The sweep settled it. Seven filter and firmware-command combinations on a
monitor-type vdev delivered data frames and never a management or control
frame, with the REO path and both error rings tapped and silent; the
monitor-filter and promiscuous WMI commands were accepted (return 0) and
changed nothing, and the firmware does not advertise the promiscuous
service bit. All 96 receive-filter bit values in `qwzreg.h` match Linux.

A second sweep with `debug.qwz.monbssid` set to a neighbouring access point
(`62:49:6b:30:10:32`) found the switch: a vdev of type **AP** brought up on
that BSSID receives management frames through `WMI_MGMT_RX_EVENTID` at that
network's beacon rate (187, 114 and 144 frames in 15 s for the three AP
sets), a vdev of type MONITOR with the same BSSID, with or without the
neighbour-receive-packets filter, receives none. The firmware forwards
management frames to the host for a vdev it considers an access point, and
the BSSID the vdev is up on is what it matches. Data frames keep arriving on
the RXDMA error destination ring in both cases.

This is the owly observation in firmware terms: "an ad-hoc cell on AWDL's
BSSID gives the firmware a BSSID filter that passes AWDL". Here the cell is
an AP vdev whose BSSID is `00:25:00:ff:94:73`. Next capture:
`scripts/qwz-mon-bssid.sh awdl` with the iPhone's share sheet open, looking
for PSF and MIF action frames with RSSI.

### M0 complete (2026-11-02): PSF and MIF received

With an AP-type vdev up on channel 6 (`scripts/qwz-mon-bssid.sh 62:49:6b:30:10:32`,
flags 0x503; the BSSID turned out not to matter) and the iPhone's share sheet
open, an 8 s drain held 164 frames: 123 beacons from three other access
points through `WMI_MGMT_RX_EVENTID` at -70 to -75 dBm (the firmware
forwards every beacon on the channel to an AP vdev, not only its own
BSSID's), 15 data frames, and **26 AWDL action frames from the phone
through the RXDMA error destination ring**: 24 PSF and 2 MIF, 348 bytes,
category 127, OUI `00:17:f2`, type 8, version 0x10, header timestamps
`phy_tx`/`target_tx` 121 to 4032 us apart, TLVs sync-params(73),
election-params(21), channel-sequence(41), 0x13(5), service-params(9),
election-params-v2(40), enclosed-bss-name(47), ht-caps(9), vht-caps(14),
version(2), 0x21(10). That is everything owly's `Node` consumes to follow a
peer's schedule and run the election. The same frames did not arrive with
a monitor-type vdev: the vdev type, not the filter, decides what this
firmware lets through.

M0's stop condition is therefore passed. M1 (be heard and ACK: inject a PSF
through `WMI_MGMT_TX_SEND` on an AP vdev carrying our MAC, verify it on the
air, then see the phone's unicast ACKed) is next, with the AP vdev as the
cell owly's exclusive mode needs. Open items carried forward: RSSI for
ring-delivered frames (needs the monitor status ring or the per-MSDU
descriptor field), the `no-mpdu-start` tag mismatch, and whether an AP vdev
must beacon to stay up (it did not need a template to come up here).

### M1 complete (2026-11-02): the iPhone ACKs frames from this card

Transmit path: the driver's own management-transmit route (`qwz_mac_mgmt_tx_wmi`,
`WMI_MGMT_TX_SEND_CMDID`) on the capture vdev, with eight DMA-mapped slots of
its own and a completion hook (`qwz_mon_tx_compl`) that claims completions
whose slot is empty on the station vdev. The frame is the last PSF heard,
replayed with our vdev MAC as sender, the AWDL BSSID kept and a fresh
sequence number (`debug.qwz.montx`, `scripts/qwz-mon-tx.sh`).

1. **First attempt: DISCARD.** A broadcast copy on the AP vdev was accepted
   by the firmware but completed with status 1, which the firmware API
   (`WMI_MGMT_TX_COMP_TYPE_*`: 0 ok, 1 discard, 2 inspect, 3 complete, no
   ACK) names DISCARD. Cause: the AP vdev had no peer entry of its own.
   Linux creates an AP vdev's self peer right after the vdev; management
   frames go out through it.
2. **With the self peer** (`qwz_peer_create` on the vdev's own MAC, through
   the port's peer list so map/unmap events find it; deleted before the
   vdev): broadcast copies complete with status 0, both on a vdev up on the
   AWDL BSSID and on one up on our own MAC with a 57-byte hidden-SSID beacon
   template (`WMI_BCN_TMPL`, accepted).
3. **Unicast ACK probe.** Auto mode answers every PSF heard with a unicast
   copy to its sender while the phone is on our channel. With the share
   sheet open: 253 PSFs heard, 253 probes sent, **37 completed ok with ACK
   RSSI -44 to -45 dBm**, 216 no ACK, 0 discarded. The phone's hardware
   acknowledged frames from this card: addr1/addr2 intact, RF path fine. The
   15 % ACK rate is timing (we transmit one frame per PSF, at whatever rate
   the firmware picks for management, typically 1 Mbit/s on 2.4 GHz, with
   our timer and WMI latency in front of it) and belongs to M2, where owly's
   node schedules unicast inside the peer's availability window.

Open question from the last run: the own-MAC beaconing configuration had
not been seen receiving AWDL frames for several minutes while it transmitted
fine; it is unclear whether the phone was sharing then. Check before relying
on the beacon flag.

M1's stop condition is passed. M2: the owly exclusive-mode port (a user
client for inject/capture, `owly-awdl` + `owly-airdrop` on a `feth` pair),
with the AP vdev plus self peer as the cell.

## M2 complete: `owly receive` on this Hackintosh (2026-11-02)

An iPhone ("RazvanC iPhone", iOS share sheet, AirDrop set to Everyone) found
this machine as "Mac-Pro", and a 193 KB JPEG arrived intact through owly's
receiver, over AWDL on the WCN7850, with the station interface down. The
whole path from M0 to a file on disk:

```
iPhone  ──AWDL on channel 6──▶  WCN7850 firmware (AP-type vdev, BSSID 00:25:00:ff:94:73,
                                 self peer, flags 0x503)
        ──RXDMA error ring / WMI──▶  qwz_mon (kext)  ──debug.qwz.moncap──▶  owlymac (root)
        ◀──WMI_MGMT_TX_SEND────────  qwz_mon          ◀──debug.qwz.montxframe──
                                      owlymac: owly_awdl::Node (exclusive mode, Presence::Fixed)
                                      ──BPF write feth1──▶ feth0 (MAC = card MAC, EUI-64 link-local, MTU 1450)
                                                            ──IPv6──▶ owly receive -i feth0 (mDNS 5353, HTTPS 8772)
```

### What was built

- Kext (`qwz_mon.c`, uncommitted on `awdl-m0-m1`): microsecond capture
  timestamps (`ts_lo`/`ts_hi`), `debug.qwz.montxframe` (an opaque write: a
  4-byte `qwz_montx_hdr` then the frame without FCS, transmitted through
  `WMI_MGMT_TX_SEND_CMDID` on the capture vdev; ENOSPC when the eight-slot
  queue is full), `debug.qwz.monpeer` (a firmware peer, unused so far),
  `debug.qwz.monmac` (the capture vdev's MAC: the AWDL address).
- owly port (`/Users/catalinr/Documents/Github/owly-macos/owly`, on 0.4.2):
  the Linux-only modules of `owly-radio` gated behind `cfg(target_os =
  "linux")`, and a new crate `owly-radio-macos`:
  - `kext.rs`: `Radio::up/down` (writes `monbssid`, `monflags`, `mon`; reads
    `monmac`), `KextAir` (a thread drains `moncap` every 2 ms into a channel,
    strips the FCS when flagged, tallies frames by kind for the log; `send`
    writes `montxframe`, a full queue is `Err(frame)` so the node retries).
  - `feth.rs`: `Pair::create` makes `feth0`/`feth1`, sets `feth0`'s MAC to
    the AWDL address and its link-local to the EUI-64 one (the kernel's
    other link-locals removed), MTU 1450; `Wire` reads `feth1` through BPF
    from a blocking thread (one-second read timeout; macOS's BPF and kqueue
    don't mix, tokio's `AsyncFd` on it fails with EINVAL) and writes to it;
    `peer_up`/`peer_down` are `ndp -s`/`ndp -d`, as rtnetlink neighbours on
    Linux.
  - `bin/owlymac.rs`: `sudo owlymac up [--channel 6] [--bssid awdl|own|MAC]
    [--flags 0x503] [--rate 12] [--name]`; a lock in `/var/run/owlymac.lock`
    (two daemons once drained the ring in turns and tore each other down);
    Ctrl-C stops the node, destroys the pair and stops the cell.

### What the run showed

- The firmware transmits data-type frames through the management-transmit
  command: every `montxframe` completed with status 0, 17 data frames among
  the first 2 500. No raw TCL path needed.
- The own-MAC beaconing cell (`monbssid` empty, flags 0x4503, the M1
  configuration left as an open question) does **not** receive AWDL frames:
  11 minutes on channel 6 with the share sheet open gave 2 232 beacons, 137
  probe requests and zero action frames. With the AWDL BSSID and flags 0x503
  the iPhone was a peer 10 s after start. The AP vdev's BSSID is what the
  firmware's filter-pass class keys on; beaconing is irrelevant.
- Channel 44 (20 MHz) heard only sub-24-byte frames, no beacons, no AWDL;
  the iPhone did its AWDL on channel 6 here. Not investigated further.
- Election, sync and IPv6 work: `ping6` to the iPhone's link-local address
  over `feth0` answers (RTT 280-820 ms, which is AWDL's availability windows
  plus our 2 ms poll and the WMI round trip; tuning is for later).
- During the 193 KB transfer the eight-slot transmit queue overflowed: `radio
  full 769, gave up 15` in owly's stats. The file still arrived (TCP
  retransmits), but the queue should grow to 32 or 64 and the node's rate
  (`--rate 12`, legacy 12 Mbit/s for data) is low.
- `rssi` is `None` for the peer: the ring path carries no RSSI yet (only the
  WMI management path does). The monitor status ring would fix that.

### The macOS trap: Local Network privacy

For an hour the receiver answered no mDNS query although the queries reached
`feth0` (a Python listener saw them). Every Rust binary started from the
Claude session, including its terminal tabs, received no multicast at all,
while Apple's own `python3` and `ping6` did: macOS 15+ applies the **Local
Network** privacy permission (System Settings > Privacy & Security > Local
Network) to third-party processes by their responsible app, and silently
drops local multicast and link-local traffic when it is not granted; platform
binaries are exempt. Started from Terminal.app, the same `owly receive`
answered at once. `owlymac` runs as root and talks to the kext and BPF, so
it is unaffected. Grant the permission to the app that launches `owly`, or
launch it from Terminal.app.

### How to run

```
# Wi-Fi off first.
sudo RUST_LOG=info owlymac up --channel 6
# in Terminal.app (Local Network permission):
owly receive -i feth0 --name Mac-Pro --accept --dir ~/Downloads
```

M2's stop condition is passed. Next: commit, then M3 (sending: `owly send`
needs a BLE advertisement to wake Apple receivers, owly's `btleplug` path
should work with the BluetoothWCN7850 kext), queue depth and rate control,
RSSI from the status ring, the receiver on the daemon's own ports so one
command does it all.

## M3, sending: blocked on the Bluetooth wake-up (2026-11-02)

Goal: `owly send` from this Mac to the iPhone. Everything up to the
iPhone's AirDrop port works; the port never opens, because the iPhone wakes
its receiver only on a sender's BLE advertisement (SPEC BLE-1), and this
Mac cannot put one on the air.

### What works

- `owly send -i feth0 --mac <iPhone AWDL MAC> file` (DISC-5: knock at the
  peer's link-local address, port 8770, no mDNS needed) reaches the iPhone:
  the knock went from "No route to host" to **"Connection refused"** once
  the neighbour entry held, i.e. our TCP SYN crossed AWDL and the iPhone
  answered with RST. The whole network path is proven in both directions.
- Neighbour entries: macOS's `ndp -d` leaves a permanent entry
  "(incomplete)", which fails every connect with EHOSTUNREACH; `peer_up`
  now deletes before setting and `peer_down` keeps the entry (an idle
  phone flaps as a peer every few seconds).
- The kext's management-transmit queue is 64 slots now (build b24c906f,
  not yet installed; 8 overflowed during the M2 transfer).
- The card's MAC changed between runs (ac:f2:3c:8d:a6:9d, then
  96:7f:00:1b:76:16: macOS rotates the station's address when Wi-Fi is
  toggled, and the capture vdev takes the station's); harmless, the AWDL
  address follows.

### What does not: raw HCI on macOS 26

`owly-radio-macos::hci` sends the AirDrop advert through IOBluetooth's
private `BluetoothHCIRequestCreate` / `BluetoothHCISendRawCommand` /
`BluetoothHCIRequestDelete` (the InternalBlue path; prototypes in its
`IOBluetoothHostController.h`). Every call returns 0, as user and as root,
in three variants (legacy advertising from the public address, legacy from a
random static address re-sent every 100 ms, and an extended-advertising set
of our own on handle 7), and **nothing appears on the air**: nRF Connect on
the iPhone, next to the Mac, listed the MacBook, AirPods and neighbours but
never a `4C 00 05 12` advert, and the iPhone's port stayed closed through
four 90-150 s attempts. A nonsense opcode (0x3fff) and wrong parameter
lengths also return 0: bluetoothd (which owns the controller through
IOUSBLib on this BlueToolFixup setup) accepts the raw request and drops it.
`log show` for bluetoothd records only its own scanning. macOS's own
sharing daemon would send the genuine advert, but only with Wi-Fi powered
on, which the capture cell forbids (Finder's AirDrop window says "Wi-Fi
Off"). CoreBluetooth's peripheral API refuses manufacturer data. The
iPhone's AirDrop settings page and Control Center do not open the port
either.

Apparent earlier wake (peer appearing 28 s into the first advert) was the
phone being unlocked, not the advert: no later advert, in any variant,
produced one.

### Routes left for the wake-up

1. **A BLE controller bluetoothd does not own.** An nRF52840 USB dongle
   running Zephyr's `hci_uart` sample over USB CDC-ACM shows up as a serial
   port (`/dev/cu.usbmodem*`), which bluetoothd ignores; owly speaks H4 to
   it (three commands: LE Set Advertising Parameters, Data, Enable). About
   10 EUR, no root, no kext, and the same code would serve Linux hosts
   without BlueZ. A dongle with the standard Bluetooth USB class would be
   seized by bluetoothd instead.
2. **Let macOS believe Wi-Fi is on.** A kext mode where `ifconfig up` with
   the cell active keeps the station idle instead of starting it: Wi-Fi
   shows powered on, sharingd sends the real AirDrop advert when the share
   sheet or Finder's AirDrop window is open, the iPhone wakes, `owly send`
   knocks. More reboot cycles, and macOS's scan/join attempts must be
   stubbed; this is also a step towards M4.
3. The iPhone's own share sheet: with it open the phone is on AWDL and
   `owly receive` works (M2), but its receiving port still needs the BLE
   wake, so this does not help sending to it.

### State of the code

- owly-radio-macos: `hci.rs` (legacy, refreshed and extended advertising,
  raw command), `bin/owly-ble.rs` (test tool, `--raw opcode:hex`), feth
  neighbour fixes; owly-cli: `machci.rs` as the macOS advertiser (kept:
  harmless, and right if a future macOS lets the commands through), the
  knock prints each distinct error.
- `ble-modes.sh` next to the owly checkout cycles the three variants for a
  phone-side scanner.

## M3 complete, and native AirDrop sending with it (2026-11-02)

With the kext's **hold mode** (`debug.qwz.monhold=1`: macOS turning Wi-Fi on
keeps the capture cell and marks the interface up without starting the
station; scans complete empty, joins are refused) and `owlymac up --channel 6
--wifi-on` (which sets the hold and powers Wi-Fi on through `networksetup`),
Finder's AirDrop window on this Hackintosh listed "RazvanC iPhone", and a
photo dragged onto it arrived on the phone. Nothing of owly's AirDrop code
was involved in that transfer: macOS's own sharing daemon did it.

What happened, in order:

1. Wi-Fi reports powered on, so sharingd treats AirDrop as available and,
   with the AirDrop window open, sends its genuine Bluetooth advertisement
   (the thing no userspace path could do: raw HCI is dropped on macOS 26).
2. The iPhone wakes its receiver, joins AWDL (the daemon shows it as a peer
   within seconds) and announces `_airdrop._tcp` over AWDL.
3. owly's node delivers the phone's mDNS onto `feth0`; mDNSResponder listens
   on every interface, so sharingd's Bonjour browse sees the phone, with no
   `awdl0` anywhere.
4. sharingd connects to the phone's link-local address on `feth0` (the
   neighbour entry the daemon installed) and runs the AirDrop HTTPS exchange;
   the node carries the TCP stream over AWDL. The phone showed the accept
   prompt and the file.

The phone appears twice in the window after a while: it rotates its AirDrop
service identity every few minutes and the old Bonjour record lingers until
it expires; either icon works.

Transfer stats: with the 64-slot transmit queue, `radio full 0` throughout
(the 8-slot queue overflowed 769 times in M2's transfer).

Kext changes (build d8b8cbca, installed): `qwz_mon_hold` and
`qwz_mon_holding()`, checked in `ItlQwz::enableGated` / `disableGated`
(flags only, no `qwz_init` / `qwz_stop` while holding) and in the WCL scan
and join entry points; `qwz_mon.h` got C linkage for C++ includers;
`qwz_txmgmt_queue` is 64 deep. Daemon: `--wifi-on`, and the neighbour-entry
fixes from the M3 attempts.

Open: native receiving (the phone's share sheet listing this Mac and
sharingd accepting), which would make `owly receive` unnecessary too; AWDL
peers other than one iPhone; what macOS does with the "on but no networks"
Wi-Fi over time (wifid rescans every few seconds and gets nothing); leaving
the hold needs Wi-Fi off, `monhold=0`, `mon=0`, which `owlymac`'s Ctrl-C
does in that order.

### Native receiving: not yet (2026-11-02, 03:15)

The iPhone's share sheet finds nobody. Bonjour publishes this Mac's own
`_airdrop._tcp` record (sharingd's, instance befbb5263e0a, port 8770 on
every interface) on loopback only: sharingd registers it with
`kDNSServiceFlagsIncludeAWDL`, and mDNSResponder maps that onto an AWDL
interface through its D2D plugin ("call AWDL and NAN plugins since
kDNSServiceFlagsIncludeAWDL is set"), of which there is none.

Tried:

1. `owly announce -i feth0 --port 8770`: a record of ours pointing at
   sharingd's port. The iPhone resolved it (it asked for the host's A
   record over feth0, as it did in the working M2 case) and showed nothing.
   Whether it connected to 8770 and sharingd refused, or whether it never
   connected, is unverified (no tcpdump run yet; sharingd's log is
   redacted).
2. Kext `debug.qwz.awdlif=feth0` (build 7d40358d, installed; daemon
   `--awdl-flag`): `ifnet_set_eflags(IFEF_AWDL)` works from the kext (the
   symbol resolves in com.apple.kpi.bsd; eflags 0x40010080 -> 0x401f0080,
   xnu adding LOCALNET_PRIVATE, IPV6_ND6ALT and RESTRICTED_RECV as its
   `IFEF_AWDL_MASK` says). After a down/up of feth0, mDNSResponder still
   publishes the record on loopback only. xnu's `if_functional_type()`
   reports WIFI_AWDL only for an interface whose subfamily is Wi-Fi; a
   `feth` is not, so to Network.framework, NECP, mDNSResponder and sharingd
   it is still a wired interface with odd flags. The subfamily is fixed at
   `ifnet_allocate_extended`; no KPI changes it later.

So the AWDL side of native receiving needs an interface the kext allocates
itself: family Ethernet, subfamily Wi-Fi, named `awdl0`, with `IFEF_AWDL`,
its output handed to the daemon and the daemon's frames injected with
`ifnet_input` (a kernel-control socket, utun-style, or a BPF tap on a
companion interface). That is M4 proper: the `feth` pair goes away, the
node's Netif talks to the kext, and sharingd, mDNSResponder and the AirDrop
window all see a real `awdl0`. Native sending already works without it.

Open question to settle first, cheaply, with `sudo tcpdump -ni feth0 'tcp
port 8770'` while the iPhone browses against `owly announce`: does sharingd
accept a connection that does not arrive on an AWDL functional-type
interface? If it does, the `awdl0` ifnet is only needed for the record
publication, and a simpler route (publishing sharingd's record from
userspace, with a copy of its TXT) might do.

## M4: the kext's own awdl0 (2026-11-02, morning)

Native receiving needs the kernel to see a real AWDL interface: the tcpdump
check settled that sharingd's listener resets every connection that arrives
on anything else (SYN every 5 s from the phone to port 8770 on feth0, RST
from the Mac each time), and a feth cannot become one: `IFEF_AWDL` plus a
delegate to the Wi-Fi interface (kext `debug.qwz.awdlif=feth0=en3`, both
calls succeed, eflags 0x401f0080) still leaves mDNSResponder publishing
sharingd's record on loopback and the resets in place. So `qwz_awdl.c`
allocates one: `ifnet_allocate_extended` (struct version 2, legacy output
model, `IFNET_INIT_NX_NOAUTO`), family Ethernet, subfamily Wi-Fi, type
Ethernet, name `awdl0`, the Ethernet demux/proto/framer KPIs, BPF attached,
`IFEF_AWDL` set after attach (xnu adds LOCALNET_PRIVATE, ND6ALT and
RESTRICTED_RECV). Packets: the stack's output goes into a ring drained by
`debug.qwz.awdltx`; frames from the air come in through `debug.qwz.awdlrx`
and `ifnet_input`. The daemon's `--awdl0` runs the node over the two
sysctls (`awdl.rs`), no feth pair, no BPF.

What it took to get stable:

- Two kernel freezes, no panic report. Fixes: no Skywalk nexus
  auto-attached (the first build had one and `CHANNEL_IO`), no flag changes
  inside the ioctl handler, and the real bug: the injected mbuf's buffer
  length was its capacity, not the frame, so the stack read past the frame.
  Chained buffers are handled through `mbuf_copyback`/`mbuf_adj`;
  rejections are counted by cause in `debug.qwz.awdl0`.
- Inbound multicast was dropped unless tcpdump had the interface in
  promiscuous mode (the user noticed discovery only worked with a capture
  running): the driver must set M_MCAST/M_BCAST on inbound frames for
  `ether_demux`. Build fb0fb545 does; until it is installed, a running
  `tcpdump -i awdl0` is the workaround.
- The daemon's `ndp -s` took five seconds a call (name lookups) and froze
  the node, which timed out its other peer: with two Apple devices around
  the peers knocked each other out every five seconds. Now `ndp -n`, off
  the node's thread.

What works with it: `ifconfig -v awdl0` says `type: Wi-Fi` with the AWDL
eflags; mDNSResponder publishes sharingd's `_airdrop._tcp` record on it
(interface 15, and resolves on it: "ifindex: 15" in its log); the iPhone's
share sheet lists "Mac-Pro"; sharingd accepts the phone's connections;
rapportd and the phone open TLS 1.3 sessions both ways over the link (seven
established connections at once). Native sending from the AirDrop window
keeps working.

What does not, yet: iOS 26's "AirDrop Code Required" step. The phone asks
for a six-digit code "from Mac Pro" and the Mac never shows one; its
rapportd pair-verify towards the phone ends with the phone resetting the
connection after the TLS handshake ("PairVerify error", "Missing ContactID
From Endpoints"). Whether that is the link (frames lost or reordered mid
exchange), a Hackintosh identity problem (no trusted device identity for
the pairing), or the code UI needing something else, is open. A contact
path (the Mac's Apple ID in the phone's contacts) would bypass the code.

Later that morning: with the multicast marks (fb0fb545) the phone lists the
Mac every time, and a capture of its pairing connection shows where the code
step dies: the TLS handshake with rapportd on port 60189 completes, then
the phone's first 274-byte record is retransmitted every two seconds and the
Mac's TCP acknowledges only the byte before it, with no checksum error
counted. The segments were lost between `ifnet_input` and TCP; the session
had used the chained-buffer injection path 92 times. bc192697 injects every
frame in a single buffer (`mbuf_allocpacket` with one chunk) and pins the
capture vdev, and so the AWDL address, to the card's permanent MAC (the
address had alternated with macOS's private Wi-Fi address between runs,
leaving phones with stale records).

With bc192697 (single-buffer injection, pinned address) the link is clean:
no rejected frames, no retransmission storms, the phone lists the Mac as
"catalinr" every time, opens two pairing connections into the Mac's
sharing service, goes to "Waiting..." and gets declined, with no prompt on
the Mac and no incoming AirDrop request in sharingd's log at all. The
decline happens inside the pairing stage that iOS 26 runs before the
AirDrop request proper (rapportd; "Missing ContactID From Endpoints", "No
PIN info found in TXT record"). Both devices use the same Apple Account,
so a healthy pair would skip the code and prompt entirely: the phone
treating the Mac as a stranger means the Mac's AirDrop identity does not
verify, which on a Hackintosh is the iMessage/IDS story (sharingd logged
"IDS Request to block until connected"), not the link. Where M4 stands:
the radio, the AWDL node and the kernel interface carry Apple's own
AirDrop end to end; what is left is the Mac's identity with Apple's
services.

The transfer stage (2026-11-02, noon): after the pairing, rapportd hands
the phone the Mac's transfer endpoint, `_asquic._udp` (QUIC over UDP, port
49453 here), which sharingd publishes on the LAN interface and loopback
only; the phone's datagrams then reach awdl0 and find no socket (UDP "no
socket" drops climb), and the phone reports a decline. Network.framework
listeners bind to interfaces with a Skywalk netif nexus, which awdl0 lacks
(NX_NOAUTO). Build 585aecef re-allows the nexus on request ("nexus:" in
the create string, daemon `--nexus`): with it the kernel no longer froze
(so the earlier freezes were the mbuf bug), the netif attached, mDNS flowed
both ways on the capture, but the phone never listed the Mac again, so the
nexus-attached legacy interface mangles something on the way to the air.
A Skywalk-native awdl0 would be the proper answer; the cheap experiment
first is the non-nexus mode with the Ethernet interface down
(`lan-off-test.sh`), so sharingd has no LAN interface to prefer.

Ethernet-down test (12:55-12:57): with en0 down for two minutes and the
phone sending, the phone sat at "Waiting..." again and the UDP "no socket"
counter barely moved, so this time its datagrams did not even land on
awdl0; sharingd's `_asquic._udp` was back on loopback and en0 as soon as
Ethernet returned, and where it was during the window is unknown (the
samples missed it). Inconclusive, and not the lever: sharingd's choice of
transfer interface is made from what Network.framework considers viable,
and a legacy ifnet without a Skywalk nexus is not. Native receiving on
iOS 26 therefore ends at the transfer stage until awdl0 is a
Skywalk-native interface (or the nexus-attached legacy path is made to
carry frames intact). Everything before that stage works natively.

### Wi-Fi after the cell: the firmware hangs (2026-11-02, 13:10)

After the long M4 session the station never came back: Wi-Fi on, every
scan empty, no network listed. The kext log says why: at the cell's stop
every firmware command timed out ("mon: vdev 1 down failed: 35", "stop
failed: 35", "delete failed: 35"), and every `qwz_init` since fails with
35 too. The firmware is wedged (the session had "utx ... fail 126, data
ok 187 bad 27" transmit completions, i.e. something in the heavy transfer
exchange upset it), and the driver's init does not restart the firmware
on a WMI timeout, so only a reboot recovers. Last night's "Artist5 asks
for a password" was the same thing in a milder form. To fix in the kext:
a firmware restart (the full bring-up) when WMI stops answering, both in
`qwz_mon_stop` and in `qwz_init`.

Fix (build da19de82): `qwz_mon_vdev_teardown` reports WMI timeouts, and
`qwz_mon_stop` then runs `qwz_core_deinit` so the next `qwz_init` takes the
full bring-up instead of failing on a wedged firmware; `qwz_init` itself
does the same when its deauth times out. The station should come back
after any cell session without a reboot. The "hold" mode's empty scans
still keep the Wi-Fi menu spinning; cosmetic, open.

### Wi-Fi after the cell, continued: every cell, not only a wedged one (2026-11-02, evening)

da19de82 was not enough: after any cell, wedged or not, the station scanned
fine but no join completed. The join diagnostics (`qwz: join: ...` lines,
build bf274a60) showed the first frame of every join going out and being
confirmed by the firmware, and no reply ever reaching the driver, while
the same exchange works at boot. The firmware left behind by a cell no
longer serves the station, whatever the vdev teardown reports.

Two reboots were wasted on a fix that never ran: dbe680c2 put the
restart under the `fail:` label of `qwz_mon_start`, the path for a cell
that fails to come up, not the stop path. Lesson: read the kext's own log
ring (`scripts/qwz-log.sh`, the messages reach neither the unified log
nor dmesg) before declaring a build tested.

Fix (build 4fb683bb): `qwz_mon_stop` sets `sc->mon_fw_dirty`, and
`qwz_init` restarts the firmware (`qwz_core_deinit`, then the full
boot-time bring-up) when it finds the flag. Not at the stop itself: a
second cell may follow without Wi-Fi in between, and `qwz_mon_start`
needs the firmware running. A wedged firmware (WMI timeouts at teardown)
still restarts on the spot. Verified: cell on channel 6 with awdl0, Ctrl-C,
Wi-Fi on; the log shows "restarting the firmware after the capture cell",
stages 9 to 19, then the full join exchange, and Artist5 is joined about
nine seconds after Wi-Fi comes on (the router answers over en3, a page
fetch bound to en3 succeeds). `networksetup -getairportnetwork` still
says "not associated" even when joined, as it did before any cell: a
separate, cosmetic gap in the SSID report.

## M5: AirDrop both ways from one command (2026-11-02, evening)

Native receiving stays blocked on a Skywalk-native awdl0 (the transfer
service binds only through the nexus, and the nexus garbles the link), so
the deliverable is a hybrid: macOS's own AirDrop sends, owly receives.

```
sudo owlymac up --wifi-on --receive ~/Downloads --accept    # from Terminal.app
```

brings the cell up on channel 6, holds it with Wi-Fi shown on (so
sharingd sends the Bluetooth advert and the Finder AirDrop window lists
the iPhone), and runs `owly receive` on feth0 as the sudo user, announced
as the host name. Verified: a file from the Finder window reaches the
iPhone; photos shared from the iPhone land in Downloads without a
prompt, three in a row. Terminal.app matters: the receiver needs the
Local Network permission, which a shell inside another app lacks.

A multi-file share then sat on "Waiting". With refusals logged (owly
5e97688) the cause was plain: the iPhone names each file of a multi-file
share `./NSIRD_AirDrop_<6 chars>/<FileName>`, a wrapper directory per
file, and owly's ASK-5 rule demanded `./<FileName>`, answering 400. owly
88672dd accepts the wrapped form, writes the wrapper nowhere and lands
each file under its own name (spec ASK-5a). Verified: several photos
from the iPhone land in Downloads in one go. Along the way 5c815b9 made
the per-address connection bound evict a sender's quietest idle
connection instead of silently refusing it (an iPhone keeps earlier
connections open between transfers).

State at the end of the day: AirDrop works in both directions from one
command, single and multiple files; nothing pushed.
