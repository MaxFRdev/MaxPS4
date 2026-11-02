import SwiftUI
import MetalKit
import GameController

#if MAXPS4_FEX_APP
private final class GameMetalDisplay: NSObject, MTKViewDelegate {
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private var source: MaxPS4GameSource?
    private var lastTexture: MTLTexture?
    private let snapshots = DispatchQueue(label: "com.enermaxfr.maxps4.display-copy", qos: .userInteractive)
    private var snapshotID: UUID?
    private var pendingTexture: MTLTexture?
    private var reportTime = ProcessInfo.processInfo.systemUptime
    private var reportFrames = 0
    // MTKView refresh callbacks (not the redraws a finished copy asks for):
    // the screen's real rate, 30 Hz in Low Power Mode.
    private var refreshes = 0
    private var preparationSeconds = 0.0
    private var needsRedraw = true
    // "fit" (whole image, black bars), "fill" (cropped) or "stretch".
    var scaling = "fit" { didSet { if scaling != oldValue { needsRedraw = true } } }
    var report: (String) -> Void
    var failed: (String) -> Void
    init(device: MTLDevice, report: @escaping (String) -> Void, failed: @escaping (String) -> Void) throws {
        guard let queue = device.makeCommandQueue() else { throw NSError(domain: "MaxPS4.Game", code: -12) }
        self.queue = queue; self.pipeline = try MaxPS4VideoOutMetal.make(device: device)
        self.report = report; self.failed = failed
        super.init()
    }
    func use(_ source: MaxPS4GameSource?) {
        if self.source !== source {
            cancel(); self.source = source; lastTexture = nil
            needsRedraw = true
            reportTime = ProcessInfo.processInfo.systemUptime; reportFrames = 0; preparationSeconds = 0; refreshes = 0
        }
    }
    func cancel() {
        snapshotID = nil; pendingTexture = nil
    }
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) { needsRedraw = true }
    // Each flip is copied into an owned texture and handed back to the game at
    // once (completed as presented), so the game never waits for the screen:
    // the next copy starts while this one is shown. Every refresh shows the
    // newest copy; flips the copy loop misses are skipped by VideoOut.
    func draw(in view: MTKView) {
        refreshes += 1
        guard !view.isPaused, view.drawableSize.width > 0, view.drawableSize.height > 0, let source else { return }
        // Acquire the drawable and encode only inside MTKView's draw cycle.
        if let texture = pendingTexture {
            pendingTexture = nil
            present(texture, in: view)
        } else if needsRedraw, let lastTexture {
            present(lastTexture, in: view)
        }
        let refreshed = ProcessInfo.processInfo.systemUptime
        // A copy still running: the game gets this refresh's vblank anyway.
        guard snapshotID == nil else { source.displayRefresh(at: refreshed); return }
        let id = UUID(); snapshotID = id
        snapshots.async { [weak self, weak view, source] in
            let started = ProcessInfo.processInfo.systemUptime
            let result = Result { try source.snapshot() }
            if case .success(let frame?) = result { source.complete(ticket: frame.info.ticket, presented: true) }
            // Only once the pending flip is taken, so the game's next image
            // cannot replace it before it is seen.
            source.displayRefresh(at: refreshed)
            let elapsed = ProcessInfo.processInfo.systemUptime - started
            DispatchQueue.main.async { [weak self, weak view] in
                guard let self, let view, self.snapshotID == id, self.source === source, !view.isPaused else { return }
                self.snapshotID = nil
                switch result {
                case .success(let frame):
                    guard let frame else { return }
                    self.preparationSeconds += elapsed
                    self.reportFrames += 1
                    let now = ProcessInfo.processInfo.systemUptime
                    let span = now - self.reportTime
                    // Report once per second instead of invalidating the
                    // entire SwiftUI game view for every displayed frame.
                    if span >= 1 {
                        let fps = Double(self.reportFrames) / span
                        let ms = self.preparationSeconds * 1000 / Double(self.reportFrames)
                        let hertz = Double(self.refreshes) / span
                        let info = ProcessInfo.processInfo
                        let power = (info.isLowPowerModeEnabled ? " (économie d’énergie)" : "")
                            + (info.thermalState == .serious ? " (chauffe)" : info.thermalState == .critical ? " (chauffe critique)" : "")
                        self.report(String(format: "%.1f images/s · écran %.0f Hz%@ · préparation image %.2f ms · ", fps, hertz, power, ms) + frame.report)
                        self.reportTime = now; self.reportFrames = 0; self.preparationSeconds = 0; self.refreshes = 0
                    }
                    // Shown on the next refresh, which also starts the next copy.
                    // Drawing at once here could wait for a free drawable and
                    // make the main thread miss every other refresh (31 Hz).
                    self.pendingTexture = frame.texture
                case .failure(let error): self.failed(error.localizedDescription)
                }
            }
        }
    }
    private func present(_ texture: MTLTexture, in view: MTKView) {
        guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable else {
            pendingTexture = texture // Retry on the next draw cycle.
            return
        }
        guard let command = queue.makeCommandBuffer(), let encoder = command.makeRenderCommandEncoder(descriptor: pass) else {
            failed("Création de la présentation Metal impossible."); return
        }
        MaxPS4VideoOutMetal.encode(encoder: encoder, pipeline: pipeline, texture: texture,
            output: view.drawableSize, fill: scaling == "fill", stretch: scaling == "stretch")
        encoder.endEncoding()
        command.addCompletedHandler { [weak self] buffer in
            guard buffer.status != .completed || buffer.error != nil else { return }
            let reason = buffer.error?.localizedDescription ?? "statut \(buffer.status.rawValue)"
            DispatchQueue.main.async { self?.failed("Présentation Metal : échec de la commande GPU (\(reason)).") }
        }
        lastTexture = texture
        needsRedraw = false
        command.present(drawable); command.commit()
    }
}

