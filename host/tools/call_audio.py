#!/usr/bin/env python3
"""
Call audio test tool for the Oh My Phone Use HFP firmware (build with sdkconfig.hfp_test).

The firmware is a Bluetooth hands-free unit for the iPhone. This tool talks to it over the USB
serial port: it shows call events, selects the audio test mode, records the call to a WAV file
and plays a WAV file (or macOS text-to-speech) into the call.

Examples:
    python3 host/tools/call_audio.py monitor                 # events, call state, level meter
    python3 host/tools/call_audio.py selftest                # during a call: beep, echo and speech in one pass
    python3 host/tools/call_audio.py tone                    # firmware sends a 400 Hz beep into the call
    python3 host/tools/call_audio.py loop                    # firmware echoes the call audio after 0.5 s
    python3 host/tools/call_audio.py record call.wav         # record the far end until Ctrl-C
    python3 host/tools/call_audio.py record call.wav -s 20   # ... for 20 seconds
    python3 host/tools/call_audio.py play hello.wav          # send a WAV file into the call
    python3 host/tools/call_audio.py play hello.wav --record call.wav
    python3 host/tools/call_audio.py say "Hello, this is a test."   # macOS text-to-speech into the call
    python3 host/tools/call_audio.py off                     # silence, stop the audio lines
    python3 host/tools/call_audio.py dial 10086 | answer | hangup | status

One question-and-answer turn in a live call (the building block for an agent):
    python3 host/tools/call_audio.py ask "您好，请问明天下午方便吗？请在嘀声后回答。" --voice Tingting \\
            --closing "好的，收到，谢谢，再见！"
  waits until a call is active, speaks the text and a beep, records the answer until the other
  party stops talking, speaks the closing text, gives the call audio back to the iPhone, and
  prints two transcripts (speech-to-text with whisper.cpp on this machine):
    TRANSCRIPT_CALL  everything the other party said from the start of the call to the end of
                     the answer; the answer is its last part. Use this one: the context makes
                     short answers such as "可以" reliable.
    TRANSCRIPT_CLIP  the answer recording alone; often wrong for answers of a few words.
    python3 host/tools/call_audio.py transcribe answer.wav   # speech-to-text of a recording
    python3 host/tools/call_audio.py transcribe call_full.wav --until 25.1   # only the first 25.1 s

ask/transcribe need ffmpeg, whisper-cli (brew install whisper-cpp) and a ggml model, by default
~/.cache/phoneuse/models/ggml-large-v3-turbo-q5_0.bin (or --model / $PHONEUSE_WHISPER_MODEL).

The serial port is taken from -p/--port, else $PHONEUSE_PORT, else auto-detected.
Audio on the wire: 8 kHz mono, G.711 mu-law, 20 ms per line ("A <base64>").
"""
import argparse
import array
import base64
import math
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import wave

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required:  python3 -m pip install pyserial")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from control import find_port  # noqa: E402

DEFAULT_BAUD = 230400
SAMPLE_RATE = 8000
FRAME_SAMPLES = 160                      # 20 ms
FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE
PREROLL_FRAMES = 10                      # sent at once, so the firmware jitter buffer stays ahead
DEFAULT_MODEL = "~/.cache/phoneuse/models/ggml-large-v3-turbo-q5_0.bin"   # whisper.cpp model for ask/transcribe
RESPONSE_WORDS = ("OK", "ERR", "PONG", "STATUS", "WIFI", "SECRET", "CALL", "AUDIO")

# ---- G.711 mu-law ---------------------------------------------------------------------------

_BIAS = 0x84
_CLIP = 32635


def _ulaw_encode_sample(sample):
    sign = 0x80 if sample < 0 else 0
    sample = min(abs(sample), _CLIP) + _BIAS
    exponent = 7
    mask = 0x4000
    while exponent > 0 and not sample & mask:
        exponent -= 1
        mask >>= 1
    mantissa = (sample >> (exponent + 3)) & 0x0F
    return ~(sign | (exponent << 4) | mantissa) & 0xFF


def _ulaw_decode_byte(value):
    value = ~value & 0xFF
    sample = ((((value & 0x0F) << 3) + _BIAS) << ((value >> 4) & 0x07)) - _BIAS
    return -sample if value & 0x80 else sample


ULAW_ENCODE = bytes(_ulaw_encode_sample(s) for s in range(-32768, 32768))   # index: sample + 32768
ULAW_DECODE = [_ulaw_decode_byte(b) for b in range(256)]


