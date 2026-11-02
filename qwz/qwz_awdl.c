/*
 * qwz_awdl.c - the kext's own awdl0 (see qwz_awdl.h).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
#ifdef __cplusplus
extern "C" {	/* bpf.h has no C-linkage guards */
#endif
#include <net/bpf.h>
#ifdef __cplusplus
}
#endif
#include <net/ethernet.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>

#include "qwz_awdl.h"

#ifdef __cplusplus
extern "C" {
#endif
/* xnu kpi_interface.h, KERNEL_PRIVATE; exported by com.apple.kpi.bsd. */
struct ifnet_init_eparams_v2 {
	u_int32_t ver, len, flags;
	const void *uniqueid; u_int32_t uniqueid_len;
	const char *name; u_int32_t unit; ifnet_family_t family; u_int32_t type;
	u_int32_t sndq_maxlen;
	ifnet_output_func output;
	void *pre_enqueue; void *start; void *output_ctl;
	u_int32_t output_sched_model, output_target_qdelay;
	u_int64_t output_bw, output_bw_max, output_lt, output_lt_max;
	u_int16_t start_delay_qlen, start_delay_timeout;
	u_int32_t _reserved[3];
	void *input_poll; void *input_ctl;
	u_int32_t rcvq_maxlen, __reserved;
	u_int64_t input_bw, input_bw_max, input_lt, input_lt_max;
	u_int64_t ___reserved[2];
	ifnet_demux_func demux;
	ifnet_add_proto_func add_proto;
	ifnet_del_proto_func del_proto;
	ifnet_check_multi check_multi;
	ifnet_framer_func framer;
	void *softc;
	ifnet_ioctl_func ioctl;
	void *set_bpf_tap;
	ifnet_detached_func detach;
	ifnet_event_func event;
	const void *broadcast_addr; u_int32_t broadcast_len;
	void *framer_extended;
	u_int32_t subfamily;
	u_int16_t tx_headroom, tx_trailer;
	u_int32_t rx_mit_ival;
	u_int32_t ____reserved;
	void *free;
};
#define QWZ_IFNET_INIT_VERSION_2	2
#define QWZ_IFNET_INIT_LEGACY		0x1
#define QWZ_IFNET_INIT_NX_NOAUTO	0x4	/* no Skywalk netif nexus on us */
#define QWZ_IFNET_SUBFAMILY_WIFI	3
#define QWZ_IFEF_AWDL			0x00100000
extern errno_t ifnet_allocate_extended(const struct ifnet_init_eparams_v2 *, ifnet_t *);
extern errno_t ifnet_set_eflags(ifnet_t, u_int32_t new_flags, u_int32_t mask);
extern u_int32_t ifnet_eflags(ifnet_t);
#ifdef __cplusplus
}
#endif

#define QWZ_AWDL_RING		(512 * 1024)
#define QWZ_AWDL_MAXFRAME	2048
#define QWZ_AWDL_MTU		1450

static const uint8_t awdl_bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static ifnet_t awdl_ifp;
static uint8_t awdl_mac[6];
static int awdl_detached;
static IOLock *awdl_lock;
static uint8_t *awdl_ring;			/* stack -> daemon */
/* With the Skywalk netif nexus (no NX_NOAUTO): Network.framework listeners
 * such as sharingd's QUIC transfer service bind only to interfaces that
 * have one; without it they fell back to the LAN interface and the phone's
 * datagrams on awdl0 found no socket. The first build with a nexus froze
 * the kernel, but that build also had the mbuf length bug; "nexus:" in the
 * create string asks for it again. */
static int awdl_nexus;
static size_t awdl_head, awdl_tail;
static uint32_t awdl_tx_dropped, awdl_tx_count, awdl_rx_count, awdl_rx_bad;
static uint32_t awdl_rx_bad_len, awdl_rx_bad_alloc, awdl_rx_bad_chain, awdl_rx_bad_input;

static size_t
ring_free(void)
{
	return (awdl_tail + QWZ_AWDL_RING - awdl_head - 1) % QWZ_AWDL_RING;
}

