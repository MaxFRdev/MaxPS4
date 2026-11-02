import Foundation
import Combine
import Metal
#if MAXPS4_FEX_APP
import Darwin
import AVFoundation

// Game audio: an AVAudioSourceNode pulls the engine's mixed AudioOut ports
// (48 kHz stereo). The render thread never waits: while the engine is being
// detached it plays silence. After stop() returns the engine is never read.
final class MaxPS4GameAudio: @unchecked Sendable {
    private final class Tap: @unchecked Sendable {
        static let capacity = 4096
        let lock = NSLock()
        var engine: OpaquePointer?
        var paused = false // Game menu open: silence, and the game's AudioOut waits.
        let mix = UnsafeMutablePointer<Float>.allocate(capacity: Tap.capacity * 2)
        deinit { mix.deallocate() }
    }
    private let tap = Tap()
    private let output = AVAudioEngine()
    // Nil on success, otherwise why playback could not start.
    func start(_ engine: OpaquePointer) -> String? {
        guard let format = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2) else { return "format 48 kHz refusé" }
        let tap = self.tap
        let node = AVAudioSourceNode(format: format) { silence, _, frameCount, list in
            let frames = min(Int(frameCount), Tap.capacity)
            var filled: Int32 = 0
            if tap.lock.try() {
                if let engine = tap.engine, !tap.paused { filled = maxps4_fex_engine_audio_read(engine, tap.mix, UInt32(frames)) }
                tap.lock.unlock()
            }
            if filled <= 0 { silence.pointee = true }
            for (channel, buffer) in UnsafeMutableAudioBufferListPointer(list).enumerated() {
                guard let data = buffer.mData?.assumingMemoryBound(to: Float.self) else { continue }
                let count = Int(buffer.mDataByteSize) / MemoryLayout<Float>.size
                for i in 0..<count { data[i] = filled > 0 && i < frames ? tap.mix[2 * i + min(channel, 1)] : 0 }
            }
            return noErr
        }
        output.attach(node)
        output.connect(node, to: output.mainMixerNode, format: format)
        do {
            #if os(iOS)
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setActive(true)
            #endif
            tap.lock.lock(); tap.engine = engine; tap.lock.unlock()
            try output.start()
            return nil
        } catch {
            stop()
            return error.localizedDescription
        }
    }
    func stop() {
        output.stop()
        tap.lock.lock(); tap.engine = nil; tap.lock.unlock()
    }
    func setPaused(_ paused: Bool) { tap.lock.lock(); tap.paused = paused; tap.lock.unlock() }
}

// The FEX worker owns execution and close. Concurrent display/input/stop calls
// retain the engine under lock, using only the native concurrent APIs.
final class MaxPS4GameSource: @unchecked Sendable {
    struct Frame { let info: maxps4_videoout_frame; let texture: MTLTexture; let report: String }
    private let lock = NSLock()
    private let inputLock = NSLock()
    private var engine: OpaquePointer?
    private var bridge: MaxPS4GameMetalBridge?
    private var stopped = false
    private var ready = false
    private var port: UInt64 = 1
    private var missingSince: Date?
    private var lockWait: TimeInterval = 0 // Last snapshot's wait for `lock`.
    private var pad = MaxPS4PadState()
    private var lastBeat = "" // The last heartbeat() text, under lock.
    // Where the run or its stop is, for the next session's loading screen.
    private let stageLock = NSLock()
    private var stage = "lancement"
    private func setStage(_ text: String) { stageLock.lock(); stage = text; stageLock.unlock() }
    func stopStatus() -> String {
        stageLock.lock(); let current = stage; stageLock.unlock()
        guard lock.try() else { return current + " (verrou de session pris)" }
        defer { lock.unlock() }
        guard let engine else { return current }
        var activity = [CChar](repeating: 0, count: 4096)
        _ = maxps4_fex_engine_thread_activity(engine, &activity, activity.count)
        return current + "\n" + String(cString: activity)
    }
    // What the last run left for the player, set before run() returns.
    struct Outcome { var missing: [String] = []; var crashed = false; var report: URL? }
    private(set) var outcome = Outcome()
    private var inputEngine: OpaquePointer?
    private var displayed = 0
    private var started: TimeInterval?
    // Rewritten each heartbeat so a run that crashes the app leaves its state.
    private var liveReport: URL?
    private var folderListing = "" // What /app0 holds, read once per run, under lock.
    private var vblanksSent = 0 // With vblankPort, under inputLock.
    private var vblankPort: UInt64 = 1
    private var lastTicket: UInt64 = 0
    private var relocation: UInt64 = 0
    private var vblankClock = MaxPS4VideoOutClock()
    // Screen refreshes seen by displayRefresh (under inputLock).
    private var lastRefresh: TimeInterval?, refreshInterval: TimeInterval = 1
    private var vblankTimer: DispatchSourceTimer?
    private let audio = MaxPS4GameAudio()
    private var audioFailure: String?
    private let vblanks = DispatchQueue(label: "com.enermaxfr.maxps4.vblank", qos: .userInteractive)

