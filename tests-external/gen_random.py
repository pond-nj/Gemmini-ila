#!/usr/bin/env python3
"""Write a constrained-random Gemmini program as a gemmini_diff trace.

Usage:
  gen_random.py --dim 16 --seed 1 [--length 40] [--ops mvin,mvout,...] > prog.trace

Every program is legal for libgemmini (it asserts on some illegal configs). Field values
lean on the encodings that are easy to get wrong in a spec: GARBAGE_ADDR operands, the
accumulator address flags (bit 30 accumulate, bit 29 full-width read), float scales,
transposes, ReLU, shifts, strides and partial rows/cols.

--ops picks the instructions after the prologue (default: all of OPS). The prologue always
configures the execute, load (all three pipelines) and store units.
"""
import argparse
import random
import struct

GARBAGE = 0xFFFFFFFF
ACC = 0x80000000
ACCUMULATE = 0x40000000
FULL = 0x20000000
IN_BASE = 0x10000   # random input bytes live here
OUT_BASE = 0x80000  # mvout targets

OPS = ["config_ex", "config_mvin", "config_mvout", "mvin", "mvin2", "mvin3", "mvout", "matmul", "flush"]
FUNCT = {"config": 0, "mvin2": 1, "mvin": 2, "mvout": 3, "compute.preloaded": 4,
         "compute.accumulated": 5, "preload": 6, "flush": 7, "mvin3": 14}


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def operand(local, rows, cols):
    return (rows << 48) | (cols << 32) | local


