#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 moogle contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Round-trip test for resources/renderedview.js.

Renders each fixture with the bundled cmark-gfm (same options moogle uses),
loads renderedview.js into headless Chrome with a mock QWebChannel, and checks:

  * every visible letter/digit in the Markdown maps to a caret placed right
    before that same character in the rendered DOM;
  * mapping a source offset to the DOM and back never moves backwards
    (except past the last block, which clamps to its end);
  * scrolling a source line to the top and reading the top line back is
    within one line.

Usage: mapping_test.py --cmark BUILD/bin/cmark-gfm [--chrome google-chrome-stable]
Exit status is non-zero on failure.
"""
import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CMARK_ARGS = ["--sourcepos", "--smart", "--unsafe", "-e", "table", "-e", "strikethrough",
              "-e", "autolink", "-e", "tagfilter", "-e", "tasklist"]


def log(level, msg, **kw):
    rec = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"), "level": level,
           "context": "mapping_test", "message": msg}
    rec.update(kw)
    print(json.dumps(rec), file=sys.stderr)


TEST_JS = r"""
window.qt = { webChannelTransport: {} };
window.__logs = [];
window.QWebChannel = function (t, cb) {
  cb({ objects: { moogleBridge: {
    styleSheet: '', styleSheetChanged: { connect() {} },
    log(l, m, f) { window.__logs.push([l, m, f]); },
    presented() {}, pageReady() {}, requestToggle() {}, openLink() {}
  } } });
};
window.addEventListener('load', () => {
  const T = window.moogle._t;
  const content = document.getElementById('moogle-content');
  const isWord = (c) => /[\p{L}\p{N}]/u.test(c);
  const charAfter = (pt) => {
    const r = document.createRange();
    r.setStart(pt.node, pt.offset);
    r.setEnd(content, content.childNodes.length);
    return r.toString()[0];
  };
  const out = [];
  for (const doc of DOCS) {
    window.moogle.present({ token: 1, html: doc.html, source: doc.md });
    const res = { name: doc.name, blocks: T.st.blocks.length, failures: [], scrollMaxErr: 0 };
    const lastEnd = T.st.blocks.length ? T.st.blocks[T.st.blocks.length - 1].end : 0;
    for (let p = 0; p <= doc.md.length; p++) {
      const pt = T.pointFromOffset(p);
      const q = T.offsetFromPoint(pt.node, pt.offset);
      const ctx = doc.md.slice(Math.max(0, p - 12), p) + '|' + doc.md.slice(p, p + 12);
      if (q === null || (q < p && p <= lastEnd)) {
        res.failures.push({ kind: 'backwards', p, q, ctx });
      }
      const sc = doc.md[p];
      if (sc && isWord(sc) && T.st.hidden[p] === 0 && T.innermostByOffset(p)) {
        const rc = charAfter(pt);
        if (rc !== sc) res.failures.push({ kind: 'caret', p, sc, rc, ctx });
      }
    }
    const n = T.st.lineStarts.length;
    for (let L = 1; L < n; L++) {
      for (const f of [0, 0.5]) {
        const a = T.yToAnchor(T.anchorToY({ offset: T.st.lineStarts[L - 1], fraction: f }));
        const err = Math.abs(T.lineOfOffset(a.offset) + a.fraction - (L + f));
        res.scrollMaxErr = Math.max(res.scrollMaxErr, err);
        if (err > 1) res.failures.push({ kind: 'scroll', L, f, err });
      }
    }
    out.push(res);
  }
  const pre = document.createElement('pre');
  pre.id = 'mapping-result';
  pre.textContent = JSON.stringify({ results: out, logs: window.__logs });
  document.body.appendChild(pre);
});
"""


def build_page(js, docs):
    needle = "window.moogle = { present, capture, scrollToHeading };"
    if needle not in js:
        raise RuntimeError("renderedview.js export line changed; update the test hook")
    js = js.replace(needle, "window.moogle = { present, capture, scrollToHeading, _t: { st, pointFromOffset, "
                    "offsetFromPoint, anchorToY, yToAnchor, lineOfOffset, innermostByOffset } };")
    css = ("#livepreviewplaceholder{position:fixed;overflow-y:scroll;top:8px;bottom:0;left:0;"
           "width:calc(100% - 16px);padding:5px}body{margin:0;overflow:hidden;font:12pt serif;line-height:1.5}"
           "#moogle-content{margin:0 auto;max-width:700px}")
    return (f"<!doctype html><html><head><meta charset='utf-8'><style>{css}</style>"
            f"<script>const DOCS = {json.dumps(docs)};</script><script>{TEST_JS}</script>"
            f"<script>{js}</script></head><body><div id='livepreviewplaceholder'>"
            f"<div id='moogle-content' contenteditable='true'></div></div></body></html>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmark", required=True, help="path to the bundled cmark-gfm executable")
    ap.add_argument("--chrome", default=os.environ.get("CHROME", "google-chrome-stable"))
    args = ap.parse_args()

    chrome = shutil.which(args.chrome) or (args.chrome if os.path.exists(args.chrome) else None)
    if not chrome:
        log("error", "chrome not found; set --chrome or CHROME", chrome=args.chrome)
        return 2

    docs = []
    for name in sorted(os.listdir(os.path.join(HERE, "docs"))):
        path = os.path.join(HERE, "docs", name)
        try:
            out = subprocess.run([args.cmark, *CMARK_ARGS, path], capture_output=True, check=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as exc:
            log("error", "cmark-gfm failed", error=str(exc), fixture=name)
            return 2
        docs.append({"name": name, "md": open(path, encoding="utf-8").read(), "html": out.stdout.decode()})

    js = open(os.path.join(ROOT, "resources", "renderedview.js"), encoding="utf-8").read()

    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "harness.html")
        with open(page, "w", encoding="utf-8") as fh:
            fh.write(build_page(js, docs))
        t0 = time.time()
        try:
            dom = subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-first-run",
                                  f"--user-data-dir={os.path.join(tmp, 'profile')}", "--window-size=1000,800",
                                  "--virtual-time-budget=10000", "--dump-dom", "file://" + page],
                                 capture_output=True, timeout=120).stdout.decode()
        except (OSError, subprocess.SubprocessError) as exc:
            log("error", "headless chrome failed", error=str(exc))
            return 2
        log("info", "chrome finished", seconds=round(time.time() - t0, 1))

    m = re.search(r'<pre id="mapping-result">(\{.*?)</pre>', dom, re.S)
    if not m:
        log("error", "no result in dumped DOM", head=dom[:300])
        return 2

    data = json.loads(html.unescape(m.group(1)))
    failed = False
    for res in data["results"]:
        status = "ok" if not res["failures"] else "FAIL"
        failed |= bool(res["failures"])
        print(f"{status:4} {res['name']}: blocks={res['blocks']} failures={len(res['failures'])} "
              f"scrollMaxErr={res['scrollMaxErr']:.2f}")
        for f in res["failures"][:20]:
            print("     ", json.dumps(f, ensure_ascii=False))
    for level, message, fields in data["logs"]:
        if level in ("warn", "error"):
            print(f"page {level}: {message} {fields}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
