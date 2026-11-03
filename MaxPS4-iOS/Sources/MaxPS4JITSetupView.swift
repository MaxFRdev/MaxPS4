import SwiftUI

// Verify preparation on the serial FEX queue, with a bounded native probe.
// No interpreter fallback or developer test menus are part of this screen.
private struct MaxPS4JITReadiness: Sendable {
    let ready: Bool
    let detail: String
    static func check() -> Self {
#if MAXPS4_FEX_APP
        var opened: OpaquePointer?
        let status = maxps4_fex_engine_open(&opened)
        guard status == 0, let opened else { return Self(ready: false, detail: "Ouverture du moteur : \(status)") }
        defer { maxps4_fex_engine_close(opened) }
        let initialized = maxps4_fex_engine_initialize(opened)
        guard initialized == 0 else {
            return Self(ready: false, detail: "Préparation du JIT : \(initialized)\nÉtat mémoire : \(maxps4_jit_state())\nErreur mémoire : \(maxps4_jit_error())\nÉtape : \(maxps4_jit_failure_stage())\nCode Mach : \(maxps4_jit_failure_mach())")
        }
        var value: UInt64 = 0
        var backend: UInt32 = 0
        let checked = maxps4_fex_engine_probe(opened, &value, &backend)
        let ready = checked == 0 && value == 42 && backend == UInt32(MAXPS4_CPU_FEX_ARM64_JIT)
        return Self(ready: ready, detail: "Vérification du JIT : \(checked)\nBackend : \(backend)\nRésultat : \(value)")
#else
        return Self(ready: false, detail: String(localized: "Le moteur FEX n’est pas inclus dans cette version.", bundle: .maxps4))
#endif
    }
}

struct MaxPS4JITSetupView: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @State private var checking = false
    @State private var requested = false
    @State private var ready = false
    @State private var message = String(localized: "Active le JIT avec StikDebug avant de lancer un jeu.", bundle: .maxps4)
    @State private var detail = ""

    var body: some View {
        NavigationStack {
            ZStack {
                MaxPS4Background()
                ScrollView {
                    VStack(alignment: .leading, spacing: 22) {
                        Image(systemName: ready ? "checkmark.shield.fill" : "bolt.fill")
                            .font(.system(size: 44)).foregroundStyle(ready ? Color.green : Color.cyan)
                        Text(ready ? "JIT prêt" : "Préparer le JIT")
                            .font(.system(size: 32, weight: .black, design: .rounded))
                        Text(message).foregroundStyle(.white.opacity(0.70))
                        DisclosureGroup("Configurer le PairingFile") {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("Dans StikDebug, importe le même fichier d’appairage que pour SideStore. Active aussi le VPN si StikDebug le demande.")
                                Text("Reviens ici, touche « Activer avec StikDebug », puis attends la fin de son script avant de vérifier le JIT.")
                                Text("Le PairingFile est propre à ton iPhone : garde-le privé et ne le joins pas aux rapports MaxPS4.")
                            }
                            .font(.footnote)
                            .foregroundStyle(.white.opacity(0.70))
                            .padding(.top, 8)
                        }
                        .tint(.cyan)
                        if checking { ProgressView("Vérification de l’activation…").tint(.cyan) }
                        Button {
                            requested = true
                            detail = MaxPS4StikActivation.begin { result in detail = result; message = result }
                            message = detail
                        } label: {
                            Label("Activer avec StikDebug", systemImage: "arrow.up.right.square")
                                .frame(maxWidth: .infinity).padding(.vertical, 8)
                        }
                        .buttonStyle(.borderedProminent).tint(.blue).disabled(checking || ready)
                        .accessibilityIdentifier("maxps4.activateJIT")
                        if ready {
                            Button("Revenir au jeu") { dismiss() }.buttonStyle(.bordered).tint(.cyan)
                        } else {
                            Button("Vérifier l’activation") { verify() }.tint(.cyan).disabled(checking)
                        }
                        if !detail.isEmpty {
                            DisclosureGroup("Détails de l’activation") {
                                VStack(alignment: .leading, spacing: 14) {
                                    Text(detail).font(.footnote.monospaced()).textSelection(.enabled)
                                    ShareLink(item: detail) { Label("Partager le rapport", systemImage: "square.and.arrow.up") }
                                    Button("Réinitialiser le JIT") {
                                        detail = MaxPS4StikActivation.release()
                                        message = detail; ready = false; requested = false
                                    }.disabled(checking)
                                }.padding(.top, 12)
                            }.tint(.cyan)
                        }
                    }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .navigationTitle("JIT et StikDebug").navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Fermer") { dismiss() } }
        }
        .preferredColorScheme(.dark)
        .onAppear {
#if MAXPS4_FEX_APP
            if maxps4_jit_state() == 2 { verify() }
#endif
        }
        .onChange(of: scenePhase) { phase in
            guard phase == .active, requested else { return }
            detail = MaxPS4StikActivation.confirmation()
            verify()
        }
    }
    private func verify() {
        guard !checking else { return }
        checking = true
        Task {
            let result = await withCheckedContinuation { continuation in
                MaxPS4FEXMetalExecution.queue.async { continuation.resume(returning: MaxPS4JITReadiness.check()) }
            }
            ready = result.ready; detail = result.detail; checking = false
            message = ready ? String(localized: "L’activation est vérifiée. Tu peux lancer ton jeu.", bundle: .maxps4) : String(localized: "Le JIT n’est pas encore prêt. Termine l’activation dans StikDebug, puis réessaie.", bundle: .maxps4)
        }
    }
}
