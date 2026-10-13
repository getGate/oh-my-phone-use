import array
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from urllib.request import Request, urlopen
from urllib.error import HTTPError

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from phone_service import Phone, AudioBus, number, rpc, web_server


class FakeWire:
    def __init__(self):
        self.active = False
        self.slc = True
        self.commands = []
        self.audio = lambda pcm: None
        self.event = lambda event: None
        self.closed = False
        self.fail_hangup = False
        self.fail_dial = False
        self.samples = 0
    def command(self, text):
        self.commands.append(text)
        if text == 'CALL':
            return 'CALL slc=%d call=%d audio=%s setup=none held=0' % (self.slc, self.active, 'cvsd' if self.active else 'off')
        if text == 'STATUS': return 'STATUS connected=1'
        if text == 'AUDIO': return 'AUDIO mode=stream rx_drop=0'
        if text.startswith('DIAL '):
            if self.fail_dial: raise RuntimeError('ERR dial_rejected')
            self.active = True
        if text == 'HANGUP':
            if self.fail_hangup: raise RuntimeError('ERR hangup_rejected')
            self.active = False
        return 'OK'
    def send(self, pcm):
        self.samples += len(pcm)
        self.audio(array.array('h', [3000]) * len(pcm))
    def close(self): self.closed = True


class FakeSpeech:
    def check(self): pass
    def speak(self, text): return array.array('h', [1000]) * 800
    def transcribe(self, pcm): return '周五下午可以。'


