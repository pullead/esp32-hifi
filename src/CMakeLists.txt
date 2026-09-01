# src/lv_conf.h must be visible to other ESP-IDF components (lib/lvgl is
# built as its own component under this arduino+espidf framework combo).
# LV_CONF_INCLUDE_SIMPLE=1 makes LVGL do a bare `#include "lv_conf.h"`,
# which only resolves if this component publishes its own directory as a
# public include path -- PlatformIO's auto-generated version of this file
# (no INCLUDE_DIRS) doesn't do that, so a fresh checkout fails with
# "lv_conf.h: No such file or directory" from inside lib/lvgl.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# espressif__esp-dsp: usb_dac.cpp 的 usbDacGetSpectrum() 用它做 512 点 FFT
# （声卡模式拿不到 Audio::getSpectrumBands()，见那边的注释）。组件本来就在
# managed_components 里，但 ESP-IDF 只沿 REQUIRES 边共享 include 路径，
# 不声明这条边的话 #include "esp_dsp.h" 找不到。
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "." PRIV_REQUIRES espressif__tinyusb espressif__esp-dsp)

# espressif/tinyusb (pulled in via platformio.ini's custom_component_add)
# requires the *consumer* to point it at a project-supplied tusb_config.h --
# it has no INCLUDE_DIRS of its own that would pick up src/tusb_config.h
# automatically (ESP-IDF only shares public include dirs along REQUIRES
# edges, and tinyusb doesn't require this component). This is the exact
# pattern documented in managed_components/espressif__tinyusb/README.md.
# PRIV_REQUIRES above guarantees tinyusb's idf_component_register() already
# ran (and so COMPONENT_LIB is queryable) by the time this line executes.
#
# PUBLIC (not PRIVATE): arduino-esp32's own esp32-hal-tinyusb.c/USB.cpp also
# `#include "tusb.h"` and need this same path. arduino-esp32's
# idf_component.yml declares espressif/tinyusb as a manifest dependency, so
# the component manager wires a REQUIRES edge from arduino-esp32 to tinyusb
# -- PUBLIC here rides that edge and reaches arduino-esp32's sources too.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
