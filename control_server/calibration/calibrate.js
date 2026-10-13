'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {stateDirectory} = require('../config');
const {chooseMove} = require('./mapping');
const {validID, validProfile} = require('./store');
const {moveBetween} = require('../high_level_actions');
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function request(port, route, body) {
  const res = await fetch(`http://127.0.0.1:${port}${route}`, {
    method: body ? 'POST' : 'GET', headers: {'Content-Type': 'application/json',
      ...(process.env.CALIBRATION_RUN ? {'X-Calibration-Run': process.env.CALIBRATION_RUN} : {})},
    body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(10000)});
  const value = await res.json();
  if (!res.ok) throw Error(value.error || JSON.stringify(value));
  return value;
}
const calibrationPort = Number(process.env.CALIBRATION_PORT || 8766);
const state = () => request(calibrationPort, '/state');
const action = actions => request(Number(process.env.PORT || 8765), '/actions', {actions});
const move = (dx, dy) => action([{type: 'move', dx, dy}]);
const setting = (name, fallback) => Number(process.env[name] || fallback);
// Over USB a report shows up as a pointer sample within milliseconds. Through a Wi-Fi + BLE
// input tool it can trail the tool's completion by a connection interval or more, and one action
// may produce several samples, so a position is read once the samples stop, not at the first one.
const FRESH_WAIT_MS = setting('CALIBRATION_FRESH_WAIT_MS', 2500);
const SETTLE_MS = setting('CALIBRATION_SETTLE_MS', 120);
const POLL_MS = setting('CALIBRATION_POLL_MS', 30);
const WAKE_WAIT_MS = setting('CALIBRATION_WAKE_WAIT_MS', 1200);
// Moves are harmless to repeat inside the calibration surface (the position is read back every
// time), so an unobserved move is recovered from, up to this many times. Clicks are never repeated.
const MAX_UNOBSERVED = 8;
const trace = {mode: 'feedback', startedAt: new Date().toISOString(), stage: 'Starting', error: null,
  geometry: null, samples: [], results: [], openLoop: null, wakes: []};