class Calls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.wire = FakeWire()
        self.phone = Phone(self.wire, FakeSpeech(), self.tmp.name)
    def tearDown(self):
        self.phone.close(); self.tmp.cleanup()
    def start(self, key='task-1'):
        return self.phone.start(key, '确认周五的时间', '+1 (202) 555-0100')
    def test_complete_main_flow_and_persisted_result(self):
        call = self.start()
        turn = self.phone.turn(call['id'], '你好，我是 AI 助手，周五可以吗？', 3)
        self.assertEqual(turn['turn']['remote'], '周五下午可以。')
        self.assertTrue(turn['turn']['uncertain'])
        result = self.phone.end(call['id'], summary='对方表示周五下午可以。', outcome='completed')
        self.assertEqual(result['state'], 'ended')
        self.assertIn('HANGUP', self.wire.commands)
        self.assertEqual(self.phone.result(call['id'])['summary'], result['summary'])
        self.assertTrue(self.wire.samples)
    def test_retries_never_redial_and_conflicts_rejected(self):
        call = self.start(); self.assertEqual(self.start()['id'], call['id'])
        self.phone.end(call['id']); self.start()
        self.assertEqual(sum(x.startswith('DIAL ') for x in self.wire.commands), 1)
        with self.assertRaises(ValueError): self.phone.start('task-1', 'different', '12345')
    def test_existing_call_not_overwritten_and_attach(self):
        self.wire.active = True
        with self.assertRaises(RuntimeError): self.start()
        call = self.phone.start('attach', 'FaceTime test', attach=True)
        self.assertEqual(call['state'], 'active')
        self.assertFalse(any(x.startswith('DIAL ') for x in self.wire.commands))
    def test_dial_failure_is_not_success(self):
        self.wire.fail_dial = True
        with self.assertRaises(RuntimeError): self.start()
        self.assertEqual(self.phone.session['state'], 'failed')
        self.assertEqual(self.phone.session['outcome'], 'unknown')
    def test_hangup_failure_is_explicit(self):
        call = self.start(); self.wire.fail_hangup = True
        with self.assertRaises(RuntimeError): self.phone.end(call['id'])
        self.assertEqual(self.phone.result(call['id'])['reason'], 'hangup_unconfirmed')
    def test_remote_hangup_stops_session(self):
        call = self.start(); self.wire.active = False
        self.phone.stop.wait(2)
        self.assertEqual(self.phone.result(call['id'])['reason'], 'remote_ended_or_disconnected')
        with self.assertRaises(RuntimeError): self.phone.turn(call['id'], 'hello')
    def test_deadline_hangs_up(self):
        call = self.start(); self.phone.deadline = time.monotonic()-1
        for _ in range(30):
            if self.phone.result(call['id'])['state'] == 'ended': break
            time.sleep(.05)
        self.assertEqual(self.phone.result(call['id'])['reason'], 'time_limit')
        self.assertFalse(self.wire.active)
    def test_restart_marks_sessions_interrupted_and_does_not_redial(self):
        call = self.start()
        self.phone.shutdown.set(); self.phone.watcher.join(2)
        other = Phone(FakeWire(), FakeSpeech(), self.tmp.name)
        try:
            result = other.start('task-1', '确认周五的时间', '+12025550100')
            self.assertEqual(result['state'], 'interrupted')
            self.assertEqual(other.wire.commands, [])
        finally: other.close()
    def test_summary_after_remote_hangup_does_not_hangup_again(self):
        call=self.start();self.wire.active=False;self.phone.stop.wait(2)
        result=self.phone.end(call['id'],summary='对方已结束通话',outcome='unknown')
        self.assertEqual(result['summary'],'对方已结束通话')
        self.assertNotIn('HANGUP',self.wire.commands)
    def test_unanswered_timeout_is_terminal_and_does_not_redial(self):
        from unittest.mock import patch
        original=self.wire.command
        def command(text):
            if text.startswith('DIAL '):
                self.wire.commands.append(text)
                return 'OK'
            return original(text)
        self.wire.command=command
        with patch('phone_service.DIAL_TIMEOUT',.01):
            with self.assertRaises(RuntimeError):self.start()
        self.assertEqual(self.phone.session['state'],'failed')
        self.start()
        self.assertEqual(sum(x.startswith('DIAL ') for x in self.wire.commands),1)
    def test_goodbye_skips_listening_and_transcription(self):
        call=self.start()
        def unexpected(pcm):raise AssertionError('Goodbye must not transcribe')
        self.phone.speech.transcribe=unexpected
        result=self.phone.end(call['id'],closing_text='谢谢，再见。')
        goodbye=result['turns'][-1]
        self.assertEqual(goodbye['status'],'spoken')
        self.assertLess(goodbye['timing']['listening_s'],.1)
        self.assertEqual(goodbye['timing']['transcription_s'],0)
        self.assertIn('HANGUP',self.wire.commands)
    def test_phase_timing_includes_idle_and_synthesis(self):
        call=self.start();time.sleep(.05)
        result=self.phone.turn(call['id'],'test',listen_seconds=0)
        t=result['turn']['timing']
        for key in ('queue_s','idle_before_s','synthesis_s','playback_s','listening_s','transcription_s','total_s'):
            self.assertGreaterEqual(t[key],0)
        self.assertGreaterEqual(t['idle_before_s'],.04)
        self.assertGreater(t['total_s'],t['synthesis_s'])
    def test_authenticated_http_tool_response(self):
        server=web_server(self.phone,0)
        try:
            base,token=self.phone.listen_url.split('/#')
            body=json.dumps({'jsonrpc':'2.0','id':8,'method':'tools/call','params':{'name':'phone_status','arguments':{}}}).encode()
            with self.assertRaises(HTTPError):urlopen(Request(base+'/mcp',data=body))
            response=json.load(urlopen(Request(base+'/mcp',data=body,headers={'Authorization':'Bearer '+token})))
            self.assertEqual(response['id'],8)
            self.assertEqual(response['result']['structuredContent']['call']['call'],'0')
        finally:server.shutdown();server.server_close()
    def test_numbers_reject_commands_and_control_codes(self):
        for bad in ('123\nHANGUP', '*21*123#', 'abc', '', '12'):
            with self.assertRaises(ValueError): number(bad)
    def test_mcp_contract_and_validation(self):
        response = rpc(self.phone, {'jsonrpc':'2.0','id':1,'method':'tools/list'})
        self.assertEqual(len(response['result']['tools']), 5)
        bad = rpc(self.phone, {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'phone_start','arguments':{}}})
        self.assertTrue(bad['result']['isError'])
        self.assertIsNone(rpc(self.phone, {'jsonrpc':'2.0','method':'notifications/initialized'}))
    def test_listener_authorization_and_stop(self):
        server = web_server(self.phone, 0)
        try:
            base, token = self.phone.listen_url.split('/#')
            with self.assertRaises(HTTPError): urlopen(base+'/state')
            call = self.start()
            req = Request(base+'/state', headers={'Authorization':'Bearer '+token})
            self.assertEqual(json.load(urlopen(req))['id'], call['id'])
            forged = Request(base+'/state',headers={'Authorization':'Bearer '+token,'Origin':'https://evil.example'})
            with self.assertRaises(HTTPError):urlopen(forged)
            stop = Request(base+'/end', data=json.dumps({'call_id':call['id']}).encode(), headers={'Authorization':'Bearer '+token}, method='POST')
            self.assertEqual(json.load(urlopen(stop))['reason'], 'user_stopped')
        finally: server.shutdown(); server.server_close()
    def test_audio_fanout_is_bounded_and_starts_live(self):
        bus = AudioBus()
        for _ in range(1200):bus.put('remote',array.array('h',[100]))
        self.assertEqual(bus.read(-1)['frames'], [])
        self.assertEqual(len(bus.read(0)['frames']),1000)
        cursor=bus.read(-1)['cursor'];bus.put('agent',array.array('h',[200]))
        self.assertEqual(bus.read(cursor)['frames'][0]['side'],'agent')


class Transport(unittest.TestCase):
    def test_fragmented_serial_audio_and_commands(self):
        from unittest.mock import patch
        import queue
        from phone_service import Wire
        class Port:
            def __init__(self, *args, **kwargs):
                self.chunks = queue.Queue()
            def read(self, count):
                try: return self.chunks.get(timeout=.02)
                except queue.Empty: return b''
            def write(self, data):
                self.chunks.put(b'CA');self.chunks.put(b'LL slc=1\n')
            def close(self): pass
        with patch('phone_service.serial.Serial', Port):
            wire=Wire('fake'); received=[]; wire.audio=received.append
            try:
                wire.serial.chunks.put(b'A /');wire.serial.chunks.put(b'w==\n')
                wire.serial.chunks.put(b'A invalid!\n')
                self.assertEqual(wire.command('CALL'), 'CALL slc=1')
                self.assertEqual(len(received),1)
                self.assertEqual(wire.bad_audio_lines,1)
                self.assertFalse(wire.closed)
            finally: wire.close()

if __name__ == '__main__': unittest.main()
