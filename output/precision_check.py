#!/usr/bin/env python3
"""
Independent double precision reference checker for the libsamplerate
single-vs-double precision work.

It is deliberately independent of the C implementation:

  * the test input signal is regenerated from the same mathematical
    description used by tests/double_precision_test.c,
  * linear, zero-order-hold and sinc polyphase converters are re-derived
    in Python using the documented algorithm and the project's own
    coefficient tables (parsed straight from src/*_coeffs.h),
  * every arithmetic operation runs in Python float == IEEE-754 double.

The hex-float dumps produced by the repo's double_precision_test in the
float and double internal precision builds are compared against each
other and against this reference.

Run with an isolated interpreter, e.g.:
    python3 -I precision_check.py dump_float.txt dump_double.txt \\
            ../src report.json report.txt
"""

import json
import math
import os
import re
import struct
import sys

INPUT_FRAMES = 1024
STREAM_CHUNK = 257
SHIFT_BITS = 12
FP_ONE = float(1 << SHIFT_BITS)
INV_FP_ONE = 1.0 / FP_ONE
FP_MASK = (1 << SHIFT_BITS) - 1
SRC_MAX_RATIO = 256

CONVERTERS = {
    0: ("SINC_BEST", "high_qual_coeffs.h"),
    1: ("SINC_MEDIUM", "mid_qual_coeffs.h"),
    2: ("SINC_FASTEST", "fastest_coeffs.h"),
    3: ("ZOH", None),
    4: ("LINEAR", None),
}


# --------------------------------------------------------------------------- #
# Small fixed point helpers, mirroring src_sinc.c (round to nearest even).
# --------------------------------------------------------------------------- #

def lrint(x):
    return int(round(x))


def double_to_fp(x):
    return lrint(x * FP_ONE)


def int_to_fp(x):
    return x << SHIFT_BITS


def fp_to_int(x):
    return x >> SHIFT_BITS


def fp_fraction(x):
    return x & FP_MASK


def fp_to_double(x):
    return float(fp_fraction(x)) * INV_FP_ONE


def fmod_one(x):
    res = x - lrint(x)
    if res < 0.0:
        res += 1.0
    return res


# --------------------------------------------------------------------------- #
# Input signal, identical description to double_precision_test.c::gen_input.
# --------------------------------------------------------------------------- #

def gen_input(frames, channels):
    freqs = (0.017, 0.049, 0.113, 0.227)
    out = [0.0] * (frames * channels)
    for frame in range(frames):
        for ch in range(channels):
            value = 0.0
            for k, freq in enumerate(freqs):
                phase = 2.0 * math.pi * freq * frame + 0.37 * ch * (k + 1)
                value += 0.22 * math.sin(phase + 0.11 * k)
            value += 0.05 * math.sin(2.0 * math.pi * (0.05 + 0.0009 * frame) * frame)
            # C stores into a float array, so round the same way.
            out[frame * channels + ch] = f32(value)
    return out


def f32(x):
    """Round a python double to the nearest binary32 value, returned as double."""
    return struct.unpack("f", struct.pack("f", x))[0]


def float_ulp(x):
    packed = struct.unpack("I", struct.pack("f", x))[0]
    if packed == 0:
        packed = 1
    nxt = struct.unpack("f", struct.pack("I", packed + 1))[0]
    return abs(nxt - x)


# --------------------------------------------------------------------------- #
# Coefficient table parsing.
# --------------------------------------------------------------------------- #

def load_coeffs(src_dir, header_name):
    path = os.path.join(src_dir, header_name)
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    # The initializer reads "} table = { <increment>, { <coeffs> ...". Grab
    # the increment first so the coefficient array dimension cannot be mixed
    # into the numeric tokens; coefficient literals always contain a dot.
    inc_match = re.search(r"=\s*\{\s*(\d+)\s*,", text)
    increment = int(inc_match.group(1))
    coeffs = [float(tok) for tok in re.findall(r"[+-]?\d+\.\d+(?:[eE][+-]?\d+)?", text)]
    return increment, coeffs


# --------------------------------------------------------------------------- #
# Streaming driver, same chunking / end_of_input protocol as the C test.
# --------------------------------------------------------------------------- #

