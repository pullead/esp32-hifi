# HiFi Player UI — 320×170 design spec (v3, dark neon)

> **v3 supersedes v1/v2.** Product direction is now the dark-neon reference
> set the user provided (glossy vinyl + neon spectrum ring + flip clock).
> The earlier light/lavender v2 is dropped. This spec is the extraction of
> that reference, adapted to 320×170 — produced with the taste-skill /
> image-to-code / soft-skill methodology (reference image = source of truth;
> deep extraction; faithful implementation).
> Visual target: `docs/ui_preview_v3.html` (4× to-scale preview).

## Design read (taste-skill step 0)
High-end portable Hi-Fi player, for audiophiles, in a **dark neon** visual
language (near-black navy ground, violet/magenta neon, glossy black vinyl,
spectrum ring), delivered in LVGL on a 320×170 landscape panel.
Dials: VARIANCE 6 · MOTION 4 (perf-bounded) · DENSITY 5.

## The 320×170 reality (honest adaptation, not a downgrade)
The references are drawn ~1712 px wide — 28× our pixel budget. Pixel-identical
is not physical; instead we keep the **identity** and adapt density:
- Status bar: the reference has 7 groups (time · Wi-Fi · 在线 · DAC/PCM · 耳放
  · 音质 · volume). At 320×170 that's illegible. Keep **4**: time, Wi-Fi,
  one audio-state tag (I2S/DAC + rate), volume bars. The rest surfaces on the
  Audio-settings page where it belongs.
- **Vinyl = static baked decoration ring + dynamic circular cover:**
  - Outer glossy vinyl grooves + neon glow ring are **baked once as an image
    asset** (rendered offline, blitted cheap).
  - The **center is a dynamic circular cover image**: local music → embedded
    cover art (ID3/FLAC picture) or folder cover, decoded + scaled + circle-
    masked; radio → the **station logo fetched over the network** (MiniWebRadio
    stream metadata usually carries a logo URL, e.g. `somafm.com/logos/...jpg`).
  - The disc **rotates slowly** (LVGL image transform, low fps ~10, paused when
    audio is paused).
  - The **spectrum ring reacts to real audio level** (VU from the audio lib;
    per-band FFT via esp-dsp is a later upgrade).
  Rotating an 84px image + reactive spectrum costs more than a fully static
  disc, so rotation fps and spectrum fps are both capped to protect audio.
- Chinese text needs a **subsetted CJK font** (only the glyphs we use), built
  from the Noto Sans SC TTF already in the repo.

## Color tokens (extracted from the references)
| Token | Hex | Use |
| --- | --- | --- |
| `bg` | `#0A0B12` | screen ground (near-black, slight navy bias) |
| `bg-panel` | `#12141F` → `#0D0F18` grad | rounded content panels |
| `statusbar` | `#05060A` | top strip (near pure black) |
| `ink` | `#F2F3F7` | primary text / active icons |
| `ink-dim` | `#9AA0B4` | secondary text |
| `ink-faint` | `#565C70` | dividers, inactive |
| `accent` | `#A855F7` | neon violet — active/glow, progress fill, play ring |
| `accent-bright` | `#C77DFF` | highlights, focused list border |
| `accent-deep` | `#6D28D9` | gradient low end, pressed |
| `magenta` | `#D946EF` | spectrum hot end only |
| `ok` | `#34D399` | "ON" / connected status (green) |
| `spectrum` | `#6366F1`→`#A855F7`→`#D946EF` | bar/ring gradient (blue→violet→magenta) |
Panels carry a 1px `rgba(168,85,247,.18)` border; the active element gets a
soft `accent` glow (LVGL: shadow_color=accent, shadow_width 8–14).

## Type
- Flip-clock digits: heavy, ~40–48px cap height, `ink`.
- Screen/track title: bold, 16–18px (CJK), `ink`, ellipsis single line.
- Subtitle/artist: 12–13px, `ink-dim`.
- Tech readout (codec·rate·bit): 10–11px, tabular, `ink-dim`, `·`/`|` separators.
- Section labels / nav: 11–12px.

## Screens (all share the 20px status bar + 150px body)
1. **Home** — left: flip clock (12:30) + 周二 08 Jul + weather 26℃. right:
   正在播放 card = mini vinyl + title/artist + progress + prev/play/next.
   bottom: 5 nav cards 播放 / 电台 / 闹钟→**解码** / 天气 / 设置 (active = 播放,
   purple fill+glow). (Per user: alarm removed; 播放→本地音乐 list is a later change.)
2. **Now Playing** — back chevron; big vinyl+moon (baked) with animated
   spectrum ring; title 夜空中最亮的星 / 逃跑计划; 当前节目 dot; tech line;
   progress + times; control bar 音效 · ◀ · ⏸(glow ring) · ▶ · 收藏.
3. **Radio list / Local music** — top tabs (播放列表 / 电台列表 or 歌曲/专辑/
   艺术家/文件夹); rows = icon/art + title + sub + state; active row = accent
   glow border; docked mini-player at the bottom.
4. **Audio / EQ settings** (replaces alarm) — left column: 输出模式(耳放/线路),
   增益, 音质模式, 数字滤波, 声道平衡; right: 5-band EQ (60/250/1K/4K/12K)
   vertical sliders + presets 关闭/流行/摇滚/爵士/自定义.

## Animation budget (perf-bounded; MOTION dial = 4)
- Page transition: 150ms horizontal slide+fade (`lv_scr_load_anim`).
- Spectrum ring/bars: cap 12–15 fps, own draw pass, only while playing + visible.
- Vinyl: optional slow rotate of the baked disc (cheap transform), pausable.
- Flip clock: 300ms flip only on minute change.
- Play/pause: 120ms glyph morph + ring glow pulse.
- Press feedback on every control (already in code).
- Out of scope: full-screen blur, parallax, per-frame gradient recompute.
