#pragma once
#include <stdint.h>

// GBK (lead 0x81-0xFE, trail 0x40-0x7E/0x80-0xFE, 126*190=23940 entries) ->
// Unicode codepoint table. index = (lead-0x81)*190 + (trail<0x7F ? trail-0x40
// : trail-0x41). See docs/gbk-table/ for the generator.
extern const uint16_t gbk_table[23940];