def stream(engine_factory, channels, ratio):
    engine = engine_factory(channels)
    signal = gen_input(INPUT_FRAMES, channels)
    output = []
    in_offset = 0
    while in_offset < INPUT_FRAMES:
        chunk = min(STREAM_CHUNK, INPUT_FRAMES - in_offset)
        eoi = (in_offset + chunk == INPUT_FRAMES)
        part = signal[in_offset * channels:(in_offset + chunk) * channels]
        produced, used = engine.process(part, chunk, ratio, eoi)
        output.extend(produced)
        # A call may consume only part of the offered chunk; the remainder is
        # re-offered on the next iteration, exactly as in the C test loop.
        in_offset += used
        if eoi and not produced:
            break
    # Drain any termination samples the engine still owes after the last chunk.
    while True:
        produced, used = engine.process([], 0, ratio, True)
        output.extend(produced)
        if not produced:
            break
    return output


# --------------------------------------------------------------------------- #
# Zero order hold reference (port of src_zoh.c, all arithmetic in double).
# --------------------------------------------------------------------------- #

class ZohRef:
    def __init__(self, channels):
        self.ch = channels
        self.last_value = [0.0] * channels
        self.dirty = False
        self.last_position = 0.0
        self.last_ratio = 0.0

    def process(self, data, in_frames, ratio, eoi):
        ch = self.ch
        in_count = in_frames * ch
        out = []
        if in_frames <= 0:
            return out, 0
        if not self.dirty:
            self.last_value = list(data[:ch])
            self.dirty = True
        src_ratio = self.last_ratio or ratio
        input_index = self.last_position
        in_used = 0
        while input_index < 1.0:
            if in_used + ch * input_index >= in_count:
                break
            for c in range(ch):
                out.append(self.last_value[c])
            input_index += 1.0 / src_ratio
        rem = fmod_one(input_index)
        in_used += ch * lrint(input_index - rem)
        input_index = rem
        while in_used + ch * input_index <= in_count:
            for c in range(ch):
                out.append(data[in_used - ch + c])
            input_index += 1.0 / src_ratio
            rem = fmod_one(input_index)
            in_used += ch * lrint(input_index - rem)
            input_index = rem
        if in_used > in_count:
            input_index += (in_used - in_count) / ch
            in_used = in_count
        self.last_position = input_index
        if in_used > 0:
            for c in range(ch):
                self.last_value[c] = data[in_used - ch + c]
        self.last_ratio = src_ratio
        return out, in_used // ch


# --------------------------------------------------------------------------- #
# Linear reference (port of src_linear.c, all arithmetic in double).
# --------------------------------------------------------------------------- #

class LinearRef:
    def __init__(self, channels):
        self.ch = channels
        self.last_value = [0.0] * channels
        self.dirty = False
        self.last_position = 0.0
        self.last_ratio = 0.0

    def process(self, data, in_frames, ratio, eoi):
        ch = self.ch
        in_count = in_frames * ch
        out = []
        if in_frames <= 0:
            return out, 0
        if not self.dirty:
            self.last_value = list(data[:ch])
            self.dirty = True
        src_ratio = self.last_ratio or ratio
        input_index = self.last_position
        in_used = 0
        while input_index < 1.0:
            if in_used + ch * (1.0 + input_index) >= in_count:
                break
            for c in range(ch):
                v = self.last_value[c] + input_index * (data[c] - self.last_value[c])
                out.append(v)
            input_index += 1.0 / src_ratio
        rem = fmod_one(input_index)
        in_used += ch * lrint(input_index - rem)
        input_index = rem
        while in_used + ch * input_index < in_count:
            for c in range(ch):
                a = data[in_used - ch + c]
                b = data[in_used + c]
                out.append(a + input_index * (b - a))
            input_index += 1.0 / src_ratio
            rem = fmod_one(input_index)
            in_used += ch * lrint(input_index - rem)
            input_index = rem
        if in_used > in_count:
            input_index += (in_used - in_count) / ch
            in_used = in_count
        self.last_position = input_index
        if in_used > 0:
            for c in range(ch):
                self.last_value[c] = data[in_used - ch + c]
        self.last_ratio = src_ratio
        return out, in_used // ch


# --------------------------------------------------------------------------- #
# Sinc polyphase reference (mono port of src_sinc.c ring buffer + FIR engine).
# --------------------------------------------------------------------------- #

