#ifndef ATHN_KNET_H
#define ATHN_KNET_H

//
// The parts of Wi-Fi with no hardware in them stay in the kernel: the
// wlanN names (wlan_dev.c), beacon parsing (wlan_scan.c), the station
// (wlan_sta.c: joining, WPA2) and the IP stack's interfaces (net/). Their
// headers give the types; the calls go through the driver table. Included
// after those headers, so only the calls below are renamed.
//

#include <kernel/arch/x86/drivers/net/wlan.h>
#include <kernel/arch/x86/drivers/net/wlan_dev.h>
#include <kernel/arch/x86/drivers/net/wlan_scan.h>
#include <kernel/arch/x86/drivers/net/wlan_sta.h>
#include <kernel/net/net.h>

#ifndef ATHN_HOST_TEST

#define wlan_register(o, c)                 (kapi->wlan_register((o), (c)))
#define wlan_dev_name(w)                    (kapi->wlan_dev_name(w))
#define wlan_unregister(w)                  (kapi->wlan_unregister(w))
#define wlan_parse_beacon(f, l, c, o)       (kapi->wlan_parse_beacon((f), (l), (c), (o)))
#define wlan_scan_merge(t, c, m, b)         (kapi->wlan_scan_merge((t), (c), (m), (b)))

#define wlan_sta_init(s, o, c, m)           (kapi->wlan_sta_init((s), (o), (c), (m)))
#define wlan_sta_connect(s, b, p, n)        (kapi->wlan_sta_connect((s), (b), (p), (n)))
#define wlan_sta_disconnect(s, r)           (kapi->wlan_sta_disconnect((s), (r)))
#define wlan_sta_rx(s, f, l, r, n)          (kapi->wlan_sta_rx((s), (f), (l), (r), (n)))
#define wlan_sta_tick(s, n)                 (kapi->wlan_sta_tick((s), (n)))
#define wlan_sta_send(s, f, l)              (kapi->wlan_sta_send((s), (f), (l)))
#define wlan_sta_link(s, o)                 (kapi->wlan_sta_link((s), (o)))

#define netif_register(n, o, d, m, t)       (kapi->netif_register((n), (o), (d), (m), (t)))
#define netif_unregister(i)                 (kapi->netif_unregister(i))
#define netif_set_link(i, l)                (kapi->netif_set_link((i), (l)))
#define netif_rx(i, f, l)                   (kapi->netif_rx((i), (f), (l)))

#endif

#endif
