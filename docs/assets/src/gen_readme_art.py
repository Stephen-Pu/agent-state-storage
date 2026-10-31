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
                 ok="#34D399", warm="#F59E0B", cool="#38BDF8", rec="#F472B6", bad="#F87171",
                 off="#475569"),
    "light": dict(bg="#FBFCFF", dot="#CDD6E8", glass="#FFFFFF", glass_op=".9", tile="#F5F7FC",
                  tile_op="1", sess="#7C3AED", kv="#0891B2", aur1="#C4B5FD", aur2="#A5F3FC",
                  aur_op=".35", frame="#A9B6CF", spark="#7C3AED", core_glow=".14",
                  shadow="#1E293B", shadow_op=".10", plan="#B45309", later="#64748B",
                  ok="#059669", warm="#D97706", cool="#0284C7", rec="#DB2777", bad="#DC2626",
                  off="#94A3B8"),
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


def arch_defs(t, a, w, h, auroras=((250, 420), (800, 460))):
    (ax1, ay1), (ax2, ay2) = auroras
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
<ellipse cx="{ax1}" cy="{ay1}" rx="420" ry="360" fill="url(#aur1)"/>
<ellipse cx="{ax2}" cy="{ay2}" rx="420" ry="360" fill="url(#aur2)"/>
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


# ---- hero and session tree: same visual language as the architecture -------
# Same honesty rule: a verb or a picture that is not built yet carries its
# milestone. The session tree is the M2 design; the "today" card says what
# M1 already does. A snapshot pod has four lanes, one per kind of state.

def lanes(a):
    return (("history", a["sess"]), ("memory", a["rec"]), ("workspace", a["warm"]), ("KV", a["kv"]))


def pod(t, a, x, y, halo=None, crashed=False):
    """One snapshot: a glass pill with a dot per lane."""
    out = []
    if halo:
        out.append(f'<ellipse cx="{x}" cy="{y}" rx="46" ry="26" fill="{halo}" opacity="{a["core_glow"]}" '
                   f'filter="url(#bloom)" class="breathe"/>')
    out.append(f'<rect x="{x - 34}" y="{y - 16}" width="68" height="32" rx="16" fill="{a["glass"]}" '
               f'stroke="{a["bad"] if crashed else (halo or a["frame"])}" stroke-width="1.5"/>')
    for dx, (_, c) in zip((-21, -7, 7, 21), lanes(a)):
        out.append(f'<circle cx="{x + dx}" cy="{y}" r="4.3" fill="{a["off"] if crashed else c}"/>')
    if crashed:
        out.append(f'<path d="M{x - 8} {y - 8}l16 16m0-16l-16 16" stroke="{a["bad"]}" stroke-width="3" '
                   f'stroke-linecap="round"/>')
    return "".join(out)


def link(x1, y1, x2, y2):
    mx = (x1 + x2) / 2
    return f"M{x1} {y1}C{mx} {y1} {mx} {y2} {x2} {y2}"


def legend(t, a, x, y):
    out = []
    for label, c in lanes(a):
        out.append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{c}"/>'
                   f'<text x="{x + 10}" y="{y + 5}" font-size="14" class="s">{label}</text>')
        x += 10 + len(label) * 7.6 + 22
    return "".join(out)


def pill(a, x, y, label, color, status=None, fs=15, h=36, strike=False):
    """A tinted capsule with a dot and a mono label; optional status mark at the end."""
    tw = len(label) * fs * 0.6
    end = x + 28 + tw
    if status in ("shipped", "partial"):
        mx, w = end + 19, tw + 28 + 19 + 9 + 12
    elif status:
        mx, w = end + 21, tw + 28 + 12 + len(status) * 8.4 + 14 + 10
    else:
        mx, w = None, tw + 28 + 16
    out = (f'<rect x="{x}" y="{y}" width="{w:.0f}" height="{h}" rx="{h / 2}" fill="{color}" fill-opacity=".12" '
           f'stroke="{color}" stroke-opacity=".6"/>'
           f'<circle cx="{x + 16}" cy="{y + h / 2}" r="4.5" fill="{color}"/>'
           f'<text x="{x + 28}" y="{y + h / 2 + fs * 0.35:.1f}" font-size="{fs}" class="t m">{label}</text>')
    if strike:
        out += f'<path d="M{x + 26} {y + h / 2}h{tw + 4:.0f}" stroke="{color}" stroke-width="1.6" opacity=".85"/>'
    if mx:
        out += mark(a, mx, y + h / 2, status)
    return out, w


def status_key(t, a, x, y, items):
    out = []
    for st, label in items:
        out.append(mark(a, x, y, st))
        off = 16 if st in ("shipped", "partial") else len(st) * 8.4 + 12
        out.append(f'<text x="{x + off}" y="{y + 5}" font-size="14" class="s">{label}</text>')
        x += off + len(label) * 7.6 + 28
    return "".join(out)


