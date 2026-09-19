'use strict';
// Test double for the hardware path: a control server /actions endpoint, an input tool and an
// iPad calibration page, driven by the real calibration service and worker. No HID is touched.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const {once} = require('node:events');
const {spawn} = require('node:child_process');
const {WebSocket} = require('ws');
const {encodeActions} = require('ipad_input_device/actions');
const {createCalibration} = require('./server');
const {CalibrationStore} = require('./store');

const deviceID = '00000000-0000-0000-0000-000000000001';
// iPadOS-like pointer acceleration: UIKit points travelled for one report of n counts.
const travel = n => n * (0.4 + 2 * (1 - Math.exp(-n / 25)));

// options.touchClicks: clicks arrive as direct touches (AssistiveTouch). A direct touch hides the
//   pointer; the next report only brings it back and is not reported as hover.
// options.hiddenReport: 'dropped' (the pointer stays put) or 'applied' (it moves, unreported).
// options.lostSamples: 1-based indexes of visible reports whose hover sample never arrives.
// options.ignoreClicks: the page never sees a click (button not mapped to a tap).
// options.sampleDelayMs: hover samples trail the input tool's completion, as over BLE.
// options.quantum: resolution of reported positions in points (0.5 on a 2x display).
// options.acceleration: replaces `travel`.
async function simulateCalibration({touchClicks = false, hiddenReport = 'dropped', lostSamples = [], ignoreClicks = false,
  sampleDelayMs = 0, quantum = 0.5, acceleration = travel, width = 1194, height = 834, script = null, env = {}, timeoutMs = 60000} = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'calibration-sim-'));
  const calibration = new CalibrationStore(directory);
  // The page was opened with a finger, so the pointer starts hidden and unreported.
  const pointer = {x: 830.5, y: 212, hidden: true, buttons: 0};
  const counters = {reports: 0, visibleReports: 0, clicks: 0, actions: 0, unpermitted: 0};
  const lost = new Set(lostSamples);
  let page = null;
  const publish = (type, delay = sampleDelayMs) => {
    const message = JSON.stringify({type, x: Math.round(pointer.x / quantum) * quantum, y: Math.round(pointer.y / quantum) * quantum,
      width, height, dpr: 2, pointerType: 'mouse', deviceID});
    setTimeout(() => {if (page?.readyState === WebSocket.OPEN) page.send(message);}, delay);
  };
  function report(buttons, dx, dy) {
    counters.reports++;
    if (dx || dy) {
      const counts = Math.hypot(dx, dy), points = acceleration(counts);
      const apply = () => {
        pointer.x = Math.min(width, Math.max(0, pointer.x + dx / counts * points));
        pointer.y = Math.min(height, Math.max(0, pointer.y + dy / counts * points));
      };
      if (pointer.hidden) {
        pointer.hidden = false;
        if (hiddenReport === 'applied') apply();
      } else {
        apply();
        if (!lost.has(++counters.visibleReports)) publish('pointer');
      }
    }
    if (pointer.buttons & 1 && !(buttons & 1)) {
      counters.clicks++;
      if (!ignoreClicks) {
        publish('click');
        if (touchClicks) pointer.hidden = true;
      }
    }
    pointer.buttons = buttons;
  }
  const relay = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    const send = (status, value) => {response.writeHead(status, {'Content-Type': 'application/json'});response.end(JSON.stringify(value));};
    if (request.url !== '/actions') {send(202, {status: 'stop_requested'}); return;}
    counters.actions++;
    if (!calibration.permits(deviceID, request.headers['x-calibration-run'])) {
      counters.unpermitted++;
      send(428, {code: 'calibration_required', error: 'Complete pointer calibration in the iPad app before issuing pointer actions.'}); return;
    }
    const records = Buffer.from(encodeActions(JSON.parse(body).actions), 'hex');
    for (let i = 0; i < records.length; i += 5) {
      if (records[i] === 2) report(records[i + 1], records.readInt8(i + 2), records.readInt8(i + 3));
    }
    setTimeout(() => send(200, {id: 'simulated', status: 'completed'}), 1);
  });
  relay.listen(0, '127.0.0.1'); await once(relay, 'listening');
  const app = createCalibration({calibration, stopInput: () => {}, launch: (executable, args, options) =>
    spawn(executable, script ? [script, ...args.slice(1)] : args, {...options, env: {...options.env,
      PORT: String(relay.address().port), CONTROL_SERVER_STATE_DIR: directory, CALIBRATION_POLL_MS: '2',
      CALIBRATION_SETTLE_MS: '8', CALIBRATION_FRESH_WAIT_MS: '250', CALIBRATION_WAKE_WAIT_MS: '100', ...env}})});
  app.server.listen(0, '127.0.0.1'); await once(app.server, 'listening');
  try {
    const statuses = [];
    const finished = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('Simulated calibration timed out: ' + statuses.at(-1))), timeoutMs);
      page = new WebSocket(`ws://127.0.0.1:${app.server.address().port}/native`);
      page.on('error', reject);
      page.on('message', data => {
        const message = JSON.parse(data);
        if (message.type === 'ready') {
          page.send(JSON.stringify({type: 'viewport', x: null, y: null, width, height, dpr: 2, pointerType: 'mouse', deviceID}));
          page.send(JSON.stringify({type: 'start'}));
        } else if (message.type === 'run') {
          statuses.push(message.status);
          if (!message.running) {clearTimeout(timer); resolve(message);}
        }
      });
    });
    const outcome = await finished;
    const traces = fs.readdirSync(directory).filter(f => /^calibration-\d+\.json$/.test(f))
      .map(f => JSON.parse(fs.readFileSync(path.join(directory, f), 'utf8')));
    return {outcome, statuses, counters, profile: calibration.profile(deviceID), traces};
  } finally {
    page?.terminate(); app.close(); relay.closeAllConnections(); relay.close();
    fs.rmSync(directory, {recursive: true, force: true});
  }
}
module.exports = {simulateCalibration, travel};
