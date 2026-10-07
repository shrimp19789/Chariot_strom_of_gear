"""Run explicitly selected bench motions once, with bounded firmware timeouts.

Human must confirm physical VM state and all wheels clear before running.
Serial state checks do not measure current, voltage, heat or actual movement.
Any error, premature stop or button press aborts the sequence; no motion retry.
"""
import argparse
import re
import time
from pathlib import Path

import serial
from serial.tools import list_ports


# Logical wheel directions, independently listed for observation/state checking.
# Order FL, RL, FR, RR. + = tire top toward vehicle front; - = rear.
MOVES = {
    "FWD": ("前进", (1, 1, 1, 1)),
    "BACK": ("后退", (-1, -1, -1, -1)),
    "LEFT": ("左横移", (-1, 1, 1, -1)),
    "RIGHT": ("右横移", (1, -1, -1, 1)),
    "FL": ("左前斜行", (0, 1, 1, 0)),
    "FR": ("右前斜行", (1, 0, 0, 1)),
    "BL": ("左后斜行", (-1, 0, 0, -1)),
    "BR": ("右后斜行", (0, -1, -1, 0)),
    "CCW": ("原地左转", (-1, -1, 1, 1)),
    "CW": ("原地右转", (1, 1, -1, -1)),
}
INFO = "INFO firmware=MecanumPS2-v1.1+diag-motion polarity=1,1,-1,-1 allCap=192 allMaxMs=300"
MOTIONINFO = "MOTIONINFO maxDuty=192 maxMs=10000"
RAMPINFO = "RAMPINFO revision=triangle-v1 maxDuty=192 minMs=1000 maxMs=10000"
FORWARDINFO = "FORWARDINFO revision=forward-30s maxDuty=192 maxMs=30000"


