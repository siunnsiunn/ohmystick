#!/usr/bin/env python3
"""winvoice: press Ctrl+F12 (the StickS3 front button in USB MIC mode), talk,
press it again; the recording goes to a cloud speech service (Azure Speech or
Groq Whisper) and the text is pasted at the cursor.

Windows 10/11 x64, Python 3.10+, one extra package: `pip install sounddevice`.
No Speech SDK and no Visual C++ runtime: everything is plain HTTPS.

Back ends (config `backend`):
  short  REST API for short audio. Works on the free F0 tier, max 60 s per
         recording, no phrase list.
  fast   Fast transcription API. Needs a paid S0 resource, takes a phrase list
         (vocab.txt) so drug names and abbreviations come out right.
  groq   Groq's Whisper large-v3. Free tier is about 8 audio hours a day; the
         vocabulary goes in as a Whisper prompt. Whisper may answer in
         Simplified Chinese, so install opencc-python-reimplemented and
         winvoice converts the result to Traditional.

Usage:
  py winvoice.py                 run (Ctrl+F12 toggles, Ctrl+Shift+F12 quits)
  py winvoice.py --list-devices  show input devices
  py winvoice.py --test          record 5 s, transcribe, print (no paste)
  py winvoice.py --file a.wav    transcribe a 16 kHz mono WAV, print
"""

from __future__ import annotations

import argparse
import array
import configparser
import io
import json
import logging
import logging.handlers
import os
import queue
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave

RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = os.name == "nt"
log = logging.getLogger("winvoice")


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

DEFAULTS = {
    "key": "",
    "region": "",
    "endpoint": "",
    "groq_key": "",
    "groq_model": "whisper-large-v3",
    "groq_url": "https://api.groq.com/openai/v1/audio/transcriptions",
    "opencc": "s2tw",
    "backend": "short",
    "language": "zh-TW",
    "microphone": "StickS3",
    "vocab_file": "vocab.txt",
    "max_seconds": "55",
    "min_seconds": "0.5",
    "silence_peak": "300",
    "output": "paste",
    "restore_clipboard": "true",
    "beep": "true",
    "log_text": "false",
    "timeout": "30",
}


class Config:
    def __init__(self, path: str | None):
        cp = configparser.ConfigParser()
        cp.read_dict({"winvoice": DEFAULTS})
        self.path = path or os.environ.get("WINVOICE_CONFIG") or os.path.join(HERE, "winvoice.ini")
        if os.path.exists(self.path):
            cp.read(self.path, encoding="utf-8-sig")
        s = cp["winvoice"]
        self.key = os.environ.get("AZURE_SPEECH_KEY") or s.get("key").strip()
        self.region = s.get("region").strip()
        self.endpoint = s.get("endpoint").strip().rstrip("/")
        self.groq_key = os.environ.get("GROQ_API_KEY") or s.get("groq_key").strip()
        self.groq_model = s.get("groq_model").strip()
        self.groq_url = s.get("groq_url").strip()
        self.opencc = s.get("opencc").strip()
        self.backend = s.get("backend").strip().lower()
        self.language = s.get("language").strip()
        self.microphone = s.get("microphone").strip()
        vf = s.get("vocab_file").strip()
        self.vocab_file = vf if not vf or os.path.isabs(vf) else os.path.join(HERE, vf)
        self.max_seconds = s.getfloat("max_seconds")
        self.min_seconds = s.getfloat("min_seconds")
        self.silence_peak = s.getint("silence_peak")
        self.output = s.get("output").strip().lower()
        self.restore_clipboard = s.getboolean("restore_clipboard")
        self.beep = s.getboolean("beep")
        self.log_text = s.getboolean("log_text")
        self.timeout = s.getfloat("timeout")
        if self.backend == "short":
            # The short-audio API rejects anything over 60 s.
            self.max_seconds = min(self.max_seconds, 58)

    def validate(self) -> list[str]:
        errs = []
        if self.backend not in ("short", "fast", "groq"):
            errs.append("backend must be short, fast or groq")
        elif self.backend == "groq":
            if not self.groq_key:
                errs.append("groq_key is empty (set it in winvoice.ini or GROQ_API_KEY)")
        else:
            if not self.key:
                errs.append("key is empty (set it in winvoice.ini or AZURE_SPEECH_KEY)")
            if not self.region and not self.endpoint:
                errs.append("set region (e.g. eastasia) or endpoint")
        if self.output not in ("paste", "type"):
            errs.append("output must be paste or type")
        return errs


