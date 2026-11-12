/*
 * The adapter, through its character device. Kept apart from the rest so
 * the host test can put a pretend adapter in its place.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "wlan_dev.h"

int wlan_dev_open(uint32_t index) {
    char path[16];
    snprintf(path, sizeof(path), "/dev/wlan%u", (unsigned)index);
    return open(path, O_RDONLY);
}

void wlan_dev_close(int fd) {
    if (fd >= 0)
        close(fd);
}

int wlan_dev_info(int fd, struct WlanInfo* out) {
    return ioctl(fd, WLAN_IOCTL_INFO, out) < 0 ? -1 : 0;
}

int wlan_dev_scan(int fd) {
    return ioctl(fd, WLAN_IOCTL_SCAN, 0) < 0 ? -1 : 0;
}

int wlan_dev_results(int fd, struct WlanScanResults* out) {
    return ioctl(fd, WLAN_IOCTL_SCAN_RESULTS, out) < 0 ? -1 : 0;
}

int wlan_dev_connect(int fd, const struct WlanConnect* c) {
    return ioctl(fd, WLAN_IOCTL_CONNECT, c) < 0 ? -1 : 0;
}

int wlan_dev_disconnect(int fd) {
    return ioctl(fd, WLAN_IOCTL_DISCONNECT, 0) < 0 ? -1 : 0;
}

int wlan_dev_link(int fd, struct WlanLink* out) {
    return ioctl(fd, WLAN_IOCTL_LINK, out) < 0 ? -1 : 0;
}
