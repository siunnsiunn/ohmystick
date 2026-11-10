// btmic — Mac side of the StickS3 "BT REC" mode (see docs/BT-MIC.md).
//
// B0 (this file): IMA ADPCM codec, the 172-byte audio frame format, WAV I/O,
// local Whisper transcription and paste. `selftest` drives all of it from a WAV
// file so the pipeline can be checked without the device.
// B1 (`listen`): CoreBluetooth receiver for the stick's custom service.
import AppKit
import CoreBluetooth
import ApplicationServices
import CryptoKit
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

let maxSamples = 16000 * 70
let flagOffline: UInt8 = 0x01     // frame byte 7: a recording stored on the stick

struct ParsedFrame {
    var flags: UInt8 = 0
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
    return ParsedFrame(flags: f[7], type: type, session: session, seq: seq, pcm: pcm)
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

/// Words and short phrases the recogniser should get right (abbreviations with
/// their capitalisation, drug names, usual clinical phrases), one per line in
/// ~/.btmic-vocab.txt. They are joined into a plain sentence: a bare word list
/// is the weakest kind of prompt, and a long one makes the model parrot it back.
func vocabularyPrompt() -> String? {
    guard let raw = try? String(contentsOfFile: NSHomeDirectory() + "/.btmic-vocab.txt", encoding: .utf8) else { return nil }
    let lines = raw.split(separator: "\n")
        .map { $0.trimmingCharacters(in: .whitespaces) }
        .filter { !$0.isEmpty && !$0.hasPrefix("#") }
    guard !lines.isEmpty else { return nil }
    return String((lines.joined(separator: "，") + "。").prefix(300))
}

func transcribe(wav: URL, language: String, model: String) throws -> String {
    let outDir = wav.deletingLastPathComponent()
    let env0 = ProcessInfo.processInfo.environment
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    var args = ["mlx_whisper", wav.path, "--model", model, "--language", language,
                "--output-format", "txt", "--output-dir", outDir.path,
                // a repeated prompt word can otherwise snowball into a loop
                "--condition-on-previous-text", "False"]
    // "=" form: a vocabulary line that starts with "-" must not be read as an option
    if let vocab = vocabularyPrompt() { args.append("--initial-prompt=\(vocab)") }
    p.arguments = args
    var env = env0
    env["PATH"] = "\(NSHomeDirectory())/.pyenv/shims:/opt/homebrew/bin:/usr/local/bin:" + (env["PATH"] ?? "")
    // The model is cached locally; without this every run still asks Hugging Face for updates.
    if env0["BTMIC_ONLINE"] != "1" { env["HF_HUB_OFFLINE"] = "1" }
    p.environment = env
    p.standardOutput = FileHandle.nullDevice
    // A file, not a pipe: a pipe that nobody drains blocks the child once it is full.
    let errURL = outDir.appendingPathComponent("stderr.txt")
    FileManager.default.createFile(atPath: errURL.path, contents: nil, attributes: [.posixPermissions: 0o600])
    p.standardError = try FileHandle(forWritingTo: errURL)
    let exited = DispatchSemaphore(value: 0)
    p.terminationHandler = { _ in exited.signal() }
    try p.run()
    let limit = Double(env0["BTMIC_TIMEOUT"] ?? "") ?? 180
    if exited.wait(timeout: .now() + limit) == .timedOut {
        p.terminate()
        if exited.wait(timeout: .now() + 5) == .timedOut { kill(p.processIdentifier, SIGKILL); _ = exited.wait(timeout: .now() + 5) }
        throw NSError(domain: "btmic", code: 5, userInfo: [NSLocalizedDescriptionKey: "mlx_whisper timed out after \(Int(limit)) s"])
    }
    guard p.terminationStatus == 0 else {
        let msg = (try? String(contentsOf: errURL, encoding: .utf8)) ?? ""
        throw NSError(domain: "btmic", code: 4, userInfo: [NSLocalizedDescriptionKey: "mlx_whisper failed: \(msg.suffix(300))"])
    }
    let txt = outDir.appendingPathComponent(wav.deletingPathExtension().lastPathComponent + ".txt")
    return (try String(contentsOf: txt, encoding: .utf8)).trimmingCharacters(in: .whitespacesAndNewlines)
}

/// Appends a transcript of a recording made while the Mac was out of reach to
/// ~/btmic-inbox.md. The time is when it reached the Mac, not when it was spoken.
func saveToInbox(_ text: String) -> Bool {
    let path = NSHomeDirectory() + "/btmic-inbox.md"
    let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm"
    let entry = Data("## \(f.string(from: Date())) (offline recording)\n\n\(text)\n\n".utf8)
    if !FileManager.default.fileExists(atPath: path) {
        guard FileManager.default.createFile(atPath: path, contents: nil, attributes: [.posixPermissions: 0o600]) else {
            log("cannot create \(path)"); return false
        }
    }
    do {
        let h = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        defer { try? h.close() }
        try h.seekToEnd()
        try h.write(contentsOf: entry)
    } catch {
        log("cannot write \(path): \(error.localizedDescription)")
        return false
    }
    InboxStore.add(text)                       // same text for agents; the .md stays for reading
    log("saved to \(path)")
    copyToCloud(text)                          // optional, off unless a folder is configured
    return true
}

/// Opt-in copy of each note to a folder that a sync service watches (for example iCloud
/// Drive), so the phone sees it. Enabled by BTMIC_CLOUD_DIR or a one-line ~/.btmic-cloud-dir.
/// One small file per note, so two devices never edit the same file. Failure only logs:
/// the local save already succeeded and the Stick may delete its copy.
func cloudDir() -> String? {
    var d = ProcessInfo.processInfo.environment["BTMIC_CLOUD_DIR"] ?? ""
    if d.isEmpty, let s = try? String(contentsOfFile: NSHomeDirectory() + "/.btmic-cloud-dir", encoding: .utf8) {
        d = s.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    if d.isEmpty { return nil }
    return (d as NSString).expandingTildeInPath
}

func copyToCloud(_ text: String) {
    guard let dir = cloudDir() else { return }
    let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
    let stamp = f.string(from: Date())
    var h: UInt32 = 5381
    for b in text.utf8 { h = (h &* 33) ^ UInt32(b) }                    // short tag so same-second notes differ
    let file = dir + "/note-\(stamp)-\(String(h, radix: 16)).md"
    do {
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try Data("# \(stamp)\n\n\(text)\n".utf8).write(to: URL(fileURLWithPath: file), options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file)
        log("copied to cloud folder")
    } catch {
        log("cloud copy failed: \(error.localizedDescription)")
    }
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


// MARK: - Inbox queue (agent-neutral)
//
// ~/btmic-inbox.jsonl is a work queue: one JSON object per line. Any agent or script
// takes items with `btmic inbox claim`, finishes them with `done` (see docs/INBOX.md).
// A claim carries a lease; if the agent never reports back the item becomes
// available again. Every change happens under a file lock and is written atomically.

struct InboxItem: Codable {
    var v: Int = 1
    var id: String
    var recorded_at: String          // when the text reached the Mac (ISO 8601)
    var text: String
    var status: String = "new"       // new | processing | done
    var kind: String? = nil
    var lease_until: String? = nil
    var handled_by: String? = nil
    var result: String? = nil
    var finished_at: String? = nil
}

enum InboxStore {
    static let path = NSHomeDirectory() + "/btmic-inbox.jsonl"
    static let iso: ISO8601DateFormatter = { let f = ISO8601DateFormatter(); f.formatOptions = [.withInternetDateTime]; return f }()

    /// Runs `body` with the queue locked and loaded; saves it afterwards when `body` returns true.
    @discardableResult
    static func transact(_ body: (inout [InboxItem]) -> Bool) -> Bool {
        let fd = open(NSHomeDirectory() + "/.btmic-inbox.lock", O_CREAT | O_RDWR, 0o600)
        guard fd >= 0, flock(fd, LOCK_EX) == 0 else { return false }
        defer { flock(fd, LOCK_UN); close(fd) }
        var items = load()
        guard body(&items) else { return true }
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        var out = Data()
        for i in items { if let d = try? enc.encode(i) { out += d; out += Data("\n".utf8) } }
        let tmp = path + ".tmp"
        guard FileManager.default.createFile(atPath: tmp, contents: out, attributes: [.posixPermissions: 0o600]) else { return false }
        return rename(tmp, path) == 0
    }

    static func load() -> [InboxItem] {
        guard let raw = try? String(contentsOfFile: path, encoding: .utf8) else { return [] }
        let dec = JSONDecoder()
        return raw.split(separator: "\n").compactMap { try? dec.decode(InboxItem.self, from: Data($0.utf8)) }
    }

    static func makeID(_ text: String) -> String {
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        var h: UInt32 = 5381
        for b in text.utf8 { h = h &* 33 &+ UInt32(b) }
        return "\(f.string(from: Date()))-\(String(h, radix: 16).suffix(4))"
    }

    /// An item whose lease ran out counts as new again.
    static func available(_ i: InboxItem, now: Date) -> Bool {
        if i.status == "new" { return true }
        if i.status == "processing", let l = i.lease_until, let d = iso.date(from: l), d <= now { return true }
        return false
    }

    @discardableResult
    static func add(_ text: String) -> Bool {
        let item = InboxItem(id: makeID(text), recorded_at: iso.string(from: Date()), text: text)
        return transact { $0.append(item); return true }
    }
}

func inboxCommand(_ a: [String]) -> Never {
    func opt(_ name: String) -> String? {
        guard let i = a.firstIndex(of: name), i + 1 < a.count else { return nil }
        return a[i + 1]
    }
    func printJSON(_ i: InboxItem) {
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        if let d = try? enc.encode(i), let s = String(data: d, encoding: .utf8) { print(s) }
    }
    let now = Date()
    let sub = a.first ?? ""
    let id = a.count > 1 && !a[1].hasPrefix("--") ? a[1] : nil
    var code: Int32 = 0
    switch sub {
    case "add":
        guard let text = id else { fail("usage: btmic inbox add <text>") }
        if !InboxStore.add(text) { code = 1 }
    case "list":
        let want = opt("--status") ?? "new"
        let json = a.contains("--json")
        for i in InboxStore.load() {
            let ok = want == "all" ? true : (want == "new" ? InboxStore.available(i, now: now) : i.status == want)
            guard ok else { continue }
            if json { printJSON(i) } else { print("\(i.id)  \(i.status)  \(i.text.prefix(60))") }
        }
    case "claim":
        let by = opt("--by") ?? "unknown"
        let minutes = Double(opt("--lease") ?? "") ?? 10
        var got: InboxItem? = nil
        InboxStore.transact { items in
            let idx = items.firstIndex { (id == nil || $0.id == id) && InboxStore.available($0, now: now) }
            guard let k = idx else { return false }
            items[k].status = "processing"
            items[k].handled_by = by
            items[k].lease_until = InboxStore.iso.string(from: now.addingTimeInterval(minutes * 60))
            got = items[k]
            return true
        }
        if let g = got { printJSON(g) } else { code = 1 }       // exit 1: nothing to claim
    case "done":
        guard let id = id else { fail("usage: btmic inbox done <id> [--result TEXT] [--kind KIND] [--by NAME]") }
        var found = false
        InboxStore.transact { items in
            guard let k = items.firstIndex(where: { $0.id == id }) else { return false }
            items[k].status = "done"
            items[k].result = opt("--result") ?? items[k].result
            items[k].kind = opt("--kind") ?? items[k].kind
            items[k].handled_by = opt("--by") ?? items[k].handled_by
            items[k].lease_until = nil
            items[k].finished_at = InboxStore.iso.string(from: now)
            found = true
            return true
        }
        if !found { code = 1 }
    case "release":
        guard let id = id else { fail("usage: btmic inbox release <id>") }
        var found = false
        InboxStore.transact { items in
            guard let k = items.firstIndex(where: { $0.id == id && $0.status == "processing" }) else { return false }
            items[k].status = "new"; items[k].lease_until = nil; found = true
            return true
        }
        if !found { code = 1 }
    default:
        fail("usage: btmic inbox list [--status new|processing|done|all] [--json] | claim [id] --by NAME [--lease MIN] | done <id> [--result T] [--kind K] | release <id> | add <text>")
    }
    exit(code)
}

// MARK: - BLE receiver (B1)

let btmicService = CBUUID(string: "5E3B0000-8A7C-4F1E-9B2D-7C4A1F0E6D10")
let btmicAudio   = CBUUID(string: "5E3B0001-8A7C-4F1E-9B2D-7C4A1F0E6D10")
let btmicCtrl    = CBUUID(string: "5E3B0002-8A7C-4F1E-9B2D-7C4A1F0E6D10")
let hidService   = CBUUID(string: "1812")

enum Ctrl: UInt8 { case busy = 1, done = 2, err = 3, ping = 5, retry = 6 }

// BTMIC_KEEP=1: keep a copy of each recording in ~/btmic-samples (private, for comparing models).
func keepSample(_ wav: URL) {
    let dir = NSHomeDirectory() + "/btmic-samples"
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true,
                                             attributes: [.posixPermissions: 0o700])
    let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
    try? FileManager.default.copyItem(at: wav, to: URL(fileURLWithPath: "\(dir)/\(f.string(from: Date())).wav"))
}

// One receiver at a time: a second copy would fight over the stick. The lock goes
// away with the process, so a crash never leaves it stuck.
func takeSingleInstanceLock() -> Bool {
    let fd = open(NSHomeDirectory() + "/.btmic.lock", O_CREAT | O_RDWR, 0o600)
    if fd < 0 { return true }
    return flock(fd, LOCK_EX | LOCK_NB) == 0           // fd stays open for the life of the process
}

func log(_ s: String) {
    let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
    let line = "[\(f.string(from: Date()))] \(s)"
    print(line)
    fflush(stdout)
    // Launched as an app there is no terminal, so also keep a log file.
    let path = NSHomeDirectory() + "/.btmic.log"
    if let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size]) as? Int, size > 1_000_000 {
        try? FileManager.default.removeItem(atPath: path + ".1")
        try? FileManager.default.moveItem(atPath: path, toPath: path + ".1")
    }
    if let h = FileHandle(forWritingAtPath: path) ?? {
        FileManager.default.createFile(atPath: path, contents: nil, attributes: [.posixPermissions: 0o600])
        return FileHandle(forWritingAtPath: path)
    }() {
        h.seekToEndOfFile(); h.write(Data((line + "\n").utf8)); try? h.close()
    }
}