private struct GameMetalSurface: UIViewRepresentable {
    let device: MTLDevice
    let source: MaxPS4GameSource?
    let active: Bool
    let scaling: String
    let report: (String) -> Void
    let failed: (String) -> Void
    final class Coordinator { var renderer: GameMetalDisplay? }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: device)
        view.colorPixelFormat = .bgra8Unorm_srgb
        view.clearColor = MTLClearColorMake(0, 0, 0, 1)
        // 120 Hz where the screen allows it (more chances to show a new copy
        // early). Three drawables: with two, presenting could wait for one.
        view.preferredFramesPerSecond = 120; view.isPaused = true
        do {
            context.coordinator.renderer = try GameMetalDisplay(device: device, report: report, failed: failed)
            view.delegate = context.coordinator.renderer
        } catch { DispatchQueue.main.async { failed(error.localizedDescription) } }
        return view
    }
    func updateUIView(_ view: MTKView, context: Context) {
        context.coordinator.renderer?.report = report
        context.coordinator.renderer?.failed = failed
        context.coordinator.renderer?.scaling = scaling
        context.coordinator.renderer?.use(source)
        view.isPaused = !active || context.coordinator.renderer == nil
        if !active { context.coordinator.renderer?.cancel() }
    }
    static func dismantleUIView(_ view: MTKView, coordinator: Coordinator) {
        view.isPaused = true; view.delegate = nil
        coordinator.renderer?.cancel(); coordinator.renderer?.use(nil)
        view.releaseDrawables(); coordinator.renderer = nil
    }
}
#endif

