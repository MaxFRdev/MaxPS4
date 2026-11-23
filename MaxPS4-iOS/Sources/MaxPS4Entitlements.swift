import Foundation
import os

// The entitlements the running process really holds, read from its own
// signature (SecTaskCopyValueForEntitlement, looked up at run time: the iOS
// SDK does not declare it). After SideStore re-signing, or inside
// LiveContainer (whose own entitlements apply), they can differ from
// MaxPS4.entitlements.
enum MaxPS4Entitlements {
    enum State { case granted, missing, unknown }
    struct Item: Identifiable {
        let id: String
        let title: String
        let detail: String
        let state: State
    }

    private typealias CreateFromSelf = @convention(c) (CFAllocator?) -> Unmanaged<AnyObject>?
    private typealias CopyValue = @convention(c) (AnyObject, CFString, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> Unmanaged<AnyObject>?

    static func state(_ key: String) -> State {
        guard let security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW),
              let createSymbol = dlsym(security, "SecTaskCreateFromSelf"),
              let copySymbol = dlsym(security, "SecTaskCopyValueForEntitlement") else { return .unknown }
        let create = unsafeBitCast(createSymbol, to: CreateFromSelf.self)
        let copy = unsafeBitCast(copySymbol, to: CopyValue.self)
        guard let task = create(nil)?.takeRetainedValue() else { return .unknown }
        guard let value = copy(task, key as CFString, nil)?.takeRetainedValue() else { return .missing }
        return (value as? Bool) == true || (value as? NSNumber)?.boolValue == true ? .granted : .missing
    }

    static func current() -> [Item] {
        [
            Item(id: "get-task-allow",
                 title: String(localized: "Débogage (JIT)", bundle: .maxps4),
                 detail: String(localized: "Permet à StikDebug d’activer le JIT.", bundle: .maxps4),
                 state: state("get-task-allow")),
            Item(id: "com.apple.developer.kernel.extended-virtual-addressing",
                 title: String(localized: "Adressage mémoire étendu", bundle: .maxps4),
                 detail: String(localized: "Plus d’espace pour la mémoire de la PS4.", bundle: .maxps4),
                 state: state("com.apple.developer.kernel.extended-virtual-addressing")),
            Item(id: "com.apple.developer.kernel.increased-memory-limit",
                 title: String(localized: "Mémoire augmentée", bundle: .maxps4),
                 detail: String(localized: "iOS laisse le jeu utiliser plus de RAM.", bundle: .maxps4),
                 state: state("com.apple.developer.kernel.increased-memory-limit")),
        ]
    }

    // RAM the app can still use before iOS closes it (0 when unknown).
    static func availableMemory() -> UInt64 { UInt64(os_proc_available_memory()) }

    // Memory in use and the iOS limit (use plus what iOS still allows), in MiB,
    // for the game report.
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
