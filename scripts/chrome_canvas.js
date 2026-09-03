// chrome_canvas.js — brightness probe for the live video tab via CDP (9222).
// Draws the playing <video> into a canvas 3x and prints per-sample JSON
// including "avg":N (0-255 mean of sampled pixels). Run on the NAS host:
//   node chrome_canvas.js
// Gate contract (scripts/chrome_smoke.sh): max of all "avg":N values must
// be >= 50 (a scene cut can darken one sample; a broken import darkens all).
const http = require("http");

function get(path) {
  return new Promise((res, rej) => {
    http.get({ host: "127.0.0.1", port: 9222, path }, r => {
      let d = "";
      r.on("data", c => d += c);
      r.on("end", () => { try { res(JSON.parse(d)); } catch (e) { rej(new Error("bad json from " + path)); } });
    }).on("error", rej);
  });
}

async function main() {
  // Optional argv[2]: URL substring to pick the tab (default: the live room).
  const want = process.argv[2] || "live.bilibili";
  const pages = (await get("/json")).filter(t => t.type === "page" && t.url.includes(want));
  if (!pages.length) {
    console.log("NO_LIVE_TAB navigate a tab to a live.bilibili.com stream first");
    process.exit(1);
  }
  const ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  let id = 0;
  const pending = {};
  ws.addEventListener("message", ev => {
    const d = JSON.parse(ev.data);
    if (d.id && pending[d.id]) { pending[d.id](d); delete pending[d.id]; }
  });
  await new Promise((res, rej) => { ws.addEventListener("open", res); ws.addEventListener("error", rej); });
  const send = (method, params) => new Promise(res => {
    const i = ++id;
    pending[i] = res;
    ws.send(JSON.stringify({ id: i, method, params }));
  });

  for (let k = 0; k < 3; k++) {
    const r = await send("Runtime.evaluate", { expression: `(function(){
      var vs=document.querySelectorAll('video');if(!vs.length)return 'no-video';
      var out=[];
      for(const v of vs){
        var c=document.createElement('canvas');c.width=v.videoWidth||2;c.height=v.videoHeight||2;
        var ctx=c.getContext('2d');
        var e='';try{ctx.drawImage(v,0,0);}catch(err){e=':'+err.message}
        var d=ctx.getImageData(0,0,c.width,c.height).data;
        var sum=0,cnt=0,maxv=0;
        for(var i=0;i<d.length;i+=160){sum+=d[i]+d[i+1]+d[i+2];cnt+=3;var m=Math.max(d[i],d[i+1],d[i+2]);if(m>maxv)maxv=m;}
        out.push({url:location.href.slice(0,44),paused:v.paused,t:Math.round(v.currentTime),avg:Math.round(sum/cnt),max:maxv,w:v.videoWidth,h:v.videoHeight,err:e});
      }
      return JSON.stringify(out);
    })()`, returnByValue: true });
    console.log("sample", k, ":", r.result && r.result.result && r.result.result.value);
    await new Promise(r2 => setTimeout(r2, 2000));
  }
  ws.close();
  process.exit(0);
}

main().catch(e => { console.log("ERR", e.message); process.exit(1); });
