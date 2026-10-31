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


# ---- architecture: the system picture ---------------------------------------
# Drawn from the code, not the roadmap: solid = shipped, dashed + milestone tag
# = planned. Two planes that share one node but not one store: session state
# (irreplaceable, agentstate.v1's own log) and the KV cache (recomputable,
# tiered). The session tree (M2) is what will span both. Keep the statuses in
# sync with the README capability matrix.
#
# Sized for GitHub's README column (~880 px): the 1040 px canvas scales by
# about 0.85, so no text is set below 13 px.

ARCH = {
    "dark": dict(bg="#05060B", dot="#232C44", glass="#0B1121", glass_op=".78", tile="#111A2F",
                 tile_op=".92", sess="#A78BFA", kv="#22D3EE", aur1="#7C3AED", aur2="#0891B2",
                 aur_op=".28", frame="#3A4767", spark="#E0E7FF", core_glow=".30",
                 shadow="#000000", shadow_op=".55", plan="#FBBF24", later="#7C8AA5",
                 ok="#34D399", warm="#F59E0B", cool="#38BDF8"),
    "light": dict(bg="#FBFCFF", dot="#CDD6E8", glass="#FFFFFF", glass_op=".9", tile="#F5F7FC",
                  tile_op="1", sess="#7C3AED", kv="#0891B2", aur1="#C4B5FD", aur2="#A5F3FC",
                  aur_op=".35", frame="#A9B6CF", spark="#7C3AED", core_glow=".14",
                  shadow="#1E293B", shadow_op=".10", plan="#B45309", later="#64748B",
                  ok="#059669", warm="#D97706", cool="#0284C7"),
}


def arch_style(t, a):
    return f"""<style>
  .t  {{ font-family: {FONT}; fill: {t['text']}; }}
  .s  {{ font-family: {FONT}; fill: {t['sub']}; }}
  .d  {{ font-family: {FONT}; fill: {t['dim']}; }}
  .m  {{ font-family: {MONO}; }}
  .eb {{ font-family: {MONO}; font-size: 12.5px; letter-spacing: 2px; fill: {t['dim']}; }}
  .spark {{ stroke-dasharray: 2 40; stroke-linecap: round; animation: spark 2.2s linear infinite; }}
  .spark.slow {{ animation-duration: 3.4s; }}
  .march {{ stroke-dasharray: 7 7; animation: march 1.8s linear infinite; }}
  .breathe {{ animation: breathe 4s ease-in-out infinite; }}
  @keyframes spark {{ to {{ stroke-dashoffset: -84; }} }}
  @keyframes march {{ to {{ stroke-dashoffset: -28; }} }}
  @keyframes breathe {{ 0%,100% {{ opacity: .5; }} 50% {{ opacity: 1; }} }}
  @media (prefers-reduced-motion: reduce) {{
    .spark, .march, .breathe {{ animation: none; }}
  }}
</style>"""


def arch_defs(t, a, w, h):
    return f"""<defs>
  <pattern id="dots" width="22" height="22" patternUnits="userSpaceOnUse">
    <circle cx="1.5" cy="1.5" r="1.1" fill="{a['dot']}"/>
  </pattern>
  <radialGradient id="aur1" cx=".5" cy=".5" r=".5">
    <stop offset="0" stop-color="{a['aur1']}" stop-opacity="{a['aur_op']}"/>
    <stop offset="1" stop-color="{a['aur1']}" stop-opacity="0"/>
  </radialGradient>
  <radialGradient id="aur2" cx=".5" cy=".5" r=".5">
    <stop offset="0" stop-color="{a['aur2']}" stop-opacity="{a['aur_op']}"/>
    <stop offset="1" stop-color="{a['aur2']}" stop-opacity="0"/>
  </radialGradient>
  <linearGradient id="word" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{a['sess']}"/><stop offset="1" stop-color="{a['kv']}"/>
  </linearGradient>
  <!-- canvas units: a straight line has a zero-width bounding box, which
       would leave a bounding-box gradient unpainted -->
  <linearGradient id="span" gradientUnits="userSpaceOnUse" x1="40" y1="0" x2="{w - 40}" y2="0">
    <stop offset="0" stop-color="{a['sess']}"/><stop offset="1" stop-color="{a['kv']}"/>
  </linearGradient>
  <linearGradient id="heat" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{a['warm']}"/><stop offset="1" stop-color="{a['cool']}"/>
  </linearGradient>
  <filter id="soft" x="-20%" y="-20%" width="140%" height="160%">
    <feDropShadow dx="0" dy="10" stdDeviation="14" flood-color="{a['shadow']}" flood-opacity="{a['shadow_op']}"/>
  </filter>
  <filter id="bloom" x="-30%" y="-80%" width="160%" height="260%">
    <feGaussianBlur stdDeviation="16"/>
  </filter>
</defs>
<rect width="{w}" height="{h}" rx="20" fill="{a['bg']}"/>
<ellipse cx="250" cy="420" rx="420" ry="360" fill="url(#aur1)"/>
<ellipse cx="800" cy="460" rx="420" ry="360" fill="url(#aur2)"/>
<rect width="{w}" height="{h}" rx="20" fill="url(#dots)" opacity=".7"/>"""