/* ---- the interface's callbacks ------------------------------------------ */

static errno_t
awdl_output(ifnet_t ifp, mbuf_t m)
{
	size_t len = mbuf_pkthdr_len(m), rec, need;
	uint32_t hdr;

	if (ifp != awdl_ifp || awdl_ring == NULL) {	/* being destroyed */
		mbuf_freem(m);
		return 0;
	}
	bpf_tap_out(ifp, DLT_EN10MB, m, NULL, 0);
	if (len < 14 || len > QWZ_AWDL_MAXFRAME) {
		awdl_tx_dropped++;
		mbuf_freem(m);
		return 0;
	}
	rec = 4 + ((len + 3) & ~(size_t)3);
	need = rec;
	IOLockLock(awdl_lock);
	if (awdl_head + rec > QWZ_AWDL_RING)
		need += QWZ_AWDL_RING - awdl_head;
	if (need > ring_free()) {
		awdl_tx_dropped++;
		IOLockUnlock(awdl_lock);
		mbuf_freem(m);
		return 0;
	}
	if (awdl_head + rec > QWZ_AWDL_RING) {
		hdr = 0xffffffffU;	/* wrap marker */
		memcpy(awdl_ring + awdl_head, &hdr, 4);
		awdl_head = 0;
	}
	hdr = (uint32_t)len;
	memcpy(awdl_ring + awdl_head, &hdr, 4);
	mbuf_copydata(m, 0, len, awdl_ring + awdl_head + 4);
	awdl_head = (awdl_head + rec) % QWZ_AWDL_RING;
	awdl_tx_count++;
	IOLockUnlock(awdl_lock);
	mbuf_freem(m);
	return 0;
}

static errno_t
awdl_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
	switch (cmd) {
	case SIOCSIFFLAGS:
		/* RUNNING is set at creation and never changes: the daemon is
		 * the link. No flag changes from inside the ioctl path. */
		return 0;
	case SIOCSIFMTU:
		return 0;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static void
awdl_detach_cb(ifnet_t ifp)
{
	(void)ifp;
	awdl_detached = 1;
}

/* ---- create / destroy ---------------------------------------------------- */

static int
parse_mac(const char *s, uint8_t *out)
{
	int i;

	for (i = 0; i < 6; i++) {
		unsigned v = 0;
		int n;
		for (n = 0; n < 2; n++) {
			char c = s[i * 3 + n];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return 0;
		}
		if (i < 5 && s[i * 3 + 2] != ':')
			return 0;
		out[i] = (uint8_t)v;
	}
	return 1;
}

static int
awdl_create(const uint8_t *mac)
{
	struct ifnet_init_eparams_v2 e;
	struct sockaddr_dl sdl;
	ifnet_t ifp = NULL;
	errno_t err;
	u_int32_t ef;

	if (awdl_ifp != NULL)
		return EEXIST;
	if (awdl_lock == NULL)
		awdl_lock = IOLockAlloc();
	if (awdl_ring == NULL)
		awdl_ring = (uint8_t *)IOMalloc(QWZ_AWDL_RING);
	if (awdl_lock == NULL || awdl_ring == NULL)
		return ENOMEM;
	awdl_head = awdl_tail = 0;
	awdl_detached = 0;
	memcpy(awdl_mac, mac, 6);

	memset(&e, 0, sizeof(e));
	e.ver = QWZ_IFNET_INIT_VERSION_2;
	e.len = sizeof(e);
	/* Legacy output model, and no Skywalk nexus auto-attached: the first
	 * build without NX_NOAUTO got a netif nexus and CHANNEL_IO, and the
	 * kernel froze ten seconds after sharingd started using the link. */
	e.flags = QWZ_IFNET_INIT_LEGACY | (awdl_nexus ? 0 : QWZ_IFNET_INIT_NX_NOAUTO);
	e.uniqueid = "qwz-awdl0";
	e.uniqueid_len = 9;
	e.name = "awdl";
	e.unit = 0;
	e.family = IFNET_FAMILY_ETHERNET;
	e.subfamily = QWZ_IFNET_SUBFAMILY_WIFI;
	e.type = IFT_ETHER;
	e.output = awdl_output;
	e.demux = ether_demux;
	e.add_proto = ether_add_proto;
	e.del_proto = ether_del_proto;
	e.check_multi = ether_check_multi;
	e.framer = ether_frameout;
	e.ioctl = awdl_ioctl;
	e.detach = awdl_detach_cb;
	e.broadcast_addr = awdl_bcast;
	e.broadcast_len = 6;
	err = ifnet_allocate_extended(&e, &ifp);
	if (err) {
		printf("qwz: awdl0: ifnet_allocate_extended -> %d (struct len %u)\n", err, (unsigned)sizeof(e));
		return err;
	}
	ifnet_set_mtu(ifp, QWZ_AWDL_MTU);
	ifnet_set_addrlen(ifp, 6);
	ifnet_set_hdrlen(ifp, 14);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST | IFF_RUNNING, 0xffff);
	ifnet_set_baudrate(ifp, 24 * 1000 * 1000);

	memset(&sdl, 0, sizeof(sdl));
	sdl.sdl_len = sizeof(sdl);
	sdl.sdl_family = AF_LINK;
	sdl.sdl_type = IFT_ETHER;
	sdl.sdl_nlen = 0;
	sdl.sdl_alen = 6;
	memcpy(LLADDR(&sdl), mac, 6);
	err = ifnet_attach(ifp, &sdl);
	if (err) {
		printf("qwz: awdl0: ifnet_attach -> %d\n", err);
		ifnet_release(ifp);
		return err;
	}
	bpfattach(ifp, DLT_EN10MB, 14);
	err = ifnet_set_eflags(ifp, QWZ_IFEF_AWDL, QWZ_IFEF_AWDL);
	ef = ifnet_eflags(ifp);
	awdl_ifp = ifp;
	printf("qwz: awdl0: up as %02x:%02x:%02x:%02x:%02x:%02x, IFEF_AWDL -> %d, eflags 0x%08x\n",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], err, ef);
	return 0;
}

