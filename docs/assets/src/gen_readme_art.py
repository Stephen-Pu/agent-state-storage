#!/usr/bin/env python3
"""Generate the README artwork (light and dark SVGs) into docs/assets/.

Run from the repo root:  python3 docs/assets/src/gen_readme_art.py
Plain SVG + CSS animation only, no external fonts or scripts, so GitHub's
image proxy renders it. Motion is disabled under prefers-reduced-motion.
"""
import os

OUT = os.path.join(os.path.dirname(__file__), "..")
FONT = "Inter, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace"

THEMES = {
    "dark": dict(bg1="#05080F", bg2="#0B1530", grid="#1C2B4D", panel="#0E1830",
                 panel2="#111E3B", stroke="#2A3D66", text="#EAF0FB", sub="#9DB0D3",
                 dim="#61739A", kv="#22D3EE", ws="#A78BFA", mem="#FBBF24",
                 red="#F87171", green="#34D399", gray="#64748B", glow_op="0.55"),
    "light": dict(bg1="#FFFFFF", bg2="#EDF3FC", grid="#D3DEEE", panel="#FFFFFF",
                  panel2="#F5F8FD", stroke="#C3D0E4", text="#0A1324", sub="#46597A",
                  dim="#8393AE", kv="#0891B2", ws="#7C3AED", mem="#D97706",
                  red="#DC2626", green="#059669", gray="#94A3B8", glow_op="0.22"),
}


def style(t):
    return f"""<style>
  .t  {{ font-family: {FONT}; fill: {t['text']}; }}
  .s  {{ font-family: {FONT}; fill: {t['sub']}; }}
  .d  {{ font-family: {FONT}; fill: {t['dim']}; }}
  .m  {{ font-family: {MONO}; }}
  .flow  {{ stroke-dasharray: 5 9; animation: flow 1.6s linear infinite; }}
  .pulse {{ animation: pulse 2.8s ease-in-out infinite; }}
  @keyframes flow  {{ to {{ stroke-dashoffset: -28; }} }}
  @keyframes pulse {{ 0%,100% {{ opacity: .25; }} 50% {{ opacity: 1; }} }}
  @media (prefers-reduced-motion: reduce) {{
    .flow, .pulse {{ animation: none; }}
  }}
</style>"""


def defs(t, w, h):
    return f"""<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="{t['bg1']}"/><stop offset="1" stop-color="{t['bg2']}"/>
  </linearGradient>
  <linearGradient id="brand" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{t['kv']}"/><stop offset=".5" stop-color="{t['ws']}"/>
    <stop offset="1" stop-color="{t['mem']}"/>
  </linearGradient>
  <pattern id="grid" width="32" height="32" patternUnits="userSpaceOnUse">
    <path d="M32 0H0V32" fill="none" stroke="{t['grid']}" stroke-width="1" opacity=".55"/>
  </pattern>
  <radialGradient id="aura" cx=".5" cy=".5" r=".5">
    <stop offset="0" stop-color="{t['ws']}" stop-opacity="{float(t['glow_op'])*0.55:.2f}"/>
    <stop offset=".55" stop-color="{t['kv']}" stop-opacity="{float(t['glow_op'])*0.18:.2f}"/>
    <stop offset="1" stop-color="{t['kv']}" stop-opacity="0"/>
  </radialGradient>
  <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
    <feGaussianBlur stdDeviation="7"/>
  </filter>
</defs>
<rect width="{w}" height="{h}" rx="18" fill="url(#bg)"/>
<rect width="{w}" height="{h}" rx="18" fill="url(#grid)"/>"""


def capsule(t, x, y, halo=None, crashed=False):
    """A session snapshot: three lanes (KV, workspace, memory) in one capsule."""
    out = [f'<g transform="translate({x},{y})">']
    if halo:
        out.append(f'<circle r="30" fill="{halo}" opacity="{t["glow_op"]}" filter="url(#glow)" class="pulse"/>')
    out.append(f'<rect x="-36" y="-17" width="72" height="34" rx="17" fill="{t["panel"]}" '
               f'stroke="{t["red"] if crashed else t["stroke"]}" stroke-width="1.5"/>')
    for dx, c in ((-17, t["kv"]), (0, t["ws"]), (17, t["mem"])):
        out.append(f'<circle cx="{dx}" cy="0" r="5.5" fill="{t["gray"] if crashed else c}"/>')
    if crashed:
        out.append(f'<path d="M-9-9L9 9M9-9L-9 9" stroke="{t["red"]}" stroke-width="3" stroke-linecap="round"/>')
    out.append("</g>")
    return "".join(out)


