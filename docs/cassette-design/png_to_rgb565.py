import sys
from PIL import Image

def convert(png_path, c_path, var_name):
    img = Image.open(png_path).convert("RGB")
    w, h = img.size
    px = img.load()
    out = []
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    with open(c_path, "w") as f:
        f.write("// Auto-generated RGB565 pixel data, %dx%d, LV_COLOR_16_SWAP=0\n" % (w, h))
        f.write("#include <stdint.h>\n")
        f.write("const uint16_t %s_w = %d;\n" % (var_name, w))
        f.write("const uint16_t %s_h = %d;\n" % (var_name, h))
        f.write("const uint8_t %s_map[] = {\n" % var_name)
        for i in range(0, len(out), 20):
            f.write(",".join(str(b) for b in out[i:i+20]) + ",\n")
        f.write("};\n")
    print(f"{var_name}: {w}x{h}, {len(out)} bytes")

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