static int
awdl_destroy(void)
{
	ifnet_t ifp = awdl_ifp;
	errno_t err;
	int i;

	if (ifp == NULL)
		return ENXIO;
	awdl_ifp = NULL;
	ifnet_set_flags(ifp, 0, IFF_UP | IFF_RUNNING);
	err = ifnet_detach(ifp);
	for (i = 0; i < 200 && !awdl_detached; i++)
		IOSleep(10);
	printf("qwz: awdl0: detach -> %d (%s)\n", err, awdl_detached ? "detached" : "still attached");
	ifnet_release(ifp);
	return err;
}

/* ---- the sysctls' side --------------------------------------------------- */

int
qwz_awdl_set(const char *cmd)
{
	uint8_t mac[6];

	if (strcmp(cmd, "down") == 0 || cmd[0] == 0)
		return awdl_destroy();
	awdl_nexus = 0;
	if (strncmp(cmd, "nexus:", 6) == 0) {
		awdl_nexus = 1;
		cmd += 6;
	}
	if (!parse_mac(cmd, mac))
		return EINVAL;
	return awdl_create(mac);
}

void
qwz_awdl_status(char *out, size_t len)
{
	if (awdl_ifp == NULL) {
		snprintf(out, len, "down");
		return;
	}
	snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x tx %u rx %u txdrop %u rxbad %u (len %u alloc %u chain %u input %u)",
	    awdl_mac[0], awdl_mac[1], awdl_mac[2], awdl_mac[3], awdl_mac[4], awdl_mac[5],
	    awdl_tx_count, awdl_rx_count, awdl_tx_dropped, awdl_rx_bad,
	    awdl_rx_bad_len, awdl_rx_bad_alloc, awdl_rx_bad_chain, awdl_rx_bad_input);
}

