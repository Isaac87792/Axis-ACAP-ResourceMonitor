/*
 * CompressingGraph: reference implementation.
 * Spec: "Scope of Work - Compressing Graph.md" in this folder (version 2.2).
 *
 * A live time-series graph where "now" is the right edge, the line glides in
 * smoothly (no per-sample jumps), and older history is squeezed toward the left
 * in speed bands (each band slower than the one before), so a long history
 * stays visible in a fixed width. Defaults are measured from the reference clip.
 *
 * The time axis counts measurements (sample intervals), not seconds: the width holds
 * `across` measurements (default 36) and every point moves one slot left per new one.
 * So changing the sample interval while it runs (a "real time" switch) never rescales
 * what is already drawn: older points keep moving left at the new pace, new ones enter
 * at the right edge (version 2.1). A value axis may be logarithmic, each power of ten the same height
 * (axes.left.log, version 2.2): bursty values (network traffic, latency) show both their everyday level and their bursts.
 *
 *   const g = new CompressingGraph(containerEl, { interval: 1, series: [...] });
 *   g.push([12.5, 3.1]);            // or g.push({ util: 12.5, kernel: 3.1 })
 *   g.setOptions({ interval: 0.5 }); // faster from now on; nothing jumps
 *
 * No dependencies. One canvas per graph, one shared requestAnimationFrame loop.
 *
 * Resource Monitor copy: additions to the reference, all opt-in: axes.left.zero + zeroColor (a brighter line at that
 * value); series.band (1.5.0: a series drawn as faint shaded bands behind the lines wherever its value is over 0.5,
 * e.g. the heater's On periods, never as a line; series.legendColor colours its legend name). Fix: the label gutter
 * also fits the middle label. Light and dark (1.8.0): colour options left out are the page's --kit-g-* colour tokens,
 * read again whenever the page's theme (data-theme on <html>) changes, so every graph follows it on its next frame;
 * series.fillColor colours the area under a line (the line's own colour when left out).
 */
