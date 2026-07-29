#include "lvgl_runtime.h"

#include "hifi_ui.h"

namespace {
HifiUi s_hifiUi;
}

bool lvglRuntimeBegin() {
    return s_hifiUi.begin();
}

void lvglRuntimeTick() {
    s_hifiUi.tick();
}