def ulaw_encode(samples):
    return bytes(ULAW_ENCODE[s + 32768] for s in samples)


def ulaw_decode(data):
    return array.array("h", (ULAW_DECODE[b] for b in data))


# ---- WAV helpers ----------------------------------------------------------------------------

def load_wav_8k_mono(path):
    """Read a 16-bit PCM WAV file and return 8 kHz mono samples (linear resampling)."""
    with wave.open(path, "rb") as wav:
        if wav.getsampwidth() != 2 or wav.getcomptype() != "NONE":
            raise ValueError("%s: only 16-bit PCM WAV files are supported" % path)
        channels, rate = wav.getnchannels(), wav.getframerate()
        pcm = array.array("h")
        pcm.frombytes(wav.readframes(wav.getnframes()))
    if sys.byteorder == "big":
        pcm.byteswap()
    if channels > 1:
        pcm = array.array("h", (sum(pcm[i:i + channels]) // channels for i in range(0, len(pcm), channels)))
    if rate == SAMPLE_RATE or not pcm:
        return pcm
    out = array.array("h")
    step = rate / SAMPLE_RATE
    last = len(pcm) - 1
    for n in range(int(len(pcm) / step)):
        pos = n * step
        i = int(pos)
        frac = pos - i
        nxt = pcm[min(i + 1, last)]
        out.append(int(pcm[i] + (nxt - pcm[i]) * frac))
    return out


class WavWriter:
    def __init__(self, path):
        self.path = path
        self.samples = 0
        self.wav = wave.open(path, "wb")
        self.wav.setnchannels(1)
        self.wav.setsampwidth(2)
        self.wav.setframerate(SAMPLE_RATE)

    def write(self, pcm):
        if sys.byteorder == "big":
            pcm = array.array("h", pcm)
            pcm.byteswap()
        self.wav.writeframes(pcm.tobytes())
        self.samples += len(pcm)

    def close(self):
        self.wav.close()


# ---- serial link ----------------------------------------------------------------------------

class Link:
    """Serial link with a reader thread: responses, events and audio lines are separated."""

    def __init__(self, port, baud, verbose=False):
        # pyserial's default DTR/RTS state does not reset the board (see host/tools/control.py).
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.verbose = verbose
        self.responses = queue.Queue()
        self.sinks = []                  # objects with write(pcm): every audio line from the phone goes to them
        self.peak = 0
        self.audio_lines = 0
        self.bad_lines = 0
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def close(self):
        self.running = False
        self.thread.join(timeout=1.0)
        self.ser.close()

    def _reader(self):
        buf = b""
        while self.running:
            try:
                data = self.ser.read(4096)
            except (serial.SerialException, OSError):
                break
            if not data:
                continue
            buf += data
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                self._handle(raw.decode("ascii", "replace").strip())

    def _handle(self, text):
        if not text:
            return
        word, _, rest = text.partition(" ")
        if word == "A":
            try:
                pcm = ulaw_decode(base64.b64decode(rest, validate=True))
            except ValueError:
                self.bad_lines += 1
                return
            with self.lock:
                self.audio_lines += 1
                if pcm:
                    self.peak = max(self.peak, max(abs(min(pcm)), max(pcm)))
                for sink in self.sinks:
                    sink.write(pcm)
        elif word == "EVT":
            print("%s  %s" % (time.strftime("%H:%M:%S"), text), flush=True)
        elif word in RESPONSE_WORDS:
            self.responses.put(text)
        elif self.verbose:
            print("  # " + text, file=sys.stderr, flush=True)

    def take_peak(self):
        with self.lock:
            peak, self.peak = self.peak, 0
        return peak

    def command(self, line, timeout=3.0):
        while not self.responses.empty():
            self.responses.get_nowait()
        self.ser.write((line + "\n").encode("ascii"))
        try:
            return self.responses.get(timeout=timeout)
        except queue.Empty:
            return None

    def must(self, line):
        response = self.command(line)
        if response is None:
            raise RuntimeError("no response to %r (wrong baud rate or firmware? this tool needs the "
                               "sdkconfig.hfp_test build at %d baud)" % (line, self.ser.baudrate))
        if response.startswith("ERR"):
            raise RuntimeError("%s -> %s" % (line, response))
        return response

    def send_audio(self, pcm_frame):
        self.ser.write(b"A " + base64.b64encode(ulaw_encode(pcm_frame)) + b"\n")


def meter(peak):
    width = 30
    filled = min(width, int(width * peak / 32767.0 + 0.5))
    return "[%s%s] %5d" % ("#" * filled, "." * (width - filled), peak)


def print_status(link):
    print(link.must("CALL"))
    print(link.must("AUDIO"))


def wait_with_meter(link, seconds=None, quiet=False):
    """Run until Ctrl-C (or for the given time) and show the receive level twice a second."""
    end = None if seconds is None else time.monotonic() + seconds
    try:
        while end is None or time.monotonic() < end:
            time.sleep(0.5)
            if not quiet:
                print("rx %s  lines %d" % (meter(link.take_peak()), link.audio_lines), flush=True)
    except KeyboardInterrupt:
        print()


def play(link, pcm, drain=True):
    """Send the samples at real-time speed; the first frames go at once as jitter reserve."""
    frames = [pcm[i:i + FRAME_SAMPLES] for i in range(0, len(pcm), FRAME_SAMPLES)]
    if frames and len(frames[-1]) < FRAME_SAMPLES:
        frames[-1].extend([0] * (FRAME_SAMPLES - len(frames[-1])))
    print("playing %.1f s into the call" % (len(frames) * FRAME_SECONDS), flush=True)
    start = time.monotonic()
    try:
        for n, frame in enumerate(frames):
            due = start + max(0, n - PREROLL_FRAMES) * FRAME_SECONDS
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            link.send_audio(frame)
        # Let the firmware play its buffer (without drain: only the part sent ahead of time).
        time.sleep(PREROLL_FRAMES * FRAME_SECONDS + (0.3 if drain else 0.0))
    except KeyboardInterrupt:
        print()


def beep(seconds=0.25, freq=1000, amplitude=6000):
    return array.array("h", (int(amplitude * math.sin(2 * math.pi * freq * n / SAMPLE_RATE))
                             for n in range(int(seconds * SAMPLE_RATE))))


class AnswerTap:
    """Collects the answer of the other party and finds its end from the signal level."""

    SPEECH_PEAK = 1200                   # a 20 ms line louder than this is speech (silence is below 100)

    def __init__(self):
        self.pcm = array.array("h")
        self.speech_lines = 0
        self.last_speech = None          # time of the last loud line
        self.lock = threading.Lock()

    def write(self, pcm):
        with self.lock:
            self.pcm.extend(pcm)
            if pcm and max(abs(min(pcm)), max(pcm)) > self.SPEECH_PEAK:
                self.speech_lines += 1
                self.last_speech = time.monotonic()

    def state(self):
        with self.lock:
            return self.speech_lines, self.last_speech


def wait_for_call(link, timeout):
    """Wait until a call is active and its audio is on the board."""
    print("waiting for an active call with audio on Oh My Phone Use Audio (up to %d s): place the call now" % timeout,
          flush=True)
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        status = link.must("CALL")
        fields = dict(f.split("=", 1) for f in status.split()[1:] if "=" in f)
        state = (fields.get("slc"), fields.get("setup"), fields.get("call"), fields.get("audio"))
        if state != last:
            print("%s  %s" % (time.strftime("%H:%M:%S"), status), flush=True)
            last = state
        if fields.get("slc") != "1":
            raise RuntimeError("no phone is connected to the board (slc=0)")
        if fields.get("call") == "1" and fields.get("audio") == "cvsd":
            return
        time.sleep(0.4)
    raise RuntimeError("no active call with audio on the board within %d s" % timeout)


def ask(link, opts):
    """
    One question-and-answer turn in a live call.
    Returns (spoke, answer_end): whether the other party said something after the question, and
    where the answer ends in the full recording (seconds).
    """
    question = say_to_pcm(opts.text, opts.voice)
    closing = say_to_pcm(opts.closing, opts.voice) if opts.closing else None
    full = WavWriter(opts.full)
    tap = AnswerTap()
    try:
        link.must("AUDIO STREAM")
        link.sinks = [full]
        wait_for_call(link, opts.wait)
        time.sleep(opts.greet_delay)                            # the other party says hello first

        print("asking: %s" % opts.text, flush=True)
        question.extend([0] * int(0.3 * SAMPLE_RATE))
        play(link, question, drain=False)
        with link.lock:
            link.sinks = [full, tap]                            # an early answer (before the beep) counts too
        play(link, beep())

        print("listening for the answer (up to %.0f s)" % opts.max_answer, flush=True)
        start = time.monotonic()
        while True:
            time.sleep(0.1)
            now = time.monotonic()
            speech_lines, last_speech = tap.state()
            if now - start >= opts.max_answer:
                break
            if speech_lines >= 5 and now - last_speech >= opts.end_silence:
                break                                           # they spoke, then stayed silent
            if speech_lines < 5 and now - start >= opts.no_answer:
                break                                           # nobody answered
        with link.lock:
            link.sinks = [full]
            answer_end = full.samples / SAMPLE_RATE
        speech_lines, _ = tap.state()
        print("answer: %.1f s recorded, %.1f s of it speech" % (len(tap.pcm) / SAMPLE_RATE,
                                                                speech_lines * FRAME_SECONDS), flush=True)

        if closing is not None:
            play(link, closing)
        print(link.must("AUDIO"))
        if not opts.keep_audio:
            # The hands-free unit releases the audio link: the iPhone takes the call audio back,
            # so the user can go on with the call on the phone itself.
            print("audio back to the iPhone: %s" % link.command("AUDIO DISCONNECT"))
        link.command("AUDIO OFF")
    finally:
        with link.lock:
            link.sinks = []
        full.close()

    answer = WavWriter(opts.answer)
    answer.write(tap.pcm)
    answer.close()
    print("ANSWER_WAV=%s\nFULL_WAV=%s\nANSWER_SPEECH_SECONDS=%.1f\nANSWER_END_SECONDS=%.2f" %
          (opts.answer, opts.full, speech_lines * FRAME_SECONDS, answer_end))
    return speech_lines >= 5, answer_end


def transcribe(path, language, model, until=None):
    """Speech-to-text with whisper.cpp on this machine; until = only the first seconds of the file."""
    model = os.path.expanduser(model)
    if not os.path.exists(model):
        raise RuntimeError("speech model not found: %s" % model)
    with tempfile.TemporaryDirectory() as tmp:
        wide = os.path.join(tmp, "in16k.wav")                   # whisper needs 16 kHz
        cut = ["-t", "%.2f" % until] if until else []
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error"] + cut + ["-i", path, "-ar", "16000", "-ac", "1",
                       "-c:a", "pcm_s16le", wide], check=True)
        out = subprocess.run(["whisper-cli", "-m", model, "-l", language, "-f", wide, "-nt", "-np"],
                             check=True, capture_output=True, text=True)
    return " ".join(line.strip() for line in out.stdout.splitlines() if line.strip())