final class Listener: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var ctrlChar: CBCharacteristic?
    private let pressReturn: Bool
    private let work = DispatchQueue(label: "btmic.work")

    // recording session
    private var active = false
    private var session: UInt16 = 0
    private var expectSeq: UInt16 = 0
    private var pcm: [Int16] = []
    private var lostFrames = 0
    private var offline = false
    private var lastFinished: UInt16? = nil      // a finished session: its late frames must not start a new one
    private var gotEnd = false
    private var queued: [UInt16] = []            // recordings waiting for or in transcription
    private var heartbeat: Timer?
    private var idleTimeout: DispatchWorkItem?

    init(pressReturn: Bool) {
        self.pressReturn = pressReturn
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
        startWifi()
    }

    // ---- finding the stick -------------------------------------------------
    private var scanTimer: Timer?

    /// Forgets the current stick, so findDevice() can look again.
    private func resetLink() {
        peripheral = nil; ctrlChar = nil; active = false; idleTimeout?.cancel(); heartbeat?.invalidate(); heartbeat = nil
    }

    private func isStick(_ name: String?) -> Bool {
        guard let n = name else { return false }
        return n.hasPrefix("StickS3") || n.hasPrefix("SS3")
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            log("Bluetooth on, looking for the stick")
            resetLink()                      // anything held from before Bluetooth went off is stale
            findDevice()
            if scanTimer == nil {
                scanTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.findDevice() }
            }
        case .unauthorized:
            log("Bluetooth permission denied: System Settings > Privacy & Security > Bluetooth")
        default:
            log("Bluetooth state \(c.state.rawValue)")
            resetLink()                      // no disconnect callback comes when the radio is switched off
        }
    }

    // The peripheral is pinned by identifier: a device that merely advertises a
    // "StickS3" name must never be able to feed text into the paste path.
    private static let pinFile = NSHomeDirectory() + "/.btmic-device"

    private func pinnedID() -> UUID? {
        (try? String(contentsOfFile: Listener.pinFile, encoding: .utf8))
            .flatMap { UUID(uuidString: $0.trimmingCharacters(in: .whitespacesAndNewlines)) }
    }

    private func findDevice() {
        guard peripheral == nil, central.state == .poweredOn else { return }
        // Only devices macOS itself holds as paired and connected are candidates (it
        // hides the HID service from apps, so query our own service); no scan fallback.
        let pin = pinnedID()
        for p in central.retrieveConnectedPeripherals(withServices: [btmicService]) {
            if let pin = pin {
                if p.identifier == pin { connect(p); return }
            } else if isStick(p.name) {
                try? p.identifier.uuidString.write(toFile: Listener.pinFile, atomically: true, encoding: .utf8)
                chmod(Listener.pinFile, 0o600)
                log("pinned this stick (\(p.identifier.uuidString)); delete \(Listener.pinFile) to re-pin")
                connect(p); return
            }
        }
    }

    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        log("connecting to \(p.name ?? "?")")
        central.connect(p, options: nil)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected, discovering services")
        p.discoverServices([btmicService])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("connect failed: \(error?.localizedDescription ?? "?")")
        peripheral = nil
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        log("disconnected")
        peripheral = nil; ctrlChar = nil; active = false; idleTimeout?.cancel()
        heartbeat?.invalidate(); heartbeat = nil
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == btmicService }) else {
            log("stick has no BT REC service (old firmware, or macOS cached the old service list: forget the device and pair again)")
            central.cancelPeripheralConnection(p)
            return
        }
        p.discoverCharacteristics([btmicAudio, btmicCtrl], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for c in s.characteristics ?? [] {
            if c.uuid == btmicAudio { p.setNotifyValue(true, for: c) }
            if c.uuid == btmicCtrl { ctrlChar = c }
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor c: CBCharacteristic, error: Error?) {
        if let e = error { log("subscribe failed: \(e.localizedDescription)") }
        else {
            log("ready: hold the front button on BT REC and talk")
            // The stick only treats this Mac as present while it hears this every few seconds.
            send(.ping)
            heartbeat?.invalidate()
            heartbeat = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.beat() }
        }
    }

    // ---- receiving ---------------------------------------------------------
    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard c.uuid == btmicAudio, let data = c.value, let f = parseFrame([UInt8](data)) else { return }
        handleFrame(f)
    }

    /// One frame from either transport: a BLE notification or the Wi-Fi link.
    private func handleFrame(_ f: ParsedFrame) {
        switch f.type {
        case .start:
            begin(session: f.session, firstSeq: 1, offline: f.flags & flagOffline != 0)
        case .audio:
            if !active && f.session == lastFinished { return }
            if !active || f.session != session { begin(session: f.session, firstSeq: f.seq, offline: f.flags & flagOffline != 0) }
            if f.seq != expectSeq {
                let missing = Int(f.seq &- expectSeq)
                if missing > 0 && missing < 100 {
                    pcm += [Int16](repeating: 0, count: missing * samplesPerFrame)
                    lostFrames += missing
                }
            }
            expectSeq = f.seq &+ 1
            if pcm.count < maxSamples { pcm += f.pcm }      // 70 s is more than the stick ever sends
            armIdleTimeout()
        case .end:
            if active && f.session == session { gotEnd = true; finish() }
        }
    }

    private func begin(session s: UInt16, firstSeq: UInt16, offline off: Bool) {
        active = true; session = s; expectSeq = firstSeq; pcm = []; lostFrames = 0; offline = off; gotEnd = false
        log(off ? "receiving a stored recording (session \(s))" : "recording (session \(s))")
        armIdleTimeout()
    }

    /// If the end frame never arrives, finish after a quiet second.
    private func armIdleTimeout() {
        idleTimeout?.cancel()
        let w = DispatchWorkItem { [weak self] in
            guard let self = self, self.active else { return }
            log("no end frame, finishing anyway")
            self.finish()
        }
        idleTimeout = w
        // A stored recording is sent with retries of up to 4 s per frame on a weak link,
        // so it gets a longer quiet period than live audio.
        DispatchQueue.main.asyncAfter(deadline: .now() + (offline ? 10 : 1.5), execute: w)
    }

    private func finish() {
        idleTimeout?.cancel()
        active = false
        let samples = pcm
        let lost = lostFrames
        let stored = offline
        pcm = []
        lastFinished = session
        // A stored recording that never reached its end frame is cut short: do not
        // transcribe or acknowledge it, the stick still has the whole thing and
        // will send it again.
        if stored && !gotEnd {
            log("stored recording incomplete (no end frame), discarded; the stick will resend")
            send(.retry, session: session)
            return
        }
        log(String(format: "recorded %.1f s, lost frames: %d", Double(samples.count) / 16000, lost))
        let sess = session
        send(.busy, session: sess)
        queued.append(sess)
        work.async { self.process(samples, stored: stored, session: sess) }
    }

    // ---- transcribe + deliver ------------------------------------------------
    private func process(_ samples: [Int16], stored: Bool, session sess: UInt16) {
        func reply(_ c: Ctrl) { DispatchQueue.main.async { self.queued.removeAll { $0 == sess }; self.send(c, session: sess) } }
        defer { DispatchQueue.main.async { self.queued.removeAll { $0 == sess } } }
        // A stored recording with nothing in it is not a failure: tell the stick it
        // may drop it, otherwise it would flash ERR and keep re-sending it.
        let nothing: Ctrl = stored ? .done : .err
        guard samples.count >= 4800 else { log("too short, ignored"); reply(nothing); return }
        var sum = 0.0
        for s in samples { sum += Double(s) * Double(s) }
        let rms = (sum / Double(samples.count)).squareRoot()
        guard rms >= 40 else { log("silence (rms \(Int(rms))), ignored"); reply(nothing); return }

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("btmic-\(getpid())-\(sess)")
        // Our own scratch files (audio and text): delete for good, never into the Trash.
        defer { try? FileManager.default.removeItem(at: dir) }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let wav = dir.appendingPathComponent("rec.wav")
            try writeWav16kMono(samples, to: wav)
            if ProcessInfo.processInfo.environment["BTMIC_KEEP"] == "1" { keepSample(wav) }
            let started = Date()
            let text = try transcribe(wav: wav, language: language, model: model)
            // The transcript is clinical content: the log gets its length, the text only on request.
            let shown = ProcessInfo.processInfo.environment["BTMIC_LOG_TEXT"] == "1" ? ": \(text)" : ""
            log(String(format: "text (%.1f s, %d chars)", Date().timeIntervalSince(started), text.count) + shown)
            guard !text.isEmpty else { reply(nothing); return }
            DispatchQueue.main.async {
                if stored {
                    // Recorded earlier: the cursor is somewhere else now, so file it instead of pasting.
                    // If it cannot be filed the stick must keep it: no DONE.
                    guard saveToInbox(text) else { return }
                } else {
                    deliver(text, pressReturn: self.pressReturn)
                }
                self.send(.done, session: sess)
            }
        } catch {
            log("error: \(error.localizedDescription)")
            // A stored recording stays on the stick for another try; only live ones are answered.
            if !stored { reply(.err) }
        }
    }

    /// Ping every 2 s; while recordings are being transcribed also repeat BUSY so a
    /// stick that is waiting for the answer does not give up and send it again.
    private func beat() {
        send(.ping)
        for s in queued { send(.busy, session: s) }
    }

    /// Status goes with the recording's session number so a stick that is waiting
    /// for an answer about one recording is never satisfied by another's.
    private func send(_ c: Ctrl, session: UInt16? = nil) {
        if let p = peripheral, let ch = ctrlChar {
            var bytes: [UInt8] = [c.rawValue]
            if let s = session { bytes += [UInt8(s & 0xFF), UInt8(s >> 8)] }
            p.writeValue(Data(bytes), for: ch, type: .withoutResponse)
        }
        wifiLink?.send(c, session: session)
    }

    // ---- the stick over Wi-Fi ------------------------------------------------
    // The stick's WIFI app connects to us (TCP, authenticated and encrypted with the pairing key).
    // Its frames go through the same handleFrame / finish / process path as the BLE ones; the
    // stick uses one transport at a time, so the recording state is shared.
    private var wifiReceiver: WifiReceiver?
    private var wifiLink: WifiLink?
    private var wifiBeat: Timer?

    private func startWifi() {
        guard let psk = wifiPairingKey() else {
            log("Wi-Fi receiver off: put a pairing key of at least \(wifiKeyMinLength) characters in ~/.btmic-wifi-key")
            return
        }
        let r = WifiReceiver(psk: psk, port: wifiPort)
        r.onUp = { [weak self] l in self?.wifiUp(l) }
        r.onDown = { [weak self] l in self?.wifiDown(l) }
        r.onFrame = { [weak self] f in self?.handleFrame(f) }
        do { try r.start(); wifiReceiver = r; log("Wi-Fi receiver listening on TCP port \(wifiPort)") }
        catch { log("Wi-Fi receiver could not start: \(error.localizedDescription)") }
    }

    private func wifiUp(_ l: WifiLink) {
        wifiLink?.hangUp()
        wifiLink = l
        log("Wi-Fi: stick at \(l.peer) passed the pairing-key check")
        send(.ping)
        wifiBeat?.invalidate()
        wifiBeat = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.beat() }
    }

    private func wifiDown(_ l: WifiLink) {
        guard wifiLink === l else { return }
        wifiLink = nil
        wifiBeat?.invalidate(); wifiBeat = nil
        log("Wi-Fi: stick disconnected")
    }
}

