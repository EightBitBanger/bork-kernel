#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

#include <stdint.h>

//
// Standard USB wire structures (USB 2.0 chapter 9, hub class chapter 11)
//

// bmRequestType
#define USB_DIR_OUT             0x00
#define USB_DIR_IN              0x80
#define USB_TYPE_STANDARD       0x00
#define USB_TYPE_CLASS          0x20
#define USB_TYPE_VENDOR         0x40
#define USB_RECIP_DEVICE        0x00
#define USB_RECIP_INTERFACE     0x01
#define USB_RECIP_ENDPOINT      0x02
#define USB_RECIP_OTHER         0x03    // Hub port

// Standard requests
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_SET_HUB_DEPTH       0x0C    // USB 3 hubs

// Descriptor types
#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIGURATION      0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05
#define USB_DESC_HUB                0x29
#define USB_DESC_SS_HUB             0x2A

// Class codes
#define USB_CLASS_PER_INTERFACE     0x00
#define USB_CLASS_AUDIO             0x01
#define USB_CLASS_COMM              0x02
#define USB_CLASS_HID               0x03
#define USB_CLASS_PRINTER           0x07
#define USB_CLASS_MASS_STORAGE      0x08
#define USB_CLASS_HUB               0x09
#define USB_CLASS_VIDEO             0x0E
#define USB_CLASS_WIRELESS          0xE0
#define USB_CLASS_MISC              0xEF
#define USB_CLASS_VENDOR            0xFF

// Hub class features (port)
#define USB_PORT_FEAT_CONNECTION    0
#define USB_PORT_FEAT_ENABLE        1
#define USB_PORT_FEAT_RESET         4
#define USB_PORT_FEAT_POWER         8
#define USB_PORT_FEAT_C_CONNECTION  16
#define USB_PORT_FEAT_C_ENABLE      17
#define USB_PORT_FEAT_C_SUSPEND     18
#define USB_PORT_FEAT_C_OVER_CURRENT 19
#define USB_PORT_FEAT_C_RESET       20

// Hub port status (wPortStatus) and change (wPortChange) bits
#define USB_PORT_STAT_CONNECTION    0x0001
#define USB_PORT_STAT_ENABLE        0x0002
#define USB_PORT_STAT_RESET         0x0010
#define USB_PORT_STAT_POWER         0x0100
#define USB_PORT_STAT_LOW_SPEED     0x0200
#define USB_PORT_STAT_HIGH_SPEED    0x0400
#define USB_PORT_STAT_C_CONNECTION  0x0001
#define USB_PORT_STAT_C_RESET       0x0010

struct __attribute__((packed)) UsbSetupPacket {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

struct __attribute__((packed)) UsbDeviceDescriptor {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;       // BCD, 0x0200 = USB 2.0
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet0;       // Endpoint 0 max packet (USB 3: 2^n)
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t  manufacturer_index;
    uint8_t  product_index;
    uint8_t  serial_index;
    uint8_t  configuration_count;
};

struct __attribute__((packed)) UsbConfigDescriptor {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;      // This descriptor plus every one that follows it
    uint8_t  interface_count;
    uint8_t  configuration_value;
    uint8_t  configuration_index;
    uint8_t  attributes;
    uint8_t  max_power;         // 2 mA units (8 mA for SuperSpeed)
};

struct __attribute__((packed)) UsbInterfaceDescriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  endpoint_count;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  interface_index;
};

struct __attribute__((packed)) UsbEndpointDescriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  address;           // Bit 7: IN
    uint8_t  attributes;        // Bits 1:0: control / iso / bulk / interrupt
    uint16_t max_packet;
    uint8_t  interval;
};

struct __attribute__((packed)) UsbHubDescriptor {
    uint8_t  length;
    uint8_t  type;
    uint8_t  port_count;
    uint16_t characteristics;   // Bits 6:5: TT think time
    uint8_t  power_on_to_good;  // 2 ms units
    uint8_t  control_current;
    uint8_t  variable[];        // DeviceRemovable, PortPwrCtrlMask
};

struct __attribute__((packed)) UsbPortStatus {
    uint16_t status;
    uint16_t change;
};

#endif
