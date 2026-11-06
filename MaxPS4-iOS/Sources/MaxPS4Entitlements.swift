import Foundation
import SwiftUI
import Darwin
import os

// What the installed app really holds: entitlements come from the signature
// SideStore wrote, not from the IPA, and iOS ignores those its profile refuses.
@_silgen_name("csops")
private func csops(_ pid: pid_t, _ operation: UInt32, _ address: UnsafeMutableRawPointer?, _ size: Int) -> Int32

enum MaxPS4Entitlements {
    enum State { case active, missing, unknown }
    struct Item: Identifiable {
        let id: String
        let title: LocalizedStringKey
        let state: State
    }
    // The running signature's entitlements (CS_OPS_ENTITLEMENTS_BLOB): a big-endian
    // magic and length, then the XML property list.
    private static func signature() -> [String: Any]? {
        var blob = [UInt8](repeating: 0, count: 1 << 18)
        guard csops(getpid(), 7, &blob, blob.count) == 0 else { return nil }
        let length = blob[4..<8].reduce(0) { $0 << 8 | Int($1) }
        guard length > 8, length <= blob.count else { return nil }
        return (try? PropertyListSerialization.propertyList(from: Data(blob[8..<length]), format: nil)) as? [String: Any]
    }
    // Otherwise what the provisioning profile allows: an XML list inside its CMS wrapper.
    private static func profile() -> [String: Any]? {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
              let data = try? Data(contentsOf: url),
              let start = data.range(of: Data("<?xml".utf8)),
              let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex),
              let list = try? PropertyListSerialization.propertyList(from: data[start.lowerBound..<end.upperBound], format: nil)
        else { return nil }
        return (list as? [String: Any])?["Entitlements"] as? [String: Any]
    }
    static func items() -> [Item] {
        let held = signature() ?? profile()
        let wanted: [(String, LocalizedStringKey)] = [
            ("get-task-allow", "Débogage (JIT)"),
            ("com.apple.developer.kernel.extended-virtual-addressing", "Grand espace d’adresses"),
            ("com.apple.developer.kernel.increased-memory-limit", "Mémoire augmentée")]
        return wanted.map { key, title in
            let state: State = held == nil ? .unknown : held?[key] as? Bool == true ? .active : .missing
            return Item(id: key, title: title, state: state)
        }
    }
    // Mebibytes the app uses and its limit (use plus what iOS still allows).
    static func memory() -> (used: UInt64, limit: UInt64)? {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.stride / MemoryLayout<natural_t>.stride)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        guard status == KERN_SUCCESS else { return nil }
        let used = UInt64(info.phys_footprint), left = UInt64(os_proc_available_memory())
        return (used >> 20, (used + left) >> 20)
    }
}

struct MaxPS4EntitlementsCard: View {
    private let items = MaxPS4Entitlements.items()
    private let memory = MaxPS4Entitlements.memory()
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Droits de l’application", systemImage: "checkmark.shield").font(.headline)
            ForEach(items) { item in
                HStack {
                    Image(systemName: icon(item.state)).foregroundStyle(color(item.state))
                    Text(item.title)
                    Spacer()
                    Text(label(item.state)).font(.footnote).foregroundStyle(.white.opacity(0.6))
                }
            }
            if let memory {
                Text("Limite de RAM iOS : \(memory.limit) Mio").font(.footnote).foregroundStyle(.white.opacity(0.6))
            }
        }
        .padding(18).frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 22))
    }
    private func icon(_ state: MaxPS4Entitlements.State) -> String {
        switch state {
        case .active: return "checkmark.circle.fill"
        case .missing: return "xmark.circle.fill"
        case .unknown: return "questionmark.circle.fill"
        }
    }
    private func color(_ state: MaxPS4Entitlements.State) -> Color {
        switch state {
        case .active: return .green
        case .missing: return .red
        case .unknown: return .orange
        }
    }
    private func label(_ state: MaxPS4Entitlements.State) -> LocalizedStringKey {
        switch state {
        case .active: return "Actif"
        case .missing: return "Absent"
        case .unknown: return "Inconnu"
        }
    }
}