    // Under inputLock alone, like input(): the game's flips wait for vblanks,
    // so one skipped while a display copy holds `lock` would slow the game.
    // The timer sends them only while the screen does not (see displayRefresh).
    private func tick(at time: TimeInterval) {
        inputLock.lock(); defer { inputLock.unlock() }
        guard let inputEngine else { return }
        pad.send(to: inputEngine)
        guard !screenPaced(at: time), vblankClock.due(at: time) else { return }
        _ = maxps4_fex_engine_videoout_vblank(inputEngine, vblankPort)
        vblanksSent += 1
    }
    // A screen refresh, after the display took the pending flip: the guest
    // vblank follows the screen, so each refresh finds one new image instead
    // of two 60 Hz clocks drifting apart. Every refresh of a screen up to 90 Hz
    // (a 59 Hz one too), and the shared 60 Hz clock above that, so a 120 Hz
    // screen gives one in two. A screen slower than 50 Hz leaves it to the timer.
    func displayRefresh(at time: TimeInterval) {
        inputLock.lock(); defer { inputLock.unlock() }
        if let last = lastRefresh, time > last { refreshInterval = min(time - last, 1) }
        lastRefresh = time
        guard let inputEngine, screenPaced(at: time),
              refreshInterval > 1.0 / 90 || vblankClock.due(at: time) else { return }
        _ = maxps4_fex_engine_videoout_vblank(inputEngine, vblankPort)
        vblanksSent += 1
    }
    // Refreshing at least 60 times a second (with jitter) and lately.
    private func screenPaced(at time: TimeInterval) -> Bool {
        guard let lastRefresh else { return false }
        return refreshInterval < 1.0 / 50 && time - lastRefresh < 1.0 / 20
    }

