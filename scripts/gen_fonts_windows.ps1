param(
    [string]$FontPath = "",
    [string]$LvFontConv = "lv_font_conv",
    [int]$Bpp = 4
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$FontDir = Join-Path $ProjectRoot "src\ui\fonts"
# NotoSansSC-Medium500.ttf is a static wght=500 instance, committed to the
# repo -- NOT the raw variable font. lv_font_conv on a variable font uses
# whatever its *default* named instance is without an explicit weight
# pinned, and this family's default instance is "Thin" (wght=100): every
# font baked straight from a variable NotoSansSC-*.ttf without instancing
# first came out that thin/washed-out by accident. See
# docs/DEV_LOG_2026-07-29_font_preview.md.
$DefaultProjectFont = Join-Path $ProjectRoot "tools\fonts\NotoSansSC-Medium500.ttf"
$DefaultWindowsFont = "C:\Windows\Fonts\NotoSansSC-Medium500.ttf"

if (-not $FontPath) {
    if (Test-Path -LiteralPath $DefaultProjectFont) {
        $FontPath = $DefaultProjectFont
    } elseif (Test-Path -LiteralPath $DefaultWindowsFont) {
        $FontPath = $DefaultWindowsFont
    }
}

if (-not $FontPath -or -not (Test-Path -LiteralPath $FontPath)) {
    throw "Font file not found. Pass -FontPath or place NotoSansSC-VF.ttf in tools\fonts\."
}

if (-not (Get-Command $LvFontConv -ErrorAction SilentlyContinue)) {
    throw "lv_font_conv not found. Install it first, for example: npm install -g lv_font_conv@1.5.3"
}

function Invoke-FontConv {
    param(
        [int]$Size,
        [string]$SymbolsFile,
        [string]$OutputFile
    )

    $SymbolsPath = Join-Path $FontDir $SymbolsFile
    if (-not (Test-Path -LiteralPath $SymbolsPath)) {
        throw "Symbols file not found: $SymbolsPath"
    }

    $Symbols = Get-Content -LiteralPath $SymbolsPath -Raw -Encoding UTF8
    $OutputPath = Join-Path $FontDir $OutputFile

    & $LvFontConv `
        --font $FontPath `
        --size $Size `
        --bpp $Bpp `
        --format lvgl `
        --lv-include lvgl.h `
        --no-compress `
        --range 0x20-0x7F `
        --symbols $Symbols `
        -o $OutputPath
}

Invoke-FontConv -Size 13 -SymbolsFile "cjk_symbols.txt" -OutputFile "lv_font_cjk_13.c"
Invoke-FontConv -Size 16 -SymbolsFile "cjk_symbols_16.txt" -OutputFile "lv_font_cjk_16.c"

Write-Host "Fonts regenerated from $FontPath with bpp=$Bpp"