def load_phrases(path: str) -> list[str]:
    if not path or not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(line)
    return out


# --------------------------------------------------------------------------
# Audio helpers
# --------------------------------------------------------------------------

def pcm_to_wav(pcm: bytes, rate: int = RATE) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


def pcm_peak(pcm: bytes) -> int:
    a = array.array("h")
    a.frombytes(pcm[: len(pcm) - len(pcm) % 2])
    if sys.byteorder != "little":
        a.byteswap()
    return max((abs(x) for x in a), default=0)


def find_input_device(name: str):
    """Index of the first input device whose name contains `name`, or None
    (= system default). MME devices come first in sounddevice's list and
    resample to 16 kHz for us, so the first match is the one we want."""
    import sounddevice as sd
    if not name:
        return None
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and name.lower() in d["name"].lower():
            return i
    log.warning("no input device matches %r, using the default microphone", name)
    return None


class Recorder:
    def __init__(self, device):
        self.device = device
        self.stream = None
        self.buf = bytearray()
        self.lock = threading.Lock()

    def _cb(self, data, frames, t, status):
        with self.lock:
            self.buf.extend(data)

    def start(self):
        import sounddevice as sd
        self.buf = bytearray()
        self.stream = sd.RawInputStream(samplerate=RATE, channels=1, dtype="int16",
                                        device=self.device, callback=self._cb)
        self.stream.start()

    def stop(self) -> bytes:
        if self.stream is not None:
            try:
                self.stream.stop()
                self.stream.close()
            finally:
                self.stream = None
        with self.lock:
            return bytes(self.buf)


# --------------------------------------------------------------------------
# Azure
# --------------------------------------------------------------------------

class TranscribeError(Exception):
    pass


def _http(req: urllib.request.Request, timeout: float) -> dict:
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")[:300]
        raise TranscribeError(f"HTTP {e.code}: {body}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise TranscribeError(f"network: {e}") from None


def short_url(cfg: Config) -> str:
    if cfg.region:
        base = f"https://{cfg.region}.stt.speech.microsoft.com"
    else:
        base = cfg.endpoint + "/stt"
    return (base + "/speech/recognition/conversation/cognitiveservices/v1"
            f"?language={cfg.language}&format=simple&profanity=raw")


def fast_url(cfg: Config) -> str:
    base = cfg.endpoint or f"https://{cfg.region}.api.cognitive.microsoft.com"
    return base + "/speechtotext/transcriptions:transcribe?api-version=2025-10-15"


def transcribe_short(wav: bytes, cfg: Config) -> str:
    req = urllib.request.Request(short_url(cfg), data=wav, method="POST", headers={
        "Ocp-Apim-Subscription-Key": cfg.key,
        "Content-Type": "audio/wav; codecs=audio/pcm; samplerate=16000",
        "Accept": "application/json",
    })
    j = _http(req, cfg.timeout)
    status = j.get("RecognitionStatus")
    if status == "Success":
        return (j.get("DisplayText") or "").strip()
    if status in ("NoMatch", "InitialSilenceTimeout", "BabbleTimeout"):
        return ""
    raise TranscribeError(f"RecognitionStatus={status}")


def multipart(fields: list[tuple[str, str]], file_field: str, wav: bytes) -> tuple[bytes, str]:
    boundary = "----winvoice" + uuid.uuid4().hex
    parts = []
    for name, value in fields:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        parts.append(value.encode("utf-8") + b"\r\n")
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{file_field}"; '
                 'filename="audio.wav"\r\nContent-Type: audio/wav\r\n\r\n'.encode())
    parts.append(wav)
    parts.append(f"\r\n--{boundary}--\r\n".encode())
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def fast_body(wav: bytes, cfg: Config, phrases: list[str]) -> tuple[bytes, str]:
    definition: dict = {"locales": [cfg.language], "profanityFilterMode": "None"}
    if phrases:
        definition["phraseList"] = {"phrases": phrases}
    return multipart([("definition", json.dumps(definition, ensure_ascii=False))], "audio", wav)


