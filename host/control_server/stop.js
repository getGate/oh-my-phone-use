#!/usr/bin/env node
'use strict';
// Stops the services started by `npm run up`: the MCP adapter, the calibration service and the relay.
const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const {stateDirectory} = require('./config');

const services = [['mcp_server', 'mcp_server.js'], ['calibration', path.join('calibration', 'server.js')], ['server', 'server.js']];
for (const [name, script] of services) {
  const file = path.join(stateDirectory, `${name}.pid`);
  let pid;
  try {pid = Number(fs.readFileSync(file, 'utf8'));} catch {continue;}
  let command = '';
  try {command = execFileSync('ps', ['-p', String(pid), '-o', 'command='], {encoding: 'utf8'});} catch {}
  // A stale PID file may name a process the system has since reused: only stop our own script.
  if (command.includes(path.join(__dirname, script))) {
    process.kill(pid);
    console.log(`Stopped ${name} (PID ${pid}).`);
  }
  fs.rmSync(file, {force: true});
}