struct MaxPS4GameView: View {
    let directory: URL
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingMenu = false
    @State private var showingDetails = false
    @State private var details = ""
#if MAXPS4_FEX_APP
    @StateObject private var session = MaxPS4GameSession()
    @State private var device = MTLCreateSystemDefaultDevice()
    @State private var touch: UInt32 = 0
    // The touch sticks, -1...1 on each axis, y growing downwards like the PS4's.
    @State private var leftStick = CGPoint.zero
    @State private var rightStick = CGPoint.zero
    @AppStorage("maxps4.touch.scale") private var touchScale = 1.0
    @AppStorage("maxps4.touch.opacity") private var touchOpacity = 0.85
    @AppStorage("maxps4.pad.stickDpad") private var stickAsDpad = true
    @AppStorage("maxps4.touch.rightStick") private var rightStickShown = true
    @State private var hasLaunched = false
    @State private var gamepad = MaxPS4GameView.gamepadConnected()
    @AppStorage("maxps4.metal.nativeTextures") private var nativeTextures = true
    @AppStorage("maxps4.fex.tso") private var tso = true
    @AppStorage("maxps4.display.scaling") private var scaling = "fit"
    @AppStorage("maxps4.video.height") private var outputHeight = 1080
    @AppStorage("maxps4.fex.profile") private var profiling = false
    // Réglages › Mode développeur: test options and the technical session report.
    @AppStorage("maxps4.developer") private var developer = false
    private let poll = Timer.publish(every: 1.0 / 60, on: .main, in: .common).autoconnect()
#endif
    init(directory: URL) {
        self.directory = directory
#if MAXPS4_FEX_APP
        // Each game keeps its own settings under "<key>.<game ID>"; a game
        // played for the first time starts from the app-wide values.
        let id = MaxPS4Saves.gameID(for: directory), defaults = UserDefaults.standard
        func own(_ key: String) -> String { key + "." + id }
        func value<T>(_ key: String, _ fallback: T) -> T { defaults.object(forKey: key) as? T ?? fallback }
        _touchScale = AppStorage(wrappedValue: value("maxps4.touch.scale", 1.0), own("maxps4.touch.scale"))
        _touchOpacity = AppStorage(wrappedValue: value("maxps4.touch.opacity", 0.85), own("maxps4.touch.opacity"))
        _stickAsDpad = AppStorage(wrappedValue: value("maxps4.pad.stickDpad", true), own("maxps4.pad.stickDpad"))
        _rightStickShown = AppStorage(wrappedValue: value("maxps4.touch.rightStick", true), own("maxps4.touch.rightStick"))
        _nativeTextures = AppStorage(wrappedValue: value("maxps4.metal.nativeTextures", true), own("maxps4.metal.nativeTextures"))
        _tso = AppStorage(wrappedValue: value("maxps4.fex.tso", true), own("maxps4.fex.tso"))
        _scaling = AppStorage(wrappedValue: value("maxps4.display.scaling", "fit"), own("maxps4.display.scaling"))
        _outputHeight = AppStorage(wrappedValue: value("maxps4.video.height", 1080), own("maxps4.video.height"))
#endif
    }
    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
#if MAXPS4_FEX_APP
            if let device {
                GameMetalSurface(device: device, source: session.source,
                    active: session.running && scenePhase == .active, scaling: scaling,
                    report: session.presented, failed: session.failed)
                    .ignoresSafeArea()
                    .accessibilityIdentifier("maxps4.game.surface")
                // A connected controller replaces the touch controls.
                if session.running && !showingMenu && !gamepad { screenControls }
                if session.running && !session.hasPresented {
                    VStack(spacing: 12) {
                        ProgressView().tint(.cyan)
                        Text(session.report).font(.footnote).multilineTextAlignment(.center)
                    }
                    .padding(20)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 20))
                    .padding(30).allowsHitTesting(false)
                }
                if !session.running {
                    VStack(spacing: 18) {
                        Text(directory.lastPathComponent).font(.title2.bold())
                        if hasLaunched {
                            Text("Session terminée. Consulte le menu pour les détails.")
                                .font(.footnote).foregroundStyle(.secondary)
                            // Why a game stopped, for the player, with the report to send.
                            let outcome = session.outcome
                            if !outcome.missing.isEmpty {
                                Text("Ce jeu a appelé \(outcome.missing.count) fonctions PS4 que MaxPS4 ne gère pas encore : c’est sans doute ce qui le bloque.")
                                    .font(.footnote).multilineTextAlignment(.center).frame(maxWidth: 320)
                            }
                            if outcome.crashed {
                                Text("Le jeu s’est arrêté sur une erreur.").font(.footnote).foregroundStyle(.orange)
                            }
                            if let report = outcome.report {
                                ShareLink(item: report) { Label("Partager le rapport de compatibilité", systemImage: "square.and.arrow.up") }
                                    .font(.footnote).tint(.cyan)
                                Text("Rapport enregistré dans Fichiers › MaxPS4 › Rapports.")
                                    .font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                        Button(hasLaunched ? "Rejouer" : "Jouer") {
                            touch = 0; leftStick = .zero; rightStick = .zero; hasLaunched = true
                            MaxPS4GameFolders.shared.remember(directory)
                            let update = MaxPS4GameFolders.useUpdate(for: directory) ? MaxPS4GameFolders.shared.update(for: directory)?.url : nil
                            let addons = MaxPS4GameFolders.shared.addons(for: directory)
                            // Without developer mode the options take their default
                            // values whatever was stored: TSO and native textures on
                            // (Sonic Mania stutters without TSO), the profiler off.
                            session.start(url: directory, update: update, addons: addons, device: device,
                                          nativeTextures: !developer || nativeTextures, tso: !developer || tso,
                                          outputHeight: outputHeight, profiling: developer && profiling)
                        }.buttonStyle(.borderedProminent).tint(.cyan)
                        Picker("Affichage", selection: $scaling) {
                            Text("Ajuster").tag("fit")
                            Text("Remplir").tag("fill")
                            Text("Étirer").tag("stretch")
                        }.pickerStyle(.segmented).frame(maxWidth: 320)
                        Picker("Résolution du jeu", selection: $outputHeight) {
                            Text("1080p").tag(1080)
                            Text("720p").tag(720)
                        }.pickerStyle(.segmented).frame(maxWidth: 320)
                        Text("Résolution annoncée au jeu : 720p peut alléger le rendu si le jeu s’y adapte.")
                            .font(.caption2).foregroundStyle(.secondary).multilineTextAlignment(.center).frame(maxWidth: 320)
                        if developer {
                            Toggle("Textures Metal natives (expérimental)", isOn: $nativeTextures)
                                .font(.footnote).frame(maxWidth: 320)
                            Toggle("Ordre mémoire x86 (TSO)", isOn: $tso)
                                .font(.footnote).frame(maxWidth: 320)
                            Toggle("Profileur processeur (diagnostic)", isOn: $profiling)
                                .font(.footnote).frame(maxWidth: 320)
                        }
                        Text("Ces réglages sont mémorisés pour ce jeu.")
                            .font(.caption2).foregroundStyle(.secondary).frame(maxWidth: 320)
                    }
                    .padding(28)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24))
                    .padding(24)
                }
            } else { Text("Metal indisponible sur cet appareil.") }
