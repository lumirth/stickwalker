#!/usr/bin/env python3
"""Replay physical sample streams through the compiled firmware receiver.

The oracle is the UART transmitter's byte stream. This deliberately models
optical exposure independently of the receiver: rectangular SIR pulses pass
through the fixed charge-integration windows, then gain, offset, bounded noise
and quantization produce raw observations. It is not hardware qualification.
Only the C++ production receiver is decoded; there is no Python decoder.
"""

import argparse
import ctypes
import hashlib
import json
import math
import random
import subprocess
import tempfile
from pathlib import Path

CPU = 240_000_000
BIT = CPU / 115_200
GATE = CPU / 345_600
ROOT = Path(__file__).resolve().parents[2]


def observations(data, *, phase=0.0, rate=1.0, baseline=180, gain=90,
                 width=1.625, gaps=0.0, seed=0, noise=1.0, bad_stop=False,
                 censor=False, prefix_us=()):
    """Return observations and first exposed gate, never a decoded bit mask."""
    bit = BIT * rate
    now = (12 + phase) * GATE
    if prefix_us:
        now += math.ceil(max(prefix_us) * 240 / GATE) * GATE
    pulses = [(now - lead * 240, 400) for lead in prefix_us]
    for index, byte in enumerate(data):
        pulses.append((now, width * 240))
        for position in range(8):
            if not byte & (1 << position):
                pulses.append((now + (position + 1) * bit, width * 240))
        if bad_stop:
            pulses.append((now + 9 * bit, width * 240))
        gap = gaps[index] if isinstance(gaps, list) else gaps
        now += (10 + gap) * bit
    count = math.ceil(now / GATE) + 80
    exposure = [0.0] * count
    for start, duration in pulses:
        end = start + duration
        lower = max(0, math.floor((start - 450) / GATE))
        upper = min(count, math.ceil((end - 65) / GATE) + 1)
        for index in range(lower, upper):
            origin = math.floor(index * GATE)
            exposure[index] += max(
                0, min(origin + 450, end) - max(origin + 65, start)
            ) / 240
    rng = random.Random(seed)
    samples = bytearray()
    for amount in exposure:
        value = round(baseline - gain * amount + rng.uniform(-noise, noise))
        samples.append(255 if censor and value >= 253 else max(0, min(253, value)))
    return samples, next((i for i, amount in enumerate(exposure) if amount), 0)


class Replay:
    def __init__(self, library):
        self.function = ctypes.CDLL(str(library)).replay_wire
        self.function.argtypes = [
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint, ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint,
            ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ]
        self.function.restype = ctypes.c_int

    def __call__(self, samples, origin=0, indices=None):
        raw = (ctypes.c_uint8 * len(samples)).from_buffer_copy(samples)
        output = (ctypes.c_uint8 * (len(samples) + 1))()
        rejected, gaps = ctypes.c_uint(), ctypes.c_uint()
        times = None if indices is None else (ctypes.c_uint32 * len(indices))(*indices)
        size = self.function(raw, len(samples), origin, times, output, len(output),
                             ctypes.byref(rejected), ctypes.byref(gaps))
        if size < 0:
            raise RuntimeError("replay output overflow")
        packets = []
        position = 0
        while position < size:
            length = output[position]
            position += 1
            packets.append(bytes(output[position:position + length]))
            position += length
        return packets, rejected.value, gaps.value