class SincRef:
    """Generic N-channel port of src_sinc.c (prepare_data + calc_output_multi)."""

    def __init__(self, channels, coeffs, index_inc):
        self.ch = channels
        self.coeffs = coeffs
        self.index_inc = index_inc
        self.half_len = len(coeffs) - 2
        b_len = 3 * lrint((self.half_len + 2.0) / index_inc * SRC_MAX_RATIO + 1)
        self.b_len = max(b_len, 4096) * channels + 1
        self.reset()

    def reset(self):
        self.buf = [0.0] * (self.b_len + self.ch)
        self.b_current = 0
        self.b_end = 0
        self.b_real_end = -1
        self.last_position = 0.0
        self.last_ratio = 0.0

    @staticmethod
    def _int_div_ceil(divident, divisor):
        return (divident + (divisor - 1)) // divisor

    def _prepare_data(self, data, in_count, in_used, half, eoi):
        ch = self.ch
        if self.b_real_end >= 0:
            return in_used
        if self.b_current == 0:
            length = self.b_len - 2 * half
            self.b_current = self.b_end = half
        elif self.b_end + half + ch < self.b_len:
            length = max(self.b_len - self.b_current - half, 0)
        else:
            length = self.b_end - self.b_current
            start = self.b_current - half
            segment = self.buf[start:start + half + length]
            self.buf[0:len(segment)] = segment
            self.b_current = half
            self.b_end = self.b_current + length
            length = max(self.b_len - self.b_current - half, 0)
        length = min(in_count - in_used, length)
        length -= length % ch
        for i in range(length):
            self.buf[self.b_end + i] = data[in_used + i]
        self.b_end += length
        in_used += length
        if in_used == in_count and self.b_end - self.b_current < 2 * half and eoi:
            if self.b_len - self.b_end < half + 5:
                length2 = self.b_end - self.b_current
                start = self.b_current - half
                segment = self.buf[start:start + half + length2]
                self.buf[0:len(segment)] = segment
                self.b_current = half
                self.b_end = self.b_current + length2
            self.b_real_end = self.b_end
            zlen = half + 5
            if zlen < 0 or self.b_end + zlen > self.b_len:
                zlen = self.b_len - self.b_end
            for i in range(zlen):
                self.buf[self.b_end + i] = 0.0
            self.b_end += zlen
        return in_used

    def _calc_output(self, increment, start_filter_index, scale):
        ch = self.ch
        max_filter_index = int_to_fp(self.half_len)

        filter_index = start_filter_index
        coeff_count = (max_filter_index - filter_index) // increment
        filter_index = filter_index + coeff_count * increment
        data_index = self.b_current - ch * coeff_count
        if data_index < 0:
            # Mono uses the raw underflow distance; the generic path divides
            # by the channel count, matching src_sinc.c's per-specialisation code.
            steps = -data_index if ch == 1 else self._int_div_ceil(-data_index, ch)
            filter_index -= increment * steps
            data_index += steps * ch
        left = [0.0] * ch
        while filter_index >= 0:
            fraction = fp_to_double(filter_index)
            indx = fp_to_int(filter_index)
            icoeff = self.coeffs[indx] + fraction * (self.coeffs[indx + 1] - self.coeffs[indx])
            for c in range(ch):
                left[c] += icoeff * self.buf[data_index + c]
            filter_index -= increment
            data_index += ch

        filter_index = increment - start_filter_index
        coeff_count = (max_filter_index - filter_index) // increment
        filter_index = filter_index + coeff_count * increment
        data_index = self.b_current + ch * (1 + coeff_count)
        right = [0.0] * ch
        while True:
            fraction = fp_to_double(filter_index)
            indx = fp_to_int(filter_index)
            icoeff = self.coeffs[indx] + fraction * (self.coeffs[indx + 1] - self.coeffs[indx])
            for c in range(ch):
                right[c] += icoeff * self.buf[data_index + c]
            filter_index -= increment
            data_index -= ch
            if filter_index <= 0:
                break
        return [scale * (left[c] + right[c]) for c in range(ch)]

    def process(self, data, in_frames, ratio, eoi):
        ch = self.ch
        in_count = in_frames * ch
        out = []
        in_used = 0
        src_ratio = self.last_ratio or ratio
        count = (self.half_len + 2.0) / self.index_inc
        if min(self.last_ratio or ratio, ratio) < 1.0:
            count /= min(self.last_ratio or ratio, ratio)
        half_filter = ch * (lrint(count) + 1)
        input_index = self.last_position
        rem = fmod_one(input_index)
        self.b_current = (self.b_current + ch * lrint(input_index - rem)) % self.b_len
        input_index = rem
        terminate = 1.0 / src_ratio + 1e-20
        while True:
            samples_in_hand = (self.b_end - self.b_current + self.b_len) % self.b_len
            if samples_in_hand <= half_filter:
                in_used = self._prepare_data(data, in_count, in_used, half_filter, eoi)
                samples_in_hand = (self.b_end - self.b_current + self.b_len) % self.b_len
                if samples_in_hand <= half_filter:
                    break
            if self.b_real_end >= 0:
                # calc_output_single (mono) uses strict '>' while the stereo /
                # quad / hex / generic paths use '>='.
                if ch == 1:
                    if self.b_current + input_index + terminate > self.b_real_end:
                        break
                elif self.b_current + input_index + terminate >= self.b_real_end:
                    break
            float_increment = self.index_inc * (src_ratio if src_ratio < 1.0 else 1.0)
            increment = double_to_fp(float_increment)
            start = double_to_fp(input_index * float_increment)
            out.extend(self._calc_output(increment, start, float_increment / self.index_inc))
            input_index += 1.0 / src_ratio
            rem = fmod_one(input_index)
            self.b_current = (self.b_current + ch * lrint(input_index - rem)) % self.b_len
            input_index = rem
        self.last_position = input_index
        self.last_ratio = src_ratio
        return out, in_used // ch


