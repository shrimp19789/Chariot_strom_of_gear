import argparse
import time
from pathlib import Path
import serial

p = argparse.ArgumentParser()
p.add_argument('--port', default='COM9')
p.add_argument('--seconds', type=float, default=12)
p.add_argument('--send', default='')
p.add_argument('--reset', action='store_true')
p.add_argument('--output', required=True)
a = p.parse_args()
data = bytearray()
with serial.Serial(a.port, 115200, timeout=0.1) as s:
    if a.reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.12)
        s.rts = False
        time.sleep(0.06)
    start = time.monotonic()
    sent = False
    while time.monotonic() - start < a.seconds:
        if a.send and not sent and time.monotonic() - start >= 2:
            s.write((a.send + '\n').encode())
            s.flush()
            sent = True
        data.extend(s.read(min(s.in_waiting or 1, 4096)))
result = data.decode('utf-8', errors='replace')
Path(a.output).write_text(result, encoding='utf-8')
print(result.encode('ascii', errors='backslashreplace').decode('ascii'))
