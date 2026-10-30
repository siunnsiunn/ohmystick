// btmic — Mac side of the StickS3 "BT REC" mode (see docs/BT-MIC.md).
//
// B0 (this file): IMA ADPCM codec, the 172-byte audio frame format, WAV I/O,
// local Whisper transcription and paste. `selftest` drives all of it from a WAV
// file so the pipeline can be checked without the device.
// B1 (BLE receive) is not written yet.
import AppKit
import ApplicationServices
import Foundation

// MARK: - IMA ADPCM (16 kHz mono)

private let stepTable: [Int] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
private let indexTable: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

struct AdpcmState {
    var predictor = 0
    var index = 0
}

private func clamp(_ v: Int, _ lo: Int, _ hi: Int) -> Int { min(max(v, lo), hi) }

func adpcmEncodeSample(_ sample: Int16, _ s: inout AdpcmState) -> UInt8 {
    var diff = Int(sample) - s.predictor
    var code = 0
    if diff < 0 { code = 8; diff = -diff }
    var step = stepTable[s.index]
    var vpdiff = step >> 3
    if diff >= step { code |= 4; diff -= step; vpdiff += step }
    step >>= 1
    if diff >= step { code |= 2; diff -= step; vpdiff += step }
    step >>= 1
    if diff >= step { code |= 1; vpdiff += step }
    s.predictor = clamp(s.predictor + ((code & 8) != 0 ? -vpdiff : vpdiff), -32768, 32767)
    s.index = clamp(s.index + indexTable[code], 0, 88)
    return UInt8(code)
}

func adpcmDecodeSample(_ code: UInt8, _ s: inout AdpcmState) -> Int16 {
    let c = Int(code & 0x0F)
    let step = stepTable[s.index]
    var vpdiff = step >> 3
    if c & 4 != 0 { vpdiff += step }
    if c & 2 != 0 { vpdiff += step >> 1 }
    if c & 1 != 0 { vpdiff += step >> 2 }
    s.predictor = clamp(s.predictor + (c & 8 != 0 ? -vpdiff : vpdiff), -32768, 32767)
    s.index = clamp(s.index + indexTable[c], 0, 88)
    return Int16(s.predictor)
}

// MARK: - Audio frame (docs/BT-MIC.md)

let samplesPerFrame = 320   // 20 ms at 16 kHz
let frameHeaderLen = 12
let frameLen = frameHeaderLen + samplesPerFrame / 2

enum FrameType: UInt8 { case audio = 1, end = 2, start = 3 }

func makeFrame(type: FrameType, session: UInt16, seq: UInt16,
               pcm: [Int16], state: inout AdpcmState) -> [UInt8] {
    var f = [UInt8](repeating: 0, count: type == .audio ? frameLen : frameHeaderLen)
    f[0] = 1; f[1] = type.rawValue
    f[2] = UInt8(session & 0xFF); f[3] = UInt8(session >> 8)
    f[4] = UInt8(seq & 0xFF);     f[5] = UInt8(seq >> 8)
    f[6] = 1                                     // codec: IMA ADPCM 16 kHz mono
    let p = UInt16(bitPattern: Int16(state.predictor))
    f[8] = UInt8(p & 0xFF); f[9] = UInt8(p >> 8); f[10] = UInt8(state.index)
    guard type == .audio else { return f }
    var padded = pcm
    if padded.count < samplesPerFrame { padded += [Int16](repeating: 0, count: samplesPerFrame - padded.count) }
    for i in 0..<(samplesPerFrame / 2) {
        let lo = adpcmEncodeSample(padded[2 * i], &state)
        let hi = adpcmEncodeSample(padded[2 * i + 1], &state)
        f[frameHeaderLen + i] = lo | (hi << 4)
    }
    return f
}

struct ParsedFrame {
    var type: FrameType
    var session: UInt16
    var seq: UInt16
    var pcm: [Int16]
}

