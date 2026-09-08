/* nat-os -- the network as a device table entry. See netdev.c for why this is
 * a device rather than a syscall, and why the request is handed to the net
 * task rather than served where it is made. */

#ifndef NATOS_NETDEV_H
#define NATOS_NETDEV_H

#include <stdint.h>

/* The device callbacks. Named in device.c's table; not called directly. */
int  netdev_read(uint32_t caller, uint32_t chan, uint32_t *out);
int  netdev_xfer_out(uint32_t caller, uint32_t chan, const uint8_t *buf,
                     uint32_t len);
int  netdev_xfer_in(uint32_t caller, uint32_t chan, uint8_t *buf, uint32_t len);

/* Driven from the NET TASK's loop, and only from there: webfetch.h requires
 * it, because the raw lwIP API is not thread safe under NO_SYS=1. */
void netdev_service(void);

#endif /* NATOS_NETDEV_H */
