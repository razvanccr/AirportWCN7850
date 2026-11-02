/*
 * qwz_mon - monitor-mode capture on the WCN7850 (see qwz_mon.h).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#include "qwz_mon.h"

/* qwz.c keeps these Linux porting macros private; same definitions. */
#ifndef __bf_shf
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#endif
#ifndef FIELD_GET
#define FIELD_GET(_m, _v) ((typeof(_m))(((_v) & (_m)) >> __bf_shf(_m)))
#endif
#ifndef FIELD_PREP
#define FIELD_PREP(_m, _v) (((typeof(_m))(_v) << __bf_shf(_m)) & (_m))
#endif

/* qwz.c (no shared header: OpenBSD keeps its prototypes in the .c) */
int	qwz_wmi_vdev_create(struct qwz_softc *, uint8_t *, struct vdev_create_params *);
int	qwz_wmi_vdev_start(struct qwz_softc *, struct wmi_vdev_start_req_arg *, int, int);
int	qwz_wmi_vdev_up(struct qwz_softc *, uint32_t, uint32_t, uint32_t, const uint8_t *, uint8_t *, uint32_t, uint32_t);
int	qwz_wmi_vdev_down(struct qwz_softc *, uint32_t, uint8_t);
int	qwz_wmi_vdev_stop(struct qwz_softc *, uint8_t, uint8_t);
int	qwz_wmi_vdev_set_param_cmd(struct qwz_softc *, uint32_t, uint8_t, uint32_t, uint32_t);
int	qwz_mac_vdev_setup_sync(struct qwz_softc *);
int	qwz_mac_setup_vdev_create_params(struct qwz_softc *, struct qwz_vif *, struct qwz_pdev *, struct vdev_create_params *);
int	qwz_mac_op_update_vif_offload(struct qwz_softc *, struct qwz_pdev *, struct qwz_vif *);
int	qwz_get_num_chains(uint32_t);
int	qwz_dp_tx_htt_rx_filter_setup(struct qwz_softc *, uint32_t, int, enum hal_ring_type, size_t, struct htt_rx_ring_tlv_filter *);
int	qwz_dp_rxdma_ring_sel_config_wcn7850(struct qwz_softc *);
void	qwz_hal_srng_access_begin(struct qwz_softc *, struct hal_srng *);
void	qwz_hal_srng_access_end(struct qwz_softc *, struct hal_srng *);
uint32_t *qwz_hal_srng_dst_get_next_entry(struct qwz_softc *, struct hal_srng *);
void	qwz_hal_rx_reo_ent_paddr_get(struct qwz_softc *, void *, uint64_t *, uint32_t *);
void	qwz_hal_rx_buf_addr_info_set(void *, uint64_t, uint32_t, uint8_t);
void	qwz_hal_rx_buf_addr_info_get(void *, uint64_t *, uint32_t *, uint8_t *);
int	qwz_dp_rx_link_desc_return(struct qwz_softc *, uint32_t *, enum hal_wbm_rel_bm_act);
struct qwz_rx_msdu *qwz_dp_rx_reap_desc(struct qwz_softc *, uint32_t, void *);
int	qwz_dp_rxbufs_replenish(struct qwz_softc *, struct dp_rxdma_ring *, void *, int);
struct mbuf *qwz_wmi_alloc_mbuf(size_t);
int	qwz_wmi_cmd_send(struct qwz_pdev_wmi *, struct mbuf *, uint32_t);
int	qwz_mac_mgmt_tx_wmi(struct qwz_softc *, struct qwz_vif *, uint8_t, struct ieee80211_node *, struct mbuf *);
int	qwz_peer_create(struct qwz_softc *, struct qwz_vif *, uint8_t, struct ieee80211_node *, struct peer_create_params *);
int	qwz_wmi_send_peer_create_cmd(struct qwz_softc *, uint8_t, struct peer_create_params *);
int	qwz_wmi_send_peer_delete_cmd(struct qwz_softc *, const uint8_t *, uint8_t, uint8_t);
int	qwz_peer_delete(struct qwz_softc *, uint32_t, uint8_t, struct ath12k_peer *);
struct ath12k_peer *qwz_peer_find_by_addr(struct qwz_softc *, const uint8_t *);
IOCommandGate *qwz_compat_gate(void);

/* ---- M1 transmit state ----------------------------------------------- */

static void qwz_mon_sender_peer(struct qwz_softc *, const uint8_t *, int);
static int qwz_mon_parse_mac(const char *, uint8_t *);
static struct qwz_softc *qwz_mon_sc;	/* the one softc; set by qwz_mon_attach() */

uint32_t qwz_mon_tx_mode;		/* bit 2: auto unicast reply */
uint32_t qwz_mon_hold;			/* debug.qwz.monhold (qwz_mon.h) */

/* Private KPI (xnu kpi_interface.h under KERNEL_PRIVATE); C linkage. */
#ifdef __cplusplus
extern "C" {
#endif
extern errno_t ifnet_set_eflags(ifnet_t, u_int32_t new_flags, u_int32_t mask);
extern u_int32_t ifnet_eflags(ifnet_t);
#ifdef __cplusplus
}
#endif
#define QWZ_IFEF_AWDL	0x00100000

int
qwz_mon_awdlif(const char *cmd)
{
	ifnet_t ifp;
	const char *name = cmd;
	int clear = 0, err;
	u_int32_t before, after;

	if (name[0] == '-') {
		clear = 1;
		name++;
	}
	if (name[0] == 0)
		return EINVAL;
	err = ifnet_find_by_name(name, &ifp);
	if (err)
		return err;
	before = ifnet_eflags(ifp);
	err = ifnet_set_eflags(ifp, clear ? 0 : QWZ_IFEF_AWDL, QWZ_IFEF_AWDL);
	after = ifnet_eflags(ifp);
	printf("qwz: mon: awdlif %s: %s IFEF_AWDL -> %d, eflags 0x%08x -> 0x%08x\n",
	    name, clear ? "clear" : "set", err, before, after);
	ifnet_release(ifp);
	return err;
}

int
qwz_mon_holding(struct ieee80211com *ic)
{
	struct qwz_softc *sc = ic ? (struct qwz_softc *)ic->ic_if.if_softc : NULL;

	return sc != NULL && qwz_mon_hold != 0 && sc->mon_active;
}
static uint8_t mon_psf[512];		/* last PSF heard, without FCS */
static size_t mon_psf_len;
static uint8_t mon_psf_src[6];
static uint64_t mon_last_tx_ns;
static uint16_t mon_tx_seq;
static int mon_txmaps_ready;
static const uint8_t qwz_awdl_bssid[6] = { 0x00, 0x25, 0x00, 0xff, 0x94, 0x73 };
static const uint8_t qwz_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Management action, category 127, Apple OUI, AWDL type 8, subtype 0 (PSF). */
static int
qwz_mon_is_psf(const uint8_t *f, size_t len)
{
	return len >= 32 && f[0] == 0xd0 && f[24] == 127 &&
	    f[25] == 0x00 && f[26] == 0x17 && f[27] == 0xf2 && f[28] == 8 && f[30] == 0;
}

/*
 * Replay the kept PSF as ours: sender = the capture vdev's MAC, so the
 * hardware recognises the ACK; BSSID unchanged; new sequence number.
 */
static int
qwz_mon_tx_copy(struct qwz_softc *sc, const uint8_t *dst)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct mbuf *m;
	uint8_t *p;
	int ret;

	if (!sc->mon_active || !vif->is_up || mon_psf_len == 0 || !mon_txmaps_ready)
		return ENXIO;
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	MCLGET(m, M_DONTWAIT);
	if ((M_FLAGS(m) & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}
	M_SETLEN(m, mon_psf_len);
	p = mtod(m, uint8_t *);
	memcpy(p, mon_psf, mon_psf_len);
	p[2] = p[3] = 0;				/* duration */
	memcpy(p + 4, dst, 6);				/* addr1 */
	memcpy(p + 10, vif->vdev_addr, 6);		/* addr2: ours */
	p[22] = (uint8_t)((mon_tx_seq & 0x0f) << 4);	/* sequence control */
	p[23] = (uint8_t)(mon_tx_seq >> 4);
	mon_tx_seq = (mon_tx_seq + 1) & 0x0fff;

	ret = qwz_mac_mgmt_tx_wmi(sc, vif, sc->pdevs[0].pdev_id, NULL, m);
	if (ret) {
		if (ret != ENOBUFS)	/* ENOBUFS: the mbuf was already freed */
			m_freem(m);
		sc->mon_stats.tx_fail++;
		return ret;
	}
	sc->mon_stats.tx_sent++;
	mon_last_tx_ns = getnsecuptime();
	return 0;
}

