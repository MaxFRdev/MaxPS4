import SwiftUI
import UniformTypeIdentifiers

struct MaxPS4GameDirectoryView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var playing = false
    @State private var showingJIT = false
    @State private var directory: URL?
    @State private var importError: String?

    var body: some View {
        NavigationStack {
            ZStack {
                MaxPS4Background()
                VStack(alignment: .leading, spacing: 24) {
                    Image(systemName: "gamecontroller.fill").font(.system(size: 44)).foregroundStyle(.cyan)
                    Text("À toi de jouer").font(.system(size: 32, weight: .black, design: .rounded))
                    Text("Choisis le dossier complet du jeu, avec eboot.bin, sce_module et ses données.")
                        .foregroundStyle(.white.opacity(0.65))
                    if let directory {
                        Label(directory.lastPathComponent, systemImage: "folder.fill")
                            .font(.headline).foregroundStyle(.cyan)
                            .padding(18).frame(maxWidth: .infinity, alignment: .leading)
                            .background(.white.opacity(0.06), in: RoundedRectangle(cornerRadius: 20))
                        Button { playing = true } label: {
                            Label("Continuer", systemImage: "play.fill").frame(maxWidth: .infinity).padding(.vertical, 8)
                        }.buttonStyle(.borderedProminent).tint(.blue)
                    }
                    Button(directory == nil ? "Choisir le dossier du jeu" : "Changer de dossier") { importing = true }
                        .buttonStyle(.bordered).tint(.cyan)
                    if let importError { Text(importError).font(.footnote).foregroundStyle(.orange) }
                    Spacer(minLength: 20)
                    Button { showingJIT = true } label: { Label("Activer le JIT avec StikDebug", systemImage: "bolt.fill") }
                        .font(.footnote).tint(.cyan)
                }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
            }
            .navigationTitle("Jouer").navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Fermer") { dismiss() } }
        }
        .preferredColorScheme(.dark)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url): directory = url; importError = nil; MaxPS4GameFolders.shared.add(url)
            case .failure(let error): importError = String(localized: "Sélection impossible : \(error.localizedDescription)", bundle: .maxps4)
            }
        }
        .sheet(isPresented: $showingJIT) { MaxPS4JITSetupView() }
        .fullScreenCover(isPresented: $playing) {
            if let directory { MaxPS4GameView(directory: directory) }
        }
    }
}

// Game folders chosen with "Jouer", kept in the library as bookmarks so they
// can be launched again without picking the folder each time, and game
// folders put in MaxPS4's own folder with the Files app, found by scanning.
@MainActor final class MaxPS4GameFolders: ObservableObject {
    struct Folder: Codable, Identifiable, Equatable {
        var id = UUID()
        var name: String
        var bookmark: Data
        var addedAt = Date()
    }
    // A folder with an eboot.bin under Documents ("Sur mon iPhone › MaxPS4").
    struct LocalGame: Identifiable, Equatable {
        var id: String { url.path }
        let url: URL
        let name: String
        let icon: URL?
        // An extracted update found for it (shadPS4 layout: "<game>-UPDATE").
        var update: URL? = nil
        var updateVersion: String? = nil
        var addons: [Addon] = []
    }
    // An extracted add-on content (param.sfo category "ac") of a game: its
    // entitlement label is the last 16 characters of its CONTENT_ID.
    struct Addon: Equatable {
        let url: URL
        let label: String
        let name: String
    }
    static let shared = MaxPS4GameFolders()
    @Published private(set) var folders: [Folder] = []
    @Published private(set) var local: [LocalGame] = []
    @Published private(set) var searched = 0 // Folders walked by the last scan.
    @Published private(set) var recent: URL? // The last game launched (home's Reprendre).
    private let recentKey = "maxps4.library.recent"
    private let key = "maxps4.library.folders"
    private var watchers: [DispatchSourceFileSystemObject] = []
    private var rescanPending = false