def check_ramp_trace(samples, logical, cap):
    """Validate reported signs, synchronization, bounds and both duty slopes."""
    signs = [v * p for v, p in zip(logical, (1, 1, -1, -1))]
    levels = []
    for sample in samples:
        level = abs(sample[0])
        if not 0 <= level <= cap or sample != [s * level for s in signs]:
            raise RuntimeError("Ramp reported direction, synchronization or bounds mismatch")
        levels.append(level)
    if len(levels) < 6:
        raise RuntimeError("Too few ramp samples")
    peak = max(levels)
    index = levels.index(peak)
    if peak < cap * 0.9 or levels[0] > cap * 0.3 or levels[-1] > cap * 0.3:
        raise RuntimeError("Ramp start, peak or end not observed")
    if any(b < a for a, b in zip(levels[:index], levels[1:index+1])):
        raise RuntimeError("Ramp rising phase was not monotonic")
    if any(b > a for a, b in zip(levels[index:], levels[index+1:])):
        raise RuntimeError("Ramp falling phase was not monotonic")
    return peak


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--moves", nargs="+", choices=MOVES, required=True)
    ap.add_argument("--duration", type=int, default=10000)
    ap.add_argument("--duty", type=int, default=192)
    ap.add_argument("--prepare", type=float, default=5)
    ap.add_argument("--gap", type=float, default=5)
    ap.add_argument("--log", required=True)
    ap.add_argument("--ramp", action="store_true", help="FWD/BACK: triangular duty, 1000..10000ms")
    args = ap.parse_args()
    max_duration = 30000 if args.moves == ["FWD"] and not args.ramp else 10000
    if not 50 <= args.duration <= max_duration or not 1 <= args.duty <= 192:
        ap.error("duration 50..10000ms (single fixed FWD up to 30000ms); duty 1..192")
    if not 0 <= args.prepare <= 30 or not 5 <= args.gap <= 30:
        ap.error("prepare 0..30s; gap 5..30s")
    if len(set(args.moves)) != len(args.moves):
        ap.error("each motion may be requested only once per sequence")
    if args.ramp and (not 1000 <= args.duration <= 10000 or
                      any(m not in ("FWD", "BACK") for m in args.moves)):
        ap.error("--ramp requires FWD/BACK and duration 1000..10000ms")
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

    def note(message):
        row = f"{time.monotonic()-started:.3f}s {message}"
        transcript.append(row)
        print(row, flush=True)

    def send(command):
        note("TX " + command)
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
                    note("RX " + line)
                    lines.append(line)
        return lines

    def state(line):
        match = re.fullmatch(
            r"STATUS hw=(\d) ps=(\d) mode=[0-9A-Fa-f]+ buttons=([0-9A-Fa-f]{4}) "
            r"control=(\S+) STBY=(\d) duty=(-?\d+,-?\d+,-?\d+,-?\d+)", line)
        if not match:
            raise RuntimeError("Unexpected status format: " + line)
        hw, ps, buttons, control, stby, duties = match.groups()
        if hw != "1" or ps != "1" or buttons != "0000":
            raise RuntimeError("Hardware/link/button check failed: " + line)
        return control, stby, duties

    def idle():
        send("STATUS")
        lines = collect(0.35)
        statuses = [line for line in lines if line.startswith("STATUS ")]
        if not statuses or state(statuses[-1]) != ("LOCKED", "0", "0,0,0,0"):
            raise RuntimeError("Idle state not confirmed")

    def quiet(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for line in collect(min(0.4, max(0, deadline-time.monotonic()))):
                if line.startswith(("ERR ", "FAULT ", "BOOT ", "OK PS2")):
                    raise RuntimeError("Unexpected event during pause: " + line)
                if line.startswith("STATUS ") and state(line) != ("LOCKED", "0", "0,0,0,0"):
                    raise RuntimeError("Pause was not idle")

    ser.open()
    try:
        note(f"PORT {ports[0]}; requested={','.join(args.moves)}; duration={args.duration}ms")
        send("STOP")
        collect(0.35)
        send("INFO")
        if INFO not in collect(0.3):
            raise RuntimeError("Motion firmware identity/calibration mismatch")
        send("MOTIONINFO")
        if MOTIONINFO not in collect(0.3):
            raise RuntimeError("Motion firmware limits mismatch")
        if args.duration > 10000:
            send("FORWARDINFO")
            if FORWARDINFO not in collect(0.3):
                raise RuntimeError("Forward 30-second firmware capability mismatch")
        if args.ramp:
            send("RAMPINFO")
            if RAMPINFO not in collect(0.3):
                raise RuntimeError("Ramp firmware capability mismatch")
        idle()
        quiet(args.prepare)
        for index, motion in enumerate(args.moves):
            if index:
                quiet(args.gap)
            idle()
            name, logical = MOVES[motion]
            duties = ",".join(str(v * args.duty * p)
                              for v, p in zip(logical, (1, 1, -1, -1)))
            note(f"BEGIN {index+1}/{len(args.moves)} {motion} {name}")
            sent_at = time.monotonic()
            control = "RAMPPULSE" if args.ramp else "MOVEPULSE"
            send(f"{control} {motion} {args.duty} {args.duration}")
            profile = "profile=triangle" if args.ramp else "fixedDuty=noRamp"
            ack = f"OK {control} {motion} duty={args.duty} duration={args.duration}ms {profile}"
            seen_ack = seen_active = seen_done = False
            ramp_samples = []

            def inspect(lines):
                nonlocal seen_ack, seen_active, seen_done
                for line in lines:
                    if line == ack:
                        seen_ack = True
                    elif line == "JOG DONE/STOP; LOCKED":
                        if time.monotonic()-sent_at < args.duration/1000-0.06:
                            raise RuntimeError("Motion stopped early; sequence aborted")
                        seen_done = True
                    elif line.startswith(("ERR ", "FAULT ", "BOOT ", "OK PS2")):
                        raise RuntimeError("Unexpected event; sequence aborted: " + line)
                    elif line.startswith("STATUS "):
                        reported = state(line)
                        if args.ramp and reported[0] == control:
                            sample = list(map(int, reported[2].split(",")))
                            level = abs(sample[0])
                            expected = [v * level * p for v, p in zip(logical, (1, 1, -1, -1))]
                            if (sample != expected or level > args.duty or
                                    reported[1] != str(int(level > 0))):
                                raise RuntimeError("Unexpected ramp state: " + line)
                            ramp_samples.append(sample)
                            seen_active |= level > 0
                        elif reported == (control, "1", duties):
                            seen_active = True
                        elif (reported == (control, "0", "0,0,0,0")
                              and not seen_active and time.monotonic()-sent_at < 0.15):
                            # The command is accepted before the next <=10ms PWM tick.
                            # A periodic STATUS may expose this brief startup state.
                            pass
                        elif (reported == ("LOCKED", "0", "0,0,0,0")
                              and not seen_ack and time.monotonic()-sent_at < 0.15):
                            # Ignore only an already-buffered idle report before ACK.
                            pass
                        elif reported == ("LOCKED", "0", "0,0,0,0"):
                            if not seen_done:
                                raise RuntimeError("Unexpected idle before motion expiry")
                        else:
                            raise RuntimeError("Unexpected motion state: " + line)

            inspect(collect(0.08))
            send("STATUS")
            deadline = sent_at + args.duration/1000 + 0.7
            next_status = time.monotonic() + 0.1
            while time.monotonic() < deadline and not seen_done:
                if args.ramp and time.monotonic() >= next_status:
                    send("STATUS")
                    next_status = time.monotonic() + 0.1
                inspect(collect(min(0.06 if args.ramp else 0.4, max(0, deadline-time.monotonic()))))
            if not (seen_ack and seen_active and seen_done):
                raise RuntimeError("Missing acceptance, active state or automatic expiry; no retry")
            if args.ramp:
                peak = check_ramp_trace(ramp_samples, logical, args.duty)
                note(f"RAMP checked: {len(ramp_samples)} samples, peak={peak}; rise/fall and wheel signs matched")
            send("STOP")
            collect(0.2)
            idle()
            note(f"END {motion}; reported locked idle confirmed")
        note("PASS: requested motions completed with reported state and expiry checks")
    finally:
        try:
            send("STOP")
            collect(0.2)
            idle()
        finally:
            ser.close()
            log = Path(args.log)
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text("\n".join(transcript) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