def edge(t, x1, y1, x2, y2, color=None, width=2):
    c = color or t["stroke"]
    mx = (x1 + x2) / 2
    d = f"M{x1} {y1}C{mx} {y1} {mx} {y2} {x2} {y2}"
    return (f'<g><path d="{d}" fill="none" stroke="{t["stroke"]}" stroke-width="{width}"/>'
            f'<path d="{d}" fill="none" stroke="{c}" stroke-width="{width}" class="flow"/></g>')


def chip(t, x, y, label, color, w=None, h=34, fs=15, mono=False):
    w = w or (len(label) * fs * 0.6 + 34)
    fam = "m" if mono else ""
    return (f'<g transform="translate({x},{y})"><rect width="{w:.0f}" height="{h}" rx="{h/2}" '
            f'fill="{color}" fill-opacity=".12" stroke="{color}" stroke-opacity=".55"/>'
            f'<circle cx="16" cy="{h/2}" r="4.5" fill="{color}"/>'
            f'<text x="28" y="{h/2 + fs*0.35:.1f}" font-size="{fs}" class="t {fam}">{label}</text></g>'), w


def hero(t):
    w, h = 1280, 440
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="AgentStateStorage: the open session layer for AI agents">',
         style(t), defs(t, w, h)]
    # brand ring glow behind the tree
    p.append('<ellipse cx="990" cy="220" rx="300" ry="210" fill="url(#aura)"/>')
    # left: words
    p.append(f'<g transform="translate(64,0)">')
    p.append(f'<rect x="0" y="86" width="268" height="30" rx="15" fill="{t["kv"]}" fill-opacity=".1" stroke="{t["kv"]}" stroke-opacity=".5"/>')
    p.append(f'<text x="16" y="106" font-size="13" letter-spacing="2" class="m" fill="{t["kv"]}">OPEN SOURCE · APACHE-2.0</text>')
    p.append(f'<text x="0" y="178" font-size="58" font-weight="800" class="t" letter-spacing="-1.5">Agent<tspan fill="url(#brand)">State</tspan>Storage</text>')
    p.append(f'<text x="0" y="226" font-size="27" font-weight="600" class="t">The open session layer for AI agents</text>')
    p.append(f'<text x="0" y="266" font-size="18" class="s">Snapshot, fork, resume and erase an agent session —</text>')
    p.append(f'<text x="0" y="292" font-size="18" class="s">its KV, workspace and memory, together.</text>')
    p.append(f'<text x="0" y="318" font-size="18" class="s">On your infrastructure. Any engine. Any cloud.</text>')
    x = 0
    for label, c in (("snapshot", t["kv"]), ("fork", t["ws"]), ("resume", t["green"]), ("erase", t["red"])):
        g, cw = chip(t, x, 350, label, c, mono=True)
        p.append(g)
        x += cw + 12
    p.append("</g>")
    # right: a session tree that grows and forks
    nodes = {"a": (800, 220), "b": (910, 220), "c": (1030, 128), "d": (1030, 220),
             "e": (1030, 312), "f": (1150, 84), "g": (1150, 168), "h": (1150, 268), "i": (1150, 356)}
    edges = [("a", "b", t["kv"]), ("b", "c", t["ws"]), ("b", "d", t["ws"]), ("b", "e", t["ws"]),
             ("c", "f", t["mem"]), ("c", "g", t["mem"]), ("e", "h", t["green"]), ("e", "i", t["green"])]
    for a, b, c in edges:
        (x1, y1), (x2, y2) = nodes[a], nodes[b]
        p.append(edge(t, x1 + 36, y1, x2 - 36, y2, c))
    halos = {"b": t["ws"], "f": t["mem"], "h": t["green"]}
    for k, (x0, y0) in nodes.items():
        p.append(capsule(t, x0, y0, halos.get(k)))
    p.append(f'<text x="910" y="188" text-anchor="middle" font-size="12" class="d m">fork ×3</text>')
    p.append(f'<text x="800" y="262" text-anchor="middle" font-size="12" class="d m">s0</text>')
    # legend
    lx = 836
    for label, c in (("KV", t["kv"]), ("workspace", t["ws"]), ("memory", t["mem"])):
        p.append(f'<circle cx="{lx}" cy="410" r="5" fill="{c}"/>'
                 f'<text x="{lx + 11}" y="415" font-size="14" class="s">{label}</text>')
        lx += 22 + len(label) * 8.5 + 18
    p.append("</svg>")
    return "\n".join(p)


