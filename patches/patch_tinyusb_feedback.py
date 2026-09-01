# pylint: disable = I,E,R,W,C,F
# ---------------------------------------------------------------------------
# PlatformIO 构建前补丁：修正 espressif/tinyusb 里 UAC2 反馈端点的数据格式。
#
# 背景（2026-09-05 排查 USB 声卡模式「声音断断续续」时定位）：
#
# USB 2.0 规范 5.12.4.2 规定，等时同步反馈值 Ff 的编码格式由**总线速度**决定：
#   全速端点 -> 10.14 定点数，3 字节
#   高速端点 -> 16.16 定点数，4 字节
#
# 但 TinyUSB 的 audiod_fb_send() 是按 **UAC 版本** 选的：
#   uac_version == 1 -> 10.14 / 3 字节
#   否则（UAC2）     -> 16.16 / 4 字节
#
# 在 UAC2 设备普遍跑高速的前提下这个近似成立，可 ESP32-S3 的 USB-OTG
# **只有全速**。于是全速 UAC2 会发出 4 字节的 16.16，而主机按全速规矩去解析
# 前 3 字节：
#
#   value = 3145728  (48 采样/帧, 16.16)  ->  小端字节 00 00 30 00
#   主机读前 3 字节 0x300000 / 16384 = 192 采样/帧   <-- 真值的 4 倍
#
# 主机被告知「每帧给我 192 个采样」，这个数荒谬到它只能不停重新协商音频流：
# 反复 set alt=1 -> 设备侧每次都清空环形缓冲 -> 声音断续、屏幕上状态狂闪、
# 缓冲水位涨到 20% 就归零。注意欠载/溢出计数全程是 0 —— 因为根本没走到欠载，
# 是被主机的重新协商反复打断的。
#
# 配套改动在 src/usb_dac.cpp：反馈端点描述符的 wMaxPacketSize 必须是 3 而不是 4。
#
# ⚠️ 这个补丁必须留在这里：managed_components/ 在 .gitignore 里，直接改那份
# 源码既不会被提交，也会在组件管理器重装依赖时被抹掉 —— 而症状（声音断续）
# 和原因（一个字节格式）相距太远，丢了之后几乎不可能再查一遍。
#
# 如果哪天上游按总线速度而不是 UAC 版本来选格式了，就可以删掉本脚本及其
# 在 platformio.ini extra_scripts 里的那一行。
# ---------------------------------------------------------------------------
import os
import re

Import("env")  # type: ignore

audio_c = os.path.join(
    env.subst("$PROJECT_DIR"),  # type: ignore
    "managed_components", "espressif__tinyusb", "src", "class", "audio", "audio_device.c",
)

MARKER = "mwr_use_10_14"

# 用正则而不是精确字符串：锚点太脆的话，上游任何一点空白改动都会让补丁静默
# 失效，而失效的症状（声音断续）跟"补丁没打上"看起来毫无关系，极难联想。
ANCHOR_DECL = re.compile(
    r"(uint8_t uac_version = tud_audio_n_version\(func_id\);\s*\n)"
    r"(\s*// Format the feedback value\s*\n)"
    r"(\s*)if \(uac_version == 1\) \{"
)
REPL_DECL = (
    r"\1"
    "  // ESP-HiFi 补丁：反馈格式按**总线速度**选，不是 UAC 版本。\n"
    "  // USB 2.0 规范 5.12.4.2：全速端点用 10.14(3 字节)，高速才是 16.16(4 字节)。\n"
    "  // ESP32-S3 的 USB 只有全速，照原来按 UAC 版本选会发出 4 字节 16.16，\n"
    "  // 主机解析成 192 采样/帧（真值 48 的 4 倍），于是不停重新协商音频流。\n"
    "  const bool mwr_use_10_14 = (uac_version == 1) || (tud_speed_get() == TUSB_SPEED_FULL);\n"
    r"\2\3"
    "if (mwr_use_10_14) {"
)

OLD_XFER = "(uint8_t *) audio->fb_buf, uac_version == 1 ? 3 : 4, is_isr);"
NEW_XFER = "(uint8_t *) audio->fb_buf, mwr_use_10_14 ? 3 : 4, is_isr);"

if not os.path.isfile(audio_c):
    print("[patch_tinyusb_feedback] audio_device.c 还没就位，跳过: %s" % audio_c)
else:
    with open(audio_c, "r", encoding="utf-8") as fh:
        src = fh.read()
    if MARKER in src:
        print("[patch_tinyusb_feedback] 已打过补丁，无需处理")
    elif not ANCHOR_DECL.search(src) or OLD_XFER not in src:
        print("[patch_tinyusb_feedback] 警告：没找到锚点，TinyUSB 的 audiod_fb_send() "
              "布局可能变了 —— 补丁**未**应用，USB 声卡模式的声音会断续")
    else:
        src = ANCHOR_DECL.sub(REPL_DECL, src, count=1).replace(OLD_XFER, NEW_XFER, 1)
        with open(audio_c, "w", encoding="utf-8") as fh:
            fh.write(src)
        print("[patch_tinyusb_feedback] 全速 UAC2 反馈格式已修正为 10.14 / 3 字节")
