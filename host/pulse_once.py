"""One bounded pulse, only after human confirms wiring, clearance and VM state.
Selected wheel: default 500ms, up to 2000ms; raw ALL: 300ms maximum.
Explicit ALL --startup: forward only, 300ms at 255, then hold; total <=30000ms.
Hold trim is opt-in with --compensate and reset off on exit.

Serial STATUS verifies reported firmware state, not physical voltage or motion.
No retry of a movement command. STOP is attempted in cleanup.
"""
import argparse
import re
import time
from pathlib import Path

import serial
from serial.tools import list_ports
from motion_sequence import COMPINFO, wheel_duties


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wheel", required=True, choices=("FL", "RL", "FR", "RR", "ALL"))
    ap.add_argument("--duty", required=True, type=int)
    ap.add_argument("--log", required=True)
    ap.add_argument("--prepare", type=float, default=5)
    ap.add_argument("--audible", action="store_true", help="Windows WAV tones before pulse and after reported expiry")
    ap.add_argument("--startup", action="store_true", help="300ms at signed 255, then requested hold; ALL is forward only")
    ap.add_argument("--compensate", action="store_true", help="ALL --startup only: apply current per-wheel hold trim after kick")
    ap.add_argument("--duration", type=int, help="milliseconds; default single=500, ALL=300")
    args = ap.parse_args()
    if not 1 <= abs(args.duty) <= 255 or not 0 <= args.prepare <= 30:
        ap.error("nonzero duty within ±255; preparation within 0..30 seconds")
    is_all = args.wheel == "ALL"
    if is_all and abs(args.duty) > 192:
        ap.error("ALL duty must be within ±192")
    duration = args.duration if args.duration is not None else (2000 if is_all and args.startup else (300 if is_all else 500))
    max_ms = 30000 if is_all and args.startup else (300 if is_all else 2000)
    min_ms = 500 if args.startup else 50
    if not min_ms <= duration <= max_ms:
        ap.error(f"duration must be {min_ms}..{max_ms}ms for this mode")
    if is_all and args.startup and args.duty < 1:
        ap.error("ALL startup supports positive forward duty only")
    if args.compensate and not (is_all and args.startup):
        ap.error("compensation requires ALL --startup")
    if args.audible:
        import math
        import struct
        import tempfile
        import wave
        import winsound
        cue_dir = tempfile.TemporaryDirectory(prefix="tb6612-cues-")
        start_cue = Path(cue_dir.name) / "start.wav"
        end_cue = Path(cue_dir.name) / "end.wav"
        rate = 44100
        for path, frequency in ((start_cue, 1500), (end_cue, 600)):
            samples = bytearray()
            for i in range(rate * 2):
                fade = min(1.0, i / (rate * .02), (rate * 2 - 1 - i) / (rate * .02))
                value = int(9000 * fade * math.sin(2 * math.pi * frequency * i / rate))
                samples.extend(struct.pack("<h", value))
            with wave.open(str(path), "wb") as cue_file:
                cue_file.setnchannels(1)
                cue_file.setsampwidth(2)
                cue_file.setframerate(rate)
                cue_file.writeframes(samples)
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
    cue_started = False
    cue_ended = False
    cue_prefix = ("STARTMOVE FWD" if args.startup else "ALLPULSE") if is_all else f"{'STARTPULSE' if args.startup else 'PULSE'} {args.wheel}"
    suffix = "startDuty=255 startMs=300" if args.startup else "fixedDuty=noRamp"
    cue_ack = f"OK {cue_prefix} duty={args.duty} duration={duration}ms {suffix}"

    def send(command):
        row = f"{time.monotonic()-started:.3f}s TX {command}"
        transcript.append(row)
        print(row, flush=True)
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

    def collect(seconds):
        nonlocal cue_started, cue_ended
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
                    if args.audible and line == cue_ack and not cue_started:
                        cue_started = True
                    if args.audible and line == "JOG DONE/STOP; LOCKED" and cue_started and not cue_ended:
                        cue_ended = True
                        winsound.PlaySound(str(end_cue), winsound.SND_FILENAME | winsound.SND_NODEFAULT)
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
            if is_all and not args.startup and (match.group(1) not in ("MecanumPS2-v1.1+diag-all", "MecanumPS2-v1.1+diag-motion") or
                           abs(args.duty) > int(match.group(6)) or duration > int(match.group(7))):
                raise RuntimeError("ALL firmware/limits mismatch")
        elif is_all or "ERR command/interlock; LOCKED" not in info_lines:
            raise RuntimeError("Missing diagnostic INFO")
        if args.startup:
            capability = "STARTMOVEINFO" if is_all else "STARTPULSEINFO"
            profile = ("STARTMOVEINFO revision=kick300-fwd-v1 startDuty=255 startMs=300 maxDuty=192 minMs=500 maxMs=30000" if is_all else
                       "STARTPULSEINFO revision=kick300-v1 startDuty=255 startMs=300 maxDuty=255 minMs=500 maxMs=2000")
            send(capability)
            if profile not in collect(0.3):
                raise RuntimeError("Firmware does not confirm bounded start/hold capability")
            if is_all:
                mode = "ON" if args.compensate else "OFF"
                send(f"COMP {mode}")
                if f"OK COMP {mode}; LOCKED" not in collect(0.3):
                    raise RuntimeError("Unable to set startup hold trim mode")
                send("COMPINFO")
                if COMPINFO.format(enabled=int(args.compensate)) not in collect(0.3):
                    raise RuntimeError("Startup hold trim profile mismatch")
        elif not is_all and duration > 500:
            send("PULSEINFO")
            if "PULSEINFO revision=single-2s maxDuty=255 maxMs=2000" not in collect(0.3):
                raise RuntimeError("Firmware does not confirm 2000ms single-wheel capability")
        collect(args.prepare)
        send("STATUS")
        check_idle(collect(0.35))
        command = f"{cue_prefix} {args.duty} {duration}"
        if args.audible:
            # Finish the start tone before energizing; audio startup cannot
            # consume the short measurement window.
            winsound.PlaySound(str(start_cue), winsound.SND_FILENAME | winsound.SND_NODEFAULT)
        pulse_started = time.monotonic()
        send(command)
        initial = collect(0.12)
        send("STATUS")
        during = collect(duration / 1000 + 0.15)
        ack = cue_ack
        if ack not in initial + during:
            raise RuntimeError("Pulse not acknowledged; no retry")
        expected = [args.duty*p for p in polarity] if is_all else [0, 0, 0, 0]
        if args.compensate:
            levels = [abs(v) for v in wheel_duties((1,)*4, args.duty, True)]
            expected = [level*p for level,p in zip(levels,polarity)]
        if not is_all:
            index = ("FL", "RL", "FR", "RR").index(args.wheel)
            expected[index] = args.duty * polarity[index]
        expected = ",".join(map(str, expected))
        control = ("STARTMOVE" if args.startup else "ALLPULSE") if is_all else ("STARTPULSE" if args.startup else "PULSE")
        if not any(f"control={control} STBY=1 duty={expected}" in line for line in during):
            raise RuntimeError("Selected-wheel reported duty not observed")
        if args.startup:
            startup = [255*p for p in polarity] if is_all else [0, 0, 0, 0]
            if not is_all:
                startup[index] = (255 if args.duty > 0 else -255) * polarity[index]
            startup = ",".join(map(str, startup))
            if not any(f"control={control} STBY=1 duty={startup}" in line for line in initial + during):
                raise RuntimeError("Startup stage reported duty not observed")
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
            if args.compensate:
                send("COMP OFF")
                if "OK COMP OFF; LOCKED" not in collect(0.3):
                    raise RuntimeError("Startup hold trim reset not acknowledged")
        finally:
            ser.close()
            Path(args.log).write_text("\n".join(transcript) + "\n", encoding="utf-8")
            if args.audible:
                cue_dir.cleanup()
    print("Single pulse ended; reported idle confirmed; serial closed.", flush=True)


if __name__ == "__main__":
    main()
