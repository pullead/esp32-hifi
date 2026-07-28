TRAIL_BYTES = list(range(0x40, 0x7F)) + list(range(0x80, 0xFF))  # 190 values, skips 0x7F
assert len(TRAIL_BYTES) == 190

rows = []
for lead in range(0x81, 0xFF):  # 126 values
    row = []
    for trail in TRAIL_BYTES:
        b = bytes([lead, trail])
        try:
            ch = b.decode('gbk')
            cp = ord(ch)
            if cp > 0xFFFF:
                cp = 0xFFFD
        except Exception:
            cp = 0xFFFD
        row.append(cp)
    rows.append(row)

flat = [v for row in rows for v in row]
print("entries:", len(flat), "bytes:", len(flat) * 2)

with open("/Users/allen/Documents/Esp-HiFi/MiniWebRadio-Waveshare/src/gbk_table.c", "w") as f:
    f.write("// Auto-generated GBK (lead 0x81-0xFE, trail 0x40-0x7E/0x80-0xFE) -> Unicode\n")
    f.write("// codepoint table, for decoding GBK-encoded .lrc lyric files on device.\n")
    f.write("// Regenerate: python3 docs/cassette-design/../gbk/gen_gbk_table.py (uses Python's\n")
    f.write("// built-in 'gbk' codec as the source of truth). 0xFFFD = unmapped/invalid pair.\n")
    f.write("#include <stdint.h>\n")
    f.write("const uint16_t gbk_table[%d] = {\n" % len(flat))
    for i in range(0, len(flat), 16):
        f.write(",".join(str(v) for v in flat[i:i+16]) + ",\n")
    f.write("};\n")