def hero(t):
    a = ARCH["dark" if t is THEMES["dark"] else "light"]
    w, h = 1040, 430
    L = 40
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="AgentStateStorage: the open session layer for AI agents">',
         arch_style(t, a), arch_defs(t, a, w, h, auroras=((230, 250), (840, 230)))]

    # ---- words ------------------------------------------------------------------
    p.append(f'<rect x="{L}" y="52" width="248" height="28" rx="14" fill="{a["kv"]}" fill-opacity=".1" '
             f'stroke="{a["kv"]}" stroke-opacity=".55"/>'
             f'<text x="{L + 14}" y="71" font-size="12.5" letter-spacing="2" class="m" fill="{a["kv"]}">'
             f'OPEN SOURCE · APACHE-2.0</text>')
    p.append(f'<text x="{L - 2}" y="142" font-size="46" font-weight="800" letter-spacing="-1.4" class="t">'
             f'Agent<tspan fill="url(#word)">State</tspan>Storage</text>')
    p.append(f'<text x="{L}" y="184" font-size="23" font-weight="650" class="t">The open session layer for AI agents</text>')
    for i, line in enumerate(("Snapshot, fork, resume and erase an agent session —",
                              "its history, memory, workspace and KV cache, together.",
                              "On your infrastructure. Any engine. Any cloud.")):
        p.append(f'<text x="{L}" y="{222 + i * 25}" font-size="16.5" class="s">{line}</text>')
    x = L
    for label, c, st in (("snapshot", a["kv"], "partial"), ("fork", a["sess"], "shipped"),
                         ("resume", a["ok"], "shipped"), ("erase", a["bad"], "M2")):
        g, cw = pill(a, x, 306, label, c, st)
        p.append(g)
        x += cw + 10
    p.append(status_key(t, a, L + 9, 376, (("shipped", "works today"), ("partial", "per framework"), ("M2", "planned"))))

    # ---- one session, forked ----------------------------------------------------
    gx0, gy0, gx1, gy1 = 560, 36, w - 40, 394
    p.append(glass(a, gx0, gy0, gx1 - gx0, gy1 - gy0, rx=22))
    p.append(f'<text x="{gx0 + 22}" y="{gy0 + 32}" class="eb">ONE SESSION, FORKED</text>')
    n = {"a": (618, 200), "b": (712, 200), "c": (822, 124), "d": (822, 200), "e": (822, 276),
         "f": (934, 92), "g": (934, 156), "h": (934, 244), "i": (934, 308)}
    edges = (("a", "b", a["kv"]), ("b", "c", a["sess"]), ("b", "d", a["sess"]), ("b", "e", a["sess"]),
             ("c", "f", a["rec"]), ("c", "g", a["rec"]), ("e", "h", a["ok"]), ("e", "i", a["ok"]))
    for k1, k2, c in edges:
        (x1, y1), (x2, y2) = n[k1], n[k2]
        p.append(beam(t, link(x1 + 34, y1, x2 - 34, y2), c, slow=k1 != "a"))
    halos = {"b": a["sess"], "f": a["rec"], "h": a["ok"]}
    for k, (x0, y0) in n.items():
        p.append(pod(t, a, x0, y0, halos.get(k)))
    p.append(f'<text x="712" y="170" text-anchor="middle" font-size="13" class="d m">fork ×3</text>')
    p.append(f'<text x="618" y="236" text-anchor="middle" font-size="13" class="d m">s0</text>')
    p.append(legend(t, a, gx0 + 26, gy1 - 28))
    p.append("</svg>")
    return "\n".join(p)