    // Progress counters refreshed even when no new image arrives, so a stalled
    // game can be told apart from a slow one. Nil while another call holds the lock.
    func heartbeat() -> String? {
        guard lock.try() else { return nil }
        defer { lock.unlock() }
        guard let started, let engine else { return nil }
        let now = ProcessInfo.processInfo.systemUptime
        let seconds = Int(now - started)
        var activity = [CChar](repeating: 0, count: 4096)
        _ = maxps4_fex_engine_thread_activity(engine, &activity, activity.count)
        inputLock.lock(); let sent = vblanksSent; inputLock.unlock()
        let missing = missingImports(engine)
        let compatibility = missing.called.isEmpty ? "" : "Fonctions PS4 manquantes appelées : \(missing.called.count) sur \(missing.total) absentes · "
            + missing.called.prefix(5).joined(separator: " · ") + "\n"
        let text = compatibility + "Depuis le démarrage du JIT : \(seconds) s · dernier ticket VideoOut \(lastTicket) · images affichées \(displayed) · sautées \(maxps4_fex_engine_videoout_dropped(engine)) · VBlank envoyés \(sent)\n" + rates(engine, now: now) + audioStatus(engine) + "\n" + String(cString: activity) + profile(engine) + mainThreadSample(engine)
        lastBeat = text
        if let liveReport {
            MaxPS4Reports.write(text + "\n\n" + movedMappings(engine) + "\n\nDerniers appels PS4 en échec :\n" + failedCalls(engine)
                + "\nFichiers ouverts par le jeu :\n" + openedFiles(engine)
                + "\nDerniers appels PS4 :\n" + recentCalls(engine)
                + "\n" + memoryLayout(engine) + "\n\n" + (bridge?.report ?? "Metal : pas encore de rendu")
                + "\n\n" + folderListing, to: liveReport)
        }
        return text
    }
    // Fixed PS4 addresses iOS refused: the game got another address and may
    // still write at the one it asked for, into MaxPS4's own memory.
    private func movedMappings(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 4096)
        let total = maxps4_fex_engine_moved_mappings(engine, &text, text.count)
        guard total > 0 else { return "Mémoire refusée ou déplacée : rien" }
        return "Mémoire refusée (-> 0x0, code d'erreur PS4) ou déplacée (\(total)) :\n" + String(cString: text)
    }
    // Whether the PS4 application area and the mmap window were granted, and where.
    private func memoryLayout(_ engine: OpaquePointer) -> String {
        var area: UInt64 = 0, areaSize: UInt64 = 0, window: UInt64 = 0, windowSize: UInt64 = 0
        var status: Int32 = 0
        _ = maxps4_fex_engine_memory_layout(engine, &area, &areaSize, &window, &windowSize, &status)
        let gib = { (bytes: UInt64) in bytes >> 30 }
        let zone = areaSize == 0 ? "zone PS4 0x1000000000 refusée par iOS (code \(status))"
            : "zone PS4 0x\(String(area, radix: 16)) (\(gib(areaSize)) Gio)"
        let spare = maxps4_fex_engine_spare_address_space(engine) >> 20
        return "Mémoire : \(zone) · fenêtre 0x\(String(window, radix: 16)) (\(gib(windowSize)) Gio) · encore libre après elle : \(spare) Mio"
    }
    // What the game printed last (its own error messages), invalid UTF-8 replaced.
    private func console(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 4097)
        guard maxps4_fex_engine_console(engine, &text, text.count) > 0 else { return "rien" }
        let bytes = text.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
        return String(decoding: bytes, as: UTF8.self)
    }
    // The run's last PS4 calls, oldest first: what a game did before it quit.
    private func recentCalls(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 32768)
        return maxps4_fex_engine_recent_calls(engine, &text, text.count) > 0 ? String(cString: text) : "aucun"
    }
    // The last calls that returned an error: what MaxPS4 refused the game.
    private func failedCalls(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 16384)
        return maxps4_fex_engine_failed_calls(engine, &text, text.count) > 0 ? String(cString: text) : "aucun"
    }
    // The last files the game opened: where its data really is.
    private func openedFiles(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 8192)
        return maxps4_fex_engine_opened_files(engine, &text, text.count) > 0 ? String(cString: text) : "aucun"
    }
    // The game folder's (and update's) top level: each folder with its file
    // count and size, so a file the game cannot find can be looked for.
    static func listing(of folders: [URL]) -> String {
        let manager = FileManager.default
        let mib = { (bytes: UInt64) in String(format: "%.1f Mio", Double(bytes) / 1_048_576) }
        var lines: [String] = []
        for folder in folders {
            lines.append("Dossier \(folder.lastPathComponent) :")
            guard let entries = try? manager.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey]) else {
                lines.append("  illisible"); continue
            }
            for entry in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }).prefix(60) {
                let values = try? entry.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey])
                guard values?.isDirectory == true else {
                    lines.append("  \(entry.lastPathComponent) · \(mib(UInt64(values?.fileSize ?? 0)))"); continue
                }
                var files = 0, bytes: UInt64 = 0
                var names: [String] = []
                let walker = manager.enumerator(at: entry, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey])
                while let file = walker?.nextObject() as? URL, files < 20000 {
                    let info = try? file.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey])
                    guard info?.isRegularFile == true else { continue }
                    files += 1; bytes += UInt64(info?.fileSize ?? 0)
                    if names.count < 4 { names.append(file.lastPathComponent) }
                }
                lines.append("  \(entry.lastPathComponent)/ · \(files) fichiers · \(mib(bytes))"
                    + (names.isEmpty ? "" : " · ex. " + names.joined(separator: ", ")))
            }
            if entries.count > 60 { lines.append("  … \(entries.count - 60) autres") }
        }
        return lines.joined(separator: "\n")
    }
    // The game's imports MaxPS4 stubs: those it called ("<library> <NID> (n appels)"), and how many exist.
    private func missingImports(_ engine: OpaquePointer) -> (called: [String], total: Int) {
        var text = [CChar](repeating: 0, count: 16384)
        let total = maxps4_fex_engine_missing_imports(engine, &text, text.count)
        let called = String(cString: text).split(separator: "\n").compactMap { line -> String? in
            let parts = line.split(separator: " ")
            guard parts.count == 3, let calls = Int(parts[2]), calls > 0 else { return nil }
            return "\(parts[0]) \(parts[1]) (\(calls) appels)"
        }
        return (called, max(0, Int(total)))
    }
    // Since the previous heartbeat: images the game asked to flip, images the
    // host displayed, and the share of the time its thread spent in Metal
    // submissions (the rest is emulated CPU and HLE calls).
    private var lastRates: (time: TimeInterval, requested: UInt64, displayed: Int, submitMs: Double)?
    private func rates(_ engine: OpaquePointer, now: TimeInterval) -> String {
        let requested = maxps4_fex_engine_videoout_requested(engine)
        let submitMs = bridge?.submitMilliseconds ?? 0
        defer { lastRates = (now, requested, displayed, submitMs) }
        guard let last = lastRates, now > last.time else { return "" }
        let span = now - last.time
        let game = Double(requested - last.requested) / span
        let shown = Double(displayed - last.displayed) / span
        let metal = (submitMs - last.submitMs) / (span * 10)
        return String(format: "Cadence : jeu %.1f images/s · affichées %.1f/s · rendu Metal %.0f %% du temps du jeu\n", game, shown, metal)
    }
    private func audioStatus(_ engine: OpaquePointer) -> String {
        if let audioFailure { return "Audio : sortie iOS indisponible (\(audioFailure))" }
        var outputs: UInt64 = 0, written: UInt64 = 0, dropped: UInt64 = 0, silent: UInt64 = 0, audible: UInt64 = 0
        var ports: UInt32 = 0
        _ = maxps4_fex_engine_audio_status(engine, &outputs, &written, &dropped, &silent, &ports, &audible)
        return "Audio : \(ports) ports · \(outputs) appels Output · \(written) trames écrites (\(audible) non muettes) · \(dropped) perdues · \(silent) silencieuses"
    }
    // The CPU profile since the run started; empty unless profiling was asked.
    private func profile(_ engine: OpaquePointer) -> String {
        var text = [CChar](repeating: 0, count: 2048)
        _ = maxps4_fex_engine_profile_report(engine, &text, text.count)
        return String(cString: text)
    }
    // One sample of where the main guest thread is (only while outside HLE).
    private func mainThreadSample(_ engine: OpaquePointer) -> String {
        var sample = maxps4_fex_thread_sample()
        let status = maxps4_fex_engine_sample_main_thread(engine, &sample)
        guard status == 0 else { return status == -EBUSY ? "" : "Échantillon thread 1 : indisponible (\(status))" }
        func text<T>(_ field: T) -> String {
            withUnsafeBytes(of: field) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        }
        let rip = sample.guest_rip, relative = rip >= relocation ? rip - relocation : rip
        let position = "RIP 0x\(String(rip, radix: 16)) (relatif 0x\(String(relative, radix: 16)))"
        // Method 1: the signal was never taken; the thread was read through Mach.
        let mach = sample.method == 1
        let label = mach ? "Échantillon thread 1 (Mach, signal non reçu)" : "Échantillon thread 1"
        let pc = "PC 0x\(String(sample.host_pc, radix: 16))"
        let place: String
        switch sample.location {
        case 1: place = mach ? "\(label) : code JIT (\(pc)), \(position)" : "\(label) : code JIT, \(position)"
        case 2: place = "\(label) : répartiteur FEX, dernier \(position)"
        default: place = "\(label) : code hôte \(text(sample.symbol)) appelé depuis \(text(sample.caller)), dernier \(position)"
        }
        guard mach else { return place }
        func words<T>(_ field: T) -> [UInt64] {
            withUnsafeBytes(of: field) { Array($0.bindMemory(to: UInt64.self)) }
        }
        func hex(_ value: UInt64) -> String { "0x" + String(value, radix: 16) }
        let rips = words(sample.rips).prefix(Int(sample.rip_count))
            .map { hex($0 >= relocation ? $0 - relocation : $0) }
        let registers = words(sample.registers).enumerated().map { "x\($0.offset)=\(hex($0.element))" }
        var lines = [place]
        lines.append("RIP JIT relatifs (16 lectures) : " + (rips.isEmpty ? "aucun" : rips.joined(separator: " ")))
        lines.append("Registres hôte : " + registers.joined(separator: " "))
        let frames = text(sample.backtrace)
        if !frames.isEmpty { lines.append("Pile thread 1 : " + frames) }
        return lines.joined(separator: "\n")
    }

    // Input goes straight to the engine under inputLock alone, never waiting
    // for `lock` (display copies, completions, reports). `inputEngine` is set
    // while the game runs and cleared under inputLock before the engine closes.
    func setPaused(_ paused: Bool) {
        audio.setPaused(paused)
        inputLock.lock(); defer { inputLock.unlock() }
        if let inputEngine { maxps4_fex_engine_videoout_pause(inputEngine, paused ? 1 : 0) }
    }
    func input(_ pad: MaxPS4PadState) {
        inputLock.lock(); defer { inputLock.unlock() }
        self.pad = pad
        if let inputEngine { pad.send(to: inputEngine) }
    }
    private func sendInputLocked(_ engine: OpaquePointer) {
        inputLock.lock(); let latest = pad; inputLock.unlock()
        latest.send(to: engine)
    }
    private func snapshotTarget() throws -> (maxps4_videoout_frame, UInt64, MaxPS4GameMetalBridge)? {
        let asked = ProcessInfo.processInfo.systemUptime
        lock.lock(); defer { lock.unlock() }
        lockWait = ProcessInfo.processInfo.systemUptime - asked
        guard ready, !stopped, let engine, let bridge else { return nil }
        if let failure = bridge.failure { throw self.failure(failure, -EIO) }
        var info = maxps4_videoout_frame(), address: UInt64 = 0
        let status = maxps4_fex_engine_videoout_target(engine, &info, &address)
        if status == -EAGAIN { return nil }
        guard status == 0, info.ticket != 0 else { throw failure("VideoOut", status) }
        port = info.port; lastTicket = info.ticket
        inputLock.lock(); vblankPort = info.port; inputLock.unlock()
        return (info, address, bridge)
    }
    func snapshot() throws -> Frame? {
        let started = ProcessInfo.processInfo.systemUptime
        guard let (info, address, bridge) = try snapshotTarget() else { return nil }
        let located = ProcessInfo.processInfo.systemUptime
        // The retained bridge owns the texture copy. No engine pointer is used
        // here, so GPU completion need not block pad updates, vblank or stop.
        let copied = bridge.copyTarget(address: address, width: info.width, height: info.height)
        let timing = String(format: "Préparation : verrou session %.1f ms · cible %.1f ms · copie %.1f ms",
                            lockWait * 1000, (located - started - lockWait) * 1000,
                            (ProcessInfo.processInfo.systemUptime - located) * 1000)
        guard let texture = copied else {
            if missingSince == nil { missingSince = Date() }
            if Date().timeIntervalSince(missingSince!) > 5 {
                throw failure("Aucune texture Metal pour le buffer retourné", -EIO)
            }
            return nil
        }
        missingSince = nil
        return Frame(info: info, texture: texture, report: bridge.report + "\n" + timing)
    }
    @discardableResult func complete(ticket: UInt64, presented: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard let engine else { return false }
        let confirmed = presented && !stopped
        let result = maxps4_fex_engine_videoout_complete(engine, ticket, confirmed ? 1 : 0)
        if result == 0 && confirmed { displayed += 1 }
        return result == 0 && confirmed
    }
    func stop() {
        // Silence at once: a stop interrupts AudioOut's pacing, so until the
        // run ends the game's audio thread would queue its last buffers in a
        // tight loop and the speaker would stutter them. close() repeats this.
        audio.stop()
        // A paused game must run again for the stop to reach its threads.
        inputLock.lock()
        if let inputEngine { maxps4_fex_engine_videoout_pause(inputEngine, 0) }
        inputEngine = nil; inputLock.unlock()
        lock.lock()
        stopped = true; ready = false
        vblankTimer?.cancel(); vblankTimer = nil
        if let engine { _ = maxps4_fex_engine_request_stop(engine) }
        lock.unlock()
        setStage("arrêt demandé, threads du jeu en cours d’arrêt")
        // close runs after run_orbis returns, never while FEX/GPU uses memory.
        MaxPS4FEXMetalExecution.queue.async { self.close() }
    }
    private func close() {
        dispatchPrecondition(condition: .onQueue(MaxPS4FEXMetalExecution.queue))
        inputLock.lock(); inputEngine = nil; inputLock.unlock()
        lock.lock(); defer { lock.unlock() }
        ready = false
        vblankTimer?.cancel(); vblankTimer = nil
        if let engine {
            setStage("fermeture : audio")
            audio.stop()
            setStage("fermeture : GPU")
            _ = maxps4_fex_engine_gnm_backend(engine, 0)
            _ = maxps4_fex_engine_gnm_host_consumer(engine, nil, nil)
            bridge = nil; self.engine = nil
            setStage("fermeture : moteur")
            maxps4_fex_engine_close(engine)
        }
        setStage("fermée")
    }
    func run(url: URL, update: URL? = nil, addons: [MaxPS4GameFolders.Addon] = [], device: MTLDevice, nativeTextures: Bool = false, tso: Bool = true, outputHeight: Int = 1080, profiling: Bool = false, progress: @escaping @Sendable (String) -> Void) -> String {
        dispatchPrecondition(condition: .onQueue(MaxPS4FEXMetalExecution.queue))
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        defer { close() }
        do {
            progress(String(localized: "Ouverture du moteur…", bundle: .maxps4))
            _ = maxps4_fex_set_tso_emulation(tso ? 1 : 0)
            var opened: OpaquePointer?
            let status = maxps4_fex_engine_open(&opened)
            guard status == 0, let opened else { throw failure("Ouverture FEX", status) }
            lock.lock()
            if stopped { lock.unlock(); maxps4_fex_engine_close(opened); return String(localized: "Arrêté.", bundle: .maxps4) }
            engine = opened; lock.unlock()
            // Same permissive import probe as the validated Sonic boot probe.
            // This remains experimental: unresolved imports are reported by FEX.
            // Mode 2: stubs without the per-call stderr trace, which cost the
            // game's thread thousands of formatted lines per second.
            _ = maxps4_fex_engine_set_import_probe(opened, 2)
            var diagnostic = [CChar](repeating: 0, count: 2048)
            var relocation: UInt64 = 0
            progress(String(localized: "Lecture du dossier et chargement de eboot.bin et des PRX…", bundle: .maxps4))
            // An extracted update: its eboot.bin and modules first, and over /app0.
            _ = (update?.path ?? "").withCString { maxps4_fex_engine_set_update(opened, $0) }
            let loaded = url.path.withCString { path in
                diagnostic.withUnsafeMutableBufferPointer {
                    maxps4_fex_engine_load_game(opened, path, &relocation, $0.baseAddress, $0.count)
                }
            }
            guard loaded == 0 else { throw failure(String(cString: diagnostic), loaded) }
            // The output resolution the game is told (sceVideoOutGetResolutionStatus).
            _ = maxps4_fex_engine_videoout_output(opened, outputHeight == 720 ? 1280 : 1920, outputHeight == 720 ? 720 : 1080)
            _ = maxps4_fex_engine_set_profiling(opened, profiling ? 1 : 0)
            _ = maxps4_fex_engine_set_system_language(opened, MaxPS4Language.ps4LanguageID)
            // Installed add-on contents: listed to the game, mounted at /addcont0 on request.
            for addon in addons {
                _ = addon.label.withCString { label in addon.url.path.withCString { maxps4_fex_engine_add_addcont(opened, label, $0) } }
            }
            if let saves = MaxPS4Saves.root(for: url) {
                _ = saves.path.withCString { maxps4_fex_engine_set_savedata_root(opened, $0) }
            }
            progress(String(localized: "Jeu chargé. Préparation du rendu Metal…", bundle: .maxps4))
            guard let metal = MaxPS4GameMetalBridge(device: device, reader: { address, destination, count in
                maxps4_fex_engine_read_memory(opened, address, destination, count) == 0
            }, nativeTextures: nativeTextures) else { throw failure("Création du renderer Metal", -ENOMEM) }
            let attached = maxps4_fex_engine_gnm_host_consumer(opened,
                Unmanaged.passUnretained(metal).toOpaque(), { context, words, count in
                    guard let context, let words else { return -EINVAL }
                    return Unmanaged<MaxPS4GameMetalBridge>.fromOpaque(context).takeUnretainedValue().submit(words, count: count)
                })
            guard attached == 0 else { throw failure("Connexion GNM", attached) }
            let graphics = maxps4_fex_engine_gnm_backend(opened, 4)
            guard graphics == 0 else { throw failure("Backend Metal", graphics) }
            progress(String(localized: "Initialisation du JIT…", bundle: .maxps4))
            let initialized = maxps4_fex_engine_initialize(opened)
            guard initialized == 0 else { throw failure(String(localized: "Activer le JIT avec StikDebug puis réessayer", bundle: .maxps4), initialized) }
            let listing = Self.listing(of: [url] + (update.map { [$0] } ?? []))
            lock.lock()
            bridge = metal; ready = !stopped
            started = ProcessInfo.processInfo.systemUptime
            liveReport = MaxPS4Reports.live(for: url)
            folderListing = listing
            self.relocation = relocation
            let cancelled = stopped
            if ready {
                sendInputLocked(opened)
                inputLock.lock(); inputEngine = opened; inputLock.unlock()
            }
            if ready {
                let timer = DispatchSource.makeTimerSource(queue: vblanks)
                timer.schedule(deadline: .now(), repeating: 1.0 / 60, leeway: .milliseconds(1))
                timer.setEventHandler { [weak self] in self?.tick(at: ProcessInfo.processInfo.systemUptime) }
                vblankTimer = timer; timer.resume()
            }
            lock.unlock()
            if cancelled { return String(localized: "Arrêté.", bundle: .maxps4) }
            let audioError = audio.start(opened)
            lock.lock(); audioFailure = audioError; lock.unlock()
            progress(String(localized: "Jeu en cours d’exécution. En attente de la première image…", bundle: .maxps4))
            var value: UInt64 = 0, entry: UInt64 = 0
            var backend: UInt32 = 0
            setStage("jeu en cours")
            let ran = maxps4_fex_engine_run_orbis(opened, &value, &backend, &entry)
            setStage("jeu arrêté, rapport de fin")
            lock.lock(); let wasStopped = stopped; let frames = displayed; lock.unlock()
            var fault = maxps4_fex_guest_fault()
            _ = maxps4_fex_engine_last_guest_fault(opened, &fault)
            let relativeRIP = fault.rip >= relocation ? fault.rip - relocation : fault.rip
            let missing = missingImports(opened)
            let crashed = !wasStopped && (ran != 0 || fault.signal != 0)
            // The last live report, not a new one: reading the engine now would
            // sample the main guest thread, which is this very thread.
            lock.lock(); let live = lastBeat; lock.unlock()
            defer {
                let text = [result(of: ran, backend: backend, wasStopped: wasStopped, frames: frames, metal: metal.report, fault: fault, relative: relativeRIP),
                            "Fonctions PS4 manquantes appelées (\(missing.called.count) sur \(missing.total) absentes) :",
                            missing.called.isEmpty ? "aucune" : missing.called.joined(separator: "\n"), "",
                            "Derniers appels PS4 en échec :", failedCalls(opened), "",
                            "Fichiers ouverts par le jeu :", openedFiles(opened), "",
                            "Derniers appels PS4 :", recentCalls(opened), "",
                            "Sortie du jeu :", console(opened), "", memoryLayout(opened), movedMappings(opened), "", live, "", folderListing].joined(separator: "\n")
                outcome = Outcome(missing: missing.called, crashed: crashed, report: MaxPS4Reports.save(text, for: url))
                // The run ended normally: its final report replaces the live one.
                lock.lock(); let liveFile = liveReport; liveReport = nil; lock.unlock()
                if let liveFile { try? FileManager.default.removeItem(at: liveFile) }
            }
            return result(of: ran, backend: backend, wasStopped: wasStopped, frames: frames, metal: metal.report, fault: fault, relative: relativeRIP)
        } catch {
            let text = String(localized: "Échec : \(error.localizedDescription)", bundle: .maxps4)
            outcome = Outcome(missing: [], crashed: true, report: MaxPS4Reports.save(text, for: url))
            return text
        }
    }
    private func result(of ran: Int32, backend: UInt32, wasStopped: Bool, frames: Int, metal: String, fault: maxps4_fex_guest_fault, relative relativeRIP: UInt64) -> String {
            // A stop interrupts the main thread: where it was shows what a stalled game waits on.
            if wasStopped {
                return "Arrêté après \(frames) images présentées. \(metal)\nThread principal interrompu à RIP 0x\(String(fault.rip, radix: 16)) (relatif 0x\(String(relativeRIP, radix: 16)), relocation 0x\(String(relocation, radix: 16)))."
            }
            return "Exécution terminée (\(ran)), backend \(backend), \(frames) images. \(metal)\nSignal \(fault.signal), code \(fault.code), adresse mémoire 0x\(String(fault.address, radix: 16)), RIP 0x\(String(fault.rip, radix: 16)).\nRelocation 0x\(String(relocation, radix: 16)), RIP relatif 0x\(String(relativeRIP, radix: 16))."
    }
    private func failure(_ text: String, _ code: Int32) -> NSError {
        NSError(domain: "MaxPS4.Game", code: Int(code), userInfo: [NSLocalizedDescriptionKey: "\(text) (\(code))"])
    }
}