def session_tree(t):
    w, h = 1280, 620
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="Session tree: snapshot, fork, resume after a crash, and cascading erase">',
         style(t), defs(t, w, h)]
    p.append(f'<text x="48" y="58" font-size="24" font-weight="700" class="t">One session tree. Every kind of state.</text>')
    p.append(f'<text x="48" y="86" font-size="16" class="s">Each snapshot captures the KV prefix, the workspace and the memory version under one lineage id.</text>')
    y0 = 210
    main = [(140, "s0"), (300, "s1"), (460, "s2")]
    for (xa, _), (xb, _) in zip(main, main[1:]):
        p.append(edge(t, xa + 36, y0, xb - 36, y0, t["kv"]))
    # crash, then resume from s2
    p.append(edge(t, 496, y0, 584, y0, t["red"]))
    p.append(capsule(t, 620, y0, crashed=True))
    p.append(f'<text x="620" y="{y0 - 30}" text-anchor="middle" font-size="13" class="m" fill="{t["red"]}">crash</text>')
    p.append(f'<path d="M460 {y0 - 20}C500 {y0 - 110} 740 {y0 - 110} 780 {y0 - 20}" fill="none" stroke="{t["green"]}" stroke-width="2.2" class="flow"/>')
    p.append(f'<text x="620" y="{y0 - 98}" text-anchor="middle" font-size="13" class="m" fill="{t["green"]}">resume from s2 — no tool call replayed</text>')
    p.append(edge(t, 816, y0, 904, y0, t["kv"]))
    for x, lab in main + [(780, "s3′"), (940, "s4")]:
        p.append(capsule(t, x, y0, halo=t["green"] if lab == "s3′" else None))
        lx = x - 30 if lab == "s2" else x
        p.append(f'<text x="{lx}" y="{y0 + 38}" text-anchor="middle" font-size="12" class="d m">{lab}</text>')
    # fork from s2
    fx = 640
    for yf, lab in ((300, "f1"), (356, "f2"), (412, "f3")):
        d = f"M480 {y0 + 16}C500 {yf} {fx - 90} {yf} {fx - 36} {yf}"
        p.append(f'<path d="{d}" fill="none" stroke="{t["stroke"]}" stroke-width="2"/>')
        p.append(f'<path d="{d}" fill="none" stroke="{t["ws"]}" stroke-width="2" class="flow"/>')
        p.append(capsule(t, fx, yf, halo=t["ws"] if lab == "f2" else None))
        p.append(f'<text x="{fx + 48}" y="{yf + 4}" font-size="12" class="d m">{lab}</text>')
    p.append(f'<text x="730" y="352" font-size="14" class="s">fork in O(1), copy-on-write:</text>')
    p.append(f'<text x="730" y="374" font-size="14" class="s">sub-agents · tree search · what-if</text>')
    # compliance strip
    sy = 470
    p.append(f'<rect x="48" y="{sy}" width="1184" height="112" rx="16" fill="{t["panel2"]}" stroke="{t["stroke"]}"/>')
    p.append(f'<text x="72" y="{sy + 30}" font-size="13" letter-spacing="1.5" class="d m">COMPLIANCE, BUILT IN</text>')
    g, cw = chip(t, 72, sy + 52, "erase(subject=user-42)", t["red"], fs=14, mono=True)
    p.append(g)
    x = 72 + cw + 18
    for label in ("memory v7", "12 embeddings", "KV prefix 3f9a…", "refs in s1–s4"):
        p.append(f'<path d="M{x - 14} {sy + 69}h10" stroke="{t["red"]}" stroke-width="2" class="flow"/>')
        g, cw = chip(t, x, sy + 52, label, t["red"], fs=13, mono=True)
        p.append(g + f'<path d="M{x + 26} {sy + 69}h{cw - 40:.0f}" stroke="{t["red"]}" stroke-width="1.6" opacity=".8"/>')
        x += cw + 22
    g, cw = chip(t, x, sy + 52, "receipt ✓", t["green"], fs=13, mono=True)
    p.append(g)
    # legal hold note on the right
    p.append(f'<g transform="translate(1062,{sy + 22})"><rect x="0" y="6" width="14" height="11" rx="2" fill="{t["mem"]}"/>'
             f'<path d="M3 6V3a4 4 0 0 1 8 0v3" fill="none" stroke="{t["mem"]}" stroke-width="2"/>'
             f'<text x="22" y="16" font-size="13" class="s">legal hold keeps the audit log</text></g>')
    p.append("</svg>")
    return "\n".join(p)