func parseFrame(_ f: [UInt8]) -> ParsedFrame? {
    guard f.count >= frameHeaderLen, f[0] == 1, f[6] == 1,
          let type = FrameType(rawValue: f[1]) else { return nil }
    let session = UInt16(f[2]) | UInt16(f[3]) << 8
    let seq = UInt16(f[4]) | UInt16(f[5]) << 8
    var pcm: [Int16] = []
    if type == .audio {
        guard f.count == frameLen, f[10] <= 88 else { return nil }
        var s = AdpcmState(predictor: Int(Int16(bitPattern: UInt16(f[8]) | UInt16(f[9]) << 8)),
                           index: Int(f[10]))
        pcm.reserveCapacity(samplesPerFrame)
        for b in f[frameHeaderLen...] {
            pcm.append(adpcmDecodeSample(b & 0x0F, &s))
            pcm.append(adpcmDecodeSample(b >> 4, &s))
        }
    }
    return ParsedFrame(type: type, session: session, seq: seq, pcm: pcm)
}

// MARK: - WAV

func readWav16kMono(_ url: URL) throws -> [Int16] {
    let d = [UInt8](try Data(contentsOf: url))
    guard d.count > 44, d[0..<4] == [0x52, 0x49, 0x46, 0x46] else { throw NSError(domain: "btmic", code: 1, userInfo: [NSLocalizedDescriptionKey: "not a RIFF/WAV file"]) }
    var pos = 12
    var fmtOK = false
    while pos + 8 <= d.count {
        let id = String(bytes: d[pos..<pos + 4], encoding: .ascii) ?? ""
        let size = Int(d[pos + 4]) | Int(d[pos + 5]) << 8 | Int(d[pos + 6]) << 16 | Int(d[pos + 7]) << 24
        let body = pos + 8
        if id == "fmt " {
            let ch = Int(d[body + 2]) | Int(d[body + 3]) << 8
            let rate = Int(d[body + 4]) | Int(d[body + 5]) << 8 | Int(d[body + 6]) << 16
            let bits = Int(d[body + 14]) | Int(d[body + 15]) << 8
            fmtOK = (ch == 1 && rate == 16000 && bits == 16)
        } else if id == "data" {
            guard fmtOK else { throw NSError(domain: "btmic", code: 2, userInfo: [NSLocalizedDescriptionKey: "need 16 kHz mono 16-bit WAV"]) }
            let end = min(body + size, d.count)
            var out: [Int16] = []
            out.reserveCapacity((end - body) / 2)
            var i = body
            while i + 1 < end { out.append(Int16(bitPattern: UInt16(d[i]) | UInt16(d[i + 1]) << 8)); i += 2 }
            return out
        }
        pos = body + size + (size & 1)
    }
    throw NSError(domain: "btmic", code: 3, userInfo: [NSLocalizedDescriptionKey: "no data chunk"])
}

func writeWav16kMono(_ pcm: [Int16], to url: URL) throws {
    var d = Data()
    func u32(_ v: Int) { var x = UInt32(v).littleEndian; d.append(Data(bytes: &x, count: 4)) }
    func u16(_ v: Int) { var x = UInt16(v).littleEndian; d.append(Data(bytes: &x, count: 2)) }
    let dataBytes = pcm.count * 2
    d.append("RIFF".data(using: .ascii)!); u32(36 + dataBytes)
    d.append("WAVEfmt ".data(using: .ascii)!); u32(16); u16(1); u16(1)
    u32(16000); u32(32000); u16(2); u16(16)
    d.append("data".data(using: .ascii)!); u32(dataBytes)
    pcm.withUnsafeBufferPointer { d.append(Data(buffer: $0)) }
    try d.write(to: url)
}

// MARK: - Whisper + paste

func transcribe(wav: URL, language: String, model: String) throws -> String {
    let outDir = wav.deletingLastPathComponent()
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    p.arguments = ["mlx_whisper", wav.path, "--model", model, "--language", language,
                   "--output-format", "txt", "--output-dir", outDir.path]
    var env = ProcessInfo.processInfo.environment
    env["PATH"] = "\(NSHomeDirectory())/.pyenv/shims:/opt/homebrew/bin:/usr/local/bin:" + (env["PATH"] ?? "")
    p.environment = env
    p.standardOutput = FileHandle.nullDevice
    let err = Pipe(); p.standardError = err
    try p.run(); p.waitUntilExit()
    guard p.terminationStatus == 0 else {
        let msg = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        throw NSError(domain: "btmic", code: 4, userInfo: [NSLocalizedDescriptionKey: "mlx_whisper failed: \(msg.suffix(300))"])
    }
    let txt = outDir.appendingPathComponent(wav.deletingPathExtension().lastPathComponent + ".txt")
    return (try String(contentsOf: txt, encoding: .utf8)).trimmingCharacters(in: .whitespacesAndNewlines)
}

