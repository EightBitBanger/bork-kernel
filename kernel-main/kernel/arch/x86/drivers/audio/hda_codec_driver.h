#ifndef _KERNEL_DRIVERS_HDA_CODEC_DRIVER_H_
#define _KERNEL_DRIVERS_HDA_CODEC_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>

//
// HDA codec drivers: codec knowledge in a driver file, on top of the HDA
// controller driver (itself the driver file sys/drivers/hda, drivers/hda/)
//
// The controller side (reset, CORB/RIRB, the DMA rings, /dev/audioN,
// /dev/micN) is the same on every Intel HDA machine and lives in the hda
// file. What differs from one machine to the next is the codec: which pins
// are really wired, vendor coefficients, which jacks should silence the
// speaker. A codec driver carries that and nothing else. The hda file's
// generic codec code (hda_codec.c) still finds the paths and routes them;
// the hooks below adjust what it sees and add to what it does.
//
// The kernel itself has no sound driver: it loads <home>/sys/drivers/hda
// when it finds an HD Audio controller (driver/autoload.c). This header and
// audio.h stay in the kernel tree because they are the ABI between the
// files and programs.
//
// A loaded driver file (driver/kdrv.h) registers one with
// KernelApi.hda_codec_register (KAPI_VERSION 3) from its probe and takes it
// out again with hda_codec_unregister from its remove. Each time, the HDA
// driver reads the codecs it uses again and sets them up afresh, so a
// stream that is playing keeps playing (with a click) and a codec whose
// driver goes away falls back to the generic setup.
//
// The HDA driver also loads one by itself: once the system disk is mounted
// it looks, for every codec it uses that no codec driver has claimed, for
//
//   <home>/sys/drivers/hda-<vendor id, 8 hex digits>     (hda-10ec0221)
//
// and loads it if it is there and only an admin can change it and its
// folder (sys/ is installed write=admin). One look per codec per boot; the
// console's "load" works any time after.
//
// Rules for this ABI (like kapi.h): fields are only ever added at the end,
// `size` says how much of a struct the other side has, the version goes up
// by one with each addition, and nothing is removed or changed.
//
// Locking: every hook runs with the HDA driver's lock held (and, from
// register / unregister, the driver lock too). Hooks may send verbs and
// sleep a few ms; they must not call hda_codec_register / unregister or
// load drivers. A hook must not keep the HdaCodecLink pointer: it lives
// on the caller's stack.
//

#define HDA_CODEC_DRIVER_VERSION    1
#define HDA_CODEC_LINK_VERSION      1
#define HDA_CODEC_DRIVERS_MAX       4       // Registered at once
#define HDA_CODEC_NAME_MAX          32      // HdaCodecDriver.name, terminator included

// HdaCodecDriver.flags
#define HDA_CODEC_LINE_MUTES_SPEAKER 0x0001u // Speakers off while a line out jack is plugged
                                             // (desktops: the rear green jack), as for headphones
#define HDA_CODEC_HP_MUTES_LINE      0x0002u // Line outs off while headphones are plugged

// HdaCodecLink.verb: `verb` is the ID as the HDA spec writes it. IDs of
// 0xF and below are the 4-bit ones with a 16-bit payload (0x2 set format,
// 0x3 set amp, 0x4 set processing coefficient, 0x5 coefficient index,
// 0xB get amp); the rest are 12-bit (0x707 pin control, 0xF00 parameter
// ...) with an 8-bit payload.
#define HDA_LINK_VERB_FAILED        0xFFFFFFFFu

struct HdaCodecLink {
    uint32_t version;               // HDA_CODEC_LINK_VERSION
    uint32_t size;                  // sizeof(struct HdaCodecLink) of this kernel

    // The codec
    uint32_t vendor_id;             // 0x10EC0221
    uint32_t revision;
    uint32_t subsystem;             // From the function group: PCI subsystem of the board (0x103Cxxxx: HP)
    uint8_t  addr;                  // On the link
    uint8_t  afg;                   // Audio function group node
    uint8_t  first_nid;             // Widgets first_nid .. first_nid + node_count - 1
    uint8_t  node_count;

    // The controller it sits on
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint16_t pci_subvendor;         // The controller's own PCI subsystem (often the board's)
    uint16_t pci_subdevice;

    void*    ctx;                   // The kernel's: pass it back, never look inside

    // One verb to node `nid`; the codec's answer, or HDA_LINK_VERB_FAILED
    uint32_t (*verb)(void* ctx, uint8_t nid, uint32_t verb, uint32_t payload);

    // A pin's configuration default as the generic code sees it (what the
    // BIOS set, or what set_pin_config put there). 0 for no such pin.
    uint32_t (*pin_config)(void* ctx, uint8_t nid);

    // Only in HdaCodecDriver.pins: what the generic code takes as the pin's
    // configuration default from now on. The codec's own register is left
    // as the BIOS set it. False for a node that is not a pin.
    bool     (*set_pin_config)(void* ctx, uint8_t nid, uint32_t config);

    // Whether something is plugged into pin `nid` (jack sense): 1, 0, or
    // 0xFF when the pin can't tell
    uint8_t  (*pin_sense)(void* ctx, uint8_t nid);

    void     (*delay_ms)(uint32_t ms);
};

struct HdaCodecDriver {
    uint32_t version;               // HDA_CODEC_DRIVER_VERSION
    uint32_t size;                  // sizeof(struct HdaCodecDriver) it was built with
    char     name[HDA_CODEC_NAME_MAX];  // What the codec is called: "Realtek ALC221"
    uint32_t flags;                 // HDA_CODEC_*

    // Whether this is a codec it drives. The link's ids are set; the
    // widgets are read, nothing has been routed. Required.
    bool (*match)(const struct HdaCodecLink* link);

    // Before the paths are found: fix pin configuration defaults the BIOS
    // got wrong (set_pin_config). Optional.
    void (*pins)(const struct HdaCodecLink* link);

    // Every time the codec is set up, after the function group is powered
    // and before anything is routed: vendor coefficients, GPIOs. Optional.
    void (*init)(const struct HdaCodecLink* link);

    // At the end of every setup, everything routed and unmuted. Optional.
    void (*ready)(const struct HdaCodecLink* link);

    // Before the codec goes back to the generic setup (the driver is being
    // unregistered): undo what init did, if it matters. Optional.
    void (*release)(const struct HdaCodecLink* link);

    // Extra lines for AUDIO_IOCTL_REPORT: NUL-terminated text, at most
    // `size` bytes with the terminator. Optional.
    void (*report)(const struct HdaCodecLink* link, char* out, uint32_t size);
};

// The HDA controller driver's side (the driver file sys/drivers/hda, built
// from drivers/hda/hda.c). A codec driver file reaches them through
// KernelApi.hda_codec_register / hda_codec_unregister, which the kernel
// passes on to it while it is loaded.
//
// register: false for a table that is not sane (no match, a name with no
// terminator, an older size), one registered already, or a full table.
// Every codec in use that no driver has claimed and `d` matches is set up
// again with it. Call from probe; the table must stay where it is until
// unregister.
//
// unregister: every codec `d` claimed gets its release hook and is set up
// again without it (another driver's, or the generic setup). Call from
// remove; nothing points into `d` afterwards.
bool hda_codec_register(const struct HdaCodecDriver* d);
void hda_codec_unregister(const struct HdaCodecDriver* d);

#endif
