#ifndef _KERNEL_DRIVERS_HDA_REGS_H_
#define _KERNEL_DRIVERS_HDA_REGS_H_

//
// Intel High Definition Audio: controller registers and codec verbs
//
// Numbers from the HDA specification, revision 1.0a. Only what hda.c and
// hda_codec.c use.
//

//
// Controller registers (offsets into BAR 0)
//

#define HDA_GCAP            0x00    // 16: global capabilities
#define HDA_VMIN            0x02    // 8
#define HDA_VMAJ            0x03    // 8
#define HDA_GCTL            0x08    // 32: global control
#define HDA_WAKEEN          0x0C    // 16
#define HDA_STATESTS        0x0E    // 16: codecs present after reset (RW1C)
#define HDA_INTCTL          0x20    // 32
#define HDA_INTSTS          0x24    // 32
#define HDA_CORBLBASE       0x40    // 32
#define HDA_CORBUBASE       0x44    // 32
#define HDA_CORBWP          0x48    // 16
#define HDA_CORBRP          0x4A    // 16
#define HDA_CORBCTL         0x4C    // 8
#define HDA_CORBSTS         0x4D    // 8
#define HDA_CORBSIZE        0x4E    // 8
#define HDA_RIRBLBASE       0x50    // 32
#define HDA_RIRBUBASE       0x54    // 32
#define HDA_RIRBWP          0x58    // 16
#define HDA_RINTCNT         0x5A    // 16
#define HDA_RIRBCTL         0x5C    // 8
#define HDA_RIRBSTS         0x5D    // 8
#define HDA_RIRBSIZE        0x5E    // 8
#define HDA_ICOI            0x60    // 32: immediate command out
#define HDA_ICII            0x64    // 32: immediate response in
#define HDA_ICIS            0x68    // 16: immediate command status
#define HDA_DPLBASE         0x70    // 32
#define HDA_DPUBASE         0x74    // 32

// GCAP fields
#define HDA_GCAP_OSS(g)     (((g) >> 12) & 0xF)     // Output streams
#define HDA_GCAP_ISS(g)     (((g) >> 8) & 0xF)      // Input streams
#define HDA_GCAP_BSS(g)     (((g) >> 3) & 0x1F)     // Bidirectional streams

#define HDA_GCTL_CRST       (1u << 0)               // 0: controller in reset
#define HDA_GCTL_UNSOL      (1u << 8)               // Accept unsolicited responses

#define HDA_CORBRP_RST      (1u << 15)
#define HDA_CORBCTL_RUN     (1u << 1)
#define HDA_RIRBWP_RST      (1u << 15)
#define HDA_RIRBCTL_DMAEN   (1u << 1)
#define HDA_RIRBCTL_RINTCTL (1u << 0)   // Flag responses in RIRBSTS (no CPU interrupt while INTCTL is 0)
#define HDA_RIRBSTS_INTFL   (1u << 0)
#define HDA_RIRBSTS_OIS     (1u << 2)

// CORBSIZE / RIRBSIZE: capability bits 7:4, size select bits 1:0
#define HDA_RINGSIZE_CAP_256    (1u << 6)
#define HDA_RINGSIZE_CAP_16     (1u << 5)
#define HDA_RINGSIZE_SEL_256    2
#define HDA_RINGSIZE_SEL_16     1
#define HDA_RINGSIZE_SEL_2      0

#define HDA_ICIS_BUSY       (1u << 0)
#define HDA_ICIS_VALID      (1u << 1)

//
// Stream descriptors: input streams first, then output, then bidirectional
//

#define HDA_SD_BASE         0x80
#define HDA_SD_SIZE         0x20

#define HDA_SD_CTL          0x00    // 24 bits: RUN / SRST in byte 0, stream tag in bits 23:20
#define HDA_SD_STS          0x03    // 8: RW1C
#define HDA_SD_LPIB         0x04    // 32: link position in buffer
#define HDA_SD_CBL          0x08    // 32: cyclic buffer length
#define HDA_SD_LVI          0x0C    // 16: last valid BDL index
#define HDA_SD_FIFOS        0x10    // 16: FIFO size
#define HDA_SD_FMT          0x12    // 16: stream format
#define HDA_SD_BDPL         0x18    // 32
#define HDA_SD_BDPU         0x1C    // 32

#define HDA_SD_CTL_SRST     (1u << 0)
#define HDA_SD_CTL_RUN      (1u << 1)
#define HDA_SD_STS_BCIS     (1u << 2)
#define HDA_SD_STS_FIFOE    (1u << 3)
#define HDA_SD_STS_DESE     (1u << 4)
#define HDA_SD_STS_ALL      (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE)

