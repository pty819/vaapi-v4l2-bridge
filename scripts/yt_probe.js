// yt_probe.js — drive a YouTube tab via CDP: consent click, play, dump
// video state + stats-for-nerds codec. Run on the NAS host:
//   node yt_probe.js <youtube-url>
const http = require("http");
const url_arg = process.argv[2] || "https://www.youtube.com";

function get(path) {
  return new Promise((res, rej) => {
    http.get({ host: "127.0.0.1", port: 9222, path }, r => {
      let d = "";
      r.on("data", c => d += c);
      r.on("end", () => { try { res(JSON.parse(d)); } catch (e) { rej(new Error("bad json " + path)); } });
    }).on("error", rej);
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const pages = await get("/json");
  let page = pages.find(t => t.type === "page" && !t.url.startsWith("chrome://") && !t.url.includes("omnibox"));
  page = page || pages.find(t => t.type === "page");
  if (!page) { console.log("NO_TAB"); process.exit(1); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.addEventListener("open", res); ws.addEventListener("error", rej); });
  let id = 0;
  const pend = {};
  ws.addEventListener("message", ev => {
    const d = JSON.parse(ev.data);
    if (d.id && pend[d.id]) { pend[d.id](d.result); delete pend[d.id]; }
  });
  const send = (method, params) => new Promise(res => {
    const i = ++id;
    pend[i] = res;
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  await send("Runtime.enable", {});
  await send("Page.enable", {});
  console.log("NAV", url_arg);
  await send("Page.navigate", { url: url_arg });
  await sleep(7000);

  // consent dialog (any locale): ytd-consent buttons
  const consent = await send("Runtime.evaluate", {
    returnByValue: true,
    expression: `(() => {
      const sels = ['button[aria-label*="Accept all"]', 'button[aria-label*="Reject all"]',
                    'ytd-consent-bump-v2-lightbox button[aria-label]',
                    'tp-yt-paper-button[aria-label*="Accept"]'];
      for (const s of sels) {
        const b = document.querySelector(s);
        if (b) { b.click(); return "clicked:" + s; }
      }
      return "no-consent";
    })()`
  });
  console.log("CONSENT", consent.result.value);
  await sleep(2500);

  // play
  const play = await send("Runtime.evaluate", {
    returnByValue: true, awaitPromise: true,
    expression: `(async () => {
      const v = document.querySelector('video');
      if (!v) return {noVideo: true, url: location.href.slice(0, 60)};
      try { v.muted = true; await v.play(); } catch (e) {}
      return {playing: !v.paused, t: v.currentTime, w: v.videoWidth, h: v.videoHeight};
    })()`
  });
  console.log("PLAY", JSON.stringify(play.result.value));
  await sleep(9000);

  // video state + codec via stats-for-nerds / player response
  const state = await send("Runtime.evaluate", {
    returnByValue: true,
    expression: `(() => {
      const v = document.querySelector('video');
      const p = document.getElementById('movie_player');
      let codec = null, sfn = null;
      try { sfn = p && p.getStatsForNerds ? p.getStatsForNerds() : null; } catch (e) {}
      if (sfn) codec = sfn.codecs || sfn.codec || JSON.stringify(sfn).slice(0, 200);
      let formats = null;
      try {
        const pr = p.getPlayerResponse();
        const sel = pr && pr.streamingData && pr.streamingData.adaptiveFormats
          ? pr.streamingData.adaptiveFormats.filter(f => (f.mimeType||'').startsWith('video'))
              .map(f => f.mimeType.split(' ')[0] + '@' + (f.height||'?') + (f.qualityLabel||'')) : null;
        formats = sel && sel.slice(0, 14);
      } catch (e) {}
      return {url: location.href.slice(0, 70), ready: v ? v.readyState : -1,
              paused: v ? v.paused : null, t: v ? Math.round(v.currentTime) : 0,
              w: v ? v.videoWidth : 0, h: v ? v.videoHeight : 0,
              codec, formats};
    })()`
  });
  console.log("STATE", JSON.stringify(state.result.value, null, 1));
  ws.close();
  process.exit(0);
})().catch(e => { console.error("ERR", e.message); process.exit(1); });