def transcribe_fast(wav: bytes, cfg: Config, phrases: list[str]) -> str:
    body, ctype = fast_body(wav, cfg, phrases)
    req = urllib.request.Request(fast_url(cfg), data=body, method="POST", headers={
        "Ocp-Apim-Subscription-Key": cfg.key,
        "Content-Type": ctype,
        "Accept": "application/json",
    })
    j = _http(req, cfg.timeout)
    texts = [p.get("text", "") for p in j.get("combinedPhrases", [])]
    return "".join(t.strip() for t in texts).strip()


def whisper_prompt(phrases: list[str]) -> str:
    """Whisper continues in the style of its prompt, so a Traditional Chinese
    sentence nudges it away from Simplified output. The vocabulary is joined
    into one sentence, as btmic does; Whisper only reads ~224 tokens of it."""
    prompt = "以下是繁體中文的口述內容。"
    if phrases:
        prompt += "，".join(phrases) + "。"
    return prompt[:150]


def whisper_language(language: str) -> str:
    # Whisper wants ISO-639-1: zh-TW -> zh
    return language.split("-")[0].lower()


def transcribe_groq(wav: bytes, cfg: Config, phrases: list[str]) -> str:
    body, ctype = multipart([
        ("model", cfg.groq_model),
        ("language", whisper_language(cfg.language)),
        ("prompt", whisper_prompt(phrases)),
        ("response_format", "json"),
        ("temperature", "0"),
    ], "file", wav)
    req = urllib.request.Request(cfg.groq_url, data=body, method="POST", headers={
        "Authorization": f"Bearer {cfg.groq_key}",
        "Content-Type": ctype,
        "Accept": "application/json",
        # Groq sits behind Cloudflare, which may refuse urllib's default agent.
        "User-Agent": "winvoice/1.0",
    })
    j = _http(req, cfg.timeout)
    return (j.get("text") or "").strip()


# What Whisper says over silence or noise: subtitle credits from its training data.
WHISPER_JUNK = ("字幕由", "Amara", "點贊", "点赞", "請訂閱", "请订阅", "訂閱我", "订阅我",
                "謝謝觀看", "谢谢观看", "感謝觀看", "感谢观看", "感謝收看", "感谢收看",
                "明鏡與點點", "明镜与点点")


def drop_whisper_junk(text: str) -> str:
    if len(text) <= 40 and any(j in text for j in WHISPER_JUNK):
        log.info("dropped a likely Whisper hallucination (%d chars)", len(text))
        return ""
    return text


_converter = None


def to_traditional(text: str, mode: str) -> str:
    """Simplified -> Traditional with OpenCC (pip install
    opencc-python-reimplemented). s2tw only swaps characters to Taiwan's
    standard forms; s2twp also rewrites vocabulary (软件 -> 軟體)."""
    global _converter
    if not mode or not text:
        return text
    if _converter is None:
        try:
            from opencc import OpenCC
            _converter = OpenCC(mode)
        except Exception as e:  # not installed, or unknown mode
            log.warning("OpenCC unavailable (%s); output is not converted", e)
            _converter = False
    return _converter.convert(text) if _converter else text


def transcribe(wav: bytes, cfg: Config, phrases: list[str]) -> str:
    if cfg.backend == "fast":
        return transcribe_fast(wav, cfg, phrases)
    if cfg.backend == "groq":
        # Azure already answers in zh-TW; only Whisper needs the conversion.
        return to_traditional(drop_whisper_junk(transcribe_groq(wav, cfg, phrases)), cfg.opencc)
    return transcribe_short(wav, cfg)


