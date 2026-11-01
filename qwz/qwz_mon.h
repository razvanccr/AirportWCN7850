/*
 * qwz_mon - monitor-mode capture on the WCN7850, the first AWDL milestone
 * (docs/airdrop-revisited.md, M0): park the radio on one channel with the
 * station down, pass every frame on the air to the host, and hand the frames
 * to userspace through a sysctl, so an iPhone's AWDL action frames can be
 * seen (or not) before any AirDrop code is written.
 *
 *   debug.qwz.mon=<channel>   start: Wi-Fi must be off (interface down)
 *   debug.qwz.mon=0           stop, restore the station's receive filter
 *   debug.qwz.monflags=<m>    experiment knobs (QWZ_MON_F_* below)
 *   debug.qwz.moncap          read-only: drains captured frames
 *                             (scripts/qwz-moncap.py decodes and writes pcap)
 *
 * Model (Linux ath12k, WCN7850 = rxdma1_enable false): a monitor vdev is
 * created, started on the channel and brought up; the RXDMA buffer rings get
 * the "monitor destination" filter with the filter-pass and monitor-other
 * bits for management, control and data frames; the hardware then routes
 * every MPDU through the RXDMA error destination ring as link descriptors,
 * which a timer reaps. The monitor status ring (per-PPDU RSSI and rates) is
 * not used yet: this milestone only has to see frames.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_MON_H
#define QWZ_MON_H

#include <stdint.h>
#include <stddef.h>

struct qwz_softc;

/* Record header in the capture stream (debug.qwz.moncap), little-endian,
 * followed by len bytes of frame and padding to a multiple of 4. */
struct qwz_moncap_hdr {
	uint32_t len;		/* frame bytes that follow */
	uint32_t ts_ms;		/* uptime at delivery, milliseconds */
	uint16_t flags;		/* QWZ_MONCAP_F_* */
	uint8_t decap;		/* DP_RX_DECAP_TYPE_* of the first MSDU */
	uint8_t chan;		/* channel the radio was parked on */
	uint32_t seq;		/* running count of captured frames */
	int8_t rssi;		/* dBm when known (WMI management path), else 0 */
	uint8_t rate;		/* 500 kbit/s units when known, else 0 */
	uint16_t pad;
};
#define QWZ_MONCAP_F_RXDMA_ERR	0x0001	/* entrance ring: error push reason */
#define QWZ_MONCAP_F_TRUNC	0x0002	/* frame longer than the record buffer */
#define QWZ_MONCAP_F_NO_MPDU	0x0004	/* first buffer lacks the MPDU_START tag */
#define QWZ_MONCAP_F_CONT	0x0008	/* MSDU spanned several buffers */
#define QWZ_MONCAP_F_DROPCLASS	0x0010	/* flush, length or overflow error code */
#define QWZ_MONCAP_F_FCS	0x0020	/* raw decap: frame ends with the FCS */
#define QWZ_MONCAP_F_HDR_SYNTH	0x0040	/* native-wifi: header rebuilt from the descriptor */
#define QWZ_MONCAP_F_WMI	0x0080	/* delivered by the firmware's WMI management RX event */
#define QWZ_MONCAP_F_REO	0x0100	/* arrived on the ordinary REO receive path (station data path) */
#define QWZ_MONCAP_SKIP		0xffffffffU	/* ring wrap marker in place of len */

