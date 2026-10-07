"""Targeted four-wheel start/hold checks; motor power must physically be OFF."""
import argparse
import re
import time
from pathlib import Path
import serial
from serial.tools import list_ports

PROFILE = "STARTMOVEINFO revision=kick300-fwd-v1 startDuty=255 startMs=300 maxDuty=192 minMs=500 maxMs=30000"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", required=True)
    args = ap.parse_args()
    log = Path(args.log)
    if log.exists():
        ap.error("log already exists")
    log.parent.mkdir(parents=True, exist_ok=True)
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x1A86, 0x7523)]
    if len(ports) != 1:
        raise RuntimeError(f"Expected one CH340: {ports}")
    ser = serial.Serial(port=None, baudrate=115200, timeout=.03, write_timeout=1)
    ser.dtr = False
    ser.rts = False
    ser.port = ports[0]
    transcript = []
    started = time.monotonic()

    def send(command):
        row = f"{time.monotonic()-started:.3f}s TX {command}"
        print(row, flush=True)
        transcript.append(row)
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

    def collect(seconds):
        rows = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            raw = ser.readline()
            if raw:
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    row = f"{time.monotonic()-started:.3f}s RX {line}"
                    print(row, flush=True)
                    transcript.append(row)
                    rows.append(line)
        return rows

    def idle():
        send("STATUS")
        rows = collect(.4)
        states = [s for s in rows if s.startswith("STATUS ")]
        if not states or not re.fullmatch(r"STATUS hw=1 ps=1 mode=[0-9A-Fa-f]+ buttons=0000 control=LOCKED STBY=0 duty=0,0,0,0", states[-1]):
            raise RuntimeError(f"Idle/link check failed: {states}")

    def arm():
        idle()
        send("STARTMOVE FWD 192 2000")
        rows = collect(.08)
        if "OK STARTMOVE FWD duty=192 duration=2000ms startDuty=255 startMs=300" not in rows:
            raise RuntimeError("Startup command not accepted; no retry")

    ser.open()
    try:
        print("VM OFF: software state checks only", flush=True)
        send("STOP"); collect(.4)
        send("STARTMOVEINFO")
        if PROFILE not in collect(.3):
            raise RuntimeError("Startup profile mismatch")
        send("COMP OFF")
        if "OK COMP OFF; LOCKED" not in collect(.3):
            raise RuntimeError("Trim off not accepted")
        send("COMPINFO")
        if not any(s.startswith("COMPINFO ") and " enabled=0 " in s for s in collect(.3)):
            raise RuntimeError("Trim off not confirmed")
        for bad in ("STARTMOVE BACK 192 2000", "STARTMOVE FWD 0 2000", "STARTMOVE FWD -192 2000",
                    "STARTMOVE FWD 193 2000", "STARTMOVE FWD 192 499", "STARTMOVE FWD 192 30001",
                    "STARTMOVE FWD 192 2000 extra", "ALLPULSE 192 301"):
            send(bad)
            expected = "ERR ALLPULSE/interlock; LOCKED" if bad.startswith("ALLPULSE") else "ERR STARTMOVE/interlock; LOCKED"
            if expected not in collect(.12):
                raise RuntimeError(f"Invalid command not rejected: {bad}")
            idle()
        arm()
        send("STATUS")
        rows = collect(.4)
        if not any("control=STARTMOVE STBY=1 duty=255,255,-255,-255" in s for s in rows):
            raise RuntimeError("Startup stage not observed")
        send("STATUS")
        rows += collect(1.7)
        if not any("control=STARTMOVE STBY=1 duty=192,192,-192,-192" in s for s in rows):
            raise RuntimeError("Hold stage not observed")
        if "JOG DONE/STOP; LOCKED" not in rows:
            raise RuntimeError("Expiry not observed")
        idle()
        for delay in (.02, .6):
            arm(); collect(delay)
            send("STOP")
            send("STARTMOVE FWD 192 2000")
            rows = collect(.15)
            if "OK STOP LOCKED" not in rows or "ERR STARTMOVE/interlock; LOCKED" not in rows:
                raise RuntimeError("STOP/cooldown failed")
            idle()
        arm()
        send("PULSE RR 192 2000")
        if "ERR PULSE/interlock; LOCKED" not in collect(.15):
            raise RuntimeError("Overlap not stopped")
        idle()
        send("PS2")
        if "OK PS2: hold L1 + D-pad / L2 / R2; CIRCLE locks" not in collect(.15):
            raise RuntimeError("Remote guard setup failed")
        send("STARTMOVE FWD 192 2000")
        if "ERR STARTMOVE/interlock; LOCKED" not in collect(.15):
            raise RuntimeError("Remote guard failed")
        idle()
        print("PASS: VM OFF startup/hold states, expiry, bounds, STOP/cooldown, overlap, remote guard", flush=True)
        transcript.append("PASS: software reports only; no physical motor movement certified")
    finally:
        try:
            send("STOP"); collect(.2); idle()
        finally:
            ser.close()
            log.write_text("\n".join(transcript) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
