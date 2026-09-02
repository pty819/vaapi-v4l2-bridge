// chrome_mi.js — Chrome live decoder probe via CDP (port 9222).
// Prints the media-internals decoder lines for the most recent player and
// any eglCreateImage problems from chrome://gpu. Run on the NAS host:
//   node chrome_mi.js
// Gate contract (scripts/chrome_smoke.sh): output must contain
// "VaapiVideoDecoder" and must NOT match /eglCreateImage.*(error|fail)/i.
const http = require('http');

function get(path) {
  return new Promise((res, rej) => {
    http.get({ host: '127.0.0.1', port: 9222, path }, r => {
      let d = '';
      r.on('data', c => d += c);
      r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(new Error('bad json from ' + path)); } });
    }).on('error', rej);
  });
}

async function main() {
  const ver = await get('/json/version');
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  let id = 0;
  const pending = {};
  ws.addEventListener('message', ev => {
    const d = JSON.parse(ev.data);
    if (d.id && pending[d.id]) { pending[d.id](d.result); delete pending[d.id]; }
  });
  await new Promise((res, rej) => { ws.addEventListener('open', res); ws.addEventListener('error', rej); });
  const send = (method, params, sessionId) => new Promise(res => {
    const i = ++id;
    pending[i] = res;
    ws.send(JSON.stringify({ id: i, method, params, sessionId }));
  });

  async function tabFor(urlPart, createUrl) {
    const pages = (await get('/json')).filter(t => t.type === 'page' && t.url.includes(urlPart));
    if (pages.length) return pages[0].id;
    return (await send('Target.createTarget', { url: createUrl })).targetId;
  }
  async function attach(targetId) {
    const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
    return sessionId;
  }
  async function text(sessionId) {
    const r = await send('Runtime.evaluate', { expression: 'document.body.innerText', returnByValue: true }, sessionId);
    return (r && r.result && r.result.value) || '';
  }

  // --- media-internals: expand the last player log and pull decoder lines ---
  const miId = await tabFor('media-internals', 'chrome://media-internals');
  const miS = await attach(miId);
  await new Promise(r => setTimeout(r, 4000));
  await send('Runtime.evaluate', { expression:
    `(()=>{const it=[...document.querySelectorAll('*')].filter(e=>!e.children.length&&/kPlay|kPause/.test(e.textContent));
      const play=it.filter(e=>e.textContent.includes('kPlay'));
      if(play.length)play[play.length-1].click();else if(it.length)it[it.length-1].click();
      return it.length;})()`
  }, miS);
  await new Promise(r => setTimeout(r, 1200));
  const mi = await text(miS);
  const lines = mi.split('\n');
  const out = ['=== media ==='];
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i];
    if (/kVideoDecoderName|kIsPlatformVideoDecoder|Selected .*Decoder|decoder fallback/i.test(l)) {
      const t = l.trim(), v = (lines[i + 1] || '').trim();
      out.push((/^00:/.test(t) ? t : '        ') + ' ' + (/^(00:|"|\{)/.test(v) ? v : '').slice(0, 60));
    }
  }

  // --- chrome://gpu: any eglCreateImage problems ---
  const gpuId = await tabFor('gpu', 'chrome://gpu');
  const gpuS = await attach(gpuId);
  await new Promise(r => setTimeout(r, 1500));
  const gpu = await text(gpuS);
  out.push('=== gpu ===');
  out.push(gpu.split('\n').filter(l => /eglCreateImage|EGL_BAD|dmabuf.*error/i.test(l)).slice(0, 8).join('\n') || 'clean');

  console.log(out.join('\n'));
  ws.close();
  process.exit(0);
}

main().catch(e => { console.log('ERR', e.message); process.exit(1); });