@MainActor final class MaxPS4GameSession: ObservableObject {
    @Published private(set) var source: MaxPS4GameSource?
    @Published private(set) var running = false
    @Published private(set) var hasPresented = false
    @Published private(set) var report = String(localized: "Activer le JIT avec StikDebug avant de lancer le jeu.", bundle: .maxps4)
    @Published private(set) var heartbeat = ""
    @Published private(set) var outcome = MaxPS4GameSource.Outcome()
    private var worker: MaxPS4GameSource?
    private var generation = UUID()
    private var stopping: UUID?
    private var heartbeatTimer: Timer?
    private var beating = false // A heartbeat is being read in the background.
    // The stopped source a new run waits for (one serial FEX queue), and
    // whether the new run has started, for the loading screen.
    private static var draining: MaxPS4GameSource? // App-wide: a new game screen waits for it too.
    private var launched = false
    func start(url: URL, update: URL? = nil, addons: [MaxPS4GameFolders.Addon] = [], device: MTLDevice, nativeTextures: Bool = false, tso: Bool = true, outputHeight: Int = 1080, profiling: Bool = false) {
        stop()
        let source = MaxPS4GameSource(), id = UUID()
        generation = id; self.source = source; worker = source; running = true
        hasPresented = false; heartbeat = ""; outcome = .init()
        report = String(localized: "Chargement du jeu et préparation Metal…", bundle: .maxps4)
        launched = false
        heartbeatTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.generation == id else { return }
                // Still waiting for the previous run to end: say where its stop is.
                if !self.launched, let previous = Self.draining {
                    let status = previous.stopStatus()
                    self.report = String(localized: "Chargement du jeu et préparation Metal…", bundle: .maxps4)
                        + "\n\nLa session précédente n’est pas encore arrêtée : " + status
                    return
                }
                guard !self.beating else { return }
                self.beating = true
                // Off the main thread: reading the engine (thread activity,
                // profiler, main thread sample) stalled the interface each second.
                DispatchQueue.global(qos: .utility).async {
                    let beat = source.heartbeat()
                    DispatchQueue.main.async { [weak self] in
                        guard let self else { return }
                        self.beating = false
                        if self.generation == id, let beat { self.heartbeat = beat }
                    }
                }
            }
        }
        MaxPS4FEXMetalExecution.queue.async { [weak self] in
            let result = source.run(url: url, update: update, addons: addons, device: device, nativeTextures: nativeTextures, tso: tso, outputHeight: outputHeight, profiling: profiling) { [weak self] text in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.generation == id, self.running else { return }
                    self.launched = true; Self.draining = nil
                    self.report = text
                }
            }
            DispatchQueue.main.async {
                guard let self else { return }
                // A stopped session's result (where its main thread was) still
                // replaces "Arrêt demandé." unless another session has started.
                if Self.draining === source { Self.draining = nil } // Its run is over.
                if self.generation == id || (self.stopping == id && !self.running) {
                    self.report = result; self.running = false; self.outcome = source.outcome
                }
            }
        }
    }
    func input(_ pad: MaxPS4PadState) { worker?.input(pad) }
    func presented(_ text: String) { if running { hasPresented = true; report = text } }
    func failed(_ text: String) { stop(); stopping = nil; report = text }
    // The game menu's pause: no vblank and no audio read, so a game paced on
    // them (Sonic Mania) waits where it is.
    func setPaused(_ paused: Bool) { worker?.setPaused(paused) }
    func stop() {
        stopping = worker == nil ? nil : generation
        if let worker { Self.draining = worker }
        generation = UUID(); worker?.stop(); worker = nil; source = nil; running = false
        heartbeatTimer?.invalidate(); heartbeatTimer = nil
        report = String(localized: "Arrêt demandé.", bundle: .maxps4)
    }
    // What "Partager le rapport" sends: the last image report plus live progress.
    var sharedReport: String { heartbeat.isEmpty ? report : report + "\n" + heartbeat }
    deinit { worker?.stop() }
}
#endif