// One buffer descriptor list entry
struct HdaBdlEntry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t flags;         // Bit 0: interrupt on completion
} __attribute__((packed));

//
// Stream / converter format (SD_FMT and verb SET_FORMAT)
//
//   bit 14      base: 0 = 48 kHz, 1 = 44.1 kHz
//   bits 13:11  multiply - 1
//   bits 10:8   divide - 1
//   bits 6:4    bits per sample: 0 = 8, 1 = 16, 2 = 20, 3 = 24, 4 = 32
//   bits 3:0    channels - 1
//

#define HDA_FMT_BASE_44K    (1u << 14)
#define HDA_FMT_MULT(m)     ((uint16_t)(((m) - 1) << 11))
#define HDA_FMT_DIV(d)      ((uint16_t)(((d) - 1) << 8))
#define HDA_FMT_BITS_8      (0u << 4)
#define HDA_FMT_BITS_16     (1u << 4)
#define HDA_FMT_BITS_20     (2u << 4)
#define HDA_FMT_BITS_24     (3u << 4)
#define HDA_FMT_BITS_32     (4u << 4)
#define HDA_FMT_CHAN(c)     ((uint16_t)((c) - 1))

//
// Codec verbs
//
// A command is codec address (31:28), node ID (27:20) and either a 12-bit
// verb with an 8-bit payload or a 4-bit verb with a 16-bit payload.
//

#define HDA_VERB(cad, nid, verb, payload) \
    ((((uint32_t)(cad) & 0xF) << 28) | (((uint32_t)(nid) & 0xFF) << 20) | \
     (((uint32_t)(verb) & 0xFFF) << 8) | ((uint32_t)(payload) & 0xFF))
#define HDA_VERB4(cad, nid, verb, payload) \
    ((((uint32_t)(cad) & 0xF) << 28) | (((uint32_t)(nid) & 0xFF) << 20) | \
     (((uint32_t)(verb) & 0xF) << 16) | ((uint32_t)(payload) & 0xFFFF))

#define HDA_GET_PARAMETER       0xF00
#define HDA_GET_CONN_SELECT     0xF01
#define HDA_SET_CONN_SELECT     0x701
#define HDA_GET_CONN_LIST       0xF02
#define HDA_GET_POWER_STATE     0xF05
#define HDA_SET_POWER_STATE     0x705
#define HDA_GET_STREAM_ID       0xF06
#define HDA_SET_STREAM_ID       0x706
#define HDA_GET_PIN_CTL         0xF07
#define HDA_SET_PIN_CTL         0x707
#define HDA_GET_PIN_SENSE       0xF09
#define HDA_EXEC_PIN_SENSE      0x709
#define HDA_GET_EAPD            0xF0C
#define HDA_SET_EAPD            0x70C
#define HDA_SET_GPIO_DATA       0x715
#define HDA_SET_GPIO_MASK       0x716
#define HDA_SET_GPIO_DIR        0x717
#define HDA_GET_CONFIG_DEFAULT  0xF1C
#define HDA_GET_SUBSYSTEM_ID    0xF20
#define HDA_FUNCTION_RESET      0x7FF

// 4-bit verbs
#define HDA_SET_COEF_INDEX      0x5     // Realtek: vendor coefficient index (NID 0x20)
#define HDA_SET_PROC_COEF       0x4
#define HDA_GET_COEF_INDEX      0xD
#define HDA_GET_PROC_COEF       0xC
#define HDA_SET_FORMAT          0x2
#define HDA_GET_FORMAT          0xA
#define HDA_SET_AMP             0x3
#define HDA_GET_AMP             0xB

// SET_AMP payload
#define HDA_AMP_OUT             (1u << 15)
#define HDA_AMP_IN              (1u << 14)
#define HDA_AMP_LEFT            (1u << 13)
#define HDA_AMP_RIGHT           (1u << 12)
#define HDA_AMP_INDEX(i)        (((uint32_t)(i) & 0xF) << 8)
#define HDA_AMP_MUTE            (1u << 7)
#define HDA_AMP_GAIN(g)         ((uint32_t)(g) & 0x7F)

// GET_AMP payload
#define HDA_GET_AMP_OUT         (1u << 15)
#define HDA_GET_AMP_LEFT        (1u << 13)

