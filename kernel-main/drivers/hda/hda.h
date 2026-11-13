#ifndef _KERNEL_DRIVERS_HDA_H_
#define _KERNEL_DRIVERS_HDA_H_

#include <stdint.h>
#include <stdbool.h>

#include <kernel/arch/x86/drivers/audio/audio.h>
#include <kernel/arch/x86/drivers/audio/hda_codec_driver.h>

//
// Intel High Definition Audio, shared between the controller (hda.c) and
// the codec side (hda_codec.c). Not for anything else in the kernel:
// programs reach it through /dev/audioN (audio.h).
//

#define HDA_MAX_WIDGETS     96      // Node IDs below this are kept (the ALC269 ends at 0x24)
#define HDA_MAX_CONN        16      // Connection list entries kept per widget
#define HDA_MAX_PATH        6       // Nodes from a pin back to its DAC
#define HDA_MAX_OUTPUTS     4       // Pins driven at once
#define HDA_MAX_INPUTS      4       // Microphone / line in pins kept

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

// One input pin and the widgets from an ADC to it: nid[0] the ADC,
// nid[length-1] the pin, sel[i] the connection index nid[i] takes to reach
// nid[i+1]. Only one is recorded at a time.
struct HdaInPath {
    uint8_t  length;
    uint8_t  nid[HDA_MAX_PATH];
    uint8_t  sel[HDA_MAX_PATH];
    uint8_t  kind;              // AUDIO_IN_*
    uint8_t  gain_nid;          // Widget whose input amp is the capture gain (0: none)
    uint8_t  gain_index;        // Which of its inputs
    bool     detect;            // A jack that can tell if something is plugged in
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

    struct HdaInPath in[HDA_MAX_INPUTS];
    uint8_t  in_count;
    uint8_t  inputs;            // AUDIO_IN_* found
    uint8_t  mic_present;       // Microphone jack: 1, 0, AUDIO_JACK_UNKNOWN
    uint32_t in_rates;          // What the ADCs on those paths share
    uint32_t in_bits;
    bool     in_has_gain;

    char     name[32];
    const char* why;            // Why hda_codec_parse said no

    // The controller it is on (hda.c sets these after hda_codec_read)
    uint16_t pci_vendor, pci_device, pci_subvendor, pci_subdevice;

    // A codec driver (hda_codec_driver.h) that claimed it, or NULL: the
    // generic setup only. flags are its HDA_CODEC_*.
    const struct HdaCodecDriver* drv;
    uint32_t flags;
    uint32_t pins_changed;      // Pin defaults its pins hook replaced
    uint8_t  lo_present;        // Line out jack (HDA_CODEC_LINE_MUTES_SPEAKER): 1, 0, AUDIO_JACK_UNKNOWN
};

// Reads the codec at `addr`: function group, widgets, outputs and paths.
// False when it has no audio function group or no analog output (an HDMI
// codec): nothing was changed on it then. hda_codec_read and then
// hda_codec_route, with no codec driver.
bool hda_codec_parse(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr);

// The first half: identity, function group and every widget. False (and
// c->why) when there is no audio function group to work with.
bool hda_codec_read(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c, uint8_t addr);

// Whether codec driver `d` claims the codec hda_codec_read read
bool hda_codec_driver_match(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                            const struct HdaCodecDriver* d);

// The second half: c->drv's pins hook (if any) on the widgets read, then
// the output and input paths from them. Sends no verbs except what the
// hook sends. False (and c->why) when there is no analog output.
bool hda_codec_route(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// c->drv's release hook, before it is unregistered
void hda_codec_driver_release(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// Powers up and routes every path, a codec driver's init hook first
void hda_codec_setup(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// Points every DAC on the paths at stream `tag` with `format` (0 tag: off)
void hda_codec_stream(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t tag, uint16_t format);

// Output amplifier on every path: 0..100 a side, or muted
void hda_codec_volume(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      uint8_t left, uint8_t right, bool mute);

// Headphones in or out: speakers off while they are in (and while a line
// out is plugged in, with HDA_CODEC_LINE_MUTES_SPEAKER). True if it changed.
bool hda_codec_jack(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// Recording: path `index` routed to its ADC and the ADC pointed at stream
// `tag` with `format`; index -1 (or tag 0) takes every input path down
// again (ADC off, pin bias off, its mixer input muted)
void hda_codec_capture(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                       int index, uint8_t tag, uint16_t format);

// The input to record: a microphone plugged into a jack that can tell,
// else the built-in microphone, else the first one found. -1: none.
// Also updates c->mic_present.
int hda_codec_capture_pick(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c);

// Capture gain 0..100 on path `index`, or muted (silence recorded)
void hda_codec_capture_gain(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                            int index, uint8_t level, bool mute);

// The capture gain level that is 0 dB on path `index` (what to start at)
uint8_t hda_codec_capture_default_gain(const struct HdaCodec* c, int index);

// One line per path, and the pins, into `out` (appended at *used); the
// codec driver's own lines too when hc / verb are given
void hda_codec_report(struct HdaController* hc, hda_verb_fn verb, struct HdaCodec* c,
                      char* out, uint32_t size, uint32_t* used);

#endif
