/*
 * Follows the camera's light or dark theme. Loaded first in <head>, so data-theme is on <html> before the page is drawn.
 * Order: the "theme" the camera's own pages keep in this browser ("dark" or "light", plain or JSON-quoted; same site,
 * so this page can read it), else "?theme=" in the address, else the computer's light or dark setting.
 * Follows changes while the page is open: the camera's theme changed in another tab (the "storage" event), or the
 * computer's setting changed. The page's graphs watch data-theme and redraw.
 */
(function () {
	'use strict';
	const root = document.documentElement;
	const mq = window.matchMedia ? window.matchMedia('(prefers-color-scheme: light)') : null;
	const pick = v => (v === 'light' || v === 'dark') ? v : null;
	function saved() {
		try {
			let v = window.localStorage.getItem('theme');
			if (v == null) return null;
			try { v = JSON.parse(v); } catch (e) { /* plain text */ }
			return pick(v);
		} catch (e) { return null; }              /* storage blocked */
	}
	function fromAddress() {
		try { return pick(new URLSearchParams(window.location.search).get('theme')); } catch (e) { return null; }
	}
	function apply() {
		const t = saved() || fromAddress() || (mq && mq.matches ? 'light' : 'dark');
		if (root.getAttribute('data-theme') !== t) root.setAttribute('data-theme', t);
	}
	apply();
	window.addEventListener('storage', e => { if (e.key === 'theme' || e.key === null) apply(); });
	if (mq) {
		if (mq.addEventListener) mq.addEventListener('change', apply);
		else if (mq.addListener) mq.addListener(apply);
	}
})();