// MARK: - Wi-Fi receiver (the stick's WIFI app over the local network)
//
// Same protocol as tools/winvoice (docs/WIFI-PLAN.md, docs/firmware/wifi-lan/README.md); keep the
// handshake and the record format in step with winvoice.py's _handshake() and main/wifimic.c:
//   stick -> Mac: ns (16 random bytes)
//   Mac -> stick: np (16 random) + HMAC-SHA256(key, "P" ns np)
//   stick -> Mac: HMAC-SHA256(key, "S" np ns)
//   then records: u16 length, AES-256-GCM ciphertext, 16-byte tag; the length is the AAD, the nonce
//   is the 64-bit message counter (little-endian) + 4 zero bytes, one key per direction:
//   HMAC(key, "1" ns np) stick -> Mac, HMAC(key, "2" ns np) Mac -> stick.
//   stick -> Mac record: one docs/BT-MIC.md frame.   Mac -> stick record: 'C', status, session (u16).

let wifiPort: UInt16 = UInt16(ProcessInfo.processInfo.environment["BTMIC_WIFI_PORT"] ?? "") ?? 47100
let wifiKeyMinLength = 12

/// The pairing key: BTMIC_WIFI_KEY, else the first line of ~/.btmic-wifi-key. Nil when missing or too short.
func wifiPairingKey() -> Data? {
    var key = ProcessInfo.processInfo.environment["BTMIC_WIFI_KEY"] ?? ""
    if key.isEmpty {
        let path = NSHomeDirectory() + "/.btmic-wifi-key"
        if let s = try? String(contentsOfFile: path, encoding: .utf8) {
            key = s.split(whereSeparator: \.isNewline).first.map(String.init) ?? ""
            if let perm = (try? FileManager.default.attributesOfItem(atPath: path)[.posixPermissions]) as? Int,
               perm & 0o077 != 0 {
                log("note: ~/.btmic-wifi-key can be read by other users; run: chmod 600 ~/.btmic-wifi-key")
            }
        }
    }
    key = key.trimmingCharacters(in: .whitespaces)
    return key.utf8.count >= wifiKeyMinLength ? Data(key.utf8) : nil
}