def say_to_pcm(text, voice=None):
    if sys.platform != "darwin":
        raise RuntimeError("text-to-speech needs macOS; use play with a WAV file")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "say.wav")
        voice_args = ["-v", voice] if voice else []
        subprocess.run(["say"] + voice_args + ["-o", path, "--data-format=LEI16@8000", text], check=True)
        return load_wav_8k_mono(path)


def selftest(link):
    """One pass over the three downlink checks during a call; the other party only has to listen."""
    if " link=1" not in link.must("AUDIO"):
        raise RuntimeError("no audio link: place a call first (and route its audio to Oh My Phone Use Audio)")
    speech = say_to_pcm("This is the computer. One, two, three, four, five.") if sys.platform == "darwin" else None

    print("\n[1/3] TONE for 6 s: the other party hears a beep, 0.5 s on and 0.5 s off")
    link.must("AUDIO TONE")
    time.sleep(6)
    print(link.must("AUDIO"))

    print("\n[2/3] LOOP for 10 s: the other party speaks and hears the same words 0.5 s late")
    link.must("AUDIO LOOP")
    time.sleep(10)
    print(link.must("AUDIO"))

    if speech is not None:
        print("\n[3/3] SPEECH: the other party hears a computer voice that counts to five")
        link.must("AUDIO STREAM")
        play(link, speech)
        print(link.must("AUDIO"))
    link.must("AUDIO OFF")
    print("\ndone: tell Claude what the other party heard in each step")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("Examples:")[1])
    parser.add_argument("-p", "--port", help="serial port (default: $PHONEUSE_PORT or auto-detect)")
    parser.add_argument("-b", "--baud", type=int, default=int(os.environ.get("PHONEUSE_BAUD", DEFAULT_BAUD)))
    parser.add_argument("-v", "--verbose", action="store_true", help="show firmware log lines on stderr")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    sub.add_parser("monitor")
    sub.add_parser("selftest")
    for name in ("tone", "loop", "off", "answer", "hangup"):
        sub.add_parser(name)
    p = sub.add_parser("dial")
    p.add_argument("number")
    p = sub.add_parser("record")
    p.add_argument("wav")
    p.add_argument("-s", "--seconds", type=float)
    p = sub.add_parser("play")
    p.add_argument("wav")
    p.add_argument("--record", metavar="OUT.wav")
    p = sub.add_parser("say")
    p.add_argument("text")
    p.add_argument("--voice", help="macOS voice, for example Tingting for Mandarin")
    p.add_argument("--record", metavar="OUT.wav")
    p = sub.add_parser("ask", help="wait for a call, speak a question, record and transcribe the answer")
    p.add_argument("text")
    p.add_argument("--voice", help="macOS voice, for example Tingting for Mandarin")
    p.add_argument("--closing", default="", help="spoken after the answer")
    p.add_argument("--answer", default="answer.wav", metavar="OUT.wav")
    p.add_argument("--full", default="call_full.wav", metavar="OUT.wav", help="everything from the phone")
    p.add_argument("--wait", type=float, default=300, help="seconds to wait for the call (default %(default)s)")
    p.add_argument("--greet-delay", type=float, default=1.5, help="pause after the call is answered")
    p.add_argument("--max-answer", type=float, default=15)
    p.add_argument("--no-answer", type=float, default=8, help="give up when nobody spoke for this time")
    p.add_argument("--end-silence", type=float, default=1.8, help="silence that ends the answer")
    p.add_argument("--keep-audio", action="store_true", help="do not give the call audio back to the iPhone")
    p.add_argument("--language", default="zh")
    p.add_argument("--model", default=os.environ.get("PHONEUSE_WHISPER_MODEL", DEFAULT_MODEL))
    p.add_argument("--no-transcribe", action="store_true")
    p = sub.add_parser("transcribe", help="speech-to-text of a recording (whisper.cpp, on this machine)")
    p.add_argument("wav")
    p.add_argument("--until", type=float, metavar="SECONDS", help="only the first seconds of the file")
    p.add_argument("--language", default="zh")
    p.add_argument("--model", default=os.environ.get("PHONEUSE_WHISPER_MODEL", DEFAULT_MODEL))
    opts = parser.parse_args(argv)

    if opts.command == "transcribe":                            # needs no board
        try:
            print("TRANSCRIPT=%s" % transcribe(opts.wav, opts.language, opts.model, opts.until))
            return 0
        except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 1

    try:
        link = Link(opts.port or find_port(), opts.baud, opts.verbose)
    except LookupError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    except (serial.SerialException, OSError) as exc:
        print("error: cannot open serial port: %s" % exc, file=sys.stderr)
        return 2

    recorder = None
    try:
        if opts.command == "status":
            print_status(link)
        elif opts.command == "monitor":
            print_status(link)
            print("events follow; the level meter needs STREAM mode (record/play). Ctrl-C stops.")
            wait_with_meter(link, quiet=True)
            print_status(link)
        elif opts.command == "selftest":
            selftest(link)
        elif opts.command == "ask":
            spoke, answer_end = ask(link, opts)
            if not spoke:
                print("nobody answered")
            elif not opts.no_transcribe:
                # A short answer alone ("好", "可以") is often transcribed wrongly. With everything the
                # other party said before it as context the result is stable; the answer is its end.
                print("TRANSCRIPT_CALL=%s" % transcribe(opts.full, opts.language, opts.model, until=answer_end))
                print("TRANSCRIPT_CLIP=%s" % transcribe(opts.answer, opts.language, opts.model))
        elif opts.command in ("tone", "loop", "off"):
            print(link.must("AUDIO " + opts.command.upper()))
            print_status(link)
        elif opts.command in ("answer", "hangup"):
            print(link.must(opts.command.upper()))
        elif opts.command == "dial":
            print(link.must("DIAL " + opts.number))
        else:
            pcm = None
            if opts.command == "play":
                pcm = load_wav_8k_mono(opts.wav)
            elif opts.command == "say":
                pcm = say_to_pcm(opts.text, opts.voice)
            out_path = opts.wav if opts.command == "record" else opts.record
            if out_path:
                recorder = WavWriter(out_path)
                link.sinks = [recorder]
            link.must("AUDIO STREAM")
            print_status(link)
            if pcm is None:
                print("recording to %s; Ctrl-C stops" % out_path)
                wait_with_meter(link, opts.seconds)
            else:
                play(link, pcm)
            link.command("AUDIO OFF")
            time.sleep(0.2)
            print_status(link)
            if recorder:
                print("%s: %.1f s recorded" % (recorder.path, recorder.samples / SAMPLE_RATE))
        return 0
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    finally:
        with link.lock:
            link.sinks = []
        if recorder:
            recorder.close()
        link.close()


if __name__ == "__main__":
    sys.exit(main())