# --------------------------------------------------------------------------- #
# Dump parsing and comparison.
# --------------------------------------------------------------------------- #

def parse_dump(path):
    cases = {}
    order = []
    precision = None
    header = None
    values = None
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        if line.startswith("# precision:"):
            precision = line.split(":", 1)[1].strip()
            continue
        if line.startswith("CASE"):
            if header is not None:
                cases[header] = values
            parts = line.split()
            conv = int(parts[1])
            ratio = float(parts[2])
            ch = int(parts[3])
            in_frames = int(parts[4])
            out_frames = int(parts[5])
            header = (conv, ratio, ch, in_frames, out_frames)
            order.append(header)
            values = []
            continue
        values.append(float.fromhex(line))
    if header is not None:
        cases[header] = values
    return precision, order, cases


def difference_stats(ref_values, other_values):
    n = len(ref_values)
    max_abs = 0.0
    max_rel = 0.0
    sum_sq_diff = 0.0
    sum_sq_sig = 0.0
    exact = 0
    max_ulp = 0.0
    for a, b in zip(ref_values, other_values):
        d = abs(a - b)
        if a == b:
            exact += 1
        if d > max_abs:
            max_abs = d
        denom = max(abs(a), 1e-300)
        rel = d / denom
        if rel > max_rel:
            max_rel = rel
        sum_sq_diff += d * d
        sum_sq_sig += a * a
        if d != 0.0:
            ulp = d / max(float_ulp(a), 1e-45)
            max_ulp = max(max_ulp, ulp)
    rms_diff = math.sqrt(sum_sq_diff / n)
    rms_sig = math.sqrt(sum_sq_sig / n)
    snr_db = 20.0 * math.log10(rms_sig / rms_diff) if rms_diff > 0.0 else float("inf")
    return {
        "samples": n,
        "exact_bit_matches": exact,
        "exact_match_pct": 100.0 * exact / n,
        "max_abs_diff": max_abs,
        "max_rel_diff": max_rel,
        "max_diff_ulp_f32": max_ulp,
        "rms_diff": rms_diff,
        "diff_snr_db": snr_db,
    }