def glass(a, x, y, w, h, rx=18, edge=None, sw=1.3, dashed=False, shadow=True):
    f = ' filter="url(#soft)"' if shadow else ""
    dash = ' stroke-dasharray="6 6"' if dashed else ""
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{a["glass"]}" '
            f'fill-opacity="{a["glass_op"]}" stroke="{edge or a["frame"]}" stroke-width="{sw}"{dash}{f}/>')


def mark(a, x, y, status):
    """Status glyph, readable without colour: check, half disc, or a milestone tag."""
    if status == "shipped":
        return (f'<circle cx="{x}" cy="{y}" r="9" fill="{a["ok"]}" fill-opacity=".18" stroke="{a["ok"]}" stroke-width="1.4"/>'
                f'<path d="M{x - 4} {y}l2.8 2.9 5.2-5.6" fill="none" stroke="{a["ok"]}" stroke-width="2" '
                f'stroke-linecap="round" stroke-linejoin="round"/>')
    if status == "partial":
        return (f'<circle cx="{x}" cy="{y}" r="9" fill="none" stroke="{a["plan"]}" stroke-width="1.4"/>'
                f'<path d="M{x} {y - 9}a9 9 0 0 1 0 18z" fill="{a["plan"]}"/>')
    color = a["later"] if status == "later" else a["plan"]
    w = len(status) * 8.4 + 14
    return (f'<rect x="{x - 9}" y="{y - 10}" width="{w:.0f}" height="20" rx="10" fill="{color}" '
            f'fill-opacity=".14" stroke="{color}" stroke-width="1.2"/>'
            f'<text x="{x - 2}" y="{y + 4.5}" font-size="12.5" font-weight="700" class="m" fill="{color}">{status}</text>')


def tile(t, a, x, y, w, h, title, sub, status, title_fs=16):
    planned = status not in ("shipped", "partial")
    dash = ' stroke-dasharray="5 5"' if planned else ""
    tx = x + 16 if planned else x + 38
    out = (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{a["tile"]}" '
           f'fill-opacity="{a["tile_op"]}" stroke="{t["stroke"]}" stroke-opacity=".8"{dash}/>')
    if planned:
        out += mark(a, x + w - len(status) * 8.4 - 16, y + 20, status)
    else:
        out += mark(a, x + 20, y + 20, status)
    out += (f'<text x="{tx}" y="{y + 25}" font-size="{title_fs}" font-weight="650" class="t">{title}</text>'
            f'<text x="{tx}" y="{y + h - 13}" font-size="13" class="d m">{sub}</text>')
    return out


def beam(t, d, color, slow=False):
    cls = "spark slow" if slow else "spark"
    return (f'<path d="{d}" fill="none" stroke="{color}" stroke-width="1.6" opacity=".4"/>'
            f'<path d="{d}" fill="none" stroke="{color}" stroke-width="3.2" class="{cls}"/>')


