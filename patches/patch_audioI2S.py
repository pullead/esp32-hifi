# pylint: disable = I,E,R,W,C,F
# ---------------------------------------------------------------------------
# PlatformIO pre-build patch for schreibfaul1/ESP32-audioI2S (v3.4.7, e78a52d)
#
# Upstream bug surfaced by GCC 14 (ESP-IDF 5.5.4 toolchain):
# The base class `Decoder` (src/Audio.h) declares
#       virtual const char* getStreamTitle();
#       virtual const char* whoIsIt();
# as NON-pure virtuals but provides no out-of-line definition. GCC picks the
# first such function as the class "key function" and only emits the vtable in
# the TU that defines it -- which never exists -- so linking fails with:
#       undefined reference to `vtable for Decoder'
#
# Every concrete decoder (aac/flac/mp3/opus/vorbis/wav) already overrides both
# methods, so promoting the base declarations to pure virtual (`= 0`) is safe
# and makes the vtable be emitted where needed. This is applied to the freshly
# fetched library copy under .pio/libdeps on every build so it survives a
# `.pio` wipe or a library re-clone.
#
# Remove this script (and its extra_scripts entry) if upstream fixes the bug.
# ---------------------------------------------------------------------------
import os
import re

Import("env")  # type: ignore

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # type: ignore
pioenv = env["PIOENV"]                        # type: ignore
audio_h = os.path.join(libdeps, pioenv, "ESP32-audioI2S", "src", "Audio.h")

# Match `virtual const char* NAME();` (base decl) but NOT `... NAME() override;`
patterns = [
    re.compile(r"(virtual\s+const\s+char\*\s+getStreamTitle\s*\(\s*\))\s*;"),
    re.compile(r"(virtual\s+const\s+char\*\s+whoIsIt\s*\(\s*\))\s*;"),
]

if not os.path.isfile(audio_h):
    print("[patch_audioI2S] Audio.h not present yet, skipping: %s" % audio_h)
else:
    with open(audio_h, "r", encoding="utf-8") as fh:
        src = fh.read()
    patched = src
    for pat in patterns:
        patched = pat.sub(r"\1 = 0;", patched)
    if patched != src:
        with open(audio_h, "w", encoding="utf-8") as fh:
            fh.write(patched)
        print("[patch_audioI2S] Decoder vtable fix applied (getStreamTitle/whoIsIt -> pure virtual)")
    else:
        print("[patch_audioI2S] Already patched, nothing to do")