private func wifiMac(_ psk: Data, _ tag: UInt8, _ x: Data, _ y: Data) -> Data {
    var msg = Data([tag]); msg.append(x); msg.append(y)
    return Data(HMAC<SHA256>.authenticationCode(for: msg, using: SymmetricKey(data: psk)))
}

private func wifiNonce(_ n: UInt64) -> AES.GCM.Nonce {
    var d = Data(count: 12)
    for i in 0..<8 { d[i] = UInt8((n >> (8 * UInt64(i))) & 0xFF) }
    return try! AES.GCM.Nonce(data: d)
}

private enum WifiRead { case data(Data), idle, closed }

/// Reads exactly n bytes. `.idle` only when the read timed out before the first byte.
private func wifiReadExactly(_ fd: Int32, _ n: Int) -> WifiRead {
    var buf = [UInt8](repeating: 0, count: n)
    var got = 0
    while got < n {
        let r = buf.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + got, n - got, 0) }
        if r > 0 { got += r }
        else if r < 0 && errno == EINTR { continue }
        else if r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && got == 0 { return .idle }
        else { return .closed }
    }
    return .data(Data(buf))
}

private func wifiWriteAll(_ fd: Int32, _ data: Data) -> Bool {
    let total = data.count
    var sent = 0
    return data.withUnsafeBytes { raw in
        while sent < total {
            let w = send(fd, raw.baseAddress! + sent, total - sent, 0)
            if w > 0 { sent += w } else if w < 0 && errno == EINTR { continue } else { return false }
        }
        return true
    }
}

