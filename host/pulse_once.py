"""One bounded pulse, only after human confirms wiring, clearance and VM state.
Selected wheel: default 500ms, up to 2000ms with PULSEINFO capability; ALL: 300ms maximum.

Serial STATUS verifies reported firmware state, not physical voltage or motion.
No retry of a movement command. STOP is attempted in cleanup.
"""
import argparse
import re
import time
from pathlib import Path

import serial
from serial.tools import list_ports


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wheel", required=True, choices=("FL", "RL", "FR", "RR", "ALL"))
    ap.add_argument("--duty", required=True, type=int)
    ap.add_argument("--log", required=True)
    ap.add_argument("--prepare", type=float, default=5)
    ap.add_argument("--duration", type=int, help="milliseconds; default single=500, ALL=300")
    args = ap.parse_args()
    if not 1 <= abs(args.duty) <= 255 or not 0 <= args.prepare <= 30:
        ap.error("nonzero duty within ±255; preparation within 0..30 seconds")
    is_all = args.wheel == "ALL"
    if is_all and abs(args.duty) > 192:
        ap.error("ALL duty must be within ±192")
    duration = args.duration if args.duration is not None else (300 if is_all else 500)
    if not 50 <= duration <= (300 if is_all else 2000):
        ap.error("duration must be 50..300 for ALL or 50..2000 for a single wheel")
    ports = [p.device for p in list_ports.comports()
             if (p.vid, p.pid) == (0x1A86, 0x7523)]
    if len(ports) != 1:
        raise RuntimeError(f"Expected one CH340: {ports}")
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=1)
    ser.dtr = False
    ser.rts = False
    ser.port = ports[0]
    transcript = []
    received = []
    started = time.monotonic()

    def send(command):
        row = f"{time.monotonic()-started:.3f}s TX {command}"
        transcript.append(row)
        print(row, flush=True)
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

    def collect(seconds):
        lines = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            raw = ser.readline()
            if raw:
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    row = f"{time.monotonic()-started:.3f}s RX {line}"
                    transcript.append(row)
                    received.append((time.monotonic(), line))
                    print(row, flush=True)
                    lines.append(line)
        return lines

    def check_idle(lines):
        states = [line for line in lines if line.startswith("STATUS ")]
        expected = r"STATUS hw=1 ps=1 mode=[0-9A-Fa-f]+ buttons=0000 control=LOCKED STBY=0 duty=0,0,0,0"
        if not states or not re.fullmatch(expected, states[-1]):
            raise RuntimeError(f"Idle/link check failed: {states}")

    ser.open()
    try:
        print(f"PORT {ports[0]}; one {args.wheel} pulse only", flush=True)
        send("STOP")
        collect(0.35)
        send("INFO")
        info_lines = collect(0.3)
        infos = [line for line in info_lines if line.startswith("INFO ")]
        polarity = [1, 1, 1, 1]
        if infos:
            match = re.fullmatch(r"INFO firmware=(\S+) polarity=(-?1),(-?1),(-?1),(-?1) allCap=(\d+) allMaxMs=(\d+)", infos[-1])
            if not match:
                raise RuntimeError(f"Unrecognized INFO: {infos[-1]}")
            polarity = [int(match.group(i)) for i in range(2, 6)]
            if is_all and (match.group(1) not in ("MecanumPS2-v1.1+diag-all", "MecanumPS2-v1.1+diag-motion") or
                           abs(args.duty) > int(match.group(6)) or duration > int(match.group(7))):
                raise RuntimeError("ALL firmware/limits mismatch")
        elif is_all or "ERR command/interlock; LOCKED" not in info_lines:
            raise RuntimeError("Missing diagnostic INFO")
        if not is_all and duration > 500:
            send("PULSEINFO")
            if "PULSEINFO revision=single-2s maxDuty=255 maxMs=2000" not in collect(0.3):
                raise RuntimeError("Firmware does not confirm 2000ms single-wheel capability")
        collect(args.prepare)
        send("STATUS")
        check_idle(collect(0.35))
        command = f"ALLPULSE {args.duty} {duration}" if is_all else f"PULSE {args.wheel} {args.duty} {duration}"
        pulse_started = time.monotonic()
        send(command)
        initial = collect(0.12)
        send("STATUS")
        during = collect(duration / 1000 + 0.15)
        prefix = "ALLPULSE" if is_all else f"PULSE {args.wheel}"
        ack = f"OK {prefix} duty={args.duty} duration={duration}ms fixedDuty=noRamp"
        if ack not in initial + during:
            raise RuntimeError("Pulse not acknowledged; no retry")
        expected = [args.duty*p for p in polarity] if is_all else [0, 0, 0, 0]
        if not is_all:
            index = ("FL", "RL", "FR", "RR").index(args.wheel)
            expected[index] = args.duty * polarity[index]
        expected = ",".join(map(str, expected))
        control = "ALLPULSE" if is_all else "PULSE"
        if not any(f"control={control} STBY=1 duty={expected}" in line for line in during):
            raise RuntimeError("Selected-wheel reported duty not observed")
        if "JOG DONE/STOP; LOCKED" not in during:
            raise RuntimeError("Automatic expiry not observed")
        endings = [at for at, line in received
                   if at >= pulse_started and line == "JOG DONE/STOP; LOCKED"]
        if not endings or abs(endings[0] - pulse_started - duration / 1000) > 0.15:
            raise RuntimeError("Reported expiry timing differs from requested duration")
    finally:
        try:
            send("STOP")
            collect(0.2)
            send("STATUS")
            check_idle(collect(0.45))
        finally:
            ser.close()
            Path(args.log).write_text("\n".join(transcript) + "\n", encoding="utf-8")
    print("Single pulse ended; reported idle confirmed; serial closed.", flush=True)


if __name__ == "__main__":
    main()