(function (global) {
  'use strict';

  // Candidate grid steps, in measurements (one line per measurement first).
  const NICE = [1, 2, 5, 10, 15, 20, 30, 60, 120, 300, 600, 900, 1800, 3600,
    7200, 10800, 21600, 43200, 86400, 172800, 604800];

  // Age -> width breakpoints measured from the reference clip (fractions of window / of width).
  // Speeds per band relative to the newest: 1, 0.44, 0.22, 0.11. Newest 10% of time = 33% of width.
  const CLIP_BANDS = [
    { age: 0.081, width: 0.304 },
    { age: 0.241, width: 0.566 },
    { age: 0.529, width: 0.805 },
  ];

  // A value at the bottom of the range (0, or the axis minimum) is drawn this many px above the frame, so a line at zero
  // stays visible instead of hiding in the bottom border.
  const ZERO_LIFT = 3;

  // Colours not given in the options: the page's tokens, read once and again after each change of theme.
  const TOKENS = {
    background: '--kit-g-bg', plotBackground: '--kit-g-plot', border: '--kit-g-border', grid: '--kit-g-grid',
    textColor: '--kit-g-text', titleColor: '--kit-g-title', cursorColor: '--kit-g-cursor',
    markerColor: '--kit-g-marker', markerEdge: '--kit-g-marker-edge',
    tipBackground: '--kit-g-tip-bg', tipBorder: '--kit-g-tip-border', tipText: '--kit-g-tip-text',
  };
  let themed = null;
  function theme() {
    if (!themed) {
      const cs = getComputedStyle(document.documentElement);
      themed = {};
      for (const k in TOKENS) themed[k] = cs.getPropertyValue(TOKENS[k]).trim();
    }
    return themed;
  }
  new MutationObserver(() => { themed = null; })
    .observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  // "r,g,b" of any CSS colour (#rgb, #rrggbb(aa), rgb(), hsl(), a name...), for the fading area under a line. A canvas
  // reads it: fillStyle keeps its old value when given something it can't read, so the colour is set over two different
  // starting values and taken only when both give the same answer. Not a colour: the reference's neutral grey.
  const FALLBACK_RGB = '214,214,214';
  let probe = null;
  const rgbCache = new Map();
  function rgbOf(c) {
    c = String(c || '').trim();
    if (rgbCache.has(c)) return rgbCache.get(c);
    probe = probe || document.createElement('canvas').getContext('2d');
    probe.fillStyle = 'transparent'; probe.fillStyle = c; const a = probe.fillStyle;
    probe.fillStyle = 'black'; probe.fillStyle = c; const b = probe.fillStyle;
    let out = FALLBACK_RGB;
    if (c && a === b) {
      const h = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})/i.exec(a), m = /^rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)/i.exec(a);
      if (h) out = [h[1], h[2], h[3]].map(x => parseInt(x, 16)).join(',');
      else if (m) out = [m[1], m[2], m[3]].map(v => Math.round(+v)).join(',');
    }
    if (rgbCache.size > 200) rgbCache.clear();
    rgbCache.set(c, out);
    return out;
  }

  const DEFAULTS = {
    across: 36,           // measurements across the full width (clip: ~32 samples in 36 s at ~1.1 s each)
    window: null,         // or seconds across the width at the starting interval: across = window / interval
    profile: 'bands',     // 'bands' (as in the clip) | 'log' (smooth curve) | 'linear'
    bands: CLIP_BANDS,    // used by 'bands'; the last band always ends at { age: 1, width: 1 }
    blend: 0.15,          // band corners are softened over +/- this share of the corner's age
    compression: 17,      // used by 'log': window / tau (17 = newest 10% of time takes ~34% of width)
    interval: 1,          // expected seconds between samples (also the display delay); may change while running
    clock: () => Date.now() / 1000, // seconds; must match the timestamps given to push()
    series: [],           // [{ name, key?, color, fillColor?, axis: 'left'|'right', width? }]
    axes: {
      left:  { min: 0, max: 100, unit: '%', color: null, labels: true },   // color null: textColor
      right: null,        // same shape, or null; log: true makes it logarithmic (min > 0; version 2.2)
    },
    title: '', titleInside: false,
    // Colours: null takes the page's token (TOKENS above), so the graph follows the page's light or dark theme
    titleColor: null, background: null, plotBackground: null, border: null, grid: null, textColor: null,
    cursorColor: null, markerColor: null, markerEdge: null, tipBackground: null, tipBorder: null, tipText: null,
    legend: true,
    footerLeft: '', footerRight: '',   // string or () => string
    hGridPx: 18,          // horizontal grid pitch in px (clip: 18 px); hDivisions overrides it
    hDivisions: null,
    gridStep: null,       // seconds between vertical grid lines at the starting interval; null = auto (clip: one per sample)
    gridMinPx: 30,        // auto step: the newest vertical grid line spacing is at least this
    gridMinGap: 3,        // thin older grid lines (fade out) once they get closer than this
    markers: true,        // "current value" triangles on the right edge
    hover: true,          // crosshair + readout under the pointer
    lineWidth: 1.6,
    aggregate: 'extreme', // how squeezed samples merge: 'extreme' (keeps spikes) | 'avg' | 'max'
    gapFactor: 3,         // break the line after this many missed measurements
    font: '11px "Segoe UI", Calibri, sans-serif',
    valueFormat: null,    // (value, series) => string for the hover readout
  };

  // ---- maths ------------------------------------------------------------

  // Fritsch-Carlson monotone cubic tangents: smooth, never overshoots the data.
  function tangents(T, V) {
    const n = T.length, m = new Array(n);
    if (n < 2) { m.fill(0); return m; }
    const d = new Array(n - 1);
    for (let i = 0; i < n - 1; i++) d[i] = (V[i + 1] - V[i]) / (T[i + 1] - T[i]);
    m[0] = d[0]; m[n - 1] = d[n - 2];
    for (let i = 1; i < n - 1; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (let i = 0; i < n - 1; i++) {
      if (d[i] === 0) { m[i] = 0; m[i + 1] = 0; continue; }
      const a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b;
      if (h > 9) { const s = 3 / Math.sqrt(h); m[i] = s * a * d[i]; m[i + 1] = s * b * d[i]; }
    }
    return m;
  }

  function hermite(t0, t1, v0, v1, m0, m1, t) {
    const h = t1 - t0, s = (t - t0) / h, s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * h * m0 +
      (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * h * m1;
  }

  function formatAge(a) {
    a = Math.max(0, Math.round(a));
    if (a < 60) return a + ' s ago';
    if (a < 3600) return Math.floor(a / 60) + ' m ' + String(a % 60).padStart(2, '0') + ' s ago';
    if (a < 86400) return Math.floor(a / 3600) + ' h ' + String(Math.floor(a / 60) % 60).padStart(2, '0') + ' m ago';
    return Math.floor(a / 86400) + ' d ' + String(Math.floor(a / 3600) % 24).padStart(2, '0') + ' h ago';
  }

  // ---- graph --------------------------------------------------------------

  class CompressingGraph {
    constructor(container, opts = {}) {
      this.o = Object.assign({}, DEFAULTS, opts);
      this.o.axes = Object.assign({}, DEFAULTS.axes, opts.axes || {});
      // The axis counts measurements. `window` (seconds) is a way of saying how many at the starting interval.
      this.N = opts.across != null ? opts.across : opts.window != null ? opts.window / this.o.interval : DEFAULTS.across;
      this.o.window = this.N * this.o.interval;
      // The measurement clock G: one unit per expected interval. It runs at 1 / interval units a second and
      // changes pace, without jumping, when the interval changes (one segment per pace).
      this._segs = [{ t: 0, g: 0, iv: this.o.interval }];
      this.samples = [];  // { t, t0, t1, g, g0, g1, n, v: [per series value or null] }
      this._pushes = 0;
      this.container = container;
      this.canvas = document.createElement('canvas');
      this.canvas.style.cssText = 'display:block;width:100%;height:100%';
      container.appendChild(this.canvas);
      this.ctx = this.canvas.getContext('2d');
      this.hoverX = null;
      this._visible = true;
      this._ro = new ResizeObserver(() => this._resize());
      this._ro.observe(container);
      this._io = new IntersectionObserver(e => { this._visible = e[0].isIntersecting; });
      this._io.observe(container);
      if (this.o.hover) {
        this.canvas.addEventListener('pointermove', e => {
          const r = this.canvas.getBoundingClientRect();
          this.hoverX = e.clientX - r.left;
        });
        this.canvas.addEventListener('pointerleave', () => { this.hoverX = null; });
      }
      this._buildMap();
      this._resize();
      CompressingGraph._all.add(this);
      CompressingGraph._start();
    }

    // values: array in series order, or object keyed by series key/name. t in clock seconds.
    push(values, t) {
      if (t == null) t = this.o.clock();
      const last = this.samples[this.samples.length - 1];
      if (last && t <= last.t) return;  // ignore out-of-order samples
      const v = Array.isArray(values) ? values
        : this.o.series.map(s => values[s.key != null ? s.key : s.name]);
      const g = this._G(t);
      this.samples.push({ t, t0: t, t1: t, g, g0: g, g1: g, n: 1,
        v: v.map(x => (x == null || Number.isNaN(+x)) ? null : +x) });
      if (++this._pushes % 16 === 0) this._compact();
    }

    clear() { this.samples = []; }
    setOptions(p) {
      // A new interval changes the pace from now on: what is drawn keeps its place and moves on at the new pace.
      if (p.interval != null && p.interval !== this.o.interval) {
        const now = this.o.clock();
        this._segs.push({ t: now, g: this._G(now), iv: p.interval });
      }
      if (p.across != null) this.N = p.across;
      else if (p.window != null) this.N = p.window / (p.interval != null ? p.interval : this.o.interval);
      Object.assign(this.o, p);
      this.o.window = this.N * this.o.interval;
      this._buildMap(); this._compact();
    }
    destroy() {
      this._ro.disconnect(); this._io.disconnect();
      CompressingGraph._all.delete(this); this.canvas.remove();
    }

    // Seconds <-> measurement clock.
    _G(t) {
      const s = this._segs;
      let k = s.length - 1;
      while (k > 0 && s[k].t > t) k--;
      return s[k].g + (t - s[k].t) / s[k].iv;
    }
    _T(g) {
      const s = this._segs;
      let k = s.length - 1;
      while (k > 0 && s[k].g > g) k--;
      return s[k].t + (g - s[k].g) * s[k].iv;
    }
    // Display position on the measurement clock: one measurement behind now, so the tip is interpolated.
    _gd() { return this._G(this.o.clock()) - 1; }

    // Measurement age <-> x (CSS px). u = 0 at the right edge, 1 at the left edge.
    // u(age / across) is tabulated once per profile change; lookups interpolate the table.
    _buildMap() {
      const o = this.o, N = 1024, U = new Float64Array(N + 1);
      if (o.profile === 'log' && o.compression > 0) {
        const lg = Math.log1p(o.compression);
        for (let i = 0; i <= N; i++) U[i] = Math.log1p(o.compression * i / N) / lg;
      } else if (o.profile === 'bands') {
        // Constant speed inside each band; each corner ramps linearly between the two speeds
        // (same area as the sharp corner, so the breakpoints stay exact). Then integrate.
        const pts = [{ age: 0, width: 0 }, ...o.bands, { age: 1, width: 1 }];
        const sp = [];
        for (let k = 0; k < pts.length - 1; k++) sp.push((pts[k + 1].width - pts[k].width) / (pts[k + 1].age - pts[k].age));
        const speed = f => {
          let k = 0;
          while (k < sp.length - 1 && f >= pts[k + 1].age) k++;
          let s = sp[k];
          for (let j = 1; j < pts.length - 1; j++) {
            const c = pts[j].age, r = c * o.blend;
            if (r > 0 && f > c - r && f < c + r) s = sp[j - 1] + (sp[j] - sp[j - 1]) * (f - c + r) / (2 * r);
          }
          return s;
        };
        for (let i = 1; i <= N; i++) U[i] = U[i - 1] + speed((i - 0.5) / N) / N;
        for (let i = 1; i <= N; i++) U[i] /= U[N];
      } else {
        for (let i = 0; i <= N; i++) U[i] = i / N;
      }
      this._U = U; this._M = N;
    }
    _uOf(units) {
      const U = this._U, M = this._M, f = Math.max(0, units) / this.N;
      if (f >= 1) return 1 + (f - 1) * (U[M] - U[M - 1]) * M;  // beyond the left edge: keep the last speed
      const i = f * M, j = Math.floor(i);
      return U[j] + (U[j + 1] - U[j]) * (i - j);
    }
    _xOf(units) { return this.R - this._uOf(units) * (this.R - this.L); }
    _unitsOfX(x) {
      const U = this._U, M = this._M, u = Math.min(1, Math.max(0, (this.R - x) / (this.R - this.L)));
      let lo = 0, hi = M;
      while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (U[mid] <= u) lo = mid; else hi = mid; }
      return (lo + (U[hi] > U[lo] ? (u - U[lo]) / (U[hi] - U[lo]) : 0)) / M * this.N;
    }
    // In seconds at the current interval (as before version 2.1, and exact while the interval is steady).
    uOfAge(a) { return this._uOf(a / this.o.interval); }
    xOfAge(a) { return this._xOf(a / this.o.interval); }
    ageOfX(x) { const gd = this._gd(); return this._T(gd) - this._T(gd - this._unitsOfX(x)); }

    // Merge samples that have been squeezed closer than ~0.75 px, drop what fell off the left.
    // Keeps memory bounded by width, not by window length or sample rate.
    _compact() {
      const s = this.samples;
      if (s.length < 3) return;
      const gd = this._gd();
      let cut = 0;
      while (cut < s.length - 2 && gd - s[cut + 1].g > this.N) cut++;
      if (cut) s.splice(0, cut);
      while (this._segs.length > 1 && this._segs[1].t <= s[0].t0) this._segs.shift();  // paces no sample needs any more
      const refW = Math.max(this.R - this.L, 600);
      const minDu = 0.75 / refW, gap = this.o.gapFactor;
      const agg = this.o.aggregate;
      const out = [];
      let cur = null;
      for (const p of s) {
        if (!cur) { cur = p; continue; }
        const near = this._uOf(gd - cur.g) - this._uOf(gd - p.g) < minDu;
        if (near && p.g0 - cur.g1 <= gap && gd - p.g > 4) {
          const n = cur.n + p.n, ref = out[out.length - 1];
          cur = {
            t: (cur.t * cur.n + p.t * p.n) / n, t0: cur.t0, t1: p.t1,
            g: (cur.g * cur.n + p.g * p.n) / n, g0: cur.g0, g1: p.g1, n,
            v: cur.v.map((a, i) => {
              const b = p.v[i];
              if (a == null) return b;
              if (b == null) return a;
              if (agg === 'max') return Math.max(a, b);
              if (agg === 'avg') return (a * cur.n + b * p.n) / n;
              // 'extreme': keep whichever departs more from the point before, so spikes and dips survive.
              const r = ref && ref.v[i] != null ? ref.v[i] : (a + b) / 2;
              return Math.abs(b - r) > Math.abs(a - r) ? b : a;
            }),
          };
        } else { out.push(cur); cur = p; }
      }
      out.push(cur);
      this.samples = out;
    }

    _resize() {
      const r = this.container.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      this.w = Math.max(10, r.width); this.h = Math.max(10, r.height);
      this.canvas.width = Math.round(this.w * dpr);
      this.canvas.height = Math.round(this.h * dpr);
      this.dpr = dpr;
      this._layout();
    }

    _layout() {
      const o = this.o, ctx = this.ctx;
      ctx.font = o.font;
      const pad = o.inset != null ? o.inset : 3;   // Resource Monitor: 0 lets a bare line run edge to edge (Overview tiles)
      const top = ((o.legend && o.series.length) || (o.title && !o.titleInside)) ? 18 : pad;
      const bottom = (o.footerLeft || o.footerRight) ? 18 : pad;
      const lab = ax => {
        if (!ax || !ax.labels) return pad;
        // Resource Monitor fix: the middle label can be the widest (e.g. "14.5%" between "0%" and "50%")
        const mid = ax.log ? Math.sqrt(ax.min * ax.max) : (ax.min + ax.max) / 2;
        return Math.max(...[ax.max, ax.min, mid].map(v => ctx.measureText(this._axLabel(ax, v)).width)) + 8;
      };
      this.L = lab(o.axes.left); this.R = this.w - lab(o.axes.right);
      this.T = top; this.B = this.h - bottom;
    }

    _axLabel(ax, v) {
      if (ax.format) return ax.format(v);
      return (Math.round(v * 10) / 10) + (ax.unit || '');
    }

    _y(v, axisName) {
      const ax = this.o.axes[axisName || 'left'] || this.o.axes.left;
      if (ax.log) { // each power of ten the same height; a value at or under min (or none above 0) sits on the bottom line
        const lo = Math.log10(ax.min), hi = Math.log10(ax.max);
        return this.B - ZERO_LIFT - ((Math.log10(Math.max(v, ax.min)) - lo) / (hi - lo)) * (this.B - this.T - ZERO_LIFT);
      }
      return this.B - ZERO_LIFT - ((v - ax.min) / (ax.max - ax.min)) * (this.B - this.T - ZERO_LIFT);
    }

    // A colour option, or the page's token for it
    _c(k) { return this.o[k] || theme()[k]; }

    _draw() {
      const o = this.o, ctx = this.ctx;
      const gd = this._gd();  // display position: one measurement behind, so the tip is interpolated
      const { L, R, T, B } = this;
      ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      ctx.font = o.font;
      ctx.fillStyle = this._c('background'); ctx.fillRect(0, 0, this.w, this.h);
      ctx.fillStyle = this._c('plotBackground'); ctx.fillRect(L, T, R - L, B - T);
      this._autoScale();
      this._drawGrid(gd);
      this._drawText();

      // Find the last sample at or before gd.
      const s = this.samples;
      let lo = 0, hi = s.length - 1, k = -1;
      while (lo <= hi) { const mid = (lo + hi) >> 1; if (s[mid].g <= gd) { k = mid; lo = mid + 1; } else hi = mid - 1; }
      let first = 0;
      while (first < k && gd - s[first + 1].g >= this.N) first++;  // keep one point beyond the left edge

      ctx.save();
      ctx.beginPath(); ctx.rect(L, T, R - L, B - T); ctx.clip();
      ctx.lineJoin = 'round'; ctx.lineCap = 'round';
      const tips = [];
      const gap = o.gapFactor;
      const last = Math.min(k + 1, s.length - 1);
      // Band series first: one shaded strip per run of readings whose value is over 0.5. A run reaches up to the
      // next reading, so readings that arrive a little late (1.4-2 s apart) leave no gaps, and one fill per run
      // leaves no seams where translucent strips would overlap.
      o.series.forEach((ser, j) => {
        if (!ser.band) return;
        ctx.fillStyle = ser.color;
        const on = i => { const v = s[i] && s[i].v[j]; return v != null && v > 0.5; };
        const fill = (g0, g1) => {
          g1 = Math.min(g1, gd);
          if (g1 <= g0) return;
          const x0 = this._xOf(gd - g0), x1 = this._xOf(gd - g1);
          ctx.fillRect(Math.min(x0, x1), T, Math.abs(x1 - x0) + 0.5, B - T);
        };
        let start = null;
        for (let i = first; i <= last && k >= 0; i++) {
          if (!on(i)) continue;
          if (start == null) start = s[i].g0 - 0.5;
          const next = i < last ? s[i + 1] : null;
          if (next && on(i + 1) && next.g0 - s[i].g1 <= gap) continue;   /* the run goes on; a gap that also breaks the line ends it */
          fill(start, s[i].g1 + 0.5);
          start = null;
        }
      });
      o.series.forEach((ser, j) => {
        if (ser.band) return;
        const axis = ser.axis || 'left';
        ctx.strokeStyle = ser.color; ctx.lineWidth = ser.width || o.lineWidth;
        // Split into runs at nulls and gaps.
        let run = [];
        const runs = [];
        for (let i = first; i <= last && k >= 0; i++) {
          const p = s[i], v = p.v[j];
          const prev = run[run.length - 1];
          if (v == null || (prev && p.g0 - prev.g1 > gap)) { if (run.length) runs.push(run); run = []; }
          if (v != null) run.push(p);
        }
        if (run.length) runs.push(run);
        let tip = null;
        for (const r of runs) {
          const Gm = r.map(p => p.g), V = r.map(p => p.v[j]);
          const m = tangents(Gm, V);
          ctx.beginPath();
          const fx0 = this._xOf(gd - Gm[0]); let fxl = fx0;
          ctx.moveTo(fx0, this._y(V[0], axis));
          for (let i = 0; i < r.length - 1; i++) {
            if (Gm[i] >= gd) break;
            const gEnd = Math.min(Gm[i + 1], gd);
            const dx = this._xOf(gd - gEnd) - this._xOf(gd - Gm[i]);
            const n = Math.min(32, Math.max(1, Math.ceil(dx / 3)));
            for (let q = 1; q <= n; q++) {
              const g = Gm[i] + (gEnd - Gm[i]) * q / n;
              const v = (n === 1 && gEnd === Gm[i + 1]) ? V[i + 1]
                : hermite(Gm[i], Gm[i + 1], V[i], V[i + 1], m[i], m[i + 1], g);
              fxl = this._xOf(gd - g); ctx.lineTo(fxl, this._y(v, axis));
              if (g === gd) tip = v;
            }
          }
          // Data late: hold the last value flat to the edge for a short while, then stop.
          const lp = r[r.length - 1];
          if (lp.g <= gd && gd - lp.g1 <= gap) { ctx.lineTo(R, this._y(V[V.length - 1], axis)); fxl = R; tip = V[V.length - 1]; }
          ctx.stroke();
          /* Filled look: the area under the line fades from the line's colour to nothing at the bottom */
          if (!ser.noFill) { ctx.save(); ctx.lineTo(fxl, B); ctx.lineTo(fx0, B); ctx.closePath();
            const top = Math.min(...V.map(v => this._y(v, axis))), many = o.series.filter(x => !x.band).length > 2;
            const rgb = rgbOf(ser.fillColor || ser.color);
            const gr = ctx.createLinearGradient(0, top, 0, B); gr.addColorStop(0, 'rgba(' + rgb + ',' + (many ? .22 : .42) + ')'); gr.addColorStop(1, 'rgba(' + rgb + ',0.02)');
            ctx.fillStyle = gr; ctx.fill(); ctx.restore(); }
        }
        if (tip != null) tips.push({ y: this._y(tip, axis), v: tip, ser });
      });
      ctx.restore();

      if (o.markers) for (const tp of tips) {
        const y = Math.min(B, Math.max(T, tp.y));
        ctx.beginPath();
        ctx.moveTo(R - 7, y); ctx.lineTo(R, y - 4.5); ctx.lineTo(R, y + 4.5); ctx.closePath();
        ctx.fillStyle = this._c('markerColor'); ctx.fill();
        ctx.strokeStyle = this._c('markerEdge'); ctx.lineWidth = 0.8; ctx.stroke();
      }

      ctx.strokeStyle = this._c('border'); ctx.lineWidth = 1;
      ctx.strokeRect(Math.round(L) + 0.5, Math.round(T) + 0.5, Math.round(R - L) - 1, Math.round(B - T) - 1);

      if (this.hoverX != null && this.hoverX >= L && this.hoverX <= R) this._drawHover(gd);
    }

    // Axes with auto: true glide their max toward a nice ceiling of the largest value in view.
    _autoScale() {
      const o = this.o;
      for (const side of ['left', 'right']) {
        const ax = o.axes[side];
        if (!ax || !ax.auto || ax.log) continue; // a log axis keeps its range
        let mx = ax.floorMax || 1;
        o.series.forEach((ser, j) => {
          if ((ser.axis || 'left') !== side) return;
          for (const p of this.samples) if (p.v[j] != null && p.v[j] > mx) mx = p.v[j];
        });
        const e = Math.pow(10, Math.floor(Math.log10(mx * 1.1)));
        const target = [1, 2, 2.5, 5, 10].map(f => f * e).find(c => c >= mx * 1.1);
        ax.max = ax.max == null ? target : ax.max + (target - ax.max) * 0.08;
        if (Math.abs(ax.max - target) < target * 1e-3) ax.max = target;
      }
    }

    _drawGrid(gd) {
      const o = this.o, ctx = this.ctx, { L, R, T, B } = this;
      const grid = this._c('grid');
      ctx.strokeStyle = grid; ctx.lineWidth = 1;
      ctx.beginPath();
      const la = o.axes.left; // a log axis: one line each power of ten, unless hDivisions says otherwise
      const div = o.hDivisions || (la && la.log ? Math.max(1, Math.round(Math.log10(la.max / la.min))) : Math.max(2, Math.round((B - T) / o.hGridPx)));
      for (let i = 1; i < div; i++) {
        const y = Math.round(T + (B - T) * i / div) + 0.5;
        ctx.moveTo(L, y); ctx.lineTo(R, y);
      }
      ctx.stroke();
      // Vertical lines are pinned to the measurement clock (multiples of step), so they travel left with
      // the data and crowd together as they age; a change of interval only changes their pace. Where
      // they get closer than gridMinGap every other line fades out, then every 4th, and so on.
      const PW = R - L;
      // Auto: one line per measurement (as in the clip) unless that is tighter than gridMinPx at the newest end.
      const step = o.gridStep ? o.gridStep / o.interval
        : NICE.find(st => PW * this._uOf(st) >= o.gridMinPx) || NICE[NICE.length - 1];
      for (let n = Math.floor(gd / step); ; n--) {
        const age = gd - n * step;
        if (age > this.N) break;
        const sp = PW * (this._uOf(age + step) - this._uOf(age));  // local spacing
        let mult = 1;
        while (sp * mult < o.gridMinGap && mult < 1 << 20) mult *= 2;
        if (n % mult) continue;
        const alpha = n % (2 * mult) ? Math.min(1, Math.max(0, (sp * mult - o.gridMinGap) / o.gridMinGap)) : 1;
        if (alpha <= 0) continue;
        const xr = Math.round(this._xOf(age)) + 0.5;
        ctx.globalAlpha = alpha;
        ctx.beginPath(); ctx.moveTo(xr, T); ctx.lineTo(xr, B); ctx.stroke();
      }
      ctx.globalAlpha = 1;
      // Resource Monitor addition: a brighter line at the left axis's `zero` value (axes.left.zero, zeroColor),
      // so a line that sits at 0 is drawn on a line of its own, above the frame.
      if (la && la.zero != null && la.zeroColor) {
        const yz = Math.round(this._y(la.zero)) + 0.5;
        ctx.strokeStyle = la.zeroColor;
        ctx.beginPath(); ctx.moveTo(L, yz); ctx.lineTo(R, yz); ctx.stroke();
        ctx.strokeStyle = grid;
      }
    }

    _drawText() {
      const o = this.o, ctx = this.ctx, { L, R, T, B } = this;
      ctx.textBaseline = 'middle';
      // Legend / title above the plot.
      let x = L;
      if (o.title && !o.titleInside) {
        ctx.fillStyle = this._c('titleColor'); ctx.textAlign = 'left';
        ctx.fillText(o.title, x, T / 2); x += ctx.measureText(o.title).width + 14;
      }
      if (o.legend) for (const ser of o.series) {
        ctx.fillStyle = ser.legendColor || ser.color; ctx.textAlign = 'left';
        ctx.fillText(ser.name, x, T / 2); x += ctx.measureText(ser.name).width + 14;
      }
      if (o.title && o.titleInside) {
        ctx.fillStyle = this._c('titleColor'); ctx.textAlign = 'left';
        ctx.fillText(o.title, L + 5, T + 9);
      }
      // Axis labels: top, middle, bottom.
      for (const side of ['left', 'right']) {
        const ax = o.axes[side];
        if (!ax || !ax.labels) continue;
        ctx.fillStyle = ax.color || this._c('textColor');
        ctx.textAlign = side === 'left' ? 'right' : 'left';
        const lx = side === 'left' ? L - 4 : R + 4;
        const mid = ax.log ? Math.sqrt(ax.min * ax.max) : (ax.min + ax.max) / 2;
        ctx.fillText(this._axLabel(ax, ax.max), lx, T + 5);
        ctx.fillText(this._axLabel(ax, mid), lx, (T + B) / 2);
        ctx.fillText(this._axLabel(ax, ax.min), lx, B - 5);
      }
      // Footer.
      const val = f => typeof f === 'function' ? f() : f;
      ctx.fillStyle = this._c('textColor');
      if (o.footerLeft) { ctx.textAlign = 'left'; ctx.fillText(val(o.footerLeft), L, (B + this.h) / 2 + 1); }
      if (o.footerRight) { ctx.textAlign = 'right'; ctx.fillText(val(o.footerRight), R, (B + this.h) / 2 + 1); }
    }

    // Value of series j at a point of the measurement clock.
    _valueAt(g, j) {
      const s = this.samples;
      let lo = 0, hi = s.length - 1, k = -1;
      while (lo <= hi) { const mid = (lo + hi) >> 1; if (s[mid].g <= g) { k = mid; lo = mid + 1; } else hi = mid - 1; }
      if (k < 0) return null;
      const a = s[k], b = s[k + 1];
      if (!b || a.v[j] == null || b.v[j] == null) return Math.abs(g - a.g1) <= this.o.gapFactor ? a.v[j] : null;
      if (b.g0 - a.g1 > this.o.gapFactor) return null;
      return a.v[j] + (b.v[j] - a.v[j]) * (g - a.g) / (b.g - a.g);
    }

    _drawHover(gd) {
      const o = this.o, ctx = this.ctx, { L, R, T, B } = this, x = this.hoverX;
      const units = this._unitsOfX(x), g = gd - units;
      const age = this._T(gd) - this._T(g);  // in seconds, across any change of interval
      ctx.strokeStyle = this._c('cursorColor'); ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(Math.round(x) + 0.5, T); ctx.lineTo(Math.round(x) + 0.5, B); ctx.stroke();
      const lines = [{ text: formatAge(age), color: this._c('tipText') }];
      o.series.forEach((ser, j) => {
        const v = this._valueAt(g, j);
        if (v == null) return;
        const ax = o.axes[ser.axis || 'left'] || o.axes.left;
        const txt = o.valueFormat ? o.valueFormat(v, ser) : (Math.round(v * 10) / 10) + (ax.unit || '');
        lines.push({ text: ser.name + '  ' + txt, color: ser.color });
        ctx.fillStyle = ser.color;
        ctx.beginPath(); ctx.arc(x, this._y(v, ser.axis || 'left'), 2.5, 0, 7); ctx.fill();
      });
      ctx.font = o.font;
      const bw = Math.max(...lines.map(l => ctx.measureText(l.text).width)) + 12, bh = lines.length * 14 + 6;
      let bx = x + 8; if (bx + bw > R) bx = x - 8 - bw; bx = Math.max(L, bx);
      const by = T + 4;
      ctx.fillStyle = this._c('tipBackground'); ctx.fillRect(bx, by, bw, bh);
      ctx.strokeStyle = this._c('tipBorder'); ctx.strokeRect(bx + 0.5, by + 0.5, bw - 1, bh - 1);
      ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      lines.forEach((l, i) => { ctx.fillStyle = l.color; ctx.fillText(l.text, bx + 6, by + 10 + i * 14); });
    }

    static _start() {
      if (CompressingGraph._running) return;
      CompressingGraph._running = true;
      const tick = () => {
        for (const g of CompressingGraph._all) if (g._visible) g._draw();
        requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
    }
  }
  CompressingGraph._all = new Set();
  CompressingGraph.formatAge = formatAge;

  global.CompressingGraph = CompressingGraph;
})(window);