private func wifiSetTimeout(_ fd: Int32, seconds: Int) {
    var tv = timeval(tv_sec: seconds, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
}

/// A connected, authenticated stick.
final class WifiLink {
    let peer: String
    private let fd: Int32
    private let txKey: SymmetricKey
    private let rxKey: SymmetricKey
    private var txN: UInt64 = 0
    private var rxN: UInt64 = 0
    private var alive = true
    private let lock = NSLock()

    fileprivate init(fd: Int32, peer: String, txKey: Data, rxKey: Data) {
        self.fd = fd; self.peer = peer
        self.txKey = SymmetricKey(data: txKey); self.rxKey = SymmetricKey(data: rxKey)
    }

    /// Mac -> stick: 'C', status, session (u16 LE); session 0 = not about a recording (PING).
    func send(_ c: Ctrl, session: UInt16?) {
        let s = session ?? 0
        let plain = Data([0x43, c.rawValue, UInt8(s & 0xFF), UInt8(s >> 8)])
        lock.lock(); defer { lock.unlock() }
        guard alive else { return }
        let n = plain.count + 16
        let hdr = Data([UInt8(n & 0xFF), UInt8(n >> 8)])
        guard let box = try? AES.GCM.seal(plain, using: txKey, nonce: wifiNonce(txN), authenticating: hdr) else { return }
        txN += 1
        if !wifiWriteAll(fd, hdr + box.ciphertext + box.tag) { alive = false }
    }

    /// Ends the connection; the reading thread notices and cleans up.
    func hangUp() { shutdown(fd, SHUT_RDWR) }

    fileprivate func retire() { lock.lock(); alive = false; lock.unlock() }

    /// One decrypted record from the stick.
    fileprivate func readRecord() -> WifiRead {
        switch wifiReadExactly(fd, 2) {
        case .idle: return .idle
        case .closed: return .closed
        case .data(let head):
            let n = Int(head[0]) | Int(head[1]) << 8
            guard n >= 16 + 12, n <= 16 + 12 + 160 + 8 else {      // nothing the stick sends is outside this
                log("Wi-Fi: bad record length \(n) from \(peer)"); return .closed
            }
            guard case .data(let body) = wifiReadExactly(fd, n) else { return .closed }
            guard let box = try? AES.GCM.SealedBox(nonce: wifiNonce(rxN), ciphertext: body.prefix(n - 16), tag: body.suffix(16)),
                  let plain = try? AES.GCM.open(box, using: rxKey, authenticating: head) else {
                log("Wi-Fi: a record from \(peer) failed authentication; closing"); return .closed
            }
            rxN += 1
            return .data(plain)
        }
    }
}

final class WifiReceiver {
    private let psk: Data
    private let port: UInt16
    // These three are called on the main queue.
    var onUp: (WifiLink) -> Void = { _ in }
    var onDown: (WifiLink) -> Void = { _ in }
    var onFrame: (ParsedFrame) -> Void = { _ in }

    init(psk: Data, port: UInt16) { self.psk = psk; self.port = port }

    func start() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        if fd < 0 { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = 0
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        if bound < 0 || listen(fd, 2) < 0 {
            let e = errno; close(fd)
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(e))
        }
        Thread.detachNewThread { [self] in
            while true {
                var peerAddr = sockaddr_in()
                var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                let cfd = withUnsafeMutablePointer(to: &peerAddr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(fd, $0, &len) }
                }
                if cfd < 0 { if errno == EINTR { continue }; return }
                let ip = UInt32(bigEndian: peerAddr.sin_addr.s_addr)
                let text = String(cString: inet_ntoa(peerAddr.sin_addr))
                guard Self.isLocalNetwork(ip) else {
                    log("Wi-Fi: refused \(text) (not a local network address)")
                    close(cfd); continue
                }
                Thread.detachNewThread { self.serve(cfd, peer: text) }
            }
        }
    }

    /// Loopback, private ranges and Tailscale. Defence in depth only: the pairing key is what keeps strangers out.
    static func isLocalNetwork(_ ip: UInt32) -> Bool {
        let a = ip >> 24, b = (ip >> 16) & 0xFF
        return a == 127 || a == 10 || (a == 192 && b == 168) || (a == 172 && (16...31).contains(b)) || (a == 100 && (64...127).contains(b))
    }

    private func handshake(_ fd: Int32) -> (stickToMac: Data, macToStick: Data)? {
        guard case .data(let ns) = wifiReadExactly(fd, 16) else { return nil }
        var rnd = [UInt8](repeating: 0, count: 16)
        guard SecRandomCopyBytes(kSecRandomDefault, 16, &rnd) == errSecSuccess else { return nil }
        let np = Data(rnd)
        guard wifiWriteAll(fd, np + wifiMac(psk, 0x50, ns, np)) else { return nil }          // "P"
        guard case .data(let got) = wifiReadExactly(fd, 32) else { return nil }
        let want = wifiMac(psk, 0x53, np, ns)                                                // "S"
        var diff: UInt8 = 0
        for (a, b) in zip(got, want) { diff |= a ^ b }
        guard diff == 0 else { return nil }
        return (wifiMac(psk, 0x31, ns, np), wifiMac(psk, 0x32, ns, np))                      // "1", "2"
    }

    private func serve(_ fd: Int32, peer: String) {
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        wifiSetTimeout(fd, seconds: 5)
        guard let keys = handshake(fd) else {
            log("Wi-Fi: refused \(peer): it did not prove it knows the pairing key")
            close(fd); return
        }
        wifiSetTimeout(fd, seconds: 30)   // the stick only talks while recording; our pings keep the link warm
        let link = WifiLink(fd: fd, peer: peer, txKey: keys.macToStick, rxKey: keys.stickToMac)
        DispatchQueue.main.async { self.onUp(link) }
        loop: while true {
            switch link.readRecord() {
            case .idle: continue
            case .closed: break loop
            case .data(let plain):
                guard plain.count >= 12, let f = parseFrame([UInt8](plain)) else { break loop }
                DispatchQueue.main.async { self.onFrame(f) }
            }
        }
        link.retire()
        close(fd)
        DispatchQueue.main.async { self.onDown(link) }
    }
}

