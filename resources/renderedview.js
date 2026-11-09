/*
 * SPDX-FileCopyrightText: 2026 moogle contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * In-place rendered view. The page shows cmark-gfm HTML rendered with
 * data-sourcepos="l1:c1-l2:c2" on block elements (1-based lines, 1-based
 * UTF-8 byte columns, inclusive end) and translates between Markdown source
 * offsets and DOM positions:
 *
 *  - scroll: the source line at the top of the view <-> the innermost block
 *    covering that line, interpolated linearly over the block's height;
 *    lines between blocks interpolate across the gap.
 *  - caret/selection: inside a block, the block's source slice S is aligned
 *    with its rendered text R by a Myers diff (markup characters in S are
 *    deletions, smart punctuation in R insertions), giving S<->R index maps
 *    that convert to and from (text node, offset) DOM points.
 *
 * Source offsets are UTF-16 indices into the exact Markdown string that was
 * rendered, which equal QTextDocument character positions.
 *
 * The content is contenteditable so Chromium provides a real caret, keyboard
 * navigation and shift-selection, but every edit is cancelled.
 */
'use strict';

(function () {
    const BLOCK_TAGS = new Set([
        'P', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'LI', 'BLOCKQUOTE', 'PRE',
        'HR', 'TABLE', 'THEAD', 'TBODY', 'TR', 'TD', 'TH', 'UL', 'OL', 'DIV',
        'DL', 'DT', 'DD', 'SECTION'
    ]);
    // Upper bound on edit distance for the exact diff, and on total diff
    // work (chars x distance) before falling back to the greedy aligner.
    const MAX_DIFF_D = 1200;
    const MAX_DIFF_WORK = 30e6;
    const WIDTH_WAIT_MS = 400;

    const st = {
        bridge: null,
        outbox: [],
        content: null,
        scroller: null,
        observer: null,
        mutating: false,
        html: '',
        source: '',
        lineStarts: [0],
        hidden: new Uint8Array(0),
        blocks: [],
        blockByEl: new Map(),
        topLevel: [],
        align: new WeakMap(),
        appliedScrollTop: null,
        appliedSelection: null,
        anchor: null,
        userAnchor: null,
        scrollDirty: false,
        anchorFrame: 0
    };

    // ---------------------------------------------------------------- logging

    function log(level, message, fields) {
        const payload = JSON.stringify(fields || {});

        if (st.bridge) {
            st.bridge.log(level, message, payload);
        } else {
            st.outbox.push([level, message, payload]);
        }
    }

    function errorFields(e, extra) {
        return Object.assign({
            error: String(e && e.message ? e.message : e),
            stack: e && e.stack ? String(e.stack) : ''
        }, extra || {});
    }

    // ------------------------------------------------------- source geometry

    function computeLineStarts(src) {
        const starts = [0];

        for (let i = 0; i < src.length; i++) {
            if (src.charCodeAt(i) === 10) {
                starts.push(i + 1);
            }
        }

        return starts;
    }

    function lineCount() {
        return st.lineStarts.length;
    }

    function lineText(line) {
        const begin = st.lineStarts[line - 1];
        const end = (line < st.lineStarts.length) ? st.lineStarts[line] - 1 : st.source.length;
        return st.source.slice(begin, end);
    }

    // 1-based line containing a source offset.
    function lineOfOffset(offset) {
        let lo = 0;
        let hi = st.lineStarts.length - 1;

        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1;

            if (st.lineStarts[mid] <= offset) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }

        return lo + 1;
    }

    // UTF-16 index of the first character whose UTF-8 start byte is >= byteOffset.
    function indexForByte(text, byteOffset) {
        let bytes = 0;
        let i = 0;

        while (i < text.length) {
            if (bytes >= byteOffset) {
                return i;
            }

            const cp = text.codePointAt(i);
            bytes += (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
            i += (cp > 0xFFFF) ? 2 : 1;
        }

        return text.length;
    }

    // Marks source characters that never appear as rendered text (link
    // destinations, HTML tags, entity names, fence lines, list markers...).
    // Without this the diff happily matches, say, the "." of a URL to the
    // sentence's final period. Code is literal, so it is never masked.
    function computeHiddenMask(src) {
        const hidden = new Uint8Array(src.length);
        const literal = new Uint8Array(src.length);
        const mark = (arr, a, b) => {
            for (let i = Math.max(0, a); i < Math.min(src.length, b); i++) {
                arr[i] = 1;
            }
        };
        const starts = computeLineStarts(src);
        let fence = null;

        for (let li = 0; li < starts.length; li++) {
            const ls = starts[li];
            const le = (li + 1 < starts.length) ? starts[li + 1] - 1 : src.length;
            const text = src.slice(ls, le);
            const fm = /^((?: {0,3}>[ \t]?)*[ \t]{0,3})(`{3,}|~{3,})(.*)$/.exec(text);

            if (fence) {
                if (fm && fm[2][0] === fence[0] && fm[2].length >= fence.length && fm[3].trim() === '') {
                    mark(hidden, ls, le);
                    fence = null;
                } else {
                    mark(literal, ls, le);
                }

                continue;
            }

            if (fm && !(fm[2][0] === '`' && fm[3].includes('`'))) {
                fence = fm[2];
                mark(hidden, ls, le);
                continue;
            }

            if (/^ {0,3}\[[^\]^][^\]]*\]:\s*\S/.test(text)
                    || /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)+\|?\s*$/.test(text)
                    || /^ {0,3}(=+|-+)\s*$/.test(text)) {
                // Reference definitions, table delimiter rows, setext underlines.
                mark(hidden, ls, le);
                continue;
            }

            const lm = /^((?: {0,3}>[ \t]?)*[ \t]*)(?:[*+-]|\d{1,9}[.)])([ \t]+)(\[[ xX]\][ \t])?/.exec(text);

            if (lm) {
                mark(hidden, ls + lm[1].length, ls + lm[0].length);
            }
        }

        const each = (re, fn) => {
            re.lastIndex = 0;

            for (let m = re.exec(src); m; m = re.exec(src)) {
                // fn may return where scanning should resume.
                const resume = fn(m);

                if (typeof resume === 'number') {
                    re.lastIndex = resume;
                } else if (m[0].length === 0) {
                    re.lastIndex++;
                }
            }
        };

        each(/(`+)(?!`)([\s\S]*?[^`])\1(?!`)/g, (m) => {
            // Fence lines are already handled and code spans never cross a
            // blank line; rescan right after this backtick run instead.
            if (hidden[m.index] || /\n[ \t]*\n/.test(m[0])) {
                return m.index + m[1].length;
            }

            mark(literal, m.index + m[1].length, m.index + m[0].length - m[1].length);
            return undefined;
        });
        // Inline link/image destination and title: "](...)".
        each(/\]\((?:<[^>\n]*>|[^()\s]*(?:\([^()\s]*\)[^()\s]*)*)(?:\s+(?:"[^"]*"|'[^']*'|\([^)]*\)))?\s*\)/g,
            (m) => mark(hidden, m.index + 1, m.index + m[0].length));
        // Image alt text is an attribute, not text.
        each(/!\[[^\]\n]*\]/g, (m) => mark(hidden, m.index, m.index + m[0].length));
        each(/<!--[\s\S]*?-->/g, (m) => mark(hidden, m.index, m.index + m[0].length));
        each(/<\/?[A-Za-z][A-Za-z0-9-]*(?:\s[^<>]*)?\/?>/g, (m) => mark(hidden, m.index, m.index + m[0].length));
        const entities = [];
        each(/&(?:#\d{1,7}|#[xX][0-9a-fA-F]{1,6}|[A-Za-z][A-Za-z0-9]{1,31});/g, (m) => {
            mark(hidden, m.index, m.index + m[0].length);
            entities.push(m.index);
        });

        for (const i of entities) {
            hidden[i] = 2; // the "&" stands in for the rendered glyph
        }

        for (let i = 0; i < src.length; i++) {
            if (literal[i]) {
                hidden[i] = 0;
            }
        }

        return hidden;
    }

    function clampLine(line) {
        return Math.min(Math.max(1, line), lineCount());
    }

    function buildBlocks() {
        const blocks = [];
        const byEl = new Map();
        const topLevel = [];
        const sourcePosRe = /^(\d+):(\d+)-(\d+):(\d+)$/;
        let skipped = 0;

        for (const el of st.content.querySelectorAll('[data-sourcepos]')) {
            if (!BLOCK_TAGS.has(el.tagName)) {
                continue;
            }

            const m = sourcePosRe.exec(el.getAttribute('data-sourcepos'));

            if (!m) {
                skipped++;
                continue;
            }

            const l1 = clampLine(+m[1]);
            const c1 = +m[2];
            let l2 = Math.max(l1, clampLine(+m[3]));
            const c2 = +m[4];
            let end;

            if (c2 <= 0) {
                // "L:0" means the block ended with the newline that closes
                // line L-1 (setext headings, lists, indented code).
                l2 = Math.max(l1, l2 - 1);
                end = st.lineStarts[l2 - 1] + lineText(l2).length;
            } else {
                end = st.lineStarts[l2 - 1] + indexForByte(lineText(l2), c2);
            }

            const start = st.lineStarts[l1 - 1] + indexForByte(lineText(l1), Math.max(0, c1 - 1));
            const block = { el, l1, l2, start, end: Math.max(start, end) };

            blocks.push(block);
            byEl.set(el, block);

            if (el.parentElement === st.content) {
                topLevel.push(block);
            }
        }

        if (skipped > 0) {
            log('warn', 'blocks with malformed data-sourcepos skipped', { skipped });
        }

        st.blocks = blocks;
        st.blockByEl = byEl;
        st.topLevel = topLevel;
    }

    // ------------------------------------------------------------- alignment

    // Markdown characters that smart typography rewrites in the rendered
    // text: quotes curl, "..." becomes an ellipsis, "--"/"---" become dashes
    // (the first source character matches the glyph, the rest are deleted).
    function eqCode(s, r) {
        if (s === r) {
            return true;
        }

        switch (s) {
        case 34:
            return r === 0x201C || r === 0x201D;
        case 39:
            return r === 0x2018 || r === 0x2019;
        case 46:
            return r === 0x2026;
        case 45:
            return r === 0x2013 || r === 0x2014;
        default:
            return false;
        }
    }

    // Source index i may match R char r: never if hidden, always if it is the
    // "&" that stands for a whole entity's rendered glyph.
    function canMatch(i, sc, rc) {
        const h = st.hidden[i];
        return (h === 0) ? eqCode(sc, rc) : (h === 2);
    }

    // Myers O((N+M)D) diff; returns index maps or null if D exceeds the limit.
    // S starts at source offset base (for the hidden-character mask).
    function myersAlign(S, R, base) {
        const n = S.length;
        const m = R.length;
        const s2r = new Int32Array(n).fill(-1);
        const r2s = new Int32Array(m).fill(-1);

        if (n === 0 || m === 0) {
            return { s2r, r2s, method: 'trivial' };
        }

        const limit = Math.min(MAX_DIFF_D, n + m, Math.max(16, Math.floor(MAX_DIFF_WORK / (n + m))));
        const off = limit + 1;
        const v = new Int32Array(2 * limit + 3);
        const trace = [];
        let found = -1;

        outer:
        for (let d = 0; d <= limit; d++) {
            // Window of V for k in [-d-1, d+1], as read during round d.
            trace.push(v.slice(off - d - 1, off + d + 2));

            for (let k = -d; k <= d; k += 2) {
                let x;

                if (k === -d || (k !== d && v[off + k - 1] < v[off + k + 1])) {
                    x = v[off + k + 1];
                } else {
                    x = v[off + k - 1] + 1;
                }

                let y = x - k;

                while (x < n && y < m && canMatch(base + x, S.charCodeAt(x), R.charCodeAt(y))) {
                    x++;
                    y++;
                }

                v[off + k] = x;

                if (x >= n && y >= m) {
                    found = d;
                    break outer;
                }
            }
        }

        if (found < 0) {
            return null;
        }

        let x = n;
        let y = m;

        for (let d = found; d > 0; d--) {
            const w = trace[d];
            const get = (k) => w[k + d + 1];
            const k = x - y;
            const prevK = (k === -d || (k !== d && get(k - 1) < get(k + 1))) ? k + 1 : k - 1;
            const prevX = get(prevK);
            const prevY = prevX - prevK;

            while (x > prevX && y > prevY) {
                x--;
                y--;
                s2r[x] = y;
                r2s[y] = x;
            }

            x = prevX;
            y = prevY;
        }

        while (x > 0 && y > 0) {
            x--;
            y--;
            s2r[x] = y;
            r2s[y] = x;
        }

        return { s2r, r2s, method: 'myers', distance: found };
    }

    // Linear fallback: skip source characters (markup) unless the rendered
    // character does not occur shortly ahead, in which case it was inserted.
    function greedyAlign(S, R, base) {
        const n = S.length;
        const m = R.length;
        const s2r = new Int32Array(n).fill(-1);
        const r2s = new Int32Array(m).fill(-1);
        let i = 0;
        let j = 0;

        while (i < n && j < m) {
            const rc = R.charCodeAt(j);

            if (canMatch(base + i, S.charCodeAt(i), rc)) {
                s2r[i] = j;
                r2s[j] = i;
                i++;
                j++;
                continue;
            }

            let ahead = false;

            for (let t = i + 1; t < Math.min(n, i + 16); t++) {
                if (canMatch(base + t, S.charCodeAt(t), rc)) {
                    ahead = true;
                    break;
                }
            }

            if (ahead) {
                i++;
            } else {
                j++;
            }
        }

        return { s2r, r2s, method: 'greedy' };
    }

    function getAlign(block) {
        let a = st.align.get(block.el);

        if (a) {
            return a;
        }

        const t0 = performance.now();
        const nodes = [];
        const starts = [];
        const parts = [];
        let total = 0;
        const walker = document.createTreeWalker(block.el, NodeFilter.SHOW_TEXT);

        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            nodes.push(node);
            starts.push(total);
            parts.push(node.data);
            total += node.data.length;
        }

        const S = st.source.slice(block.start, block.end);
        const R = parts.join('');
        let maps = myersAlign(S, R, block.start);

        if (!maps) {
            log('warn', 'diff limit exceeded; using greedy alignment', { sourceChars: S.length, renderedChars: R.length, tag: block.el.tagName });
            maps = greedyAlign(S, R, block.start);
        }

        a = { S, R, nodes, starts, s2r: maps.s2r, r2s: maps.r2s };
        st.align.set(block.el, a);

        const ms = performance.now() - t0;

        if (ms > 20) {
            log('info', 'slow block alignment', { ms: Math.round(ms), method: maps.method, sourceChars: S.length, renderedChars: R.length });
        }

        return a;
    }

    // Caret before source index k -> caret before the rendered index it maps to.
    function sourceToRendered(a, k) {
        for (let j = k; j < a.S.length; j++) {
            if (a.s2r[j] >= 0) {
                return a.s2r[j];
            }
        }

        return a.R.length;
    }

    function renderedToSource(a, r) {
        if (r >= a.R.length) {
            return a.S.length;
        }

        if (a.r2s[r] >= 0) {
            return a.r2s[r];
        }

        for (let j = r - 1; j >= 0; j--) {
            if (a.r2s[j] >= 0) {
                return a.r2s[j] + 1;
            }
        }

        return 0;
    }

    // ------------------------------------------------- offset <-> DOM point

    function innermostByOffset(p) {
        let best = null;

        for (const b of st.blocks) {
            if (b.start <= p && p <= b.end) {
                best = b; // document order: later matches are nested deeper
            }
        }

        return best;
    }

    function pointInBlock(block, r) {
        const a = getAlign(block);

        if (a.nodes.length === 0) {
            return { node: block.el, offset: 0 };
        }

        for (let i = 0; i < a.nodes.length; i++) {
            if (r < a.starts[i] + a.nodes[i].data.length) {
                return { node: a.nodes[i], offset: Math.max(0, r - a.starts[i]) };
            }
        }

        const last = a.nodes[a.nodes.length - 1];
        return { node: last, offset: last.data.length };
    }

    function pointFromOffset(p) {
        const block = innermostByOffset(p);

        if (block) {
            return pointInBlock(block, sourceToRendered(getAlign(block), p - block.start));
        }

        // Between blocks (blank lines, link reference definitions): use the
        // start of the next block, else the end of the last one.
        for (const b of st.blocks) {
            if (b.start > p) {
                return pointInBlock(b, 0);
            }
        }

        if (st.blocks.length > 0) {
            const last = st.blocks[st.blocks.length - 1];
            return pointInBlock(last, getAlign(last).R.length);
        }

        return { node: st.content, offset: 0 };
    }

    function blockAncestor(node) {
        let el = (node.nodeType === Node.ELEMENT_NODE) ? node : node.parentElement;

        while (el && el !== st.content) {
            const b = st.blockByEl.get(el);

            if (b) {
                return b;
            }

            el = el.parentElement;
        }

        return null;
    }

    function offsetFromPoint(node, offset) {
        if (!st.content.contains(node)) {
            return null;
        }

        const block = blockAncestor(node);

        if (!block) {
            const caret = document.createRange();
            caret.setStart(node, offset);
            caret.collapse(true);

            for (const b of st.blocks) {
                if (caret.comparePoint(b.el, 0) > 0) {
                    return b.start;
                }
            }

            return st.blocks.length > 0 ? st.blocks[st.blocks.length - 1].end : 0;
        }

        const before = document.createRange();
        before.setStart(block.el, 0);
        before.setEnd(node, offset);

        const r = before.toString().length;
        return block.start + renderedToSource(getAlign(block), r);
    }

    // --------------------------------------------------------------- scroll

    function rectsOf(blocks) {
        const base = st.scroller.getBoundingClientRect().top - st.scroller.scrollTop;
        return blocks.map((b) => {
            const r = b.el.getBoundingClientRect();
            return { b, top: r.top - base, bottom: r.bottom - base, height: r.height };
        });
    }

    // Content y for a source anchor {offset, fraction}: the top of the line
    // containing offset, plus fraction of that line.
    function anchorToY(anchor) {
        const L = lineOfOffset(anchor.offset);
        const f = Math.min(Math.max(anchor.fraction || 0, 0), 1);
        let best = null;

        for (const b of st.blocks) {
            if (b.l1 <= L && L <= b.l2) {
                best = b;
            }
        }

        if (best) {
            const r = rectsOf([best])[0];
            return r.top + ((L - best.l1) + f) / (best.l2 - best.l1 + 1) * r.height;
        }

        let prev = null;
        let next = null;

        for (const r of rectsOf(st.topLevel)) {
            if (r.b.l2 < L) {
                prev = r;
            } else if (r.b.l1 > L && !next) {
                next = r;
            }
        }

        const yPrev = prev ? prev.bottom : 0;
        const yNext = next ? next.top : yPrev;
        const g0 = prev ? prev.b.l2 + 1 : 1;
        const g1 = next ? next.b.l1 : L + 1;
        const t = (L - g0 + f) / Math.max(1, g1 - g0);
        return yPrev + t * (yNext - yPrev);
    }

    function lineToAnchor(lineFloat) {
        const L = clampLine(Math.floor(lineFloat));
        return { offset: st.lineStarts[L - 1], fraction: Math.min(Math.max(lineFloat - Math.floor(lineFloat), 0), 0.999) };
    }

    function yToAnchor(y) {
        const rects = rectsOf(st.blocks);
        let best = null;

        for (const r of rects) {
            if (r.top <= y && y < r.bottom && r.height > 0) {
                best = r;
            }
        }

        if (best) {
            const span = best.b.l2 - best.b.l1 + 1;
            return lineToAnchor(best.b.l1 + (y - best.top) / best.height * span);
        }

        let prev = null;
        let next = null;

        for (const r of rectsOf(st.topLevel)) {
            if (r.bottom <= y) {
                prev = r;
            } else if (r.top > y && !next) {
                next = r;
            }
        }

        const yPrev = prev ? prev.bottom : 0;
        const yNext = next ? next.top : yPrev;
        const g0 = prev ? prev.b.l2 + 1 : 1;
        const g1 = next ? next.b.l1 : g0;
        const t = (yNext > yPrev) ? (y - yPrev) / (yNext - yPrev) : 0;
        return lineToAnchor(g0 + t * (g1 - g0));
    }

    function setScrollTop(y) {
        st.scroller.scrollTop = Math.max(0, Math.round(y));
        st.appliedScrollTop = st.scroller.scrollTop;
    }

    function onScroll() {
        if (st.appliedScrollTop !== null && Math.abs(st.scroller.scrollTop - st.appliedScrollTop) <= 1) {
            return;
        }

        st.scrollDirty = true;

        if (!st.anchorFrame) {
            st.anchorFrame = requestAnimationFrame(() => {
                st.anchorFrame = 0;

                try {
                    st.userAnchor = yToAnchor(st.scroller.scrollTop);
                } catch (e) {
                    log('error', 'failed to track scroll anchor', errorFields(e));
                }
            });
        }
    }

    // Keep the same source line at the top when the layout changes size.
    function reanchor(reason) {
        const anchor = st.scrollDirty ? st.userAnchor : st.anchor;

        if (!anchor) {
            return;
        }

        try {
            const keepDirty = st.scrollDirty;
            setScrollTop(anchorToY(anchor));
            st.scrollDirty = keepDirty;
        } catch (e) {
            log('error', 'failed to re-anchor scroll', errorFields(e, { reason }));
        }
    }

    // ------------------------------------------------------------- content

    function setContent(html, source) {
        st.mutating = true;
        st.content.innerHTML = html;
        st.observer.takeRecords();
        st.mutating = false;

        st.html = html;
        st.source = source;
        st.lineStarts = computeLineStarts(source);
        st.hidden = computeHiddenMask(source);
        st.align = new WeakMap();
        st.appliedScrollTop = null;
        st.appliedSelection = null;
        st.anchor = null;
        st.userAnchor = null;
        st.scrollDirty = false;
        buildBlocks();
    }

    function snapshotSelection(sel) {
        return {
            anchorNode: sel.anchorNode,
            anchorOffset: sel.anchorOffset,
            focusNode: sel.focusNode,
            focusOffset: sel.focusOffset
        };
    }

    function sameSelection(sel, snap) {
        return !!snap
            && sel.anchorNode === snap.anchorNode && sel.anchorOffset === snap.anchorOffset
            && sel.focusNode === snap.focusNode && sel.focusOffset === snap.focusOffset;
    }

    function applyState(p) {
        const result = { blocks: st.blocks.length };

        st.content.focus({ preventScroll: true });

        if (p.selection && typeof p.selection.anchor === 'number') {
            const a = pointFromOffset(p.selection.anchor);
            const f = (p.selection.focus === p.selection.anchor) ? a : pointFromOffset(p.selection.focus);
            const sel = window.getSelection();
            sel.setBaseAndExtent(a.node, a.offset, f.node, f.offset);
            st.appliedSelection = snapshotSelection(sel);
            result.selectionMapped = true;
        }

        if (p.scroll && typeof p.scroll.offset === 'number') {
            st.anchor = { offset: p.scroll.offset, fraction: p.scroll.fraction || 0 };
            st.userAnchor = null;
            st.scrollDirty = false;
            setScrollTop(anchorToY(st.anchor));
            result.scrollTop = st.appliedScrollTop;
        }

        return result;
    }

    function finish(token, result) {
        if (st.bridge) {
            st.bridge.presented(token, JSON.stringify(result));
        }
    }

    // Positions depend on layout, which depends on the viewport width that
    // Qt resizes asynchronously; wait until it matches what Qt reported.
    function whenWidthSettles(expected, cb) {
        if (!expected || Math.abs(window.innerWidth - expected) <= 2) {
            cb(false);
            return;
        }

        let done = false;
        const run = (timedOut) => {
            if (done) {
                return;
            }

            done = true;
            window.removeEventListener('resize', onResize);

            if (timedOut) {
                log('warn', 'viewport width did not settle; positioning anyway', { expected, actual: window.innerWidth });
            }

            cb(timedOut);
        };
        const onResize = () => {
            if (Math.abs(window.innerWidth - expected) <= 2) {
                run(false);
            }
        };

        window.addEventListener('resize', onResize);
        setTimeout(() => run(true), WIDTH_WAIT_MS);
    }

    function present(p) {
        const t0 = performance.now();

        try {
            if (typeof p.html === 'string') {
                setContent(p.html, typeof p.source === 'string' ? p.source : '');
            }

            st.content.style.maxWidth = (p.contentWidth > 0) ? (p.contentWidth + 'px') : '';
        } catch (e) {
            log('error', 'failed to set rendered content', errorFields(e));
            finish(p.token, { ok: false, error: String(e) });
            return;
        }

        whenWidthSettles(p.expectedWidth, (timedOut) => {
            try {
                const result = applyState(p);
                result.ok = true;
                result.widthTimedOut = timedOut;
                result.ms = Math.round(performance.now() - t0);
                finish(p.token, result);
            } catch (e) {
                log('error', 'failed to apply view state', errorFields(e));
                finish(p.token, { ok: false, error: String(e) });
            }
        });
    }

    function capture() {
        const out = {};

        try {
            const sel = window.getSelection();

            if (sel.rangeCount > 0 && st.content.contains(sel.anchorNode) && st.content.contains(sel.focusNode)
                    && !sameSelection(sel, st.appliedSelection)) {
                const anchor = offsetFromPoint(sel.anchorNode, sel.anchorOffset);
                const focus = offsetFromPoint(sel.focusNode, sel.focusOffset);

                if (anchor !== null && focus !== null) {
                    out.selection = { anchor, focus };
                }
            }

            if (st.scrollDirty) {
                out.scroll = yToAnchor(st.scroller.scrollTop);
            }
        } catch (e) {
            log('error', 'failed to capture view state', errorFields(e));
        }

        return JSON.stringify(out);
    }

    function scrollToHeading(n) {
        const headings = st.content.querySelectorAll(':scope > h1, :scope > h2, :scope > h3, :scope > h4, :scope > h5, :scope > h6');

        if (n > 0 && n <= headings.length) {
            headings[n - 1].scrollIntoView();
            st.scrollDirty = true;
            st.userAnchor = yToAnchor(st.scroller.scrollTop);
        } else {
            log('warn', 'heading out of range', { requested: n, available: headings.length });
        }
    }

    // ---------------------------------------------------------------- input

    function blockEdit(e) {
        e.preventDefault();
    }

    function onKeyDown(e) {
        if ((e.ctrlKey || e.metaKey) && e.shiftKey && !e.altKey && (e.code === 'KeyV' || e.key === 'V' || e.key === 'v')) {
            e.preventDefault();
            e.stopPropagation();

            if (st.bridge) {
                st.bridge.requestToggle();
            }
        }
    }

    function onClick(e) {
        const link = e.target.closest ? e.target.closest('a[href]') : null;

        if (!link || !(e.ctrlKey || e.metaKey)) {
            return;
        }

        e.preventDefault();
        const href = link.getAttribute('href') || '';

        if (href.startsWith('#')) {
            const id = decodeURIComponent(href.slice(1));
            const target = document.getElementById(id) || document.getElementsByName(id)[0];

            if (target) {
                target.scrollIntoView();
            }
        } else if (st.bridge) {
            st.bridge.openLink(link.href);
        }
    }

    function onMutation(records) {
        if (st.mutating || records.length === 0) {
            return;
        }

        // Some input (e.g. IME composition) cannot be cancelled; undo it.
        log('warn', 'rendered content was modified by input; restoring', { records: records.length });
        const top = st.scroller.scrollTop;
        setContent(st.html, st.source);
        st.scroller.scrollTop = top;
    }

    function loadStyleSheet(css) {
        let el = document.getElementById('ghostwriter_css');

        if (!el) {
            el = document.createElement('style');
            el.id = 'ghostwriter_css';
            el.type = 'text/css';
            el.media = 'all';
            document.head.appendChild(el);
        }

        el.textContent = css;
    }

    function init() {
        st.content = document.getElementById('moogle-content');
        st.scroller = document.getElementById('livepreviewplaceholder');

        st.observer = new MutationObserver(onMutation);
        st.observer.observe(st.content, { childList: true, characterData: true, subtree: true });

        st.content.addEventListener('beforeinput', blockEdit);
        st.content.addEventListener('paste', blockEdit);
        st.content.addEventListener('cut', blockEdit);
        st.content.addEventListener('drop', blockEdit);
        st.content.addEventListener('click', onClick);
        st.content.addEventListener('load', () => {
            if (!st.scrollDirty) {
                reanchor('image load');
            }
        }, true);
        document.addEventListener('keydown', onKeyDown, true);
        st.scroller.addEventListener('scroll', onScroll, { passive: true });
        window.addEventListener('resize', () => reanchor('resize'));

        new QWebChannel(qt.webChannelTransport, (channel) => {
            st.bridge = channel.objects.moogleBridge;

            for (const [level, message, payload] of st.outbox) {
                st.bridge.log(level, message, payload);
            }

            st.outbox = [];
            loadStyleSheet(st.bridge.styleSheet);
            st.bridge.styleSheetChanged.connect((css) => {
                loadStyleSheet(css);
                reanchor('style change');
            });
            st.bridge.pageReady();
        });
    }

    window.addEventListener('error', (e) => {
        log('error', 'uncaught page error', errorFields(e.error || e.message, { source: e.filename, line: e.lineno }));
    });

    window.moogle = { present, capture, scrollToHeading };
    document.addEventListener('DOMContentLoaded', init);
})();