/* debug.qwz.monflags */
#define QWZ_MON_F_OFFSETS	0x0001	/* keep the explicit RX TLV offsets in the monitor filter (default on) */
#define QWZ_MON_F_RXMON		0x0002	/* set EN_RXMON in the filter command, as Linux does (default on) */
#define QWZ_MON_F_FP_ONLY	0x0004	/* filter-pass bits only, no monitor-other (experiment) */
#define QWZ_MON_F_KEEP_FILTER	0x0008	/* leave the receive filter alone (vdev only, experiment) */
#define QWZ_MON_F_VERBOSE	0x0010	/* log every MPDU */
#define QWZ_MON_F_DELETE_STA	0x0020	/* delete the station vdev for the capture (qwz_init recreates it) */
#define QWZ_MON_F_PROMISC	0x0040	/* WMI_PDEV_SET_RX_FILTER_PROMISCUOUS on for the capture */
#define QWZ_MON_F_MNTFILTER	0x0080	/* WMI_MNT_FILTER_CMDID set, all frame types, on the monitor vdev */
#define QWZ_MON_F_REO		0x0100	/* also capture what reaches the REO receive path (default on) */
#define QWZ_MON_F_BSSID_MAC	0x0200	/* create the vdev with debug.qwz.monbssid as its own MAC too */
#define QWZ_MON_F_AP		0x0400	/* vdev type AP (hidden SSID) instead of MONITOR */
#define QWZ_MON_F_NRP		0x0800	/* WMI neighbour-rx filter: accept monbssid as an AP BSSID */
#define QWZ_MON_F_NRP_STA	0x1000	/* ... as a station MAC instead */
#define QWZ_MON_F_NO_SELF_PEER	0x2000	/* AP vdev without its self peer (transmit then fails: DISCARD) */
#define QWZ_MON_F_BEACON	0x4000	/* give the AP vdev a minimal hidden-SSID beacon template before vdev up */
#define QWZ_MON_F_PEER_SENDER	0x8000	/* raw firmware peer for the first PSF sender heard (unicast needs?) */
extern uint32_t qwz_mon_flags;
/* debug.qwz.monbssid: "xx:xx:xx:xx:xx:xx"; when set, vdev_up uses it as the BSSID
 * (the firmware's filter-pass class), and the flags above build on it. Empty: own MAC. */
extern char qwz_mon_bssid_str[32];	/* room for the terminator sysctl(8) sends */

/* qwz_attach(): allocates the capture ring and remembers the softc. */
void	qwz_mon_attach(struct qwz_softc *);
/* qwz_init(): called under the gate before the station comes back. */
void	qwz_mon_stop(struct qwz_softc *);
/* qwz_mgmt_rx_event(): a management frame from the firmware while capturing. */
struct mbuf;
void	qwz_mon_capture_mgmt_mbuf(struct qwz_softc *, struct mbuf *, int rssi, uint32_t rate_kbps);
/* qwz_dp_rx_process_received_packets(): an 802.11 frame off the REO path while capturing. */
void	qwz_mon_capture_reo_mbuf(struct qwz_softc *, struct mbuf *);

/*
 * M1, transmitting: debug.qwz.montx. The last PSF heard (any peer) is kept
 * and replayed as our own frame: sender = the capture vdev's MAC, BSSID
 * kept, destination broadcast or the peer. Bits: 1 send one broadcast copy
 * now, 4 send one unicast copy to the last PSF's sender now, 2 auto mode
 * (answer every PSF heard with a unicast copy to its sender while the peer
 * is still on our channel). Completions are counted by status; a status 0
 * on a unicast means the peer's hardware ACKed our frame.
 */
int	qwz_mon_tx(int ops);		/* errno */
extern uint32_t qwz_mon_tx_mode;
struct wmi_mgmt_tx_compl_event;
/* qwz_wmi_process_mgmt_tx_comp(): 1 when the completion was one of ours. */
int	qwz_mon_tx_compl(struct qwz_softc *, struct wmi_mgmt_tx_compl_event *);

/* From the sysctls (process context; dispatched onto the gate). */
int	qwz_mon_set(int chan);		/* errno; 0 stops */
int	qwz_mon_status(void);		/* channel while active, else 0 */
size_t	qwz_mon_cap_pending(void);	/* bytes a drain would return */
size_t	qwz_mon_cap_drain(void *buf, size_t max);	/* whole records only */

#endif /* QWZ_MON_H */
