// Project-supplied TinyUSB configuration, required by managed component
// espressif/tinyusb (see platformio.ini's custom_component_add) -- that
// component ships no config of its own, every consumer must provide one.
//
// This is adapted from espressif/esp32-arduino-lib-builder's
// components/arduino_tinyusb/include/tusb_config.h -- the actual reference
// tusb_config.h that arduino-esp32's C++ wrapper (USB.h, USBCDC.cpp,
// HWCDC.cpp, esp32-hal-tinyusb.c) was written against when it ships as part
// of the official prebuilt esp32-arduino-libs. This project builds
// arduino-esp32 from source via the espressif/tinyusb managed component
// instead, which doesn't compile the USB *host* stack (no host/*.c in its
// CMakeLists SRCS) or mtp/printer/bth/usbtmc/ecm_rndis, so those bits from
// the upstream file are trimmed/hardcoded off here rather than copied.
//
// Placed in src/ (not include/) because src/CMakeLists.txt injects this
// directory into espressif__tinyusb's PUBLIC include dirs via
// target_include_directories(tusb_lib PUBLIC ...) -- PUBLIC so the include
// path also reaches arduino-esp32 (which depends on tinyusb per its own
// idf_component.yml manifest, so it gets tinyusb's public includes too).

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*         */
/* KCONFIG */
/*         */
// Fallback defaults for symbols declared in src/Kconfig.projbuild, in case
// this header is ever processed before sdkconfig.h picks them up.

#ifndef CONFIG_TINYUSB_CDC_ENABLED
#define CONFIG_TINYUSB_CDC_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_MSC_ENABLED
#define CONFIG_TINYUSB_MSC_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_HID_ENABLED
#define CONFIG_TINYUSB_HID_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_MIDI_ENABLED
#define CONFIG_TINYUSB_MIDI_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_VENDOR_ENABLED
#define CONFIG_TINYUSB_VENDOR_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_DFU_ENABLED
#define CONFIG_TINYUSB_DFU_ENABLED 0
#endif
#ifndef CONFIG_TINYUSB_DFU_RT_ENABLED
#define CONFIG_TINYUSB_DFU_RT_ENABLED 0
#endif
// USBCDC.cpp references this raw Kconfig symbol directly (not the CFG_TUD_
// prefixed one) for its RX scratch buffer size.
#ifndef CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define CONFIG_TINYUSB_CDC_RX_BUFSIZE 64
#endif

#if CONFIG_TINYUSB_ENABLED
#define CFG_TUD_ENABLED 1
#endif

/*                      */
/* COMMON CONFIGURATION */
/*                      */

// CFG_TUSB_MCU is set via espressif/tinyusb's own CMakeLists
// (target_compile_options(${COMPONENT_LIB} PUBLIC "-DCFG_TUSB_MCU=...")).
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (set via espressif tinyusb component CMakeLists compile_options)
#endif

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#define BOARD_TUD_RHPORT      0

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED
#define BOARD_TUD_MAX_SPEED CFG_TUD_MAX_SPEED

/*                      */
/* DEVICE CONFIGURATION */
/*                      */

#define CFG_TUD_ENDPOINT0_SIZE 64

// arduino-esp32's esp32-hal-tinyusb.c references this misspelled macro name
// (missing the "P" -- compare its own CFG_TUD_ENDOINT_SIZE typo a few lines
// up in esp32-hal-tinyusb.h) instead of the standard CFG_TUD_ENDPOINT0_SIZE.
// Alias it here rather than patching framework source.
#define CFG_TUD_ENDOINT0_SIZE CFG_TUD_ENDPOINT0_SIZE

// Enabled drivers -- only MSC is actually used by this project (USB drive
// export); CDC is force-compiled (but never instantiated at runtime, see
// TINYUSB_CDC_ENABLED above) purely so arduino-esp32's own headers resolve.
// The rest are classes espressif/tinyusb's CMakeLists compiles unconditionally
// (cdc/hid/midi/midi2/msc/mtp/printer/vendor/audio/video/bth/usbtmc/dfu/
// dfu_rt/ecm_rndis/ncm are all always in its SRCS list) so they all need a
// CFG_TUD_* value even when off.
#if CONFIG_TINYUSB_CDC_ENABLED
#define CFG_TUD_CDC 1
#else
#define CFG_TUD_CDC 0
#endif
#define CFG_TUD_MSC         CONFIG_TINYUSB_MSC_ENABLED
#define CFG_TUD_HID         CONFIG_TINYUSB_HID_ENABLED
#define CFG_TUD_MIDI        CONFIG_TINYUSB_MIDI_ENABLED
#define CFG_TUD_MIDI2       0
#define CFG_TUD_VENDOR      CONFIG_TINYUSB_VENDOR_ENABLED
#define CFG_TUD_DFU         CONFIG_TINYUSB_DFU_ENABLED
#define CFG_TUD_DFU_RUNTIME CONFIG_TINYUSB_DFU_RT_ENABLED
#define CFG_TUD_AUDIO       0
#define CFG_TUD_VIDEO       0
#define CFG_TUD_BTH         0
#define CFG_TUD_USBTMC      0
#define CFG_TUD_MTP         0
#define CFG_TUD_PRINTER     0
#define CFG_TUD_ECM_RNDIS   0
#define CFG_TUD_NCM         0

// Buffer sizes -- only meaningful for enabled classes, but the macro must
// resolve to *some* integer even for disabled ones (their .c files still
// reference it inside `#if CFG_TUD_X` guarded code that some compilers
// still parse tokens of before discarding).
#define CFG_TUD_CDC_RX_BUFSIZE CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE 64
#define CFG_TUD_MSC_EP_BUFSIZE 512
#define CFG_TUD_HID_BUFSIZE    64
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#define CFG_TUD_VENDOR_TX_BUFSIZE 64
#define CFG_TUD_DFU_XFER_BUFSIZE  4096

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
