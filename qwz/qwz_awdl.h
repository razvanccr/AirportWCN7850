/*
 * qwz_awdl - an `awdl0` of our own (docs/airdrop-revisited.md, M4).
 *
 * macOS's AirDrop receiver (sharingd) listens on every interface but its
 * listener's policy, and mDNSResponder's publication of its record, want
 * an AWDL interface: Ethernet family, Wi-Fi subfamily, IFEF_AWDL, named
 * awdl0. A fake-Ethernet pair cannot be that (the subfamily is fixed at
 * allocation), so the kext allocates the interface itself and hands its
 * packets to the userspace AWDL node through sysctls, like the capture
 * ring:
 *
 *   debug.qwz.awdl0 = "xx:xx:xx:xx:xx:xx"   create awdl0 with this MAC
 *   debug.qwz.awdl0 = "down"                destroy it
 *   debug.qwz.awdltx                        read-only: frames the stack sent
 *                                           (records: uint32 len, frame, pad 4)
 *   debug.qwz.awdlrx                        write-only: frames from the air,
 *                                           same records, injected as received
 *
 * The daemon marks it up, gives it the EUI-64 link-local address and the
 * peers' neighbour entries, and relays frames both ways.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_AWDL_H
#define QWZ_AWDL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int	qwz_awdl_set(const char *cmd);			/* errno */
void	qwz_awdl_status(char *out, size_t len);		/* "down" or the MAC */
size_t	qwz_awdl_tx_pending(void);			/* bytes a drain would return */
size_t	qwz_awdl_tx_drain(void *buf, size_t max);	/* whole records only */
int	qwz_awdl_rx_inject(const void *buf, size_t len);	/* errno */

#ifdef __cplusplus
}
#endif

#endif /* QWZ_AWDL_H */