// Parameters (GET_PARAMETER payload)
#define HDA_PAR_VENDOR_ID       0x00
#define HDA_PAR_REVISION_ID     0x02
#define HDA_PAR_NODE_COUNT      0x04    // Start NID 23:16, count 7:0
#define HDA_PAR_FG_TYPE         0x05
#define HDA_PAR_AFG_CAPS        0x08
#define HDA_PAR_WIDGET_CAPS     0x09
#define HDA_PAR_PCM             0x0A    // Rates 11:0, bit depths 20:16
#define HDA_PAR_STREAM_FORMATS  0x0B
#define HDA_PAR_PIN_CAPS        0x0C
#define HDA_PAR_AMP_IN_CAPS     0x0D
#define HDA_PAR_CONN_LEN        0x0E
#define HDA_PAR_POWER_STATES    0x0F
#define HDA_PAR_GPIO_COUNT      0x11
#define HDA_PAR_AMP_OUT_CAPS    0x12

#define HDA_FG_TYPE_AUDIO       0x01

// Widget capabilities
#define HDA_WCAP_TYPE(c)        (((c) >> 20) & 0xF)
#define HDA_WCAP_STEREO         (1u << 0)
#define HDA_WCAP_IN_AMP         (1u << 1)
#define HDA_WCAP_OUT_AMP        (1u << 2)
#define HDA_WCAP_AMP_OVERRIDE   (1u << 3)
#define HDA_WCAP_CONN_LIST      (1u << 8)
#define HDA_WCAP_DIGITAL        (1u << 9)
#define HDA_WCAP_POWER          (1u << 10)

#define HDA_WIDGET_OUTPUT       0x0     // DAC
#define HDA_WIDGET_INPUT        0x1     // ADC
#define HDA_WIDGET_MIXER        0x2
#define HDA_WIDGET_SELECTOR     0x3
#define HDA_WIDGET_PIN          0x4
#define HDA_WIDGET_POWER        0x5
#define HDA_WIDGET_VOLUME_KNOB  0x6
#define HDA_WIDGET_BEEP         0x7
#define HDA_WIDGET_VENDOR       0xF

// Amplifier capabilities
#define HDA_AMPCAP_OFFSET(c)    ((c) & 0x7F)
#define HDA_AMPCAP_STEPS(c)     (((c) >> 8) & 0x7F)     // Number of steps - 1
#define HDA_AMPCAP_MUTE         (1u << 31)

// Pin capabilities
#define HDA_PINCAP_TRIGGER      (1u << 1)   // Presence detect needs EXEC_PIN_SENSE
#define HDA_PINCAP_PRESENCE     (1u << 2)
#define HDA_PINCAP_HP_DRIVE     (1u << 3)
#define HDA_PINCAP_OUTPUT       (1u << 4)
#define HDA_PINCAP_INPUT        (1u << 5)
#define HDA_PINCAP_HDMI         (1u << 7)
#define HDA_PINCAP_VREF_50      (1u << 9)   // Bits 15:8: the bias levels it can give
#define HDA_PINCAP_VREF_80      (1u << 12)
#define HDA_PINCAP_VREF_100     (1u << 13)
#define HDA_PINCAP_EAPD         (1u << 16)
#define HDA_PINCAP_DP           (1u << 24)

// Pin widget control
#define HDA_PINCTL_IN           (1u << 5)
#define HDA_PINCTL_OUT          (1u << 6)
#define HDA_PINCTL_VREF_HIZ     0x0     // Bits 2:0: microphone bias
#define HDA_PINCTL_VREF_50      0x1
#define HDA_PINCTL_VREF_80      0x4
#define HDA_PINCTL_VREF_100     0x5
#define HDA_PINCTL_HP           (1u << 7)

#define HDA_EAPD_ENABLE         (1u << 1)
#define HDA_PIN_SENSE_PRESENT   (1u << 31)

// Configuration default
#define HDA_CFG_PORT(c)         (((c) >> 30) & 0x3)
#define HDA_CFG_DEVICE(c)       (((c) >> 20) & 0xF)
#define HDA_CFG_LOCATION(c)     (((c) >> 24) & 0x3F)
#define HDA_CFG_MISC(c)         (((c) >> 8) & 0xF)
#define HDA_CFG_ASSOC(c)        (((c) >> 4) & 0xF)
#define HDA_CFG_SEQ(c)          ((c) & 0xF)

#define HDA_PORT_JACK           0
#define HDA_PORT_NONE           1
#define HDA_PORT_FIXED          2
#define HDA_PORT_BOTH           3

#define HDA_DEV_LINE_OUT        0x0
#define HDA_DEV_SPEAKER         0x1
#define HDA_DEV_HP_OUT          0x2
#define HDA_DEV_LINE_IN         0x8
#define HDA_DEV_MIC_IN          0xA

#define HDA_CFG_MISC_NO_DETECT  (1u << 0)   // Jack detect override

#endif