/* Every captured frame passes here (under the gate) before the ring. */
static void
qwz_mon_rx_hook(struct qwz_softc *sc, const uint8_t *frame, size_t len, uint16_t flags)
{
	if ((flags & QWZ_MONCAP_F_FCS) && len >= 4)
		len -= 4;
	if (!qwz_mon_is_psf(frame, len) || len > sizeof(mon_psf) ||
	    memcmp(frame + 16, qwz_awdl_bssid, 6) != 0)
		return;
	sc->mon_stats.rx_psf++;
	memcpy(mon_psf, frame, len);
	mon_psf_len = len;
	memcpy(mon_psf_src, frame + 10, 6);
	if ((qwz_mon_flags & QWZ_MON_F_PEER_SENDER) && !sc->mon_sender_peer)
		qwz_mon_sender_peer(sc, mon_psf_src, 1);
	if ((qwz_mon_tx_mode & 2) && getnsecuptime() - mon_last_tx_ns > 50000000ULL)
		qwz_mon_tx_copy(sc, mon_psf_src);	/* the peer is on our channel right now */
}

int
qwz_mon_tx_compl(struct qwz_softc *sc, struct wmi_mgmt_tx_compl_event *ev)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	struct qwz_tx_data *td;

	if (ev->desc_id >= nitems(q->data))
		return 0;
	td = &q->data[ev->desc_id];
	if (td->m == NULL || sc->sc_vif.txmgmt.data[ev->desc_id].m != NULL)
		return 0;
	bus_dmamap_unload(sc->sc_dmat, td->map);
	m_freem(td->m);
	td->m = NULL;
	td->ni = NULL;
	if (q->queued > 0)
		q->queued--;
	switch (sc->mon_tx_kind[ev->desc_id]) {
	case 1: if (ev->status == 0) sc->mon_stats.ufr_mgmt_ok++; else sc->mon_stats.ufr_mgmt_bad++; break;
	case 2: if (ev->status == 0) sc->mon_stats.ufr_data_ok++; else sc->mon_stats.ufr_data_bad++; break;
	case 3: if (ev->status == 0) sc->mon_stats.ufr_ctrl_ok++; else sc->mon_stats.ufr_ctrl_bad++; break;
	default: break;
	}
	sc->mon_tx_kind[ev->desc_id] = 0;
	/* WMI_MGMT_TX_COMP_TYPE_*: 0 complete ok, 1 discard, 2 inspect, 3 complete no ack. */
	if (ev->status == 0) {
		sc->mon_stats.tx_ok++;
		sc->mon_stats.last_ack_rssi = (int32_t)ev->ack_rssi;
	} else if (ev->status == 1)
		sc->mon_stats.tx_discard++;
	else if (ev->status == 2)
		sc->mon_stats.tx_inspect++;
	else if (ev->status == 3)
		sc->mon_stats.tx_noack++;
	else
		sc->mon_stats.tx_other++;
	if (qwz_mon_flags & QWZ_MON_F_VERBOSE)
		printf("qwz: mon: tx compl desc %u status %u ack_rssi %d\n", ev->desc_id,
		    ev->status, (int32_t)ev->ack_rssi);
	return 1;
}

/* Transmit `frame` (no FCS) on the capture vdev; the slot remembers the frame type. */
static int
qwz_mon_tx_bytes(struct qwz_softc *sc, const uint8_t *frame, size_t len, uint8_t kind)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct mbuf *m;
	int slot, ret;

	if (!sc->mon_active || !vif->is_up || !mon_txmaps_ready)
		return ENXIO;
	if (len < 10 || len > QWZ_MONTX_MAXFRAME)
		return EINVAL;
	if (vif->txmgmt.queued >= (int)nitems(vif->txmgmt.data) - 1)
		return ENOSPC;
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return ENOBUFS;
	if (len <= MCLBYTES)
		MCLGET(m, M_DONTWAIT);
	else
		MCLGETL(m, M_DONTWAIT, len);
	if ((M_FLAGS(m) & M_EXT) == 0) {
		m_freem(m);
		return ENOBUFS;
	}
	M_SETLEN(m, len);
	memcpy(mtod(m, uint8_t *), frame, len);
	slot = vif->txmgmt.cur;
	ret = qwz_mac_mgmt_tx_wmi(sc, vif, sc->pdevs[0].pdev_id, NULL, m);
	if (ret) {
		if (ret != ENOBUFS)
			m_freem(m);
		return ret;
	}
	sc->mon_tx_kind[slot] = kind;
	return 0;
}

static IOReturn
qwz_mon_txframe_gated(OSObject *, void *arg0, void *arg1, void *arg2, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const uint8_t *buf = (const uint8_t *)arg0;
	size_t len = (size_t)(uintptr_t)arg1;
	int *errp = (int *)arg2;
	const struct qwz_montx_hdr *h = (const struct qwz_montx_hdr *)buf;
	const uint8_t *frame = buf + sizeof(*h);
	size_t flen = len - sizeof(*h);
	uint8_t type, kind;
	int ret;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	type = (frame[0] >> 2) & 3;
	kind = type == 0 ? 1 : type == 2 ? 2 : 3;
	ret = qwz_mon_tx_bytes(sc, frame, flen, kind);
	if (ret == 0)
		sc->mon_stats.ufr_sent++;
	else if (ret == ENOSPC)
		sc->mon_stats.ufr_full++;
	else
		sc->mon_stats.ufr_fail++;
	(void)h;	/* flags and rate are recorded by the caller for now */
	*errp = ret;
	return kIOReturnSuccess;
}

int
qwz_mon_tx_frame(const void *buf, size_t len)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	if (len < sizeof(struct qwz_montx_hdr) + 10 ||
	    len > sizeof(struct qwz_montx_hdr) + QWZ_MONTX_MAXFRAME)
		return EINVAL;
	gate->runAction(qwz_mon_txframe_gated, (void *)buf, (void *)(uintptr_t)len, &err);
	return err;
}

static IOReturn
qwz_mon_peer_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const char *cmd = (const char *)arg0;
	int *errp = (int *)arg1;
	uint8_t mac[6];

	if (sc == NULL || !sc->mon_active) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	if (strncmp(cmd, "add ", 4) == 0 && qwz_mon_parse_mac(cmd + 4, mac)) {
		if (sc->mon_sender_peer && memcmp(sc->mon_sender_addr, mac, 6) != 0)
			qwz_mon_sender_peer(sc, NULL, 0);	/* one peer at a time for now */
		qwz_mon_sender_peer(sc, mac, 1);
		*errp = sc->mon_sender_peer ? 0 : EIO;
	} else if (strncmp(cmd, "del", 3) == 0) {
		qwz_mon_sender_peer(sc, NULL, 0);
		*errp = 0;
	} else
		*errp = EINVAL;
	return kIOReturnSuccess;
}

int
qwz_mon_peer(const char *cmd)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	gate->runAction(qwz_mon_peer_gated, (void *)cmd, &err);
	return err;
}

static void
qwz_mon_txq_purge(struct qwz_softc *sc)
{
	struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
	int i;

	for (i = 0; i < nitems(q->data); i++) {
		if (q->data[i].m) {
			bus_dmamap_unload(sc->sc_dmat, q->data[i].map);
			m_freem(q->data[i].m);
			q->data[i].m = NULL;
		}
		q->data[i].ni = NULL;
	}
	q->cur = q->queued = 0;
}

/*
 * Two firmware-level receive filters that Linux ath12k never sends but the
 * firmware API (wmi_unified.h) documents; experiments for frames the HTT
 * filter alone does not deliver.
 */