const progress = (completed, status) => {
  trace.stage = status;
  console.log(JSON.stringify({type: 'progress', completed, total: 19, status}));
};
// The newest mouse sample after `revision` once the pointer has been quiet for SETTLE_MS; null if none arrives.
async function settled(revision, wait = FRESH_WAIT_MS) {
  const started = Date.now();
  let latest = null, changed = 0;
  for (;;) {
    const s = await state();
    if (!s.connected) throw Error('Calibration page disconnected');
    const now = Date.now();
    if (s.sample?.revision > revision && s.sample.x !== null && s.sample.pointerType === 'mouse' &&
        s.sample.revision !== latest?.revision) {latest = s.sample; changed = now;}
    if (latest && (now - changed >= SETTLE_MS || now - started >= wait + 2000)) return latest;
    if (!latest && now - started >= wait) return null;
    await sleep(POLL_MS);
  }
}
// A direct touch hides the iPadOS pointer: the finger that opened this page, and every click when
// AssistiveTouch turns the mouse button into a tap. The report that brings the pointer back is not
// reported as a position, so nudge out and back until the page sees the pointer again.
async function wake(reason) {
  for (let attempt = 1; attempt <= 4; attempt++) {
    const revision = (await state()).revision;
    const n = attempt % 2 ? 4 : -4;
    await action([{type: 'move', dx: n, dy: n}, {type: 'wait', ms: 120}, {type: 'move', dx: -n, dy: -n}]);
    const sample = await settled(revision, WAKE_WAIT_MS);
    if (sample) {trace.wakes.push({reason, stage: trace.stage, attempts: attempt}); return sample;}
  }
  throw Error('Pointer position not reported; keep the calibration page in front and check the mouse connection');
}
async function main() {
  const s = await state();
  if (!s.connected) throw Error('Open the calibration page on the iPad first');
  if (!s.sample) throw Error('Waiting for calibration surface geometry');
  if (s.sample.pointerType !== 'mouse') throw Error('Mouse telemetry required');
  if (s.sample.source !== 'native' || !validID(s.sample.deviceID)) throw Error('Native iPad calibration required');
  const geometry = trace.geometry = [s.sample.width, s.sample.height];
  const samples = trace.samples, results = trace.results;
  trace.stage = 'Finding pointer';
  let p = await wake('start');
  let gain = 1, unobserved = 0;
  const profileName = `pointer-${p.deviceID}-${p.width}x${p.height}.json`;
  const modelFile=path.join(stateDirectory,profileName);
  let model=null;
  if(fs.existsSync(modelFile)) {
    try {model=JSON.parse(fs.readFileSync(modelFile,'utf8'));} catch {}
    if(!validProfile(model)||JSON.stringify(model.geometry)!==JSON.stringify(geometry)) model=null;
  }
  // Returns the settled sample, or null when the move was not observed (the pointer is found again first).
  async function measuredMove(dx,dy) {
    const before = (await state()).sample;
    if (before.width!==geometry[0]||before.height!==geometry[1]) throw Error('Viewport changed; recalibrate');
    await move(dx,dy);
    const after = await settled(before.revision);
    if (!after) {
      samples.push({dx,dy,from:[before.x,before.y],to:null,stage:trace.stage});
      if (++unobserved > MAX_UNOBSERVED) throw Error(`Pointer position stopped updating (${unobserved} moves were not observed)`);
      p = await wake('move not observed');
      return null;
    }
    p = after;
    const distance = Math.hypot(dx,dy);
    const observed = Math.hypot(p.x-before.x,p.y-before.y)/distance;
    if (observed>0.1&&observed<8) gain=observed;
    samples.push({dx,dy,from:[before.x,before.y],to:[p.x,p.y],gain:observed});
    return p;
  }
  // Stay inside the page: never use edge saturation to establish an origin.
  async function aim(x,y,tolerance=2) {
    for(let steps=0;steps<24;) {
      p=(await state()).sample;
      const ex=x-p.x, ey=y-p.y;
      if(Math.hypot(ex,ey)<=tolerance) return steps;
      const length=Math.hypot(ex,ey), step=Math.min(length/gain,80);
      let dx=Math.round(ex/length*step),dy=Math.round(ey/length*step);
      if(model)({dx,dy}=chooseMove(ex,ey,model.curve));
      if(!dx&&!dy) {if(Math.abs(ex)>Math.abs(ey))dx=Math.sign(ex);else dy=Math.sign(ey);}
      if(await measuredMove(dx,dy)) steps++;
    }
    throw Error('Aim did not converge; no click sent');
  }
  const [w,h]=geometry;
  {
    trace.stage = 'Centering pointer';
    await aim(w/2,h/2);
    await measuredMove(5,0);await measuredMove(-5,0);
    const curve=[];
    for(const n of [1,2,4,6,8,12,16,24,32,48,64,80]) {
      trace.stage = `Measuring ${n} counts`;
      // An unobserved move may have moved the pointer anyway; keep the measurements away from the edges.
      if(Math.hypot(p.x-w/2,p.y-h/2)>60) await aim(w/2,h/2,20);
      const values=[];
      for(const [dx,dy] of [[n,0],[-n,0],[0,n],[0,-n]]) {
        for(let tries=1;!await measuredMove(dx,dy);tries++) {
          if(tries===3) throw Error(`A ${n}-count report was not observed in 3 attempts`);
        }
        const s=samples.at(-1);
        values.push(Math.hypot(s.to[0]-s.from[0],s.to[1]-s.from[1]));
      }
      values.sort((a,b)=>a-b);
      curve.push({input:n,output:(values[1]+values[2])/2});
      console.log(JSON.stringify(curve.at(-1)));
      progress(curve.length, `Measuring pointer ${curve.length} of 12`);
    }
    // Safeguard: if positions are reported more coarsely than the slowest reports travel, those measure
    // as no travel at all. The curve is linear through the origin there; it must also never decrease.
    const first=curve.find(v=>v.output>0);
    if(!first) throw Error('Pointer did not move during measurement');
    for(const [i,v] of curve.entries()) {
      if(v.input<first.input) v.output=first.output*v.input/first.input;
      if(i) v.output=Math.max(v.output,curve[i-1].output);
    }
    model={geometry,curve,deviceID:p.deviceID,source:p.source,measuredAt:new Date().toISOString(),
      units:'UIKit surface points',scope:'Relative mouse reports issued separately, at most 80 counts'};
  }
  const coordinates = [[w*.5,h*.5],[w*.25,h*.3],[w*.75,h*.3],[w*.75,h*.75],[w*.25,h*.75],[w*.5,h*.5]];
  for(const [x,y] of coordinates) {
    trace.stage = `Verifying target ${results.length+1} of 6`;
    const target=await request(calibrationPort,'/target',{x:Math.round(x),y:Math.round(y)});
    const steps=await aim(target.x,target.y);
    const revision=(await state()).revision;
    await action([{type:'click',button:'left'}]);
    let click;
    for(const deadline=Date.now()+FRESH_WAIT_MS;;) {
      click=(await state()).clicks.at(-1);
      if((click?.revision>revision&&click.targetId===target.id)||Date.now()>=deadline)break;
      await sleep(POLL_MS);
    }
    if(!click||click.revision<=revision||click.targetId!==target.id) throw Error('Click was not seen by the calibration page; not repeated');
    if(!click.hit) throw Error(`Click landed ${click.error.toFixed(1)} points from the target`);
    results.push({...click,steps,target});
    progress(12+results.length, `Verifying target ${results.length} of 6`);
    console.log(JSON.stringify({target:[target.x,target.y],actual:[click.x,click.y],error:click.error,steps}));
    if(results.length<coordinates.length) p = await wake('after click');
  }
  const maxError=Math.max(...results.map(r=>r.error));
  if(maxError>3)throw Error('Validation error exceeded 3 pixels/points; previous profile kept');
  model.validation={targets:results.length,maxError};
  if(!validProfile(model)) throw Error('Measured pointer curve is not usable; previous profile kept');
  const saveModel=()=>{
    const temporary=modelFile+'.tmp';
    fs.writeFileSync(temporary,JSON.stringify(model,null,2),{mode:0o600});
    fs.renameSync(temporary,modelFile);
  };
  saveModel();
  // The targets above were reached with position feedback. Real use has none: a move is one batch of
  // reports at the input tool's spacing. Measure how close such batches land; informative, not a gate.
  if(process.env.CALIBRATION_OPEN_LOOP!=='0') {
    trace.stage = 'Checking open-loop moves';
    try {
      const hops=[[.35,.35],[.65,.65],[.35,.65],[.35,.45],[.42,.5]].map(([fx,fy])=>({x:Math.round(w*fx),y:Math.round(h*fy)}));
      p=await wake('after click');
      await aim(hops[0].x,hops[0].y);
      const checks=[];
      for(const to of hops.slice(1)) {
        const from=(await state()).sample;
        const reports=moveBetween(from,to,model);
        await action(reports);
        const landed=await settled(from.revision);
        if(landed) p=landed; else p=await wake('open-loop move not observed');
        checks.push({from:[from.x,from.y],to:[to.x,to.y],landed:landed?[landed.x,landed.y]:null,reports:reports.length,
          error:landed?Math.hypot(landed.x-to.x,landed.y-to.y):null});
      }
      const errors=checks.map(c=>c.error).filter(Number.isFinite);
      trace.openLoop={checks};
      if(errors.length) model.openLoop={moves:errors.length,maxError:Math.max(...errors),meanError:errors.reduce((a,b)=>a+b,0)/errors.length};
    } catch(error) {trace.openLoop={...trace.openLoop,error:error.message};}
    if(model.openLoop) saveModel();
  }
  trace.stage = 'Complete';
  console.log(JSON.stringify({type:'result',maxError,targets:results.length,modelFile,openLoop:model.openLoop||null}));
}
// Every run leaves its measurements behind, most usefully the ones that failed.
function saveTrace() {
  try {
    trace.finishedAt = new Date().toISOString();
    const file=path.join(stateDirectory,'calibration-'+Date.now()+'.json');
    fs.mkdirSync(stateDirectory,{recursive:true,mode:0o700});
    fs.writeFileSync(file,JSON.stringify(trace,null,2),{mode:0o600,flag:'wx'});
    console.log('Saved measurements: '+file);
  } catch {}
}
process.once('SIGTERM',()=>{trace.error='Stopped';saveTrace();process.exit(143);});
main().then(saveTrace,e=>{
  trace.error=e.message;saveTrace();
  console.error(`${e.message} (${trace.stage})`);process.exitCode=1;
});
