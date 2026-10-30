# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds landing/landing.html: the custom Gumroad product page for EXRay.
# One self-contained file: screenshots and icon embedded as data: URIs (the
# Gumroad sandbox blocks external images). Run: python3 tools/make_landing.py
import base64, io, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..")
P = os.path.join(ROOT, "promo")
OUT = os.path.join(ROOT, "landing", "landing.html")

LIMIT = 480_000   # Gumroad allows 500,000 characters of custom HTML; keep a margin
WIDTH, QUALITY = 1280, 80

def webp(name, q=None, width=None):
    q = QUALITY if q is None else q
    width = WIDTH if width is None else width
    im = Image.open(os.path.join(P, name)).convert("RGB")
    if im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, "WEBP", quality=q, method=6)
    return "data:image/webp;base64," + base64.b64encode(b.getvalue()).decode()

import make_promo_icon as mi   # small helper module written below
ic = mi.icon(192)
b = io.BytesIO(); ic.save(b, "PNG", optimize=True)
ICON = "data:image/png;base64," + base64.b64encode(b.getvalue()).decode()

def build_shots():
    return {k: webp(f"shot_{k}.png") for k in ["image", "grid", "360", "fix", "passes", "crypto", "mask"]}
import re, glob
# Version from main_win.cpp, checksum of the packed .exe (run tools/package.sh first).
VERSION = re.search(r'kVersion\[\] *= *L"([^"]*)"', open(os.path.join(ROOT, "src", "main_win.cpp"), encoding="utf-8").read()).group(1)
_sha = os.path.join(ROOT, "release", "dist", "EXRay_v" + VERSION.replace(" ", "_") + ".sha256.txt")
SHA = next(l.split()[0] for l in open(_sha) if l.strip().endswith(".exe"))

