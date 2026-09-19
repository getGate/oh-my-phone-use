'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {simulateCalibration,travel}=require('./simulated_ipad');

function assertCalibrated(run) {
 assert.equal(run.outcome.status,'Calibration complete');
 assert.equal(run.counters.clicks,6);assert.equal(run.counters.unpermitted,0);
 assert(run.profile,'the control server accepts the saved profile');
 assert(run.outcome.maxError<=3);
 // The measured curve follows the simulated acceleration within the reporting resolution.
 for(const v of run.profile.curve) assert(Math.abs(v.output-travel(v.input))<=1,`curve at ${v.input} counts`);
 assert.equal(run.profile.openLoop.moves,4);assert(run.profile.openLoop.maxError<=4);
 assert.equal(run.traces.length,1);assert.equal(run.traces[0].error,null);
}
test('calibration worker against a simulated iPad',{concurrency:true},async t=>{
 await Promise.all([
  t.test('native pointer',async()=>assertCalibrated(await simulateCalibration())),
  t.test('AssistiveTouch clicks hide the pointer; the report that reveals it is dropped',async()=>{
   const run=await simulateCalibration({touchClicks:true,hiddenReport:'dropped'});
   assertCalibrated(run);
   assert(run.traces[0].wakes.filter(w=>w.reason==='after click').length>=5);
  }),
  t.test('AssistiveTouch clicks hide the pointer; the report that reveals it moves it unreported',async()=>
   assertCalibrated(await simulateCalibration({touchClicks:true,hiddenReport:'applied'}))),
  t.test('samples that trail the input tool completion, as over BLE',async()=>
   assertCalibrated(await simulateCalibration({touchClicks:true,sampleDelayMs:15,
    env:{CALIBRATION_SETTLE_MS:'25',CALIBRATION_FRESH_WAIT_MS:'300',CALIBRATION_WAKE_WAIT_MS:'150'}}))),
  t.test('unobserved moves are measured again instead of ending the run',async()=>{
   const run=await simulateCalibration({touchClicks:true,lostSamples:[20,21,47]});
   assertCalibrated(run);
   assert(run.traces[0].samples.some(s=>s.to===null));
   assert(run.traces[0].wakes.some(w=>w.reason==='move not observed'));
  }),
  t.test('reports too slow to show up as travel still give a usable curve',async()=>{
   const run=await simulateCalibration({acceleration:n=>n<2?0.02:travel(n)});
   assert.equal(run.outcome.status,'Calibration complete');
   const raw=run.traces[0].samples.filter(s=>s.to&&Math.hypot(s.dx,s.dy)===1).slice(-4);
   assert(raw.every(s=>s.to[0]===s.from[0]&&s.to[1]===s.from[1]),'one count measured as no travel');
   assert(run.profile,'the control server accepts the saved profile');
   assert.equal(run.profile.curve[0].output,run.profile.curve[1].output/2);
  }),
  t.test('a click the page never sees is not repeated and leaves its measurements behind',async()=>{
   const run=await simulateCalibration({touchClicks:true,ignoreClicks:true});
   assert.match(run.outcome.status,/^Click was not seen by the calibration page; not repeated \(Verifying target 1 of 6\)/);
   assert.equal(run.counters.clicks,1);assert.equal(run.profile,null);
   assert.equal(run.traces.length,1);assert.match(run.traces[0].error,/^Click was not seen/);
   assert(run.traces[0].samples.length>=48);
  }),
 ]);
});