// What the virtual DualShock 4 reports: PS4 button bits (sceKernel pad
// layout), sticks (0...255, 128 centred, Y down) and analog L2/R2.
struct MaxPS4PadState: Equatable {
    static let l3: UInt32 = 0x2, r3: UInt32 = 0x4, options: UInt32 = 0x8
    static let up: UInt32 = 0x10, right: UInt32 = 0x20, down: UInt32 = 0x40, left: UInt32 = 0x80
    static let l2: UInt32 = 0x100, r2: UInt32 = 0x200, l1: UInt32 = 0x400, r1: UInt32 = 0x800
    static let triangle: UInt32 = 0x1000, circle: UInt32 = 0x2000, cross: UInt32 = 0x4000, square: UInt32 = 0x8000
    static let touchpad: UInt32 = 0x100000
    var buttons: UInt32 = 0
    var lx: UInt8 = 128, ly: UInt8 = 128, rx: UInt8 = 128, ry: UInt8 = 128
    var l2: UInt8 = 0, r2: UInt8 = 0
    // Touch buttons: L2/R2 pressed all the way.
    static func touch(_ buttons: UInt32) -> Self {
        Self(buttons: buttons, l2: buttons & l2 != 0 ? 255 : 0, r2: buttons & r2 != 0 ? 255 : 0)
    }
#if MAXPS4_FEX_APP
    func send(to engine: OpaquePointer) {
        _ = maxps4_fex_engine_pad_update(engine, buttons, lx, ly, rx, ry, l2, r2, 1)
    }
#endif
}
