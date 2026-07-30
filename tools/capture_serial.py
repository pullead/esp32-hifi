import sys
import time

import serial


port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
no_reset = "--no-reset" in sys.argv

p = serial.Serial(port, baud, timeout=0.25)
if not no_reset:
    p.dtr = False
    p.rts = True
    time.sleep(0.12)
    p.rts = False
    time.sleep(0.12)
    p.dtr = True

end = time.monotonic() + seconds
chunks = []
while time.monotonic() < end:
    data = p.read(4096)
    if data:
        chunks.append(data)
p.close()

text = b"".join(chunks).decode("utf-8", errors="replace")
for key in ("Guru Meditation", "abort()", "NULL TX buffer pointer", "ESP_ERR_NO_MEM", "[LCD]", "[LVGL]"):
    print(f"{key}: {text.count(key)}")
print(text[-9000:])
