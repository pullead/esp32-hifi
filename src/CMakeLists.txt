# src/lv_conf.h must be visible to other ESP-IDF components (lib/lvgl is
# built as its own component under this arduino+espidf framework combo).
# LV_CONF_INCLUDE_SIMPLE=1 makes LVGL do a bare `#include "lv_conf.h"`,
# which only resolves if this component publishes its own directory as a
# public include path -- PlatformIO's auto-generated version of this file
# (no INCLUDE_DIRS) doesn't do that, so a fresh checkout fails with
# "lv_conf.h: No such file or directory" from inside lib/lvgl.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources} INCLUDE_DIRS ".")
