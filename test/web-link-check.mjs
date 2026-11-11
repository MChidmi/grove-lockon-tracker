// Drives the web page's protocol module (web/grove-link.js) against the real board from Node,
// the same way the page does over Web Serial: open (the board restarts), check the firmware,
// start the live view, lock on, count tracking results, stop.
//
//   node test/web-link-check.mjs [/dev/ttyACM0] [video fps, default 0]
// Uses serialport from test/full-web-ui (npm install there first).
import { createRequire } from 'node:module';

const require = createRequire(new URL('./full-web-ui/package.json', import.meta.url));
const { SerialPort } = require('serialport');
const { GroveLink } = createRequire(import.meta.url)('../web/grove-link.js');

const path = process.argv[2] || '/dev/ttyACM0';
const video = Number(process.argv[3] ?? 0);
const port = new SerialPort({ path, baudRate: 921600 });
const link = new GroveLink((bytes) => new Promise((res, rej) => port.write(Buffer.from(bytes), (e) => (e ? rej(e) : res()))));
port.on('data', (chunk) => link.feed(new Uint8Array(chunk)));

let frames = 0, pos = 0, images = 0;
const states = {};
link.addEventListener('frame', (e) => { frames++; if (e.detail.image) images++; if (e.detail.track) pos++; });
link.addEventListener('trackpos', (e) => { if (e.detail.track && e.detail.track.state !== 'off') pos++; });
link.addEventListener('track', (e) => { states[e.detail.state] = (states[e.detail.state] || 0) + 1; });

await new Promise((r) => port.on('open', r));
const ready = await link.waitReady(8000);
console.log('board restarted and ready:', ready);
const ver = await link.command('VER?');
console.log('VER?', JSON.stringify(ver));
if (ver.firmware !== 'grove-sd' || (ver.demo ?? 1) < 2) throw new Error('not the tracker firmware');
console.log('PREVIEW', JSON.stringify(await link.command('PREVIEW=1,320,1,70')));
console.log('VIDEO', JSON.stringify(await link.command(`VIDEO=${video}`)));
await link.command(`INVOKE=-1,0,${video ? 0 : 1}`, 5000);
await new Promise((r) => setTimeout(r, 1500));
console.log('TRACK', JSON.stringify(await link.command('TRACK=1,80,60')));
frames = pos = images = 0;
const t0 = Date.now();
await new Promise((r) => setTimeout(r, 6000));
const s = (Date.now() - t0) / 1000;
console.log(`${(pos / s).toFixed(1)} positions/s, ${(frames / s).toFixed(1)} frames/s (${images} with images); TRACK events ${JSON.stringify(states)}`);
console.log('TRACK?', JSON.stringify(await link.command('TRACK?')));
let bad = 0;
for (let i = 0; i < 30; i++) {
  try { await link.command(i % 2 ? 'TRACK?' : 'PREVIEW?'); } catch { bad++; }
}
console.log(`30 commands while streaming: ${30 - bad} answered`);
await link.command('TRACK=0');
await link.command('BREAK');
port.close();
