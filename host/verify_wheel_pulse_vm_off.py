"""Run only with VM physically OFF. Keep the current known motor/wiring unchanged.

Checks the diagnostic command parser and reported automatic stop.
Does not measure physical outputs, current, or motor movement.
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
        raise RuntimeError(f"Expected one CH340, got {ports}")
    transcript = []
    started = time.monotonic()
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.05,
                        write_timeout=1)
    ser.dtr = False
    ser.rts = False
    ser.port = ports[0]
    ser.open()

    def collect(seconds):
        lines = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                record = f"{time.monotonic()-started:.3f}s RX {line}"
                transcript.append(record)
                print(record, flush=True)
                lines.append(line)
        return lines

    def send(command):
        record = f"{time.monotonic()-started:.3f}s TX {command}"
        transcript.append(record)
        print(record, flush=True)
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

    def check_idle():
        send("STATUS")
        lines = collect(0.35)
        states = [line for line in lines if line.startswith("STATUS ")]
        expected = r"STATUS hw=1 ps=1 mode=[0-9A-Fa-f]+ buttons=0000 control=LOCKED STBY=0 duty=0,0,0,0"
        if not states or not re.fullmatch(expected, states[-1]):
            raise RuntimeError(f"Idle/link precheck failed: {states}")

    try:
        print(f"VM-OFF verification only; port={ports[0]}", flush=True)
        collect(0.6)
        send("STOP")
        collect(0.35)
        check_idle()
        for command in ("PULSE XX -128 500", "PULSE RL 256 500",
                        "PULSE RL 0 500", "PULSE RL -128 49",
                        "PULSE RL -128 501", "PULSE RL -128 500 extra"):
            send(command)
            lines = collect(0.3)
            if "ERR PULSE/interlock; LOCKED" not in lines:
                raise RuntimeError(f"Invalid command not rejected: {command}")
            check_idle()
        for index, wheel in enumerate(("FL", "RL", "FR", "RR")):
            check_idle()
            duty = 128 if index % 2 == 0 else -128
            expected = [0, 0, 0, 0]
            expected[index] = duty
            expected = ",".join(map(str, expected))
            send(f"PULSE {wheel} {duty} 500")
            initial = collect(0.12)
            send("STATUS")
            during = collect(0.62)
            ack = f"OK PULSE {wheel} duty={duty} duration=500ms fixedDuty=noRamp"
            if ack not in initial + during:
                raise RuntimeError(f"Valid {wheel} pulse not accepted")
            if not any(f"control=PULSE STBY=1 duty={expected}" in s for s in during):
                raise RuntimeError(f"Selected {wheel} duty/rest-zero state not observed")
            if "JOG DONE/STOP; LOCKED" not in during:
                raise RuntimeError(f"{wheel} automatic expiry not observed")
            check_idle()
        # A second pulse while one is active must be rejected and lock all outputs.
        send("PULSE FL 128 500")
        first = collect(0.12)
        if "OK PULSE FL duty=128 duration=500ms fixedDuty=noRamp" not in first:
            raise RuntimeError("Overlap test first pulse not accepted")
        send("PULSE RR 128 500")
        rejected = collect(0.2)
        if "ERR PULSE/interlock; LOCKED" not in rejected:
            raise RuntimeError("Overlapping command not rejected")
        check_idle()
        # Explicit STOP during a pulse must restore reported locked/zero state.
        send("PULSE FR 128 500")
        first = collect(0.12)
        if "OK PULSE FR duty=128 duration=500ms fixedDuty=noRamp" not in first:
            raise RuntimeError("STOP test pulse not accepted")
        send("STOP")
        collect(0.2)
        check_idle()
        print("PASS: four selectable outputs, other duties zero, auto expiry, overlap rejection, STOP.", flush=True)
        transcript.append("PASS: parser and reported timed-stop checks; VM OFF; no powered motor test.")
    finally:
        try:
            send("STOP")
            collect(0.2)
            send("STATUS")
            collect(0.35)
        finally:
            ser.close()
            path = Path(__file__).resolve().parents[1] / "logs" / "四通道诊断程序-VM关闭验证-2026-10-07.txt"
            path.write_text("\n".join(transcript) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