struct qwz_wmi_mnt_filter_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t clear_or_set;
	uint32_t configure_type;	/* 0 mgmt, 1 ctrl, 2 data, 3 all */
} __packed;
#define QWZ_WMI_MNT_FILTER_CONFIG_ALL	3

struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd {
	uint32_t tlv_header;
	uint32_t pdev_id;
	uint32_t rx_filter_promiscuous_enable;
} __packed;

static int
qwz_mon_wmi_mnt_filter(struct qwz_softc *sc, uint32_t vdev_id, int set)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_mnt_filter_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_mnt_filter_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_MNT_FILTER_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vdev_id;
	cmd->clear_or_set = set ? 1 : 0;
	cmd->configure_type = QWZ_WMI_MNT_FILTER_CONFIG_ALL;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_MNT_FILTER_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI_MNT_FILTER vdev %u %s: %d\n", vdev_id, set ? "set" : "clear", ret);
	return ret;
}

struct qwz_wmi_vdev_filter_nrp_cmd {
	uint32_t tlv_header;
	uint32_t vdev_id;
	uint32_t addr[2];	/* wmi_mac_addr */
	uint32_t action;	/* 1 add, 2 remove */
	uint32_t type;		/* 1 AP BSSID, 2 station MAC */
	uint32_t flag;		/* 1 capture RX only */
	uint32_t bssid_idx;
} __packed;

static int
qwz_mon_wmi_nrp(struct qwz_softc *sc, uint32_t vdev_id, const uint8_t *mac, int add, int sta)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_vdev_filter_nrp_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_vdev_filter_nrp_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_VDEV_FILTER_NRP_CONFIG_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vdev_id;
	memcpy(cmd->addr, mac, 6);	/* wmi_mac_addr: bytes 0-3 in word 0, 4-5 low in word 1 */
	cmd->action = add ? 1 : 2;
	cmd->type = sta ? 2 : 1;
	cmd->flag = 1;
	cmd->bssid_idx = 0;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_VDEV_FILTER_NEIGHBOR_RX_PACKETS_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI neighbour-rx filter vdev %u %s %s (%s): %d\n", vdev_id,
	    add ? "add" : "remove", ether_sprintf((u_char *)mac), sta ? "sta" : "bssid", ret);
	return ret;
}

