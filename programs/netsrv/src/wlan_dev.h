#ifndef NETTRAY_WLAN_DEV_H
#define NETTRAY_WLAN_DEV_H

// /dev/wlanN, which needs the network permission (SEC_NET). All return 0
// (or a handle), or -1 with errno: EPERM without the permission, ENOENT
// for no such adapter (or one that was unplugged), and the driver's codes
// for a scan (EBUSY one is running, EAGAIN still identifying the adapter,
// ENOTTY a chip that can't scan yet, EIO the radio would not come up).
// Connecting: ENOENT not in the last scan, EINVAL a bad password or
// security it can't do, ENOTTY an adapter that can't connect yet.

#include <stdint.h>
#include "wlan_abi.h"

int  wlan_dev_open(uint32_t index);
void wlan_dev_close(int fd);
int  wlan_dev_info(int fd, struct WlanInfo* out);
int  wlan_dev_scan(int fd);
int  wlan_dev_results(int fd, struct WlanScanResults* out);
int  wlan_dev_connect(int fd, const struct WlanConnect* c);
int  wlan_dev_disconnect(int fd);
int  wlan_dev_link(int fd, struct WlanLink* out);

#endif
