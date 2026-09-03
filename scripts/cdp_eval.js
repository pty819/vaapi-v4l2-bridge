// cdp_eval.js — evaluate a JS expression (from a file) in the first
// non-chrome:// page tab and print the JSON result.
//   node cdp_eval.js expr.js [url-substr]
const http = require("http");
const fs = require("fs");
const expr = fs.readFileSync(process.argv[2], "utf8");
const want = process.argv[3] || "";

function get(path) {
  return new Promise((res, rej) => {
    http.get({ host: "127.0.0.1", port: 9222, path }, r => {
      let d = "";
      r.on("data", c => d += c);
      r.on("end", () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } });
    }).on("error", rej);
  });
}

(async () => {
  const pages = await get("/json");
  let pg = pages.find(t => t.type === "page" && t.url.includes(want) && !t.url.startsWith("chrome://"));
  pg = pg || pages.find(t => t.type === "page" && !t.url.startsWith("chrome://"));
  if (!pg) { console.log("NO_TAB"); process.exit(1); }
  const ws = new WebSocket(pg.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.addEventListener("open", res); ws.addEventListener("error", rej); });
  let id = 0;
  const pend = {};
  ws.addEventListener("message", ev => {
    const d = JSON.parse(ev.data);
    if (d.id && pend[d.id]) { pend[d.id](d.result); delete pend[d.id]; }
  });
  const send = (m, p) => new Promise(res => {
    const i = ++id;
    pend[i] = res;
    ws.send(JSON.stringify({ id: i, method: m, params: p }));
  });
  await send("Runtime.enable", {});
  const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails)
    console.log("EXC", JSON.stringify(r.exceptionDetails.exception || {}).slice(0, 300));
  console.log(typeof r.result.value === "string" ? r.result.value : JSON.stringify(r.result.value, null, 1));
  ws.close();
  process.exit(0);
})().catch(e => { console.error("ERR", e.message); process.exit(1); });