/// Copies to the clipboard, and pastes (Cmd+V, optional Return) only when this
/// process has the Accessibility permission. Never pastes silently otherwise.
func deliver(_ text: String, pressReturn: Bool) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(text, forType: .string)
    guard AXIsProcessTrusted() else {
        FileHandle.standardError.write(Data("(no Accessibility permission: text copied, not pasted)\n".utf8))
        return
    }
    func key(_ code: CGKeyCode, flags: CGEventFlags = []) {
        for down in [true, false] {
            let e = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down)
            e?.flags = flags
            e?.post(tap: .cghidEventTap)
        }
    }
    key(9, flags: .maskCommand)             // V
    if pressReturn { usleep(80_000); key(36) }
}

// MARK: - Entry point

func fail(_ s: String) -> Never { FileHandle.standardError.write(Data((s + "\n").utf8)); exit(1) }

let args = Array(CommandLine.arguments.dropFirst())
let language = ProcessInfo.processInfo.environment["BTMIC_LANG"] ?? "zh"
let model = ProcessInfo.processInfo.environment["BTMIC_MODEL"] ?? "mlx-community/whisper-medium"

switch args.first {
case "selftest":
    guard args.count >= 2 else { fail("usage: btmic selftest <16k-mono-16bit.wav> [--paste]") }
    do {
        let src = try readWav16kMono(URL(fileURLWithPath: args[1]))
        // Encode into frames, "send" them as bytes, parse and decode on the other side.
        var enc = AdpcmState()
        var wire: [[UInt8]] = []
        var seq: UInt16 = 0
        var i = 0
        wire.append(makeFrame(type: .start, session: 1, seq: seq, pcm: [], state: &enc)); seq += 1
        while i < src.count {
            let chunk = Array(src[i..<min(i + samplesPerFrame, src.count)])
            wire.append(makeFrame(type: .audio, session: 1, seq: seq, pcm: chunk, state: &enc)); seq += 1
            i += samplesPerFrame
        }
        wire.append(makeFrame(type: .end, session: 1, seq: seq, pcm: [], state: &enc))
        var out: [Int16] = []
        var expect: UInt16 = 1
        var gaps = 0
        for f in wire {
            guard let pf = parseFrame(f) else { fail("frame parse failed") }
            if pf.type == .audio {
                if pf.seq != expect { gaps += 1 }
                expect = pf.seq &+ 1
                out += pf.pcm
            }
        }
        out = Array(out.prefix(src.count))
        var sig = 0.0, noise = 0.0
        for (a, b) in zip(src, out) { sig += Double(a) * Double(a); let e = Double(a) - Double(b); noise += e * e }
        let snr = noise > 0 ? 10 * log10(sig / noise) : 99
        print(String(format: "frames=%d wire=%d bytes (%.1f kbps) gaps=%d SNR=%.1f dB",
                     wire.count, wire.reduce(0) { $0 + $1.count },
                     Double(wire.reduce(0) { $0 + $1.count }) * 8 / (Double(src.count) / 16000) / 1000,
                     gaps, snr))
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("btmic-\(getpid())")
        try FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        let wav = tmp.appendingPathComponent("rec.wav")
        try writeWav16kMono(out, to: wav)
        let text = try transcribe(wav: wav, language: language, model: model)
        print("text: \(text)")
        if args.contains("--paste") { deliver(text, pressReturn: false) }
        print("temp: \(tmp.path)   (move to Trash when done)")
    } catch { fail("error: \(error.localizedDescription)") }
case "listen":
    fail("listen (BLE receive) is B1 and not implemented yet")
default:
    fail("usage: btmic selftest <wav> [--paste] | listen")
}
