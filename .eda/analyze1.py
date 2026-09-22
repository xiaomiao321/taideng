import json, collections, sys

P = r"D:\files\26_Summer\taideng\.eda\dump.json"
d = json.load(open(P, encoding="utf-8"))
r = d["result"]
comps = r["comps"]

print("components:", len(comps), "nets:", r["netCount"])

lh = collections.Counter(c["layer"] for c in comps)
print("component layers:", dict(lh))

print("\n--- designator prefixes ---")
pref = collections.Counter()
for c in comps:
    dg = c["designator"] or "?"
    pref["".join(ch for ch in dg if ch.isalpha())] += 1
for k, v in sorted(pref.items()):
    print(f"  {k}: {v}")

print("\n--- components ---")
for c in sorted(comps, key=lambda x: (x["designator"] or "")):
    npads = len(c["pads"])
    print(f'{c["designator"]:>9} | L{c["layer"]} | {c["x"]:>10} {c["y"]:>10} | rot {c["rot"]:>5} | pads {npads:>3} | {c["footprint"]} | {c["name"]}')

print("\n--- nets ---")
for n in sorted(r["nets"], key=lambda x: x["net"]):
    print(f'{n["net"]:<28} ({n["count"]:>2})  {", ".join(n["pins"][:14])}{" ..." if n["count"]>14 else ""}')
