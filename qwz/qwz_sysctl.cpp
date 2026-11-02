/*
 * qwz_sysctl - the debug.qwz.* sysctls. See qwz_sysctl.h.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_sysctl.h"
#include "qwz_log.h"
#include "qwz_mon.h"

#include <IOKit/IOLib.h>

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern int qwz_stage_sysctl;	/* qwz_compat.cpp */

static qwz_sysctl_start_fn qwz_sysctl_start_cb;
static void *qwz_sysctl_start_arg;
static int qwz_logfile_on;	/* debug.qwz.logfile: file sink kept open */
static bool qwz_sysctl_registered;

static int
qwz_sysctl_start(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v != 1)
		return EINVAL;
	if (qwz_sysctl_start_cb == NULL)
		return ENXIO;	/* this product's bring-up cannot be rerun */
	qwz_log_file_begin();
	qwz_log("qwz: sysctl debug.qwz.start=1 received\n");
	qwz_log_file_end();
	return qwz_sysctl_start_cb(qwz_sysctl_start_arg) ? 0 : EBUSY;
}

static int
qwz_sysctl_publish(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	qwz_log_publish();	/* refresh QwzMacLog in the I/O Registry */
	return 0;
}

static int
qwz_sysctl_logfile(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_logfile_on;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v && !qwz_logfile_on) {
		qwz_log_file_begin();
		qwz_log("qwz: ==== file log on (sysctl) ====\n");
	} else if (!v && qwz_logfile_on) {
		qwz_log("qwz: ==== file log off (sysctl) ====\n");
		qwz_log_file_end();
	}
	qwz_logfile_on = v ? 1 : 0;
	return 0;
}

/* debug.qwz.mon: channel while capturing; write a channel to start, 0 to stop. */
static int
qwz_sysctl_mon(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_mon_status();
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_set(v);
}

/* debug.qwz.montx: M1 transmit ops (bit 1 broadcast once, 2 auto reply, 4 unicast once). */
static int
qwz_sysctl_montx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = (int)qwz_mon_tx_mode;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_tx(v);
}

/* debug.qwz.montxframe: write a qwz_montx_hdr plus 802.11 frame to transmit it (M2). */
static int
qwz_sysctl_montxframe(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	uint8_t buf[sizeof(struct qwz_montx_hdr) + QWZ_MONTX_MAXFRAME];
	int err;

	if (req->newptr == USER_ADDR_NULL)
		return 0;	/* nothing to read back */
	if (req->newlen > sizeof(buf))
		return EINVAL;
	err = SYSCTL_IN(req, buf, req->newlen);
	if (err)
		return err;
	return qwz_mon_tx_frame(buf, req->newlen);
}