static int
qwz_mon_wmi_promisc(struct qwz_softc *sc, int enable)
{
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd *cmd;
	struct mbuf *m = qwz_wmi_alloc_mbuf(sizeof(*cmd));
	int ret;

	if (m == NULL)
		return ENOMEM;
	cmd = (struct qwz_wmi_pdev_set_rx_filter_promiscuous_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_PDEV_SET_RX_FILTER_PROMISCUOUS_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->pdev_id = sc->pdevs[0].pdev_id;
	cmd->rx_filter_promiscuous_enable = enable ? 1 : 0;
	ret = qwz_wmi_cmd_send(wmi, m, WMI_PDEV_SET_RX_FILTER_PROMISCUOUS_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: WMI_PDEV_SET_RX_FILTER_PROMISCUOUS %d: %d (service bit %s)\n", enable, ret,
	    isset(sc->wmi.svc_map, WMI_TLV_SERVICE_RX_PROMISC_ENABLE_SUPPORT) ? "advertised" : "absent");
	return ret;
}

uint32_t qwz_mon_flags = QWZ_MON_F_OFFSETS | QWZ_MON_F_RXMON | QWZ_MON_F_REO;
char qwz_mon_bssid_str[32];

/* "xx:xx:xx:xx:xx:xx" -> 6 bytes; 0 when empty or malformed. */
static int
qwz_mon_parse_mac(const char *s, uint8_t *out)
{
	int i, v;

	if (s[0] == 0)
		return 0;
	for (i = 0; i < 6; i++) {
		char a = s[i * 3], b = s[i * 3 + 1];
		int ha = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
		int hb = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
		if (ha < 0 || hb < 0)
			return 0;
		v = ha * 16 + hb;
		out[i] = (uint8_t)v;
		if (i < 5 && s[i * 3 + 2] != ':')
			return 0;
	}
	return 1;
}


/* ---- capture ring ----------------------------------------------------- */

#define QWZ_MONCAP_SIZE		(512 * 1024)
#define QWZ_MONCAP_MAXFRAME	4096

static uint8_t *moncap_ring;
static size_t moncap_head, moncap_tail;	/* producer, consumer */
static IOLock *moncap_lock;
static uint32_t moncap_seq, moncap_dropped;

static size_t
moncap_free(void)
{
	return (moncap_tail + QWZ_MONCAP_SIZE - moncap_head - 1) % QWZ_MONCAP_SIZE;
}

static void
moncap_put(const uint8_t *frame, size_t len, uint16_t flags, uint8_t decap,
    uint8_t chan, int8_t rssi, uint8_t rate)
{
	struct qwz_moncap_hdr hdr;
	size_t rec = sizeof(hdr) + ((len + 3) & ~(size_t)3), need = rec;

	if (moncap_ring == NULL)
		return;
	IOLockLock(moncap_lock);
	if (moncap_head + rec > QWZ_MONCAP_SIZE)
		need += QWZ_MONCAP_SIZE - moncap_head;	/* the tail of the buffer is skipped */
	if (need > moncap_free()) {
		moncap_dropped++;
		IOLockUnlock(moncap_lock);
		return;
	}
	if (moncap_head + rec > QWZ_MONCAP_SIZE) {
		if (QWZ_MONCAP_SIZE - moncap_head >= sizeof(uint32_t)) {
			uint32_t skip = QWZ_MONCAP_SKIP;
			memcpy(moncap_ring + moncap_head, &skip, sizeof(skip));
		}
		moncap_head = 0;
	}
	uint64_t us = getnsecuptime() / 1000ULL;

	hdr.len = (uint32_t)len;
	hdr.ts_lo = (uint32_t)us;
	hdr.flags = flags;
	hdr.decap = decap;
	hdr.chan = chan;
	hdr.seq = moncap_seq++;
	hdr.rssi = rssi;
	hdr.rate = rate;
	hdr.ts_hi = (uint16_t)(us >> 32);
	memcpy(moncap_ring + moncap_head, &hdr, sizeof(hdr));
	memcpy(moncap_ring + moncap_head + sizeof(hdr), frame, len);
	moncap_head = (moncap_head + rec) % QWZ_MONCAP_SIZE;
	IOLockUnlock(moncap_lock);
}

size_t
qwz_mon_cap_pending(void)
{
	size_t n;

	if (moncap_ring == NULL)
		return 0;
	IOLockLock(moncap_lock);
	n = (moncap_head + QWZ_MONCAP_SIZE - moncap_tail) % QWZ_MONCAP_SIZE;
	IOLockUnlock(moncap_lock);
	return n;
}

size_t
qwz_mon_cap_drain(void *buf, size_t max)
{
	size_t n = 0, rec;
	uint32_t len;

	if (moncap_ring == NULL)
		return 0;
	IOLockLock(moncap_lock);
	while (moncap_tail != moncap_head) {
		if (QWZ_MONCAP_SIZE - moncap_tail < sizeof(struct qwz_moncap_hdr)) {
			moncap_tail = 0;
			continue;
		}
		memcpy(&len, moncap_ring + moncap_tail, sizeof(len));
		if (len == QWZ_MONCAP_SKIP) {
			moncap_tail = 0;
			continue;
		}
		if (len > QWZ_MONCAP_MAXFRAME) {	/* corrupt ring: give up on it */
			moncap_tail = moncap_head;
			break;
		}
		rec = sizeof(struct qwz_moncap_hdr) + ((len + 3) & ~3U);
		if (n + rec > max)
			break;
		memcpy((uint8_t *)buf + n, moncap_ring + moncap_tail, rec);
		n += rec;
		moncap_tail = (moncap_tail + rec) % QWZ_MONCAP_SIZE;
	}
	IOLockUnlock(moncap_lock);
	return n;
}

/* ---- channel ------------------------------------------------------------ */

static uint32_t
qwz_mon_chan_freq(int chan)
{
	if (chan >= 1 && chan <= 13)
		return 2407 + 5 * chan;
	if (chan == 14)
		return 2484;
	if (chan >= 36 && chan <= 177)
		return 5000 + 5 * chan;
	return 0;
}

/* ---- receive filter ------------------------------------------------------ */

/*
 * The monitor filter on the RXDMA buffer rings, as Linux
 * ath12k_dp_tx_htt_rx_monitor_mode_ring_config() sets it for a chip without
 * rxdma1: monitor-destination TLVs, logging of all three frame types, and
 * the filter-pass plus monitor-other packet filters. Unlike Linux we keep
 * the explicit TLV offsets the station path needs on this firmware
 * (qwz_dp_rxdma_ring_sel_config_wcn7850), so popped buffers keep the layout
 * the descriptor getters expect; QWZ_MON_F_OFFSETS turns that off.
 */
static int
qwz_mon_rx_filter(struct qwz_softc *sc, int enable)
{
	struct qwz_dp *dp = &sc->dp;
	struct htt_rx_ring_tlv_filter f;
	int i, ret;

	if (!enable)
		return qwz_dp_rxdma_ring_sel_config_wcn7850(sc);

	memset(&f, 0, sizeof(f));
	f.rx_filter = HTT_RX_MON_FILTER_TLV_FLAGS_MON_DEST_RING;
	f.drop_threshold_valid = 1;
	f.rx_drop_threshold = HTT_RX_RING_TLV_DROP_THRESHOLD_VALUE;
	f.enable_log_mgmt_type = 1;
	f.enable_log_ctrl_type = 1;
	f.enable_log_data_type = 1;
	f.conf_len_mgmt = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.conf_len_ctrl = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.conf_len_data = HTT_RX_RING_DEFAULT_DMA_LENGTH;
	f.enable_rx_tlv_offset = 1;
	f.rx_tlv_offset = HTT_RX_RING_PKT_TLV_OFFSET;
	f.en_rxmon = !!(qwz_mon_flags & QWZ_MON_F_RXMON);
	f.pkt_filter_flags0 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS0;
	f.pkt_filter_flags1 = HTT_RX_MON_FP_MGMT_FILTER_FLAGS1;
	f.pkt_filter_flags2 = HTT_RX_MON_FP_CTRL_FILTER_FLASG2;
	f.pkt_filter_flags3 = HTT_RX_MON_FP_CTRL_FILTER_FLASG3 |
	    HTT_RX_MON_FP_DATA_FILTER_FLASG3;
	if (!(qwz_mon_flags & QWZ_MON_F_FP_ONLY)) {
		f.pkt_filter_flags0 |= HTT_RX_MON_MO_MGMT_FILTER_FLAGS0;
		f.pkt_filter_flags1 |= HTT_RX_MON_MO_MGMT_FILTER_FLAGS1;
		f.pkt_filter_flags2 |= HTT_RX_MON_MO_CTRL_FILTER_FLASG2;
		f.pkt_filter_flags3 |= HTT_RX_MON_MO_CTRL_FILTER_FLASG3 |
		    HTT_RX_MON_MO_DATA_FILTER_FLASG3;
	}
	if (qwz_mon_flags & QWZ_MON_F_OFFSETS) {
		f.offset_valid = 1;
		f.rx_packet_offset = sc->hal.hal_desc_sz;
		f.rx_header_offset = offsetof(struct hal_rx_desc_wcn7850, pkt_hdr_tlv);
		f.rx_mpdu_start_offset = offsetof(struct hal_rx_desc_wcn7850, mpdu_start_tag);
		f.rx_msdu_end_offset = offsetof(struct hal_rx_desc_wcn7850, msdu_end_tag);
	}

	for (i = 0; i < sc->hw_params.num_rxmda_per_pdev; i++) {
		ret = qwz_dp_tx_htt_rx_filter_setup(sc, dp->rx_mac_buf_ring[i].ring_id,
		    i, HAL_RXDMA_BUF, DP_RXDMA_REFILL_RING_SIZE, &f);
		if (ret) {
			printf("qwz: mon: rx_mac_buf_ring%d filter failed: %d\n", i, ret);
			return ret;
		}
	}
	return 0;
}

/* ---- reaping the RXDMA error destination ring ---------------------------- */

#define QWZ_MON_FRAG_LEN(sc)	((DP_RX_BUFFER_SIZE - (sc)->hal.hal_desc_sz) & ~127U)
#define QWZ_MON_MAX_LINKS	8

static uint8_t *mon_frame;	/* assembly buffer, QWZ_MONCAP_MAXFRAME bytes */

/* 802.11 MAC header length from the frame control field. */
static size_t
qwz_mon_hdrlen(const uint8_t *fc)
{
	uint8_t type = fc[0] & 0x0c, subtype = fc[0] & 0xf0;

	if (type == 0x04) {	/* control */
		if (subtype == 0xc0 || subtype == 0xd0)	/* CTS, ACK */
			return 10;
		return 16;
	}
	if (type == 0x08) {	/* data */
		size_t n = (fc[1] & 0x03) == 0x03 ? 30 : 24;
		if (subtype & 0x80)	/* QoS */
			n += 2;
		return n;
	}
	return 24;	/* management, extension */
}

/*
 * One entrance-ring entry: an MPDU as a chain of MSDU link descriptors whose
 * buffers come from the station's refill ring. Returns the number of
 * buffers consumed (they go to used_list for replenishing).
 */
static int
qwz_mon_pop_mpdu(struct qwz_softc *sc, struct hal_reo_entrance_ring *ent,
    void *used_list)
{
	struct qwz_dp *dp = &sc->dp;
	struct dp_link_desc_bank *bank;
	struct hal_rx_msdu_link *link;
	struct hal_wbm_release_ring rel;
	struct qwz_rx_msdu *msdu;
	uint64_t paddr, last_paddr = 0;
	uint32_t cookie, info, msdu_cnt, desc_bank, push, ecode;
	uint16_t flags = 0;
	uint8_t rbm, decap = 0, hdr[32];
	size_t flen = 0, hdrlen = 0;
	int nbuf = 0, first = 1, links = 0, i;
	bus_size_t offset;

	qwz_hal_rx_reo_ent_paddr_get(sc, ent, &paddr, &cookie);
	rbm = FIELD_GET(BUFFER_ADDR_INFO1_RET_BUF_MGR, le32toh(ent->buf_addr_info.info1));
	msdu_cnt = FIELD_GET(RX_MPDU_DESC_INFO0_MSDU_COUNT, le32toh(ent->rx_mpdu_info.info0));
	push = FIELD_GET(HAL_REO_ENTR_RING_INFO1_RXDMA_PUSH_REASON, le32toh(ent->info1));
	ecode = FIELD_GET(HAL_REO_ENTR_RING_INFO1_RXDMA_ERROR_CODE, le32toh(ent->info1));
	if (push == HAL_REO_DEST_RING_PUSH_REASON_ERR_DETECTED) {
		flags |= QWZ_MONCAP_F_RXDMA_ERR;
		if (ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_FLUSH_REQUEST_ERR ||
		    ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_MPDU_LEN_ERR ||
		    ecode == HAL_REO_ENTR_RING_RXDMA_ECODE_OVERFLOW_ERR)
			flags |= QWZ_MONCAP_F_DROPCLASS;
	}
	sc->mon_stats.entries++;
	if (qwz_mon_flags & QWZ_MON_F_VERBOSE)
		printf("qwz: mon: entry paddr 0x%llx cookie 0x%x msdus %u push %u ecode %u\n",
		    (unsigned long long)paddr, cookie, msdu_cnt, push, ecode);

	do {
		if (paddr == last_paddr) {
			sc->mon_stats.duplink++;
			break;
		}
		last_paddr = paddr;
		desc_bank = FIELD_GET(DP_LINK_DESC_BANK_MASK, cookie);
		if (desc_bank >= nitems(dp->link_desc_banks)) {
			sc->mon_stats.nolink++;
			break;
		}
		bank = &dp->link_desc_banks[desc_bank];
		if (bank->mem == NULL || bank->size < sizeof(*link) ||
		    paddr < bank->paddr ||
		    paddr - bank->paddr > bank->size - sizeof(*link)) {
			sc->mon_stats.nolink++;
			break;
		}
		offset = paddr - bank->paddr;
		link = (struct hal_rx_msdu_link *)((uint8_t *)bank->vaddr + offset);
		bus_dmamap_sync(sc->sc_dmat, bank->mem->map, offset, sizeof(*link),
		    BUS_DMASYNC_POSTREAD);

		for (i = 0; i < HAL_NUM_RX_MSDUS_PER_LINK_DESC; i++) {
			struct hal_rx_msdu_details *d = &link->msdu_link[i];
			uint32_t mcookie, mlen, cont;
			size_t l3pad, avail, plen;
			uint8_t *data, *payload;

			if (!FIELD_GET(BUFFER_ADDR_INFO0_ADDR, le32toh(d->buf_addr_info.info0)))
				break;
			mcookie = FIELD_GET(BUFFER_ADDR_INFO1_SW_COOKIE, le32toh(d->buf_addr_info.info1));
			info = le32toh(d->rx_msdu_info.info0);
			mlen = FIELD_GET(RX_MSDU_DESC_INFO0_MSDU_LENGTH, info);
			cont = info & RX_MSDU_DESC_INFO0_MSDU_CONTINUATION;
			if (!cont && msdu_cnt > 0)
				msdu_cnt--;

			msdu = qwz_dp_rx_reap_desc(sc, mcookie, used_list);
			if (msdu == NULL) {
				sc->mon_stats.badcookie++;
				continue;
			}
			nbuf++;
			data = mtod(msdu->m, uint8_t *);
			l3pad = sc->hal_rx_ops->rx_desc_get_l3_pad_bytes(msdu->rx_desc);
			if (first) {
				first = 0;
				if (sc->hal_rx_ops->rx_desc_get_mpdu_start_tag(msdu->rx_desc) !=
				    HAL_RX_MPDU_START)
					flags |= QWZ_MONCAP_F_NO_MPDU;
				decap = sc->hal_rx_ops->rx_desc_get_decap_type(msdu->rx_desc);
				if (mlen == 0)
					mlen = sc->hal_rx_ops->rx_desc_get_msdu_len(msdu->rx_desc);
				if (decap == DP_RX_DECAP_TYPE_NATIVE_WIFI) {
					/* The hardware stripped the 802.11 header; it is in the descriptor. */
					const uint8_t *h = msdu->rx_desc->u.wcn7850.pkt_hdr_tlv.rx_pkt_hdr;
					hdrlen = qwz_mon_hdrlen(h);
					if (hdrlen > sizeof(hdr))
						hdrlen = sizeof(hdr);
					memcpy(hdr, h, hdrlen);
					memcpy(mon_frame, hdr, hdrlen);
					flen = hdrlen;
					flags |= QWZ_MONCAP_F_HDR_SYNTH;
				} else if (decap == DP_RX_DECAP_TYPE_RAW)
					flags |= QWZ_MONCAP_F_FCS;
			}
			if (cont)
				flags |= QWZ_MONCAP_F_CONT;
			payload = data + sc->hal.hal_desc_sz + l3pad;
			avail = DP_RX_BUFFER_SIZE - sc->hal.hal_desc_sz - l3pad;
			plen = cont ? QWZ_MON_FRAG_LEN(sc) : mlen;
			if (plen > avail)
				plen = avail;
			if (flen + plen > QWZ_MONCAP_MAXFRAME) {
				plen = QWZ_MONCAP_MAXFRAME - flen;
				flags |= QWZ_MONCAP_F_TRUNC;
			}
			memcpy(mon_frame + flen, payload, plen);
			flen += plen;
			m_freem(msdu->m);
			msdu->m = NULL;
		}

		/* Return this link descriptor and follow the chain. */
		memset(&rel, 0, sizeof(rel));
		qwz_hal_rx_buf_addr_info_set(&rel.buf_addr_info, paddr, cookie, rbm);
		qwz_hal_rx_buf_addr_info_get(&link->buf_addr_info, &paddr, &cookie, &rbm);
		bus_dmamap_sync(sc->sc_dmat, bank->mem->map, offset, sizeof(*link),
		    BUS_DMASYNC_PREREAD);
		if (qwz_dp_rx_link_desc_return(sc, (uint32_t *)&rel,
		    HAL_WBM_REL_BM_ACT_PUT_IN_IDLE))
			sc->mon_stats.linkret_fail++;
	} while (paddr != 0 && msdu_cnt > 0 && ++links < QWZ_MON_MAX_LINKS);

	sc->mon_stats.mpdus++;
	if (flen > 0) {
		sc->mon_stats.frames++;
		qwz_mon_rx_hook(sc, mon_frame, flen, flags);
		moncap_put(mon_frame, flen, flags, decap, (uint8_t)sc->mon_chan, 0, 0);
	}
	return nbuf;
}

static int
qwz_mon_reap_ring(struct qwz_softc *sc)
{
	struct qwz_dp *dp = &sc->dp;
	TAILQ_HEAD(, ath12k_rx_desc_info) used_list;
	struct hal_srng *srng;
	struct hal_reo_entrance_ring *ent;
	int nbuf = 0, budget = 64;

	TAILQ_INIT(&used_list);
	srng = &sc->hal.srng_list[dp->rxdma_err_dst_ring[0].ring_id];
	qwz_hal_srng_access_begin(sc, srng);
	while (budget-- > 0 && (ent = (struct hal_reo_entrance_ring *)
	    qwz_hal_srng_dst_get_next_entry(sc, srng)) != NULL)
		nbuf += qwz_mon_pop_mpdu(sc, ent, &used_list);
	qwz_hal_srng_access_end(sc, srng);
	if (nbuf) {
		sc->mon_stats.bufs += nbuf;
		qwz_dp_rxbufs_replenish(sc, &dp->rx_refill_buf_ring, &used_list, nbuf);
	}
	return nbuf;
}

static void
qwz_mon_reap(void *arg)
{
	struct qwz_softc *sc = (struct qwz_softc *)arg;
	uint64_t now;
	int n;

	if (!sc->mon_active)
		return;
	n = qwz_mon_reap_ring(sc);
	now = getnsecuptime();
	if (now - sc->mon_stats.last_log > 5000000000ULL) {
		sc->mon_stats.last_log = now;
		printf("qwz: mon: ch %u: entries %u mpdus %u frames %u mgmt %u reo %u "
		    "reo_err %u wbm_err %u bufs %u badcookie %u nolink %u duplink %u "
		    "linkret_fail %u capdrop %u\n",
		    sc->mon_chan, sc->mon_stats.entries, sc->mon_stats.mpdus,
		    sc->mon_stats.frames, sc->mon_stats.mgmt, sc->mon_stats.reo,
		    sc->mon_stats.reo_err, sc->mon_stats.wbm_err, sc->mon_stats.bufs,
		    sc->mon_stats.badcookie, sc->mon_stats.nolink, sc->mon_stats.duplink,
		    sc->mon_stats.linkret_fail, moncap_dropped);
		if (sc->mon_stats.ufr_sent || sc->mon_stats.ufr_full || sc->mon_stats.ufr_fail)
			printf("qwz: mon: utx: sent %u full %u fail %u; mgmt ok %u bad %u, data ok %u bad %u, ctrl ok %u bad %u\n",
			    sc->mon_stats.ufr_sent, sc->mon_stats.ufr_full, sc->mon_stats.ufr_fail,
			    sc->mon_stats.ufr_mgmt_ok, sc->mon_stats.ufr_mgmt_bad,
			    sc->mon_stats.ufr_data_ok, sc->mon_stats.ufr_data_bad,
			    sc->mon_stats.ufr_ctrl_ok, sc->mon_stats.ufr_ctrl_bad);
		if (sc->mon_stats.rx_psf || sc->mon_stats.tx_sent)
			printf("qwz: mon: tx: psf heard %u, sent %u fail %u, compl ok %u discard %u inspect %u noack %u other %u, last ack rssi %d, mode 0x%x\n",
			    sc->mon_stats.rx_psf, sc->mon_stats.tx_sent, sc->mon_stats.tx_fail,
			    sc->mon_stats.tx_ok, sc->mon_stats.tx_discard, sc->mon_stats.tx_inspect,
			    sc->mon_stats.tx_noack, sc->mon_stats.tx_other,
			    sc->mon_stats.last_ack_rssi, qwz_mon_tx_mode);
	}
	timeout_add_msec(&sc->mon_reap_timer, n ? 2 : 10);
}

/* ---- the monitor vdev ------------------------------------------------------ */

static int
qwz_mon_vdev_create(struct qwz_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	struct vdev_create_params params;
	int ret, bit, nss;

	if (sc->free_vdev_map == 0)
		return EBUSY;
	bit = ffs(sc->free_vdev_map) - 1;

	memset(vif, 0, offsetof(struct qwz_vif, txmgmt));	/* the DMA maps persist */
	vif->vdev_id = bit;
	vif->vdev_type = (qwz_mon_flags & QWZ_MON_F_AP) ? WMI_VDEV_TYPE_AP : WMI_VDEV_TYPE_MONITOR;
	vif->vdev_subtype = WMI_VDEV_SUBTYPE_NONE;
	vif->bank_id = -1;
	IEEE80211_ADDR_COPY(vif->vdev_addr, ic->ic_myaddr);
	if ((qwz_mon_flags & QWZ_MON_F_BSSID_MAC) && sc->mon_bssid_set)
		IEEE80211_ADDR_COPY(vif->vdev_addr, sc->mon_bssid);

	memset(&params, 0, sizeof(params));
	ret = qwz_mac_setup_vdev_create_params(sc, vif, pdev, &params);
	if (ret)
		return ret;
	ret = qwz_wmi_vdev_create(sc, vif->vdev_addr, &params);
	if (ret) {
		printf("qwz: mon: vdev %d create failed: %d\n", bit, ret);
		return ret;
	}
	sc->num_created_vdevs++;
	sc->allocated_vdev_map |= 1U << bit;
	sc->free_vdev_map &= ~(1U << bit);
	printf("qwz: mon: vdev %d created, type %s, mac %s\n", bit,
	    vif->vdev_type == WMI_VDEV_TYPE_AP ? "AP" : "MONITOR", ether_sprintf(vif->vdev_addr));

	ret = qwz_mac_op_update_vif_offload(sc, pdev, vif);
	if (ret)
		return ret;
	nss = qwz_get_num_chains(sc->cfg_tx_chainmask) ? : 1;
	ret = qwz_wmi_vdev_set_param_cmd(sc, vif->vdev_id, pdev->pdev_id,
	    WMI_VDEV_PARAM_NSS, nss);
	if (ret)
		printf("qwz: mon: vdev %d nss failed: %d\n", bit, ret);

	/*
	 * Linux creates an AP vdev's own peer right after the vdev (its
	 * management frames go out through that entry); without it the
	 * firmware completes our transmits with DISCARD. Goes through the
	 * port's peer list so the map/unmap events find it.
	 */
	sc->mon_self_peer = 0;
	if (vif->vdev_type == WMI_VDEV_TYPE_AP && !(qwz_mon_flags & QWZ_MON_F_NO_SELF_PEER)) {
		struct peer_create_params pp;
		struct qwz_node dummy;

		memset(&pp, 0, sizeof(pp));
		memset(&dummy, 0, sizeof(dummy));
		pp.peer_addr = vif->vdev_addr;
		pp.peer_type = WMI_PEER_TYPE_DEFAULT;
		pp.vdev_id = vif->vdev_id;
		ret = qwz_peer_create(sc, vif, pdev->pdev_id, &dummy.ni, &pp);
		printf("qwz: mon: self peer %s on vdev %d: %d\n",
		    ether_sprintf(vif->vdev_addr), bit, ret);
		sc->mon_self_peer = (ret == 0);
	}
	return 0;
}

/*
 * The smallest beacon an AP vdev can carry: hidden SSID, basic rates, DS
 * parameter, an empty TIM (the firmware fills the timestamp and TIM). Sent
 * with WMI_BCN_TMPL as Linux ath12k_wmi_bcn_tmpl() does.
 */
static int
qwz_mon_bcn_tmpl(struct qwz_softc *sc, int chan)
{
	struct qwz_vif *vif = &sc->sc_monvif;
	struct qwz_pdev_wmi *wmi = &sc->wmi.wmi[0];
	struct wmi_bcn_tmpl_cmd *cmd;
	struct wmi_bcn_prb_info *info;
	struct wmi_tlv *tlv;
	struct mbuf *m;
	uint8_t bcn[96], *p = bcn;
	uint32_t blen, aligned, tim_off;
	int len, ret;

	memset(bcn, 0, sizeof(bcn));
	p[0] = 0x80;					/* beacon */
	memset(p + 4, 0xff, 6);				/* addr1 */
	memcpy(p + 10, vif->vdev_addr, 6);		/* addr2 */
	memcpy(p + 16, sc->mon_bssid_set ? sc->mon_bssid : vif->vdev_addr, 6);	/* addr3 */
	p += 24;
	p += 8;						/* timestamp: firmware */
	p[0] = 100; p[1] = 0; p += 2;			/* beacon interval, TU */
	p[0] = 0x01; p[1] = 0x00; p += 2;		/* capability: ESS */
	*p++ = 0; *p++ = 0;				/* SSID, hidden */
	*p++ = 1; *p++ = 8;				/* supported rates */
	memcpy(p, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8); p += 8;
	*p++ = 3; *p++ = 1; *p++ = (uint8_t)chan;	/* DS parameter set */
	tim_off = (uint32_t)(p - bcn);
	*p++ = 5; *p++ = 4; *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 0;	/* TIM */
	blen = (uint32_t)(p - bcn);
	aligned = roundup(blen, 4);

	len = sizeof(*cmd) + sizeof(*info) + TLV_HDR_SIZE + aligned;
	m = qwz_wmi_alloc_mbuf(len);
	if (m == NULL)
		return ENOMEM;
	cmd = (struct wmi_bcn_tmpl_cmd *)(mtod(m, uint8_t *) +
	    sizeof(struct ath12k_htc_hdr) + sizeof(struct wmi_cmd_hdr));
	cmd->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_BCN_TMPL_CMD) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*cmd) - TLV_HDR_SIZE);
	cmd->vdev_id = vif->vdev_id;
	cmd->tim_ie_offset = tim_off;
	cmd->buf_len = blen;
	info = (struct wmi_bcn_prb_info *)((uint8_t *)cmd + sizeof(*cmd));
	info->tlv_header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_BCN_PRB_INFO) |
	    FIELD_PREP(WMI_TLV_LEN, sizeof(*info) - TLV_HDR_SIZE);
	tlv = (struct wmi_tlv *)((uint8_t *)info + sizeof(*info));
	tlv->header = FIELD_PREP(WMI_TLV_TAG, WMI_TAG_ARRAY_BYTE) |
	    FIELD_PREP(WMI_TLV_LEN, aligned);
	memcpy(tlv->value, bcn, blen);
	ret = qwz_wmi_cmd_send(wmi, m, WMI_BCN_TMPL_CMDID);
	if (ret)
		m_freem(m);
	printf("qwz: mon: beacon template (%u bytes) for vdev %u: %d\n", blen, vif->vdev_id, ret);
	return ret;
}

