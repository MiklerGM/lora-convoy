"""An offline view of recorded events; it never evaluates a channel or runs a node."""
import json
from pathlib import Path


def write_replay(events, output_path, world_config=None):
    data = {"events": sorted(events, key=lambda event: event["t_ns"]), "world": world_config or {}}
    encoded = json.dumps(data, ensure_ascii=False, allow_nan=False).replace("<", "\\u003c")
    encoded = encoded.replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    html = _HTML.replace("__RECORDED_TRACE__", encoded)
    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(html, encoding="utf-8")
    return output


_HTML = r'''<!doctype html>
<html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Recorded protocol simulation</title>
<style>
body{font:15px system-ui,sans-serif;margin:20px;color:#17263a;background:#f7f9fc}
main{max-width:1200px;margin:auto}h1{font-size:23px;margin:0 0 8px}
.controls{display:flex;gap:14px;align-items:center;flex-wrap:wrap;margin:16px 0}
input[type=range]{flex:1;min-width:200px}button,select{font:inherit;padding:6px}
canvas{display:block;width:100%;height:520px;background:white;border:1px solid #b5c4d8;border-radius:5px}
.legend{display:flex;gap:18px;flex-wrap:wrap;margin:12px 0}table{width:100%;border-collapse:collapse;margin:12px 0}
th,td{text-align:left;padding:7px;border-bottom:1px solid #d5deeb}small{color:#52637a}
.truth{color:#315a82}.known{color:#a33d96}.ok{color:#147952}.lost{color:#be3333}
</style><main>
<h1>Recorded protocol simulation</h1>
<p>Inspect a receiver's known positions beside recorded truth. Every mark comes from this trace.</p>
<div class="controls"><button id="play">Play</button><label>Receiver <select id="receiver"></select></label>
<input id="time" type="range" min="0" max="1" step="0.001" value="0" aria-label="Simulation time">
<output id="clock">0.000 s</output><label>Speed <select id="speed"><option>1</option><option>5</option><option>20</option></select>×</label></div>
<canvas id="map" aria-label="Truth and selected receiver's known positions"></canvas>
<div class="legend"><span class="truth">● Recorded truth</span><span class="known">◇ Known by receiver</span>
<span class="ok">Green line: received</span><span class="lost">Red line: lost</span><span>Amber ring: transmission</span></div>
<p id="stats"></p><small>Truth positions are held between recorded samples. Dashed lines connect known points to truth samples;
they are visual displacement, not an integrated error measurement. Radio activity shows the preceding two seconds.
The drawing uses x/y; height stays in the event data. No map service, channel model or random generator is called.</small>
<table><thead><tr><th>Time</th><th>Event</th><th>Participants</th><th>Result / state</th></tr></thead><tbody id="recent"></tbody></table>
</main><script id="trace" type="application/json">__RECORDED_TRACE__</script>
<script>
'use strict';
const data=JSON.parse(document.getElementById('trace').textContent), events=data.events;
const slider=document.getElementById('time'), receiver=document.getElementById('receiver');
const canvas=document.getElementById('map'), ctx=canvas.getContext('2d');
const nodes=new Set(), coordinates=[];
for(const e of events){
  for(const key of ['node','author','sender','receiver']) if(e[key]!==undefined&&e[key]!==null) nodes.add(String(e[key]));
  if(e.event==='positions') for(const [node,p] of Object.entries(e.positions)){nodes.add(node);coordinates.push(p)}
  if(Array.isArray(e.position)) coordinates.push(e.position);
  if(e.record&&Array.isArray(e.record.position)) coordinates.push(e.record.position);
}
for(const node of [...nodes].sort()){const option=document.createElement('option');option.value=node;option.textContent=node;receiver.append(option)}
const end=events.length?events[events.length-1].t_ns/1e9:0;slider.max=Math.max(.001,end);
const bounds=coordinates.length?coordinates.reduce((b,p)=>[Math.min(b[0],p[0]),Math.max(b[1],p[0]),Math.min(b[2],p[1]),Math.max(b[3],p[1])],[Infinity,-Infinity,Infinity,-Infinity]):[-10,10,-10,10];
let playing=false, previousFrame=null;
function project(p){
  const spanX=Math.max(10,bounds[1]-bounds[0]),spanY=Math.max(10,bounds[3]-bounds[2]);
  const scale=Math.min((canvas.width-100)/spanX,(canvas.height-90)/spanY);
  return [(p[0]-(bounds[0]+bounds[1])/2)*scale+canvas.width/2,canvas.height/2-(p[1]-(bounds[2]+bounds[3])/2)*scale];
}
function draw(){
  const width=Math.max(400,Math.round(canvas.clientWidth*devicePixelRatio)),height=Math.round(520*devicePixelRatio);
  if(canvas.width!==width||canvas.height!==height){canvas.width=width;canvas.height=height}
  const now=Number(slider.value)*1e9, selected=receiver.value, truth={}, known={}, records={}, recent=[];
  let tx=0,rx=0,lost=0;
  for(const e of events){
    if(e.t_ns>now) break;
    if(e.event==='positions') Object.assign(truth,e.positions);
    if(e.event==='source_record'){const r=e.record||e;records[r.key||r.record_key]=r}
    if(e.event==='view_reset'&&String(e.receiver)===selected){
      if(e.author===undefined)for(const author of Object.keys(known))delete known[author];else delete known[String(e.author)];
    }
    if(e.event==='applied'&&String(e.receiver)===selected){
      const r=records[e.record_key]||{};const author=e.author===undefined?r.author:e.author;
      if(author!==undefined) known[String(author)]={position:Object.hasOwn(e,'position')?e.position:r.position,state:Object.hasOwn(e,'state')?e.state:r.state,at:e.t_ns};
    }
    if(e.event==='tx'&&String(e.sender)===selected) tx++;
    if(e.event==='rx'&&String(e.receiver)===selected){if(e.accepted)rx++;else lost++}
    if(['tx','rx','state','applied'].includes(e.event)) recent.push(e);
  }
  ctx.clearRect(0,0,canvas.width,canvas.height);ctx.font=`${14*devicePixelRatio}px system-ui`;
  for(const e of recent){
    if(e.t_ns<now-2e9) continue;
    if(e.event==='rx'&&String(e.receiver)===selected&&truth[String(e.sender)]&&truth[selected]){
      const a=project(truth[String(e.sender)]),b=project(truth[selected]);
      ctx.beginPath();ctx.moveTo(...a);ctx.lineTo(...b);ctx.lineWidth=2*devicePixelRatio;
      ctx.strokeStyle=e.accepted?'#14795288':'#be333388';ctx.stroke();
    }
    if(e.event==='tx'&&truth[String(e.sender)]){
      const p=project(truth[String(e.sender)]);ctx.beginPath();ctx.arc(...p,15*devicePixelRatio,0,Math.PI*2);ctx.strokeStyle='#b57513';ctx.stroke();
    }
  }
  for(const [node,p] of Object.entries(truth)){
    const point=project(p);ctx.beginPath();ctx.arc(...point,6*devicePixelRatio,0,Math.PI*2);
    ctx.fillStyle=node===selected?'#0a345d':'#527c9f';ctx.fill();ctx.fillText(node,point[0]+9*devicePixelRatio,point[1]-7*devicePixelRatio);
  }
  for(const [node,view] of Object.entries(known)){
    if(!view.position) continue;const p=project(view.position),r=7*devicePixelRatio;
    if(truth[node]){const q=project(truth[node]);ctx.setLineDash([4*devicePixelRatio,4*devicePixelRatio]);ctx.beginPath();ctx.moveTo(...p);ctx.lineTo(...q);ctx.strokeStyle='#a33d9680';ctx.stroke();ctx.setLineDash([])}
    ctx.beginPath();ctx.moveTo(p[0],p[1]-r);ctx.lineTo(p[0]+r,p[1]);ctx.lineTo(p[0],p[1]+r);ctx.lineTo(p[0]-r,p[1]);ctx.closePath();ctx.strokeStyle='#a33d96';ctx.stroke();
    if(view.state)ctx.fillText(view.state,p[0]+9*devicePixelRatio,p[1]+15*devicePixelRatio);
  }
  document.getElementById('clock').textContent=(now/1e9).toFixed(3)+' s';
  document.getElementById('stats').textContent=`Receiver ${selected}: ${Object.keys(known).length} known participants; ${tx} transmissions; ${rx} received, ${lost} failed radio receptions.`;
  const body=document.getElementById('recent');body.replaceChildren();
  for(const e of recent.slice(-10).reverse()){
    const row=document.createElement('tr');
    for(const value of [(e.t_ns/1e9).toFixed(3),e.event,[e.sender??e.author??e.node,e.receiver].filter(x=>x!==undefined).join(' → '),e.reason??e.state??'']){const cell=document.createElement('td');cell.textContent=String(value);row.append(cell)}
    body.append(row);
  }
}
function frame(timestamp){
  if(playing&&previousFrame!==null){slider.value=Math.min(end,Number(slider.value)+(timestamp-previousFrame)/1000*Number(document.getElementById('speed').value));if(Number(slider.value)>=end){playing=false;document.getElementById('play').textContent='Play'}draw()}
  previousFrame=timestamp;requestAnimationFrame(frame);
}
document.getElementById('play').onclick=()=>{if(Number(slider.value)>=end)slider.value=0;playing=!playing;previousFrame=null;document.getElementById('play').textContent=playing?'Pause':'Play';draw()};
slider.oninput=draw;receiver.onchange=draw;window.addEventListener('resize',draw);draw();requestAnimationFrame(frame);
</script></html>'''