/* debug.qwz.monpeer: "add <mac>" / "del" for a firmware peer on the capture vdev. */
static char qwz_mon_peer_str[32];
static int
qwz_sysctl_monpeer(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int err = sysctl_handle_string(oidp, qwz_mon_peer_str, sizeof(qwz_mon_peer_str), req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_peer(qwz_mon_peer_str);
}

static int
qwz_sysctl_awdlif(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int err = sysctl_handle_string(oidp, qwz_mon_peer_str, sizeof(qwz_mon_peer_str), req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	return qwz_mon_awdlif(qwz_mon_peer_str);
}

/* debug.qwz.monmac: the capture vdev's MAC (the AWDL address a node here must use). */
static int
qwz_sysctl_monmac(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	char mac[18];

	qwz_mon_mac_str(mac, sizeof(mac));
	return sysctl_handle_string(oidp, mac, sizeof(mac), req);
}

/* debug.qwz.moncap: drains the capture ring as a stream of qwz_moncap_hdr records. */
static int
qwz_sysctl_moncap(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	const size_t MAX = 1024 * 1024;
	size_t avail = qwz_mon_cap_pending(), max, n;
	void *buf;
	int err;

	if (req->newptr != USER_ADDR_NULL)
		return EPERM;
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = avail;
		return 0;
	}
	max = req->oldlen < MAX ? req->oldlen : MAX;
	if (max == 0)
		return 0;
	buf = IOMalloc(max);
	if (buf == NULL)
		return ENOMEM;
	n = qwz_mon_cap_drain(buf, max);
	err = n ? SYSCTL_OUT(req, buf, n) : 0;
	IOFree(buf, max);
	return err;
}

SYSCTL_NODE(_debug, OID_AUTO, qwz, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "QwzMac");
SYSCTL_PROC(_debug_qwz, OID_AUTO, mon, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_mon, "I", "monitor-mode capture: channel to start, 0 to stop (Wi-Fi must be off)");
SYSCTL_UINT(_debug_qwz, OID_AUTO, monflags, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_mon_flags, 0, "monitor-mode experiment flags (QWZ_MON_F_* in qwz_mon.h)");
SYSCTL_UINT(_debug_qwz, OID_AUTO, monhold, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_mon_hold, 0, "1: Wi-Fi on keeps the capture cell instead of starting the station");
SYSCTL_PROC(_debug_qwz, OID_AUTO, monmac, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_monmac, "A", "the capture vdev's MAC: the AWDL address for a node on this card");
SYSCTL_PROC(_debug_qwz, OID_AUTO, montxframe, CTLTYPE_OPAQUE | CTLFLAG_WR | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_montxframe, "S", "transmit a frame from userspace (qwz_montx_hdr + 802.11 frame)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, monpeer, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_monpeer, "A", "firmware peer on the capture vdev: 'add xx:xx:xx:xx:xx:xx' or 'del'");
SYSCTL_PROC(_debug_qwz, OID_AUTO, awdlif, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_awdlif, "A", "\"feth0\" marks the interface AWDL (IFEF_AWDL) for mDNSResponder/sharingd, \"-feth0\" clears");
SYSCTL_PROC(_debug_qwz, OID_AUTO, montx, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_montx, "I", "monitor-mode transmit: 1 broadcast PSF copy once, 2 auto unicast reply, 4 unicast once");
SYSCTL_STRING(_debug_qwz, OID_AUTO, monbssid, CTLFLAG_RW | CTLFLAG_LOCKED,
    qwz_mon_bssid_str, sizeof(qwz_mon_bssid_str), "BSSID the monitor vdev comes up with (xx:xx:xx:xx:xx:xx; empty: own MAC)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, moncap, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_moncap, "S", "drain captured frames (scripts/qwz-moncap.py)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, start, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_start, "I", "write 1 to run the bring-up");
SYSCTL_INT(_debug_qwz, OID_AUTO, stage, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_stage_sysctl, 0, "stop the bring-up before this stage (-1: boot-time limit)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, publish, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_publish, "I", "write 1 to republish the log ring as QwzMacLog");
SYSCTL_PROC(_debug_qwz, OID_AUTO, logfile, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_logfile, "I", "1: every log line also goes to " QWZ_LOG_FILE);
#ifdef QWZ_DEBUG
extern uint32_t qwz_debug;	/* qwz.c: QWZ_D_* mask from qwzvar.h */
SYSCTL_UINT(_debug_qwz, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_debug, 0, "qwz debug mask (QWZ_D_* in qwzvar.h)");
#endif

void
qwz_sysctl_register(qwz_sysctl_start_fn start, void *arg)
{
	qwz_sysctl_start_cb = start;
	qwz_sysctl_start_arg = arg;
	if (qwz_sysctl_registered)
		return;
	sysctl_register_oid(&sysctl__debug_qwz);
	sysctl_register_oid(&sysctl__debug_qwz_start);
	sysctl_register_oid(&sysctl__debug_qwz_stage);
	sysctl_register_oid(&sysctl__debug_qwz_publish);
	sysctl_register_oid(&sysctl__debug_qwz_logfile);
	sysctl_register_oid(&sysctl__debug_qwz_mon);
	sysctl_register_oid(&sysctl__debug_qwz_monflags);
	sysctl_register_oid(&sysctl__debug_qwz_monhold);
	sysctl_register_oid(&sysctl__debug_qwz_monbssid);
	sysctl_register_oid(&sysctl__debug_qwz_montx);
	sysctl_register_oid(&sysctl__debug_qwz_monmac);
	sysctl_register_oid(&sysctl__debug_qwz_montxframe);
	sysctl_register_oid(&sysctl__debug_qwz_monpeer);
	sysctl_register_oid(&sysctl__debug_qwz_awdlif);
	sysctl_register_oid(&sysctl__debug_qwz_moncap);
#ifdef QWZ_DEBUG
	sysctl_register_oid(&sysctl__debug_qwz_debug);
#endif
	qwz_sysctl_registered = true;
}

void
qwz_sysctl_unregister(void)
{
	qwz_sysctl_start_cb = NULL;
	qwz_sysctl_start_arg = NULL;
	if (!qwz_sysctl_registered)
		return;
#ifdef QWZ_DEBUG
	sysctl_unregister_oid(&sysctl__debug_qwz_debug);
#endif
	sysctl_unregister_oid(&sysctl__debug_qwz_moncap);
	sysctl_unregister_oid(&sysctl__debug_qwz_monpeer);
	sysctl_unregister_oid(&sysctl__debug_qwz_awdlif);
	sysctl_unregister_oid(&sysctl__debug_qwz_montxframe);
	sysctl_unregister_oid(&sysctl__debug_qwz_monmac);
	sysctl_unregister_oid(&sysctl__debug_qwz_montx);
	sysctl_unregister_oid(&sysctl__debug_qwz_monbssid);
	sysctl_unregister_oid(&sysctl__debug_qwz_monflags);
	sysctl_unregister_oid(&sysctl__debug_qwz_monhold);
	sysctl_unregister_oid(&sysctl__debug_qwz_mon);
	sysctl_unregister_oid(&sysctl__debug_qwz_logfile);
	sysctl_unregister_oid(&sysctl__debug_qwz_publish);
	sysctl_unregister_oid(&sysctl__debug_qwz_stage);
	sysctl_unregister_oid(&sysctl__debug_qwz_start);
	sysctl_unregister_oid(&sysctl__debug_qwz);
	qwz_sysctl_registered = false;
}

int
qwz_sysctl_logfile_on(void)
{
	return qwz_logfile_on;
}