/* A firmware peer for the peer we talk to, bypassing the port's single-peer list. */
static void
qwz_mon_sender_peer(struct qwz_softc *sc, const uint8_t *addr, int add)
{
	struct peer_create_params pp;
	int ret;

	if (add) {
		if (sc->mon_sender_peer)
			return;
		memset(&pp, 0, sizeof(pp));
		pp.peer_addr = (uint8_t *)addr;
		pp.peer_type = WMI_PEER_TYPE_DEFAULT;
		pp.vdev_id = sc->sc_monvif.vdev_id;
		ret = qwz_wmi_send_peer_create_cmd(sc, sc->pdevs[0].pdev_id, &pp);
		if (ret == 0) {
			sc->mon_sender_peer = 1;
			memcpy(sc->mon_sender_addr, addr, 6);
		}
		printf("qwz: mon: raw peer %s on vdev %u: %d\n", ether_sprintf((u_char *)addr),
		    sc->sc_monvif.vdev_id, ret);
	} else if (sc->mon_sender_peer) {
		ret = qwz_wmi_send_peer_delete_cmd(sc, sc->mon_sender_addr,
		    sc->sc_monvif.vdev_id, sc->pdevs[0].pdev_id);
		printf("qwz: mon: raw peer %s deleted: %d\n", ether_sprintf(sc->mon_sender_addr), ret);
		sc->mon_sender_peer = 0;
	}
}