def main():
    float_dump, double_dump, src_dir, json_path, txt_path = sys.argv[1:6]
    prec_f, order_f, cases_f = parse_dump(float_dump)
    prec_d, order_d, cases_d = parse_dump(double_dump)

    assert prec_f == "float" and prec_d == "double"
    assert order_f == order_d, "case layout differs between precision dumps"

    coeff_cache = {}

    def coeffs_for(conv):
        if conv not in coeff_cache:
            _, header = CONVERTERS[conv]
            coeff_cache[conv] = load_coeffs(src_dir, header)
        return coeff_cache[conv]

    # Every case is cross-checked against the independent reference:
    # linear/zoh for all channels and ratios, sinc for all quality tiers,
    # channels and ratios.
    report = {"cases": [], "reference_failures": [], "global": {}}
    worst_float_ulp = 0.0
    worst_double_ulp = 0.0
    total_exact = 0
    total_samples = 0

    for header in order_f:
        conv, ratio, ch, in_frames, out_frames = header
        name = CONVERTERS[conv][0]
        out_f = cases_f[header]
        out_d = cases_d[header]
        assert len(out_f) == len(out_d) == out_frames * ch

        stats = difference_stats(out_d, out_f)
        entry = {"case": {"converter": name, "conv_id": conv, "ratio": ratio,
                          "channels": ch, "out_frames": out_frames},
                 "float_vs_double": stats}
        total_exact += stats["exact_bit_matches"]
        total_samples += stats["samples"]

        use_reference = False
        if conv == 3:
            ref = stream(lambda c: ZohRef(c), ch, ratio)
            use_reference = True
        elif conv == 4:
            ref = stream(lambda c: LinearRef(c), ch, ratio)
            use_reference = True
        else:
            inc, table = coeffs_for(conv)
            ref = stream(lambda c: SincRef(c, table, inc), ch, ratio)
            use_reference = True

        if use_reference:
            assert len(ref) == len(out_d), (
                "reference length mismatch on %s ch=%d ratio=%g: %d vs %d"
                % (name, ch, ratio, len(ref), len(out_d)))
            # The library narrows its internal result to binary32 at the API
            # boundary, so compare against the float32-rounded reference too.
            ref_f32 = [f32(x) for x in ref]
            dbl_vs_ref = difference_stats(ref_f32, out_d)
            flt_vs_ref = difference_stats(ref_f32, out_f)
            entry["double_vs_reference"] = dbl_vs_ref
            entry["float_vs_reference"] = flt_vs_ref
            worst_double_ulp = max(worst_double_ulp, dbl_vs_ref["max_diff_ulp_f32"])
            worst_float_ulp = max(worst_float_ulp, flt_vs_ref["max_diff_ulp_f32"])
            if dbl_vs_ref["max_diff_ulp_f32"] > 2.0:
                report["reference_failures"].append(
                    {"converter": name, "ratio": ratio, "channels": ch,
                     "max_ulp": dbl_vs_ref["max_diff_ulp_f32"]})
        report["cases"].append(entry)

    referenced = [c for c in report["cases"] if "double_vs_reference" in c]
    assert referenced, "no cases were cross-checked against the reference"

    def agg(key, sub):
        vals = [c[sub][key] for c in referenced]
        return {"max": max(vals), "mean": sum(vals) / len(vals)}

    report["global"] = {
        "cases_compared": len(order_f),
        "cases_reference_checked": len(referenced),
        "float_vs_double_exact_pct": 100.0 * total_exact / total_samples,
        "double_path_max_ulp_vs_ref": worst_double_ulp,
        "float_path_max_ulp_vs_ref": worst_float_ulp,
        "double_path_rel_vs_ref": agg("max_rel_diff", "double_vs_reference"),
        "float_path_rel_vs_ref": agg("max_rel_diff", "float_vs_reference"),
        "reference_failures": len(report["reference_failures"]),
    }

    with open(json_path, "w") as fh:
        json.dump(report, fh, indent=2)

    lines = []
    g = report["global"]
    lines.append("libsamplerate precision verification report")
    lines.append("=" * 60)
    lines.append("cases compared (float build vs double build) : %d" % g["cases_compared"])
    lines.append("cases cross-checked vs independent reference : %d" % g["cases_reference_checked"])
    lines.append("float-vs-double bit-exact output share       : %.4f%%" % g["float_vs_double_exact_pct"])
    lines.append("double path max error vs reference (f32 ULP) : %.3f" % g["double_path_max_ulp_vs_ref"])
    lines.append("float  path max error vs reference (f32 ULP) : %.3f" % g["float_path_max_ulp_vs_ref"])
    lines.append("double path max rel error vs reference       : %.3e (mean %.3e)"
                 % (g["double_path_rel_vs_ref"]["max"], g["double_path_rel_vs_ref"]["mean"]))
    lines.append("float  path max rel error vs reference       : %.3e (mean %.3e)"
                 % (g["float_path_rel_vs_ref"]["max"], g["float_path_rel_vs_ref"]["mean"]))
    lines.append("reference failures                           : %d" % g["reference_failures"])
    lines.append("")
    header = "%-14s %5s %3s %8s | %13s %13s %10s | %10s %10s" % (
        "converter", "ratio", "ch", "frames", "max|d-f|", "max rel", "SNR(dB)", "ulp d/ref", "ulp f/ref")
    lines.append(header)
    lines.append("-" * len(header))
    for entry in report["cases"]:
        c = entry["case"]
        s = entry["float_vs_double"]
        row = "%-14s %5.2f %3d %8d | %13.3e %13.3e %10.3f |" % (
            c["converter"], c["ratio"], c["channels"], c["out_frames"],
            s["max_abs_diff"], s["max_rel_diff"], s["diff_snr_db"])
        if "double_vs_reference" in entry:
            row += " %10.3f %10.3f" % (entry["double_vs_reference"]["max_diff_ulp_f32"],
                                       entry["float_vs_reference"]["max_diff_ulp_f32"])
        else:
            row += "        n/a        n/a"
        lines.append(row)
    text = "\n".join(lines) + "\n"
    with open(txt_path, "w") as fh:
        fh.write(text)
    print(text)

    if report["reference_failures"]:
        print("REFERENCE CHECK FAILED", file=sys.stderr)
        return 1
    print("ALL REFERENCE CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
