// Release builds: minify the page's scripts and styles in a copy of app/ before packaging.
// The repo keeps the readable source; only the shipped .eap gets the minified files.
//   node tools/minify.mjs <copy of app/> <folder holding node_modules/terser>
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { createRequire } from 'node:module';

const [appDir, toolsDir] = process.argv.slice(2);
if (!appDir || !toolsDir) { console.error('usage: node tools/minify.mjs <app copy> <terser folder>'); process.exit(2); }
const { minify } = createRequire(join(toolsDir, 'package.json'))('terser');

const jsDir = join(appDir, 'html', 'js'), cssDir = join(appDir, 'html', 'css');
for (const f of readdirSync(jsDir).filter(n => n.endsWith('.js'))) {
	const p = join(jsDir, f), src = readFileSync(p, 'utf8');
	const out = await minify(src, { compress: { passes: 2 }, mangle: true, format: { comments: false } });
	writeFileSync(p, out.code);
	console.log(`${f}: ${src.length} -> ${out.code.length} bytes`);
}
// CSS: drop comments and spare white space (no CSS strings here hold any)
for (const f of readdirSync(cssDir).filter(n => n.endsWith('.css'))) {
	const p = join(cssDir, f), src = readFileSync(p, 'utf8');
	const out = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\s+/g, ' ').replace(/\s*([{};])\s*/g, '$1').replace(/;}/g, '}').trim();
	writeFileSync(p, out);
	console.log(`${f}: ${src.length} -> ${out.length} bytes`);
}
// HTML: drop comments
const h = join(appDir, 'html', 'index.html'), hs = readFileSync(h, 'utf8');
writeFileSync(h, hs.replace(/<!--[\s\S]*?-->/g, ''));