class Gen:
    def __init__(self, dim, seed):
        self.dim = dim
        self.r = random.Random(seed)
        self.lines = []
        self.region_size = 64 * dim * dim  # bytes at IN_BASE and at OUT_BASE
        self.mvin_stride = [dim] * 3
        self.shrunk = [0] * 3

    def emit(self, name, rs1, rs2):
        self.lines.append(f"{name:<22} funct={FUNCT[name]:<3} rs1=0x{rs1:016x} rs2=0x{rs2:016x}")

    def pick(self, *weighted):
        """pick((w, v), ...): v with probability proportional to w."""
        return self.r.choices([v for _, v in weighted], [w for w, _ in weighted])[0]

    def scale(self):
        return f32(self.pick((6, 1.0), (1, 0.5), (1, 2.0), (1, 0.0), (1, -1.0)))

    def size(self):
        return self.pick((3, self.dim), (1, self.r.randint(1, self.dim)))

    def sp_row(self):
        return self.r.randrange(0, 8 * self.dim)

    def acc_row(self):
        return self.r.randrange(0, 4 * self.dim)

    def fill_input(self):
        for off in range(0, self.region_size, 64):
            data = bytes(self.r.randrange(256) for _ in range(64))
            self.lines.append(f".mem 0x{IN_BASE + off:x} {data.hex()}")

    def config_ex(self):
        ws = self.r.random() < 0.6
        a_t, b_t = self.r.random() < 0.2, self.r.random() < 0.2
        if not ws and not a_t and b_t:  # illegal in OS mode
            b_t = False
        if ws and a_t and b_t:  # illegal in WS mode
            a_t = False
        relu = self.r.random() < 0.2
        shift = 0 if ws else self.pick((3, 0), (1, self.r.randint(1, 8)))
        c_stride = self.pick((4, 1), (1, 2))
        rs1 = (ws << 2) | (relu << 3) | (a_t << 8) | (b_t << 9) | (1 << 16) | (f32(1.0) << 32)
        self.emit("config", rs1, (c_stride << 48) | shift)

    def config_mvin(self, pipeline=None):
        pid = self.r.randrange(3) if pipeline is None else pipeline
        self.shrunk[pid] = int(self.r.random() < 0.3)
        self.mvin_stride[pid] = self.pick((2, self.dim), (2, 4 * self.dim), (1, 2 * self.dim), (1, 1))
        rs1 = 1 | (self.shrunk[pid] << 2) | (pid << 3) | (self.dim << 16) | (self.scale() << 32)
        self.emit("config", rs1, self.mvin_stride[pid])

    def config_mvout(self):
        relu = self.r.random() < 0.2
        stride = self.pick((2, self.dim), (1, 4 * self.dim), (1, 2 * self.dim))
        self.emit("config", 2 | (relu << 2), (self.scale() << 32) | stride)

    def mvin(self, name="mvin"):
        pid = {"mvin": 0, "mvin2": 1, "mvin3": 2}[name]
        rows = self.size()
        if self.r.random() < 0.4:
            local, cols = ACC | self.acc_row(), self.size()
            if self.r.random() < 0.3:
                local |= ACCUMULATE
            elem = 1 if self.shrunk[pid] else 4
        else:
            local, cols = self.sp_row(), self.pick((3, self.size()), (1, 2 * self.dim))
            elem = 1
        span = (rows - 1) * self.mvin_stride[pid] + cols * elem
        dram = 0 if self.r.random() < 0.1 else IN_BASE + self.r.randrange(self.region_size - span)
        self.emit(name, dram, operand(local, rows, cols))

    def mvin2(self):
        self.mvin("mvin2")

    def mvin3(self):
        self.mvin("mvin3")

    def flush(self):
        self.emit("flush", 0, 0)

    def mvout(self):
        rows, cols = self.size(), self.size()
        if self.r.random() < 0.5:
            local = ACC | self.acc_row() | (FULL if self.r.random() < 0.3 else 0)
        else:
            local = self.sp_row()
        dram = OUT_BASE + self.r.randrange(self.region_size // 2)
        self.emit("mvout", dram, operand(local, rows, cols))

    def output(self):
        kind = self.pick((4, "acc"), (2, "sp"), (1, "garbage"))
        if kind == "garbage":
            return GARBAGE
        if kind == "sp":
            return self.sp_row()
        return ACC | self.acc_row() | (ACCUMULATE if self.r.random() < 0.3 else 0)

    def matmul(self):
        """preload + compute.preloaded, sometimes followed by preload + compute.accumulated."""
        bd = self.sp_row() if self.r.random() < 0.8 else GARBAGE
        self.emit("preload", operand(bd, self.size(), self.size()), operand(self.output(), self.size(), self.size()))
        d = self.sp_row() if self.r.random() < 0.6 else GARBAGE
        self.emit("compute.preloaded", operand(self.sp_row(), self.size(), self.size()), operand(d, self.size(), self.size()))
        if self.r.random() < 0.4:
            self.emit("preload", operand(GARBAGE, self.dim, self.dim), operand(self.output(), self.size(), self.size()))
            d = self.sp_row() if self.r.random() < 0.6 else GARBAGE
            self.emit("compute.accumulated", operand(self.sp_row(), self.size(), self.size()), operand(d, self.size(), self.size()))

    def run(self, length, ops):
        self.fill_input()
        self.config_ex()
        for pid in range(3):
            self.config_mvin(pid)
        self.config_mvout()
        for _ in range(length):
            getattr(self, self.r.choice(ops))()
        return "\n".join(self.lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dim", type=int, required=True, help="Gemmini DIM; must match the gemmini_diff build")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--length", type=int, default=40, help="instructions after the prologue")
    ap.add_argument("--ops", default=",".join(OPS), help="comma-separated subset of: " + ",".join(OPS))
    a = ap.parse_args()
    ops = a.ops.split(",")
    unknown = set(ops) - set(OPS)
    if unknown:
        ap.error("unknown ops: " + ",".join(sorted(unknown)))
    print(f"# gen_random.py --dim {a.dim} --seed {a.seed} --length {a.length} --ops {a.ops}")
    print(Gen(a.dim, a.seed).run(a.length, ops), end="")


if __name__ == "__main__":
    main()
