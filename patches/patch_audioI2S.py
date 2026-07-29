# pylint: disable = I,E,R,W,C,F
# ---------------------------------------------------------------------------
# PlatformIO pre-build patch for schreibfaul1/ESP32-audioI2S.
#
# This project's main.cpp calls two Audio:: methods that exist only in a
# locally-modified copy of the library (added directly to a vendored/cached
# copy on the original dev machine, never captured in a fork or committed
# patch anywhere -- discovered 2026-07-29 when a fresh checkout on another
# machine failed to build against the plain upstream repo):
#
#   uint32_t Audio::getAudioDataStartOffset()      -- used by playerCoreSeekTo()
#   void     Audio::getSpectrumBands(uint8_t* out) -- used by playerCoreReadSnapshot()
#
# Both are thin exposures of state the library already tracks/computes
# internally -- no new logic:
#   - m_audioDataStart (private, already maintained across every codec path)
#     is exactly "byte offset of the first audio frame".
#   - m_fft_items.spectrum[FFT_BANDS] (private) is the already-computed,
#     already-0..255-normalized FFT output from processSpectrum() when
#     settings.SPECTRUM is enabled.
#
# This is applied to the freshly fetched library copy under .pio/libdeps on
# every build so it survives a `.pio` wipe or a library re-clone. Remove
# this script (and its extra_scripts entry) if these methods get added
# upstream, or if the original locally-modified library is ever recovered
# and vendored/forked properly instead.
# ---------------------------------------------------------------------------
import os
import re

Import("env")  # type: ignore

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")  # type: ignore
pioenv = env["PIOENV"]                        # type: ignore
audio_h = os.path.join(libdeps, pioenv, "ESP32-audioI2S", "src", "Audio.h")

MARKER = "getAudioDataStartOffset"

ADDITION = """    uint32_t         getAudioDataStartOffset() { return m_audioDataStart; } // byte offset of the first audio frame (after ID3/WAV/FLAC headers), for byte-linear seek math
    void             getSpectrumBands(uint8_t* outBands) { if (outBands) memcpy(outBands, m_fft_items.spectrum, m_fft_items.BANDS); } // copies the already-computed 0..255 FFT band levels (settings.SPECTRUM must be true)
"""

ANCHOR = re.compile(r"(\n\s*uint16_t\s+getVUlevel\(\);\s*\n)")

if not os.path.isfile(audio_h):
    print("[patch_audioI2S] Audio.h not present yet, skipping: %s" % audio_h)
else:
    with open(audio_h, "r", encoding="utf-8") as fh:
        src = fh.read()
    if MARKER in src:
        print("[patch_audioI2S] Already patched, nothing to do")
    else:
        patched, n = ANCHOR.subn(r"\1" + ADDITION, src, count=1)
        if n == 0:
            print("[patch_audioI2S] WARNING: anchor (getVUlevel declaration) not found -- "
                  "library layout may have changed, patch NOT applied")
        else:
            with open(audio_h, "w", encoding="utf-8") as fh:
                fh.write(patched)
            print("[patch_audioI2S] Added getAudioDataStartOffset() and getSpectrumBands()")
