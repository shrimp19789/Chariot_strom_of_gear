"""VM must be physically OFF. Verify calibrated firmware state and command guards.

These serial tests do not certify physical outputs, current or motor movement.
"""
import re
import time
from pathlib import Path

import serial
from serial.tools import list_ports


def main():
    ports = [p.device for p in list_ports.comports()
             if (p.vid, p.pid) == (0x1A86, 0x7523)]
    if len(ports) != 1:
        raise RuntimeError(f"Expected one CH340: {ports}")
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=1)
    ser.dtr = False
    ser.rts = False
    ser.port = ports[0]
    transcript = []
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
                    print(row, flush=True)
                    lines.append(line)
        return lines

    def idle():
        send("STATUS")
        lines = collect(0.35)
        states = [line for line in lines if line.startswith("STATUS ")]
        expected = r"STATUS hw=1 ps=1 mode=[0-9A-Fa-f]+ buttons=0000 control=LOCKED STBY=0 duty=0,0,0,0"
        if not states or not re.fullmatch(expected, states[-1]):
            raise RuntimeError(f"Idle/link check failed: {states}")

    def pulse(command, ack, control, duties, duration):
        idle()
        send(command)
        initial = collect(0.08)
        send("STATUS")
        during = collect(duration / 1000 + 0.12)
        if ack not in initial + during:
            raise RuntimeError(f"Pulse not acknowledged: {command}")
        if not any(f"control={control} STBY=1 duty={duties}" in x for x in during):
            raise RuntimeError(f"Expected calibrated duty not observed: {command}")
        if "JOG DONE/STOP; LOCKED" not in during:
            raise RuntimeError(f"Automatic expiry not observed: {command}")
        idle()

    ser.open()
    try:
        print(f"VM OFF checks only; port={ports[0]}", flush=True)
        send("STOP")
        collect(0.35)
        send("INFO")
        info = collect(0.35)
        expected_info = "INFO firmware=MecanumPS2-v1.1+diag-all polarity=1,1,-1,-1 allCap=192 allMaxMs=300"
        if expected_info not in info:
            raise RuntimeError(f"Firmware identity/calibration mismatch: {info}")
        idle()
        for command in ("ALLPULSE 193 300", "ALLPULSE -193 300",
                        "ALLPULSE 0 300", "ALLPULSE 192 49",
                        "ALLPULSE 192 301", "ALLPULSE 192 300 extra"):
            send(command)
            if "ERR ALLPULSE/interlock; LOCKED" not in collect(0.25):
                raise RuntimeError(f"Invalid ALLPULSE not rejected: {command}")
            idle()
        polarity = [1, 1, -1, -1]
        for i, wheel in enumerate(("FL", "RL", "FR", "RR")):
            duties = [0, 0, 0, 0]
            duties[i] = 128 * polarity[i]
            pulse(f"PULSE {wheel} 128 500",
                  f"OK PULSE {wheel} duty=128 duration=500ms fixedDuty=noRamp",
                  "PULSE", ",".join(map(str, duties)), 500)
        for duty in (192, -192):
            duties = ",".join(str(duty * p) for p in polarity)
            pulse(f"ALLPULSE {duty} 300",
                  f"OK ALLPULSE duty={duty} duration=300ms fixedDuty=noRamp",
                  "ALLPULSE", duties, 300)
        # Both kinds of overlapping request must stop and lock the active output.
        for first, second in (("ALLPULSE 192 300", "PULSE FL 128 500"),
                              ("PULSE FL 128 500", "ALLPULSE 192 300")):
            idle()
            send(first)
            if not any(line.startswith("OK ") for line in collect(0.08)):
                raise RuntimeError("Overlap test first command not accepted")
            send(second)
            expected = "ERR PULSE/interlock; LOCKED" if second.startswith("PULSE ") else "ERR ALLPULSE/interlock; LOCKED"
            if expected not in collect(0.25):
                raise RuntimeError("Overlapping command not rejected")
            idle()
        # STOP during ALLPULSE, then an immediate restart must be refused.
        idle()
        send("ALLPULSE 192 300")
        if "OK ALLPULSE duty=192 duration=300ms fixedDuty=noRamp" not in collect(0.08):
            raise RuntimeError("STOP/cooldown first command not accepted")
        send("STOP")
        send("ALLPULSE 192 300")
        stopped = collect(0.25)
        if "OK STOP LOCKED" not in stopped or "ERR ALLPULSE/interlock; LOCKED" not in stopped:
            raise RuntimeError("STOP or immediate restart guard failed")
        idle()
        # A diagnostic pulse must not override unlocked remote mode.
        send("PS2")
        if "OK PS2: hold L1 + D-pad / L2 / R2; CIRCLE locks" not in collect(0.2):
            raise RuntimeError("PS2 guard test not armed")
        send("ALLPULSE 192 300")
        if "ERR ALLPULSE/interlock; LOCKED" not in collect(0.25):
            raise RuntimeError("Remote-mode interlock failed")
        idle()
        print("PASS: calibrated selected/all states, limits, expiry, overlap, STOP, cooldown, remote interlock.", flush=True)
        transcript.append("PASS: VM OFF; reported software states only; no powered four-wheel test.")
    finally:
        try:
            send("STOP")
            collect(0.2)
            idle()
        finally:
            ser.close()
            log = Path(__file__).resolve().parents[1] / "logs" / "四轮联动诊断-VM关闭验证-2026-10-07.txt"
            log.write_text("\n".join(transcript) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