static int
qwz_mon_vdev_start(struct qwz_softc *sc, int chan, uint32_t freq)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	struct wmi_vdev_start_req_arg arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.vdev_id = vif->vdev_id;
	arg.channel.freq = freq;
	arg.channel.band_center_freq1 = freq;
	arg.channel.band_center_freq2 = freq;
	arg.channel.allow_ht = true;
	arg.channel.mode = chan <= 14 ? MODE_11NG_HT20 : MODE_11NA_HT20;
	arg.channel.passive = chan >= 52 && chan <= 144;	/* DFS: never transmit */
	arg.channel.min_power = 0;
	arg.channel.max_power = 20;
	arg.channel.max_reg_power = 20;
	arg.channel.max_antenna_gain = 0;
	arg.pref_tx_streams = sc->num_tx_chains;
	arg.pref_rx_streams = sc->num_rx_chains;
	if (vif->vdev_type == WMI_VDEV_TYPE_AP) {
		static uint8_t ssid[] = "qwzmon";
		arg.ssid = ssid;
		arg.ssid_len = 6;
		arg.hidden_ssid = true;
		arg.bcn_intval = 100;
		arg.dtim_period = 1;
	}

	sc->vdev_setup_done = 0;
	ret = qwz_wmi_vdev_start(sc, &arg, pdev->pdev_id, 0);
	if (ret) {
		printf("qwz: mon: vdev %d start failed: %d\n", vif->vdev_id, ret);
		return ret;
	}
	ret = qwz_mac_vdev_setup_sync(sc);
	if (ret) {
		printf("qwz: mon: vdev %d start: no response: %d\n", vif->vdev_id, ret);
		return ret;
	}
	sc->num_started_vdevs++;
	vif->is_started = 1;
	vif->chan_mode = arg.channel.mode;
	vif->chan_cfreq1 = freq;

	if (vif->vdev_type == WMI_VDEV_TYPE_AP && (qwz_mon_flags & QWZ_MON_F_BEACON))
		qwz_mon_bcn_tmpl(sc, chan);

	{
		const uint8_t *bssid = sc->mon_bssid_set ? sc->mon_bssid : vif->vdev_addr;

		ret = qwz_wmi_vdev_up(sc, vif->vdev_id, pdev->pdev_id, 0, bssid, NULL, 0, 0);
		if (ret) {
			printf("qwz: mon: vdev %d up failed: %d\n", vif->vdev_id, ret);
			return ret;
		}
		vif->is_up = 1;
		printf("qwz: mon: vdev %d up on channel %d (%u MHz), bssid %s\n", vif->vdev_id,
		    chan, freq, ether_sprintf((u_char *)bssid));
	}
	return 0;
}

