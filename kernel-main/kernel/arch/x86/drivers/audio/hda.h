#ifndef _KERNEL_DRIVERS_HDA_H_
#define _KERNEL_DRIVERS_HDA_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/audio/audio.h>

//
// Intel High Definition Audio, shared between the controller (hda.c) and
// the codec side (hda_codec.c). Not for anything else in the kernel:
// programs reach it through /dev/audioN (audio.h).
//

#define HDA_MAX_WIDGETS     96      // Node IDs below this are kept (the ALC269 ends at 0x24)
#define HDA_MAX_CONN        16      // Connection list entries kept per widget
#define HDA_MAX_PATH        6       // Nodes from a pin back to its DAC
#define HDA_MAX_OUTPUTS     4       // Pins driven at once

struct HdaController;

// What the codec side needs from the controller
typedef bool (*hda_verb_fn)(struct HdaController* hc, uint32_t verb, uint32_t* response);

struct HdaWidget {
    uint32_t caps;              // HDA_PAR_WIDGET_CAPS (0: no such node)
    uint32_t pin_caps;
    uint32_t config;            // Configuration default (pins)
    uint32_t amp_in_caps;
    uint32_t amp_out_caps;
    uint32_t pcm;               // HDA_PAR_PCM (DACs)
    uint8_t  conn_count;
    uint8_t  conn[HDA_MAX_CONN];
};

// One pin and the widgets from it to a DAC: nid[0] the pin, nid[length-1]
// the DAC, sel[i] the connection index nid[i] takes to reach nid[i+1]
struct HdaPath {
    uint8_t  length;
    uint8_t  nid[HDA_MAX_PATH];
    uint8_t  sel[HDA_MAX_PATH];
    uint8_t  kind;              // AUDIO_OUT_*
    uint8_t  vol_nid;           // Widget whose output amp is the volume (0: none)
    bool     detect;            // The pin can tell if a jack is plugged in
    bool     enabled;           // Pin output on right now
};

struct HdaCodec {
    uint8_t  addr;              // Codec address on the link
    uint8_t  afg;               // Audio function group node
    uint32_t vendor_id;         // 0x10EC0269
    uint32_t revision;
    uint32_t subsystem;
    uint32_t afg_amp_in_caps;   // Defaults for widgets without their own
    uint32_t afg_amp_out_caps;
    uint32_t afg_pcm;
    uint8_t  first_nid;
    uint8_t  node_count;
    struct HdaWidget w[HDA_MAX_WIDGETS];

    struct HdaPath out[HDA_MAX_OUTPUTS];
    uint8_t  out_count;
    uint8_t  outputs;           // AUDIO_OUT_* found
    uint8_t  hp_present;        // 1, 0, AUDIO_JACK_UNKNOWN
    uint32_t rates;             // Shared by every DAC used
    uint32_t bits;              // PCM bit depths (HDA_PAR_PCM 20:16)
    bool     has_volume;
    char     name[32];
    const char* why;            // Why hda_codec_parse said no
};

// Reads the codec at `addr`: function group, widgets, outputs and paths.
// False when it has no audio function group or no analog output (an HDMI
// codec): nothing was changed on it then.
bool hda_codec_parse(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr);

// Powers up and routes every path, vendor fixes first (the ALC269's)
void hda_codec_setup(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// Points every DAC on the paths at stream `tag` with `format` (0 tag: off)
void hda_codec_stream(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t tag, uint16_t format);

// Output amplifier on every path: 0..100 a side, or muted
void hda_codec_volume(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t left, uint8_t right, bool mute);

// Headphones in or out: speakers off while they are in. True if it changed.
bool hda_codec_jack(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// One line per path, and the pins, into `out` (appended at *used)
void hda_codec_report(const struct HdaCodec* c, char* out, uint32_t size, uint32_t* used);

#endif