def architecture(t):
    w, h = 1280, 640
    shipped, nxt, later = t["green"], t["mem"], t["gray"]
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="Architecture: agents, SDK, session layer, state planes, spine, engines and storage">',
         style(t), defs(t, w, h)]
    p.append(f'<text x="48" y="58" font-size="24" font-weight="700" class="t">Where it sits</text>')
    lx = 790
    for label, c in (("shipped", shipped), ("next (M1–M2)", nxt), ("later", later)):
        p.append(f'<circle cx="{lx}" cy="52" r="6" fill="{c}"/><text x="{lx + 13}" y="57" font-size="15" class="s">{label}</text>')
        lx += 40 + len(label) * 8.6
    layers = [
        ("Your agents", [("OpenAI Agents SDK", nxt), ("LangGraph", nxt), ("MCP clients", later), ("your own loop", later)]),
        ("SDK · API", [("Python SDK", nxt), ("gRPC API", nxt), ("TypeScript SDK", later)]),
        ("Session layer", [("session tree", nxt), ("erase · hold · audit", nxt), ("keep-or-recompute brain", shipped)]),
        ("State", [("KV prefix", shipped), ("durable ingest (1 node)", shipped), ("agent memory", nxt), ("execution state", later), ("embeddings · tool results", later)]),
        ("Spine", [("identity + lineage", shipped), ("DRAM → NVMe → object", shipped), ("hard multi-tenancy · mTLS", shipped), ("cross-node data path", later)]),
        ("Engines · storage", [("vLLM", shipped), ("SGLang", later), ("Dynamo", later), ("object storage (S3 API)", shipped)]),
    ]
    y = 92
    for i, (name, items) in enumerate(layers):
        hl = name == "Session layer"
        p.append(f'<rect x="48" y="{y}" width="1184" height="72" rx="14" fill="{t["panel2"] if not hl else t["panel"]}" '
                 f'stroke="{t["ws"] if hl else t["stroke"]}" stroke-width="{2 if hl else 1}"/>')
        if hl:
            p.append(f'<rect x="48" y="{y}" width="1184" height="72" rx="14" fill="none" stroke="{t["ws"]}" '
                     f'stroke-width="2" opacity=".6" class="pulse"/>')
        p.append(f'<text x="72" y="{y + 43}" font-size="17" font-weight="700" class="t">{name}</text>')
        x = 270
        for label, c in items:
            g, cw = chip(t, x, y + 19, label, c, fs=14)
            p.append(g)
            x += cw + 14
        if i < len(layers) - 1:
            p.append(f'<path d="M58 {y + 72}v12" stroke="{t["kv"]}" stroke-width="2" class="flow"/>')
        y += 84
    p.append("</svg>")
    return "\n".join(p)


def main():
    for name, fn in (("hero", hero), ("session-tree", session_tree), ("architecture", architecture)):
        for theme, t in THEMES.items():
            path = os.path.join(OUT, f"{name}-{theme}.svg")
            with open(path, "w") as f:
                f.write(fn(t) + "\n")
            print(path)


if __name__ == "__main__":
    main()