static void
qwz_mon_vdev_teardown(struct qwz_softc *sc)
{
	struct qwz_pdev *pdev = &sc->pdevs[0];
	struct qwz_vif *vif = &sc->sc_monvif;
	int ret;

	qwz_mon_txq_purge(sc);
	qwz_mon_sender_peer(sc, NULL, 0);

	if (vif->is_up) {
		ret = qwz_wmi_vdev_down(sc, vif->vdev_id, pdev->pdev_id);
		if (ret)
			printf("qwz: mon: vdev %d down failed: %d\n", vif->vdev_id, ret);
		vif->is_up = 0;
	}
	if (vif->is_started) {
		sc->vdev_setup_done = 0;
		ret = qwz_wmi_vdev_stop(sc, vif->vdev_id, pdev->pdev_id);
		if (ret == 0)
			ret = qwz_mac_vdev_setup_sync(sc);
		if (ret)
			printf("qwz: mon: vdev %d stop failed: %d\n", vif->vdev_id, ret);
		vif->is_started = 0;
		if (sc->num_started_vdevs > 0)
			sc->num_started_vdevs--;
	}
	if (sc->mon_self_peer) {
		struct ath12k_peer *peer = qwz_peer_find_by_addr(sc, vif->vdev_addr);

		if (peer != NULL) {
			ret = qwz_peer_delete(sc, vif->vdev_id, pdev->pdev_id, peer);
			printf("qwz: mon: self peer deleted: %d\n", ret);
		}
		sc->mon_self_peer = 0;
	}
	if (sc->allocated_vdev_map & (1U << vif->vdev_id)) {
		ret = qwz_mac_vdev_delete_wait(sc, vif);
		if (ret)
			printf("qwz: mon: vdev %d delete failed: %d\n", vif->vdev_id, ret);
		else
			printf("qwz: mon: vdev %d deleted\n", vif->vdev_id);
	}
}

/* Under the gate; the caller holds ioctl_rwl where it matters. */
static int
qwz_mon_start(struct qwz_softc *sc, int chan)
{
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	uint32_t freq = qwz_mon_chan_freq(chan);
	int ret;

	if (freq == 0)
		return EINVAL;
	if (!sc->attached || !sc->fw_initialized)
		return ENXIO;
	if (sc->mon_active)
		return EALREADY;
	if (ifp->if_flags & IFF_RUNNING) {
		printf("qwz: mon: refused: the interface is up (turn Wi-Fi off first)\n");
		return EBUSY;
	}
	if (sc->num_started_vdevs > 0 || !TAILQ_EMPTY(&sc->peers)) {
		printf("qwz: mon: refused: a vdev is still started or peers remain\n");
		return EBUSY;
	}
	if (mon_frame == NULL || moncap_ring == NULL)
		return ENOMEM;

	memset(&sc->mon_stats, 0, sizeof(sc->mon_stats));
	sc->mon_chan = chan;
	sc->mon_freq = freq;
	sc->mon_bssid_set = qwz_mon_parse_mac(qwz_mon_bssid_str, sc->mon_bssid);
	if (qwz_mon_bssid_str[0] != 0 && !sc->mon_bssid_set)
		printf("qwz: mon: monbssid '%s' is not xx:xx:xx:xx:xx:xx; ignored\n", qwz_mon_bssid_str);
	printf("qwz: mon: starting on channel %d, flags 0x%x\n", chan, qwz_mon_flags);

	if ((qwz_mon_flags & QWZ_MON_F_DELETE_STA) && sc->num_created_vdevs > 0 &&
	    (sc->allocated_vdev_map & (1U << sc->sc_vif.vdev_id))) {
		/* qwz_init() recreates it on the next interface-up (num_created_vdevs == 0). */
		ret = qwz_mac_vdev_delete_wait(sc, &sc->sc_vif);
		printf("qwz: mon: station vdev %u deleted for the capture: %d\n",
		    sc->sc_vif.vdev_id, ret);
		if (ret)
			return ret;
	}

	ret = qwz_mon_vdev_create(sc);
	if (ret)
		goto fail;
	ret = qwz_mon_vdev_start(sc, chan, freq);
	if (ret)
		goto fail;
	if (!(qwz_mon_flags & QWZ_MON_F_KEEP_FILTER)) {
		ret = qwz_mon_rx_filter(sc, 1);
		if (ret)
			goto fail;
	}
	if (qwz_mon_flags & QWZ_MON_F_MNTFILTER)
		qwz_mon_wmi_mnt_filter(sc, sc->sc_monvif.vdev_id, 1);
	if (qwz_mon_flags & QWZ_MON_F_PROMISC)
		qwz_mon_wmi_promisc(sc, 1);
	if ((qwz_mon_flags & (QWZ_MON_F_NRP | QWZ_MON_F_NRP_STA)) && sc->mon_bssid_set)
		qwz_mon_wmi_nrp(sc, sc->sc_monvif.vdev_id, sc->mon_bssid, 1,
		    !!(qwz_mon_flags & QWZ_MON_F_NRP_STA));
	sc->mon_active = 1;
	if (sc->mon_reap_timer == NULL)
		timeout_set(&sc->mon_reap_timer, qwz_mon_reap, sc);
	timeout_add_msec(&sc->mon_reap_timer, 10);
	printf("qwz: mon: capturing\n");
	return 0;

fail:
	qwz_mon_vdev_teardown(sc);
	return ret;
}