size_t
qwz_awdl_tx_pending(void)
{
	size_t n;

	if (awdl_lock == NULL || awdl_ring == NULL)
		return 0;
	IOLockLock(awdl_lock);
	n = (awdl_head + QWZ_AWDL_RING - awdl_tail) % QWZ_AWDL_RING;
	IOLockUnlock(awdl_lock);
	return n;
}

size_t
qwz_awdl_tx_drain(void *buf, size_t max)
{
	uint8_t *out = (uint8_t *)buf;
	size_t n = 0;

	if (awdl_lock == NULL || awdl_ring == NULL)
		return 0;
	IOLockLock(awdl_lock);
	while (awdl_tail != awdl_head) {
		uint32_t len;
		size_t rec;

		memcpy(&len, awdl_ring + awdl_tail, 4);
		if (len == 0xffffffffU) {
			awdl_tail = 0;
			continue;
		}
		rec = 4 + ((len + 3) & ~(size_t)3);
		if (n + rec > max)
			break;
		memcpy(out + n, awdl_ring + awdl_tail, rec);
		n += rec;
		awdl_tail = (awdl_tail + rec) % QWZ_AWDL_RING;
	}
	IOLockUnlock(awdl_lock);
	return n;
}

int
qwz_awdl_rx_inject(const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	ifnet_t ifp = awdl_ifp;
	size_t off = 0;
	int injected = 0;

	if (ifp == NULL)
		return ENXIO;
	while (off + 4 <= len) {
		uint32_t flen;
		size_t rec;
		mbuf_t m = NULL;
		struct ifnet_stat_increment_param st;
		char *data;

		memcpy(&flen, p + off, 4);
		rec = 4 + ((flen + 3) & ~(size_t)3);
		if (flen < 14 || flen > QWZ_AWDL_MAXFRAME || off + 4 + flen > len) {
			awdl_rx_bad++;
			awdl_rx_bad_len++;
			return injected ? 0 : EINVAL;
		}
		/* One buffer per frame, always: a frame split across a chain
		 * had its header taken apart wrongly and the stack dropped the
		 * phone's TLS records (M4). */
		{
			unsigned int chunks = 1;

			if (mbuf_allocpacket(MBUF_WAITOK, flen, &chunks, &m) != 0 || m == NULL) {
				awdl_rx_bad++;
				awdl_rx_bad_alloc++;
				return ENOBUFS;
			}
		}
		if (mbuf_next(m) != NULL || mbuf_maxlen(m) < flen) {
			mbuf_freem(m);
			awdl_rx_bad++;
			awdl_rx_bad_chain++;
			off += rec;
			continue;
		}
		data = (char *)mbuf_data(m);
		memcpy(data, p + off + 4, flen);
		mbuf_setlen(m, flen);
		mbuf_pkthdr_setlen(m, flen);
		mbuf_pkthdr_setrcvif(m, ifp);
		bpf_tap_in(ifp, DLT_EN10MB, m, NULL, 0);
		/* The stack wants the frame header aside and the data past it:
		 * both the buffer's and the packet's length shrink by it. */
		mbuf_pkthdr_setheader(m, data);
		mbuf_setdata(m, data + 14, flen - 14);
		mbuf_pkthdr_setlen(m, flen - 14);
		/* The stack's demux trusts the driver for these: a multicast
		 * frame without M_MCAST is matched against our own MAC and
		 * dropped (unless something put the interface in promiscuous
		 * mode, which is how tcpdump made AirDrop discovery work). */
		{
			const uint8_t *dst = (const uint8_t *)data;
			mbuf_flags_t f = 0;

			if (dst[0] & 1)
				f |= (memcmp(dst, awdl_bcast, 6) == 0) ? MBUF_BCAST : MBUF_MCAST;
			mbuf_setflags_mask(m, f, MBUF_BCAST | MBUF_MCAST);
		}
		memset(&st, 0, sizeof(st));
		st.packets_in = 1;
		st.bytes_in = (u_int32_t)flen;
		if (ifnet_input(ifp, m, &st) != 0) {
			awdl_rx_bad++;
			awdl_rx_bad_input++;
		} else
			awdl_rx_count++;
		injected++;
		off += rec;
	}
	return 0;
}