def session_tree(t):
    a = ARCH["dark" if t is THEMES["dark"] else "light"]
    w, h = 1040, 640
    L, R = 40, w - 40
    p = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
         f'role="img" aria-label="Session tree: snapshot, fork, resume after a crash, and cascading erase">',
         arch_style(t, a), arch_defs(t, a, w, h, auroras=((300, 300), (760, 420)))]

    # ---- header -------------------------------------------------------------------
    p.append(f'<text x="{L}" y="54" font-size="23" font-weight="800" letter-spacing="-.4" class="t">'
             f'One session tree. Every kind of state.</text>')
    p.append(f'<text x="{L}" y="80" class="eb">SESSION TREE · THE M2 DESIGN</text>')
    p.append(status_key(t, a, 700, 50, (("shipped", "works today"), ("M2", "planned"))))
    p.append(f'<text x="{L}" y="112" font-size="15" class="s">A snapshot captures history (items), memory (records), '
             f'workspace (blobs) and the KV prefix under one lineage id.</text>')

    # ---- the tree -------------------------------------------------------------------
    ty0, ty1 = 134, 474
    p.append(glass(a, L, ty0, R - L, ty1 - ty0, rx=22))
    p.append(mark(a, L + 24, ty0 + 26, "M2"))
    p.append(legend(t, a, L + 26, ty1 - 26))
    y0 = 256
    xs = {"s0": 110, "s1": 228, "s2": 346, "x": 466, "s3′": 586, "s4": 704}
    for k1, k2 in (("s0", "s1"), ("s1", "s2"), ("s3′", "s4")):
        p.append(beam(t, f"M{xs[k1] + 34} {y0}H{xs[k2] - 34}", a["kv"]))
    p.append(beam(t, f"M{xs['s2'] + 34} {y0}H{xs['x'] - 34}", a["bad"]))
    p.append(f'<text x="{xs["x"]}" y="{y0 + 40}" text-anchor="middle" font-size="13" class="m" fill="{a["bad"]}">crash</text>')
    # resume: an arc from s2 over the crash to s3′
    arc = f"M{xs['s2']} {y0 - 18}C{xs['s2'] + 40} {y0 - 112} {xs['s3′'] - 40} {y0 - 112} {xs['s3′']} {y0 - 18}"
    p.append(beam(t, arc, a["ok"]))
    p.append(f'<text x="{xs["x"]}" y="{y0 - 104}" text-anchor="middle" font-size="13.5" class="m" fill="{a["ok"]}">'
             f'resume from s2 · no tool call replayed</text>')
    # forks from s2
    fx = 540
    for yf, lab in ((336, "f1"), (384, "f2"), (432, "f3")):
        p.append(beam(t, link(xs["s2"] + 20, y0 + 16, fx - 34, yf), a["sess"], slow=True))
        p.append(pod(t, a, fx, yf, a["sess"] if lab == "f2" else None))
        p.append(f'<text x="{fx + 44}" y="{yf + 5}" font-size="13" class="d m">{lab}</text>')
    p.append(f'<text x="{fx + 82}" y="372" font-size="15" font-weight="650" class="t">fork · copy-on-write, O(1)</text>')
    p.append(f'<text x="{fx + 82}" y="396" font-size="13" class="d m">sub-agents · tree search · what-if</text>')
    for k, x in xs.items():
        if k == "x":
            p.append(pod(t, a, x, y0, crashed=True))
            continue
        p.append(pod(t, a, x, y0, a["ok"] if k == "s3′" else None))
        lx = x - 26 if k == "s2" else x
        p.append(f'<text x="{lx}" y="{y0 + 40}" text-anchor="middle" font-size="13" class="d m">{k}</text>')

    # today, in M1: what already works without the tree
    cx0, cy0 = 784, 168
    p.append(f'<rect x="{cx0}" y="{cy0}" width="{R - 20 - cx0}" height="150" rx="14" fill="{a["tile"]}" '
             f'fill-opacity="{a["tile_op"]}" stroke="{a["ok"]}" stroke-opacity=".55"/>')
    p.append(f'<text x="{cx0 + 18}" y="{cy0 + 28}" class="eb" style="fill:{a["ok"]}">TODAY, IN M1</text>')
    for i, line in enumerate(("resume after a restart", "fork by copy", "framework snapshots")):
        y = cy0 + 58 + i * 30
        p.append(mark(a, cx0 + 27, y, "shipped") + f'<text x="{cx0 + 44}" y="{y + 5}" font-size="14.5" class="s">{line}</text>')

    # ---- erase · hold · audit ---------------------------------------------------------
    ey0 = ty1 + 22
    p.append(glass(a, L, ey0, R - L, 114, rx=20, edge=a["bad"]))
    p.append(f'<text x="{L + 22}" y="{ey0 + 30}" class="eb" style="fill:{a["bad"]}">ERASE · HOLD · AUDIT</text>')
    p.append(mark(a, L + 254, ey0 + 25, "M2"))
    p.append(f'<g transform="translate({R - 262},{ey0 + 14})"><rect x="0" y="6" width="14" height="11" rx="2" fill="{a["warm"]}"/>'
             f'<path d="M3 6V3a4 4 0 0 1 8 0v3" fill="none" stroke="{a["warm"]}" stroke-width="2"/>'
             f'<text x="22" y="16" font-size="14" class="s">legal hold keeps the audit log</text></g>')
    x, y = L + 22, ey0 + 54
    g, cw = pill(a, x, y, "erase(user-42)", a["bad"], fs=13, h=34)
    p.append(g)
    x += cw
    for label in ("memory v7", "12 embeddings", "KV prefix 3f9a…", "refs in s1–s4"):
        p.append(beam(t, f"M{x + 2} {y + 17}H{x + 14}", a["bad"]))
        x += 16
        g, cw = pill(a, x, y, label, a["bad"], fs=13, h=34, strike=True)
        p.append(g)
        x += cw
    p.append(beam(t, f"M{x + 2} {y + 17}H{x + 14}", a["ok"]))
    g, cw = pill(a, x + 16, y, "receipt ✓", a["ok"], fs=13, h=34)
    assert x + 16 + cw <= R - 16, "erase chain overflows the card"
    p.append(g)
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