def exercise(replay, phases):
    results = {}
    cases = 0
    cold = 0
    for width, gain in [(1.625, 90), (1.410, 180), (2.230, 180)]:
        for phase_index in range(phases):
            for rate in [.99, 1.0, 1.01]:
                for value in range(256):
                    expected = bytes([value])
                    samples, first = observations(
                        expected, phase=phase_index / phases, rate=rate,
                        width=width, gain=gain, seed=cases)
                    assert replay(samples) == ([expected], 0, 0), (
                        "byte/phase/clock", width, phase_index, rate, value,
                        replay(samples))
                    cases += 1
                    if width == 1.625:
                        assert replay(samples[first:]) == ([expected], 0, 0), (
                            "first exposed sample", phase_index, rate, value)
                        cold += 1
    results["all_byte_phase_clock_width_cases"] = cases
    results["no_pre_signal_history_cases"] = cold

    values = bytes([0, 255, 0x55, 0xAA, 0x56, 0x50, 1, 0x80])
    cases = 0
    for baseline in [24, 80, 128, 180, 240]:
        for gain in [60, 90, 180, 360]:
            for phase_index in range(phases // 2):
                for rate in [.99, 1.0, 1.01]:
                    samples, _ = observations(
                        values, baseline=baseline, gain=gain,
                        phase=2 * phase_index / phases, rate=rate, gaps=.4,
                        seed=cases)
                    assert replay(samples) == ([values], 0, 0), (
                        "offset/gain", baseline, gain, phase_index, rate)
                    cases += 1
    results["offset_gain_cases"] = cases

    rng = random.Random(20261030)
    cases = 1200
    for index in range(cases):
        length = rng.choice([1, 8, 32, 104, 136])
        data = bytes(rng.randrange(256) for _ in range(length))
        samples, _ = observations(
            data, phase=rng.random(), rate=rng.uniform(.99, 1.01),
            baseline=rng.uniform(24, 240), gain=rng.uniform(60, 360),
            gaps=[rng.choice([0, .1, 1 / 3, 1, 2]) for _ in data], seed=index)
        assert replay(samples) == ([data], 0, 0), ("packet", index)
    results["random_packet_cases"] = cases

    # FF has no data pulses. Frame boundaries must follow its completed stop,
    # including when the next frame's start is already being acquired.
    first, second = b"\x56\xff", b"\xff\x00\x56"
    for idle, expected in [(0, [first + second]), (4, [first + second]),
                           (5, [first, second]), (12, [first, second])]:
        gaps = [0.0] * (len(first) + len(second))
        gaps[len(first) - 1] = idle
        samples, _ = observations(first + second, gaps=gaps, phase=.3, noise=0)
        assert replay(samples) == (expected, 0, 0), ("completed byte gap", idle)
    for idle_us in [61, 100]:
        first = bytes.fromhex("84aba398aba8a9ae")  # valid 2E maintenance header
        second = bytes(range(136))
        gaps = [0.0] * (len(first) + len(second))
        gaps[len(first) - 1] = idle_us * 115200 / 1_000_000
        samples, _ = observations(first + second, gaps=gaps, phase=.3, noise=0)
        assert replay(samples) == ([first, second], 0, 0), ("maintenance turn", idle_us)
    results["completed_byte_boundary_cases"] = 6

    samples, _ = observations(values, phase=.37)
    # Broad light is impossible SIR framing. It must not stop acquisition or
    # hold a packet-wide phase; a subsequent first packet remains decodable.
    noise = bytearray([180] * 8 + [80] * 8 + [180] * 80)
    packets, rejected, gaps = replay(noise + samples[12:])
    assert packets == [values] and rejected and not gaps, "noise recovery"
    # A timing erasure discards the candidate; later packets still arrive.
    damaged = bytearray(samples)
    damaged[45] = 254
    packets, rejected, gaps = replay(damaged + samples)
    assert packets[-1] == values and gaps == 1, "erasure recovery"
    # A pulse in the stop cell may create a new serial start; the damaged
    # original byte is never accepted. Native framing validates any tail.
    samples, _ = observations(b"\x55", bad_stop=True, noise=0)
    packets, rejected, gaps = replay(samples)
    assert b"\x55" not in packets and rejected and not gaps, "stop pulse"
    # Buffer bounds reject an overlong contiguous packet rather than truncating
    # it into an apparently valid maximum-length frame.
    for length in [137, 144, 272]:
        samples, _ = observations(bytes(index % 256 for index in range(length)), noise=0)
        follow, _ = observations(values, noise=0)
        packets, rejected, gaps = replay(samples + follow)
        assert packets == [values] and rejected and not gaps, ("packet overflow", length)
    # Missing raw samples cannot be mistaken for uninterrupted darkness.
    samples, _ = observations(values, noise=0)
    stream = samples + samples
    indices = list(range(len(stream)))
    indices[45:] = [index + 7 for index in indices[45:]]
    packets, rejected, gaps = replay(stream, indices=indices)
    assert packets[-1] == values and gaps == 1, "index discontinuity"
    results["noise_erasure_stop_overflow_cases"] = 7
    samples, _ = observations(values, noise=0)
    for position in range(math.ceil((12 + 30 * len(values))) + 4):
        damaged = bytearray(samples)
        damaged[position] = 254
        packets, _, gaps = replay(damaged + samples)
        assert packets == [values] and gaps == 1, ("erasure position", position)
    results["erasure_position_cases"] = position + 1

    # A valid native ACK can also be interpreted as an entirely valid wrong
    # UART stream after added light. This is an information ambiguity, not a
    # framing error which a more persistent phase search can uniquely repair.
    # Check the native checksum independently before introducing the pulse.
    logical = bytes.fromhex("0401090801020304")
    def native_checksum(packet):
        total = 2 + sum(value << 8 if index % 2 == 0 else value
                        for index, value in enumerate(packet[:2] + b"\0\0" + packet[4:]))
        total = (total >> 16) + (total & 65535)
        return (total + (total >> 16)) & 65535

    assert int.from_bytes(logical[2:4], "little") == native_checksum(logical)
    wire = bytes(value ^ 0xAA for value in logical)
    ambiguous = []
    for lead in [26, 87]:
        samples, _ = observations(wire, phase=.3, noise=0, prefix_us=(500, lead))
        packets, rejected, gaps = replay(samples)
        assert not rejected and not gaps and packets[-1] != wire
        corrupted = bytes(value ^ 0xAA for value in packets[-1])
        # The complete native checksum must reject these alternate UART
        # interpretations before command dispatch or any state update.
        assert int.from_bytes(corrupted[2:4], "little") != native_checksum(corrupted)
        ambiguous.append({"prefix_us": [500, lead],
                          "original_native_ack": logical.hex(),
                          "delivered_wire_frames": [packet.hex() for packet in packets],
                          "native_rejection": "checksum mismatch",
                          "received_checksum": int.from_bytes(corrupted[2:4], "little"),
                          "computed_checksum": native_checksum(corrupted)})
    results["ambiguous_noise_is_not_repaired"] = ambiguous

    for phase_index in range(phases):
        samples, _ = observations(values, baseline=300, gain=400, censor=True,
                                  phase=phase_index / phases, noise=0)
        assert 255 in samples and replay(samples) == ([values], 0, 0), (
            "censored dark", phase_index)
    results["censored_dark_cases"] = phases
    samples, _ = observations(values)
    assert replay(samples, 0xFFFF_FFE0) == ([values], 0, 0), "gate wrap"
    results["wrapping_timestamp_cases"] = 1

    # A measured margin is still necessary. Report failures below the modeled
    # envelope instead of disguising missed light as guaranteed rejection.
    weak = []
    for gain in [0, 4, 10, 20, 40, 60]:
        correct = rejected = wrong = 0
        for phase_index in range(phases):
            data = b"\x00\xff\x55\xaa"
            samples, _ = observations(data, gain=gain, rate=1.01,
                                      phase=phase_index / phases, seed=1)
            packets, _, _ = replay(samples)
            correct += packets == [data]
            rejected += not packets
            wrong += bool(packets) and packets != [data]
        weak.append({"gain": gain, "correct": correct, "no_frame": rejected,
                     "wrong_uart_frames": wrong})
    results["outside_signal_envelope"] = weak
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phases", type=int, default=64)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.phases < 2 or args.phases % 2:
        parser.error("--phases must be a positive even number")
    sources = [ROOT / "stick/ir_rx_core.cpp", ROOT / "stick/ir_rx_core.h",
               ROOT / "stick/ir_gate.h",
               ROOT / "stick/audits/ir_rx_replay.cpp", Path(__file__).resolve()]
    hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sources}
    with tempfile.TemporaryDirectory(prefix="stick-ir-replay-") as directory:
        library = Path(directory) / "receiver.so"
        subprocess.run([
            "c++", "-std=c++17", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra",
            "-Werror", "-DPW_IR_REPLAY_LIBRARY",
            str(ROOT / "stick/ir_rx_core.cpp"),
            str(ROOT / "stick/audits/ir_rx_replay.cpp"), "-o", str(library),
        ], check=True)
        checks = exercise(Replay(library), args.phases)
    if hashes != {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sources}:
        raise RuntimeError("Receiver sources changed during replay; rerun before retaining evidence")
    results = {"scope": "Compiled firmware receiver; synthetic signal model, "
                        "not physical qualification",
               "source_sha256": hashes,
               "model": {"cpu_hz": CPU, "integration_cycles": [65, 450],
                         "noise_bound_counts": 1, "phase_samples": args.phases},
               "results": checks,
               "total_positive_checks": sum(value for name, value in checks.items()
                                            if name.endswith("_cases")),
               "passed": True}
    rendered = json.dumps(results, indent=2) + "\n"
    print(rendered, end="")
    if args.output:
        args.output.write_text(rendered)


if __name__ == "__main__":
    main()