// MARK: - Probe (diagnostics)

final class Probe: NSObject, CBCentralManagerDelegate {
    var c: CBCentralManager!
    override init() { super.init(); c = CBCentralManager(delegate: self, queue: nil) }
    func centralManagerDidUpdateState(_ m: CBCentralManager) {
        guard m.state == .poweredOn else { log("state \(m.state.rawValue)"); return }
        let sets: [(String, [CBUUID])] = [("HID 1812", [hidService]), ("battery 180F", [CBUUID(string: "180F")]),
                                          ("btmic service", [btmicService]), ("devinfo 180A", [CBUUID(string: "180A")])]
        for (n, u) in sets {
            let r = m.retrieveConnectedPeripherals(withServices: u)
            log("connected with \(n): \(r.map { ($0.name ?? "?") + " " + $0.identifier.uuidString })")
        }
        m.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        log("scanning 12 s for anything named Stick/SS3...")
        DispatchQueue.main.asyncAfter(deadline: .now() + 12) { log("done"); exit(0) }
    }
    func centralManager(_ m: CBCentralManager, didDiscover p: CBPeripheral, advertisementData a: [String: Any], rssi: NSNumber) {
        let n = (a[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        if n.hasPrefix("Stick") || n.hasPrefix("SS3") { log("scan saw \(n) \(p.identifier.uuidString) rssi \(rssi)") }
    }
}

// MARK: - Entry point

/// `btmic wifi-selftest`: the receiver and a stand-in for the stick (same steps as main/wifimic.c)
/// talk over loopback; no Bluetooth, no transcription, nothing is pasted.
func wifiSelfTest() -> Never {
    let key = Data("selftest-pairing-key-0123".utf8)
    let wrong = Data("not-the-pairing-key-0000".utf8)
    let port: UInt16 = 47198
    final class State { var frames = 0, ends = 0, ups = 0, downs = 0; var link: WifiLink? }
    let st = State()
    let rx = WifiReceiver(psk: key, port: port)
    rx.onUp = { l in st.ups += 1; st.link = l; l.send(.ping, session: nil) }
    rx.onDown = { _ in st.downs += 1 }
    rx.onFrame = { f in
        st.frames += 1
        if f.type == .end { st.ends += 1; st.link?.send(.done, session: f.session) }
    }
    do { try rx.start() } catch { fail("cannot listen on \(port): \(error.localizedDescription)") }

    func connectLocal() -> Int32? {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        var a = sockaddr_in()
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
        a.sin_port = port.bigEndian; a.sin_addr.s_addr = UInt32(0x7F000001).bigEndian
        let rc = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        if rc < 0 { close(fd); return nil }
        wifiSetTimeout(fd, seconds: 3)
        return fd
    }

    /// The stick's side. checkPC = false skips the check of the PC's proof (to see the PC refuse us).
    func stick(key k: Data, checkPC: Bool, frames: Int) -> (connected: Bool, ctrl: [[UInt8]]) {
        guard let fd = connectLocal() else { return (false, []) }
        defer { close(fd) }
        var r = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, 16, &r)
        let ns = Data(r)
        guard wifiWriteAll(fd, ns), case .data(let resp) = wifiReadExactly(fd, 48) else { return (false, []) }
        let np = Data(resp.prefix(16))
        if checkPC && Data(resp.suffix(32)) != wifiMac(k, 0x50, ns, np) { return (false, []) }
        guard wifiWriteAll(fd, wifiMac(k, 0x53, np, ns)) else { return (false, []) }
        let tx = SymmetricKey(data: wifiMac(k, 0x31, ns, np)), rxk = SymmetricKey(data: wifiMac(k, 0x32, ns, np))
        var txN: UInt64 = 0
        func put(_ f: [UInt8]) {
            let n = f.count + 16
            let hdr = Data([UInt8(n & 0xFF), UInt8(n >> 8)])
            guard let box = try? AES.GCM.seal(Data(f), using: tx, nonce: wifiNonce(txN), authenticating: hdr) else { return }
            txN += 1
            _ = wifiWriteAll(fd, hdr + box.ciphertext + box.tag)
        }
        var enc = AdpcmState()
        put(makeFrame(type: .start, session: 7, seq: 0, pcm: [], state: &enc))
        for i in 1...max(frames, 1) {
            put(makeFrame(type: .audio, session: 7, seq: UInt16(i), pcm: [Int16](repeating: 1000, count: samplesPerFrame), state: &enc))
        }
        put(makeFrame(type: .end, session: 7, seq: UInt16(frames + 1), pcm: [], state: &enc))
        var got: [[UInt8]] = []
        var rxN: UInt64 = 0
        while case .data(let head) = wifiReadExactly(fd, 2) {
            let n = Int(head[0]) | Int(head[1]) << 8
            guard case .data(let body) = wifiReadExactly(fd, n),
                  let box = try? AES.GCM.SealedBox(nonce: wifiNonce(rxN), ciphertext: body.prefix(n - 16), tag: body.suffix(16)),
                  let plain = try? AES.GCM.open(box, using: rxk, authenticating: head) else { break }
            rxN += 1
            got.append([UInt8](plain))
            if got.count >= 2 { break }
        }
        return (true, got)
    }

    var good: (connected: Bool, ctrl: [[UInt8]]) = (false, [])
    var wrongStrict = (connected: true, ctrl: [[UInt8]]())
    var wrongLenient = (connected: true, ctrl: [[UInt8]]())
    let done = DispatchSemaphore(value: 0)
    Thread.detachNewThread {
        good = stick(key: key, checkPC: true, frames: 30)
        wrongStrict = stick(key: wrong, checkPC: true, frames: 5)
        wrongLenient = stick(key: wrong, checkPC: false, frames: 5)
        done.signal()
    }
    let deadline = Date().addingTimeInterval(20)
    while done.wait(timeout: .now()) == .timedOut && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
    RunLoop.main.run(until: Date().addingTimeInterval(0.3))

    var failures: [String] = []
    func check(_ ok: Bool, _ what: String) { print((ok ? "ok   " : "FAIL ") + what); if !ok { failures.append(what) } }
    check(good.connected, "stick with the right key connects")
    check(st.frames == 32 && st.ends == 1, "receiver got start + 30 audio + end (\(st.frames) frames, \(st.ends) ends)")
    check(good.ctrl.first == [0x43, 5, 0, 0], "stick got the PING record")
    check(good.ctrl.dropFirst().first == [0x43, 2, 7, 0], "stick got DONE for session 7")
    check(!wrongStrict.connected, "stick with a wrong key notices the PC does not match")
    check(wrongLenient.ctrl.isEmpty && st.ups == 1, "PC refuses a stick with a wrong key (links up: \(st.ups))")
    check(st.frames == 32, "refused sticks delivered no frames")
    check(WifiReceiver.isLocalNetwork(0xC0A80170) && !WifiReceiver.isLocalNetwork(0x08080808), "address filter")
    print(failures.isEmpty ? "wifi selftest passed" : "wifi selftest FAILED: \(failures.count)")
    exit(failures.isEmpty ? 0 : 1)
}

func fail(_ s: String) -> Never { FileHandle.standardError.write(Data((s + "\n").utf8)); exit(1) }

let args = Array(CommandLine.arguments.dropFirst())
let language = ProcessInfo.processInfo.environment["BTMIC_LANG"] ?? "zh"
let model = ProcessInfo.processInfo.environment["BTMIC_MODEL"] ?? "eoleedi/Breeze-ASR-25-mlx"

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
case "wifi-selftest":
    wifiSelfTest()
case "probe":
    let probe = Probe()
    withExtendedLifetime(probe) { RunLoop.main.run() }
case "inbox":
    inboxCommand(Array(args.dropFirst()))
case "listen":
    if !takeSingleInstanceLock() { log("btmic is already running, exiting"); exit(0) }
    log("btmic listen started")
    if !AXIsProcessTrusted() {
        log("note: no Accessibility permission, text will be copied but not pasted")
    }
    let listener = Listener(pressReturn: ProcessInfo.processInfo.environment["BTMIC_ENTER"] == "1")
    withExtendedLifetime(listener) { RunLoop.main.run() }
default:
    fail("usage: btmic selftest <wav> [--paste] | listen | inbox ...")
}