#else
            Text("Le runtime FEX n’est pas inclus dans cette compilation.")
#endif
            VStack {
                HStack {
                    Button {
#if MAXPS4_FEX_APP
                        touch = 0; leftStick = .zero; rightStick = .zero; session.input(MaxPS4PadState())
#endif
                        showingMenu = true
                    } label: {
                        Image(systemName: "ellipsis")
                            .font(.headline).frame(width: 44, height: 44)
                            .background(.black.opacity(0.35), in: Circle())
                            .overlay(Circle().stroke(.cyan.opacity(0.4)))
                    }
                    .accessibilityLabel("Menu du jeu")
                    .accessibilityIdentifier("maxps4.game.menu")
                    Spacer()
                }
                Spacer()
            }.padding(12)
        }
        .preferredColorScheme(.dark)
        .statusBarHidden(true)
        // Games are played in landscape: turn the iPhone there while this
        // screen is shown, and back to portrait for the library after.
        .onAppear { MaxPS4Orientation.landscape() }
        .onDisappear { MaxPS4Orientation.release() }
        .persistentSystemOverlays(.hidden)
        .sheet(isPresented: $showingMenu) {
            NavigationStack {
                ZStack {
                    MaxPS4Background()
                    ScrollView {
                        VStack(alignment: .leading, spacing: 22) {
                            Text(directory.lastPathComponent).font(.title2.bold())
                            Button("Revenir au jeu") { showingMenu = false }
                                .buttonStyle(.borderedProminent).tint(.cyan)
#if MAXPS4_FEX_APP
                            Picker("Affichage", selection: $scaling) {
                                Text("Ajuster").tag("fit")
                                Text("Remplir").tag("fill")
                                Text("Étirer").tag("stretch")
                            }.pickerStyle(.segmented)
                            VStack(alignment: .leading, spacing: 10) {
                                Text("Commandes").font(.headline)
                                Toggle("Stick gauche comme croix directionnelle", isOn: $stickAsDpad)
                                Toggle("Stick droit tactile", isOn: $rightStickShown)
                                Text("Taille des commandes tactiles")
                                Slider(value: $touchScale, in: 0.7...1.5)
                                Text("Opacité des commandes tactiles")
                                Slider(value: $touchOpacity, in: 0.25...1)
                            }.font(.subheadline).tint(.cyan)
                            if session.running {
                                Button("Arrêter la session", role: .destructive) {
                                    touch = 0; leftStick = .zero; rightStick = .zero; session.stop(); showingMenu = false
                                }
                            }
                            // A snapshot of the report, refreshed on demand: the live
                            // report changes every second while the game runs.
                            if developer {
                            DisclosureGroup("Détails de la session", isExpanded: $showingDetails) {
                                VStack(alignment: .leading, spacing: 12) {
                                    // One lazily laid-out view per line: a single Text of
                                    // the whole report stalled the menu while it was laid out.
                                    LazyVStack(alignment: .leading, spacing: 2) {
                                        ForEach(Array(details.split(separator: "\n", omittingEmptySubsequences: false).enumerated()), id: \.offset) {
                                            Text(verbatim: String($0.element)).font(.footnote.monospaced())
                                        }
                                    }
                                    HStack {
                                        Button("Actualiser") { details = session.sharedReport }
                                        Spacer()
                                        ShareLink(item: details) { Label("Partager le rapport", systemImage: "square.and.arrow.up") }
                                    }
                                }.padding(.top, 8)
                            }
                            .tint(.cyan)
                            .onChange(of: showingDetails) { _, open in if open { details = session.sharedReport } }
                            }
#endif
                            Button("Quitter le jeu", role: .destructive) { dismiss() }
                        }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .navigationTitle("Menu du jeu").navigationBarTitleDisplayMode(.inline)
                .toolbar { Button("Fermer") { showingMenu = false } }
            }.preferredColorScheme(.dark)
        }
#if MAXPS4_FEX_APP
        .onReceive(poll) { _ in
            guard session.running, scenePhase == .active, !showingMenu else { return }
            session.input(Self.padState(touchPad, touchLeft: leftStick, touchRight: rightStick, stickAsDpad: stickAsDpad))
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            gamepad = Self.gamepadConnected(); if gamepad { touch = 0; leftStick = .zero; rightStick = .zero }
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            gamepad = Self.gamepadConnected()
        }
        // The game waits while its menu is open.
        .onChange(of: showingMenu) { _, open in session.setPaused(open) }
        .onDisappear { touch = 0; leftStick = .zero; rightStick = .zero; session.stop(); UIApplication.shared.isIdleTimerDisabled = false }
        .onChange(of: scenePhase) { _, phase in if phase != .active { touch = 0; leftStick = .zero; rightStick = .zero; session.stop() } }
        // A running game keeps the screen awake: auto-lock would stop it.
        .onChange(of: session.running, initial: true) { _, running in UIApplication.shared.isIdleTimerDisabled = running }
#endif
    }
#if MAXPS4_FEX_APP
    // The touch controls plus every connected controller, as one DualShock 4.
    // `stickAsDpad` also presses the D-pad with the left stick (2D games that
    // read only the D-pad); the stick's analog position is always sent.
    private static func padState(_ touchPad: MaxPS4PadState, touchLeft: CGPoint, touchRight: CGPoint,
                                 stickAsDpad: Bool) -> MaxPS4PadState {
        typealias P = MaxPS4PadState
        var state = touchPad
        // Deflection to beat (dead zone first); the touch sticks compete too.
        var left = max(0.1, Float(abs(touchLeft.x) + abs(touchLeft.y)))
        var right = max(0.1, Float(abs(touchRight.x) + abs(touchRight.y)))
        func axis(_ value: Float) -> UInt8 { UInt8(max(0, min(255, ((value + 1) * 127.5).rounded()))) }
        func analog(_ value: Float) -> UInt8 { UInt8(max(0, min(255, (value * 255).rounded()))) }
        for controller in GCController.controllers() {
            var b: UInt32 = 0
            if let pad = controller.extendedGamepad {
                let lx = pad.leftThumbstick.xAxis.value, ly = pad.leftThumbstick.yAxis.value
                let rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value
                if pad.dpad.left.isPressed || (stickAsDpad && lx < -0.3) { b |= P.left }
                if pad.dpad.right.isPressed || (stickAsDpad && lx > 0.3) { b |= P.right }
                if pad.dpad.up.isPressed || (stickAsDpad && ly > 0.3) { b |= P.up }
                if pad.dpad.down.isPressed || (stickAsDpad && ly < -0.3) { b |= P.down }
                if pad.buttonA.isPressed { b |= P.cross }
                if pad.buttonB.isPressed { b |= P.circle }
                if pad.buttonX.isPressed { b |= P.square }
                if pad.buttonY.isPressed { b |= P.triangle }
                if pad.buttonMenu.isPressed { b |= P.options }
                if pad.leftShoulder.isPressed { b |= P.l1 }
                if pad.rightShoulder.isPressed { b |= P.r1 }
                if pad.leftTrigger.isPressed { b |= P.l2 }
                if pad.rightTrigger.isPressed { b |= P.r2 }
                if pad.leftThumbstickButton?.isPressed == true { b |= P.l3 }
                if pad.rightThumbstickButton?.isPressed == true { b |= P.r3 }
                // The touchpad click; on other controllers the View/Select button.
                if let dualSense = pad as? GCDualSenseGamepad {
                    if dualSense.touchpadButton.isPressed { b |= P.touchpad }
                } else if let dualShock = pad as? GCDualShockGamepad {
                    if dualShock.touchpadButton.isPressed { b |= P.touchpad }
                } else if pad.buttonOptions?.isPressed == true { b |= P.touchpad }
                state.l2 = max(state.l2, analog(pad.leftTrigger.value))
                state.r2 = max(state.r2, analog(pad.rightTrigger.value))
                // PS4 sticks grow downwards; the most deflected controller wins.
                if abs(lx) + abs(ly) > left { left = abs(lx) + abs(ly); state.lx = axis(lx); state.ly = axis(-ly) }
                if abs(rx) + abs(ry) > right { right = abs(rx) + abs(ry); state.rx = axis(rx); state.ry = axis(-ry) }
            } else if let pad = controller.microGamepad {
                if pad.dpad.xAxis.value < -0.3 { b |= P.left }
                if pad.dpad.xAxis.value > 0.3 { b |= P.right }
                if pad.dpad.yAxis.value > 0.3 { b |= P.up }
                if pad.dpad.yAxis.value < -0.3 { b |= P.down }
                if pad.buttonA.isPressed { b |= P.cross }
                if pad.buttonX.isPressed { b |= P.square }
                if pad.buttonMenu.isPressed { b |= P.options }
            }
            state.buttons |= b
        }
        return state
    }
    // The touch buttons and sticks; with `stickAsDpad` the left stick also
    // presses the D-pad (the touch arrows stay beside it).
    private var touchPad: MaxPS4PadState {
        typealias P = MaxPS4PadState
        func axis(_ value: CGFloat) -> UInt8 { UInt8(max(0, min(255, ((value + 1) * 127.5).rounded()))) }
        var state = P.touch(touch)
        if leftStick != .zero { state.lx = axis(leftStick.x); state.ly = axis(leftStick.y) }
        if rightStick != .zero { state.rx = axis(rightStick.x); state.ry = axis(rightStick.y) }
        if stickAsDpad {
            if leftStick.x < -0.3 { state.buttons |= P.left }
            if leftStick.x > 0.3 { state.buttons |= P.right }
            if leftStick.y < -0.3 { state.buttons |= P.up }
            if leftStick.y > 0.3 { state.buttons |= P.down }
        }
        return state
    }
    private static func gamepadConnected() -> Bool {
        GCController.controllers().contains { $0.extendedGamepad != nil || $0.microGamepad != nil }
    }
    private var screenControls: some View {
        GeometryReader { geometry in
            let base: CGFloat = max(44, min(56, (geometry.size.width - 32) / 6))
            let size: CGFloat = max(36, min(80, base * CGFloat(touchScale)))
            VStack(spacing: 0) {
                control("Options", label: "Options", bit: 0x8, size: 44)
                Spacer(minLength: 0)
            }.frame(maxWidth: .infinity)
            // Shoulder buttons below the menu button's corner.
            VStack {
                HStack {
                    VStack(spacing: 8) {
                        control("L2", label: "L2", bit: MaxPS4PadState.l2, size: size * 0.8)
                        control("L1", label: "L1", bit: MaxPS4PadState.l1, size: size * 0.8)
                    }
                    Spacer(minLength: 0)
                    VStack(spacing: 8) {
                        control("R2", label: "R2", bit: MaxPS4PadState.r2, size: size * 0.8)
                        control("R1", label: "R1", bit: MaxPS4PadState.r1, size: size * 0.8)
                    }
                }
                Spacer(minLength: 0)
            }.padding(.top, 56)
            HStack(alignment: .bottom, spacing: 0) {
                // The D-pad, then the left stick beside it.
                HStack(alignment: .bottom, spacing: size * 0.25) {
                    VStack(spacing: 0) {
                        control("↑", label: "Haut", bit: 0x10, size: size)
                        HStack(spacing: 0) {
                            control("←", label: "Gauche", bit: 0x80, size: size)
                            Color.clear.frame(width: size, height: size).allowsHitTesting(false)
                            control("→", label: "Droite", bit: 0x20, size: size)
                        }
                        control("↓", label: "Bas", bit: 0x40, size: size)
                    }
                    stickControl($leftStick, name: "L", label: "Stick gauche", size: size * 2)
                }
                Spacer(minLength: 0)
                // Where a DualShock 4 has it: below and left of the face buttons.
                if rightStickShown {
                    stickControl($rightStick, name: "R", label: "Stick droit", size: size * 1.9).padding(.trailing, size * 0.25)
                }
                VStack(spacing: 0) {
                    control("△", label: "Triangle", bit: 0x1000, size: size)
                    HStack(spacing: 0) {
                        control("□", label: "Carré", bit: 0x8000, size: size)
                        Color.clear.frame(width: size, height: size).allowsHitTesting(false)
                        control("○", label: "Rond", bit: 0x2000, size: size)
                    }
                    control("✕", label: "Croix, sauter", bit: 0x4000, size: size)
                }
            }.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
        }
        .padding(6)
        .opacity(touchOpacity)
        .accessibilityIdentifier("maxps4.game.controls")
    }

    // A touch stick: full deflection at 80% of the radius; the knob stays inside the ring.
    private func stickControl(_ stick: Binding<CGPoint>, name: String, label: LocalizedStringKey, size: CGFloat) -> some View {
        let radius = size / 2, position = stick.wrappedValue, active = position != .zero
        return ZStack {
            Circle().fill(Color.black.opacity(0.25))
                .overlay(Circle().stroke(active ? Color.cyan : Color.white.opacity(0.35), lineWidth: 1))
            Text(verbatim: name).font(.system(size: 14, weight: .bold, design: .rounded))
                .foregroundStyle(.white.opacity(active ? 1 : 0.85))
                .frame(width: size * 0.42, height: size * 0.42)
                .background(active ? Color.cyan.opacity(0.50) : Color.white.opacity(0.18), in: Circle())
                .overlay(Circle().stroke(active ? Color.cyan : Color.white.opacity(0.35), lineWidth: 1))
                .offset(x: position.x * radius * 0.6, y: position.y * radius * 0.6)
        }
        .frame(width: size, height: size)
        .shadow(color: .purple.opacity(active ? 0.45 : 0.15), radius: 6)
        .contentShape(Circle())
        .accessibilityLabel(label)
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { value in
                var x = (value.location.x - radius) / (radius * 0.8)
                var y = (value.location.y - radius) / (radius * 0.8)
                let length = (x * x + y * y).squareRoot()
                if length > 1 { x /= length; y /= length }
                stick.wrappedValue = CGPoint(x: x, y: y)
                session.input(touchPad)
            }
            .onEnded { _ in stick.wrappedValue = .zero; session.input(touchPad) })
    }

    private func control(_ name: String, label: LocalizedStringKey, bit: UInt32, size: CGFloat) -> some View {
        let pressed = touch & bit != 0
        return Text(name)
            .font(.system(size: name == "Options" ? 10 : name.count == 2 ? 14 : 22, weight: .bold, design: .rounded))
            .foregroundStyle(.white.opacity(pressed ? 1 : 0.85))
            .frame(width: size, height: size)
            .background(pressed ? Color.cyan.opacity(0.50) : Color.black.opacity(0.25), in: Circle())
            .overlay(Circle().stroke(pressed ? Color.cyan : Color.white.opacity(0.35), lineWidth: 1))
            .shadow(color: .purple.opacity(pressed ? 0.45 : 0.15), radius: 6)
            .contentShape(Circle())
            .accessibilityLabel(label).accessibilityAddTraits(.isButton)
            .accessibilityAction {
                touch |= bit; session.input(touchPad)
                touch &= ~bit
            }
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in touch |= bit; session.input(touchPad) }
                .onEnded { _ in touch &= ~bit; session.input(touchPad) })
    }
#endif
}