# --------------------------------------------------------------------------
# Windows: hotkey, clipboard, keystrokes, beeps
# --------------------------------------------------------------------------

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    ULONG_PTR = ctypes.c_size_t

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

    class HARDWAREINPUT(ctypes.Structure):
        _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]

    class _INPUTUNION(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]

    class INPUT(ctypes.Structure):
        _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]

    user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
    user32.SendInput.restype = wintypes.UINT
    user32.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
    user32.RegisterHotKey.restype = wintypes.BOOL
    user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
    user32.GetMessageW.restype = wintypes.BOOL
    user32.PostThreadMessageW.argtypes = [wintypes.DWORD, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.OpenClipboard.argtypes = [wintypes.HWND]
    user32.OpenClipboard.restype = wintypes.BOOL
    user32.CloseClipboard.restype = wintypes.BOOL
    user32.EmptyClipboard.restype = wintypes.BOOL
    user32.GetClipboardData.argtypes = [wintypes.UINT]
    user32.GetClipboardData.restype = wintypes.HANDLE
    user32.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
    user32.SetClipboardData.restype = wintypes.HANDLE
    user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
    user32.GetAsyncKeyState.restype = ctypes.c_short
    user32.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]
    user32.RegisterClipboardFormatW.restype = wintypes.UINT
    kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = wintypes.HGLOBAL
    kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalLock.restype = wintypes.LPVOID
    kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalFree.argtypes = [wintypes.HGLOBAL]
    kernel32.GlobalFree.restype = wintypes.HGLOBAL
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.GetCurrentThreadId.restype = wintypes.DWORD

    INPUT_KEYBOARD = 1
    KEYEVENTF_KEYUP = 0x0002
    KEYEVENTF_UNICODE = 0x0004
    VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN = 0x11, 0x10, 0x12, 0x5B, 0x5C
    VK_F12, VK_V = 0x7B, 0x56
    MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_NOREPEAT = 0x1, 0x2, 0x4, 0x4000
    WM_HOTKEY, WM_APP = 0x0312, 0x8000
    CF_UNICODETEXT, GMEM_MOVEABLE = 13, 0x0002
    ERROR_ALREADY_EXISTS = 183

    def _key(vk=0, scan=0, flags=0) -> INPUT:
        inp = INPUT(type=INPUT_KEYBOARD)
        inp.u.ki = KEYBDINPUT(wVk=vk, wScan=scan, dwFlags=flags, time=0, dwExtraInfo=0)
        return inp

    def _send(inputs: list) -> None:
        arr = (INPUT * len(inputs))(*inputs)
        if user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT)) != len(inputs):
            log.warning("SendInput was blocked (err %d)", ctypes.get_last_error())

    def wait_modifiers_released(limit: float = 1.5) -> None:
        """The hotkey itself includes Ctrl; a held modifier would turn our
        Ctrl+V or typed text into some other shortcut."""
        end = time.monotonic() + limit
        while time.monotonic() < end:
            if not any(user32.GetAsyncKeyState(vk) & 0x8000
                       for vk in (VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN)):
                return
            time.sleep(0.02)

    def _open_clipboard() -> bool:
        for _ in range(20):
            if user32.OpenClipboard(None):
                return True
            time.sleep(0.025)
        return False

    def get_clipboard_text() -> str | None:
        if not _open_clipboard():
            return None
        try:
            h = user32.GetClipboardData(CF_UNICODETEXT)
            if not h:
                return None
            p = kernel32.GlobalLock(h)
            if not p:
                return None
            try:
                return ctypes.wstring_at(p)
            finally:
                kernel32.GlobalUnlock(h)
        finally:
            user32.CloseClipboard()

    def _alloc(data: bytes):
        h = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
        if not h:
            return None
        p = kernel32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        kernel32.GlobalUnlock(h)
        return h

    def _mark_private() -> None:
        """Tell Windows this clipboard content is not for the clipboard history
        (Win+V) or the cloud clipboard, so dictated text stays out of both.
        Called with the clipboard open."""
        for name, value in (("ExcludeClipboardContentFromMonitorProcessing", 0),
                            ("CanIncludeInClipboardHistory", 0),
                            ("CanUploadToCloudClipboard", 0)):
            fmt = user32.RegisterClipboardFormatW(name)
            h = _alloc(value.to_bytes(4, "little")) if fmt else None
            if h and not user32.SetClipboardData(fmt, h):
                kernel32.GlobalFree(h)

    def set_clipboard_text(text: str, private: bool = False) -> bool:
        h = _alloc(text.encode("utf-16-le") + b"\x00\x00")
        if not h:
            return False
        if not _open_clipboard():
            kernel32.GlobalFree(h)
            return False
        try:
            user32.EmptyClipboard()
            if not user32.SetClipboardData(CF_UNICODETEXT, h):
                kernel32.GlobalFree(h)
                return False
            if private:
                _mark_private()
            return True  # the clipboard owns h now
        finally:
            user32.CloseClipboard()

    def paste_text(text: str, restore: bool) -> None:
        old = get_clipboard_text() if restore else None
        if not set_clipboard_text(text, private=True):
            log.warning("clipboard busy, typing instead")
            type_text(text)
            return
        wait_modifiers_released()
        _send([_key(VK_CONTROL), _key(VK_V), _key(VK_V, flags=KEYEVENTF_KEYUP),
               _key(VK_CONTROL, flags=KEYEVENTF_KEYUP)])
        if old is not None:
            # Give the target app time to read the clipboard before we put the
            # old text back. Only text survives; an image on the clipboard is lost.
            time.sleep(0.6)
            set_clipboard_text(old)

    def type_text(text: str) -> None:
        wait_modifiers_released()
        units = text.encode("utf-16-le")
        inputs = []
        for i in range(0, len(units), 2):
            cu = int.from_bytes(units[i:i + 2], "little")
            inputs.append(_key(scan=cu, flags=KEYEVENTF_UNICODE))
            inputs.append(_key(scan=cu, flags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP))
        for i in range(0, len(inputs), 200):
            _send(inputs[i:i + 200])

    def _beep(pattern) -> None:
        import winsound
        for f, d in pattern:
            winsound.Beep(f, d)

    def beep(kind: str) -> None:
        pattern = {"start": [(880, 90)], "stop": [(587, 90)],
                   "error": [(300, 160), (300, 160)], "ready": [(660, 60), (880, 60)]}[kind]
        threading.Thread(target=_beep, args=(pattern,), daemon=True).start()