html = r"""<!doctype html>
<html lang="en">
<head>
<style>
:root{
  --bg:#0f1013; --bg2:#15171c; --card:rgba(255,255,255,.045); --line:rgba(255,255,255,.10);
  --text:#ececee; --dim:#a3a7b0; --faint:#6f7480; --accent:#4a8bd8; --accent2:#3a7bc8; --warm:#f6a65a;
  --glowA:rgba(40,70,140,.55); --glowB:rgba(150,80,25,.38); --shadow:0 30px 80px rgba(0,0,0,.55);
  --btnText:#fff; --chip:rgba(255,255,255,.06);
}
@media (prefers-color-scheme: light){
  :root{
    --bg:#f6f5f2; --bg2:#ffffff; --card:#ffffff; --line:rgba(20,24,35,.10);
    --text:#15171c; --dim:#4f5562; --faint:#7d8390; --accent:#2f6fbf; --accent2:#2a64ad; --warm:#c46a14;
    --glowA:rgba(74,139,216,.22); --glowB:rgba(246,166,90,.25); --shadow:0 24px 60px rgba(20,24,35,.18);
    --chip:rgba(20,24,35,.05);
  }
}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
@media (prefers-reduced-motion: reduce){html{scroll-behavior:auto}*{animation:none!important;transition:none!important}}
body{margin:0;background:var(--bg);color:var(--text);
  font:17px/1.6 "Segoe UI Variable Text","Segoe UI",Inter,system-ui,-apple-system,"Helvetica Neue",Arial,sans-serif;
  -webkit-font-smoothing:antialiased}
a{color:var(--accent)}
img{max-width:100%;height:auto;display:block}
.wrap{max-width:1140px;margin:0 auto;padding:0 24px}
:focus-visible{outline:3px solid var(--warm);outline-offset:3px;border-radius:6px}
.skip{position:absolute;left:-999px}.skip:focus{left:16px;top:16px;background:var(--bg2);padding:8px 12px;z-index:9}

/* hero */
.hero{position:relative;overflow:hidden;padding:72px 0 40px;text-align:center}
.hero::before{content:"";position:absolute;inset:-20% -10% auto -10%;height:900px;
  background:radial-gradient(ellipse 50% 45% at 22% 18%,var(--glowA),transparent 70%),
             radial-gradient(ellipse 45% 40% at 78% 10%,var(--glowB),transparent 70%);pointer-events:none}
.hero>*{position:relative}
.brand{display:inline-flex;align-items:center;gap:18px;margin:0}
.brand img{width:76px;height:76px;filter:drop-shadow(0 10px 25px rgba(246,166,90,.35))}
h1{font-size:clamp(52px,9vw,92px);line-height:1;margin:0;letter-spacing:-.03em;font-weight:800}
.tag{font-size:clamp(20px,2.6vw,28px);font-weight:650;margin:22px 0 8px;letter-spacing:-.01em}
.sub{color:var(--dim);max-width:640px;margin:0 auto}
.cta{display:flex;flex-wrap:wrap;gap:14px;justify-content:center;margin:30px 0 14px}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:10px;min-height:52px;padding:0 26px;border-radius:12px;
  font:inherit;font-weight:650;font-size:17px;text-decoration:none;cursor:pointer;border:1px solid transparent;transition:transform .15s,background .15s,box-shadow .15s}
.btn:hover{transform:translateY(-1px)}
.btn-primary{background:linear-gradient(180deg,var(--accent),var(--accent2));color:var(--btnText);box-shadow:0 10px 30px rgba(58,123,200,.35)}
.btn-ghost{background:var(--chip);color:var(--text);border-color:var(--line)}
.meta{color:var(--faint);font-size:14.5px;display:flex;flex-wrap:wrap;gap:6px 16px;justify-content:center}
.meta span::before{content:"•";margin-right:16px;color:var(--warm)}
.meta span:first-child::before{content:none}
.shot{border-radius:14px;overflow:hidden;border:1px solid var(--line);box-shadow:var(--shadow);background:#111;cursor:zoom-in}
.hero .shot{max-width:1040px;margin:46px auto 0}

/* features */
section{padding:72px 0}
.kicker{color:var(--accent);font-weight:700;font-size:14px;letter-spacing:.12em;text-transform:uppercase;margin:0 0 10px}
h2{font-size:clamp(30px,4vw,44px);line-height:1.12;letter-spacing:-.02em;margin:0 0 16px;font-weight:780}
.lead{color:var(--dim);margin:0 0 22px}
.row{display:grid;grid-template-columns:1fr 1.45fr;gap:56px;align-items:center;padding:44px 0}
.row.flip{grid-template-columns:1.45fr 1fr}
.row.flip .txt{order:2}
.chips{display:flex;flex-wrap:wrap;gap:10px;padding:0;margin:0;list-style:none}
.chips li{background:var(--chip);border:1px solid var(--line);border-radius:999px;padding:6px 14px;font-size:14.5px}
.pair{display:grid;grid-template-columns:1fr 1fr;gap:14px}
@media (max-width:860px){.row,.row.flip{grid-template-columns:1fr;gap:26px}.row.flip .txt{order:0}}

.grid9{display:grid;grid-template-columns:repeat(3,1fr);gap:18px;margin-top:30px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:22px 22px 20px}
.tile h3{margin:0 0 6px;font-size:18.5px;display:flex;align-items:center;gap:10px}
.tile h3::before{content:"";width:9px;height:9px;border-radius:50%;background:var(--accent);flex:none}
.tile.sun h3::before{background:var(--warm)}
.tile p{margin:0;color:var(--dim);font-size:15.5px}
@media (max-width:860px){.grid9{grid-template-columns:1fr 1fr}}
@media (max-width:560px){.grid9{grid-template-columns:1fr}}

/* support */
.support{background:var(--bg2);border:1px solid var(--line);border-radius:22px;padding:44px;text-align:center;position:relative;overflow:hidden}
.support::before{content:"";position:absolute;inset:auto -20% -60% -20%;height:420px;background:radial-gradient(ellipse 40% 50% at 50% 50%,var(--glowB),transparent 70%);pointer-events:none}
.support>*{position:relative}
.amounts{display:flex;flex-wrap:wrap;gap:12px;justify-content:center;margin:26px 0 18px}
.own{display:flex;flex-wrap:wrap;gap:10px;justify-content:center;align-items:center;margin-top:6px}
.own label{color:var(--dim);font-size:15.5px}
.field{display:flex;align-items:center;background:var(--bg);border:1px solid var(--line);border-radius:12px;height:52px;padding:0 6px 0 16px}
.field span{color:var(--dim);font-weight:650}
.field input{width:110px;border:0;background:transparent;color:var(--text);font:inherit;font-size:17px;padding:0 8px;height:100%}
.field input:focus{outline:none}
.field:focus-within{border-color:var(--accent)}
.small{color:var(--faint);font-size:14px;margin-top:16px}

/* steps + faq */
.steps{counter-reset:s;display:grid;grid-template-columns:repeat(3,1fr);gap:18px;padding:0;margin:26px 0 0;list-style:none}
.steps li{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:22px;counter-increment:s}
.steps li::before{content:counter(s);display:inline-grid;place-items:center;width:34px;height:34px;border-radius:50%;
  background:linear-gradient(180deg,var(--accent),var(--accent2));color:#fff;font-weight:700;margin-bottom:10px}
.steps strong{display:block;margin-bottom:4px}
.steps p{margin:0;color:var(--dim);font-size:15.5px}
@media (max-width:760px){.steps{grid-template-columns:1fr}}
.note{margin-top:18px;background:var(--card);border:1px solid var(--line);border-left:4px solid var(--warm);border-radius:12px;padding:16px 18px;color:var(--dim);font-size:15.5px}
.note b{color:var(--text)}
details{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:4px 20px;margin-top:12px}
summary{cursor:pointer;font-weight:650;padding:14px 0;list-style:none;display:flex;justify-content:space-between;gap:16px}
summary::-webkit-details-marker{display:none}
summary::after{content:"+";color:var(--accent);font-size:22px;line-height:1}
details[open] summary::after{content:"–"}
details p{margin:0 0 16px;color:var(--dim)}
code{font:13px/1.5 ui-monospace,Consolas,monospace;word-break:break-all;background:var(--chip);padding:2px 6px;border-radius:6px}

footer{border-top:1px solid var(--line);padding:36px 0 56px;color:var(--faint);font-size:15px;text-align:center}
footer .heart{color:#e04a5a}

/* reveal + lightbox */
.reveal{opacity:0;transform:translateY(18px);transition:opacity .7s ease,transform .7s ease}
.reveal.in{opacity:1;transform:none}
.lb{position:fixed;inset:0;background:rgba(5,6,8,.88);display:none;align-items:center;justify-content:center;padding:24px;z-index:50;cursor:zoom-out}
.lb.open{display:flex}
.lb img{max-height:92vh;border-radius:10px;box-shadow:0 30px 90px rgba(0,0,0,.6)}
.lb button{position:absolute;top:16px;right:16px;width:46px;height:46px;border-radius:50%;border:1px solid rgba(255,255,255,.25);background:rgba(0,0,0,.4);color:#fff;font-size:24px;cursor:pointer}
</style>
</head>
<body>
<a class="skip" href="#main">Skip to content</a>

<header class="hero">
  <div class="wrap">
    <div class="brand"><img src="@ICON@" alt="" width="76" height="76"><h1>EXRay</h1></div>
    <p class="tag">The fast HDR &amp; EXR viewer for 3D artists</p>
    <p class="sub">Browse, inspect and fix your HDRIs and EXR renders as easily as JPGs — one small portable app for Windows 11, no Photoshop needed.</p>
    <div class="cta">
      <a class="btn btn-primary" data-gumroad-action="buy" data-gumroad-price="0" href="#support">Download free</a>
      <a class="btn btn-ghost" href="#support">Support with a tip</a>
    </div>
    <p class="meta"><span>Free for everyone</span><span>Windows 11, 64-bit</span><span>~2 MB, nothing to install</span><span>v@VERSION@</span></p>
    <div class="shot reveal"><img src="@S_image@" alt="EXRay showing an 8K sunset HDRI with exposure and tonemap controls" width="1280" height="720"></div>
  </div>
</header>

<main id="main">
  <section aria-labelledby="f-title">
    <div class="wrap">
      <div class="row reveal">
        <div class="txt">
          <p class="kicker">Thumbnail grid</p>
          <h2 id="f-title">Find the right HDRI in seconds</h2>
          <p class="lead">Open a folder and every .hdr and .exr shows up as a thumbnail — 8K, 16K, even 32K files. No more opening them one by one.</p>
          <ul class="chips"><li>Folders of any size</li><li>Adjustable size</li><li>Arrow-key browsing</li></ul>
        </div>
        <div class="shot"><img src="@S_grid@" alt="A folder of Poly Haven HDRIs as a thumbnail grid" loading="lazy" width="1280" height="720"></div>
      </div>

      <div class="row flip reveal">
        <div class="txt">
          <p class="kicker">360° view</p>
          <h2>Stand inside your HDRI</h2>
          <p class="lead">Drag to look around, use the wheel for field of view. Check the horizon, the sun and the backplate before it goes into your scene.</p>
          <ul class="chips"><li>Drag to look</li><li>Wheel = field of view</li><li>Exposure &amp; tonemaps</li></ul>
        </div>
        <div class="shot"><img src="@S_360@" alt="Looking around inside a sunset HDRI in the 360 degree view" loading="lazy" width="1280" height="720"></div>
      </div>

      <div class="row reveal">
        <div class="txt">
          <p class="kicker">Fix HDRI</p>
          <h2>Check it before it lights your scene</h2>
          <p class="lead">A clipped sun holds only a tiny part of its real light, so shadows go soft and highlights dull. One click finds it — and broken or hot pixels — and tells you how much light is missing. Rebuild the sun with the right brightness for a clear or hazy sky, choose its size and colour, compare before / after and save a clean EXR or HDR. Tested against real unclipped HDRIs: the rebuilt sun lands within about 25 % of the real light.</p>
          <ul class="chips"><li>Clipped sun rebuild</li><li>Sun size &amp; colour</li><li>Clear or hazy sky</li><li>Sun check</li><li>NaN / hot pixels</li><li>Before / after</li></ul>
        </div>
        <div class="shot"><img src="@S_fix@" alt="The Fix HDRI window reporting a clipped sun with a sky choice" loading="lazy" width="1280" height="720"></div>
      </div>

      <div class="row flip reveal">
        <div class="txt">
          <p class="kicker">EXR layers</p>
          <h2>Look inside your renders</h2>
          <p class="lead">Multi-layer EXRs from Unreal's Movie Render Queue, Blender and more. Depth, normals, motion vectors and AO are shown so you can actually read them — with the real values under the mouse.</p>
          <ul class="chips"><li>Multi-part &amp; multi-view</li><li>Readable data passes</li><li>Real pixel values</li></ul>
        </div>
        <div class="shot"><img src="@S_passes@" alt="A world normal pass with the list of render layers open" loading="lazy" width="1280" height="720"></div>
      </div>

      <div class="row reveal">
        <div class="txt">
          <p class="kicker">Cryptomatte</p>
          <h2>Every object, one click away</h2>
          <p class="lead">See object and material names under the mouse and isolate any of them as a clean mask. Ctrl+click to add more, then export.</p>
          <ul class="chips"><li>Names from the manifest</li><li>Click to isolate</li><li>Export the mask</li></ul>
        </div>
        <div class="pair">
          <div class="shot"><img src="@S_crypto@" alt="Cryptomatte ID colours of a car render" loading="lazy" width="1280" height="720"></div>
          <div class="shot"><img src="@S_mask@" alt="One material isolated as a white mask" loading="lazy" width="1280" height="720"></div>
        </div>
      </div>
    </div>
  </section>

  <section aria-labelledby="all-title" style="padding-top:20px">
    <div class="wrap">
      <p class="kicker">Everything in one small app</p>
      <h2 id="all-title">All of it is free</h2>
      <div class="grid9">
        <div class="tile reveal"><h3>Thumbnail grid</h3><p>Whole folders of HDRIs at a glance, 16K and up.</p></div>
        <div class="tile reveal"><h3>Exposure &amp; tonemaps</h3><p>−10 to +10 EV with Clip, Reinhard and ACES.</p></div>
        <div class="tile reveal"><h3>360° view</h3><p>Look around inside equirectangular HDRIs.</p></div>
        <div class="tile reveal"><h3>EXR layers</h3><p>Render passes, depth, normals, motion vectors.</p></div>
        <div class="tile reveal"><h3>Cryptomatte</h3><p>Object names under the mouse, one-click masks.</p></div>
        <div class="tile reveal"><h3>Pixel readout</h3><p>The real stored values, copied with Ctrl+C.</p></div>
        <div class="tile sun reveal"><h3>Fix HDRI</h3><p>Rebuild a clipped sun — size and colour your choice — and repair broken pixels.</p></div>
        <div class="tile reveal"><h3>Export</h3><p>PNG / JPEG, single images or whole selections.</p></div>
        <div class="tile reveal"><h3>Portable</h3><p>One .exe, no install, no account, offline.</p></div>
      </div>
    </div>
  </section>

  <section id="support" aria-labelledby="s-title">
    <div class="wrap">
      <div class="support reveal">
        <p class="kicker">Free for everyone</p>
        <h2 id="s-title">Download it free — tip if you like it</h2>
        <p class="lead" style="max-width:620px;margin-left:auto;margin-right:auto">Every feature, no time limit, for personal use and paid client work. If EXRay saves you time, a tip helps keep it growing. It's completely optional and changes nothing in the app.</p>
        <div class="amounts" role="group" aria-label="Choose an amount">
          <a class="btn btn-primary" data-gumroad-action="buy" data-gumroad-price="0" href="#support">Download free</a>
          <a class="btn btn-ghost" data-gumroad-action="buy" data-gumroad-price="5" href="#support">Tip €5</a>
          <a class="btn btn-ghost" data-gumroad-action="buy" data-gumroad-price="10" href="#support">Tip €10</a>
        </div>
        <div class="own">
          <label for="gr-price">Or your own amount</label>
          <div class="field"><span aria-hidden="true">€</span><input id="gr-price" type="number" min="0" step="1" placeholder="15" aria-label="Your amount in euros"></div>
          <button class="btn btn-ghost" id="gr-buy" type="button">Tip &amp; download</button>
        </div>
        <p class="small">Secure checkout by Gumroad. The download link is emailed to you too, and new versions arrive the same way.</p>
      </div>
    </div>
  </section>

  <section aria-labelledby="start-title" style="padding-top:20px">
    <div class="wrap">
      <p class="kicker">Getting started</p>
      <h2 id="start-title">Running in under a minute</h2>
      <ol class="steps">
        <li class="reveal"><strong>Unzip anywhere</strong><p>Desktop, a tools folder or a USB stick. Nothing is installed.</p></li>
        <li class="reveal"><strong>Run EXRay</strong><p>Double-click the .exe. Settings stay in a small file next to it.</p></li>
        <li class="reveal"><strong>Drop your files</strong><p>Drop an .hdr, an .exr or a whole folder onto the window. F1 lists every shortcut.</p></li>
      </ol>
      <p class="note"><b>"Windows protected your PC"?</b> EXRay comes from an independent developer and isn't code-signed yet, so Windows doesn't know it. Click <b>More info → Run anyway</b>.</p>

      <h2 style="margin-top:64px">Questions</h2>
      <details><summary>Is it really free?</summary><p>Yes — every feature, no time limit, no ads, no account. Paying is optional and only supports development.</p></details>
      <details><summary>Is it open source?</summary><p>Yes. EXRay's source code is on <a href="https://github.com/p1xelrise/EXRay">GitHub</a> under the GNU GPL v3. You can read exactly what it does, build it yourself, report bugs or suggest features there.</p></details>
      <details><summary>Can I use it for client work?</summary><p>Yes. Personal and professional use, including paid client work. The images you view, fix or export are yours.</p></details>
      <details><summary>Does it need the internet?</summary><p>No. EXRay works fully offline and collects nothing.</p></details>
      <details><summary>Which files does it open?</summary><p>Radiance .hdr and OpenEXR .exr: multi-layer, multi-part and multi-view, half or float, most compressions (not DWAA/DWAB or deep EXRs yet).</p></details>
      <details><summary>Does the sun fix really give the right light?</summary><p>We tested it on real HDRIs whose suns are not clipped: clip a copy, fix it, and compare with the original. With the sky set to match the photo (Clear, Light haze or Hazy), the rebuilt sun gave 75–122 % of the real light in every case. A clipped file no longer records how strong the sun was, so the sky choice is yours — EXRay can't guess haze.</p></details>
      <details><summary>How do I know if my HDRI's sun is clipped?</summary><p>Press S (Sun check): EXRay looks straight at the sun with the exposure set for a real sun. A real sun is a small white disc; a clipped one looks dark, and the status bar shows the numbers. Fix HDRI also tells you how much of the sun's light is missing.</p></details>
      <details><summary>Mac or Linux?</summary><p>Windows 11 (64-bit) only for now.</p></details>
      <details><summary>It's a beta — what does that mean?</summary><p>It works well, but has been tested on only a few PCs so far. If something goes wrong, please open an issue on <a href="https://github.com/p1xelrise/EXRay/issues">GitHub</a> or write to <a href="mailto:pixelrise@gmail.com">pixelrise@gmail.com</a>.</p></details>
      <details><summary>How can I check the download is genuine?</summary><p>SHA-256 of EXRay_v@VERSIONU@.exe: <code>@SHA@</code></p></details>
    </div>
  </section>
</main>

<footer>
  <div class="wrap">
    <p>Made with <span class="heart" aria-label="love">❤</span> from Greece by pixelrise · <a href="mailto:pixelrise@gmail.com">pixelrise@gmail.com</a> · <a href="https://github.com/p1xelrise/EXRay">Source code on GitHub</a></p>
    <p>EXRay v@VERSION@ · Free, no ads, no tracking, works offline</p>
  </div>
</footer>

<div class="lb" id="lb" role="dialog" aria-modal="true" aria-label="Screenshot"><button type="button" aria-label="Close">×</button><img alt=""></div>

<script>
(function () {
  // Pay-what-you-want with the buyer's own amount.
  var input = document.getElementById("gr-price");
  function go() {
    var v = (input.value || "").trim(), params = {};
    if (v !== "") { var n = parseFloat(v); if (!isNaN(n) && n >= 0) params.price = String(n); }
    parent.postMessage({ type: "gumroad:checkout", params: params }, "*");
  }
  document.getElementById("gr-buy").addEventListener("click", go);
  input.addEventListener("keydown", function (e) { if (e.key === "Enter") go(); });

  // Fade sections in as they scroll into view.
  var els = document.querySelectorAll(".reveal");
  if ("IntersectionObserver" in window && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (e) { if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); } });
    }, { rootMargin: "0px 0px -8% 0px" });
    els.forEach(function (el) { io.observe(el); });
  } else { els.forEach(function (el) { el.classList.add("in"); }); }

  // Click a screenshot to see it large.
  var lb = document.getElementById("lb"), big = lb.querySelector("img"), last = null;
  function close() { lb.classList.remove("open"); if (last) last.focus(); }
  document.querySelectorAll(".shot img").forEach(function (img) {
    img.tabIndex = 0;
    function open() { last = img; big.src = img.src; big.alt = img.alt; lb.classList.add("open"); lb.querySelector("button").focus(); }
    img.addEventListener("click", open);
    img.addEventListener("keydown", function (e) { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open(); } });
  });
  lb.addEventListener("click", close);
  document.addEventListener("keydown", function (e) { if (e.key === "Escape" && lb.classList.contains("open")) close(); });
})();
</script>
</body>
</html>
"""
TEMPLATE = html
for WIDTH, QUALITY in [(1280, 80), (1280, 72), (1200, 70), (1100, 68), (1024, 65), (960, 60)]:
    S = build_shots()
    html = TEMPLATE
    html = html.replace("@ICON@", ICON).replace("@VERSION@", VERSION).replace("@VERSIONU@", VERSION.replace(" ", "_")).replace("@SHA@", SHA)
    for k, v in S.items():
        html = html.replace("@S_" + k + "@", v)
    print(f"  {WIDTH}px q{QUALITY}: {len(html):,} characters")
    if len(html) <= LIMIT:
        break
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w", encoding="utf-8").write(html)
print("wrote", OUT, f"{len(html):,} characters (Gumroad limit 500,000)")
