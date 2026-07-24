import sys, time
import serial  # pyserial, bundled with platformio env

PORT = "COM5"
BAUD = 921600
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "boot_log.txt"

ser = serial.Serial(PORT, BAUD, timeout=0.5)
# toggle DTR/RTS to reset the board so we catch the boot from the start
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
t0 = time.time()
buf = bytearray()
while time.time() - t0 < SECS:
    chunk = ser.read(4096)
    if chunk:
        buf.extend(chunk)
ser.close()
text = buf.decode("utf-8", errors="replace")
with open(OUT, "w", encoding="utf-8") as f:
    f.write(text)
print(f"captured {len(buf)} bytes in {SECS}s -> {OUT}")