# --------------------------------------------------------------------------
# App
# --------------------------------------------------------------------------

HOTKEY_TOGGLE, HOTKEY_QUIT = 1, 2
MSG_TOGGLE = 0x8000 + 1  # WM_APP + 1, posted by the max-length timer


class App:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.recorder = Recorder(find_input_device(cfg.microphone))
        self.recording = False
        self.timer: threading.Timer | None = None
        self.jobs: queue.Queue = queue.Queue()
        self.main_thread_id = 0
        self.generation = 0  # tells a stale max-length timer from the current one
        threading.Thread(target=self._worker, daemon=True).start()

    def notify(self, kind: str) -> None:
        if self.cfg.beep:
            beep(kind)

    def toggle(self) -> None:
        if not self.recording:
            try:
                self.recorder.start()
            except Exception as e:  # device unplugged, in use, ...
                log.error("cannot open microphone: %s", e)
                self.notify("error")
                return
            self.recording = True
            self.generation += 1
            self.notify("start")
            self.timer = threading.Timer(self.cfg.max_seconds, self._timeout, args=(self.generation,))
            self.timer.daemon = True
            self.timer.start()
            log.info("recording")
        else:
            if self.timer:
                self.timer.cancel()
                self.timer = None
            pcm = self.recorder.stop()
            self.recording = False
            self.notify("stop")
            self.jobs.put(pcm)

    def _timeout(self, generation: int) -> None:
        # Hand the stop to the main thread so toggle() only ever runs there.
        log.info("max length reached, stopping")
        user32.PostThreadMessageW(self.main_thread_id, MSG_TOGGLE, generation, 0)

    def _worker(self) -> None:
        while True:
            pcm = self.jobs.get()
            try:
                self._process(pcm)
            except Exception:
                log.exception("unexpected error")
                self.notify("error")

    def _process(self, pcm: bytes) -> None:
        secs = len(pcm) / 2 / RATE
        peak = pcm_peak(pcm)
        if secs < self.cfg.min_seconds or peak < self.cfg.silence_peak:
            # Do not spend quota on an accidental tap or a muted (flat) stick.
            log.info("skipped: %.1f s, peak %d", secs, peak)
            self.notify("error")
            return
        t0 = time.monotonic()
        try:
            text = transcribe(pcm_to_wav(pcm), self.cfg, load_phrases(self.cfg.vocab_file))
        except TranscribeError as e:
            log.error("transcription failed (%.1f s audio): %s", secs, e)
            self.notify("error")
            return
        log.info("%.1f s audio -> %d chars in %.1f s%s", secs, len(text), time.monotonic() - t0,
                 f": {text}" if self.cfg.log_text else "")
        if not text:
            self.notify("error")
            return
        if self.cfg.output == "type":
            type_text(text)
        else:
            paste_text(text, self.cfg.restore_clipboard)

    def run(self) -> int:
        self.main_thread_id = kernel32.GetCurrentThreadId()
        ok = user32.RegisterHotKey(None, HOTKEY_TOGGLE, MOD_CONTROL | MOD_NOREPEAT, VK_F12)
        ok2 = user32.RegisterHotKey(None, HOTKEY_QUIT, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12)
        if not ok:
            log.error("Ctrl+F12 is taken by another program (WeType? AutoHotkey?)")
            self.notify("error")
            return 1
        if not ok2:
            log.warning("Ctrl+Shift+F12 (quit) is taken; end winvoice from Task Manager")
        log.info("ready: Ctrl+F12 to dictate, Ctrl+Shift+F12 to quit (%s, %s)",
                 self.cfg.backend, self.cfg.language)
        self.notify("ready")
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            if msg.message == WM_HOTKEY and msg.wParam == HOTKEY_QUIT:
                break
            if (msg.message == WM_HOTKEY and msg.wParam == HOTKEY_TOGGLE) or \
               (msg.message == MSG_TOGGLE and self.recording and msg.wParam == self.generation):
                self.toggle()
        if self.recording:
            self.recorder.stop()
        log.info("quit")
        return 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def setup_logging(console: bool) -> None:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    d = os.path.join(base, "winvoice")
    os.makedirs(d, exist_ok=True)
    fh = logging.handlers.RotatingFileHandler(os.path.join(d, "winvoice.log"),
                                              maxBytes=512 * 1024, backupCount=2, encoding="utf-8")
    handlers: list[logging.Handler] = [fh]
    if console and sys.stderr is not None:
        handlers.append(logging.StreamHandler())
    logging.basicConfig(level=logging.INFO, handlers=handlers,
                        format="%(asctime)s %(levelname)s %(message)s")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Ctrl+F12 dictation via Azure Speech or Groq")
    ap.add_argument("--config", help="path to winvoice.ini")
    ap.add_argument("--list-devices", action="store_true", help="list input devices")
    ap.add_argument("--test", action="store_true", help="record 5 s, transcribe, print")
    ap.add_argument("--file", help="transcribe a 16 kHz mono 16-bit WAV and print")
    args = ap.parse_args(argv)
    setup_logging(console=True)

    if args.list_devices:
        import sounddevice as sd
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] > 0:
                api = sd.query_hostapis(d["hostapi"])["name"]
                print(f"{i:3d}  {d['name']}  [{api}]")
        return 0

    cfg = Config(args.config)
    errs = cfg.validate()
    if errs:
        for e in errs:
            log.error("config %s: %s", cfg.path, e)
        return 2
    phrases = load_phrases(cfg.vocab_file)
    if phrases and cfg.backend == "short":
        log.info("vocab.txt is ignored by backend=short (fast and groq use it)")

    if args.file:
        with open(args.file, "rb") as f:
            print(transcribe(f.read(), cfg, phrases))
        return 0

    if args.test:
        rec = Recorder(find_input_device(cfg.microphone))
        print("Recording 5 s, talk now...")
        rec.start()
        time.sleep(5)
        pcm = rec.stop()
        print(f"peak {pcm_peak(pcm)} (below {cfg.silence_peak} counts as silence)")
        t0 = time.monotonic()
        print(transcribe(pcm_to_wav(pcm), cfg, phrases))
        print(f"({time.monotonic() - t0:.1f} s)")
        return 0

    if not IS_WINDOWS:
        log.error("the hotkey mode only runs on Windows")
        return 1
    kernel32.CreateMutexW(None, False, "Local\\winvoice-ohmystick")
    if ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
        log.error("winvoice is already running")
        return 1
    return App(cfg).run()


if __name__ == "__main__":
    sys.exit(main())
