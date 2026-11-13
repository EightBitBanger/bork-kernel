#ifndef RTW_DEVICES_H
#define RTW_DEVICES_H

//
// Devices handled. One list builds the driver's match table and family
// lookup, and the kernel's list of which sticks to load sys/drivers/rtw-usb
// for (driver/autoload.c), so the three can't drift apart. From the Linux
// rtw88 and rtl8821cu / rtl8812au tables. X(vendor, product, family):
// family is a WLAN_FAMILY_* (arch/x86/drivers/net/wlan.h).
//
#define RTW_DEVICES(X) \
    /* RTL8811AU / RTL8821AU */ \
    X(0x0BDA, 0x0811, WLAN_FAMILY_RTL8821A)     /* Realtek reference */ \
    X(0x0BDA, 0x0820, WLAN_FAMILY_RTL8821A) \
    X(0x0BDA, 0x0821, WLAN_FAMILY_RTL8821A) \
    X(0x0BDA, 0x0823, WLAN_FAMILY_RTL8821A) \
    X(0x0BDA, 0xA811, WLAN_FAMILY_RTL8821A) \
    X(0x0411, 0x0242, WLAN_FAMILY_RTL8821A)     /* Buffalo WI-U2-433DM */ \
    X(0x0411, 0x029B, WLAN_FAMILY_RTL8821A)     /* Buffalo WI-U2-433DHP */ \
    X(0x04BB, 0x0953, WLAN_FAMILY_RTL8821A)     /* I-O DATA WN-AC433UK */ \
    X(0x056E, 0x4007, WLAN_FAMILY_RTL8821A)     /* Elecom WDC-433DU2HBK */ \
    X(0x056E, 0x400E, WLAN_FAMILY_RTL8821A)     /* Elecom WDC-433SU2M */ \
    X(0x056E, 0x400F, WLAN_FAMILY_RTL8821A)     /* Elecom WDC-433SU2M2 */ \
    X(0x0846, 0x9052, WLAN_FAMILY_RTL8821A)     /* Netgear A6100 */ \
    X(0x0E66, 0x0023, WLAN_FAMILY_RTL8821A)     /* Hawking HD65U */ \
    X(0x2001, 0x3314, WLAN_FAMILY_RTL8821A)     /* D-Link DWA-171 rev B1 */ \
    X(0x2001, 0x3318, WLAN_FAMILY_RTL8821A)     /* D-Link DWA-172 */ \
    X(0x2019, 0xAB32, WLAN_FAMILY_RTL8821A)     /* Planex GW-450S */ \
    X(0x2357, 0x011E, WLAN_FAMILY_RTL8821A)     /* TP-Link Archer T2U Nano */ \
    X(0x2357, 0x011F, WLAN_FAMILY_RTL8821A)     /* TP-Link Archer T2U Nano */ \
    X(0x2357, 0x0120, WLAN_FAMILY_RTL8821A)     /* TP-Link Archer T2U Plus */ \
    X(0x2357, 0x0122, WLAN_FAMILY_RTL8821A)     /* TP-Link Archer T2U Nano */ \
    X(0x3823, 0x6249, WLAN_FAMILY_RTL8821A)     /* Obihai OBiWiFi */ \
    X(0x7392, 0xA811, WLAN_FAMILY_RTL8821A)     /* Edimax EW-7811UTC */ \
    X(0x7392, 0xA812, WLAN_FAMILY_RTL8821A)     /* Edimax EW-7811UTC */ \
    X(0x7392, 0xA813, WLAN_FAMILY_RTL8821A)     /* Edimax EW-7811UAC */ \
    X(0x7392, 0xB611, WLAN_FAMILY_RTL8821A)     /* Edimax EW-7611UCB */ \
    /* RTL8811CU / RTL8821CU */ \
    X(0x0BDA, 0x2006, WLAN_FAMILY_RTL8821C)     /* Realtek reference */ \
    X(0x0BDA, 0x8731, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0x8811, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xB820, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xB82B, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC80C, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC811, WLAN_FAMILY_RTL8821C)     /* Most generic AC600 sticks */ \
    X(0x0BDA, 0xC820, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC821, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC82A, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC82B, WLAN_FAMILY_RTL8821C) \
    X(0x0BDA, 0xC82C, WLAN_FAMILY_RTL8821C) \
    X(0x2001, 0x331D, WLAN_FAMILY_RTL8821C)     /* D-Link DWA-171 rev D1 */ \
    X(0x7392, 0xC811, WLAN_FAMILY_RTL8821C)     /* Edimax EW-7811ULC */ \
    X(0x7392, 0xD811, WLAN_FAMILY_RTL8821C)     /* Edimax EW-7811UTC */ \
    /* RTL8192EU */ \
    X(0x0BDA, 0x818B, WLAN_FAMILY_RTL8192E)     /* Realtek reference, most generic 300M sticks */ \
    X(0x0BDA, 0x818C, WLAN_FAMILY_RTL8192E) \
    X(0x2001, 0x3319, WLAN_FAMILY_RTL8192E)     /* D-Link DWA-131 rev E1 */ \
    X(0x2019, 0xAB33, WLAN_FAMILY_RTL8192E)     /* Planex GW-300S-KATANA */ \
    X(0x2357, 0x0107, WLAN_FAMILY_RTL8192E)     /* TP-Link TL-WN821N v5 */ \
    X(0x2357, 0x0108, WLAN_FAMILY_RTL8192E)     /* TP-Link TL-WN822N v4 */ \
    X(0x2357, 0x0109, WLAN_FAMILY_RTL8192E)     /* TP-Link TL-WN823N v2 */ \
    X(0x7392, 0xB722, WLAN_FAMILY_RTL8192E)     /* Edimax EW-7722UTn v3 */

#define RTW_ZEROCD_VENDOR       0x0BDA
#define RTW_ZEROCD_PRODUCT      0x1A2B

#endif
