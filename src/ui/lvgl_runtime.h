#pragma once

// Deliberately does not include LVGL.  The MiniWebRadio core can call this
// bridge without pulling LVGL into legacy TFT/font translation units.
bool lvglRuntimeBegin();
void lvglRuntimeTick();
// USB storage mode: switch the (already-built) UI to the USB storage page.
void lvglRuntimeShowUsbStoragePage();
void lvglRuntimeShowUsbDacPage();