    private init() {
        if let data = UserDefaults.standard.data(forKey: key),
           let saved = try? JSONDecoder().decode([Folder].self, from: data) { folders = saved }
        if let data = UserDefaults.standard.data(forKey: recentKey) {
            var stale = false
            recent = try? URL(resolvingBookmarkData: data, bookmarkDataIsStale: &stale)
        }
        scanDocuments()
    }

    // Looks up to six levels below Documents (its "Jeux" folder is made for
    // this) for a folder holding eboot.bin in any letter case, and watches the
    // first folders it walked, so a game copied in with the Files app shows up
    // without restarting MaxPS4.
    func scanDocuments() {
        let manager = FileManager.default
        let documents = manager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        try? manager.createDirectory(at: documents.appendingPathComponent("Jeux", isDirectory: true),
                                     withIntermediateDirectories: true)
        var games: [LocalGame] = [], walked: [URL] = []
        // Updates by TITLE_ID and by the game folder name they are named after.
        var updates: [(url: URL, titleID: String?, base: String, version: String?)] = []
        var addons: [(titleID: String, addon: Addon)] = []
        func named(_ name: String, in entries: [URL]) -> URL? {
            entries.first { $0.lastPathComponent.lowercased() == name }
        }
        func visit(_ folder: URL, depth: Int) {
            walked.append(folder)
            let entries = ((try? manager.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey],
                                                             options: [.skipsHiddenFiles])) ?? [])
                .sorted { $0.lastPathComponent < $1.lastPathComponent }
            // An add-on content folder (no eboot.bin): attached to its game below.
            if depth > 0, let system = named("sce_sys", in: entries),
               let sfo = named("param.sfo", in: (try? manager.contentsOfDirectory(at: system, includingPropertiesForKeys: nil)) ?? []),
               Self.sfoString(sfo, key: "CATEGORY")?.lowercased() == "ac",
               let content = Self.sfoString(sfo, key: "CONTENT_ID"), content.count >= 16 {
                let titleID = Self.sfoString(sfo, key: "TITLE_ID")
                    ?? String(content.dropFirst(7).prefix(9)) // UP0177-CUSA07010_00-…
                addons.append((titleID, Addon(url: folder, label: String(content.suffix(16)),
                                              name: Self.title(sfo) ?? folder.lastPathComponent)))
                return
            }
            if depth > 0, named("eboot.bin", in: entries) != nil {
                let system = named("sce_sys", in: entries)
                let files = system.flatMap { try? manager.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil) } ?? []
                let sfo = named("param.sfo", in: files)
                func field(_ key: String) -> String? { sfo.flatMap { Self.sfoString($0, key: key) } }
                // A patch (category gp) or a folder named "<game>-UPDATE" / "-PATCH" is
                // an update of a game, not a game of its own.
                let upper = folder.lastPathComponent.uppercased()
                let suffix = ["-UPDATE", "-PATCH"].first { upper.hasSuffix($0) }
                if field("CATEGORY")?.lowercased().hasPrefix("gp") == true || suffix != nil {
                    let base = suffix.map { String(folder.lastPathComponent.dropLast($0.count)) } ?? folder.lastPathComponent
                    updates.append((folder, field("TITLE_ID"), base.uppercased(), field("APP_VER")))
                    return
                }
                games.append(LocalGame(url: folder,
                    name: sfo.flatMap(Self.title) ?? folder.lastPathComponent,
                    icon: named("icon0.png", in: files)))
                return
            }
            guard depth < 6 else { return }
            for entry in entries where walked.count < 400
                && (try? entry.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true
                && !["Fichiers importés", "MaxPS4Games", "Sauvegardes", "Rapports"].contains(entry.lastPathComponent) {
                visit(entry, depth: depth + 1)
            }
        }
        visit(documents, depth: 0)
        for index in games.indices {
            let game = games[index]
            let id = Self.sfoString(game.url.appendingPathComponent("sce_sys/param.sfo"), key: "TITLE_ID")
            let match = updates.first { ($0.titleID != nil && $0.titleID == id) || $0.base == game.url.lastPathComponent.uppercased() }
            games[index].update = match?.url
            games[index].updateVersion = match?.version
            games[index].addons = addons.filter { $0.titleID == id }.map(\.addon)
        }
        if games != local { local = games }
        if walked.count != searched { searched = walked.count }
        watch(walked.prefix(64))
    }
    private func watch(_ folders: ArraySlice<URL>) {
        watchers.forEach { $0.cancel() }
        watchers = folders.compactMap { folder in
            let descriptor = open(folder.path, O_EVTONLY)
            guard descriptor >= 0 else { return nil }
            let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: descriptor,
                                                                   eventMask: [.write, .rename, .delete], queue: .main)
            source.setEventHandler { MainActor.assumeIsolated { MaxPS4GameFolders.shared.scheduleRescan() } }
            source.setCancelHandler { close(descriptor) }
            source.resume()
            return source
        }
    }
    // A copy writes many entries: rescan once it settles.
    private func scheduleRescan() {
        guard !rescanPending else { return }
        rescanPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            MainActor.assumeIsolated {
                MaxPS4GameFolders.shared.rescanPending = false
                MaxPS4GameFolders.shared.scanDocuments()
            }
        }
    }
    private static func title(_ url: URL) -> String? { sfoString(url, key: "TITLE") }
    // A string value of a PS4 param.sfo (PSF: key and data tables, 16-byte entries).
    nonisolated static func sfoString(_ url: URL, key wanted: String) -> String? {
        guard let data = try? Data(contentsOf: url), data.count >= 20,
              data.prefix(4) == Data([0, 0x50, 0x53, 0x46]) else { return nil }
        let bytes = [UInt8](data)
        func u32(_ at: Int) -> Int { at + 4 <= bytes.count ? Int(bytes[at]) | Int(bytes[at+1]) << 8 | Int(bytes[at+2]) << 16 | Int(bytes[at+3]) << 24 : 0 }
        func u16(_ at: Int) -> Int { at + 2 <= bytes.count ? Int(bytes[at]) | Int(bytes[at+1]) << 8 : 0 }
        func string(_ at: Int, _ limit: Int) -> String? {
            guard at >= 0, at < bytes.count else { return nil }
            let end = bytes[at..<min(bytes.count, at + limit)].firstIndex(of: 0) ?? min(bytes.count, at + limit)
            return String(bytes: bytes[at..<end], encoding: .utf8)
        }
        let keys = u32(8), values = u32(12), count = min(u32(16), 256)
        for index in 0..<count {
            let entry = 20 + index * 16
            guard string(keys + u16(entry), 64) == wanted else { continue }
            let title = string(values + u32(entry + 12), u32(entry + 4))?.trimmingCharacters(in: .whitespacesAndNewlines)
            return title?.isEmpty == false ? title : nil
        }
        return nil
    }
    func add(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let bookmark = try? url.bookmarkData() else { return }
        // The same folder chosen again moves to the front instead of repeating.
        let target = url.standardizedFileURL
        folders.removeAll { Self.location($0)?.standardizedFileURL == target }
        folders.insert(Folder(name: url.lastPathComponent, bookmark: bookmark), at: 0)
        save()
    }
    // The folder's current URL, or nil once it was moved or deleted.
    func resolve(_ folder: Folder) -> URL? {
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: folder.bookmark, bookmarkDataIsStale: &stale) else { return nil }
        if stale, let index = folders.firstIndex(where: { $0.id == folder.id }) {
            let scoped = url.startAccessingSecurityScopedResource()
            if let fresh = try? url.bookmarkData() { folders[index].bookmark = fresh; save() }
            if scoped { url.stopAccessingSecurityScopedResource() }
        }
        return url
    }
    private static func location(_ folder: Folder) -> URL? {
        var stale = false
        return try? URL(resolvingBookmarkData: folder.bookmark, bookmarkDataIsStale: &stale)
    }
    // The update found for a game folder, if any (from the last scan).
    func update(for game: URL) -> (url: URL, version: String?)? {
        guard let found = local.first(where: { $0.url.standardizedFileURL == game.standardizedFileURL }),
              let update = found.update else { return nil }
        return (update, found.updateVersion)
    }
    // The add-on contents found for a game folder (from the last scan).
    func addons(for game: URL) -> [Addon] {
        local.first(where: { $0.url.standardizedFileURL == game.standardizedFileURL })?.addons ?? []
    }
    // Whether a game launches with its update ("Utiliser la mise à jour", on by default).
    static func useUpdate(for game: URL) -> Bool {
        UserDefaults.standard.object(forKey: "maxps4.update." + MaxPS4Saves.gameID(for: game)) as? Bool ?? true
    }
    func remember(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        if let data = try? url.bookmarkData() { UserDefaults.standard.set(data, forKey: recentKey) }
        recent = url
    }
    // The recent game's name and icon: from the scanned list, else its param.sfo.
    func describe(_ url: URL) -> (name: String, icon: URL?) {
        if let game = local.first(where: { $0.url.standardizedFileURL == url.standardizedFileURL }) { return (game.name, game.icon) }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        return (Self.sfoString(url.appendingPathComponent("sce_sys/param.sfo"), key: "TITLE") ?? url.lastPathComponent, nil)
    }
    func remove(_ folder: Folder) {
        folders.removeAll { $0.id == folder.id }
        save()
    }
    private func save() {
        if let data = try? JSONEncoder().encode(folders) { UserDefaults.standard.set(data, forKey: key) }
    }
}

