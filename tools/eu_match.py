"""Maps every recompiled US function to the EU EBOOT and reports counts only.

usage: python3 -I tools/eu_match.py US_EBOOT EU_EBOOT RECOMP_JSON [OUT_JSON]
Words are compared with relocated fields masked (R_26 -> opcode, HI16/LO16/16 -> top half,
R_32 -> whole word). A match is then checked by its jal targets (mapped through the
function map) and its hi/lo data addresses (must give one consistent delta per US address).
"""
import collections, json, struct, sys
from pathlib import Path

us_path, eu_path, rj_path = sys.argv[1:4]
out_path = sys.argv[4] if len(sys.argv) > 4 else None
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recomp"))
from pspelf import PspModule, SHT_PSPREL, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16, R_MIPS_16, R_MIPS_32  # noqa


def load(path):
    m = PspModule.parse(open(path, "rb").read())
    m.load(0)
    rel = {}
    seg = [s.vaddr for s in m.segments]
    for sec in m.sections:
        if sec.type != SHT_PSPREL:
            continue
        for i in range(sec.size // 8):
            off, info = struct.unpack_from("<II", m.data, sec.offset + i * 8)
            rw = (info >> 8) & 0xFF
            if rw < len(seg):
                rel[off + seg[rw]] = info & 0xF
    t = m.section(".text")
    n = t.size // 4
    words = list(struct.unpack_from("<%dI" % n, m.image, t.addr))
    masked = []
    for i, w in enumerate(words):
        typ = rel.get(t.addr + 4 * i)
        if typ == R_MIPS_26:
            w &= 0xFC000000
        elif typ in (R_MIPS_HI16, R_MIPS_LO16, R_MIPS_16):
            w &= 0xFFFF0000
        elif typ == R_MIPS_32:
            w = 0
        masked.append(w)
    hilo = {lo: v for hi, lo, v in m.hilo_values}  # lo16 addr -> full value
    return m, t.addr, words, masked, rel, hilo


us, us_t, us_w, us_m, us_rel, us_hilo = load(us_path)
eu, eu_t, eu_w, eu_m, eu_rel, eu_hilo = load(eu_path)
rj = json.load(open(rj_path))
funcs = [(int(f["addr"], 16), [(int(a, 16), int(b, 16)) for a, b in f["ranges"]]) for f in rj["functions"]]

K = 6
index = collections.defaultdict(list)
for i in range(len(eu_m) - K):
    index[tuple(eu_m[i:i + K])].append(i)


def seq(m, t, ranges, d=0):
    out = []
    for a, b in ranges:
        i, j = (a + d - t) // 4, (b + 1 + d - t) // 4
        if i < 0 or j > len(m):
            return None
        out += m[i:j]
    return out


fmap, ambiguous, missing = {}, [], []
last = 0
for addr, ranges in funcs:
    want = seq(us_m, us_t, ranges)
    if want is None:
        missing.append(addr); continue
    cands = []
    if seq(eu_m, eu_t, ranges, last) == want:
        cands = [last]
    else:
        first = (ranges[0][0] - us_t) // 4
        key = tuple(us_m[first:first + K])
        for p in index.get(key, []) if len(key) == K else []:
            d = eu_t + 4 * p - ranges[0][0]
            if seq(eu_m, eu_t, ranges, d) == want:
                cands.append(d)
    if len(cands) == 1:
        fmap[addr] = cands[0]; last = cands[0]
    elif len(cands) > 1:
        # prefer the delta closest to the previous one (code order is kept)
        cands.sort(key=lambda d: abs(d - last))
        ambiguous.append(addr); fmap[addr] = cands[0]; last = cands[0]
    else:
        missing.append(addr)


def jals(addr, ranges, d):
    """(US target, EU target) of every relocated jal in a function placed at delta d."""
    for a, b in ranges:
        for pc in range(a, b + 1, 4):
            w = us_w[(pc - us_t) // 4]
            if us_rel.get(pc) == R_MIPS_26 and (w >> 26) == 3:
                yield (w & 0x03FFFFFF) << 2, (eu_w[(pc + d - eu_t) // 4] & 0x03FFFFFF) << 2


# call evidence: the jal of a function matched word for word gives its target's EU entry,
# even when the target's body changed. Repeated while it places something new.
exact = set(fmap) - set(ambiguous)
changed, moved = set(), set()
fr = dict(funcs)
for _ in range(5):
    ev = collections.defaultdict(collections.Counter)
    for addr in exact:
        for tgt, etgt in jals(addr, fr[addr], fmap[addr]):
            ev[tgt][etgt] += 1
    new = 0
    for addr, c in ev.items():
        if addr not in fr or len(c) > 1:
            continue
        d = next(iter(c)) - addr
        if fmap.get(addr) == d and (addr in exact or addr in changed):
            continue
        if addr in fmap and fmap[addr] != d:
            moved.add(addr)
        fmap[addr] = d
        if seq(eu_m, eu_t, fr[addr], d) == seq(us_m, us_t, fr[addr]):
            exact.add(addr); changed.discard(addr)
        else:
            changed.add(addr); exact.discard(addr)
        new += 1
    if not new:
        break
ambiguous = [a for a in ambiguous if a not in exact and a not in changed]
missing = [a for a in missing if a not in fmap]
print(f"call evidence: {len(moved)} moved, {len(changed)} changed (entry from calls, body differs), "
      f"{len(ambiguous)} still ambiguous, {len(missing)} still not found")

# jal checks: a matched function's call targets must map through fmap
fmap_abs = {a: a + d for a, d in fmap.items()}
bad_calls, calls, bad_pairs = set(), 0, []
data_map = collections.defaultdict(collections.Counter)
for addr, ranges in funcs:
    if addr not in fmap or addr in changed:
        continue
    d = fmap[addr]
    for a, b in ranges:
        for pc in range(a, b + 1, 4):
            w = us_w[(pc - us_t) // 4]
            if us_rel.get(pc) == R_MIPS_26 and (w >> 26) == 3:
                tgt = (w & 0x03FFFFFF) << 2
                ew = eu_w[(pc + d - eu_t) // 4]
                etgt = (ew & 0x03FFFFFF) << 2
                calls += 1
                if tgt in fmap_abs and fmap_abs[tgt] != etgt:
                    bad_calls.add(addr); bad_pairs.append((addr, tgt, etgt))
            if pc in us_hilo and (pc + d) in eu_hilo:
                data_map[us_hilo[pc]][eu_hilo[pc + d]] += 1

conflict = {k: c for k, c in data_map.items() if len(c) > 1}
deltas = collections.Counter()
for k, c in data_map.items():
    deltas[c.most_common(1)[0][0] - k] += 1

tot_bytes = sum(b + 1 - a for _, r in funcs for a, b in r)
ok_bytes = sum(b + 1 - a for f, r in funcs if f in fmap and f not in changed for a, b in r)
print(f"text US {len(us_w)*4:#x}  EU {len(eu_w)*4:#x}  diff {4*(len(eu_w)-len(us_w)):+d} bytes")
print(f"functions {len(funcs)}: placed {len(fmap)} ({100*len(fmap)/len(funcs):.2f} %), {len(changed)} of them changed, "
      f"ambiguous {len(ambiguous)}, not found {len(missing)}")
print(f"bytes matched {100*ok_bytes/tot_bytes:.2f} %")
print(f"jal checked {calls}, functions with a call to the wrong place {len(bad_calls)}")
fd = collections.Counter(fmap.values())
print("code deltas (delta: functions):", ", ".join(f"{d:+#x}: {n}" for d, n in fd.most_common(12)),
      f"... {len(fd)} distinct")
print(f"data addresses seen {len(data_map)}, with more than one EU value {len(conflict)}")
print("data deltas (delta: addresses):", ", ".join(f"{d:+#x}: {n}" for d, n in deltas.most_common(12)),
      f"... {len(deltas)} distinct")
miss_sizes = sorted(((sum(b + 1 - a for a, b in r), f) for f, r in funcs if f in set(missing) | changed), reverse=True)
print("largest not found or changed (addr: bytes):", ", ".join(f"{f:#x}: {s}" for s, f in miss_sizes[:10]))
if out_path:
    json.dump({"fmap": {f"{a:#x}": d for a, d in fmap.items()}, "missing": [f"{a:#x}" for a in missing],
               "ambiguous": [f"{a:#x}" for a in ambiguous],
               "changed": [f"{a:#x}" for a in sorted(changed)], "bad_calls": [f"{a:#x}" for a in sorted(bad_calls)],
               "data": {f"{k:#x}": c.most_common(1)[0][0] - k for k, c in data_map.items()}},
              open(out_path, "w"))
kinds = collections.Counter()
mapped_eu = set(fmap_abs.values())
for caller, tgt, etgt in bad_pairs:
    kinds["EU calls another mapped function" if etgt in mapped_eu else "EU calls an unmapped place"] += 1
print("wrong calls:", len(bad_pairs), dict(kinds), "to", len({t for _, t, _ in bad_pairs}), "US targets")
