"""Moves a US recomp.json to an EU EBOOT with eu_match.py's map, and adds the EU-only entries.

usage: python3 -I tools/eu_recomp_json.py US_RECOMP_JSON MAP_JSON EU_EBOOT OUT_JSON
- Functions matched word for word move with their ranges, jump tables and indirect calls.
- Changed functions (entry known from their callers) keep only their entry: recomp.py's own walk
  builds the body, and their US jump tables are dropped.
- US functions not placed are left out. Their EU code is reached through the entries the EU EBOOT
  itself names: every relocated jal target, and every relocated code address (R_32 words, hi/lo
  pairs) outside the bodies matched word for word. A jump-table target found that way becomes an
  entry, and the jr reaching it goes through rt_call, which only costs speed.
"""
import json, struct, sys
from pathlib import Path

us_path, map_path, eu_path, out_path = sys.argv[1:5]
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recomp"))
from pspelf import PspModule, SHT_PSPREL, R_MIPS_26, R_MIPS_32  # noqa

g = json.load(open(us_path))
mp = json.load(open(map_path))
fmap = {int(a, 16): d for a, d in mp["fmap"].items()}
changed = {int(a, 16) for a in mp.get("changed", [])}

eu = PspModule.parse(open(eu_path, "rb").read())
eu.load(0)
t = eu.section(".text")
t_lo, t_hi = t.addr, t.addr + t.size
seg = [s.vaddr for s in eu.segments]
rel = {}
for sec in eu.sections:
    if sec.type != SHT_PSPREL:
        continue
    for i in range(sec.size // 8):
        off, info = struct.unpack_from("<II", eu.data, sec.offset + i * 8)
        rw = (info >> 8) & 0xFF
        if rw < len(seg):
            rel[off + seg[rw]] = info & 0xF


def mv(addr, d):
    return f"{int(addr, 16) + d:#x}"


out = dict(g)
out["program"] = eu_path.replace("\\", "/").split("/")[-1]
out["functions"], out["jumptables"], out["indirect_calls"] = [], [], []
entries, exact_body = set(), set()
spans = []  # (US lo, US hi, delta) of the bodies moved word for word
for f in g["functions"]:
    a = int(f["addr"], 16)
    if a not in fmap:
        continue
    d = fmap[a]
    e = a + d
    entries.add(e)
    if a in changed:
        out["functions"].append({"addr": f"{e:#x}", "name": f"FUN_{e:08x}", "ranges": [[f"{e:#x}", f"{e + 3:#x}"]]})
        continue
    out["functions"].append({**f, "addr": f"{e:#x}", "name": f["name"] if d == 0 else f"FUN_{e:08x}",
                             "ranges": [[mv(lo, d), mv(hi, d)] for lo, hi in f["ranges"]]})
    for lo, hi in f["ranges"]:
        exact_body.update(range(int(lo, 16) + d, int(hi, 16) + d + 1, 4))
        spans.append((int(lo, 16), int(hi, 16), d))
spans.sort()
starts = [s[0] for s in spans]


def us_delta(addr):
    import bisect
    k = bisect.bisect_right(starts, addr) - 1
    if k >= 0 and spans[k][0] <= addr <= spans[k][1]:
        return spans[k][2]
    return None


for key in ("jumptables", "indirect_calls"):
    for j in g[key]:
        d = us_delta(int(j["at"], 16))
        if d is not None:
            out[key].append({**j, "at": mv(j["at"], d), "targets": [mv(x, d) for x in j["targets"]]})

new = {}
for a, typ in rel.items():
    w = eu.u32(a)
    if typ == R_MIPS_26 and (w >> 26) in (2, 3) and t_lo <= a < t_hi:
        v, why = (w & 0x03FFFFFF) << 2, "jal" if (w >> 26) == 3 else "j"
    elif typ == R_MIPS_32:
        v, why = w, "pointer"
    else:
        continue
    if not (t_lo <= v < t_hi) or v & 3 or v in entries:
        continue
    if why == "j" or v in exact_body:  # j is a branch inside a function; inside a moved body is its own code
        continue
    new.setdefault(v, why)
for hi, lo, v in eu.hilo_values:
    if t_lo <= v < t_hi and not v & 3 and v not in entries and v not in exact_body:
        new.setdefault(v, "hi/lo")
for e in sorted(new):
    out["functions"].append({"addr": f"{e:#x}", "name": f"FUN_{e:08x}", "ranges": [[f"{e:#x}", f"{e + 3:#x}"]]})
out["functions"].sort(key=lambda f: int(f["addr"], 16))
json.dump(out, open(out_path, "w"), separators=(",", ":"))
kinds = {}
for why in new.values():
    kinds[why] = kinds.get(why, 0) + 1
print(f"{len(fmap)} US functions placed ({len(changed)} changed: entry only), "
      f"{len(g['functions']) - len(fmap)} left out; {len(new)} EU entries added {kinds}; "
      f"{len(out['jumptables'])} jump tables, {len(out['indirect_calls'])} indirect calls -> {out_path}")