// Each game's save data: Documents/Sauvegardes/<TITLE_ID>, outside the game
// folder so replacing or copying the game again keeps them.
enum MaxPS4Saves {
    // The game's TITLE_ID (CUSA07010), else its folder name, safe as a path
    // component; also the key of its own settings.
    static func gameID(for game: URL) -> String {
        let system = game.appendingPathComponent("sce_sys/param.sfo")
        let name = MaxPS4GameFolders.sfoString(system, key: "TITLE_ID") ?? game.lastPathComponent
        let safe = String(name.unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) || $0 == "-" || $0 == "_" })
        return safe.isEmpty ? "jeu" : safe
    }
    static func root(for game: URL) -> URL? {
        let name = gameID(for: game)
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Sauvegardes", isDirectory: true).appendingPathComponent(name, isDirectory: true)
        do { try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true) } catch { return nil }
        return url
    }
}

// The report of every run, kept in Documents/Rapports ("Sur mon iPhone ›
// MaxPS4 › Rapports") so a blocked game's can be sent later.
enum MaxPS4Reports {
    private static var folder: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Rapports", isDirectory: true)
    }
    // The running session's report, rewritten every second: what is left
    // when the app itself crashes.
    static func live(for game: URL) -> URL {
        folder.appendingPathComponent("\(MaxPS4Saves.gameID(for: game))-en-cours.txt")
    }
    // Written by the engine when a host fault ends the app (see maxps4_fex_set_host_crash_note).
    static func hostCrash(for game: URL) -> URL {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.appendingPathComponent("\(MaxPS4Saves.gameID(for: game))-plantage-app.txt")
    }
    static func write(_ text: String, to file: URL) {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? text.write(to: file, atomically: true, encoding: .utf8)
    }
    static func save(_ text: String, for game: URL) -> URL? {
        let stamp = DateFormatter(); stamp.dateFormat = "yyyyMMdd-HHmmss"; stamp.locale = Locale(identifier: "en_US_POSIX")
        let file = folder.appendingPathComponent("\(MaxPS4Saves.gameID(for: game))-\(stamp.string(from: Date())).txt")
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"
        let header = "MaxPS4 build \(version) · \(game.lastPathComponent) · \(Date())\n\n"
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try (header + text).write(to: file, atomically: true, encoding: .utf8)
            return file
        } catch { return nil }
    }
}