def joint(a, x, y, color):
    return (f'<circle cx="{x}" cy="{y}" r="7" fill="{color}" opacity=".2"/>'
            f'<circle cx="{x}" cy="{y}" r="3.4" fill="{a["glass"]}" stroke="{color}" stroke-width="1.6"/>')


def plane_tag(a, x, y, label, color):
    w = len(label) * 7.9 + 20
    return (f'<rect x="{x}" y="{y - 14}" width="{w:.0f}" height="22" rx="11" fill="{color}" fill-opacity=".12" '
            f'stroke="{color}" stroke-opacity=".6"/>'
            f'<text x="{x + 10}" y="{y + 1.5}" font-size="12.5" class="m" fill="{color}">{label}</text>')


def architecture(t):
    a = ARCH["dark" if t is THEMES["dark"] else "light"]
    # vertical layout: clients, API edge, node (two planes + session tree), control plane
    ny0 = 392
    py0, py1 = ny0 + 44, ny0 + 44 + 330
    ny1 = py1 + 108
    w, h = 1040, ny1 + 28 + 108 + 36
    L, R, MID = 40, w - 40, w // 2           # outer margins; the two planes split at MID
    lw = MID - 20 - L                         # width of each half column (440)
    rx0 = MID + 20
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="AgentStateStorage system architecture">',
         arch_style(t, a), arch_defs(t, a, w, h)]
    joints = []

    # ---- header + legend ------------------------------------------------------
    p.append(f'<text x="{L}" y="54" font-size="23" font-weight="800" letter-spacing="-.4" class="t">'
             f'Agent<tspan fill="url(#word)">State</tspan>Storage</text>')
    p.append(f'<text x="{L}" y="80" class="eb">SYSTEM ARCHITECTURE · AS OF M1</text>')
    lx = 612
    for st, label in (("shipped", "shipped"), ("partial", "partial"), ("M2", "planned")):
        p.append(mark(a, lx, 50, st))
        off = 16 if st in ("shipped", "partial") else len(st) * 8.4 + 12
        p.append(f'<text x="{lx + off}" y="55" font-size="14" class="s">{label}</text>')
        lx += off + len(label) * 7.6 + 30

    # ---- clients: agents over the session plane, engines over the KV plane -----
    cy = 128
    p.append(f'<text x="{L}" y="{cy - 14}" class="eb">YOUR AGENTS</text>')
    p.append(f'<text x="{rx0}" y="{cy - 14}" class="eb">INFERENCE ENGINES</text>')
    tw = (lw - 16) // 2
    agents = [("OpenAI Agents SDK", "session · snapshots", "shipped"), ("LangGraph", "checkpointer · store", "shipped"),
              ("Your own loop", "Python SDK · gRPC", "shipped"), ("MCP clients", "session handles", "M3")]
    engines = [("vLLM", "KV connector", "shipped"), ("SGLang", "HiCache backend", "M4"),
               ("NVIDIA Dynamo", "native offload", "M4"), ("TensorRT-LLM", "via LMCache", "later")]
    for group, x0 in ((agents, L), (engines, rx0)):
        for i, (name, sub, st) in enumerate(group):
            x = x0 + (i % 2) * (tw + 16)
            y = cy + (i // 2) * 72
            p.append(tile(t, a, x, y, tw, 60, name, sub, st))

    # ---- API edge ---------------------------------------------------------------
    ey = 300
    sx, kx = L + lw / 2, rx0 + lw / 2
    for x, c in ((sx, a["sess"]), (kx, a["kv"])):
        p.append(beam(t, f"M{x} {cy + 132}V{ey}", c))
        joints += [(x, cy + 132, c), (x, ey, c)]
    p.append(glass(a, L, ey, lw, 56, rx=28, edge=a["sess"]))
    p.append(glass(a, rx0, ey, lw, 56, rx=28, edge=a["kv"]))
    p.append(f'<text x="{L + 24}" y="{ey + 34}" font-size="15" font-weight="650" class="t">Python SDK · gRPC</text>')
    p.append(plane_tag(a, L + 196, ey + 29, "mTLS: tenant from cert", a["sess"]))
    p.append(mark(a, L + lw - 24, ey + 28, "shipped"))
    p.append(f'<text x="{rx0 + 24}" y="{ey + 34}" font-size="15" font-weight="650" class="t">C ABI · gRPC</text>')
    p.append(plane_tag(a, rx0 + 148, ey + 29, "mTLS: tenant binding off", a["kv"]))
    p.append(mark(a, rx0 + lw - 24, ey + 28, "partial"))

    # ---- the node: two planes ---------------------------------------------------
    p.append(f'<rect x="{L - 16}" y="{ny0}" width="{R - L + 32}" height="{ny1 - ny0}" rx="26" fill="none" '
             f'stroke="{a["frame"]}" stroke-width="1.4" stroke-dasharray="3 6"/>')
    p.append(f'<text x="{L + 4}" y="{ny0 + 26}" class="eb">KVSTORE-NODE · ONE PROCESS</text>')
    for x, c in ((sx, a["sess"]), (kx, a["kv"])):
        p.append(beam(t, f"M{x} {ey + 56}V{py0}", c, slow=True))
        joints += [(x, ey + 56, c), (x, py0, c)]

    # session state plane
    p.append(f'<rect x="{L + 30}" y="{py0 + 20}" width="{lw - 60}" height="{py1 - py0 - 40}" rx="30" '
             f'fill="{a["sess"]}" opacity="{a["core_glow"]}" filter="url(#bloom)" class="breathe"/>')
    p.append(glass(a, L, py0, lw, py1 - py0, rx=20, edge=a["sess"], sw=1.6))
    p.append(f'<text x="{L + 22}" y="{py0 + 32}" class="eb" style="fill:{a["sess"]}">SESSION STATE</text>')
    p.append(plane_tag(a, L + lw - 142, py0 + 28, "irreplaceable", a["sess"]))
    p.append(f'<text x="{L + 22}" y="{py0 + 62}" font-size="20" font-weight="750" class="t">agentstate.v1</text>')
    for i, (name, sub) in enumerate((("Items", "per-session log · fork"), ("Records", "checkpoints · store"),
                                     ("Blobs", "workspace snapshots"))):
        p.append(tile(t, a, L + 20, py0 + 80 + i * 64, lw - 40, 54, name, sub, "shipped"))
    p.append(f'<text x="{L + 22}" y="{py1 - 22}" font-size="13" class="s m">fsync before ack · 1 replica · local disk</text>')

    # KV cache plane
    p.append(f'<rect x="{rx0 + 30}" y="{py0 + 20}" width="{lw - 60}" height="{py1 - py0 - 40}" rx="30" '
             f'fill="{a["kv"]}" opacity="{a["core_glow"]}" filter="url(#bloom)" class="breathe"/>')
    p.append(glass(a, rx0, py0, lw, py1 - py0, rx=20, edge=a["kv"], sw=1.6))
    p.append(f'<text x="{rx0 + 22}" y="{py0 + 32}" class="eb" style="fill:{a["kv"]}">KV CACHE</text>')
    p.append(plane_tag(a, rx0 + lw - 140, py0 + 28, "recomputable", a["kv"]))
    p.append(f'<text x="{rx0 + 22}" y="{py0 + 62}" font-size="20" font-weight="750" class="t">NodeData</text>')
    p.append(tile(t, a, rx0 + 20, py0 + 80, lw - 40, 54, "Prefix index", "content identity · lookup · fetch", "shipped"))
    p.append(tile(t, a, rx0 + 20, py0 + 144, lw - 40, 54, "Keep-or-recompute", "cost-based tiering · priorities", "shipped"))
    # tiers
    ty = py0 + 210
    tiers = (("DRAM", 104), ("NVMe", 104), ("S3 · Alluxio", 152))
    x = rx0 + 20
    for i, (name, tw_) in enumerate(tiers):
        p.append(f'<rect x="{x}" y="{ty}" width="{tw_}" height="40" rx="10" fill="{a["tile"]}" '
                 f'fill-opacity="{a["tile_op"]}" stroke="{t["stroke"]}" stroke-opacity=".8"/>'
                 f'<text x="{x + tw_ / 2}" y="{ty + 25}" text-anchor="middle" font-size="14.5" font-weight="650" class="t">{name}</text>')
        if i < len(tiers) - 1:
            ax = x + tw_ + 3
            p.append(f'<path d="M{ax} {ty + 20}h14M{ax + 9} {ty + 15}l5 5-5 5" fill="none" stroke="{t["dim"]}" stroke-width="1.6"/>')
        x += tw_ + 20
    p.append(f'<rect x="{rx0 + 20}" y="{ty + 50}" width="{x - rx0 - 40}" height="4" rx="2" fill="url(#heat)"/>')
    p.append(f'<text x="{rx0 + 22}" y="{py1 - 22}" font-size="13" class="s m">cache · a lost entry is recomputed</text>')

    # ---- the session tree (M2) spans both planes ---------------------------------
    by0 = py1 + 26
    for x, c in ((sx, a["sess"]), (kx, a["kv"])):
        p.append(f'<path d="M{x} {by0}V{py1}" stroke="{c}" stroke-width="2" class="march"/>')
        joints += [(x, py1, c)]
    p.append(f'<rect x="{L + 60}" y="{by0 + 8}" width="{R - L - 120}" height="40" rx="20" fill="url(#span)" '
             f'opacity="{a["core_glow"]}" filter="url(#bloom)" class="breathe"/>')
    p.append(f'<rect x="{L}" y="{by0}" width="{R - L}" height="62" rx="31" fill="{a["glass"]}" fill-opacity="{a["glass_op"]}" '
             f'stroke="url(#span)" stroke-width="1.8" stroke-dasharray="7 6"/>')
    gx, gy = L + 30, by0 + 31
    for (x1, y1, x2, y2) in ((0, 0, 22, 0), (22, 0, 44, -10), (22, 0, 44, 10)):
        p.append(f'<path d="M{gx + x1} {gy + y1}L{gx + x2} {gy + y2}" stroke="{a["sess"]}" stroke-width="1.6"/>')
    for (nx, ny) in ((0, 0), (22, 0), (44, -10), (44, 10)):
        p.append(f'<circle cx="{gx + nx}" cy="{gy + ny}" r="4" fill="{a["glass"]}" stroke="{a["sess"]}" stroke-width="1.6"/>')
    p.append(f'<text x="{L + 92}" y="{by0 + 27}" font-size="16.5" font-weight="700" class="t">Session tree · erase · hold · audit</text>')
    p.append(f'<text x="{L + 92}" y="{by0 + 48}" font-size="13" class="d m">one snapshot = items + records + blobs + KV prefix · erase cascades along lineage</text>')
    p.append(mark(a, R - 54, by0 + 31, "M2"))

    # ---- control plane + peers -----------------------------------------------------
    cy0 = ny1 + 28
    p.append(glass(a, L, cy0, 560, 108, rx=18))
    p.append(f'<text x="{L + 22}" y="{cy0 + 30}" class="eb">CONTROL PLANE</text>')
    for i, label in enumerate(("etcd membership", "HRW routing", "operator · CRD", "quotas")):
        x = L + 22 + (i % 2) * 268
        y = cy0 + 50 + (i // 2) * 32
        p.append(mark(a, x + 8, y + 8, "shipped") + f'<text x="{x + 26}" y="{y + 13}" font-size="14.5" class="s">{label}</text>')
    px0 = L + 580
    p.append(glass(a, px0, cy0, R - px0, 108, rx=18, dashed=True))
    p.append(f'<text x="{px0 + 22}" y="{cy0 + 30}" class="eb">PEER NODES</text>')
    p.append(mark(a, px0 + 30, cy0 + 58, "shipped") + f'<text x="{px0 + 48}" y="{cy0 + 63}" font-size="14.5" class="s">lookup forwarding</text>')
    p.append(mark(a, px0 + 30, cy0 + 90, "M4") + f'<text x="{px0 + 66}" y="{cy0 + 95}" font-size="14.5" class="s">cross-node data path</text>')

    p.extend(joint(a, x, y, c) for x, y, c in joints)
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