void
qwz_mon_stop(struct qwz_softc *sc)
{
	if (!sc->mon_active)
		return;
	sc->mon_active = 0;
	timeout_del(&sc->mon_reap_timer);
	if (qwz_mon_flags & QWZ_MON_F_PROMISC)
		qwz_mon_wmi_promisc(sc, 0);
	if ((qwz_mon_flags & QWZ_MON_F_MNTFILTER) && sc->sc_monvif.is_up)
		qwz_mon_wmi_mnt_filter(sc, sc->sc_monvif.vdev_id, 0);
	if ((qwz_mon_flags & (QWZ_MON_F_NRP | QWZ_MON_F_NRP_STA)) && sc->mon_bssid_set &&
	    sc->sc_monvif.is_up)
		qwz_mon_wmi_nrp(sc, sc->sc_monvif.vdev_id, sc->mon_bssid, 0,
		    !!(qwz_mon_flags & QWZ_MON_F_NRP_STA));
	qwz_mon_vdev_teardown(sc);
	qwz_mon_reap_ring(sc);	/* return the buffers still in the ring */
	if (!(qwz_mon_flags & QWZ_MON_F_KEEP_FILTER)) {
		if (qwz_mon_rx_filter(sc, 0))
			printf("qwz: mon: could not restore the station filter\n");
	}
	printf("qwz: mon: stopped: entries %u mpdus %u frames %u mgmt %u reo %u "
	    "reo_err %u wbm_err %u bufs %u badcookie %u nolink %u duplink %u "
	    "linkret_fail %u capdrop %u\n",
	    sc->mon_stats.entries, sc->mon_stats.mpdus, sc->mon_stats.frames,
	    sc->mon_stats.mgmt, sc->mon_stats.reo, sc->mon_stats.reo_err,
	    sc->mon_stats.wbm_err, sc->mon_stats.bufs, sc->mon_stats.badcookie,
	    sc->mon_stats.nolink, sc->mon_stats.duplink, sc->mon_stats.linkret_fail,
	    moncap_dropped);
	printf("qwz: mon: tx: psf heard %u, sent %u fail %u, compl ok %u discard %u inspect %u noack %u other %u, last ack rssi %d\n",
	    sc->mon_stats.rx_psf, sc->mon_stats.tx_sent, sc->mon_stats.tx_fail,
	    sc->mon_stats.tx_ok, sc->mon_stats.tx_discard, sc->mon_stats.tx_inspect,
	    sc->mon_stats.tx_noack, sc->mon_stats.tx_other, sc->mon_stats.last_ack_rssi);
}

/* ---- entry points ----------------------------------------------------------- */

/*
 * Management frames never reach the RXDMA rings on this firmware: it
 * delivers them through WMI_MGMT_RX_EVENTID, with the channel, SNR and rate.
 * The event handler calls this while a capture runs instead of feeding
 * net80211. No FCS on this path.
 */
static uint8_t *mon_mgmt_buf;	/* QWZ_MONCAP_MAXFRAME bytes; WMI events and the REO path run under the gate */

void
qwz_mon_capture_reo_mbuf(struct qwz_softc *sc, struct mbuf *m)
{
	int len = M_PKTLEN(m);

	if (!sc->mon_active || mon_mgmt_buf == NULL || len <= 0 ||
	    len > QWZ_MONCAP_MAXFRAME)
		return;
	m_copydata(m, 0, len, mon_mgmt_buf);
	sc->mon_stats.reo++;
	moncap_put(mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_REO,
	    DP_RX_DECAP_TYPE_NATIVE_WIFI, (uint8_t)sc->mon_chan, 0, 0);
}

void
qwz_mon_capture_mgmt_mbuf(struct qwz_softc *sc, struct mbuf *m, int rssi,
    uint32_t rate_kbps)
{
	uint32_t r = rate_kbps / 500;
	int len = M_PKTLEN(m);

	if (!sc->mon_active || mon_mgmt_buf == NULL || len <= 0 ||
	    len > QWZ_MONCAP_MAXFRAME)
		return;
	m_copydata(m, 0, len, mon_mgmt_buf);
	sc->mon_stats.mgmt++;
	qwz_mon_rx_hook(sc, mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_WMI);
	moncap_put(mon_mgmt_buf, (size_t)len, QWZ_MONCAP_F_WMI,
	    DP_RX_DECAP_TYPE_RAW, (uint8_t)sc->mon_chan,
	    (int8_t)(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi),
	    (uint8_t)(r > 255 ? 255 : r));
}

void
qwz_mon_attach(struct qwz_softc *sc)
{
	qwz_mon_sc = sc;
	if (moncap_lock == NULL)
		moncap_lock = IOLockAlloc();
	if (moncap_ring == NULL)
		moncap_ring = (uint8_t *)IOMalloc(QWZ_MONCAP_SIZE);
	if (mon_frame == NULL)
		mon_frame = (uint8_t *)IOMalloc(QWZ_MONCAP_MAXFRAME);
	if (mon_mgmt_buf == NULL)
		mon_mgmt_buf = (uint8_t *)IOMalloc(QWZ_MONCAP_MAXFRAME);
	if (!mon_txmaps_ready) {
		struct qwz_txmgmt_queue *q = &sc->sc_monvif.txmgmt;
		const bus_size_t size = IEEE80211_MAX_LEN;
		int i, ret = 0;

		memset(q, 0, sizeof(*q));
		for (i = 0; i < nitems(q->data) && ret == 0; i++)
			ret = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
			    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, &q->data[i].map);
		mon_txmaps_ready = (ret == 0);
		if (ret)
			printf("qwz: mon: transmit DMA maps failed: %d (no M1 transmit)\n", ret);
	}
	moncap_head = moncap_tail = 0;
}

static IOReturn
qwz_mon_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	int chan = (int)(intptr_t)arg0;
	int *errp = (int *)arg1;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	rw_enter_write(&sc->ioctl_rwl);
	if (chan == 0) {
		qwz_mon_stop(sc);
		*errp = 0;
	} else {
		if (sc->mon_active)
			qwz_mon_stop(sc);
		*errp = qwz_mon_start(sc, chan);
	}
	rw_exit(&sc->ioctl_rwl);
	return kIOReturnSuccess;
}

int
qwz_mon_set(int chan)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	if (chan < 0 || (chan != 0 && qwz_mon_chan_freq(chan) == 0))
		return EINVAL;
	gate->runAction(qwz_mon_gated, (void *)(intptr_t)chan, &err);
	return err;
}

static IOReturn
qwz_mon_tx_gated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	struct qwz_softc *sc = qwz_mon_sc;
	int ops = (int)(intptr_t)arg0, *errp = (int *)arg1, ret = 0;

	if (sc == NULL) {
		*errp = ENXIO;
		return kIOReturnSuccess;
	}
	qwz_mon_tx_mode = ops & 2;
	if (ops & 1)
		ret = qwz_mon_tx_copy(sc, qwz_bcast);
	if ((ops & 4) && ret == 0)
		ret = qwz_mon_tx_copy(sc, mon_psf_src);
	if (ops & 5)
		printf("qwz: mon: tx: ops 0x%x -> %d (psf %zu bytes from %s)\n", ops, ret,
		    mon_psf_len, ether_sprintf(mon_psf_src));
	*errp = ret;
	return kIOReturnSuccess;
}

int
qwz_mon_tx(int ops)
{
	IOCommandGate *gate = qwz_compat_gate();
	int err = ENXIO;

	if (gate == NULL || qwz_mon_sc == NULL)
		return ENXIO;
	gate->runAction(qwz_mon_tx_gated, (void *)(intptr_t)ops, &err);
	return err;
}

void
qwz_mon_mac_str(char *out, size_t len)
{
	struct qwz_softc *sc = qwz_mon_sc;
	const uint8_t *m;

	if (sc == NULL) {
		snprintf(out, len, "00:00:00:00:00:00");
		return;
	}
	m = sc->mon_active ? sc->sc_monvif.vdev_addr : sc->mac_addr;
	snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int
qwz_mon_status(void)
{
	struct qwz_softc *sc = qwz_mon_sc;

	return (sc != NULL && sc->mon_active) ? sc->mon_chan : 0;
}
