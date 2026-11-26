/*
 * Resource Monitor settings page.
 * Live graphs use compressing-graph.js (copied unchanged from the Compressing Graph project).
 * Endpoints (this app, admin only): live, history, processes, apps, control, log, app.
 */
(function () {
'use strict';
const $ = (s, r) => (r || document).querySelector(s);
const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const num = v => (v == null || isNaN(v)) ? NaN : +v;
/* 1.6.5: no hover popups anywhere in the tool: every
 * title attribute is taken off as soon as it appears, so nothing shows a tooltip. Explanations are in the (i) buttons. */
(function noTitles() {
	const strip = n => { if (n.nodeType !== 1) return; if (n.hasAttribute('title')) n.removeAttribute('title'); n.querySelectorAll('[title]').forEach(e => e.removeAttribute('title')); };
	const go = () => {
		strip(document.documentElement);
		new MutationObserver(ms => ms.forEach(m => { if (m.type === 'attributes') { if (m.target.hasAttribute('title')) m.target.removeAttribute('title'); } else m.addedNodes.forEach(strip); }))
			.observe(document.documentElement, { subtree: true, childList: true, attributes: true, attributeFilter: ['title'] });
	};
	if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', go); else go();
})();
/* Colours: every one is a token in css/acap-theme.css or app.css (dark and light). They are read here, and again
 * whenever js/theme.js changes the page's theme; then every graph is drawn again in the new colours (retheme()). */
let BRAND, BRAND_FILL, BRAND_TEXT, EDGE, TOTAL, RX, TX, TEMP, BUSY, CAM_LINE, AXIS_TEXT, CURSOR, GRID, GRID2, ZERO, HEAT_BAND, TEMP_FRAME;
const C = {};
let PAL = [], DRV = [], PAIRS = [];
function readColours() {
	const cs = getComputedStyle(document.documentElement), t = n => cs.getPropertyValue(n).trim();
	BRAND = t('--kit-c-apps-line');          /* the apps as a line (yellow; darker gold on light) */
	BRAND_FILL = t('--kit-c-apps');          /* the apps as a fill: always the Axis yellow */
	BRAND_TEXT = t('--kit-c-apps-text');     /* the apps as words and axis labels */
	EDGE = t('--brand-edge');
	TOTAL = t('--kit-c-total'); RX = t('--kit-c-blue'); TX = t('--kit-c-amber'); TEMP = t('--kit-c-orange');
	BUSY = t('--rm-c-busy'); CAM_LINE = t('--kit-c-camera-line'); AXIS_TEXT = t('--kit-g-text'); CURSOR = t('--rm-cursor');
	GRID = t('--kit-g-grid'); GRID2 = t('--rm-g-grid-2'); ZERO = t('--kit-g-zero'); HEAT_BAND = t('--kit-g-band');
	TEMP_FRAME = { border: t('--rm-g-temp-border'), plotBackground: t('--rm-g-temp-plot') };
	Object.assign(C, { blue: RX, amber: TX, teal: t('--kit-c-teal'), violet: t('--kit-c-violet'), warn: t('--kit-c-warn'), danger: t('--kit-c-red'), grey: t('--kit-c-grey') });
	PAL = [BRAND, C.blue, C.teal, C.violet, C.amber, C.warn, C.grey, C.danger];       /* one colour per sensor */
	DRV = [TOTAL, C.blue, C.teal, C.violet, C.amber, C.warn, C.grey, C.danger];       /* one colour per drive */
	PAIRS = [[C.blue, C.amber], [C.teal, C.violet], [TOTAL, C.warn], [C.grey, C.danger]];   /* read and write, per device */
}
readColours();
/* A line's area fades from this colour: the apps' area is the Axis yellow under their own line colour */
const fillOf = c => c === BRAND ? BRAND_FILL : c;

/*---------------------------------------------------- formatting */
function fmtKb(kb) {
	kb = num(kb);
	if (isNaN(kb)) return '–';
	if (kb >= 1024 * 1024 * 1024) return (kb / 1073741824).toFixed(1) + ' TB';
	if (kb >= 1024 * 1024) return (kb / 1048576).toFixed(kb >= 10 * 1048576 ? 0 : 1) + ' GB';
	if (kb >= 1024) return (kb / 1024).toFixed(kb >= 100 * 1024 ? 0 : 1) + ' MB';
	return Math.round(kb) + ' kB';
}
function fmtMbps(v) {
	v = num(v);
	if (isNaN(v)) return '–';
	if (v >= 100) return Math.round(v) + ' Mbit/s';
	if (v >= 1) return v.toFixed(1) + ' Mbit/s';
	return Math.round(v * 1000) + ' kbit/s';
}
function fmtRate(kbs) {                       /* kB/s, read and write */
	kbs = num(kbs);
	if (isNaN(kbs)) return '–';
	if (kbs >= 1048576) return (kbs / 1048576).toFixed(1) + ' GB/s';
	if (kbs >= 1024) return (kbs / 1024).toFixed(kbs >= 10240 ? 0 : 1) + ' MB/s';
	if (kbs >= 1) return kbs.toFixed(kbs >= 10 ? 0 : 1) + ' kB/s';
	return Math.round(kbs * 1024) + ' B/s';
}
function fmtCount(v) { v = num(v); return isNaN(v) ? '–' : v >= 10000 ? Math.round(v / 1000) + 'k' : v >= 100 ? String(Math.round(v)) : String(Math.round(v * 10) / 10); }
function fmtPct(v, d) { v = num(v); return isNaN(v) ? '–' : v.toFixed(d == null ? (v < 10 && v > 0 ? 1 : 0) : d) + ' %'; }
function fmtW(v) { v = num(v); return isNaN(v) ? '–' : v.toFixed(v >= 100 ? 0 : 1) + ' W'; }
/* 1.6.1: temperatures are read and kept in °C; Settings › Show temperatures in picks what the page shows */
function tempF() { return !!(S.now && S.now.settings && S.now.settings.tempUnit === 'F'); }
function toT(c) { return c == null || isNaN(c) ? c : tempF() ? c * 9 / 5 + 32 : c; }
function tUnit() { return tempF() ? '°F' : '°C'; }
function fmtC(v) { v = num(v); return isNaN(v) ? '–' : toT(v).toFixed(1) + ' ' + tUnit(); }
function fmtLink(m) { return m >= 1000 ? m / 1000 + ' Gbit/s' : m + ' Mbit/s'; }
function fmtAge(s) {
	s = Math.max(0, Math.round(num(s) || 0));
	if (s < 60) return s + ' s';
	if (s < 3600) return Math.floor(s / 60) + ' min';
	if (s < 86400) return Math.floor(s / 3600) + ' h ' + Math.floor(s % 3600 / 60) + ' min';
	return Math.floor(s / 86400) + ' d ' + Math.floor(s % 86400 / 3600) + ' h';
}
function hhmm(t, sec) {
	const d = new Date(t * 1000), p = n => (n < 10 ? '0' : '') + n;
	return p(d.getHours()) + ':' + p(d.getMinutes()) + (sec ? ':' + p(d.getSeconds()) : '');
}

/*---------------------------------------------------- talking to the camera */
async function getJSON(url) {
	const r = await fetch(url, { cache: 'no-store', credentials: 'same-origin' });
	if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
	return r.json();
}
async function postJSON(url, body) {
	const r = await fetch(url, { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
	const text = await r.text();
	let j = null;
	try { j = JSON.parse(text); } catch (e) { /* plain text error */ }
	if (!r.ok) throw new Error((j && j.message) || text || r.statusText);
	return j;
}

/*---------------------------------------------------- state */
const S = { seq: 0, last: null, recent: [], now: {}, topCpu: [], topMem: [], apps: {}, appList: [], device: {}, procs: null, log: [] };
let sel = 'cpu', range = 'live', tab = 'perf';
let cpuParent = 'all';                                                        /* CPU: Overall or Each core; the view below applies to both */
const view = { cpu: 'use', mem: 'use', net: 'traffic', flash: 'space' };    /* the view picked on each page */
let netIf = 0;                                                                /* the interface picked on Ethernet */
let skew = 0;                               /* camera clock minus this computer's clock, in seconds */
const clock = () => Date.now() / 1000 + skew;
let G = null;
const KEYS = ['cpu', 'mem', 'flash', 'net', 'temp', 'power', 'dlpu'];
const VIEWS = {
	cpu: [['use', 'Usage'], ['work', 'Kind of work'], ['load', 'Load']],
	mem: [['use', 'In use'], ['breakdown', 'Breakdown']],
	net: [['traffic', 'Traffic'], ['packets', 'Packets'], ['errors', 'Errors and drops']],
	flash: [['space', 'Space'], ['activity', 'Activity']]
};
const RES = {
	cpu:  { name: 'CPU', top: 'Using the most CPU' },
	mem:  { name: 'Memory', top: 'Using the most memory' },
	flash:{ name: 'Storage', top: 'Apps using the most storage' },
	net:  { name: 'Network', top: 'Using the most network' },
	temp: { name: 'Temperature', top: 'Using the most CPU (heat follows the CPU)' },
	power:{ name: 'Power', top: 'Using the most CPU' },
	dlpu: { name: 'AI chip', top: 'Using the most CPU' }
};
const at = (p, k, i) => { const a = p && p[k]; return a && a[i] != null ? a[i] : null; };
const at2 = (p, k, i, j) => { const a = p && p[k] && p[k][i]; return a && a[j] != null ? a[j] : null; };
function peak(pts, gets) { let m = 0; pts.forEach(p => gets.forEach(g => { const v = g(p); if (v != null && v > m) m = v; })); return m; }
function niceMax(v) { for (const s of [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000]) if (s >= v) return s; return v; }

/* Every drive with its space: the mounted ones, and the SD card from the camera's disk list when the app can't see its folder */
function drives() {
	const n = S.now, out = [];
	(n.mounts || []).forEach((m, i) => { if (m.present) out.push({ name: m.name, kind: m.kind, totalKb: m.totalKb, freeKb: m.freeKb, disk: m.disk, mi: i }); });
	if (n.sdTotalKb && !out.some(d => d.kind === 'sd')) out.push({ name: 'SD card', kind: 'sd', totalKb: n.sdTotalKb, freeKb: n.sdFreeKb, disk: -1, sd: true });
	if (!out.length && n.flashTotalKb) out.push({ name: 'Flash (apps)', kind: 'flash', totalKb: n.flashTotalKb, freeKb: n.flashFreeKb, disk: -1, flash: true });
	return out;
}
const usedPct = d => d.totalKb > 0 ? 100 * (1 - d.freeKb / d.totalKb) : NaN;
function fullest() { return drives().filter(d => !isNaN(usedPct(d))).sort((a, b) => usedPct(b) - usedPct(a))[0]; }
function hottest() {
	const s = S.last || {}, sens = S.now.sensors || [];
	let best = null;
	sens.forEach((x, i) => { const v = at(s, 'temps', i); if (v != null && (!best || v > best.v)) best = { name: x.name, v }; });
	return best || (s.temp != null ? { name: '', v: s.temp } : null);
}

/* What a graph shows: its lines (each reads a live reading or a history point the same way), its axis and number format.
   pts: the readings on screen, for the axes that follow the data. */
/* The apps' own scale on the right */
const appsAxis = (pts, gets, unit, floor) => ({ min: 0, max: niceMax(Math.max(floor, peak(pts, gets)) * 1.2), unit, floorMax: floor });
function spec(k, pts) {
	const n = S.now, v = view[k], pct = { min: 0, max: 100, unit: '%' };
	pts = pts || [];
	const L = (name, color, get, o) => Object.assign({ name, color, get }, o || {});
	switch (k) {
		case 'cpu':
			if (v === 'work') return { stacked: true, axis: pct, fmt: x => fmtPct(x, 1), note: 'Live: each line is that kind\'s own share of the CPU', series: [
				L('Apps and services (user)', C.blue, p => at(p, 'work', 0), { stack: 1 }), L('System (kernel)', C.amber, p => at(p, 'work', 1), { stack: 1 }),
				L('Waiting on disk', C.violet, p => at(p, 'work', 2), { stack: 1 }), L('Interrupts', C.teal, p => at(p, 'work', 3), { stack: 1 }),
				L('Apps', BRAND, p => p.apps)] };
			if (v === 'load') {
				const s = [L('1 min', BRAND, p => at(p, 'load', 0)), L('5 min', C.blue, p => at(p, 'load', 1)), L('15 min', C.teal, p => at(p, 'load', 2))];
				return { axis: { min: 0, max: niceMax(Math.max(n.cores || 1, peak(pts, s.map(x => x.get))) * 1.1), unit: '' }, fmt: x => x.toFixed(2), series: s,
					note: 'Processes running or waiting, on average; ' + (n.cores || '?') + ' cores' };
			}
			return { axis: pct, fmt: x => fmtPct(x, 0), axis2: appsAxis(pts, [p => p.apps], '%', 1),
				series: [L('Apps', BRAND, p => p.apps, { right: true, back: true, fmt: x => fmtPct(x, 1) }), L('Camera, all', TOTAL, p => p.cpu)] };
		case 'mem':
			if (v === 'breakdown') return { axis: pct, fmt: x => fmtPct(x, 0), series: [L('In use', TOTAL, p => p.mem), L('Cache and buffers', C.blue, p => p.memCache),
				L('Available', C.teal, p => p.mem == null ? null : 100 - p.mem)].concat(n.videoTotalKb ? [L('Video memory', C.violet, p => p.memVideo)] : []) };
			return { axis: pct, fmt: x => fmtPct(x, 0), series: [L('In use', TOTAL, p => p.mem)] };
		case 'net': {
			const i = netIf, nv = (p, j) => { const x = at2(p, 'nets', i, j); return x != null ? x : i === 0 && j < 2 ? (j ? p.tx : p.rx) : null; };
			if (v === 'packets') return { axis: { min: 1, max: 100000, log: true, unit: '', format: x => fmtCount(x) + '/s' }, fmt: x => fmtCount(x) + ' /s',
				note: 'Each step up is 10 times more packets', series: [L('Sent', TX, p => nv(p, 3)), L('Received', RX, p => nv(p, 2))] };
			if (v === 'errors') {
				const s = [L('Received errors', C.danger, p => nv(p, 4)), L('Received drops', C.warn, p => nv(p, 5)), L('Sent errors', C.violet, p => nv(p, 6)), L('Sent drops', C.teal, p => nv(p, 7))];
				return { count: true, axis: { min: 0, max: niceMax(Math.max(5, peak(pts, s.map(x => x.get)) * 1.15)), unit: '' }, fmt: x => String(Math.round(x)), series: s,
					note: 'A count in each reading, not a running total' };
			}
			return { axis: { min: 0.01, max: 1000, log: true, unit: ' Mbit/s' }, fmt: fmtMbps, note: 'Each step up is 10 times more traffic', series: [L('Sent', TX, p => nv(p, 1)), L('Received', RX, p => nv(p, 0))] };
		}
		case 'flash':
			if (v === 'activity') {
				const s = [];
				(n.disks || []).forEach((d, i) => { if (!d.present) return; const c = PAIRS[i % PAIRS.length]; s.push(L(d.name + ' read', c[0], p => at2(p, 'disks', i, 0)), L(d.name + ' write', c[1], p => at2(p, 'disks', i, 1))); });
				return { axis: { min: 1, max: 1048576, log: true, unit: '', format: fmtRate }, fmt: fmtRate, note: 'Each step up is 10 times more', series: s };
			}
			return { axis: pct, fmt: x => fmtPct(x, 0), series: drives().map((d, j) => L(d.name, DRV[j % DRV.length], d.sd ? p => p.sd : d.flash ? p => p.flash : p => at(p, 'mounts', d.mi))) };
		case 'temp': {
			const sens = n.sensors || [];
			const s = sens.length ? sens.map((x, i) => L(x.name, sens.length === 1 ? TEMP : PAL[i % PAL.length], p => toT(at(p, 'temps', i)))) : [L('Temperature', TEMP, p => toT(p.temp))];
			let lo = Math.floor(toT(20) / 10) * 10, hi = Math.ceil(toT(90) / 10) * 10;
			pts.forEach(p => s.forEach(x => { const t = x.get(p); if (t != null) { if (t < lo) lo = Math.floor(t / 10) * 10; if (t > hi) hi = Math.ceil(t / 10) * 10; } }));
			return { axis: { min: lo, max: hi, unit: ' ' + tUnit() }, fmt: v => num(v).toFixed(1) + ' ' + tUnit(), series: s, sig: tUnit() };
		}
		case 'power': {
			const s = [L(n.power && n.power.kind === 'ports' ? 'Given out on the ports' : 'Drawing', BRAND, p => p.power)];
			const ax = { min: 0, max: niceMax(Math.max(5, peak(pts, [s[0].get]) * 1.3)), unit: ' W' };
			/* 1.5.0: the heater's On periods as faint bands behind the line, so a rise lines up with the heater */
			if (n.heater) s.unshift(L('Heater on', HEAT_BAND, p => p.heat, { band: true, legendColor: BRAND_TEXT, tipName: 'Heater', fmt: v => v > 0.5 ? 'On' : 'Off' }));
			return { axis: ax, fmt: fmtW, series: s };
		}
		case 'dlpu': {
			const s = [L('Jobs per second', C.teal, p => p.infer)];
			return { axis: { min: 0, max: niceMax(Math.max(10, peak(pts, [s[0].get]) * 1.2)), unit: '/s' }, fmt: x => fmtCount(x) + ' /s', series: s };
		}
	}
}
/* Each core: a reading's busy share and apps for core i. Live readings carry them as cores[] and
   coresApps[]; history points as coresWork[i] = [user, system, waiting on disk, interrupts, apps]. */
function coreBusy(p, i) {
	if (p.cores) return at(p, 'cores', i);
	const w = p.coresWork && p.coresWork[i];
	return w && w[0] != null ? w[0] + (w[1] || 0) + (w[3] || 0) : null;
}
function coreApps(p, i) { return p.coresApps ? at(p, 'coresApps', i) : at2(p, 'coresWork', i, 4); }
const KINDS = ['Apps and services', 'System', 'Waiting on disk', 'Interrupts'];
/* The graph of one core in the Each core tiles, for the view picked (Usage, Kind of work, Load) */
function coreSpec(i) {
	const L = (name, color, get, o) => Object.assign({ name, color, get }, o || {}), ax = { min: 0, max: 102, unit: '%' };   /* 102: a full core runs just inside the top edge */
	if (view.cpu === 'work') return { stacked: true, axis: ax, fmt: x => fmtPct(x, 1), series: [
		L('Apps and services (user)', C.blue, p => at2(p, 'coresWork', i, 0), { stack: 1 }), L('System (kernel)', C.amber, p => at2(p, 'coresWork', i, 1), { stack: 1 }),
		L('Waiting on disk', C.violet, p => at2(p, 'coresWork', i, 2), { stack: 1 }), L('Interrupts', C.teal, p => at2(p, 'coresWork', i, 3), { stack: 1 }),
		L('Apps', BRAND, p => coreApps(p, i))] };
	if (view.cpu === 'load') return { axis: ax, fmt: x => fmtPct(x, 0), series: [L('Core busy', BUSY, p => coreBusy(p, i))] };
	return { axis: ax, fmt: x => fmtPct(x, 0), series: [L('Apps', BRAND, p => coreApps(p, i), { back: true }), L('Camera, all', TOTAL, p => coreBusy(p, i))] };
}
/* A core tile's label: "Core 1 · 64 % · apps 2 %" (and "Mostly system" for Kind of work), or "Core 1 · busy 64 %" for Load */
function coreLabel(i) {
	const s = S.last || {}, b = coreBusy(s, i), a = coreApps(s, i);
	if (view.cpu === 'load') return '<b>Core ' + (i + 1) + '</b> · busy ' + fmtPct(b, 0);
	let h = '<b>Core ' + (i + 1) + '</b> · ' + fmtPct(b, 0) + (a != null ? ' · apps ' + fmtPct(a, 0) : '');
	if (view.cpu === 'work') {
		const w = (s.coresWork || [])[i] || [];
		let k = -1;
		w.forEach((v, j) => { if (v != null && j < 4 && (k < 0 || v > w[k])) k = j; });
		h += '<br><span class="m">' + (k < 0 ? '–' : b != null && b < 1 ? 'Mostly idle' : 'Mostly ' + KINDS[k].toLowerCase()) + '</span>';
	}
	return h;
}
function subText() {
	const n = S.now;
	switch (sel) {
		case 'cpu':
			if (cpuParent === 'cores' && view.cpu === 'load') return 'Camera load ' + (n.load1 != null ? n.load1.toFixed(2) + ' (1 min) · ' + n.load5.toFixed(2) + ' (5 min) · ' + n.load15.toFixed(2) + ' (15 min)' : '–');
			if (cpuParent === 'cores') return (view.cpu === 'work' ? 'Kind of work on each of the ' : 'Each of the ') + (n.cores || '?') + ' cores' + (n.mhz ? ' · ' + Math.round(n.mhz) + ' MHz now' : '');
			if (view.cpu === 'load') return 'Load average · ' + (n.cores || '?') + ' cores, so ' + (n.cores || 1) + '.0 means every core busy';
			return 'Share of all ' + (n.cores || '?') + ' cores in use' + (n.mhz ? ' · ' + Math.round(n.mhz) + ' MHz now' : '');
		case 'mem': return fmtKb(n.memTotalKb) + ' in all. Cache the camera can free at once counts as available.';
		case 'flash': { const d = drives(), f = fullest(); return d.length + (d.length === 1 ? ' drive' : ' drives') + (f ? ' · fullest: ' + f.name + ' ' + fmtPct(usedPct(f), 0) : ''); }
		case 'net': { const i = (n.nets || [])[netIf]; if (!i) return (n.netIf || 'Ethernet') + (n.linkMbps ? ', link ' + fmtLink(n.linkMbps) : '');
			return i.name + (i.linkMbps ? ', link ' + fmtLink(i.linkMbps) : '') + (i.state ? ', ' + i.state : ''); }
		case 'temp': { const s = (n.sensors || []).length, h = hottest(); return s > 1 ? s + ' sensors' + (h ? ' · hottest: ' + h.name + ' ' + fmtC(h.v) : '') : s === 1 ? 'One sensor: ' + n.sensors[0].name : 'The highest of the camera\'s temperature sensors'; }
		case 'power': { const p = n.power || {}; return p.kind === 'ports' ? 'PoE ports · giving out ' + fmtW((S.last || {}).power) + (p.budgetW ? ' of ' + fmtW(p.budgetW) + ' budget' : '')
			: (p.class ? 'PoE class ' + p.class + ' · ' : '') + 'drawing ' + fmtW((S.last || {}).power) + (p.budgetW ? ' of ' + fmtW(p.budgetW) + ' budget' : ''); }
		case 'dlpu': return 'Deep learning unit' + (n.dlpu && /^(ARTPEC|Ambarella)$/.test(n.dlpu.kind) ? ' (' + n.dlpu.kind + ')' : '') + ' · AI chip jobs a second';
	}
}
/* 1.6.1: every installed app, Axis-made (Object Analytics, ...) or not, is "Apps" (yellow); the camera itself is grey */
const isAxis = id => !!(id && S.apps[id] && S.apps[id].axis);
const ownApp = id => !!id;                         /* 1.6.1: every installed app, Axis-made or not, is an app (yellow) */
function appName(id) { return !id ? 'Camera' : (S.apps[id] ? S.apps[id].name : id); }
function selfApp() { return S.appList.find(a => a.self) || {}; }
function appsDiskKb() { return S.appList.reduce((a, x) => a + (x.diskKb || 0), 0); }

/*---------------------------------------------------- live graphs */
/* The graphs' frame colours come from the --kit-g-* tokens (compressing-graph.js reads them itself) */
const THEME = { font: '11px "Segoe UI", sans-serif', interval: 1, across: 60, clock: clock };
function cg(el, o) { return new CompressingGraph(el, Object.assign({}, THEME, o)); }
const LC = {};                                  /* each live graph's current set-up: { sig, sp } */
function makeGraphs(cores) {
	const el = k => $('.cg[data-k="' + k + '"]');
	G = { cores: [] };
	KEYS.forEach(k => {
		G[k] = cg(el(k), Object.assign({ hover: false, series: [], axes: { left: { min: 0, max: 100, unit: '%', color: AXIS_TEXT, labels: true }, right: null }, footerRight: k === 'cpu' ? () => 'Load average ' + (S.now.load1 != null ? S.now.load1.toFixed(2) : '–') : '' },
			k === 'temp' ? Object.assign({}, TEMP_FRAME) : {}));
		liveHover(G[k], el(k), k, s => valsOf(LC[k] ? LC[k].sp : spec(k), s));
	});
	/* Each core: a label above each small graph (two lines for Kind of work), the key above them all, the Load note under them */
	$('#cores').insertAdjacentHTML('beforeend', '<div class="cores-grid" id="cores-grid"></div><p class="cores-live-note" id="cores-live-note" hidden>Live: each line is that kind\'s own share of the core.</p><div class="cores-note" id="cores-note" hidden>Linux keeps one load average for the whole camera, not one per core, so each core shows how busy it is and the camera\'s load is written above.</div>');
	for (let i = 0; i < cores; i++) {
		const d = document.createElement('div');
		d.className = 'ct';
		d.innerHTML = '<div class="lbl"></div><div class="cg-core"></div>';
		$('#cores-grid').appendChild(d);
		const box = $('.cg-core', d), g = cg(box, { titleInside: false, legend: false, gridMinPx: 18, lineWidth: 1.4, hDivisions: 4, hover: false, series: [], axes: { left: { min: 0, max: 102, unit: '%', labels: false }, right: null } });
		G.cores.push(g);
		liveHover(g, box, 'cpu', s => valsOf(coreSpec(i), s));
	}
}
/* 1.5.0: CPU and Memory have their legend under the graph, the same under Overall and Each core, so switching only
   swaps the graphs. Kind of work and Memory: one item a line; Usage and Load: on one line. */
const belowLegend = k => k === 'cpu' || k === 'mem';
const legendCls = k => 'legend g-legend' + (k === 'mem' || (k === 'cpu' && view.cpu === 'work') ? ' g-col' : '');
function legendHtml(series) {
	return series.slice().sort((a, b) => (a.back ? 1 : 0) - (b.back ? 1 : 0)).map(x => '<span class="key"' + (x.color === BRAND && sel === 'cpu' && cpuParent === 'cores' ? ' title="Each thread of these apps is counted on the core it last ran on, so this is close, not exact."' : '') + '>' +
		(x.band ? '<i class="k-band"></i>' : x.color === BRAND ? '<i class="k-app"></i>' : '<i style="background:' + x.color + '"></i>') + esc(x.name) + '</span>').join('');
}
function drawLiveLegend() {
	const el = $('#live-legend');
	if (!el) return;
	el.hidden = !belowLegend(sel) || !$('#no-data').hidden;
	if (el.hidden) return;
	const sp = sel === 'cpu' && cpuParent === 'cores' ? coreSpec(0) : spec(sel, S.window || []);
	const cls = legendCls(sel), html = legendHtml(sp.series);
	if (el.className !== cls) el.className = cls;
	if (el.innerHTML !== html) el.innerHTML = html;
}
/* The values a live graph draws for one reading. Live draws each kind of work at its own value (lines, not stacked);
   log axes keep to their floor. */
/* Every graph that starts at 0 runs its value axis a little below 0 (and to 102 % at the top of a % axis), with a
   brighter 0 line, so a line at 0 sits on a line of its own above the frame. A log axis keeps its floor (where 0 is
   drawn) a little above the bottom edge, on that line. */
/* ZERO: the brighter 0 line; HEAT_BAND (1.5.0): the heater's On periods behind the power line (both set in readColours) */
function plotAxis(ax) {
	const fmt = v => ax.format ? ax.format(v) : (Math.round(v * 10) / 10) + (ax.unit || '');
	if (ax.log) return Object.assign({}, ax, { min: ax.min / 1.6, zero: ax.min, zeroColor: ZERO, format: v => fmt(Math.max(v, ax.min)) });
	if (ax.min !== 0) return Object.assign({}, ax);
	const pct = ax.unit === '%' && ax.max >= 100 && ax.max <= 102, top = pct ? 100 : ax.max, pad = top * 0.02;
	return Object.assign({}, ax, { min: -pad, max: top + (pct ? pad : 0), zero: 0, zeroColor: ZERO, format: v => fmt(clamp(v, 0, top)) });
}
function liveVals(sp, s) {
	return sp.series.map(x => {
		const v = x.get(s);
		if (v == null || isNaN(v)) return null;
		return sp.axis.log ? Math.max(sp.axis.min, +v) : +v;
	});
}
/* Set a live graph up again when its view, interface, lines or axis changed, and draw the readings kept so far.
   Returns true when it did (the new readings are then drawn already). */
function syncLive(k) {
	if (!G || !G[k]) return false;
	const sp = spec(k, S.window || []), ax = sp.axis;
	const sig = JSON.stringify([view[k] || '', k === 'net' ? netIf : 0, sp.series.map(x => x.name + x.color), ax.min, ax.max, ax.unit || '', !!ax.log, !!sp.stacked, sp.axis2 ? sp.axis2.unit : '']);
	if (LC[k] && LC[k].sig === sig) { LC[k].sp = sp; return false; }
	LC[k] = { sig, sp };
	const g = G[k];
	g.clear();
	g.setOptions({ series: sp.series.map(x => ({ name: x.name, color: x.color, fillColor: fillOf(x.color), band: !!x.band, legendColor: x.legendColor || (x.color === BRAND ? BRAND_TEXT : undefined), axis: x.right ? 'right' : 'left', fmt: x.fmt })), legend: !belowLegend(k), axes: { left: Object.assign({ color: AXIS_TEXT, labels: true }, plotAxis(ax)),
		right: sp.axis2 ? { min: 0, max: sp.axis2.floorMax, auto: true, floorMax: sp.axis2.floorMax, unit: sp.axis2.unit, color: BRAND_TEXT, labels: true } : null },
		valueFormat: (v, ser) => (ser && ser.fmt ? ser.fmt : sp.fmt)(v), footerLeft: sp.note || (k === 'cpu' ? (S.now.cores || '?') + ' cores' : k === 'mem' ? () => fmtKb(S.now.memTotalKb) + ' in all' : '') });
	(S.window || []).forEach(s => g.push(liveVals(sp, s), s.t));
	return true;
}
/* The core tiles follow the CPU view too; true when they were set up again (and drawn) */
let coresSig = '';
function syncCores() {
	if (!G || !G.cores.length || coresSig === view.cpu) return false;
	coresSig = view.cpu;
	G.cores.forEach((g, i) => {
		const sp = coreSpec(i);
		g.clear();
		g.setOptions({ series: sp.series.map(x => ({ name: x.name, color: x.color, fillColor: fillOf(x.color) })), valueFormat: v => sp.fmt(v), axes: { left: Object.assign({ labels: false }, plotAxis(sp.axis)), right: null } });
		(S.window || []).forEach(s => g.push(liveVals(sp, s), s.t));
	});
	drawLiveLegend();
	$('#cores-note').hidden = view.cpu !== 'load';
	$('#cores-live-note').hidden = view.cpu !== 'work';
	return true;
}
function drawCoreLabels() { $$('#cores-grid .ct .lbl').forEach((l, i) => { l.innerHTML = coreLabel(i); }); }
/* Live graphs: point at a moment to see the top 10 then; click to pin it (the pin moves left with the graph).
   k picks the top-10 list (CPU, memory or network); valsFn gives the graph's own values at that reading. */
/* Graph tooltips sit in the window, not in the graph's panel: beside the pointed moment with a gap (right, or left when
   there is no room), never over the point or its line; kept inside the window (shifted up, or capped in height with only
   its list scrolling), so nothing on the page grows or scrolls. owner: one tooltip per graph, made again on redraw. */
function makeTip(owner) {
	$$('.ftip[data-owner="' + owner + '"]').forEach(t => t.remove());
	const tip = document.createElement('div');
	tip.className = 'tip htip ftip';
	tip.dataset.owner = owner;
	tip.hidden = true;
	document.body.appendChild(tip);
	return tip;
}
function placeTip(tip, lineX, y) {
	const gap = 18, m = 8, W = window.innerWidth, H = window.innerHeight;
	tip.dataset.x = lineX; tip.dataset.y = y;
	tip.style.maxHeight = (H - 2 * m) + 'px';
	const tw = tip.offsetWidth, th = tip.offsetHeight;
	let x = lineX + gap;
	if (x + tw > W - m) x = lineX - gap - tw;              /* no room on the right: the left side */
	if (x < m) x = Math.min(W - m - tw, lineX + gap);       /* no room either side: the wider side, clamped */
	let top = y - 24;
	if (top + th > H - m) top = H - m - th;
	if (top < m) top = m;
	tip.style.left = Math.max(m, x) + 'px';
	tip.style.top = top + 'px';
}
/* A pinned tooltip stays where it is; on resize it is kept inside the window */
window.addEventListener('resize', () => { $$('.ftip:not([hidden])').forEach(t => placeTip(t, +t.dataset.x, +t.dataset.y)); });

/* onPoint(reading): the moment pointed at, or null; kindFn: which top-10 list (for a graph whose list changes) */
function liveHover(g, el, k, valsFn, onPoint, kindFn) {
	let lastP = null, lastY = 0;
	const hl = document.createElement('div'), tip = makeTip('live-' + (k || 'g') + '-' + (liveHover.n = (liveHover.n || 0) + 1));
	hl.className = 'cg-hl'; hl.hidden = true;
	el.appendChild(hl);
	let pinT = null, hoverX = null, running = false;
	const tdNow = () => clock() - THEME.interval;              /* the graph runs one reading behind */
	const nearest = (arr, t) => arr.reduce((b, x) => !b || Math.abs(x.t - t) < Math.abs(b.t - t) ? x : b, null);
	const off = () => { hl.hidden = true; tip.hidden = true; running = false; if (onPoint && lastP) { lastP = null; onPoint(null); } };
	function render() {
		if (!el.offsetParent) { pinT = null; hoverX = null; off(); return; }   /* its page was left */
		const t = pinT !== null ? pinT : hoverX !== null ? tdNow() - g.ageOfX(hoverX) : null;
		if (t === null) { off(); return; }
		const x = g.xOfAge(tdNow() - t);
		if (x < g.L) { pinT = null; hoverX = null; off(); return; }
		hl.hidden = false; hl.style.left = x + 'px'; hl.style.top = g.T + 'px'; hl.style.height = (g.B - g.T) + 'px';
		hl.classList.toggle('pinned', pinT !== null);
		const s = nearest(S.window || [], t), kind = kindFn ? kindFn() : topKind(k, (S.liveTops || []).some(p => p.netTop));
		const tops = (S.liveTops || []).filter(p => p.t <= t + 1);
		const p = tops.length ? tops[tops.length - 1] : nearest(S.liveTops || [], t);
		if (onPoint && p !== lastP) { lastP = p; onPoint(p); }
		tip.innerHTML = topTipHtml({ t, seconds: true, ago: tdNow() - t, pinned: pinT !== null, vals: s ? valsFn(s) : [], title: kind.title,
			top: p ? p[kind.list] : [], other: p ? p[kind.other] : 0, usualOf: usual(S.liveTops || [], kind.list), isMem: kind.isMem, isNet: kind.isNet, isIo: kind.isIo, none: kind.none, fmtv: kind.fmtv });
		tip.hidden = false;
		placeTip(tip, g.canvas.getBoundingClientRect().left + x, lastY);
		requestAnimationFrame(render);
	}
	const start = () => { if (!running) { running = true; requestAnimationFrame(render); } };
	const xOf = e => e.clientX - g.canvas.getBoundingClientRect().left;
	el.addEventListener('pointermove', e => { const x = xOf(e); if (pinT === null) lastY = e.clientY; hoverX = x >= g.L && x <= g.R ? x : null; start(); });
	el.addEventListener('pointerleave', () => { hoverX = null; });
	el.addEventListener('click', e => {
		const x = xOf(e);
		if (pinT !== null) pinT = null;
		else if (x >= g.L && x <= g.R) { pinT = tdNow() - g.ageOfX(x); lastY = e.clientY; }
		start();
	});
}
function valsOf(sp, p) {
	const v = sp.series.map(x => { const v = x.get(p); return [x.tipName || x.name, x.legendColor || x.color, v == null || isNaN(v) ? '–' : (x.fmt || sp.fmt)(v)]; });
	return v;
}
function clearGraphs() { Object.values(OVG).forEach(o => { if (o.g) o.g.clear(); }); if (!G) return; KEYS.forEach(k => { G[k].clear(); delete LC[k]; }); G.cores.forEach(g => g.clear()); coresSig = ''; }

/* The camera's theme changed (js/theme.js set data-theme on <html>): read the colours again and draw every graph in them */
function retheme() {
	readColours();
	if (G) {
		G.temp.setOptions(Object.assign({}, TEMP_FRAME));
		KEYS.forEach(k => { delete LC[k]; syncLive(k); });
		coresSig = ''; syncCores();
	}
	if (tab === 'overview') drawOverview();
	if (tab === 'perf') { drawExtra(); if (range === 'live') drawLiveLegend(); else if (hist) drawHist(true); }   /* pins kept */
}
new MutationObserver(retheme).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });

/* Readings older than 5 s: the app is restarting, being updated, or the camera is very busy */
function checkStale() {
	const age = S.last ? clock() - S.last.t : 0, stale = !!S.last && age > 5;
	const st = $('#stale');
	st.hidden = !stale;
	if (stale) st.textContent = 'No new readings since ' + hhmm(S.last.t, 1) + ' (' + fmtAge(age) + ' ago). Resource Monitor may be restarting or the camera is very busy; the graph goes on when readings come back. The numbers shown are the last ones received.';
	setLive(S.fetchOk && !stale, stale ? 'No new readings for ' + fmtAge(age) : S.fetchError);
}
function setLive(ok, why) {
	$$('.live').forEach(l => { l.classList.toggle('off', !ok); l.textContent = ok ? 'Live' : S.fetchOk ? 'No new readings' : 'Not connected'; l.title = ok ? '' : (why || ''); });
}

async function pollLive() {
	try {
		const d = await getJSON('live?since=' + S.seq + (S.seq ? '' : '&max=75'));
		if (d.seq < S.seq) { S.seq = 0; clearGraphs(); S.window = []; S.liveTops = []; }   /* the app restarted */
		S.now = d.now || {};
		if (netIf >= (S.now.nets || []).length) netIf = 0;      /* the list can be shorter after a restart */
		if (!G) makeGraphs(S.now.cores || 1);
		const smp = d.samples || [];
		if (smp.length) {
			const lastT = smp[smp.length - 1].t, want = lastT - Date.now() / 1000 + 0.4;
			if (!S.seq || Math.abs(want - skew) > 3) skew = want;     /* follow the camera's clock */
			S.window = (S.window || []).concat(smp).slice(-150);
			KEYS.forEach(k => { if (!syncLive(k)) smp.forEach(s => G[k].push(liveVals(LC[k].sp, s), s.t)); });
			if (!syncCores()) smp.forEach(s => G.cores.forEach((g, i) => g.push(liveVals(coreSpec(i), s), s.t)));
			feedSparks(smp);
			S.last = smp[smp.length - 1];
			/* Each core tile says its current share above its box: "Core 1 · 54 % · apps 2 %" */
			drawCoreLabels();
			S.recent = S.recent.concat(smp).slice(-60);
			S.liveTops = (S.liveTops || []).concat(smp.filter(x => x.top).map(x => Object.assign({ t: x.t }, x.top))).filter(x => x.t > lastT - 120);
		}
		S.seq = d.seq;
		$('#f-chip').textContent = (S.chip || '') + (S.chip && S.now.cores ? ', ' : '') + (S.now.cores ? S.now.cores + ' cores' : ''); S.topCpu = d.topCpu || []; S.topMem = d.topMem || []; S.topNet = d.topNet || [];
		S.fetchOk = true; S.fetchError = '';
		checkStale();
		drawBar();
		$('#f-up').textContent = fmtAge(S.now.uptime);
		if (tab === 'perf') { longRange(); drawPowerNote(); drawSegs(); $('#c-sub').textContent = subText(); drawStats(); drawExtra(); if (range === 'live') { showNoData(); drawLiveLegend(); } }
		if (tab === 'overview') drawOverview();
		if (tab === 'drives') drawDrives();
		if (tab === 'conns') drawConns();
	} catch (e) {
		S.fetchOk = false; S.fetchError = String(e.message || e);
		checkStale();
	}
	setTimeout(pollLive, 1000);
}

/*---------------------------------------------------- the sidebar: name and live value per item (cam/app shares kept for the tooltips' wording) */
function drawBar() {
	const s = S.last || {}, n = S.now, me = selfApp();
	const set = (k, cam, app, html, title, na) => {
		const b = $('.rb-item[data-k="' + k + '"]');
		$('.rb-t', b).innerHTML = html;
		b.classList.toggle('na', !!na);
		b.title = title;
	};
	set('cpu', s.cpu, s.meCpu, fmtPct(s.cpu, 0) + ' · <b>' + fmtPct(s.meCpu, 1) + '</b>', 'Camera ' + fmtPct(s.cpu, 0) + ' of all cores. Resource Monitor ' + fmtPct(s.meCpu, 1) + '.');
	set('mem', s.mem, 100 * num(s.meMemKb) / num(n.memTotalKb), fmtPct(s.mem, 0) + ' · <b>' + fmtKb(s.meMemKb) + '</b>', 'Camera ' + fmtKb(n.memTotalKb - n.memAvailKb) + ' of ' + fmtKb(n.memTotalKb) + '. Resource Monitor ' + fmtKb(s.meMemKb) + '.');
	/* Storage: the fullest drive */
	const all = drives(), f = fullest();
	set('flash', f ? usedPct(f) : s.flash, 100 * num(me.diskKb) / num(n.flashTotalKb), f ? fmtPct(usedPct(f), 0) + ' · ' + esc(f.name) : fmtPct(s.flash, 0),
		all.map(d => d.name + ': ' + fmtKb(d.totalKb - d.freeKb) + ' of ' + fmtKb(d.totalKb) + ' used.').join(' ') + ' Resource Monitor ' + fmtKb(me.diskKb) + ' (its history is ' + histWhere() + ').');
	set('net', 100 * (num(s.tx) + num(s.rx)) / (n.linkMbps || 1000), 0, fmtMbps(num(s.tx) + num(s.rx)), 'Sent ' + fmtMbps(s.tx) + ', received ' + fmtMbps(s.rx) + (n.linkMbps ? ' on a ' + n.linkMbps + ' Mbit/s link' : '') + (n.netIf ? ' (' + n.netIf + ').' : '.') + ' The camera does not count network use per app.');
	const h = hottest();
	if (h) set('temp', 100 * h.v / 90, 0, fmtC(h.v), 'The hottest of the camera\'s temperature sensors' + (h.name ? ': ' + h.name : '') + '.');
	else set('temp', 0, 0, 'Not reported', 'This camera does not report a temperature to apps.', true);
	/* Power and AI chip: only on cameras that have them */
	const pb = $('.rb-item[data-k="power"]'), db = $('.rb-item[data-k="dlpu"]');
	pb.hidden = (!n.power && sel !== 'power') || !shown(GB.power); db.hidden = (!n.dlpu && sel !== 'dlpu') || !shown(GB.dlpu);
	if (!n.power) set('power', 0, 0, 'Not answering', 'The camera stopped answering about power.', true);
	if (!n.dlpu) set('dlpu', 0, 0, 'Not found', 'The AI chip is not counted any more.', true);
	if (n.power) set('power', 100 * num(s.power) / (n.power.budgetW || 30), 0, fmtW(s.power), n.power.kind === 'ports' ? 'Power the PoE ports give out' + (n.power.budgetW ? ', of a ' + fmtW(n.power.budgetW) + ' budget.' : '.') : 'Power the camera draws' + (n.power.class ? ', PoE class ' + n.power.class : '') + '.');
	if (n.dlpu) { const mx = Math.max(10, ...(S.window || []).map(x => x.infer || 0)); set('dlpu', 100 * num(s.infer) / mx, 0, s.infer != null ? fmtCount(s.infer) + ' /s' : '–', 'AI chip jobs a second.'); }
	/* Overview and Apps: a neutral line each */
	const ob = $('.rb-item[data-k="overview"]'), ab = $('.rb-item[data-k="apps"]');
	$('.rb-t', ob).textContent = 'All resources'; ob.title = 'All resources at a glance.';
	$('.rb-t', ab).textContent = S.appList.length ? S.appList.length + ' apps' : '–'; ab.title = 'Installed apps: start, stop, restart and the log.';
	const sb = $('.rb-item[data-k="settings"]'), sd = (n.settings && n.settings.historyDays) || 1;
	$('.rb-t', sb).textContent = 'History ' + (sd === 1 ? '24 h' : sd + ' days'); sb.title = 'Power source, how long history is kept, and what is read.';
	drawAlertsTile();
	[['flash', GB.flash], ['net', GB.net], ['temp', GB.temp]].forEach(([k, b]) => { $('.rb-item[data-k="' + k + '"]').hidden = !shown(b); });
	$$('.rb-item').forEach(b => b.classList.toggle('on', b.dataset.k === item));
	/* A page that is open stays open when its source stops answering; it says so instead */
}

/*---------------------------------------------------- Performance */
function stat(l, v, cls) { return '<div class="stat' + (cls ? ' ' + cls : '') + '"><div class="s-l">' + esc(l) + '</div><div class="s-v">' + v + '</div></div>'; }
function unitHtml(txt) { const m = /^([\d.,–-]+)\s*(.*)$/.exec(txt); return m ? m[1] + ' <small>' + esc(m[2]) + '</small>' : esc(txt); }
function lastMin(get) { return S.recent.map(get).filter(v => v != null && !isNaN(v)); }
function drawStats() {
	const s = S.last || {}, n = S.now, me = selfApp(), o = [];
	switch (sel) {
		case 'cpu': {
			const w = s.work || [];
			o.push(stat('In use', unitHtml(fmtPct(s.cpu, 0))), stat('Apps', unitHtml(fmtPct(n.appsCpu, 0))),
				stat('Apps and services', unitHtml(fmtPct(w[0], 1))), stat('System', unitHtml(fmtPct(w[1], 1))), stat('Waiting on disk', unitHtml(fmtPct(w[2], 1))));
			if (n.mhz) o.push(stat('Clock', unitHtml(Math.round(n.mhz) + ' MHz')));
			o.push(stat('Load average', (n.load1 != null ? n.load1.toFixed(2) : '–') + ' <small>' + (n.load5 != null ? n.load5.toFixed(2) + ' ' + n.load15.toFixed(2) : '') + '</small>'),
				stat('Resource Monitor', unitHtml(fmtPct(s.meCpu, 1))), stat('Processes', (n.processes || 0) + ' <small>' + (n.threads || 0) + ' threads</small>'));
			break;
		}
		case 'mem':
			o.push(stat('In use', unitHtml(fmtKb(n.memTotalKb - n.memAvailKb))), stat('Available', unitHtml(fmtKb(n.memAvailKb))), stat('Cache', unitHtml(fmtKb(n.memCacheKb))));
			if (n.videoTotalKb) o.push(stat('Video memory', unitHtml(fmtKb(n.videoUsedKb)).replace('</small>', ' of ' + esc(fmtKb(n.videoTotalKb)) + '</small>')));
			o.push(stat('Swap', n.swapTotalKb ? unitHtml(fmtKb(n.swapUsedKb)).replace('</small>', ' of ' + esc(fmtKb(n.swapTotalKb)) + '</small>') : 'None'),
				stat('Apps', unitHtml(fmtKb(n.appsMemKb))), stat('Resource Monitor', unitHtml(fmtKb(s.meMemKb))));
			break;
		case 'flash': {
			const all = drives(), f = fullest();
			o.push(stat('Drives', String(all.length)), stat('Fullest', f ? unitHtml(fmtPct(usedPct(f), 0)).replace('</small>', ' · ' + esc(f.name) + '</small>') : '–'),
				stat('Apps', unitHtml(fmtKb(appsDiskKb()))), stat('Resource Monitor', unitHtml(fmtKb(me.diskKb))));
			break;
		}
		case 'net': {
			const i = (n.nets || [])[netIf], x = (s.nets || [])[netIf] || [];
			const tx = x[1] != null ? x[1] : s.tx, rx = x[0] != null ? x[0] : s.rx;
			o.push(stat('Sent', unitHtml(fmtMbps(tx))), stat('Received', unitHtml(fmtMbps(rx))));
			if (i) {
				o.push(stat('Packets', x[2] != null ? unitHtml(fmtCount(x[2] + (x[3] || 0)) + ' /s') : '–'),
					stat('Drops, last hour', fmtCount(i.dropsHour || 0), i.dropsHour > 0 ? 'warn' : ''), stat('Errors, last hour', fmtCount(i.errorsHour || 0), i.errorsHour > 0 ? 'warn' : ''),
					stat('Link', i.linkMbps ? unitHtml(i.linkMbps + ' Mbit/s') : esc(i.state || '–')));
			} else o.push(stat('Link', n.linkMbps ? unitHtml(n.linkMbps + ' Mbit/s') : '–'), stat('Errors and drops', (n.rxErrors + n.txErrors || 0) + ' <small>since the camera started</small>'));
			break;
		}
		case 'temp': {
			const h = hottest(), t = lastMin(x => x.temp);
			o.push(stat('Hottest now', h ? unitHtml(fmtC(h.v)).replace('</small>', (h.name ? ' · ' + esc(h.name) : '') + '</small>') : 'Not reported'),
				stat('Highest, last min', t.length ? unitHtml(fmtC(Math.max(...t))) : '–'), stat('Lowest, last min', t.length ? unitHtml(fmtC(Math.min(...t))) : '–'));
			break;
		}
		case 'power': {
			const p = n.power || {}, w = lastMin(x => x.power);
			o.push(stat(p.kind === 'ports' ? 'Given out' : 'Drawing', unitHtml(fmtW(s.power))));
			if (p.avgW != null) o.push(stat('Average', unitHtml(fmtW(p.avgW))));
			o.push(stat('Highest', p.maxW != null ? unitHtml(fmtW(p.maxW)) : w.length ? unitHtml(fmtW(Math.max(...w))).replace('</small>', ', last min</small>') : '–'));
			if (p.requestedW != null) o.push(stat('Asked for', unitHtml(fmtW(p.requestedW))));
			if (p.class) o.push(stat('PoE class', esc(p.class)));
			if (p.budgetW) o.push(stat('Budget', unitHtml(fmtW(p.budgetW))));
			break;
		}
		case 'dlpu': {
			const v = lastMin(x => x.infer);
			o.push(stat('Jobs', s.infer != null ? unitHtml(fmtCount(s.infer) + ' /s') : '–'), stat('Highest, last min', v.length ? unitHtml(fmtCount(Math.max(...v)) + ' /s') : '–'),
				stat('Average, last min', v.length ? unitHtml(fmtCount(v.reduce((a, b) => a + b, 0) / v.length) + ' /s') : '–'));
			break;
		}
	}
	$('#stats').innerHTML = o.join('');
	fitTiles();
}
/* Below the tiles: the Drives table, the sensors with fan and heater, the PoE ports */
function drawExtra() {
	const s = S.last || {}, n = S.now;
	let h = '';
	if (sel === 'temp' && (n.sensors || []).length) {
		const sens = '<div><div class="sub-h"><h3>Sensors</h3></div><table class="acap-table rows-t"><colgroup><col><col class="c-n"><col class="c-n"></colgroup><thead><tr><th>Sensor</th><th class="num">Now</th><th class="num">24 h high</th></tr></thead><tbody>' +
			n.sensors.map((x, i) => '<tr><td><span class="sw-dot" style="background:' + (n.sensors.length === 1 ? TEMP : PAL[i % PAL.length]) + '"></span>' + esc(x.name) + '</td><td class="num">' + fmtC(at(s, 'temps', i)) + '</td><td class="num">' + fmtC(x.high24) + '</td></tr>').join('') + '</tbody></table></div>';
		let fh = '';
		if (n.fan || n.heater) {
			/* 1.5.0: the heater stands out while on, with how long; a small graph per row over the range shown */
			const P = range === 'live' ? (S.window || []) : ((hist && hist.points) || []), hz = n.heater || {};
			const rpm = P.map(p => p.fan), mxR = Math.max(100, ...rpm.filter(v => v != null)) * 1.15;
			const heatTxt = hz.on ? (hz.since ? 'On for ' + fmtAge(clock() - hz.since) : 'On')
				: hz.lastOn ? 'Last on until ' + whenTxt(hz.lastOn) + (hz.lastFor ? ', for ' + fmtAge(hz.lastFor) : '') : '';
			fh = '<div><div class="sub-h"><h3>Fan and heater</h3><span class="hint small">' + (range === 'live' ? 'Graphs: the last minutes' : 'Graphs: the last ' + esc(spanName(range))) + '</span></div><table class="acap-table rows-t fh-t"><colgroup><col class="c-fh"><col><col class="c-fh-sp"></colgroup><tbody>' +
				(n.fan ? '<tr><td>Fan</td><td><span class="status ' + (n.fan.running ? 'ok' : 'idle') + '">' + (n.fan.running ? 'Running' : 'Stopped') + '</span>' + (s.fan != null ? ' ' + Math.round(s.fan).toLocaleString('en-US') + ' rpm' : '') + '</td><td class="fh-sp" title="Fan speed over the range shown">' + spark(rpm, mxR, C.teal) + '</td></tr>' : '') +
				(n.heater ? '<tr class="' + (hz.on ? 'heat-on' : '') + '"><td>Heater</td><td>' + (hz.on ? '<span class="heat-badge">On</span>' : '<span class="status idle">Off</span>') + (heatTxt ? ' <span class="heat-for">' + esc(heatTxt) + '</span>' : '') + '</td><td class="fh-sp" title="Heater on (yellow) over the range shown">' + sparkOnOff(P.map(p => p.heat)) + '</td></tr>' : '') + '</tbody></table></div>';
		}
		h = '<div class="two">' + sens + fh + '</div>';
	} else if (sel === 'power' && n.power && (n.power.ports || []).length) {
		h = '<div class="sub-h"><h3>Ports</h3></div><table class="acap-table rows-t stack"><colgroup><col class="c-port"><col><col class="c-n"><col class="c-n"><col class="c-n"></colgroup><thead><tr><th>Port</th><th>State</th><th class="num">Drawing</th><th class="num">Allowed</th><th class="num">Class</th></tr></thead><tbody>' +
			n.power.ports.filter(p => { const pick = (setData && setData.settings.poePorts) || []; return !pick.length || pick.includes(p.port); }).map(p => '<tr><td data-l="Port">' + p.port + '</td><td data-l="State">' + esc(p.state || '–') + '</td><td class="num" data-l="Drawing">' + fmtW(p.w) + '</td><td class="num" data-l="Allowed">' + fmtW(p.limitW) + '</td><td class="num" data-l="Class">' + esc(p.class || '–') + '</td></tr>').join('') + '</tbody></table>';
	} else if (sel === 'dlpu') {
		h = '<p class="hint small extra-note">Counted from the chip\'s work interrupts.</p>';
	}
	$('#extra').innerHTML = h;
}
/* "13:05" today, else "Nov 23 13:05" */
function whenTxt(t) { const d = new Date(t * 1000), today = new Date().toDateString() === d.toDateString(); return (today ? '' : d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' }) + ' ') + hhmm(t); }
/* On and off as a strip: yellow where on (1.5.0, the heater) */
function sparkOnOff(vals) {
	const W = 200, H = 28, n = vals.length, w = n ? W / n : W;
	let r = '';
	vals.forEach((v, i) => { if (v != null && v > 0.5) r += '<rect x="' + (i * w).toFixed(1) + '" y="4" width="' + (w + 0.3).toFixed(1) + '" height="' + (H - 6) + '" fill="' + BRAND_FILL + '" fill-opacity=".75"/>'; });
	return '<svg class="spark" viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none"><line x1="0" x2="' + W + '" y1="' + (H - 2) + '" y2="' + (H - 2) + '" stroke="' + ZERO + '" vector-effect="non-scaling-stroke"/>' + r + '</svg>';
}
function compBar(title, parts, fmt) {
	parts = parts.filter(p => num(p[1]) > 0);
	return '<div class="sub-h"><h3>' + esc(title) + '</h3></div><div class="comp" role="img" aria-label="' + esc(title) + '">' +
		parts.map(p => '<span class="c-' + p[2] + '" style="flex:' + p[1] + '" title="' + esc(p[0] + ': ' + fmt(p[1])) + '"></span>').join('') + '</div>' +
		'<div class="legend">' + parts.map(p => '<span class="key"><i class="k-' + p[2] + '"></i>' + esc(p[0] + ' ' + fmt(p[1])) + '</span>').join('') + '</div>';
}
/* The split and the top lists moved to each resource's Processes page (1.2.0) */
function drawBreakdown() {}
function showNoData() {
	const n = S.now;
	const msg = sel === 'temp' && !hottest() && S.recent.every(x => x.temp == null) ? 'This camera does not report a temperature to apps.'
		: sel === 'flash' && view.flash === 'activity' && !(n.disks || []).some(d => d.present) ? 'This camera does not show its drives\' activity to apps.'
		: sel === 'power' && !n.power ? 'The camera is not answering about power right now. The page goes on when it does.'
		: sel === 'dlpu' && !n.dlpu ? 'The AI chip is not being counted right now.' : '';
	$('#no-data').hidden = !msg;
	$('#no-data').textContent = msg;
	$$('.cg').forEach(d => { d.hidden = !!msg || !(d.dataset.k === sel && !(sel === 'cpu' && cpuParent === 'cores')); });
}
/* The view buttons for this page, and the interface chips on Ethernet (only with more than one) */
let segSig = '';
function drawSegs() {
	const nets = S.now.nets || [], v = VIEWS[sel];
	const sig = sel + '|' + view[sel] + '|' + cpuParent + '|' + netIf + '|' + nets.map(x => x.name).join(',');
	if (sig === segSig) return;
	segSig = sig;
	const vs = $('#view-seg'), is = $('#if-seg'), ps = $('#parent-seg');
	/* CPU: what you look at (Overall or Each core), then how (Usage, Kind of work, Load); each keeps the other */
	ps.hidden = $('#seg-arrow').hidden = sel !== 'cpu';
	ps.innerHTML = sel !== 'cpu' ? '' : [['all', 'Overall'], ['cores', 'Each core']].map(x => '<button type="button" data-p="' + x[0] + '"' + (cpuParent === x[0] ? ' class="on"' : '') + '>' + x[1] + '</button>').join('');
	$$('button', ps).forEach(b => { b.onclick = () => { cpuParent = b.dataset.p; pinned = null; showPerf(); }; });
	vs.hidden = !v;
	vs.innerHTML = v ? v.map(x => '<button type="button" data-v="' + x[0] + '"' + (view[sel] === x[0] ? ' class="on"' : '') + '>' + esc(x[1]) + '</button>').join('') : '';
	$$('button', vs).forEach(b => { b.onclick = () => { view[sel] = b.dataset.v; pinned = null; showPerf(); }; });
	is.hidden = !(sel === 'net' && nets.length > 1);
	is.innerHTML = is.hidden ? '' : nets.map((x, i) => '<button type="button" data-i="' + i + '"' + (i === netIf ? ' class="on"' : '') + ' title="' + esc(x.present ? (x.state || '') : 'Gone') + '">' + esc(x.name) + '</button>').join('');
	$$('button', is).forEach(b => { b.onclick = () => { netIf = +b.dataset.i; pinned = null; showPerf(); }; });
}
let lastPowerHist = null;                                /* the camera's power log as last read, for the source note */
function spanName(s) { return s >= 2592000 ? '30 days' : s >= 604800 ? '7 days' : s >= 86400 ? '24 h' : s >= 3600 ? '1 h' : fmtAge(s); }
function spanAgo(s) { return s >= 2592000 ? '30 days ago' : s >= 604800 ? '7 days ago' : s >= 86400 ? '24 hours ago' : '1 hour ago'; }
/* Which of the 7 d and 30 d buttons show (1.3.0: Settings › Keep history) */
function longRange() {
	/* 7 d and 30 d: on every graph when Settings keeps that much on the SD card; on Power also from the camera's own log */
	const camLog = sel === 'power' && !!(S.now.power && S.now.power.history), days = (S.now.settings && S.now.settings.historyDays) || 1;
	const ok = r => camLog || r <= days * 86400;
	$$('#tab-perf .range button.long').forEach(b => { b.hidden = !ok(+b.dataset.r); });
	if (typeof range === 'number' && range > 86400 && !ok(range)) { range = 86400; $$('.range button').forEach(b => b.classList.toggle('on', b.dataset.r === '86400')); }
}
/* A history step in plain words: "1 min", "15 min", "1 h" */
function fmtStep(s) { return s % 3600 === 0 ? s / 3600 + ' h' : s % 60 === 0 ? s / 60 + ' min' : fmtAge(s); }
function showPerf() {
	$('#c-title').textContent = RES[sel].name;
	$('#c-sub').innerHTML = esc(subText());
	drawSegs();
	const live = range === 'live';
	$('#live-wrap').hidden = !live;
	$('#hist').hidden = live;
	$('#cores').hidden = !(sel === 'cpu' && cpuParent === 'cores');
	if (G) { syncLive(sel); if (sel === 'cpu') { syncCores(); drawCoreLabels(); } }
	showNoData();
	drawLiveLegend();
	longRange();
	if (!live) loadHist();
	drawStats(); drawExtra(); drawBar();
	$('#usage-note').innerHTML = '';   /* 1.5.2: no how-to hints */
	drawPowerNote();
}
/* 1.5.0: where our history is kept, in a few words */
function histWhere() { const k = S.now.settings || {}; return k.onCard ? 'kept on the SD card' : k.onFlash ? 'kept 24 h on the camera\'s built-in flash' : 'kept in memory for 24 h'; }
/* Power: where its history comes from (the camera's own log, or ours) */
function drawPowerNote() {
	if (sel !== 'power' || !S.now.power) return;
		const ph = lastPowerHist, src = S.now.power.history ? 'History from the camera\'s own power log' + (ph && ph.step ? ' (one point every ' + fmtStep(ph.step) + ' for ' + spanName(ph.seconds) + ')' : '') + '.' : 'Recorded by Resource Monitor (' + histWhere() + ').';
		$('#usage-note').innerHTML = '<span class="src-note">' + esc(src) + '</span>';
}

/*---------------------------------------------------- history (1 h, 24 h): plain time axis, top 10 on hover */
let hist = null, histTimer = null, pinned = null;
async function loadHist() {
	clearTimeout(histTimer);
	try {
		/* Power on a camera that keeps its own power log: that log, not a copy of ours (1 h to 30 d) */
		const ph = sel === 'power' && S.now.power && S.now.power.history ? await getJSON('power?seconds=' + range).catch(() => null) : null;
		hist = ph && ph.ok ? { seconds: range, step: ph.step, kept: ph.seconds, points: ph.points, source: 'camera' } : await getJSON('history?seconds=' + range + '&points=120');   /* over 24 h: from the SD card */
		/* 1.5.0: the camera's power log has no heater: take its On periods from our own history (where kept) */
		if (ph && ph.ok && S.now.heater) {
			const our = await getJSON('history?seconds=' + range + '&points=400').catch(() => null);
			const hp = ((our && our.points) || []).filter(q => q.heat != null);
			if (hp.length) hist.points.forEach((p, i) => {
				const t0 = i ? hist.points[i - 1].t : p.t - (hist.step || 60), in1 = hp.filter(q => q.t > t0 && q.t <= p.t + 5);
				p.heat = in1.length ? (in1.some(q => q.heat > 0.5) ? 1 : 0) : null;
			});
		}
		if (ph && ph.ok) { lastPowerHist = ph; drawPowerNote(); }
		if (pinned === null) drawHist();
	} catch (e) {
		$('#hist').innerHTML = '<div class="empty-note">Could not read the history: ' + esc(e.message) + '</div>';
	}
	if (range !== 'live') histTimer = setTimeout(loadHist, 30000);
}
/* What each process usually uses over the shown range, to point out the one behind a spike */
function usual(points, list) {
	const by = {};
	points.forEach(p => (p[list] || []).forEach(e => { (by[e[0]] = by[e[0]] || []).push(e[2]); }));
	const n = points.length, out = {};
	Object.keys(by).forEach(k => { const v = by[k].concat(Array(Math.max(0, n - by[k].length)).fill(0)).sort((a, b) => a - b); out[k] = v[Math.floor(v.length / 2)]; });
	return out;
}
/* The top-10 tooltip, shared by the history graphs and the Live graphs.
   o: { t, ago, pinned, vals: [[name, color, text]], title, top: [[name, owner, value]], other, usualOf, isMem, isNet, fmtv } */
function topTipHtml(o) {
	const top = o.top || [], mx = Math.max(...top.map(e => e[2]), 0.0001);
	let note = '';
	const rows = top.map(e => {
		const u = o.usualOf[e[0]] || 0, high = o.isIo ? false : o.isMem ? (e[2] > u * 1.5 && e[2] - u > 20480) : o.isNet ? (e[2] >= Math.max(u * 3, u + 1)) : (e[2] >= Math.max(u * 3, u + 5));
		if (high && !note) note = esc(e[0]) + ' was using ' + esc(o.fmtv(e[2])) + '; usually ' + esc(o.fmtv(u)) + '.';
		return '<tr' + (high ? ' class="culprit"' : '') + '><td class="n"><span class="' + (ownApp(e[1]) ? 'owner-app' : '') + '">' + esc(e[0]) + '</span><small>' + esc(o.isNet ? '' : appName(e[1])) + '</small></td><td class="v">' + esc(o.fmtv(e[2])) + '</td><td class="m"><div class="meter"><span class="' + (ownApp(e[1]) ? 'm-app' : 'm-cam') + '" style="width:' + clamp(100 * e[2] / mx, 2, 100) + '%"></span></div></td></tr>';
	}).join('');
	return '<div class="ht-h"><b>' + hhmm(o.t, o.seconds) + '</b><span class="hint">' + fmtAge(o.ago) + ' ago' + (o.pinned ? ' · pinned' : '') + '</span></div>' +
		'<div class="ht-v">' + o.vals.map(v => '<span><span class="sw" style="background:' + v[1] + '"></span>' + esc(v[0]) + ' <b>' + esc(v[2]) + '</b></span>').join('') + '</div>' +
		(o.none ? '' : '<div class="ht-t">' + esc(o.title) + '</div>') +
		(o.none ? '' : top.length ? '<table class="ht-tab">' + rows + (o.isIo ? '' : '<tr class="ht-other"><td class="n">' + (o.isNet ? 'Everything else (UDP, closed connections)' : 'Everything else') + '</td><td class="v">' + esc(o.fmtv(o.other || 0)) + '</td><td></td></tr>') + '</table>'
			: '<p class="hint">' + (o.isIo ? 'Nothing was waiting on the disk then.' : 'No list for this moment yet.') + '</p>') +
		(note ? '<div class="ht-note">' + note + '</div>' : '') +
		'';
}
/* Which list a graph shows on hover, its title and number format */
/* Processes waiting on the disk: seconds blocked (state D) at that moment or over that point */
const NO_KIND = { none: true, list: 'none', other: null, fmtv: v => String(v), title: '' };
const IO_KIND = { isIo: true, list: 'ioTop', other: null, fmtv: v => 'blocked ' + (v < 10 ? (+v).toFixed(1) : Math.round(v)) + ' s', title: 'Waiting on the disk then' };
function topKind(k, hasNet) {
	if (k === 'io' || k === 'flash') return IO_KIND;            /* storage graphs: what waited on the disk */
	if (k === 'temp' || k === 'power' || k === 'dlpu') return NO_KIND;   /* no process list here: it would be another resource's */
	const isMem = k === 'mem', isNet = k === 'net' && hasNet;
	return {
		isMem, isNet,
		list: isMem ? 'memTop' : isNet ? 'netTop' : 'cpuTop',
		other: isMem ? 'memOther' : isNet ? 'netOther' : 'cpuOther',
		fmtv: isMem ? fmtKb : isNet ? fmtMbps : v => fmtPct(v, 1),
		title: isMem ? 'Using the most memory then' : isNet ? 'Using the most network then (per TCP connection)' : 'Using the most CPU then' + (k === 'temp' ? ' (heat)' : '')
	};
}
/* One history graph as SVG: stacked areas for kinds of work, then the lines; X maps a time to the 0..W axis */
function histSvg(sp, P, X, W, H) {
	histSvg.n = histSvg.n || 0;
	const ax = sp.axis, pa = plotAxis(ax), lg = !!ax.log, l0 = lg ? Math.log10(pa.min) : 0, l1 = lg ? Math.log10(pa.max) : 0;
	const Y = v => (H - 3) * (1 - clamp(lg ? (Math.log10(Math.max(ax.min, v)) - l0) / (l1 - l0) : (v - pa.min) / (pa.max - pa.min), 0, 1));
	const a2 = sp.axis2, Y2 = a2 ? v => (H - 3) * (1 - clamp((v + a2.max * 0.02) / (a2.max * 1.02), 0, 1)) : Y, YS = x => x.right ? Y2 : Y;
	let g = '';
	for (let q = 1; q < 4; q++) g += '<line x1="0" x2="' + W + '" y1="' + H * q / 4 + '" y2="' + H * q / 4 + '" stroke="' + GRID + '"/>';
	for (let x = 1; x < 6; x++) g += '<line y1="0" y2="' + H + '" x1="' + W * x / 6 + '" x2="' + W * x / 6 + '" stroke="' + GRID2 + '"/>';
	/* 1.5.0: band series (the heater's On periods): a shaded strip over each point whose value is over 0.5 */
	sp.series.filter(x => x.band).forEach(x => {
		const dt = P.length > 1 ? P[1].t - P[0].t : 10;
		P.forEach((p, i) => {
			const v = x.get(p);
			if (v == null || v <= 0.5) return;
			const x0 = X(i ? P[i - 1].t : p.t - dt), x1 = X(p.t);
			if (x1 > x0) g += '<rect x="' + x0.toFixed(1) + '" y="0" width="' + (x1 - x0 + 0.4).toFixed(1) + '" height="' + H + '" fill="' + x.color + '"/>';
		});
	});
	if (pa.zero != null) g += '<line class="zero" x1="0" x2="' + W + '" y1="' + Y(pa.zero).toFixed(1) + '" y2="' + Y(pa.zero).toFixed(1) + '" stroke="' + ZERO + '" vector-effect="non-scaling-stroke"/>';
	/* Stacked areas: each kind of work on top of the ones before it; a gap in the readings breaks the area */
	let base = P.map(() => 0);
	sp.series.filter(x => x.stack).forEach(x => {
		const top = P.map((p, i) => { const v = x.get(p); return v == null ? null : base[i] + v; });
		let run = [];
		const flush = () => {
			if (run.length) g += '<path d="M' + run.map(i => X(P[i].t).toFixed(1) + ' ' + Y(top[i]).toFixed(1)).join(' L') + ' L' + run.slice().reverse().map(i => X(P[i].t).toFixed(1) + ' ' + Y(base[i]).toFixed(1)).join(' L') + ' Z" fill="' + x.color + '" fill-opacity=".55" stroke="' + x.color + '" stroke-width="1" vector-effect="non-scaling-stroke"/>';
			run = [];
		};
		top.forEach((v, i) => { if (v == null) flush(); else run.push(i); });
		flush();
		base = top.map((v, i) => v == null ? base[i] : v);
	});
	/* 1.5.2 filled look: each line's area fades from its colour to nothing at the bottom */
	const lines = sp.series.filter(x => !x.stack && !x.band), fa = lines.length > 2 ? .22 : .42;
	let defs = '';
	lines.forEach((x, n) => {
		const id = 'hf' + (++histSvg.n) + '-' + n;
		defs += '<linearGradient id="' + id + '" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="' + fillOf(x.color) + '" stop-opacity="' + fa + '"/><stop offset="1" stop-color="' + fillOf(x.color) + '" stop-opacity="0.02"/></linearGradient>';
		let run = [];
		const flush = () => { if (run.length > 1) g += '<path d="M' + run.map(q => q[0] + ' ' + q[1]).join(' L') + ' L' + run[run.length - 1][0] + ' ' + H + ' L' + run[0][0] + ' ' + H + ' Z" fill="url(#' + id + ')" stroke="none"/>'; run = []; };
		const Yx = YS(x);
		P.forEach(p => { const v = x.get(p); if (v == null) { flush(); return; } run.push([X(p.t).toFixed(1), Yx(v).toFixed(1)]); });
		flush();
	});
	if (defs) g = '<defs>' + defs + '</defs>' + g;
	sp.series.filter(x => !x.stack && !x.band).forEach(x => {
		let d = '', pen = false;
		const Yx = YS(x);
		P.forEach(p => { const v = x.get(p); if (v == null) { pen = false; return; } d += (pen ? 'L' : 'M') + X(p.t).toFixed(1) + ' ' + Yx(v).toFixed(1) + ' '; pen = true; });
		g += '<path d="' + d + '" fill="none" stroke="' + x.color + '" stroke-width="' + (x.color === BRAND ? 2 : 1.6) + '" stroke-linejoin="round" vector-effect="non-scaling-stroke"/>';
	});
	g += '<line class="hl" y1="0" y2="' + H + '" stroke="' + CURSOR + '" vector-effect="non-scaling-stroke" visibility="hidden"/>';
	return '<svg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none">' + g + '</svg>';
}
/* Hover on a history graph: the graph's own values and the top 10 then. pin: { get(), set(i) } (the main graph's pin is kept
   across refreshes; each core tile has its own). */
/* Returns reshow(i, y): shows moment i again, its tooltip at height y (a pinned moment, after the graph was drawn again) */
function histHover(box, svg, sp, P, X, W, t0, secs, tEnd, kind, usualOf, pin, onPoint, owner) {      /* onPoint(point): the moment pointed at, or null */
	const tip = makeTip(owner || 'hist');
	let lastY = 0;
	function show(i) {
		const p = P[i], x = X(p.t), hl = $('.hl', svg), r = svg.getBoundingClientRect();
		hl.setAttribute('x1', x); hl.setAttribute('x2', x); hl.setAttribute('visibility', 'visible');
		hl.setAttribute('stroke', pin.get() !== null ? EDGE : CURSOR);
		tip.innerHTML = topTipHtml({ t: p.t, ago: tEnd - p.t, pinned: pin.get() !== null, vals: valsOf(sp, p), title: kind.title, top: p[kind.list] || [], other: p[kind.other], usualOf, isMem: kind.isMem, isNet: kind.isNet, isIo: kind.isIo, none: kind.none, fmtv: kind.fmtv });
		if (onPoint) onPoint(p);
		tip.hidden = false;
		placeTip(tip, r.left + x / W * r.width, lastY);
	}
	const idxAt = e => {
		const r = svg.getBoundingClientRect(), t = t0 + (e.clientX - r.left) / r.width * secs;
		let best = 0;
		P.forEach((p, i) => { if (Math.abs(p.t - t) < Math.abs(P[best].t - t)) best = i; });
		return best;
	};
	svg.onmousemove = e => { if (pin.get() === null) { lastY = e.clientY; show(idxAt(e)); } };
	svg.onclick = e => { lastY = e.clientY; pin.set(pin.get() === null ? idxAt(e) : null); show(pin.get() === null ? idxAt(e) : pin.get()); };
	svg.onmouseleave = () => { if (pin.get() === null) { if (onPoint) onPoint(null); $('.hl', svg).setAttribute('visibility', 'hidden'); tip.hidden = true; } };
	return (i, y) => { lastY = y; show(i); };
}
/* keepPins (a change of theme): the same points drawn again, so a pinned moment stays pinned where it was; otherwise
   (new points) each core tile starts unpinned, as before */
const corePins = [];
function pinTipY(owner) { const t = $('.ftip[data-owner="' + owner + '"]'); return t && !t.hidden ? +t.dataset.y || 0 : 0; }
function drawHist(keepPins) {
	const box = $('#hist');
	if (!keepPins) corePins.length = 0;
	if (!hist || !hist.points.length) { box.innerHTML = '<div class="empty-note">No history yet: the first point is kept 10 seconds after Resource Monitor starts.</div>'; return; }
	const W = 800, P = hist.points, secs = hist.seconds, tEnd = clock(), t0 = tEnd - secs;
	const X = t => clamp((t - t0) / secs * W, 0, W);
	const kind = topKind(sel, P.some(p => p.netTop)), usualOf = usual(P, kind.list);
	const keptNote = hist.kept < secs ? 'History starts when Resource Monitor starts (' + fmtAge(hist.kept) + ' kept so far). ' : '';
	const foot = kindNote => '<div class="chart-note"><span>' + spanAgo(secs) + '</span><span>' + (hist.source === 'camera' ? 'From the camera\'s own power log: one point every ' + fmtStep(hist.step) + '.' : keptNote + kindNote + 'Each point is ' + fmtStep(hist.step) + ', keeping its peak. ' + (S.now.settings && S.now.settings.onCard ? 'History is kept on the SD card for ' + spanName(S.now.settings.historyDays * 86400) + ' and survives restarts.' : S.now.settings && S.now.settings.onFlash ? 'History is kept on the camera\'s built-in flash for 24 h and survives restarts.' : 'History is kept in memory for 24 h and starts again if the app or camera restarts.')) + '</span><span>Now</span></div>' +
		(secs > 86400 && !kind.none ? '<div class="hint small hist-help">The top 10 lists are kept for the last 24 h only.</div>' : '');
	if (sel === 'cpu' && cpuParent === 'cores') {
		/* Each core: a small graph per core, with the label the live tiles have */
		const nc = Math.max(0, ...P.map(p => (p.coresWork || []).length));
		if (!nc) { box.innerHTML = '<div class="empty-note">No history for each core yet.</div>'; return; }
		/* 1.5.0: the core graphs together take the Overall graph's height, and the legend is under them as it is there */
		box.innerHTML = '<div class="chart-label"><span class="hist-keys"></span><span class="hist-max">100 %</span></div><div class="hist-cores-box"><div class="cores-grid hist-cores">' +
			Array.from({ length: nc }, (_, i) => '<div class="ct"><div class="lbl">' + coreLabel(i) + '</div><div class="hc">' + histSvg(coreSpec(i), P, X, W, 110) + '</div></div>').join('') + '</div>' +
			(view.cpu === 'load' ? '<div class="cores-note">' + esc($('#cores-note') ? $('#cores-note').textContent : '') + '</div>' : '') + '</div>' +
			'<div class="' + legendCls('cpu') + '">' + legendHtml(coreSpec(0).series) + '</div>' +
			foot(view.cpu === 'work' ? 'Stacked per core: the top edge is that core in use. ' : '');
		$$('.hist-cores .hc', box).forEach((hc, i) => {
			const owner = 'hist-core-' + i, y = pinTipY(owner), pin = corePins[i] != null && P[corePins[i]] ? corePins[i] : (corePins[i] = null);
			const reshow = histHover(hc, $('svg', hc), coreSpec(i), P, X, W, t0, secs, tEnd, kind, usualOf, { get: () => corePins[i], set: v => { corePins[i] = v; } }, null, owner);
			if (pin !== null) reshow(pin, y);
		});
		return;
	}
	const sp = spec(sel, P), ax = sp.axis, lg = !!ax.log;
	const axTxt = v => ax.format ? ax.format(v) : (Math.round(v * 100) / 100) + (ax.unit || '');
	const kindNote = sp.count ? 'Errors and drops are counted in each point. ' : sel === 'flash' && view.flash === 'space' ? 'Space is the latest reading in each point. ' : sp.stacked ? 'Stacked: the top edge is the whole CPU in use. ' : '';
	const below = belowLegend(sel);
	box.innerHTML = '<div class="chart-label"><span class="hist-keys">' + (below ? '' : legendHtml(sp.series)) + '</span><span class="hist-max">' + esc(axTxt(ax.max)) + (lg ? ' (each line down is 10 times less)' : '') + (sp.axis2 ? ' · <span class="ax2">apps scale ' + esc((Math.round(sp.axis2.max * 10) / 10) + (sp.axis2.unit === '%' ? ' %' : sp.axis2.unit)) + '</span>' : '') + '</span></div>' +
		histSvg(sp, P, X, W, 280) + (below ? '<div class="' + legendCls(sel) + '">' + legendHtml(sp.series) + '</div>' : '') + foot(kindNote);
	const y = pinTipY('hist');
	if (pinned !== null && !P[pinned]) pinned = null;
	const reshow = histHover(box, $('svg', box), sp, P, X, W, t0, secs, tEnd, kind, usualOf, { get: () => pinned, set: v => { pinned = v; } }, null, 'hist');
	if (keepPins && pinned !== null) reshow(pinned, y);
}

/*---------------------------------------------------- Processes */
let pSort = 'cpu', pWho = 'all', otherOpen = null, procTimer = null;
/* A value and its bar: left-aligned as one group, the value in a fixed slot so the bars line up; the header is centred over it */
function cellNone() { return '<div class="cellm"><span>–</span></div>'; }
function cellMeter(v, max, app, txt) { return '<div class="cellm"><span>' + esc(txt) + '</span><div class="meter"><span class="' + (app ? 'm-app' : 'm-cam') + '" style="width:' + clamp(100 * v / max, 2, 100) + '%"></span></div></div>'; }
/*---------------------------------------------------- navigation: the bar picks an item, the row under it that item's pages */
const ITEMS = {
	overview: { pages: [['overview', 'All resources']] },
	cpu: { pages: [['use', 'Usage'], ['procs', 'Processes']] },
	mem: { pages: [['use', 'Usage'], ['procs', 'Processes']] },
	flash: { pages: [['use', 'Usage'], ['drives', 'Drives'], ['procs', 'Processes']] },
	net: { pages: [['use', 'Usage'], ['conns', 'Connections']] },
	temp: { pages: [['use', 'Sensors']] },
	power: { pages: [['use', 'Power']] },
	dlpu: { pages: [['use', 'AI chip']] },
	apps: { pages: [['list', 'Installed apps'], ['log', 'Log']] },
	alerts: { pages: [['alerts', 'Alerts']] },
	settings: { pages: [['settings', 'Settings']] }
};
const TAB_OF = { overview: 'overview', use: 'perf', procs: 'procs', drives: 'drives', conns: 'conns', list: 'apps', log: 'log', alerts: 'alerts', settings: 'settings' };
let item = 'overview';
const pageOf = { overview: 'overview', cpu: 'use', mem: 'use', flash: 'use', net: 'use', temp: 'use', power: 'use', dlpu: 'use', apps: 'list', alerts: 'alerts', settings: 'settings' };
/* Open an item (and a page of it; without one, the page it was last on). Kept in the address as #item/page. */
/* 1.6.1: leaving Settings with changes not saved asks first */
function go(it, page) {
	if (item === 'settings' && it !== 'settings' && settingsDirty()) {
		history.replaceState(null, '', '#settings');           /* the back button or a typed address: stay put while asking */
		ask('Leave without saving?', 'You changed settings and haven\'t saved them. Leaving now throws those changes away.', 'Leave without saving', () => {
			setForm = JSON.parse(JSON.stringify(setData.settings)); $('#s-msg').textContent = ''; drawSettings(); go(it, page);
		}, false);
		$('#da-ok').classList.add('danger');
		return;
	}
	$('#da-ok').classList.remove('danger');
	if (!ITEMS[it]) it = 'overview';
	if (page && ITEMS[it].pages.some(p => p[0] === page)) pageOf[it] = page;
	item = it;
	if (!ITEMS[it].pages.some(p => p[0] === pageOf[it] && pageOk(p))) pageOf[it] = ITEMS[it].pages[0][0];
	const pg = pageOf[it];
	if (it !== 'overview' && it !== 'apps' && it !== 'alerts' && it !== 'settings') sel = it;
	if (pg === 'procs') procMode = it === 'mem' ? 'mem' : it === 'flash' ? 'io' : 'cpu';
	const h = '#' + it + '/' + pg;
	if (location.hash !== h) history.replaceState(null, '', h);
	pinned = null;
	drawSubnav();
	showTab(TAB_OF[pg]);
	window.scrollTo({ top: 0 });
}
/* Processes and Connections pages only while Settings has them on */
const pageOk = p => p[0] === 'procs' ? shown(GB.procs) : p[0] === 'conns' ? shown(GB.conns) : true;
function drawSubnav() {
	const pg = pageOf[item];
	$('#subnav').innerHTML = ITEMS[item].pages.filter(pageOk).map(p => '<button type="button" data-page="' + p[0] + '"' + (p[0] === pg ? ' class="on" aria-current="page"' : '') + '>' + esc(p[1]) + '</button>').join('');
	$$('#subnav button').forEach(b => { b.onclick = () => go(item, b.dataset.page); });
}
/* The Live | 1 h | 24 h choice is one, shared by every page */
function setRange(r) {
	range = r === 'live' ? 'live' : +r;
	$$('.range button').forEach(b => b.classList.toggle('on', String(b.dataset.r) === String(r)));
	pinned = null;
	if (tab === 'perf') showPerf();
}

/*---------------------------------------------------- Overview: one row per resource */
let ovHist = null, ovTimer = null, ovPower = null;
async function loadOverview() {
	clearTimeout(ovTimer);
	if (tab !== 'overview') return;
	try { ovHist = await getJSON('history?seconds=3600&points=60'); } catch (e) { /* the rows show without their lines */ }
	/* Power's last hour from the camera's own power log when it keeps one */
	ovPower = S.now.power && S.now.power.history ? await getJSON('power?seconds=3600').then(p => p.ok ? p.points : null).catch(() => null) : null;
	drawOverview();
	ovTimer = setTimeout(loadOverview, 30000);
}
function spark(vals, max, color) {
	const W = 200, H = 28, n = vals.length;
	let d = '', pen = false;
	vals.forEach((v, i) => { if (v == null) { pen = false; return; } d += (pen ? 'L' : 'M') + (n > 1 ? i * W / (n - 1) : 0).toFixed(1) + ' ' + (H - 2 - clamp(v / max, 0, 1) * (H - 4)).toFixed(1) + ' '; pen = true; });
	return '<svg class="spark" viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none"><line x1="0" x2="' + W + '" y1="' + (H - 2) + '" y2="' + (H - 2) + '" stroke="' + ZERO + '" vector-effect="non-scaling-stroke"/><path d="' + d + '" fill="none" stroke="' + color + '" stroke-width="1.6" vector-effect="non-scaling-stroke"/></svg>';
}
/* 1.6.1 The tiles are redrawn every second: when the same tiles are there, only what is inside each one changes, so a tile
 * the pointer is on keeps its raised hover look instead of bouncing */
function setTiles(box, html) {
	const tmp = document.createElement('div');
	tmp.innerHTML = html;
	const now = [...box.children], next = [...tmp.children];
	if (now.length !== next.length || now.some((el, i) => el.tagName !== next[i].tagName || el.getAttribute('data-go') !== next[i].getAttribute('data-go'))) { box.innerHTML = html; return; }
	now.forEach((el, i) => {
		const n = next[i];
		if (el.innerHTML !== n.innerHTML) el.innerHTML = n.innerHTML;
		['title', 'href'].forEach(k => { if (el.getAttribute(k) !== n.getAttribute(k)) el.setAttribute(k, n.getAttribute(k)); });
	});
}
/* 1.6.1 Overview tile graphs: the last minute as a compressing graph (60 readings across, newest widest), drawn in the
 * same look as the other pages' graphs (no axis labels in the small tiles). One graph per
 * tile lives across redraws: each redraw leaves a slot that the graph's own element is moved into. */
const OVG = {};
/* The same look as the graphs on the other pages (plot background, grid, frame, end markers), so the compressing is visible; no axis labels in the small tiles */
const OV_STYLE = { hover: false, legend: false, title: '', lineWidth: 1.6, gridMinPx: 18 };
/* val: one reading's value, or [[name, colour, value], ...] for several lines (Network: Sent and Received, as on its page) */
function liveSpark(k, val, axis, color, axis2) {
	const o = OVG[k] || (OVG[k] = {});
	o.lines = Array.isArray(val) ? val : [[k, color, val]];
	o.axis = Object.assign({ unit: '', labels: false }, axis); o.color = o.lines.map(l => l[1]).join();
	/* lines marked 1 are on the apps' own scale (right, not labelled in the small tiles) */
	o.axis2 = o.lines.some(l => l[3]) ? Object.assign({ unit: '', labels: false }, axis2 || { min: 0, max: 1, auto: true, floorMax: 1 }) : null;
	return '<div class="d-spk" data-ovg="' + k + '"></div>';
}
function mountSparks() {
	$$('#ov-tiles [data-ovg]').forEach(ph => {
		const o = OVG[ph.dataset.ovg];
		if (!o.el) { o.el = document.createElement('div'); o.el.className = 'd-spk'; }
		ph.replaceWith(o.el);
		const sig = JSON.stringify(o.axis) + o.color;
		if (!o.g) {
			o.g = cg(o.el, Object.assign({}, OV_STYLE, { series: ovSeries(o), axes: { left: o.axis, right: o.axis2 } }));
			(S.window || []).forEach(s => o.g.push(ovVal(o, s), s.t));
		} else if (sig !== o.sig) {   /* a new range or unit: draw the minute again in it */
			o.g.clear();
			o.g.setOptions({ series: ovSeries(o), axes: { left: o.axis, right: o.axis2 } });
			(S.window || []).forEach(s => o.g.push(ovVal(o, s), s.t));
		}
		o.sig = sig;
	});
}
function ovSeries(o) { return o.lines.map(l => ({ name: l[0], color: l[1], fillColor: fillOf(l[1]), axis: l[3] ? 'right' : 'left' })); }
function ovVal(o, s) { return o.lines.map(l => { const v = l[2](s); return v == null || isNaN(v) ? null : v; }); }
function feedSparks(smp) { Object.values(OVG).forEach(o => { if (o.g) smp.forEach(s => o.g.push(ovVal(o, s), s.t)); }); }
/* 1.5.0: a tile per resource (opens its page, as the sidebar does), status chips, and the biggest users now */
function drawOverview() {
	const s = S.last || {}, n = S.now, P = (ovHist && ovHist.points) || [], f = fullest(), h = hottest();
	const col = k => P.map(k), okv = a => a.filter(v => v != null && !isNaN(v));
	const avg = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : NaN;
	const tile = (k, label, value, detail, graph, share) => '<a class="d-tile" href="#' + k + '" data-go="' + k + '"><div class="d-h"><span class="d-l">' + esc(label) + '</span><span class="d-r">Last minute</span></div>' +
		'<div class="d-v">' + unitHtml(value) + '</div><div class="d-s">' + esc(detail) + '</div>' + graph + (share || '') + '</a>';
	const share = (title, cam, app, camTxt, appTxt) => '<div class="d-share" title="' + esc(title) + '"><span class="sh-cam" style="width:' + clamp(num(cam) || 0, 0, 100).toFixed(1) + '%"></span><span class="sh-app" style="width:' + clamp(num(app) || 0, 0, 100).toFixed(1) + '%"></span></div>' +
		'<div class="d-share-k"><span><i class="k-cam"></i>Camera ' + esc(camTxt) + '</span><span><i class="k-app"></i>Apps ' + esc(appTxt) + '</span></div>';
	const tiles = [];
	if (shown(GB.cpu)) {
		const v = okv(col(p => p.cpu)), cam = Math.max(0, num(s.cpu) - num(n.appsCpu));
		tiles.push(tile('cpu', 'CPU', fmtPct(s.cpu, 0), (v.length ? 'Last hour: peak ' + fmtPct(Math.max(...v), 0) + ' · avg ' + fmtPct(avg(v), 0) + ' · ' : '') + (n.cores || '?') + ' cores' + (n.mhz ? ' at ' + Math.round(n.mhz) + ' MHz' : ''),
			liveSpark('cpu', [['Apps', BRAND, p => p.apps, 1], ['Camera', CAM_LINE, p => p.cpu]], { min: 0, max: 100 }), share('CPU: the camera and apps', cam, n.appsCpu, fmtPct(cam, 0), fmtPct(n.appsCpu, 1))));
	}
	if (shown(GB.mem)) {
		const used = n.memTotalKb - n.memAvailKb, cam = Math.max(0, used - num(n.appsMemKb)), pc = kb => 100 * kb / n.memTotalKb;
		tiles.push(tile('mem', 'Memory', fmtPct(s.mem, 0), fmtKb(used) + ' of ' + fmtKb(n.memTotalKb) + (n.videoTotalKb ? ' · video memory ' + fmtPct(100 * n.videoUsedKb / n.videoTotalKb, 0) : ''),
			liveSpark('mem', p => p.mem, { min: 0, max: 100 }, CAM_LINE), share('Memory: the camera and apps', pc(cam), pc(n.appsMemKb), fmtPct(pc(cam), 0), fmtPct(pc(n.appsMemKb), 1))));
	}
	if (shown(GB.flash)) {
		const dk = f && f.disk >= 0 ? (n.disks || [])[f.disk] : null;
		tiles.push(tile('flash', 'Storage', f ? fmtPct(usedPct(f), 0) : fmtPct(s.flash, 0), f ? f.name + ' ' + fmtKb(f.freeKb) + ' free' + (dk && dk.life ? ' · disk life ' + dk.life + ' used' + (dk.eol ? ', ' + dk.eol : '') : '') : '–',
			liveSpark('flash', f ? (f.sd ? p => p.sd : f.flash ? p => p.flash : p => at(p, 'mounts', f.mi)) : p => p.flash, { min: 0, max: 100 }, C.teal)));
	}
	if (shown(GB.net)) {
		const v = okv(col(p => (p.tx || 0) + (p.rx || 0))), i0 = (n.nets || [])[0], errs = (n.nets || []).reduce((a, x) => a + (x.errorsHour || 0) + (x.dropsHour || 0), 0);
		tiles.push(tile('net', 'Network', fmtMbps(num(s.tx) + num(s.rx)), (v.length ? 'Last hour: peak ' + fmtMbps(Math.max(...v)) + ' · ' : '') + (i0 ? i0.name + ' ' + (i0.state || '').toLowerCase() + (i0.linkMbps ? ' at ' + fmtLink(i0.linkMbps) : '') : n.netIf || '') + ' · ' + errs + (errs === 1 ? ' error' : ' errors') + ' last hour',
			liveSpark('net', [['Sent', TX, p => p.tx], ['Received', RX, p => p.rx]], { min: 0, max: 1, auto: true, floorMax: 0.1 })));
	}
	if (shown(GB.temp) && h) {
		const v = okv(col(p => p.temp)), hi24 = Math.max(...(n.sensors || []).map(x => x.high24 || -99), -99);
		tiles.push(tile('temp', 'Temperature', fmtC(h.v), (h.name ? 'Hottest: ' + h.name : 'Hottest sensor') + (hi24 > -99 ? ' · highest in 24 h ' + fmtC(hi24) : ''),
			liveSpark('temp', p => toT(p.temp), { min: Math.floor(toT(v.length ? Math.min(...v) : 20) / 10) * 10 - 10, max: Math.ceil(toT(v.length ? Math.max(...v) : 60) / 10) * 10 + 10 }, C.warn)));
	}
	if (shown(GB.power) && n.power) {
		const pp = ovPower || P, v = okv(pp.map(p => p.power)), p = n.power;
		tiles.push(tile('power', 'Power', fmtW(s.power), [p.avgW != null ? 'Avg ' + fmtW(p.avgW) : '', p.maxW != null ? 'max ' + fmtW(p.maxW) : v.length ? 'max ' + fmtW(Math.max(...v)) : '', p.class ? 'PoE class ' + p.class : p.kind === 'ports' ? 'PoE ports' : ''].filter(Boolean).join(' · '),
			liveSpark('power', p => p.power, { min: 0, max: 5, auto: true, floorMax: 5 }, BRAND)));
	}
	if (shown(GB.dlpu) && n.dlpu) {
		const v = okv(col(p => p.infer));
		tiles.push(tile('dlpu', 'AI chip', s.infer != null ? fmtCount(s.infer) + ' /s' : '–', 'Jobs a second' + (v.length ? ' · peak ' + fmtCount(Math.max(...v)) + ' /s' : ''), liveSpark('dlpu', p => p.infer, { min: 0, max: 10, auto: true, floorMax: 10 }, C.teal)));
	}
	setTiles($('#ov-tiles'), tiles.join('') || '<p class="hint">Every resource is switched off in Settings › What to collect.</p>');
	mountSparks();
	fitTiles();
	/* Status chips */
	const chip = (l, v, cls, title) => '<div class="d-chip' + (cls ? ' ' + cls : '') + '"' + (title ? ' title="' + esc(title) + '"' : '') + '><span class="c-l">' + esc(l) + '</span><span class="c-v">' + esc(v) + '</span></div>';
	const ch = [chip('Up', fmtAge(n.uptime), '', 'Time since the camera started'),
		chip('Load', n.load1 != null ? n.load1.toFixed(2) + ' · ' + n.load5.toFixed(2) + ' · ' + n.load15.toFixed(2) : '–', '', 'Load average over 1, 5 and 15 min; ' + (n.cores || '?') + ' cores'),
		chip('Processes', (n.processes || 0) + ' · ' + (n.threads || 0) + ' threads')];
	if (n.heater) {
		const hz = n.heater;
		ch.push(hz.on ? chip('Heater', hz.since ? 'On for ' + fmtAge(clock() - hz.since) : 'On', 'hot', 'The camera\'s heater is on') : chip('Heater', 'Off' + (hz.lastOn ? ' · last on until ' + whenTxt(hz.lastOn) : ''), 'dim'));
	}
	if (shown(GB.conns) && S.cam && S.cam.streams) {
		const ss = S.cam.streams, who = new Set(ss.map(x => x.dst)).size, v = ss.find(x => mediaKind(x) === 'video') || ss[0];
		ch.push(ss.length ? chip('Streams', who + (who === 1 ? ' viewer' : ' viewers') + (v ? ' · ' + [(v.protocol || '').toUpperCase(), mediaKind(v) === 'video' ? videoCodec(v) : ''].filter(Boolean).join(' ') : ''), '', 'Who is watching: Network › Connections') : chip('Streams', 'None', 'dim'));
	}
	if (AL.data) {
		const l = AL.data.alerts || [], w = x => l.filter(a => a.status && a.status.word === x).length, on = l.filter(a => a.on).length, trig = w('Triggered'), wait = w('Waiting');
		ch.push(trig ? chip('Alerts', trig + ' triggered' + (wait ? ' · ' + wait + ' waiting' : ''), 'hot') : wait ? chip('Alerts', wait + ' waiting for approval', 'hot') : on ? chip('Alerts', on + ' on · none triggered', 'ok') : chip('Alerts', l.length ? 'All off' : 'None set up', 'dim'));
	}
	if (shown(GB.flash)) ch.push(n.sdTotalKb ? chip('SD card', fmtKb(n.sdFreeKb) + ' free of ' + fmtKb(n.sdTotalKb), 'ok') : chip('SD card', 'None', 'dim'));
	$('#ov-chips').innerHTML = ch.join('');
	/* The biggest users now: value and bar */
	const top = (list, key, fmt, min) => {
		const mx = Math.max(min, ...list.map(p => p[key] || 0));
		return list.length ? list.slice(0, 5).map(p => '<tr><td title="' + esc(p.name + ' · ' + appName(p.app)) + '"><span class="' + (ownApp(p.app) ? 'owner-app' : '') + '">' + esc(p.name) + '</span></td><td class="bar-col">' + cellMeter(p[key] || 0, mx, ownApp(p.app), fmt(p[key])) + '</td></tr>').join('')
			: '<tr><td colspan="2" class="hint">Reading…</td></tr>';
	};
	$('#ov-cpu').innerHTML = top(S.topCpu || [], 'cpu', v => fmtPct(v, 1), 1);
	$('#ov-mem').innerHTML = top(S.topMem || [], 'memKb', fmtKb, 1024);
	$$('#tab-overview .d-all').forEach(a => { a.hidden = !shown(GB.procs); });
	drawCamStatus();
}
/* 1.6.1 Balanced rows, one rule for every set of tiles (Overview tiles, each-core graphs, the figures under a graph):
 * as few rows as the width allows (each item at least minW wide, at most cap a row), and every row filled edge to edge,
 * so 7 tiles are 4 + 3, 6 cores 3 + 3, 9 figures 5 + 4: never a blank spot. A 12-column grid takes spans (cap 4 or less); a flex row takes widths. */
function fitRows(box, sel, minW, cap) {
	if (!box || !box.clientWidth) return;
	const t = [...box.children].filter(x => x.matches(sel));
	if (!t.length) return;
	const cs = getComputedStyle(box), gap = parseFloat(cs.columnGap) || 0, grid = cs.display === 'grid';
	const max = Math.max(1, Math.min(cap, t.length, Math.floor((box.clientWidth + gap) / (minW + gap)))), rows = Math.ceil(t.length / max);
	const base = Math.floor(t.length / rows), extra = t.length % rows;
	let i = 0;
	for (let r = 0; r < rows; r++) {
		const k = base + (r < extra ? 1 : 0);
		for (let j = 0; j < k; j++, i++) {
			if (grid) t[i].style.gridColumn = 'span ' + (12 / k);
			else t[i].style.flex = '0 0 calc((100% - ' + ((k - 1) * gap) + 'px) / ' + k + ')';
		}
	}
}
function fitTiles() {
	fitRows(document.getElementById('ov-tiles'), '.d-tile', 300, 4);
	document.querySelectorAll('.cores-grid').forEach(g => fitRows(g, '.ct', 200, 4));
	fitRows(document.getElementById('stats'), '.stat', 130, 6);
}
window.addEventListener('resize', fitTiles);
/* Time sync and Security, read by the app from the camera's own APIs (ntp.cgi, param.cgi, virtualhost, applications/config.cgi); same words and warnings as the camera's own pages */
let ntpDue = null;
function hms(s) { s = Math.max(0, Math.round(s)); return [s / 3600 | 0, (s % 3600) / 60 | 0, s % 60].map(x => String(x).padStart(2, '0')).join(':'); }
function kvRow(label, value, state, tip, id) {
	/* 1.6.5: the camera's advice for a warning is in an (i) next to it, not a hover popup */
	const ic = state === 'warn' ? '<span class="kv-i warn">⚠' + (tip ? '<button type="button" class="kit-info" data-help-title="' + esc(label) + '" data-help="' + esc(tip) + '" aria-label="Help: ' + esc(label) + '" aria-expanded="false">i</button>' : '') + '</span>' : state === 'ok' ? '<span class="kv-i ok">✓</span>' : '<span></span>';
	return '<div class="kv"><span class="kv-l">' + esc(label) + '</span><span class="kv-v"' + (id ? ' id="' + id + '"' : '') + '>' + value + '</span>' + ic + '</div>';
}
function drawCamStatus() {
	const c = S.cam || {}, t = c.time, s = c.security, cam = location.protocol + '//' + location.host + '/camera/index.html#/system/';
	$('#ov-time-go').href = cam + 'dateTime'; $('#ov-sec-go').href = cam + 'security';
	if (!t) $('#ov-time').innerHTML = '<p class="hint">Reading…</p>';
	else if (!t.enabled) $('#ov-time').innerHTML = kvRow('Synchronized', 'Off · the camera keeps its own clock', 'warn', 'Turn on NTP in the camera');
	else {
		/* Counted from when this answer came, not from each redraw */
		if (t.nextSync != null && !t.due) t.due = Date.now() + t.nextSync * 1000;
		ntpDue = t.due || null;
		$('#ov-time').innerHTML = kvRow('Synchronized', '<span class="dot ' + (t.synced ? 'ok' : 'bad') + '"></span>' + (t.synced ? 'Yes' : 'No'), '', '') +
			kvRow('Next sync', nextTxt(), '', '', 'ov-next') +
			kvRow('Time offset', t.synced && t.offsetMs != null ? t.offsetMs.toFixed(3) + ' ms' : '–', '', '') +
			kvRow((t.source === 'secure' ? 'NTS KE servers' : 'NTP servers'), esc((t.servers || []).join(', ') || '–'), '', '');
	}
	if (!s) { $('#ov-sec').innerHTML = '<p class="hint">Reading…</p>'; return; }
	const list = a => a.length < 2 ? a.join('') : a.slice(0, -1).join(', ') + ' and ' + a[a.length - 1];
	const on = s.discovery.filter(d => d.on).map(d => d.name), all = s.discovery.map(d => d.name);
	$('#ov-sec').innerHTML = (s.access ? kvRow('Access', s.access === 'https' ? 'HTTPS' : s.access === 'http' ? 'HTTP' : 'HTTP and HTTPS', s.access === 'https' ? 'ok' : 'warn', 'Turn off HTTP access.') : '') +
		(s.oldTls != null ? kvRow('TLS 1.0/1.1', s.oldTls ? 'On' : 'Off', s.oldTls ? 'warn' : 'ok', 'Turn off TLS 1.0 and TLS 1.1.') : '') +
		(s.ssh != null ? kvRow('SSH', s.ssh ? 'On' : 'Off', s.ssh ? 'warn' : 'ok', 'Turn off SSH access.') : '') +
		(all.length ? kvRow('Discovery', on.length ? esc(list(on)) : esc(list(all)) + (all.length === 1 ? ' is' : ' are') + ' turned off', on.length ? 'warn' : 'ok', 'Turn off ' + list(all) + '.') : '') +
		(s.unsigned != null ? kvRow('ACAP apps', s.unsigned ? 'Unsigned' : 'Signed', s.unsigned ? 'warn' : 'ok', 'Turn off "Allow unsigned apps" on the camera\'s Apps page.') : '') +
		(s.devMode ? kvRow('Developer mode', 'On', 'warn', 'A developer certificate is installed.') : '');
}
/* Next sync counts down each second; at zero it reads "Syncing…" and the camera is asked again 5 s later (once per answer) */
let ntpAsked = null;
function nextTxt() { return !ntpDue ? '–' : ntpDue > Date.now() ? hms((ntpDue - Date.now()) / 1000) : 'Syncing…'; }
setInterval(() => {
	const e = document.getElementById('ov-next');
	if (!e || !ntpDue || tab !== 'overview') return;
	e.textContent = nextTxt();
	if (Date.now() > ntpDue + 5000 && ntpAsked !== ntpDue) { ntpAsked = ntpDue; clearTimeout(camTimer); pollCamera(); }
}, 1000);

/*---------------------------------------------------- Storage › Drives: each drive, and the apps' storage on flash */
function drawDrives() {
	const s = S.last || {}, n = S.now;
	const rows = drives().map(d => ({ name: d.name, kind: d.kind, used: usedPct(d), d, disk: d.disk >= 0 ? (n.disks || [])[d.disk] : null, di: d.disk }));
	(n.disks || []).forEach((k, i) => { if (k.present && !rows.some(r => r.di === i)) rows.push({ name: k.name, kind: 'dev', used: NaN, disk: k, di: i }); });
	const life = k => {
		if (!k || (!k.life && !k.eol)) return null;
		const bad = k.lifeLow >= 90 || k.eol === 'Urgent', warn = k.lifeLow >= 70 || k.eol === 'Warning';
		return '<span class="status ' + (bad ? 'error' : warn ? 'warn' : 'ok') + '">' + esc(k.life ? 'Card life used ' + k.life : 'Card wear: ' + k.eol) + '</span>';
	};
	/* The camera's own reading of a card or share: health, wear (negative: not an Axis card), temperature, and its recordings */
	const camDisk = kind => (((S.cam && S.cam.disks) || []).find(d => kind === 'sd' ? /SD/i.test(d.id) : kind === 'nas' ? /network|nas|share/i.test(d.id) : false));
	const camHealth = d => {
		if (!d || (d.health == null && d.wear == null)) return null;
		const bad = d.health === 0 || d.wear >= 90, warn = d.wear >= 70;
		return '<span class="status ' + (bad ? 'error' : warn ? 'warn' : 'ok') + '">' + (d.health === 0 ? 'Failed its health check' : d.health === 1 ? 'Healthy' : 'Health not reported') + '</span>' +
			'<small class="cell-sub">' + esc((d.wear == null || d.wear < 0 ? 'Wear not reported' : 'Wear ' + Math.round(d.wear) + ' %') + (d.tempC != null ? ' · ' + toT(d.tempC).toFixed(0) + ' ' + tUnit() : '')) + '</small>';
	};
	const recLine = d => !d || d.recLimitKb == null ? '' : '<small class="cell-sub">Recordings: ' + esc(d.recUsedKb != null ? 'used ' + fmtKb(d.recUsedKb) : '') + esc(d.recLimitKb > 0 ? (d.recUsedKb != null ? ' of ' : '') + fmtKb(d.recLimitKb) + ' allowed' : (d.recUsedKb != null ? ', ' : '') + 'no limit') + '</small>';
	const apps = S.appList.filter(a => a.diskKb).sort((a, b) => b.diskKb - a.diskKb), mx = Math.max(1, ...apps.map(a => a.diskKb));
	const a = appsDiskKb(), u = n.flashTotalKb - n.flashFreeKb;
	$('#drives').innerHTML = '<table class="acap-table drives stack"><colgroup><col class="c-dr"><col><col class="c-rw"><col class="c-rw"><col class="c-busy"><col class="c-hl"></colgroup>' +
		'<thead><tr><th>Drive</th><th class="bar-col">Space used</th><th class="num">Read</th><th class="num">Write</th><th class="num">Busy</th><th>Health</th></tr></thead><tbody>' +
		rows.map(r => {
			const v = r.di >= 0 ? (s.disks || [])[r.di] || [] : [], nas = r.kind === 'nas';
			const space = isNaN(r.used) ? '<span class="hint">' + (r.kind === 'dev' ? 'The whole device' : '–') + '</span>'
				: '<div class="bar"><span' + (r.used >= 90 ? ' class="w"' : '') + ' style="width:' + clamp(r.used, 1, 100) + '%"></span></div><span class="hint small">' + fmtPct(r.used, 0) + ' · ' + fmtKb(r.d.totalKb - r.d.freeKb) + ' of ' + fmtKb(r.d.totalKb) + '</span>' + recLine(camDisk(r.kind));
			return '<tr><td data-l="Drive">' + esc(r.name) + '</td><td class="bar-col" data-l="Space used">' + space + '</td>' +
				'<td class="num" data-l="Read">' + (nas ? '–' : fmtRate(v[0])) + '</td><td class="num" data-l="Write">' + (nas ? '–' : fmtRate(v[1])) + '</td><td class="num" data-l="Busy">' + (nas || v[2] == null ? '–' : fmtPct(v[2], 0)) + '</td>' +
				'<td data-l="Health">' + ((r.kind === 'sd' || r.kind === 'nas' ? camHealth(camDisk(r.kind)) : life(r.disk)) || '<span class="hint">' + (nas ? 'Network share' : '–') + '</span>') + '</td></tr>';
		}).join('') + '</tbody></table>' +
		'<p class="hint small extra-note">The SD card\'s health, wear and temperature are the camera\'s own reading (a card that isn\'t Axis-made reports no wear); the built-in flash\'s card life comes from the chip itself. Amber at 70 %, red at 90 %. A NAS\'s reads and how busy it is are not visible to the camera.</p>' +
		'<div class="sub-h" style="margin-top:14px"><h3>Apps on Flash (apps)</h3></div>' +
		compBar('', [['Apps', Math.min(a, u), 'app'], ['Camera', Math.max(0, u - a), 'cam'], ['Free', n.flashFreeKb, 'free']], fmtKb).replace('<div class="sub-h"><h3></h3></div>', '') +
		'<table class="acap-table rows-t drv-apps stack"><colgroup><col><col class="c-bar"></colgroup><thead><tr><th>App</th><th class="bar-col">On flash</th></tr></thead><tbody>' +
		(apps.length ? apps.map(x => '<tr><td data-l="App"><span class="' + (ownApp(x.id) ? 'owner-app' : '') + '">' + esc(x.name) + '</span>' + (x.axis ? '<span class="this-app axis-tag">Axis</span>' : '') + '</td><td class="bar-col" data-l="On flash">' + cellMeter(x.diskKb, mx, ownApp(x.id), fmtKb(x.diskKb)) + '</td></tr>').join('')
			: '<tr><td colspan="2" class="hint">No app storage measured yet.</td></tr>') + '</tbody></table>';
}

/*---------------------------------------------------- what the camera itself reports (streams, disks, audit log): every 30 s */
let camTimer = null;
async function pollCamera() {
	clearTimeout(camTimer);
	try { S.cam = await getJSON('camera'); } catch (e) { /* keep the last answer */ }
	if (tab === 'conns') drawConns();
	if (tab === 'drives') drawDrives();
	if (tab === 'overview') drawOverview();
	camTimer = setTimeout(pollCamera, 30000);
}
/* Streams in plain words. One compact line per row ("Video 4K · H.264 · 60 fps · audio"); every detail is in the row's hover. */
const CODEC = { h264: 'H.264', h265: 'H.265', av1: 'AV1', jpeg: 'MJPEG', mjpeg: 'MJPEG' };
const AUDIO_MIME = { mpeg: 'AAC', 'mpeg4-generic': 'AAC', aac: 'AAC', basic: 'G.711', pcmu: 'G.711', pcma: 'G.711', g726: 'G.726', 'g726-32': 'G.726', 'g726-24': 'G.726', opus: 'Opus' };
const RES_NAME = { '3840x2160': '4K', '2560x1440': '1440p', '1920x1080': '1080p', '1280x720': '720p', '640x360': '360p' };
const resName = r => RES_NAME[(r || '').toLowerCase()] || (r || '').replace(/x/i, '×');
const mediaKind = s => s.media === 'application' ? 'metadata' : (s.media || 'stream');
const mimeSub = s => ((s.mime || '').split('/')[1] || '').toLowerCase();
/* Audio is named from its own mime (the request's options carry the video codec, not the audio one) */
const audioCodec = s => AUDIO_MIME[mimeSub(s)] || '';
function videoCodec(s) { const c = (s.codec || '').toLowerCase() || mimeSub(s); return CODEC[c] || (c ? c.toUpperCase() : ''); }
function streamLabel(s) {
	const k = mediaKind(s);
	if (k === 'video') return ['Video ' + resName(s.resolution), videoCodec(s), s.fps ? s.fps + ' fps' : ''].map(x => x.trim()).filter(Boolean).join(' · ');
	if (k === 'audio') return ['Audio', audioCodec(s)].filter(Boolean).join(' · ');
	if (k === 'metadata') return 'Metadata';
	return k[0].toUpperCase() + k.slice(1);
}
/* Everything about one stream, for the hover */
function streamDetail(s) {
	const k = mediaKind(s);
	return [k[0].toUpperCase() + k.slice(1), k === 'video' ? s.resolution : '', k === 'video' ? videoCodec(s) : k === 'audio' ? audioCodec(s) || s.mime : '', s.fps ? s.fps + ' fps' : '',
		[(s.protocol || '').toUpperCase(), (s.transport || '').toUpperCase()].filter(Boolean).join(' over ') + (s.multicast ? ' multicast' : ''), s.path || '', s.srcPort ? 'port ' + s.srcPort + (s.dstPort ? ' to ' + s.dstPort : '') : '',
		s.encrypted ? 'encrypted' : 'not encrypted', s.state || ''].filter(Boolean).join(' · ');
}
/* Several streams to one address in one line: the first video in full, the rest as short grey words ("audio", "metadata") */
function streamsSummary(ss) {
	const main = ss.find(s => mediaKind(s) === 'video') || ss[0], rest = ss.filter(s => s !== main), extra = [];
	const nv = rest.filter(s => mediaKind(s) === 'video').length;
	if (nv) extra.push('+' + nv + ' video');
	rest.forEach(s => { const k = mediaKind(s); if (k !== 'video' && !extra.includes(k)) extra.push(k); });
	return { main: streamLabel(main), extra };
}
const streamKey = s => [mediaKind(s), s.resolution || '', s.codec || '', mimeSub(s)].join(' ') + '|' + s.dst + (s.dstPort ? ':' + s.dstPort : '');
/* The player, shortened: "VLC/3.0.20 LibVLC/3.0.20" is "VLC 3.0.20"; the full text is in the hover */
function playerName(a) { const m = /^([^/]+)\/(\S+)/.exec(a || ''); return m ? m[1].trim() + ' ' + m[2] : (a || ''); }
/*---------------------------------------------------- Network › Connections */
/* Connections and streams that stop stay 60 s as "stale" (dimmed, 0, "Stopped … ago"), then go; one that comes back within
   60 s is live again in the same row. Keyed by service + other end (+ port for streams). Kept here in the page. */
const connSeen = new Map();
const STALE_S = 60;
function drawConns() {
	/* Each TCP connection's bytes, merged with the camera's own list of streams going to the same address;
	   streams with no byte count (UDP, multicast) are listed too, with Sent "–". One line per row; details on hover. */
	const conns = (S.topNet || []).filter(r => (r.tx || 0) + (r.rx || 0) > 0);
	const streams = (S.cam && S.cam.streams) || [];
	const used = new Set(), rows = [], merged = [];   /* merged: the stream keys now shown inside a connection's row */
	const tipOf = (ss, extra) => [...new Set(ss.map(streamDetail))].concat(ss.length && ss[0].agent ? ['Player: ' + ss[0].agent] : []).concat(extra || []).join('\n');
	conns.forEach(r => {
		const ss = streams.filter((s, i) => !used.has(i) && s.dst === r.peer && used.add(i));
		ss.forEach(s => merged.push(streamKey(s)));
		const sum = ss.length ? streamsSummary(ss) : { main: r.service, extra: [] };
		rows.push({ key: r.service + '|' + r.peer, service: sum.main, extra: sum.extra, peer: r.peer, player: ss.length ? playerName(ss[0].agent) : '', agent: ss.length ? ss[0].agent : '',
			state: ss.length ? ss[0].state : '', tip: tipOf(ss, [r.service + ' · ' + r.conns + (r.conns === 1 ? ' connection' : ' connections')]), tx: r.tx, rx: r.rx, conns: r.conns });
	});
	streams.forEach((s, i) => {
		if (used.has(i)) return;
		const peer = s.dst + (s.dstPort ? ':' + s.dstPort : '');
		rows.push({ key: streamKey(s), service: streamLabel(s), extra: [], peer, player: playerName(s.agent), agent: s.agent, state: s.state, tip: tipOf([s]), tx: null, rx: null, conns: null });
	});
	/* Live rows first, then the stale ones, most recently stopped first */
	const nowS = Date.now() / 1000, live = new Set(rows.map(r => r.key).concat(merged));
	merged.forEach(k => connSeen.delete(k));   /* a stream that joined its connection's row is not a stopped one */
	rows.forEach(r => connSeen.set(r.key, { row: r, at: nowS }));
	const stale = [];
	connSeen.forEach((v, k) => {
		if (live.has(k)) return;
		if (nowS - v.at > STALE_S) connSeen.delete(k);
		else stale.push(Object.assign({}, v.row, { stale: nowS - v.at, stoppedAt: v.at }));
	});
	stale.sort((a, b) => b.stoppedAt - a.stoppedAt);
	const all = rows.concat(stale), mx = Math.max(0.001, ...rows.map(r => r.tx || 0));
	const zero = '<div class="cellm"><span>0</span><div class="meter"></div></div>', tail = t => '<small class="cell-tail"> · ' + esc(t) + '</small>';
	$('#conn-rows').innerHTML = !S.now.netConnOk && !streams.length && !stale.length ? '<tr><td colspan="6" class="hint">This camera does not allow reading its connections.</td></tr>'
		: all.length ? all.map(r => '<tr' + (r.stale != null ? ' class="stale"' : '') + ' title="' + esc(r.stale != null ? 'Stopped at ' + hhmm(r.stoppedAt, 1) + '\n' + r.tip : r.tip) + '">' +
			'<td data-l="Service">' + esc(r.service) + r.extra.map(tail).join('') + '</td><td data-l="Other end">' + esc(r.peer) + '</td>' +
			'<td data-l="Player">' + esc(r.player || '–') + (r.stale != null ? tail('stopped ' + Math.round(r.stale) + ' s ago') : r.state ? tail(r.state) : '') + '</td>' +
			'<td class="bar-col" data-l="Sent">' + (r.stale != null ? zero : r.tx == null ? cellNone() : cellMeter(r.tx || 0, mx, false, fmtMbps(r.tx))) + '</td>' +
			'<td class="num" data-l="Received">' + (r.stale != null ? '0' : r.rx == null ? '–' : fmtMbps(r.rx)) + '</td><td class="num" data-l="Connections">' + (r.stale != null || r.conns == null ? '–' : r.conns) + '</td></tr>').join('')
		: '<tr><td colspan="6" class="hint">No connection is sending or receiving right now.</td></tr>';
}

/*---------------------------------------------------- CPU / Memory / Storage › Processes */
let procMode = 'cpu', procGroup = 'app', ioHist = null, ioHistAt = 0;   /* 1.5.0: Group by app first */
const PROC_COLS = {
	cpu: { title: 'Processes by CPU', sub: 'Share of the whole camera over the last 2 seconds; processes with the same name are added together.', key: 'cpu', head: 'CPU', fmt: v => fmtPct(v, v < 0.1 ? 2 : 1), extra: ['Threads', p => String(p.threads)] },
	mem: { title: 'Processes by memory', sub: 'Private memory of each process (resident, not shared); processes with the same name are added together.', key: 'memKb', head: 'Memory', fmt: fmtKb, extra: ['Running for', p => fmtAge(p.ageS)] },
	io: { title: 'Processes waiting on the disk', sub: 'Programs found waiting on the disk (state D) in the process scans over the last hour, and how long they were blocked.', key: 'io', head: 'Waiting on the disk, last hour', fmt: v => 'blocked ' + (v < 10 ? v.toFixed(1) : Math.round(v)) + ' s', extra: null }
};
async function pollProcs() {
	clearTimeout(procTimer);
	if (tab !== 'procs') return;
	try {
		if (procMode === 'io') {
			if (Date.now() - ioHistAt > 30000) { ioHist = await getJSON('history?seconds=3600&points=20'); ioHistAt = Date.now(); }
		} else S.procs = await getJSON('processes');
		drawProcs();
	} catch (e) { $('#p-rows').innerHTML = '<tr><td colspan="4" class="hint">Could not read the processes: ' + esc(e.message) + '</td></tr>'; }
	procTimer = setTimeout(pollProcs, 2000);
}
/* The rows for the mode: [{ name, app, v, extra }] */
function procRows() {
	const m = PROC_COLS[procMode];
	if (procMode === 'io') {
		const by = {};
		((ioHist && ioHist.points) || []).forEach(p => (p.ioTop || []).forEach(e => { const k = e[0] + '\u0000' + e[1]; by[k] = by[k] || { name: e[0], app: e[1], v: 0 }; by[k].v += e[2]; }));
		return Object.values(by).sort((a, b) => b.v - a.v);
	}
	return ((S.procs && S.procs.list) || []).map(p => ({ name: p.name, app: p.app, v: p[m.key] || 0, extra: m.extra[1](p), count: p.count })).sort((a, b) => b.v - a.v);
}
function drawProcs() {
	const m = PROC_COLS[procMode], n = S.now, s = S.last || {};
	$('#pr-title').textContent = m.title;
	$('#pr-sub').textContent = m.sub;
	$('#pr-group').hidden = procMode === 'io';
	$$('#pr-group button').forEach(b => b.classList.toggle('on', b.dataset.g === procGroup));
	/* The split at the top: Apps, Camera, Idle (CPU) or Video memory and Available (memory) */
	const used = n.memTotalKb - n.memAvailKb, vid = n.videoUsedKb > 0 ? Math.min(n.videoUsedKb, Math.max(0, used - n.appsMemKb)) : 0;
	$('#pr-split').innerHTML = procMode === 'cpu' ? compBar('Where the CPU goes', [['Apps', n.appsCpu, 'app'], ['Camera', Math.max(0, num(s.cpu) - num(n.appsCpu)), 'cam'], ['Idle', Math.max(0, 100 - num(s.cpu)), 'free']], v => fmtPct(v, 0))
		: procMode === 'mem' ? compBar('Where the memory goes', [['Apps', n.appsMemKb, 'app'], ['Camera', Math.max(0, used - n.appsMemKb - vid), 'cam'], ['Video memory', vid, 'video'], ['Available', n.memAvailKb, 'free']], fmtKb) : '';
	const q = $('#p-find').value.trim().toLowerCase();
	const keep = p => (pWho === 'all' || (pWho === 'app') === ownApp(p.app)) && (!q || (p.name + ' ' + appName(p.app)).toLowerCase().includes(q));
	let all = procRows();
	const group = procGroup === 'app' && procMode !== 'io';
	if (group) {
		/* Group by app: each installed app, and the camera */
		const by = {};
		all.forEach(p => { const k = ownApp(p.app) ? p.app : ''; by[k] = by[k] || { name: k ? appName(k) : 'Camera (AXIS OS and Axis apps)', app: k, v: 0, n: 0 }; by[k].v += p.v; by[k].n += p.count || 1; });
		all = Object.values(by).sort((a, b) => b.v - a.v);
	}
	const cols = group ? '<col><col class="c-bar"><col class="c-n">' : m.extra ? '<col><col class="c-own"><col class="c-bar"><col class="c-n">' : '<col><col class="c-own"><col class="c-bar">';
	$('#pr-cols').innerHTML = cols;
	$('#pr-head').innerHTML = '<tr>' + (group ? '<th>App</th>' : '<th>Process</th><th>Belongs to</th>') + '<th class="bar-col">' + esc(m.head) + ' ▾</th>' + (group ? '<th class="num">Processes</th>' : m.extra ? '<th class="num">' + esc(m.extra[0]) + '</th>' : '') + '</tr>';
	const ncol = group ? 3 : m.extra ? 4 : 3;
	const list = all.filter(p => group ? (pWho === 'all' || (pWho === 'app') === !!p.app) && (!q || p.name.toLowerCase().includes(q)) : keep(p));
	const mx = Math.max(procMode === 'cpu' ? 10 : procMode === 'mem' ? 1024 : 1, ...all.map(p => p.v));
	if (group) {
		$('#p-rows').innerHTML = list.length ? list.map(p => '<tr><td data-l="App"><span class="' + (p.app ? 'owner-app' : 'owner-cam') + '">' + esc(p.name) + '</span></td><td class="bar-col" data-l="' + esc(m.head) + '">' + cellMeter(p.v, mx, !!p.app, m.fmt(p.v)) + '</td><td class="num" data-l="Processes">' + p.n + '</td></tr>').join('')
			: '<tr><td colspan="3" class="hint">No app matches.</td></tr>';
		return;
	}
	const top = list.slice(0, 10), others = list.slice(10);
	const open = otherOpen === null ? (!!q && others.length > 0) : otherOpen;
	const row = (p, sub) => '<tr' + (sub ? ' class="sub"' : '') + '><td data-l="Process"><span class="' + (ownApp(p.app) ? 'owner-app' : '') + '">' + esc(p.name) + '</span></td><td data-l="Belongs to" class="' + (ownApp(p.app) ? 'owner-app' : 'owner-cam') + '">' + esc(appName(p.app)) + '</td>' +
		'<td class="bar-col" data-l="' + esc(m.head) + '">' + cellMeter(p.v, mx, ownApp(p.app), m.fmt(p.v)) + '</td>' + (m.extra ? '<td class="num" data-l="' + esc(m.extra[0]) + '">' + esc(p.extra) + '</td>' : '') + '</tr>';
	const sum = others.reduce((a, p) => a + p.v, 0);
	const otherRow = others.length ? '<tr class="other" id="other-row" role="button" tabindex="0" aria-expanded="' + open + '" title="' + (open ? 'Hide' : 'Show') + ' every process in Other">' +
		'<td data-l="Process"><span class="o-chev" aria-hidden="true">' + (open ? '▾' : '▸') + '</span>Other (' + others.length + ' processes)</td><td data-l="Belongs to">' + (pWho === 'app' ? 'Apps' : 'Camera and apps') + '</td>' +
		'<td class="bar-col" data-l="' + esc(m.head) + '">' + cellMeter(sum, Math.max(mx, sum), false, m.fmt(sum)) + '</td>' + (m.extra ? '<td class="num o-cell"><span class="o-act">' + (open ? 'Hide' : 'Show all') + '</span></td>' : '') + '</tr>' : '';
	$('#p-rows').innerHTML = list.length ? top.map(p => row(p)).join('') + otherRow + (open ? others.map(p => row(p, 1)).join('') : '')
		: '<tr><td colspan="' + ncol + '" class="hint">' + (procMode === 'io' ? 'Nothing was found waiting on the disk in the last hour.' : 'No process matches.') + '</td></tr>';
	const o = $('#other-row');
	if (o) {
		o.onclick = () => { otherOpen = !open; drawProcs(); };
		o.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); o.onclick(); } };
	}
}

/*---------------------------------------------------- Apps and the log */
let appsTimer = null, logTimer = null;
async function loadApps() {
	const d = await getJSON('apps');
	S.appList = d.apps || [];
	S.apps = {};
	S.appList.forEach(a => { S.apps[a.id] = a; });
	return S.appList;
}
async function pollApps() {
	clearTimeout(appsTimer);
	try { await loadApps(); if (tab === 'apps') drawApps(); } catch (e) { if (tab === 'apps') $('#a-rows').innerHTML = '<tr><td colspan="6" class="hint">Could not read the app list: ' + esc(e.message) + '</td></tr>'; }
	appsTimer = setTimeout(pollApps, tab === 'apps' ? 3000 : 30000);
}
function drawApps() {
	$('#a-rows').innerHTML = S.appList.length ? S.appList.map(a => {
		const run = a.status === 'Running', u = a.use || {}, cls = run ? 'ok' : a.status === 'Stopped' ? 'idle' : 'error';
		const btns = a.self ? '<button type="button" class="btn" disabled title="Resource Monitor can\'t stop itself">Stop</button><button type="button" class="btn" disabled>Restart</button>'
			: '<button type="button" class="btn admin-only" data-ask="' + (run ? 'stop' : 'start') + '" data-id="' + esc(a.id) + '"' + (isAdmin() ? '' : ' disabled title="Admin only"') + '>' + (run ? 'Stop' : 'Start') + '</button><button type="button" class="btn admin-only" data-ask="restart" data-id="' + esc(a.id) + '"' + (run && isAdmin() ? '' : ' disabled') + (isAdmin() ? '' : ' title="Admin only"') + '>Restart</button>';
		return '<tr><td data-l="App"><b>' + esc(a.name) + '</b>' + (a.self ? '<span class="this-app">this app</span>' : '') + (a.axis ? '<span class="this-app axis-tag" title="Made by Axis">Axis</span>' : '') + ' <span class="info-i" tabindex="0" title="' + esc('Made by ' + (a.vendor || 'unknown') + ' · ' + a.id) + '">i</span></td>' +
			'<td data-l="Version">' + esc(a.version) + '</td><td data-l="Status"><span class="status ' + cls + '">' + esc(a.status || 'Unknown') + '</span></td>' +
			'<td class="num" data-l="On flash">' + (a.diskKb ? fmtKb(a.diskKb) : '–') + '</td><td class="num" data-l="Running for">' + (run && u.ageS ? fmtAge(u.ageS) : '–') + '</td>' +
			'<td data-l=""><div class="row-actions">' + btns + '<button type="button" class="btn" data-log="' + esc(a.name) + '">Log</button></div></td></tr>';
	}).join('') : '<tr><td colspan="6" class="hint">No apps found.</td></tr>';
	$$('[data-ask]').forEach(b => { b.onclick = () => askApp(b.dataset.ask, S.apps[b.dataset.id]); });
	$$('[data-log]').forEach(b => { b.onclick = () => { go('apps', 'log'); fillAppFilter(); $('#f-app').value = b.dataset.log; drawLog(); }; });
}
let askOk = null;
function ask(title, text, okText, onOk, showLogNote) {
	$('#da-h').textContent = title; $('#da-p').textContent = text; $('#da-ok').textContent = okText;
	$('#da-p2').hidden = !showLogNote;
	askOk = onOk; $('#dlg-ask').hidden = false; $('#da-ok').focus();
}
function askApp(action, a) {
	if (!a) return;
	const W = { stop: 'Stop', start: 'Start', restart: 'Restart' }[action];
	const text = action === 'stop' ? 'The app stops until someone starts it again. Its events stop too, so rules in other apps that listen to it will show Problem.'
		: action === 'restart' ? 'The app stops and starts again, which takes a few seconds. Its events pause meanwhile.' : 'The app starts and begins using CPU and memory again.';
	ask(W + ' ' + a.name + '?', text, W + ' app', async () => {
		const m = $('#a-msg');
		m.hidden = false; m.className = 'msg'; m.textContent = W + 'ing ' + a.name + '…';
		try {
			const r = await postJSON('control', { action: action, package: a.id });
			m.className = 'msg ' + (r.ok ? 'ok' : 'bad'); m.textContent = a.name + ': ' + r.message;
		} catch (e) { m.className = 'msg bad'; m.textContent = a.name + ': ' + e.message; }
		pollApps(); pollLog();
	}, true);
}
async function pollLog() {
	clearTimeout(logTimer);
	if (tab !== 'log') return;
	try { S.log = (await getJSON('log')).lines || []; drawLog(); } catch (e) { /* keep the last lines */ }
	logTimer = setTimeout(pollLog, 5000);
}
function fillAppFilter() {
	const f = $('#f-app'), cur = f.value, names = [];
	S.log.forEach(l => { if (!names.includes(l.app)) names.push(l.app); });
	S.appList.forEach(a => { if (!names.includes(a.name)) names.push(a.name); });
	f.innerHTML = '<option value="">All apps</option>' + names.map(n => '<option' + (n === cur ? ' selected' : '') + '>' + esc(n) + '</option>').join('');
}
function logRows() {
	const ft = $('#f-time').value.trim(), fa = $('#f-app').value, fw = $('#f-what').value.trim().toLowerCase(), fu = $('#f-who').value.trim().toLowerCase();
	return S.log.filter(l => (!ft || hhmm(l.t, 1).includes(ft)) && (!fa || l.app === fa) && (!fw || l.what.toLowerCase().includes(fw)) && (!fu || l.who.toLowerCase().includes(fu)));
}
function drawLog() {
	fillAppFilter();
	const rows = logRows();
	$('#log-rows').innerHTML = rows.length ? rows.map(l => '<tr><td data-l="Time" class="mono" title="' + esc(new Date(l.t * 1000).toLocaleString()) + '">' + hhmm(l.t, 1) + '</td><td data-l="App">' + esc(l.app) + '</td><td data-l="What happened">' + esc(l.what) + '</td><td data-l="Who">' + esc(l.who) + '</td></tr>').join('')
		: '<tr><td colspan="4" class="hint">' + (S.log.length ? 'No lines match the filters.' : 'Nothing logged yet.') + '</td></tr>';
}
function exportLog() {
	const q = v => '"' + String(v).replace(/"/g, '""') + '"';
	const csv = ['Time,App,What happened,Who'].concat(S.log.map(l => [new Date(l.t * 1000).toISOString(), l.app, l.what, l.who].map(q).join(','))).join('\r\n');
	const a = document.createElement('a'), d = new Date(), p = n => (n < 10 ? '0' : '') + n;
	a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
	a.download = 'resource-monitor-log-' + (S.device.serial || 'camera') + '-' + d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + '.csv';
	document.body.appendChild(a); a.click(); a.remove();
	setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/*---------------------------------------------------- Settings (1.3.0): rows, saved with Save (nothing to confirm) */
/* Groups of readings, as the camera side numbers them; a group neither shown nor sent is not read at all */
const GB = { cpu: 1, mem: 2, flash: 4, net: 8, temp: 16, power: 32, dlpu: 64, procs: 128, conns: 256, events: 512 };
const shown = b => !S.now || !S.now.settings || (S.now.settings.collect & b) !== 0;   /* b: a GB bit */
const GROUPS = [
	['cpu', 'CPU, each core, kind of work, clock', '/proc/stat, cpufreq', 'always'],
	['mem', 'Memory, video memory, swap', '/proc/meminfo', 'always'],
	['storage', 'Storage, drives and card health', 'mounts, diskstats, the camera\'s disk list'],
	['net', 'Network, every interface', '/proc/net/dev'],
	['temp', 'Temperature, fan, heater', 'thermal zones, the camera\'s temperature service'],
	['power', 'Power (PoE)', 'the camera\'s power settings service'],
	['dlpu', 'AI chip', '/proc/interrupts'],
	['procs', 'Processes (top 10 at each moment)', '/proc', 'never'],
	['conns', 'Network per connection, and streams', 'TCP table, the camera\'s stream list', 'never'],
];
let setData = null, setForm = null;                     /* as loaded from the camera; as on the page now */
const daysName = d => d === 30 ? '30 days' : d === 7 ? '7 days' : '24 h';
const fmtMB = b => (b / 1048576 < 10 ? (b / 1048576).toFixed(1) : Math.round(b / 1048576)) + ' MB';
async function loadSettings() {
	try {
		setData = await getJSON('config');
		setForm = JSON.parse(JSON.stringify(setData.settings));
		drawSettings();
	} catch (e) { $('#s-msg').textContent = 'Could not read the settings: ' + e.message; }
}
/* "auto" shows as the source it picks now */
function powerPick(s, log) { return s.powerSource !== 'auto' ? s.powerSource : log === 'no' ? 'app' : 'camera'; }
function drawSettings() {
	if (!setData) return;
	const f = setForm, st = setData.status || {}, h = st.history || {}, log = st.powerLog, card = h.card;
	/* Display */
	$('#s-temp').value = f.tempUnit === 'F' ? 'F' : 'C';
	/* Power */
	const pick = powerPick(f, log);
	$('#s-power').innerHTML = [['camera', 'Camera\'s power log (recommended)', log === 'no'], ['app', 'Recorded by Resource Monitor'], ['off', 'Off']]
		.map(o => '<option value="' + o[0] + '"' + (o[0] === pick ? ' selected' : '') + (o[2] ? ' disabled' : '') + '>' + esc(o[2] ? 'Camera\'s power log (this camera keeps none)' : o[1]) + '</option>').join('');
	$('#s-power-note').textContent = '';   /* 1.6.4: what each choice does is in the row's (i) */
	const ports = (S.now.power && S.now.power.kind === 'ports' && S.now.power.ports) || [];
	$('#s-ports-row').hidden = !ports.length || pick === 'off';
	$('#s-ports').innerHTML = ports.map(p => '<label class="set-port"><input type="checkbox" data-port="' + p.port + '"' + ((f.poePorts || []).includes(p.port) ? ' checked' : '') + '> Port ' + p.port + '</label>').join('');
	/* History */
	const okCard = card === 'ok', tiers = h.tiers || [], bytes = d => tiers.slice(0, d >= 30 ? 3 : d >= 7 ? 2 : 1).reduce((a, t) => a + (t.bytes || 0), 0);
	$$('#s-days option').forEach(o => { const d = +o.value; o.disabled = d > 1 && !okCard; o.selected = d === f.historyDays; o.textContent = daysName(d) + (d > 1 && !okCard ? ' (needs an SD card)' : okCard ? ' (about ' + fmtMB(bytes(d)) + ' on the card)' : ''); });
	/* 1.6.4: only what is true now stays here (a save that deletes, why there is no card); the rest is in the row's (i) */
	$('#s-days-note').textContent = okCard ? (f.historyDays < setData.settings.historyDays ? 'Saving deletes history older than ' + daysName(f.historyDays) + ' from the card.' : '') : (h.why || '');
	const where = h.where || (okCard ? 'card' : 'memory');
	$('#s-where').innerHTML = where === 'card' ? '<span class="st-ok">SD card</span> <span class="set-note">survives restarts · written once a minute</span>'
		: where === 'flash' ? '<span class="st-ok">Camera\'s built-in flash, 24 h, kept across restarts</span> <span class="set-note">written once a minute' + (h.flashBytes ? ', about ' + fmtMB(h.flashBytes) : '') + '; 7 and 30 days need an SD card</span>'
		: '<span class="st-mem">Memory only</span> <span class="set-note">24 h, lost when the app restarts' + (h.flashWhy ? ' · ' + esc(h.flashWhy) : h.why ? ' · ' + esc(h.why) : '') + '</span>';
	/* Send out: the Prometheus page */
	$('#s-prom').checked = !!f.prometheus;
	$('#s-prom-info').innerHTML = f.prometheus ? '<code>/local/resource_monitor/metrics</code> <span class="set-note">viewer login needed to read</span>' : '<span class="set-note">Off: nothing is sent out.</span>';
	$('#s-prom-test').hidden = !f.prometheus;
	drawOut(f);
	/* What to collect */
	$('#s-collect').innerHTML = GROUPS.map(g => {
		const c = (f.collect || {})[g[0]] || {}, always = g[3] === 'always', never = g[3] === 'never';
		const sw = (kind, on, dis, why) => '<label class="switch" title="' + esc(why || '') + '"><input type="checkbox" data-g="' + g[0] + '" data-kind="' + kind + '"' + (on ? ' checked' : '') + (dis ? ' disabled' : '') + ' aria-label="' + esc(kind + ' ' + g[1]) + '"><span class="track"></span></label>';
		return '<tr><td data-l="Metric">' + esc(g[1]) + '</td><td data-l="Show">' + sw('show', always || c.show !== false, always, always ? 'Always on: the other figures need it' : '') + '</td>' +
			'<td data-l="Send">' + sw('send', !never && g[3] !== 'nometric' && !!c.send, never || g[3] === 'nometric', never ? 'Never sent out' : g[3] === 'nometric' ? 'Not on the Prometheus page' : 'On the Prometheus page') + '</td><td data-l="Read from" class="set-note">' + esc(g[2]) + '</td></tr>';
	}).join('');
	setDirty();
	applyAccess();
}
function readForm() {
	const f = setForm;
	const p = $('#s-power').value;
	f.powerSource = setData.settings.powerSource === 'auto' && p === powerPick(setData.settings, setData.status.powerLog) ? 'auto' : p;
	f.historyDays = +$('#s-days').value;
	f.tempUnit = $('#s-temp').value;
	f.poePorts = $$('#s-ports input:checked').map(i => +i.dataset.port);
	f.prometheus = $('#s-prom').checked;
	readOut(f);
	$$('#s-collect input[data-kind="send"]:not(:disabled)').forEach(i => { f.collect[i.dataset.g] = Object.assign({}, f.collect[i.dataset.g], { send: i.checked }); });
	$$('#s-collect input[data-kind="show"]').forEach(i => { f.collect[i.dataset.g] = Object.assign({}, f.collect[i.dataset.g], { show: i.checked }); });
}
const settingsDirty = () => !!setData && !!setForm && JSON.stringify(setForm) !== JSON.stringify(setData.settings);
function setDirty() {
	const dirty = settingsDirty();
	$('#s-save').disabled = !dirty; $('#s-cancel').disabled = !dirty;
	$('#s-bar').classList.toggle('show', dirty || $('#s-msg').textContent.startsWith('Not saved'));
	$('#set-state').textContent = dirty ? 'Not saved yet' : 'Saved on the camera';
}
/* Test: read the Prometheus page as a scraper would, and say what came back */
async function testMetrics() {
	const m = $('#s-prom-msg');
	if (JSON.stringify(setForm) !== JSON.stringify(setData.settings)) { m.textContent = 'Save first, then test.'; return; }
	m.textContent = 'Reading…';
	try {
		const r = await fetch('metrics', { cache: 'no-store', credentials: 'same-origin' });
		const t = await r.text();
		if (!r.ok) { m.textContent = 'Not working: ' + r.status + ' ' + (t || r.statusText).slice(0, 160); return; }
		if (r.headers.get('X-Resource-Monitor') === 'off') { m.textContent = 'Off on the camera: ' + t.replace(/^#\s*/, '').trim(); return; }
		const n = t.split('\n').filter(l => l && l[0] !== '#').length;
		m.textContent = 'OK, ' + n + ' metrics' + ((r.headers.get('Content-Type') || '').includes('version=0.0.4') ? '' : ' (but not marked as the Prometheus text format)') + '.';
	} catch (e) { m.textContent = 'Not working: ' + e.message; }
}
async function saveSettings() {
	$('#s-save').disabled = true;
	try {
		const r = await postJSON('configset', setForm);
		if (!r.ok) { $('#s-msg').textContent = r.error || 'Not saved.'; setDirty(); return; }
		setData = r; setForm = JSON.parse(JSON.stringify(r.settings));
		$('#s-msg').textContent = 'Saved.';
		drawSettings();
	} catch (e) { $('#s-msg').textContent = 'Not saved: ' + e.message; setDirty(); }
}

/*---------------------------------------------------- 1.3.3: Send out over MQTT and to InfluxDB */
/* Passwords and tokens: the page never gets them back, only whether one is set; one typed here replaces it */
const OUT_ST = { off: 'st-off', connecting: 'st-wait', connected: 'st-ok', sending: 'st-ok', failing: 'st-bad' };
function drawOutStatus() {
	const so = (setData && setData.status && setData.status.sendOut) || {};
	[['mqtt', '#s-mq-st'], ['influx', '#s-ix-st']].forEach(([k, id]) => {
		const s = so[k] || { state: 'off', text: 'Off' };
		const el = $(id);
		el.className = 'out-st ' + (OUT_ST[s.state] || '');
		el.textContent = s.text;
	});
}
function drawOut(f) {
	const serial = (S.device && S.device.serial) || '<serial>';
	const m = f.mqtt || {}, x = f.influx || {};
	$('#s-mq-on').checked = !!m.on;
	$$('#tab-settings .mq-row').forEach(r => { r.hidden = !m.on; });
	$('#s-mq-host').value = m.host || ''; $('#s-mq-port').value = m.port || (m.tls ? 8883 : 1883);
	$('#s-mq-tls').checked = !!m.tls; $('#s-mq-check').checked = !!m.checkName; $('#s-mq-check').disabled = !m.tls;
	$$('#tab-settings .mq-tls').forEach(r => { r.hidden = !m.on || !m.tls; });
	$('#s-mq-ca').value = m.ca || '';
	$('#s-mq-user').value = m.user || '';
	$('#s-mq-pass').value = m.password || '';
	$('#s-mq-pass').placeholder = m.passwordSet && m.password === undefined ? 'Password set: type to replace' : 'Password (optional)';
	$('[data-clearpw="mqtt"]').hidden = !m.passwordSet || m.password !== undefined;
	$('#s-mq-id').value = m.clientId || ''; $('#s-mq-id').placeholder = 'resource_monitor-' + serial;
	$('#s-mq-prefix').value = m.prefix || ''; $('#s-mq-prefix').placeholder = 'axis/' + serial + '/resource_monitor';
	$('#s-mq-every').value = m.interval || 30;
	$('#s-mq-ha').checked = !!m.haDiscovery;
	$('#s-ix-on').checked = !!x.on;
	$$('#tab-settings .ix-row').forEach(r => { r.hidden = !x.on; });
	const v = x.version === 1 ? 1 : 2;
	$$('#tab-settings .ix-v1').forEach(r => { r.hidden = !x.on || v !== 1; });
	$$('#tab-settings .ix-v2').forEach(r => { r.hidden = !x.on || v !== 2; });
	$('#s-ix-url').value = x.url || 'https://'; $('#s-ix-ver').value = String(v);
	$('#s-ix-db').value = x.database || ''; $('#s-ix-user').value = x.user || '';
	$('#s-ix-pass').value = x.password || '';
	$('#s-ix-pass').placeholder = x.passwordSet && x.password === undefined ? 'Password set: type to replace' : 'Password (optional)';
	$('[data-clearpw="influx-pass"]').hidden = !x.passwordSet || x.password !== undefined;
	$('#s-ix-org').value = x.org || ''; $('#s-ix-bucket').value = x.bucket || '';
	$('#s-ix-token').value = x.token || '';
	$('#s-ix-token').placeholder = x.tokenSet && x.token === undefined ? 'Token set: type to replace' : 'Token';
	$('[data-clearpw="influx-token"]').hidden = !x.tokenSet || x.token !== undefined;
	$('#s-ix-check').checked = x.checkName !== false;
	$('#s-ix-ca').value = x.ca || '';
	$('#s-ix-every').value = x.interval || 30;
	drawOutStatus();
}
function readOut(f) {
	const m = f.mqtt = Object.assign({}, f.mqtt), x = f.influx = Object.assign({}, f.influx);
	const tlsWas = m.tls;
	m.on = $('#s-mq-on').checked; m.host = $('#s-mq-host').value.trim();
	m.tls = $('#s-mq-tls').checked; m.checkName = $('#s-mq-check').checked;
	m.port = +$('#s-mq-port').value || (m.tls ? 8883 : 1883);
	if (m.tls !== tlsWas && (m.port === 8883 || m.port === 1883)) m.port = m.tls ? 8883 : 1883;   /* the usual port follows TLS */
	m.ca = $('#s-mq-ca').value.trim(); m.user = $('#s-mq-user').value.trim();
	if ($('#s-mq-pass').value) m.password = $('#s-mq-pass').value;
	m.clientId = $('#s-mq-id').value.trim(); m.prefix = $('#s-mq-prefix').value.trim();
	m.interval = +$('#s-mq-every').value || 30; m.haDiscovery = $('#s-mq-ha').checked;
	x.on = $('#s-ix-on').checked; x.url = $('#s-ix-url').value.trim(); x.version = +$('#s-ix-ver').value;
	x.database = $('#s-ix-db').value.trim(); x.user = $('#s-ix-user').value.trim();
	if ($('#s-ix-pass').value) x.password = $('#s-ix-pass').value;
	x.org = $('#s-ix-org').value.trim(); x.bucket = $('#s-ix-bucket').value.trim();
	if ($('#s-ix-token').value) x.token = $('#s-ix-token').value;
	x.checkName = $('#s-ix-check').checked; x.ca = $('#s-ix-ca').value.trim(); x.interval = +$('#s-ix-every').value || 30;
}
/* Test: the saved settings, now (a short second connection; the running one is not disturbed) */
async function testOut(what) {
	const m = $(what === 'mqtt' ? '#s-mq-msg' : '#s-ix-msg');
	if (JSON.stringify(setForm) !== JSON.stringify(setData.settings)) { m.textContent = 'Save first, then test.'; return; }
	m.textContent = 'Trying…';
	try { const r = await postJSON('sendtest', { what }); m.textContent = r.message || (r.ok ? 'OK' : 'Not working'); }
	catch (e) { m.textContent = 'Not working: ' + e.message; }
}
/* The status lines follow while Settings is open */
let outTimer = null;
async function pollOutStatus() {
	clearTimeout(outTimer);
	if (tab !== 'settings') return;
	try { const r = await getJSON('config'); if (setData && r && r.status) { setData.status.sendOut = r.status.sendOut; drawOutStatus(); } } catch (e) { /* next time */ }
	outTimer = setTimeout(pollOutStatus, 5000);
}

/*---------------------------------------------------- 1.4.0 Alerts: a reading past its limit for a while sends a camera event */
/* 1.6.1: one plain line per alert ("CPU busy above 90 % for 1 min"); Edit and + Add alert open a dialog that saves at once.
   No approval step. */
const AL = { data: null, edit: null, logId: null, timer: null };
const isOperator = () => me.level === 'admin' || me.level === 'operator' || me.level === 'unknown';
const AL_ST = { Listening: 'ok', Triggered: 'triggered', Waiting: 'warn', Rejected: 'idle', Disabled: 'idle', Problem: 'error' };
/* 1.5.0 Send to: where each alert goes. Saved without it (before 1.5.0): the camera event only */
function alTo(a) { const t = a.to || { event: true }; return { event: !!t.event, mqtt: !!t.mqtt, influx: !!t.influx, prom: !!t.prom }; }
const AL_TO = [['event', 'Camera event'], ['mqtt', 'MQTT'], ['influx', 'InfluxDB'], ['prom', 'Prometheus']];
const AL_OFF = { mqtt: 'MQTT is off in Settings', influx: 'InfluxDB is off in Settings', prom: 'The Prometheus page is off in Settings' };
const alOff = k => k !== 'event' && !!(AL.data && AL.data.ways) && !AL.data.ways[k];
const alMetric = k => (AL.data && AL.data.metrics.find(m => m.key === k)) || { name: k, unit: '' };
function alFmt(m, v) { v = num(v); if (isNaN(v)) return '–'; const u = alMetric(m).unit; return (Math.abs(v) >= 100 || v === Math.floor(v) ? v.toFixed(0) : v.toFixed(1)) + (u ? ' ' + u : ''); }
const alFor = s => (s = +s || 0) >= 60 && s % 60 === 0 ? s / 60 + ' min' : s + ' s';
const alRule = a => alMetric(a.metric).name + ' ' + (a.op === 'below' ? 'below' : 'above') + ' ' + alFmt(a.metric, a.value) + ' for ' + alFor(a.for);
/* The alert as the camera takes it (approval is never asked for any more) */
const alOut = a => ({ id: a.id || undefined, name: a.name, on: !!a.on, metric: a.metric, op: a.op, value: a.value === '' || a.value == null ? null : +a.value, for: a.for === '' || a.for == null ? null : +a.for, approval: false, to: alTo(a), context: a.context && a.context.text ? a.context : undefined });
function initAlerts() {
	$('#al-add').onclick = () => alEdit(null);
	$('#al-log-close').onclick = () => { AL.logId = null; $('#al-log').hidden = true; };
	$('#al-log-clear').onclick = () => { ['#al-f-time', '#al-f-what', '#al-f-sent', '#al-f-who'].forEach(s => { $(s).value = ''; }); drawAlertLog(); };
	['#al-f-time', '#al-f-what', '#al-f-sent', '#al-f-who'].forEach(s => { $(s).oninput = () => drawAlertLog(); });
	const rows = $('#al-rows');
	/* The Enabled switch on a line saves that change at once */
	rows.addEventListener('change', e => {
		if (e.target.dataset.f !== 'on') return;
		const a = alById(e.target.closest('tr[data-id]').dataset.id);
		if (a) saveAlerts(AL.data.alerts.map(x => x === a ? Object.assign({}, x, { on: e.target.checked }) : x), a.name + (e.target.checked ? ': on.' : ': off.'));
	});
	rows.addEventListener('click', e => {
		const b = e.target.closest('button[data-a]');
		if (!b) return;
		const a = alById(b.closest('tr[data-id]').dataset.id);
		if (!a) return;
		if (b.dataset.a === 'edit') alEdit(a);
		else if (b.dataset.a === 'log') openAlertLog(a);
		else if (b.dataset.a === 'test') actAlert(a, 'test');
	});
	/* The dialog */
	$('#ae-cancel').onclick = alClose;
	$('#dlg-alert').onclick = e => { if (e.target.id === 'dlg-alert') alClose(); };
	/* Esc closes the top dialog only: the Context editor sits over Edit alert */
	document.addEventListener('keydown', e => {
		if (e.key !== 'Escape') return;
		if (!$('#dlg-ctx').hidden) ctxClose(false); else if (!$('#dlg-cfg').hidden) cpClose(); else alClose();
	});
	initAlerts163();
	$('#ae-metric').onchange = () => { $('#ae-unit').textContent = alMetric($('#ae-metric').value).unit; alNowHint(); };
	$('#ae-to').onclick = e => { const c = e.target.closest('button[data-to]'); if (c) { c.classList.toggle('on'); c.setAttribute('aria-pressed', c.classList.contains('on')); } };
	$('#ae-save').onclick = alSaveEdit;
	$('#ae-del').onclick = () => {
		const a = AL.edit;
		if (!a || !a.id) return;
		alClose();
		ask('Delete ' + (a.name || 'this alert') + '?', 'It stops at once and its camera event is removed. Rules that use the event will show Problem.', 'Delete alert',
			() => saveAlerts(AL.data.alerts.filter(x => x.id !== a.id), (a.name || 'Alert') + ': deleted.'), true);
	};
	pollAlerts();
}
const alById = id => AL.data && AL.data.alerts.find(x => x.id === id);
async function pollAlerts() {
	clearTimeout(AL.timer);
	try {
		AL.data = await getJSON('alerts');
		if (tab === 'alerts') { drawAlerts(); drawAlWf(); } else drawAlertsTile();
		if (AL.logId) drawAlertLog();
	} catch (e) { if (tab === 'alerts' && !AL.data) $('#al-rows').innerHTML = '<tr><td colspan="5" class="hint">Could not read the alerts: ' + esc(e.message) + '</td></tr>'; }
	AL.timer = setTimeout(pollAlerts, tab === 'alerts' ? 2000 : 15000);
}
function drawAlertsTile() {
	const b = $('.rb-item[data-k="alerts"]');
	if (!b) return;
	const l = (AL.data && AL.data.alerts) || [];
	const on = l.filter(a => a.on).length, trig = l.filter(a => a.status && a.status.word === 'Triggered').length;
	$('.rb-t', b).textContent = !AL.data ? '…' : !l.length ? 'None yet' : trig ? trig + ' triggered' : on + ' on';
	b.title = 'Camera events a VMS can use when a reading passes its limit.';
}
function alStatusHtml(a) {
	const st = a.status || {}, w = st.word || 'Listening', since = st.since ? new Date(st.since * 1000).toLocaleString() : '';
	const tip = w === 'Listening' ? 'Watching ' + alMetric(a.metric).name + '; now ' + alFmt(a.metric, st.now) + '.'
		: w === 'Triggered' ? 'Sent On since ' + since + '. Now ' + alFmt(a.metric, st.now) + '.'
		: w === 'Disabled' ? 'Turned off: its event is not published.' : (st.problem || 'Something is wrong.');
	return '<div class="al-st"><span class="status ' + (AL_ST[w] || 'idle') + '" title="' + esc(tip) + '">' + esc(w) + '</span>' +
		(st.now != null && w !== 'Disabled' ? '<span class="al-now">now ' + esc(alFmt(a.metric, st.now)) + '</span>' : '') +
		(w === 'Problem' && st.problem ? '<span class="al-now">' + esc(st.problem) + '</span>' : '') + '</div>';   /* 1.6.5: shown, not in a hover */
}
function drawAlerts() {
	drawAlertsTile();
	if (!AL.data) return;
	const admin = isAdmin(), op = isOperator(), dis = admin ? '' : ' disabled';
	$('#al-add').hidden = !admin; $('#al-add').disabled = AL.data.alerts.length >= 20;
	$$('#tab-alerts .admin-hint').forEach(h => { h.hidden = admin; });
	$('#al-rows').innerHTML = AL.data.alerts.length ? AL.data.alerts.map(a => {
		const to = alTo(a), ways = AL_TO.filter(t => to[t[0]]);
		const toTxt = ways.length ? ways.map(t => alOff(t[0]) ? '<span class="off" title="' + esc(AL_OFF[t[0]] + ': nothing goes out until it is on') + '">' + t[1] + ' (off in Settings)</span>' : t[1]).join(', ') : '<span class="off">Nowhere</span>';
		const topic = a.status && a.status.topic;
		return '<tr data-id="' + esc(a.id) + '"' + (a.on ? '' : ' class="al-off"') + '>' +
			'<td data-l="On"><label class="switch" title="' + (a.on ? 'Enabled' : 'Disabled') + '"><input type="checkbox" data-f="on"' + (a.on ? ' checked' : '') + dis + ' aria-label="Enabled"><span class="track"></span></label></td>' +
			'<td data-l="Alert"><div class="al-nm"' + (topic ? ' title="' + esc('Event: Resource Monitor › ' + a.name + '\nTopic: ' + topic) + '"' : '') + '>' + esc(a.name || '(no name)') + '</div><div class="al-rule">' + esc(alRule(a)) + '</div>' + (a.context && a.context.text ? '<div class="al-ctx" title="Context">' + esc(a.context.text) + '</div>' : '') + '</td>' +
			'<td data-l="Send to" class="al-to-txt">' + toTxt + '</td>' +
			'<td data-l="Status">' + alStatusHtml(a) + '</td>' +
			'<td data-l=""><div class="row-actions"><button type="button" class="btn" data-a="test"' + (op && a.on ? ' title="Sends it On, then Off 3 s later, to each Send to (for real)"' : ' disabled title="' + (a.on ? 'Operators and admins' : 'Turn it on first') + '"') + '>Test</button>' +
				'<button type="button" class="btn" data-a="log">Log</button>' + (admin ? '<button type="button" class="btn" data-a="edit">Edit</button>' : '') + '</div></td></tr>';
	}).join('') : '<tr><td colspan="5" class="hint">No alerts yet.' + (admin ? ' Add one, for example CPU busy above 90 % for 1 min.' : '') + '</td></tr>';
}
/* Add (a = null) or edit one alert in the dialog */
function alEdit(a) {
	AL.edit = a || { name: '', on: true, metric: 'cpu', op: 'above', value: 90, for: 60, to: { event: true } };
	const e = AL.edit, f = +e.for || 0, mins = f >= 60 && f % 60 === 0;
	$('#ae-h').textContent = a ? 'Edit alert' : 'Add alert';
	$('#ae-name').value = e.name || '';
	$('#ae-metric').innerHTML = ((AL.data && AL.data.metrics) || []).map(m => '<option value="' + m.key + '"' + (m.key === e.metric ? ' selected' : '') + '>' + esc(m.name) + '</option>').join('');
	$('#ae-op').value = e.op === 'below' ? 'below' : 'above';
	$('#ae-value').value = e.value == null ? '' : e.value;
	$('#ae-unit').textContent = alMetric(e.metric).unit;
	$('#ae-for').value = mins ? f / 60 : f; $('#ae-forunit').value = mins ? '60' : '1';
	const to = alTo(e);
	$('#ae-to').innerHTML = '<div class="al-to" role="group" aria-label="Send to">' + AL_TO.map(t => '<button type="button" class="to-chip' + (to[t[0]] ? ' on' : '') + (alOff(t[0]) ? ' off-set' : '') + '" data-to="' + t[0] + '" aria-pressed="' + to[t[0]] + '"' +
		(alOff(t[0]) ? ' title="' + esc(AL_OFF[t[0]] + ': nothing goes out until it is on') + '"' : '') + '>' + t[1] + (alOff(t[0]) ? ' (off in Settings)' : '') + '</button>').join('') + '</div>';
	$('#ae-on').checked = e.on !== false;
	AL.ctx = e.context && e.context.text ? Object.assign({}, e.context) : null;   /* 1.6.3: kept until Save */
	alCtxRow();
	$('#ae-del').hidden = !a;
	$('#ae-msg').hidden = true;
	alNowHint();
	$('#dlg-alert').hidden = false;
	$('#ae-name').focus();
}
function alNowHint() {
	const k = $('#ae-metric').value, a = AL.data && AL.data.alerts.find(x => x.metric === k && x.status && x.status.now != null);
	$('#ae-now').textContent = a ? 'Now ' + alFmt(k, a.status.now) : '';
}
function alClose() { $('#dlg-alert').hidden = true; AL.edit = null; }
async function alSaveEdit() {
	const e = AL.edit;
	if (!e) return;
	const to = {};
	$$('#ae-to button[data-to]').forEach(c => { to[c.dataset.to] = c.classList.contains('on'); });
	const v = $('#ae-value').value, fr = $('#ae-for').value;
	const n = Object.assign({}, e, { name: $('#ae-name').value.trim(), metric: $('#ae-metric').value, op: $('#ae-op').value, value: v === '' ? '' : +v,
		for: fr === '' ? '' : Math.round(+fr * +$('#ae-forunit').value), on: $('#ae-on').checked, to, context: AL.ctx || undefined });
	const list = e.id ? AL.data.alerts.map(x => x.id === e.id ? n : x) : AL.data.alerts.concat([n]);
	const err = await saveAlerts(list, (n.name || 'Alert') + ': saved.');
	if (err) { $('#ae-msg').textContent = 'Not saved: ' + err; $('#ae-msg').hidden = false; } else alClose();
}
/* Saves the whole list; returns the reason when the camera refuses it */
async function saveAlerts(list, okText) {
	const m = $('#al-msg');
	try {
		const r = await postJSON('alertset', { alerts: list.map(alOut) });
		if (!r.ok) { m.textContent = 'Not saved: ' + r.error; return r.error || 'refused'; }
		AL.data = r;
		m.textContent = okText || 'Saved.';
		drawAlerts();
		return null;
	} catch (e) { m.textContent = 'Not saved: ' + e.message; drawAlerts(); return e.message; }
}
async function actAlert(a, action) {
	const m = $('#al-msg');
	try {
		const r = await postJSON('alertact', { id: a.id, action });
		m.textContent = !r.ok ? a.name + ': ' + r.error : a.name + ': test sent On to each Send to; Off follows in 3 s.';
		if (r.ok) setTimeout(pollAlerts, 3500);
	} catch (e) { m.textContent = a.name + ': ' + e.message; }
}
function openAlertLog(a) {
	AL.logId = a.id;
	$('#al-log-h').textContent = 'Log: ' + (a.name || a.id);
	$('#al-log').hidden = false;
	drawAlertLog(true);
	$('#al-log').scrollIntoView({ behavior: 'smooth', block: 'start' });
}
function drawAlertLog(toEnd) {
	if (!AL.logId || !AL.data) return;
	const body = $('#al-log-body'), atEnd = toEnd || body.scrollTop + body.clientHeight >= body.scrollHeight - 4;
	const ft = $('#al-f-time').value.trim(), fw = $('#al-f-what').value.trim().toLowerCase(), fs = $('#al-f-sent').value.trim().toLowerCase(), fu = $('#al-f-who').value.trim().toLowerCase();
	const toOf = l => (l.to || (l.event ? ['Camera event'] : [])).join(', ');      /* 1.5.0: each way out it went to */
	const sentOf = l => l.state && toOf(l) ? toOf(l) + ' · ' + l.state + (l.event ? ' ' + l.event : '') : '';
	const all = (AL.data.log || []).filter(l => l.id === AL.logId);
	const rows = all.filter(l => (!ft || hhmm(l.t, 1).includes(ft)) && (!fw || l.what.toLowerCase().includes(fw)) && (!fs || sentOf(l).toLowerCase().includes(fs)) && (!fu || (l.who || '').toLowerCase().includes(fu)));
	$('#al-log-rows').innerHTML = rows.length ? rows.map(l => '<tr><td data-l="Time" class="mono" title="' + esc(new Date(l.t * 1000).toLocaleString()) + '">' + hhmm(l.t, 1) + '</td><td data-l="What happened">' + esc(l.what) + '</td>' +
		'<td data-l="Sent"' + (l.topic ? ' title="' + esc((l.event ? 'Camera event: ' + l.event + '\n' : '') + 'Topic: ' + l.topic) + '"' : '') + '>' + (l.state && toOf(l) ? esc(toOf(l)) + ' · <b>' + esc(l.state) + '</b>' : '<span class="hint">–</span>') + '</td><td data-l="Who">' + esc(l.who) + '</td></tr>').join('')
		: '<tr><td colspan="4" class="hint">' + (all.length ? 'No lines match the filters.' : 'Nothing logged yet for this alert.') + '</td></tr>';
	if (atEnd) body.scrollTop = body.scrollHeight;
}

/*---------------------------------------------------- 1.6.3 Alerts: Context editor, Workflow view (as in Data Bender) */
const CTX_TOKENS = ['{value}', '{limit}', '{unit}', '{reading}', '{alert}', '{state}', '{for}', '{count}', '{time}', '{date}', '{camera}', '{serial}', '{ip}',
	'{round(value, 1)}', '{pad(count, 4)}', '{upper(alert)}', '{if(value > 95, "critical", "high")}', '{if(state == "On", "ALARM", "clear")}'];
const AL_HELP = {
	alerts: 'An alert is a reading past its limit for a while, for example CPU busy above 90 % for 1 min.\nIt is sent as a camera event (for a VMS, or the camera\'s Events › Rules), or to MQTT, InfluxDB or Prometheus when those are on in Settings.\nTest sends On, then Off 3 s later, to each Send to.',
	context: 'The text sent with the alert\'s camera event, MQTT message and InfluxDB point. Left empty, it sends the reading and the limit, for example "CPU busy 95 % (above 90 % for 60 s)".\nFields: {value} the reading, {limit}, {unit}, {reading} (its name), {alert}, {state} (On or Off), {for}, {count}, {time}, {date}, {camera}, {serial}, {ip}.\nFormulas: + - * / %, round, pad, upper, lower and if(test, then, else), for example {if(value > 95, "critical", "high")}.',
	when: 'What to watch, the limit, and how long the reading must stay past it before the alert turns On. It turns Off again when the reading is back (after at least 5 s).',
	counter: '{count} goes up each time the alert turns On. Test does not move it. Reset picks when it starts again from Start at.'
};
function initAlerts163() {
	$('#al-view-table').onclick = () => setAlView('table');
	$('#al-view-wf').onclick = () => setAlView('wf');
	/* Context editor */
	$('#ae-ctx-edit').onclick = ctxOpen;
	$('#ctx-tokens').innerHTML = CTX_TOKENS.map(t => '<button type="button" class="token">' + esc(t) + '</button>').join('');
	$('#ctx-tokens').onclick = e => { const b = e.target.closest('.token'); if (b) ctxInsert(b.textContent); };
	['#ctx-text', '#ctx-start', '#ctx-step'].forEach(s => { $(s).oninput = ctxPreview; });
	$('#ctx-reset').onchange = ctxPreview;
	$('#ctx-done').onclick = () => ctxClose(true);
	$('#ctx-cancel').onclick = () => ctxClose(false);
	$('#ctx-clear').onclick = () => { $('#ctx-text').value = ''; ctxPreview(); };
	$('#dlg-ctx').onclick = e => { if (e.target.id === 'dlg-ctx') ctxClose(false); };
	/* Workflow */
	$('#al-wf-run').onclick = () => { AL.wfRun = !AL.wfRun; $('#al-wf-run').textContent = AL.wfRun ? 'Hide last trigger' : 'Show last trigger'; $('#al-wf-run').classList.toggle('active', AL.wfRun); $('#al-wf-run').setAttribute('aria-pressed', String(AL.wfRun)); drawAlWf(); };
	$('#al-wf-test').onclick = () => { const a = alById(AL.wfId); if (a) actAlert(a, 'test'); };
	$('#al-wf-edit').onclick = () => { const a = alById(AL.wfId); if (a) alEdit(a); };
	$('#al-wf-list').onclick = e => { const b = e.target.closest('[data-id]'); if (b) { AL.wfId = b.dataset.id; AL.wfNode = 'when'; drawAlWf(); } };
	$('#al-wf-canvas').onclick = e => { const g = e.target.closest('[data-node]'); if (g) { AL.wfNode = g.dataset.node; drawAlWf(); } };
	$('#al-wf-canvas').onkeydown = e => { const g = e.target.closest('[data-node]'); if (g && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); AL.wfNode = g.dataset.node; drawAlWf(); } };
	$('#al-wf-props').onclick = e => { if (e.target.id === 'al-wf-ctx') { const a = alById(AL.wfId); if (a) { alEdit(a); ctxOpen(); } } };
	/* Help (i): the page's explanations, one click away */
	Help.add($('#tab-alerts .al-head h2'), 'Alerts', AL_HELP.alerts);
	$('.al-where').remove(); $('#al-foot').remove();
	Help.add($('#ae-ctx').closest('tr').firstElementChild, 'Context', AL_HELP.context);
	Help.add($('#ae-metric').closest('tr').firstElementChild, 'When', AL_HELP.when);
	Help.add($$('#dlg-ctx .ctx-l')[2], 'Counter', AL_HELP.counter);
	let v = 'table';
	try { v = localStorage.getItem('rm-al-view') || 'table'; } catch (e) { /* default */ }
	setAlView(v);
}
function setAlView(v) {
	AL.view = v === 'wf' ? 'wf' : 'table';
	try { localStorage.setItem('rm-al-view', AL.view); } catch (e) { /* not kept */ }
	$('#al-view-table').classList.toggle('on', AL.view === 'table'); $('#al-view-table').setAttribute('aria-pressed', String(AL.view === 'table'));
	$('#al-view-wf').classList.toggle('on', AL.view === 'wf'); $('#al-view-wf').setAttribute('aria-pressed', String(AL.view === 'wf'));
	$('#al-table-view').hidden = AL.view !== 'table';
	drawAlWf();
}

/* The Context row in Edit alert, and its preview */
function alFormNow() {
	const v = $('#ae-value').value, fr = $('#ae-for').value;
	return { id: AL.edit && AL.edit.id, name: $('#ae-name').value.trim(), metric: $('#ae-metric').value, op: $('#ae-op').value,
		value: v === '' ? 0 : +v, for: fr === '' ? 0 : Math.round(+fr * +$('#ae-forunit').value) };
}
async function alCtxRow() {
	const c = AL.ctx;
	$('#ae-ctx').textContent = c ? c.text : 'None: sends the reading and the limit';
	$('#ae-ctx').classList.toggle('none', !c);
	$('#ae-ctx-next').textContent = '';
	try {
		const r = await postJSON('alertpreview', Object.assign(alFormNow(), { context: c || undefined }));
		if (r && r.ok) $('#ae-ctx-next').textContent = 'Next On sends: ' + r.text;
	} catch (e) { /* the preview is a nicety */ }
}
function ctxOpen() {
	const c = AL.ctx || { text: '', start: 1, step: 1, reset: 'daily' };
	$('#ctx-h').textContent = 'Context: ' + ($('#ae-name').value.trim() || '(no name)');
	$('#ctx-text').value = c.text || '';
	$('#ctx-start').value = c.start == null ? 1 : c.start;
	$('#ctx-step').value = c.step == null ? 1 : c.step;
	$('#ctx-reset').value = c.reset || 'daily';
	$('#dlg-ctx').hidden = false;
	$('#ctx-text').focus();
	ctxPreview();
}
function ctxFromDialog() {
	const n = (s, d) => { const x = parseFloat($(s).value); return isFinite(x) ? x : d; };
	return { text: $('#ctx-text').value.trim(), start: n('#ctx-start', 1), step: n('#ctx-step', 1), reset: $('#ctx-reset').value };
}
function ctxPreview() {
	clearTimeout(AL.ctxTimer);
	AL.ctxTimer = setTimeout(async () => {
		const c = ctxFromDialog();
		if (!c.text) { $('#ctx-preview').textContent = 'The reading and the limit (no Context of its own)'; return; }
		try {
			const r = await postJSON('alertpreview', Object.assign(alFormNow(), { context: c }));
			$('#ctx-preview').textContent = r && r.ok ? r.text : 'Preview not available';
		} catch (e) { $('#ctx-preview').textContent = 'Preview not available: ' + e.message; }
	}, 250);
}
function ctxInsert(t) {
	const ta = $('#ctx-text'), s = ta.selectionStart, e = ta.selectionEnd;
	ta.value = ta.value.slice(0, s) + t + ta.value.slice(e);
	ta.focus();
	ta.selectionStart = ta.selectionEnd = s + t.length;
	ctxPreview();
}
function ctxClose(keep) {
	if (keep) { const c = ctxFromDialog(); AL.ctx = c.text ? c : null; alCtxRow(); }
	$('#dlg-ctx').hidden = true;
	if (!$('#dlg-alert').hidden) $('#ae-ctx-edit').focus();
}

/* Workflow: the chosen alert drawn as a flowchart. When > above/below? > lasts > Send > Off */
const WF_COL = { when: 'var(--rm-wf-when)', decide: 'var(--rm-wf-decide)', flow: 'var(--rm-wf-flow)', do: 'var(--rm-wf-do)', end: 'var(--rm-wf-end)' };   /* tokens in app.css */
const trunc = (s, n) => { s = String(s || ''); return s.length > n ? s.slice(0, n - 1) + '…' : s; };
function alLastOn(a) {
	const l = (AL.data.log || []).filter(x => x.id === a.id);
	let on = -1;
	l.forEach((x, i) => { if (x.state === 'On' && !/^Test/.test(x.what || '')) on = i; });
	if (on < 0) return null;
	return { line: l[on], off: l.slice(on + 1).some(x => x.state === 'Off' && !/^Test/.test(x.what || '')) };
}
function alWfChart(a) {
	const st = a.status || {}, m = alMetric(a.metric), f = +a.for || 0, c = a.context && a.context.text;
	const last = AL.wfRun ? alLastOn(a) : null;
	const reached = !AL.wfRun ? null : !last ? ['when'] : ['when', 'cond', 'lasts', 'send'].concat(last.off ? ['end'] : []);
	const chain = [
		['when', 'box', 'when', 65, 200, 54, 'When: ' + trunc(m.name, 24), st.now != null ? 'now ' + alFmt(a.metric, st.now) : (st.word || ''), null, ''],
		['cond', 'dia', 'decide', 165, 0, 70, a.op === 'below' ? 'Below' : 'Above', trunc(alFmt(a.metric, a.value), 10) + '?', ['x1', 'Listening', 'no'], 'yes'],
		['lasts', 'box', 'flow', 65, 200, 54, f ? 'Lasts: ' + alFor(f) : 'Lasts: no wait', f ? 'ends sooner: nothing sent' : 'sends at once', f ? ['x2', 'Not sent', 'sooner'] : null, 'lasted'],
		['send', 'box', 'do', 55, 220, 62, 'Send: ' + trunc(a.name || '(no name)', 24), c ? 'Context: ' + trunc(c, 26) : 'Context: reading and limit', null, ''],
		['end', 'pill', 'end', 95, 140, 36, 'Off when back to normal', '', null, '']
	];
	const N = [], W = [], GAP = 38;
	let top = 16;
	chain.forEach((n, i) => {
		const dia = n[1] === 'dia', h = n[5], mid = top + h / 2;
		N.push([n[0], n[1], n[2], n[3], dia ? mid : top, n[4], h, n[6], n[7]]);
		const side = n[8];
		if (side) {
			/* 1.6.5: the side exits sit a little further right, so the label on a box's exit wire has room and is not on the box's edge */
			N.push([side[0], 'pill', 'end', 310, mid - 18, 96, 36, side[1], '']);
			W.push(dia ? ['M215 ' + mid + ' L310 ' + mid, side[2], 262, mid - 7, 1] : ['M265 ' + mid + ' L310 ' + mid, side[2], 287.5, mid - 7, 1]);
		}
		if (i < chain.length - 1) W.push(['M165 ' + (top + h) + ' L165 ' + (top + h + GAP), n[9], 172, top + h + 23]);
		top += h + GAP;
	});
	const height = top - GAP + 16;
	const nodes = N.map(([id, sh, k, x, y, wd, h, t1, t2]) => {
		let shape, tx, ty, tk;
		if (sh === 'dia') {
			shape = '<polygon points="' + x + ',' + (y - 35) + ' ' + (x + 52) + ',' + y + ' ' + x + ',' + (y + 35) + ' ' + (x - 52) + ',' + y + '" style="fill:' + WF_COL[k] + '"/>';
			tx = x; ty = y + (t2 ? -4 : 4); tk = [x + 34, y - 30];
		} else {
			shape = '<rect x="' + x + '" y="' + y + '" width="' + wd + '" height="' + h + '" rx="' + (sh === 'pill' ? h / 2 : 8) + '" style="fill:' + WF_COL[k] + '"/>';
			tx = x + wd / 2; ty = y + h / 2 + (t2 ? -4 : 4); tk = [x + wd - 4, y + 4];
		}
		const hit = reached && reached.indexOf(id) >= 0;
		const cls = 'node' + (id === AL.wfNode ? ' sel' : '') + (reached && !hit ? ' miss' : '');
		return '<g class="' + cls + '" data-node="' + id + '" tabindex="0" role="button" aria-label="' + esc(t1 + ' ' + t2) + '">' + shape +
			'<text x="' + tx + '" y="' + ty + '" text-anchor="middle">' + esc(t1) + '</text>' +
			(t2 ? '<text class="sub" x="' + tx + '" y="' + (ty + 15) + '" text-anchor="middle">' + esc(t2) + '</text>' : '') +
			(hit ? '<g class="tick"><circle cx="' + tk[0] + '" cy="' + tk[1] + '" r="8"/><text x="' + tk[0] + '" y="' + (tk[1] + 4) + '" text-anchor="middle">✓</text></g>' : '') + '</g>';
	}).join('');
	const wires = W.map(x => '<path class="wire" d="' + x[0] + '" marker-end="url(#al-ar)"/>' + (x[1] ? '<text class="w" x="' + x[2] + '" y="' + x[3] + '"' + (x[4] ? ' text-anchor="middle"' : '') + '>' + x[1] + '</text>' : '')).join('');
	return '<svg viewBox="0 0 410 ' + height + '" xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Workflow of the alert ' + esc(a.name) + '">' +
		'<defs><marker id="al-ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z"/></marker></defs>' + wires + nodes + '</svg>';
}
function alWfProps(a) {
	const st = a.status || {}, m = alMetric(a.metric), f = +a.for || 0, c = a.context && a.context.text, to = alTo(a);
	const ways = AL_TO.filter(t => to[t[0]]).map(t => t[1]).join(', ') || 'Nowhere';
	const head = (k, tag, title) => '<h4><span class="kind' + (k === 'when' || k === 'decide' ? ' dark' : '') + '" style="background:' + WF_COL[k] + '">' + tag + '</span>' + esc(title) + '</h4>';
	const row = (l, v) => '<span class="p-l">' + esc(l) + '</span><div>' + v + '</div>';
	const n = AL.wfNode;
	let h;
	if (n === 'when') h = head('when', 'When', m.name) + row('Reading', esc(m.name)) + row('Now', st.now != null ? esc(alFmt(a.metric, st.now)) : '<span class="hint">no reading</span>') + row('Status', esc(st.word || ''));
	else if (n === 'cond' || n === 'x1') h = head('decide', 'Condition', (a.op === 'below' ? 'Below ' : 'Above ') + alFmt(a.metric, a.value)) + '<div class="help">While the reading is ' + (a.op === 'below' ? 'at or above' : 'at or under') + ' the limit, the alert keeps Listening and sends nothing.</div>';
	else if (n === 'lasts' || n === 'x2') h = head('flow', 'Lasts', f ? alFor(f) : 'No wait') + '<div class="help">' + (f ? 'The reading must stay past the limit this long. If it comes back sooner, nothing is sent.' : 'It turns On as soon as the reading passes the limit.') + '</div>';
	else if (n === 'send') h = head('do', 'Send', ways) + row('Event name', esc(a.name || '(no name)')) + '<div class="help">Sent as: Resource Monitor › ' + esc(a.name || '…') + '</div>' +
		row('Context', '<code class="' + (c ? '' : 'none') + '">' + esc(c || 'None: sends the reading and the limit') + '</code>') +
		(isAdmin() ? '<button type="button" class="btn" id="al-wf-ctx" style="margin-top:6px">Edit context…</button>' : '');
	else h = head('end', 'Off', 'Back to normal') + '<div class="help">It turns Off when the reading has been back on the right side of the limit for ' + (f > 5 ? alFor(f) : '5 s') + ', and sends Off to each Send to.</div>';
	return h + '<div class="how"><p>When <b>' + esc(m.name) + '</b> is <b>' + (a.op === 'below' ? 'below ' : 'above ') + esc(alFmt(a.metric, a.value)) + '</b>' + (f ? ' for <b>' + esc(alFor(f)) + '</b>' : '') +
		', the alert turns On and sends <b>Resource Monitor › ' + esc(a.name || '…') + '</b> to ' + esc(ways) + '.</p></div>';
}
function drawAlWf() {
	if (!AL.data) return;
	const admin = isAdmin();
	const wf = AL.view === 'wf', list = AL.data.alerts;
	$('#al-wf-view').hidden = !wf;
	if (!wf) return;
	if (!list.length) { $('#al-wf-view').hidden = true; $('#al-table-view').hidden = false; return; }
	$('#al-table-view').hidden = true;
	if (!alById(AL.wfId)) { AL.wfId = list[0].id; AL.wfNode = 'when'; }
	const a = alById(AL.wfId);
	$('#al-wf-edit').hidden = !admin;
	$('#al-wf-test').disabled = !(isOperator() && a.on);
	$('#al-wf-list').innerHTML = list.map(x => {
		const w = (x.status && x.status.word) || '', bad = x.on && w === 'Problem';
		return '<button type="button" class="wf-rule' + (x.id === a.id ? ' on' : '') + '" data-id="' + esc(x.id) + '" role="option" aria-selected="' + (x.id === a.id) + '">' +
			'<span class="wf-dot' + (!x.on ? '' : bad ? ' err' : ' ok') + '"></span><span><b>' + esc(x.name || '(no name)') + '</b><span class="sub">' + esc(alMetric(x.metric).name + ' · ' + (x.on ? w : 'Disabled')) + '</span></span></button>';
	}).join('');
	const cv = $('#al-wf-canvas');
	const focused = document.activeElement && cv.contains(document.activeElement) ? document.activeElement.dataset.node : null;
	cv.innerHTML = alWfChart(a);
	cv.classList.toggle('run', !!AL.wfRun);
	if (focused) { const g = cv.querySelector('[data-node="' + focused + '"]'); if (g) g.focus(); }
	$('#al-wf-props').innerHTML = alWfProps(a);
	const last = AL.wfRun ? alLastOn(a) : null;
	$('#al-wf-note').hidden = !AL.wfRun;
	$('#al-wf-note').textContent = !AL.wfRun ? '' : last ? 'Last On ' + hhmm(last.line.t, 1) + ': ' + last.line.what + (last.off ? '; it has turned Off since.' : '; still On.') + ' ✓ reached, faded = not reached.' : 'This alert has not turned On in its log yet.';
}

/*---------------------------------------------------- 1.7.0 Copy settings to other cameras (Settings tab; the UI kit's configio pattern)
 * Export: tick sections (each list with its count), Download. Import: Choose file -> a dialog with the file's app,
 * version, model and date; tick sections; Replace (default) or Merge; Preview = a dry run on the camera; Apply.
 * Undo last import, when there is one. The camera does every check (kit_configio.c, cfgsections.c): it holds an import
 * to exactly what the Settings and Alerts pages are held to; the page only shows what it says.
 * Old alert files (format 1, from the Alerts page of earlier versions) load here as the Alerts section. */
const CP = { S: null, file: null, text: null, mode: 'replace', busy: false, back: null, req: 0 };   /* req: changes with every change in the dialog */
const CP_MAX = 256 * 1024;
const CP_HELP = 'Export saves the ticked sections to a file. Import loads that file on another camera: Replace makes each ticked section exactly the file\'s; Merge adds the file\'s alerts and changes those with the same name, removes nothing, and changes settings only where the file has them.\n' +
	'Preview shows what would change; nothing is saved until Apply.\n' +
	'The MQTT and InfluxDB passwords and the InfluxDB token are never in the file, and CA certificates stay on each camera: the other camera keeps its own. The camera\'s name, address, logs, alert counters and history are never in the file either.\n' +
	'Undo last import puts the sections back as they were before the last import.';
async function cpPost(url, body) {
	const r = await fetch(url, { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body });
	let a = null;
	try { a = await r.json(); } catch (e) { /* not JSON */ }
	if (!a) throw new Error(r.status + ' ' + r.statusText);
	a.status = r.status;   /* 400: refused, nothing written; 500: the camera could not save (the answer says what is left) */
	return a;   /* {ok:false, error} is an answer, not a throw */
}
const cpSecName = id => { const s = CP.S && CP.S.sections.find(x => x.id === id); return s ? s.name : id; };
function cpMsg(text, bad) { const m = $('#cp-msg'); m.textContent = text || ''; m.classList.toggle('bad', !!bad); }
async function cpRefresh() {
	try { CP.S = await getJSON('configio?action=sections'); }
	catch (e) {
		CP.S = null;
		$('#cp-ticks').innerHTML = '<span class="hint">' + (/^40[13]/.test(e.message) ? 'Admin only: an administrator can copy settings.' : 'Could not read the sections: ' + esc(e.message)) + '</span>';
		$('#cp-left').textContent = ''; $('#cp-undo').hidden = true;
		$('#cp-dl').disabled = $('#cp-pick').disabled = true;
		return;
	}
	const list = CP.S.sections || [];
	const was = $$('#cp-ticks input').reduce((m, i) => (m[i.value] = i.checked, m), {});
	$('#cp-ticks').innerHTML = list.map(s => '<label class="cfg-tick"><input type="checkbox" value="' + esc(s.id) + '"' + (was[s.id] === false ? '' : ' checked') + '>' +
		esc(s.name) + (s.list ? ' <span class="n">' + esc(s.count) + '</span>' : '') + '</label>').join('');
	$('#cp-left').textContent = 'Passwords and the InfluxDB token are never in the file, and CA certificates stay on each camera: the other camera keeps its own.';
	$('#cp-pick').disabled = !list.length || !isAdmin();
	cpTicked();
	const u = CP.S.undo || {};
	$('#cp-undo').hidden = !u.available;
	if (u.available) {
		const t = u.time ? new Date(u.time) : null;
		$('#cp-undo-t').textContent = 'Last import: ' + (t && !isNaN(t) ? t.toLocaleString() : 'time unknown') + ', ' + (u.mode === 'merge' ? 'Merge' : 'Replace') + ', ' + (u.sections || []).map(cpSecName).join(', ') + '.';
	}
	$('#cp-undo-b').disabled = !isAdmin();
}
function cpTicked() { $('#cp-dl').disabled = !isAdmin() || !$$('#cp-ticks input:checked').length; }
async function cpExport() {
	const ids = $$('#cp-ticks input:checked').map(i => i.value);
	try {
		const f = await getJSON('configio?action=export&sections=' + encodeURIComponent(ids.join(',')));
		const d = new Date(), p = n => (n < 10 ? '0' : '') + n;
		const model = String(f.fromModel || 'camera').replace(/[^A-Za-z0-9-]+/g, '-');
		const link = document.createElement('a');
		link.href = URL.createObjectURL(new Blob([JSON.stringify(f, null, 2)], { type: 'application/json' }));
		link.download = 'resource-monitor-settings-' + model + '-' + d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + '.json';
		document.body.appendChild(link);
		link.click();
		setTimeout(() => { URL.revokeObjectURL(link.href); link.remove(); }, 1000);
		cpMsg('Downloaded: ' + ids.map(cpSecName).join(', ') + '.' + (f.omitted && f.omitted.length ? ' Left out: ' + f.omitted.length + ' password' + (f.omitted.length === 1 ? '' : 's') + ' or token' + (f.omitted.length === 1 ? '' : 's') + '.' : ''));
	} catch (e) { cpMsg('Could not export: ' + e.message, true); }
}
async function cpFile(f) {
	if (!f || !CP.S) return;
	if (f.size > CP_MAX) { cpMsg('The file is too big: the limit is 256 KB.', true); return; }
	const text = await f.text();
	let j = null;
	try { j = JSON.parse(text); } catch (e) { /* said below */ }
	if (!j || typeof j !== 'object' || Array.isArray(j)) { cpMsg('That is not a settings file: it is not JSON. Choose a file exported from Resource Monitor.', true); return; }
	if (j.app !== CP.S.app) { cpMsg(j.app ? 'This file is for another app (' + String(j.app).slice(0, 40) + '), not Resource Monitor. Export the file from Resource Monitor on the other camera.' : 'That is not a settings file: it does not say which app it is for.', true); return; }
	if (+j.format > CP.S.format) { cpMsg('This file is from a newer version of Resource Monitor: update the app first.', true); return; }
	cpMsg('');
	cpOpen(j, text);
}
/* The import dialog: the file's facts, its sections, Replace or Merge, Preview, Apply */
function cpOpen(j, text) {
	CP.file = j; CP.text = text;
	const old = !(j.format >= 2);
	const secs = old ? {} : (j.sections && typeof j.sections === 'object' ? j.sections : {});
	if (old) CP.S.sections.forEach(s => { if (s.list && Array.isArray(j[s.id])) secs[s.id] = j[s.id]; });
	const known = CP.S.sections.filter(s => secs[s.id] !== undefined);
	const unknown = Object.keys(secs).filter(k => !CP.S.sections.some(s => s.id === k));
	const when = j.exported ? new Date(j.exported) : null;
	const fact = (k, v) => '<b>' + esc(k) + '</b><span>' + esc(v || '–') + '</span>';
	$('#cp-facts').innerHTML = fact('App', 'Resource Monitor' + (old ? ' (an older alerts file)' : '')) + fact('Version', j.appVersion) + fact('Camera model', j.fromModel) +
		fact('Exported', when && !isNaN(when) ? when.toLocaleString() : '');
	$('#cp-secs').innerHTML = known.length ? known.map(s => '<label class="cfg-tick"><input type="checkbox" value="' + esc(s.id) + '" checked>' + esc(s.name) +
		(Array.isArray(secs[s.id]) ? ' <span class="n">' + secs[s.id].length + '</span>' : '') + '</label>').join('') : '<p class="cfg-err">The file has none of the sections Resource Monitor uses.</p>';
	const extra = [];
	if (unknown.length) extra.push('Not used here: ' + unknown.join(', ') + '.');
	if (Array.isArray(j.omitted) && j.omitted.length) extra.push('Not in the file: ' + j.omitted.join(', ') + '. This camera keeps its own.');
	$('#cp-extra').textContent = extra.join(' ');
	$('#cp-extra').hidden = !extra.length;
	$('#cp-dirty').hidden = !settingsDirty();
	cpSetMode('replace');
	CP.back = document.activeElement;
	$('#dlg-cfg').hidden = false;
	const first = $('#cp-secs input');
	(first || $('#cp-cancel')).focus();
}
function cpClose() {
	$('#dlg-cfg').hidden = true;
	CP.file = CP.text = null;
	if (CP.back && CP.back.focus && document.contains(CP.back)) CP.back.focus();
}
const cpChosen = () => $$('#cp-secs input:checked').map(i => i.value);
function cpSetMode(m) {
	CP.mode = m === 'merge' ? 'merge' : 'replace';
	$$('#cp-mode button').forEach(b => { const on = b.dataset.v === CP.mode; b.classList.toggle('on', on); b.setAttribute('aria-pressed', String(on)); });
	cpStale();
}
/* Anything changed in the dialog: the preview no longer holds, so Apply waits for a new one */
function cpStale() { CP.req++; $('#cp-prev').innerHTML = ''; $('#cp-apply').disabled = true; $('#cp-preview').disabled = CP.busy || !cpChosen().length; }
function cpShow(r) {
	const list = (r.sections || []).map(s => '<li><b>' + esc(s.name) + ':</b> ' + esc(s.added) + ' added, ' + esc(s.changed) + ' changed, ' + esc(s.removed) + ' removed, ' + esc(s.unchanged) + ' unchanged' +
		((s.notes || []).length ? '<ul>' + s.notes.map(n => '<li>' + esc(n) + '</li>').join('') + '</ul>' : '') + '</li>').join('');
	/* Only a refusal (400) is sure to have changed nothing; a failed save (500) says itself what is left */
	const sure = r.status === 400 && !/nothing/i.test(r.error || '');
	$('#cp-prev').innerHTML = (r.ok ? (r.dry ? '<p>Nothing is saved yet. Apply saves this:</p>' : '') : '<p class="cfg-err">' + esc(r.error || 'Refused.') + (sure ? ' Nothing was changed.' : '') + '</p>') + (list ? '<ul>' + list + '</ul>' : '') +
		((r.notes || []).length ? '<ul class="hint small">' + r.notes.map(n => '<li>' + esc(n) + '</li>').join('') + '</ul>' : '');
}
/* While the camera answers, the sections and the mode stay as they were sent */
function cpLock(on) { $$('#cp-mode button, #cp-secs input').forEach(x => { x.disabled = on; }); }
/* One request at a time; the mode and ticks are locked meanwhile. null: the answer is to older choices (the sections
 * or the mode changed since, or the dialog was closed), so it does not count */
async function cpSend(dry) {
	const my = ++CP.req;
	CP.busy = true;
	cpLock(true);
	$('#cp-preview').disabled = $('#cp-apply').disabled = true;
	if (dry) $('#cp-prev').innerHTML = '<p>Checking on the camera…</p>';
	let r;
	try {
		r = await cpPost('configio?action=import&mode=' + CP.mode + '&sections=' + encodeURIComponent(cpChosen().join(',')) + (dry ? '&dry=1' : ''), CP.text);
	} catch (e) { r = { ok: false, error: 'The camera did not answer: ' + e.message + (dry ? '' : '. Check the settings before trying again.') }; }
	finally { CP.busy = false; cpLock(false); $('#cp-preview').disabled = !cpChosen().length; }
	return my === CP.req && CP.text ? r : null;
}
async function cpPreview() {
	const r = await cpSend(true);
	if (!r) return;
	cpShow(r);
	$('#cp-apply').disabled = !r.ok;
	if (r.ok) $('#cp-apply').focus();
}
async function cpApply() {
	const r = await cpSend(false);
	if (!r) { cpAfter(); return; }                            /* closed meanwhile: the camera may have changed */
	if (!r.ok) {
		/* The camera's own words, and the page reads the camera again: a failed save may have left an undo copy
		 * (the put-back failed too), and the Undo row must show it */
		cpShow(r);
		cpAfter();
		return;
	}
	cpClose();
	cpMsg('Imported: ' + (r.sections || []).map(s => s.name).join(', ') + '. Undo last import puts them back.');
	cpAfter();
}
/* The camera changed: read its settings and alerts again */
function cpAfter() { cpRefresh(); loadSettings(); pollAlerts(); pollLog(); }
function cpUndo() {
	const u = (CP.S && CP.S.undo) || {};
	ask('Undo the last import?', (u.sections || []).map(cpSecName).join(', ') + ': back to how they were before the import. Changes made to them since then are lost.', 'Undo import', async () => {
		try {
			const a = await cpPost('configio?action=undo', '{}');
			if (!a.ok) throw new Error(a.error || 'Refused.');
			cpMsg('Import undone: ' + (a.sections || []).map(s => s.name).join(', ') + ' put back.');
			cpAfter();
		} catch (e) { cpMsg('Could not undo: ' + e.message, true); cpRefresh(); }
	}, true);
}
function initCopy() {
	Help.add($('#cp-h'), 'Copy settings to other cameras', CP_HELP);
	Help.add($('#cp-mode-h'), 'Replace or Merge', 'Replace: each ticked section becomes exactly the file\'s.\nMerge: the file\'s alerts are added, and alerts with the same name are changed; nothing is removed. Settings change only where the file has them.\nTwo alerts with the same name: Replace keeps both; Merge can\'t tell them apart and refuses the file.\nPasswords and the token this camera has are kept either way.');
	$('#cp-ticks').addEventListener('change', cpTicked);
	$('#cp-dl').onclick = cpExport;
	$('#cp-pick').onclick = () => { $('#cp-file').value = ''; $('#cp-file').click(); };
	$('#cp-file').onchange = e => cpFile(e.target.files && e.target.files[0]);
	$('#cp-undo-b').onclick = cpUndo;
	$('#cp-secs').addEventListener('change', cpStale);
	$('#cp-mode').onclick = e => { const b = e.target.closest('button[data-v]'); if (b) cpSetMode(b.dataset.v); };
	$('#cp-cancel').onclick = cpClose;
	$('#cp-preview').onclick = cpPreview;
	$('#cp-apply').onclick = cpApply;
	$('#dlg-cfg').onclick = e => { if (e.target.id === 'dlg-cfg') cpClose(); };
	/* Focus stays inside the dialog */
	$('#dlg-cfg').addEventListener('keydown', e => {
		if (e.key !== 'Tab') return;
		const f = $$('#dlg-cfg button:not(:disabled), #dlg-cfg input:not(:disabled)').filter(x => x.offsetParent);
		if (!f.length) return;
		if (e.shiftKey && document.activeElement === f[0]) { e.preventDefault(); f[f.length - 1].focus(); }
		else if (!e.shiftKey && document.activeElement === f[f.length - 1]) { e.preventDefault(); f[0].focus(); }
	});
}

/*---------------------------------------------------- 1.6.3 Help (i): a short explanation one click away (the same part as the UI kit's Kit.help) */
const Help = (() => {
	let pop = null, cur = null;
	function close() {
		if (!pop || pop.hidden) return;
		pop.hidden = true;
		if (cur) { cur.classList.remove('open'); cur.setAttribute('aria-expanded', 'false'); }
		cur = null;
	}
	function place(btn) {
		const r = btn.getBoundingClientRect(), w = Math.min(340, innerWidth - 16);
		pop.style.width = w + 'px';
		const left = Math.max(8, Math.min(r.left - 18, innerWidth - w - 8));
		let top = r.bottom + 9, up = false;
		pop.style.left = left + 'px'; pop.style.top = top + 'px';
		if (top + pop.offsetHeight > innerHeight - 8 && r.top - pop.offsetHeight - 9 > 8) { top = r.top - pop.offsetHeight - 9; up = true; }
		pop.style.top = top + 'px';
		pop.classList.toggle('up', up);
		pop.style.setProperty('--ax', Math.max(10, Math.min(w - 22, r.left + r.width / 2 - left - 6)) + 'px');
	}
	function open(btn) {
		if (cur === btn) { close(); return; }
		close();
		if (!pop) {
			pop = document.createElement('div');
			pop.className = 'kit-pop'; pop.setAttribute('role', 'dialog'); pop.hidden = true;
			document.body.appendChild(pop);
			pop.addEventListener('click', e => { if (e.target.closest('.kit-pop-x')) { const b = cur; close(); if (b) b.focus(); } });
		}
		const title = btn.dataset.helpTitle || 'Help';
		pop.setAttribute('aria-label', title);
		pop.innerHTML = '<div class="kit-pop-t"><b>' + esc(title) + '</b><button type="button" class="kit-pop-x" aria-label="Close">×</button></div>' +
			String(btn.dataset.help || '').split('\n').map(p => '<p>' + esc(p) + '</p>').join('');
		pop.hidden = false;
		place(btn);
		cur = btn;
		btn.classList.add('open'); btn.setAttribute('aria-expanded', 'true');
	}
	function make(title, text) {
		const b = document.createElement('button');
		b.type = 'button'; b.className = 'kit-info'; b.textContent = 'i';
		b.dataset.helpTitle = title; b.dataset.help = text;
		b.setAttribute('aria-label', 'Help: ' + title); b.setAttribute('aria-expanded', 'false');
		return b;
	}
	/* After el (a heading, a setting's name): add an (i) with this text */
	function add(el, title, text) { if (el && text) el.appendChild(make(title, text)); }
	document.addEventListener('click', e => {
		const b = e.target.closest('.kit-info');
		if (b) { e.preventDefault(); e.stopPropagation(); open(b); return; }
		if (pop && !pop.hidden && !e.target.closest('.kit-pop')) close();
	}, true);
	document.addEventListener('keydown', e => { if (e.key === 'Escape' && pop && !pop.hidden) { const b = cur; close(); if (b) b.focus(); e.stopImmediatePropagation(); } }, true);
	addEventListener('resize', close);
	addEventListener('scroll', e => { if (pop && !pop.hidden && !(e.target instanceof Node && pop.contains(e.target))) close(); }, true);
	return { add, close, make };
})();
/* The page's grey how-to sentences move into (i) buttons: panel headings, Settings rows and section notes */
function helpify() {
	const label = el => (el.textContent || '').replace(/\s+/g, ' ').trim();
	$$('.acap-panel-head > .hint.small:not([id])').forEach(s => { const h = s.parentNode.querySelector('h2'); if (h) { Help.add(h, label(h), label(s)); s.remove(); } });
	$$('#tab-settings .set-rows tr').forEach(tr => {
		const name = tr.firstElementChild, notes = $$('td:not(:first-child) span.set-note:not([id])', tr);
		if (!name || !notes.length) return;
		const target = name.querySelector('.label') || name;
		Help.add(target, label(target), notes.map(label).join('\n'));
		notes.forEach(n => n.remove());
	});
	$$('#tab-settings td.set-note:not([id])').forEach(td => { const b = td.parentNode.querySelector('button'); if (b) { Help.add(td.parentNode.firstElementChild, label(b).replace(/…$/, ''), label(td)); td.textContent = ''; } });
	/* 1.6.4: the two rows whose note the page writes itself */
	Help.add($('#s-power').closest('tr').firstElementChild, 'Power figures from', 'Camera\'s power log (recommended): the camera keeps its own log; Power shows 1 h to 30 days from it, and Resource Monitor keeps no copy.\nRecorded by Resource Monitor: Resource Monitor reads the power draw and keeps it in its own history.\nOff: power is not read, and the Power page is hidden.');
	Help.add($('#s-days').closest('tr').firstElementChild, 'Keep history', '24 h is kept without an SD card. 7 and 30 days need one.\nOn the card each file is set aside in full when it is made, so it never grows. Choosing a shorter time deletes older history from the card when you save.');
	$$('#tab-settings p.set-note.set-foot').forEach(p => {
		const panel = p.closest('.acap-panel, section, .set-sec'), h = panel && panel.querySelector('h2, h3, .set-cap');
		if (h) { Help.add(h, label(h), label(p)); p.remove(); }
	});
}
/*---------------------------------------------------- 1.3.1: who is looking, and Clear */
/* Viewers see every page; Start, Stop, Restart, Clear and Settings changes are admin only (the camera refuses them to
   anyone else too). Not known (no answer): every control shows, and the camera decides. */
let me = { level: 'unknown' };
const isAdmin = () => me.level === 'admin' || me.level === 'unknown';
function applyAccess() {
	const admin = isAdmin();
	if (!admin) $$('#tab-settings select, #tab-settings input, #tab-settings textarea').forEach(i => { i.disabled = true; });
	if (!admin) $$('#tab-settings [data-clearpw]').forEach(b => { b.hidden = true; });
	$('#s-save').hidden = $('#s-cancel').hidden = !admin;
	$$('.admin-hint').forEach(h => { h.hidden = admin; });
	$$('.admin-only').forEach(b => { if (!admin) { b.disabled = true; b.title = 'Admin only'; } });
}
const CLEAR = {
	history: ['Clear history?', 'Every history point from 1 h to 30 days goes, in memory, on the camera\'s built-in flash and on the SD card; the graphs start again from now. Live readings and the camera\'s own power log stay. This can\'t be undone.', 'Clear history'],
	log: ['Clear the app log?', 'Every line of Apps › Log goes from the camera. A line saying who cleared it is kept. This can\'t be undone.', 'Clear log'],
};
function askClear(what, after) {
	const c = CLEAR[what];
	ask(c[0], c[1], c[2], async () => {
		try {
			const r = await postJSON('clear', { what });
			$('#s-msg').textContent = (r && r.message ? r.message : 'Cleared') + '.';
			if (what === 'history') { hist = null; ovHist = null; }
			if (what === 'log') S.log = (await getJSON('log')).lines || [];
			if (after) after();
		} catch (e) { $('#s-msg').textContent = 'Not cleared: ' + e.message; alert('Not cleared: ' + e.message); }
	}, true);
}

/*---------------------------------------------------- tabs and start */
function showTab(t) {
	$$('.ftip').forEach(x => { x.hidden = true; });       /* a tooltip left open belongs to the page being left */
	pinned = null;
	tab = t;
	$$('.tab-page').forEach(p => { p.hidden = p.id !== 'tab-' + t; });
	if (t === 'perf') showPerf(); else clearTimeout(histTimer);
	if (t === 'procs') { otherOpen = null; pollProcs(); }
	if (t === 'apps') { drawApps(); pollApps(); }
	if (t === 'log') pollLog();
	if (t === 'overview') { drawOverview(); loadOverview(); } else clearTimeout(ovTimer);
	if (t === 'drives') drawDrives();
	if (t === 'conns') drawConns();
	if (t === 'alerts') pollAlerts();
	if (t === 'settings') { loadSettings(); outTimer = setTimeout(pollOutStatus, 5000); cpRefresh(); }
	if (t === 'perf' && sel === 'power' && !setData) loadSettings();   /* the PoE ports Settings picked */
	drawBar();
}
/* #cpu/procs opens CPU › Processes; no hash opens the Overview */
function fromHash() {
	const m = /^#([a-z]+)(?:\/([a-z]+))?/.exec(location.hash || '');
	go(m && ITEMS[m[1]] ? m[1] : 'overview', m ? m[2] : null);
}

document.addEventListener('DOMContentLoaded', () => {
	/* Links between pages: data-go="item/page" */
	document.addEventListener('click', e => {
		const a = e.target.closest('a[data-go]');
		if (!a) return;
		e.preventDefault();
		const [it, pg] = a.dataset.go.split('/');
		go(it, pg);
	});
	$$('.rb-item').forEach(b => { b.onclick = () => go(b.dataset.k); });
	/* 1.6.1 Hover: two yellow lines chase round a lifted sidebar tile */
	$$('.side .rb-item').forEach(b => b.insertAdjacentHTML('beforeend', '<svg class="trace" aria-hidden="true"><rect pathLength="100"/></svg>'));
	$$('.range button').forEach(b => { b.onclick = () => setRange(b.dataset.r); });
	$$('#pr-group button').forEach(b => { b.onclick = () => { procGroup = b.dataset.g; drawProcs(); }; });
	$('#p-find').oninput = drawProcs;
	$$('#p-who button').forEach(b => { b.onclick = () => { $$('#p-who button').forEach(x => x.classList.toggle('on', x === b)); pWho = b.dataset.w; drawProcs(); }; });
	['#f-time', '#f-what', '#f-who'].forEach(s => { $(s).oninput = drawLog; });
	$('#f-app').onchange = drawLog;
	$('#clear-filters').onclick = () => { ['#f-time', '#f-what', '#f-who'].forEach(s => { $(s).value = ''; }); $('#f-app').value = ''; drawLog(); };
	$('#export-log').onclick = exportLog;
	/* Settings: any change marks the form; Save saves at once, Cancel puts back what is saved */
	$('#tab-settings').addEventListener('change', e => { if (e.target.matches('select, input') && !e.target.closest('#copy')) { readForm(); $('#s-msg').textContent = ''; drawSettings(); } });
	/* 1.6.1: typing marks the page changed at once, so the Save bar comes up without leaving the field */
	$('#tab-settings').addEventListener('input', e => { if (e.target.matches('input[type=text], input[type=number], input[type=password], textarea')) { readForm(); $('#s-msg').textContent = ''; setDirty(); } });
	$('#s-save').onclick = saveSettings;
	$('#s-prom-test').onclick = testMetrics;
	$$('[data-test]').forEach(b => { b.onclick = () => testOut(b.dataset.test); });
	/* Clear a saved password or token: sent as empty on Save */
	$$('[data-clearpw]').forEach(b => { b.onclick = () => { const k = b.dataset.clearpw; if (k === 'mqtt') setForm.mqtt.password = ''; else if (k === 'influx-pass') setForm.influx.password = ''; else setForm.influx.token = ''; drawSettings(); }; });
	/* Text fields mark the form as they are typed in */
	$('#tab-settings').addEventListener('input', e => { if (e.target.matches('input[type=text], input[type=password], input[type=number], textarea')) { readForm(); setDirty(); } });
	$('#s-cancel').onclick = () => { setForm = JSON.parse(JSON.stringify(setData.settings)); $('#s-msg').textContent = ''; drawSettings(); };
	$('#clear-log').onclick = () => askClear('log', drawLog);
	$$('[data-clear]').forEach(b => { b.onclick = () => askClear(b.dataset.clear, () => { if (b.dataset.clear === 'log') drawLog(); }); });
	/* Who is looking: a viewer gets the pages with the admin-only controls greyed out */
	getJSON('whoami').then(r => { me = r || me; applyAccess(); if (tab === 'apps') drawApps(); if (tab === 'alerts') drawAlerts(); }).catch(() => {});
	$$('[data-close]').forEach(b => { b.onclick = () => { $('#dlg-ask').hidden = true; }; });
	$('#da-ok').onclick = () => { $('#dlg-ask').hidden = true; if (askOk) askOk(); };
	$('#dlg-ask').onclick = e => { if (e.target.id === 'dlg-ask') $('#dlg-ask').hidden = true; };
	document.addEventListener('keydown', e => { if (e.key === 'Escape') $('#dlg-ask').hidden = true; });
	window.addEventListener('hashchange', fromHash);
	/* Closing the tab or going to another site with Settings changes not saved: the browser asks first */
	window.addEventListener('beforeunload', e => { if (settingsDirty()) { e.preventDefault(); e.returnValue = ''; } });
	/* The sidebar stays in view under the header, whatever height the header wraps to */
	const hdrH = () => document.documentElement.style.setProperty('--hdr-h', $('.acap-header').offsetHeight + 'px');
	hdrH(); window.addEventListener('resize', hdrH);

	getJSON('app').then(a => {
		const d = (a && a.device) || {}, setup = ((a && a.manifest) || {}).acapPackageConf;
		const ver = setup && setup.setup && setup.setup.version;
		$('#app-ver').textContent = ver || '';
		S.device = d;
		$('#f-model').textContent = d.model || 'Axis camera';
		$('#f-sn').textContent = d.serial || '–';
		$('#f-os').textContent = d.firmware || '–';
		S.chip = d.platform && d.platform !== 'Unknown' ? d.platform.replace(/^axiss+/i, '').toUpperCase() : (d.chip && d.chip !== 'Unknown' ? d.chip : '');
	}).catch(() => {});
	pollApps();
	pollCamera();
	helpify();
	initAlerts();
	initCopy();
	fromHash();
	pollLive();
	setInterval(checkStale, 1000);
});
})();
