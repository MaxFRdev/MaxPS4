import SwiftUI
import UniformTypeIdentifiers
import UIKit

private enum MaxPS4Tab: Hashable { case home, games, settings }
private struct MaxPS4PlayingFolder: Identifiable { let id = UUID(); let url: URL }

struct MaxPS4HomeView: View {
    @EnvironmentObject private var emulator: MaxPS4Emulator
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject private var folders = MaxPS4GameFolders.shared
    @State private var playingFolder: MaxPS4PlayingFolder?
    @State private var detailsFolder: MaxPS4PlayingFolder?
    @State private var selectedTab: MaxPS4Tab = .home
    @State private var importingGame = false
    @State private var showingGameDirectory = false
    @State private var showingJIT = false
    @State private var gameSearch = ""
    @State private var gameFilter = 0
    @State private var selectedGameDetails: MaxPS4Game?
    @State private var selectedTitleInfo: MaxPS4PKGInspector.TitleInfo?
    @State private var gameToRename: MaxPS4Game?
    @State private var renamedGameTitle = ""
    @State private var gameToDelete: MaxPS4Game?
    @State private var confirmingClear = false
    @State private var pendingDirectory = false
    @State private var message: String?
    @AppStorage(MaxPS4Language.key) private var language = ""
    @AppStorage("maxps4.developer") private var developer = false

    private var filteredGames: [MaxPS4Game] {
        let query = gameSearch.trimmingCharacters(in: .whitespacesAndNewlines)
        let matching = emulator.games.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) }
        return gameFilter == 1 ? matching.sorted { $0.importedAt > $1.importedAt } : matching
    }
    private var filteredFolders: [MaxPS4GameFolders.Folder] {
        let query = gameSearch.trimmingCharacters(in: .whitespacesAndNewlines)
        return folders.folders.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) }
    }
    private var filteredLocal: [MaxPS4GameFolders.LocalGame] {
        let query = gameSearch.trimmingCharacters(in: .whitespacesAndNewlines)
        return folders.local.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) }
    }
    private var libraryEmpty: Bool { emulator.games.isEmpty && folders.folders.isEmpty && folders.local.isEmpty }
    private var libraryCount: Int { folders.local.count + folders.folders.count + emulator.games.count }
    var body: some View {
        ZStack {
            MaxPS4Background()
            Group {
                switch selectedTab {
                case .home: home
                case .games: library
                case .settings: settings
                }
            }
        }
        .preferredColorScheme(.dark)
        .safeAreaInset(edge: .bottom) { tabBar }
        .sheet(isPresented: $showingGameDirectory) { MaxPS4GameDirectoryView() }
        .fullScreenCover(item: $playingFolder) { MaxPS4GameView(directory: $0.url) }
        .sheet(item: $detailsFolder) { folder in
            MaxPS4GameDetailsView(url: folder.url) {
                // Once the sheet is gone: a cover cannot open over it.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { playingFolder = MaxPS4PlayingFolder(url: folder.url) }
            }
        }
        // Back from the Files app: pick up games copied into MaxPS4's folder.
        .onChange(of: scenePhase) { _, phase in if phase == .active { folders.scanDocuments() } }
        .sheet(isPresented: $showingJIT) { MaxPS4JITSetupView() }
        .sheet(item: $selectedGameDetails, onDismiss: {
            selectedTitleInfo = nil
            if pendingDirectory { pendingDirectory = false; showingGameDirectory = true }
        }) { game in
            gameDetails(game)
        }
        .alert("Renommer le jeu", isPresented: Binding(
            get: { gameToRename != nil }, set: { if !$0 { gameToRename = nil } }
        )) {
            TextField("Nom", text: $renamedGameTitle)
            Button("Annuler", role: .cancel) { gameToRename = nil }
            Button("Enregistrer") {
                if let game = gameToRename { emulator.rename(game, to: renamedGameTitle) }
                gameToRename = nil
            }
        }
        .alert("Supprimer ce fichier ?", isPresented: Binding(
            get: { gameToDelete != nil }, set: { if !$0 { gameToDelete = nil } }
        )) {
            Button("Annuler", role: .cancel) { gameToDelete = nil }
            Button("Supprimer", role: .destructive) {
                if let game = gameToDelete { emulator.remove(game) }
                gameToDelete = nil
            }
        } message: { Text("Seule la copie importée dans MaxPS4 sera supprimée.") }
        .confirmationDialog("Vider la bibliothèque ?", isPresented: $confirmingClear, titleVisibility: .visible) {
            Button("Supprimer les fichiers importés", role: .destructive) { emulator.removeAll() }
        } message: { Text("Les copies importées dans MaxPS4 seront supprimées.") }
        .alert("MaxPS4", isPresented: Binding(
            get: { message != nil }, set: { if !$0 { message = nil } }
        )) { Button("Fermer", role: .cancel) { message = nil } } message: { Text(message ?? "") }
        .fileImporter(isPresented: $importingGame, allowedContentTypes: [.data, .item], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                let count = emulator.games.count
                emulator.importGame(from: url)
                if emulator.games.count == count { message = emulator.status }
                else { selectedTab = .games }
            case .failure(let error): message = String(localized: "Import impossible : \(error.localizedDescription)", bundle: .maxps4)
            }
        }
    }

    private var home: some View {
        ScrollView(showsIndicators: false) {
            VStack(alignment: .leading, spacing: 22) {
                HStack {
                    Text(verbatim: "MaxPS4").font(.system(size: 32, weight: .bold, design: .rounded))
                    Spacer()
                    Button { showingJIT = true } label: {
                        Label("JIT", systemImage: "bolt.fill")
                            .font(.subheadline.weight(.semibold))
                            .padding(.horizontal, 14).padding(.vertical, 8)
                            .background(.white.opacity(0.08), in: Capsule())
                    }
                    .tint(.cyan)
                    .accessibilityLabel("Activer le JIT")
                }
                .padding(.top, 8)
                if let recent = folders.recent, FileManager.default.fileExists(atPath: recent.path) {
                    let described = folders.describe(recent)
                    gameRow(described.name, subtitle: "Dernier jeu lancé", icon: described.icon, playLabel: "Reprendre",
                            play: { playingFolder = MaxPS4PlayingFolder(url: recent) }) {
                        detailsFolder = MaxPS4PlayingFolder(url: recent)
                    }
                    .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 18))
                }
                Text("Mes jeux").font(.title3.bold())
                if libraryEmpty {
                    Text("Copie le dossier d’un jeu dans « Sur mon iPhone › MaxPS4 › Jeux » avec l’app Fichiers, ou ajoute-le ici.")
                        .font(.subheadline).foregroundStyle(.white.opacity(0.6))
                } else {
                    VStack(spacing: 0) {
                        ForEach(folders.local) { game in
                            gameRow(game.name, subtitle: "Dossier de jeu", icon: game.icon,
                                    play: { playingFolder = MaxPS4PlayingFolder(url: game.url) }) {
                                detailsFolder = MaxPS4PlayingFolder(url: game.url)
                            }
                            Divider().overlay(.white.opacity(0.08))
                        }
                        ForEach(folders.folders) { folder in
                            gameRow(folder.name, subtitle: "Dossier de jeu", icon: nil, play: { play(folder) }) {
                                if let url = folders.resolve(folder) { detailsFolder = MaxPS4PlayingFolder(url: url) } else { play(folder) }
                            }
                                .contextMenu {
                                    Button(role: .destructive) { folders.remove(folder) } label: {
                                        Label("Retirer de la bibliothèque", systemImage: "minus.circle")
                                    }
                                }
                            Divider().overlay(.white.opacity(0.08))
                        }
                        ForEach(emulator.games) { game in
                            gameRow(game.name, subtitle: "Fichier importé", icon: nil, pkg: game) { selectedGameDetails = game }
                                .contextMenu {
                                    Button { renamedGameTitle = game.name; gameToRename = game } label: { Label("Renommer", systemImage: "pencil") }
                                    Button(role: .destructive) { gameToDelete = game } label: { Label("Supprimer…", systemImage: "trash") }
                                }
                            Divider().overlay(.white.opacity(0.08))
                        }
                    }
                    .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 18))
                }
                Menu {
                    Button { showingGameDirectory = true } label: { Label("Choisir un dossier", systemImage: "folder") }
                    Button { importingGame = true } label: { Label("Importer un fichier", systemImage: "doc") }
                } label: {
                    Label("Ajouter un jeu", systemImage: "plus")
                        .font(.headline).frame(maxWidth: .infinity).padding(.vertical, 14)
                        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 18))
                }
                .tint(.cyan)
                .accessibilityIdentifier("maxps4.openGameDirectory")
                Spacer(minLength: 30)
            }.padding(.horizontal, 20).padding(.top, 12)
        }
    }

    // One game of the home list. A folder plays at once; an imported file
    // (`pkg`) opens its details, since a file alone cannot be launched.
    // One game of the home list: the row opens its sheet; `play` (a folder)
    // adds a Jouer button. An imported file alone cannot be launched.
    private func gameRow(_ name: String, subtitle: LocalizedStringKey, icon: URL?, pkg: MaxPS4Game? = nil,
                         playLabel: LocalizedStringKey = "Jouer", play: (() -> Void)? = nil,
                         open: @escaping () -> Void) -> some View {
        HStack(spacing: 14) {
            Button(action: open) {
                HStack(spacing: 14) {
                    Group {
                        if let icon, let image = UIImage(contentsOfFile: icon.path) {
                            Image(uiImage: image).resizable().scaledToFill()
                        } else if let pkg, pkg.fileName.lowercased().hasSuffix(".pkg") {
                            MaxPS4PKGArtworkView(path: pkg.localPath, entryID: 0x1200).scaledToFill()
                        } else {
                            Image(systemName: pkg == nil ? "folder.fill" : "doc.fill")
                                .font(.title3).foregroundStyle(.cyan)
                                .frame(maxWidth: .infinity, maxHeight: .infinity)
                                .background(.white.opacity(0.08))
                        }
                    }
                    .frame(width: 52, height: 52).clipShape(RoundedRectangle(cornerRadius: 12))
                    VStack(alignment: .leading, spacing: 3) {
                        Text(verbatim: name).font(.headline).lineLimit(1)
                        Text(subtitle).font(.caption).foregroundStyle(.white.opacity(0.55))
                    }
                    Spacer(minLength: 8)
                }.contentShape(Rectangle())
            }.buttonStyle(.plain)
            if let play {
                Button(action: play) {
                    Label(playLabel, systemImage: "play.fill")
                        .font(.subheadline.weight(.semibold))
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(Color.cyan, in: Capsule()).foregroundStyle(.black)
                }.buttonStyle(.plain)
            } else {
                Image(systemName: "chevron.right").foregroundStyle(.white.opacity(0.4))
            }
        }
        .padding(12)
    }

    private var settings: some View {
        ScrollView(showsIndicators: false) {
            VStack(alignment: .leading, spacing: 20) {
                Text("RÉGLAGES").font(.system(size: 36, weight: .black, design: .rounded))
                VStack(alignment: .leading, spacing: 14) {
                    Label("Langue", systemImage: "globe").font(.headline)
                    Picker("Langue", selection: $language) {
                        Text("Langue de l’iPhone").tag("")
                        ForEach(MaxPS4Language.choices, id: \.code) { Text(verbatim: $0.name).tag($0.code) }
                    }
                    .pickerStyle(.menu).tint(.cyan)
                    .onChange(of: language) { _, code in MaxPS4Language.apply(code) }
                }
                .padding(18).frame(maxWidth: .infinity, alignment: .leading)
                .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 22))
                secondaryAction(icon: "bolt.fill", title: "JIT et StikDebug", subtitle: "Activation et état de préparation") {
                    showingJIT = true
                }
                MaxPS4EntitlementsCard()
                VStack(alignment: .leading, spacing: 14) {
                    Label("Stockage", systemImage: "internaldrive").font(.headline)
                    // Game folders (found in Jeux or chosen with Jouer) count too, not only imported files.
                    Text("\(libraryCount) jeu(x) dans la bibliothèque")
                        .foregroundStyle(.white.opacity(0.65))
                    if !emulator.games.isEmpty {
                        Text("dont \(emulator.games.count) fichier(s) importé(s)").font(.footnote).foregroundStyle(.white.opacity(0.5))
                    }
                    Button("Ouvrir la bibliothèque") { selectedTab = .games }.tint(.cyan)
                    Button("Vider la bibliothèque…", role: .destructive) { confirmingClear = true }
                        .disabled(emulator.games.isEmpty)
                }
                .padding(18).frame(maxWidth: .infinity, alignment: .leading)
                .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 22))
                Text("MaxPS4 · \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "") · build \(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "")")
                    .font(.footnote).foregroundStyle(.white.opacity(0.55))
                    // Hidden from players: five taps on the version line turn the
                    // test options and technical reports on or off (for support).
                    .onTapGesture(count: 5) {
                        developer.toggle()
                        message = developer ? String(localized: "Mode développeur activé.", bundle: .maxps4)
                            : String(localized: "Mode développeur désactivé.", bundle: .maxps4)
                    }
                Text("Aucun jeu ni firmware n’est inclus. Utilise uniquement tes fichiers autorisés.")
                    .font(.caption).foregroundStyle(.white.opacity(0.45))
            }.padding(20)
        }
    }

    private var emptyLibrary: some View {
        VStack(spacing: 14) {
            Image(systemName: "gamecontroller").font(.system(size: 36)).foregroundStyle(.cyan)
            Text("Ta bibliothèque commence ici").font(.headline)
            Text("Copie le dossier d’un jeu dans « Sur mon iPhone › MaxPS4 › Jeux » avec l’app Fichiers, lance-le depuis son dossier ou importe un fichier.")
                .font(.subheadline).foregroundStyle(.white.opacity(0.58)).multilineTextAlignment(.center)
            Button("Importer un fichier") { importingGame = true }.buttonStyle(.bordered).tint(.cyan)
        }
        .frame(maxWidth: .infinity).padding(.vertical, 30).padding(.horizontal, 16)
        .background(.white.opacity(0.025), in: RoundedRectangle(cornerRadius: 24))
        .overlay(RoundedRectangle(cornerRadius: 24).stroke(.blue.opacity(0.32), style: StrokeStyle(lineWidth: 1, dash: [7,6])))
    }

    private func gameCard(_ game: MaxPS4Game, fixedWidth: CGFloat?) -> some View {
        let colors: [Color] = [.blue, .indigo, .purple, .cyan]
        let accent = colors[game.name.utf8.reduce(0) { ($0 + Int($1)) % colors.count }]
        let isPKG = game.fileName.lowercased().hasSuffix(".pkg")
        return VStack(alignment: .leading, spacing: 10) {
            Button { selectedGameDetails = game } label: {
                ZStack(alignment: .bottomLeading) {
                    LinearGradient(colors: [accent.opacity(0.95), .black], startPoint: .topLeading, endPoint: .bottomTrailing)
                    if isPKG {
                        MaxPS4PKGArtworkView(path: game.localPath, entryID: 0x1200)
                            .scaledToFill().frame(maxWidth: .infinity, maxHeight: .infinity).clipped()
                    }
                    Circle().fill(.white.opacity(0.10)).frame(width: 120, height: 120).offset(x: 58, y: -78)
                    VStack(alignment: .leading, spacing: 6) {
                        Image(systemName: "gamecontroller.fill").font(.title2).foregroundStyle(.white.opacity(0.78))
                        Spacer()
                        Text(game.name).font(.headline).lineLimit(2).multilineTextAlignment(.leading)
                        Group {
                            if isPKG { MaxPS4PKGSubtitleView(path: game.localPath) }
                            else { Text(game.fileName) }
                        }.font(.caption2).lineLimit(1).foregroundStyle(.white.opacity(0.56))
                    }.padding(14)
                }
                .frame(maxWidth: fixedWidth == nil ? .infinity : nil)
                .frame(width: fixedWidth, height: 220).clipShape(RoundedRectangle(cornerRadius: 20))
                .overlay(RoundedRectangle(cornerRadius: 20).stroke(.white.opacity(0.12), lineWidth: 1))
            }.buttonStyle(.plain)
            HStack {
                Button { showingGameDirectory = true } label: {
                    Label("Jouer", systemImage: "play.fill").font(.caption.weight(.semibold))
                }
                Spacer()
                Menu {
                    Button { renamedGameTitle = game.name; gameToRename = game } label: { Label("Renommer", systemImage: "pencil") }
                    Button(role: .destructive) { gameToDelete = game } label: { Label("Supprimer…", systemImage: "trash") }
                } label: { Image(systemName: "ellipsis.circle").font(.title3) }
            }.foregroundStyle(.white.opacity(0.82))
        }.frame(width: fixedWidth)
    }

    private func folderCard(_ folder: MaxPS4GameFolders.Folder, fixedWidth: CGFloat?) -> some View {
        folderCard(name: folder.name, icon: nil, fixedWidth: fixedWidth, play: { play(folder) },
                   remove: { folders.remove(folder) })
    }

    private func localCard(_ game: MaxPS4GameFolders.LocalGame, fixedWidth: CGFloat?) -> some View {
        folderCard(name: game.name, icon: game.icon, fixedWidth: fixedWidth,
                   play: { playingFolder = MaxPS4PlayingFolder(url: game.url) }, remove: nil)
    }

    // `remove` is nil for a game found in MaxPS4's folder: delete it with Files.
    private func folderCard(name: String, icon: URL?, fixedWidth: CGFloat?, play: @escaping () -> Void,
                            remove: (() -> Void)?) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Button(action: play) {
                ZStack(alignment: .bottomLeading) {
                    LinearGradient(colors: [Color.cyan.opacity(0.85), .black], startPoint: .topLeading, endPoint: .bottomTrailing)
                    if let icon, let image = UIImage(contentsOfFile: icon.path) {
                        Image(uiImage: image).resizable().scaledToFill()
                            .frame(maxWidth: .infinity, maxHeight: .infinity).clipped()
                        LinearGradient(colors: [.clear, .black.opacity(0.8)], startPoint: .center, endPoint: .bottom)
                    } else {
                        Circle().fill(.white.opacity(0.10)).frame(width: 120, height: 120).offset(x: 58, y: -78)
                    }
                    VStack(alignment: .leading, spacing: 6) {
                        Image(systemName: "folder.fill").font(.title2).foregroundStyle(.white.opacity(0.78))
                        Spacer()
                        Text(name).font(.headline).lineLimit(2).multilineTextAlignment(.leading)
                        Text("Dossier de jeu").font(.caption2).lineLimit(1).foregroundStyle(.white.opacity(0.56))
                    }.padding(14)
                }
                .frame(maxWidth: fixedWidth == nil ? .infinity : nil)
                .frame(width: fixedWidth, height: 220).clipShape(RoundedRectangle(cornerRadius: 20))
                .overlay(RoundedRectangle(cornerRadius: 20).stroke(.white.opacity(0.12), lineWidth: 1))
            }.buttonStyle(.plain)
            HStack {
                Button(action: play) {
                    Label("Jouer", systemImage: "play.fill").font(.caption.weight(.semibold))
                }
                Spacer()
                if let remove {
                    Menu {
                        Button(role: .destructive, action: remove) {
                            Label("Retirer de la bibliothèque", systemImage: "minus.circle")
                        }
                    } label: { Image(systemName: "ellipsis.circle").font(.title3) }
                }
            }.foregroundStyle(.white.opacity(0.82))
        }.frame(width: fixedWidth)
    }

    private func play(_ folder: MaxPS4GameFolders.Folder) {
        if let url = folders.resolve(folder) { playingFolder = MaxPS4PlayingFolder(url: url) }
        else { message = String(localized: "Dossier introuvable : il a été déplacé ou supprimé. Choisis-le à nouveau avec Jouer.", bundle: .maxps4) }
    }

    private func gameDetails(_ game: MaxPS4Game) -> some View {
        NavigationStack {
            ZStack {
                MaxPS4Background()
                ScrollView {
                    VStack(alignment: .leading, spacing: 18) {
                        Text(game.name).font(.title2.bold())
                        if game.fileName.lowercased().hasSuffix(".pkg") {
                            MaxPS4PKGArtworkView(path: game.localPath, entryID: 0x1006)
                                .scaledToFit().frame(maxHeight: 200).clipShape(RoundedRectangle(cornerRadius: 20))
                        }
                        primaryAction(icon: "folder.fill", title: "Choisir le dossier du jeu", subtitle: "Le dossier complet est nécessaire au lancement") {
                            pendingDirectory = true; selectedGameDetails = nil
                        }
                        Text(game.fileName).font(.footnote).foregroundStyle(.secondary).textSelection(.enabled)
                        if let info = selectedTitleInfo { MaxPS4TitleInfoView(info: info) }
                        if let attributes = try? FileManager.default.attributesOfItem(atPath: game.localPath),
                           let size = attributes[.size] as? NSNumber {
                            Text("Taille : \(ByteCountFormatter.string(fromByteCount: size.int64Value, countStyle: .file))")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                        if game.fileName.lowercased().hasSuffix(".pkg") {
                            Text("Un fichier PKG seul ne suffit pas au lancement. Sélectionne le dossier du jeu avec eboot.bin et ses données.")
                                .font(.footnote).foregroundStyle(.secondary)
                            if developer {
                                NavigationLink("Extraire un fichier du PKG") { MaxPS4PFSExportView(source: URL(fileURLWithPath: game.localPath)) }
                                    .tint(.cyan)
                            }
                        }
                    }.padding(20).frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .navigationTitle("Fiche du jeu").navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Fermer") { selectedGameDetails = nil } }
            .task(id: game.id) {
                selectedTitleInfo = nil
                guard game.fileName.lowercased().hasSuffix(".pkg") else { return }
                let url = URL(fileURLWithPath: game.localPath)
                let info = await Task.detached(priority: .utility) { MaxPS4PKGInspector.titleInfo(url: url) }.value
                guard !Task.isCancelled, selectedGameDetails?.id == game.id else { return }
                selectedTitleInfo = info
            }
        }.preferredColorScheme(.dark)
    }

    private var tabBar: some View {
        HStack(spacing: 0) {
            tabButton(.home, icon: "house.fill", title: "Accueil")
            tabButton(.games, icon: "square.grid.2x2.fill", title: "Bibliothèque")
            tabButton(.settings, icon: "gearshape.fill", title: "Réglages")
        }
        .padding(.horizontal, 12).padding(.vertical, 11)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 24))
        .overlay(RoundedRectangle(cornerRadius: 24).stroke(.white.opacity(0.10)))
        .padding(.horizontal, 20).padding(.bottom, 8)
    }
    private func tabButton(_ tab: MaxPS4Tab, icon: String, title: LocalizedStringKey) -> some View {
        Button {
            withAnimation(.easeInOut(duration: 0.22)) { selectedTab = tab }
        } label: {
            VStack(spacing: 5) { Image(systemName: icon).font(.system(size: 18, weight: .semibold)); Text(title).font(.caption2.weight(.semibold)) }
                .frame(maxWidth: .infinity).padding(.vertical, 8)
                .foregroundStyle(selectedTab == tab ? Color.cyan : Color.white.opacity(0.52))
                .background(selectedTab == tab ? Color.cyan.opacity(0.12) : Color.clear, in: RoundedRectangle(cornerRadius: 15))
        }.buttonStyle(.plain).accessibilityAddTraits(selectedTab == tab ? .isSelected : [])
    }

    private func primaryAction(
        icon: String,
        title: LocalizedStringKey,
        subtitle: LocalizedStringKey,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: icon)
                    .font(.title2)
                    .frame(width: 52, height: 52)
                    .background(.white.opacity(0.14), in: RoundedRectangle(cornerRadius: 15))

                VStack(alignment: .leading, spacing: 4) {
                    Text(title)
                        .font(.headline)
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(.white.opacity(0.76))
                }

                Spacer()
                Image(systemName: "chevron.right")
                    .font(.headline)
            }
            .foregroundStyle(.white)
            .padding(18)
            .background(
                LinearGradient(
                    colors: [
                        Color(red: 0.02, green: 0.31, blue: 0.95),
                        Color(red: 0.0, green: 0.56, blue: 1.0)
                    ],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                ),
                in: RoundedRectangle(cornerRadius: 24)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 24)
                    .stroke(.cyan.opacity(0.60), lineWidth: 1)
            )
            .shadow(color: .blue.opacity(0.28), radius: 18, y: 6)
        }
        .buttonStyle(.plain)
    }

    private func secondaryAction(
        icon: String,
        title: LocalizedStringKey,
        subtitle: LocalizedStringKey,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: icon)
                    .font(.title2)
                    .frame(width: 52, height: 52)
                    .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 15))

                VStack(alignment: .leading, spacing: 4) {
                    Text(title)
                        .font(.headline)
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(.white.opacity(0.60))
                }

                Spacer()
                Image(systemName: "chevron.right")
            }
            .foregroundStyle(.white)
            .padding(18)
            .background(.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 24))
            .overlay(
                RoundedRectangle(cornerRadius: 24)
                    .stroke(.blue.opacity(0.22), lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
    }

    private var library: some View {
        ScrollView(showsIndicators: false) {
            VStack(alignment: .leading, spacing: 18) {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("BIBLIOTHÈQUE")
                            .font(.system(size: 36, weight: .black, design: .rounded))

                        Text("Tes dossiers de jeu et fichiers importés")
                            .foregroundStyle(.white.opacity(0.58))
                    }

                    Spacer()

                    Button {
                        importingGame = true
                    } label: {
                        Image(systemName: "plus")
                            .font(.headline)
                            .frame(width: 44, height: 44)
                            .background(.blue)
                            .clipShape(Circle())
                            .foregroundStyle(.white)
                    }.accessibilityLabel("Importer un fichier")
                }

                secondaryAction(icon: "folder.fill", title: "Jouer depuis un dossier",
                    subtitle: "Ouvrir un jeu avec ses données") { showingGameDirectory = true }

                HStack(alignment: .firstTextBaseline) {
                    Text("Jeux trouvés dans « Sur mon iPhone › MaxPS4 » : \(folders.local.count) (\(folders.searched) dossiers parcourus). Un jeu est un dossier qui contient eboot.bin.")
                        .font(.caption).foregroundStyle(.white.opacity(0.55))
                    Spacer()
                    Button("Actualiser") { folders.scanDocuments() }.font(.caption.weight(.semibold)).tint(.cyan)
                }

                HStack {
                    Image(systemName: "magnifyingglass")
                    TextField("Rechercher un jeu", text: $gameSearch)
                        .autocorrectionDisabled()
                }
                .padding(14)
                .background(.white.opacity(0.07), in: RoundedRectangle(cornerRadius: 16))
                Picker("Tri", selection: $gameFilter) {
                    Text("Tous").tag(0)
                    Text("Récents").tag(1)
                }
                .pickerStyle(.segmented)
                .tint(.cyan)

                if !filteredFolders.isEmpty || !filteredLocal.isEmpty {
                    LazyVGrid(columns: [GridItem(.flexible(), spacing: 14), GridItem(.flexible(), spacing: 14)], spacing: 18) {
                        ForEach(filteredLocal) { game in localCard(game, fixedWidth: nil) }
                        ForEach(filteredFolders) { folder in folderCard(folder, fixedWidth: nil) }
                    }
                }
                if libraryEmpty {
                    emptyLibrary.padding(.top, 24)
                } else if emulator.games.isEmpty {
                    EmptyView()
                } else if filteredGames.isEmpty {
                    Text("Aucun jeu correspondant")
                        .foregroundStyle(.white.opacity(0.65))
                        .padding(.vertical, 24)
                } else {
                    LazyVGrid(
                        columns: [
                            GridItem(.flexible(), spacing: 14),
                            GridItem(.flexible(), spacing: 14)
                        ],
                        spacing: 18
                    ) {
                        ForEach(filteredGames) { game in
                            gameCard(game, fixedWidth: nil)
                        }
                    }
                }

                Spacer(minLength: 90)
            }
            .padding(.horizontal, 20)
            .padding(.top, 12)
        }
    }

}

struct MaxPS4Background: View {
    var body: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.012, green: 0.025, blue: 0.07),
                    Color(red: 0.055, green: 0.04, blue: 0.12),
                    .black
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            Circle()
                .fill(.purple.opacity(0.18))
                .frame(width: 360, height: 360)
                .blur(radius: 80)
                .offset(x: 180, y: -300)

            Circle()
                .fill(.cyan.opacity(0.08))
                .frame(width: 260, height: 260)
                .blur(radius: 70)
                .offset(x: -180, y: 280)
        }
    }
}

/// Card subtitle such as "CUSA12345 • v01.23 • Jeu", read once per file and cached
/// for the session so scrolling the library never rereads the PKG.
private struct MaxPS4PKGSubtitleView: View {
    let path: String
    private static let cache = NSCache<NSString, NSString>()
    @State private var subtitle: String?

    var body: some View {
        Text(subtitle ?? String(localized: "PKG PS4", bundle: .maxps4))
            .task(id: path) {
                let url = URL(fileURLWithPath: path)
                let text = await Task.detached(priority: .utility) { () -> String in
                    // A file replaced under the same name gets a new modification date.
                    let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
                        .contentModificationDate?.timeIntervalSince1970 ?? 0
                    let key = "\(url.path)|\(modified)" as NSString
                    if let cached = Self.cache.object(forKey: key) { return cached as String }
                    let info = MaxPS4PKGInspector.titleInfo(url: url)
                    let parts = [info?.titleID, info?.appVersion.map { "v" + $0 }, info?.categoryName].compactMap { $0 }
                    let text = parts.isEmpty ? String(localized: "PKG PS4", bundle: .maxps4) : parts.joined(separator: " • ")
                    Self.cache.setObject(text as NSString, forKey: key)
                    return text
                }.value
                subtitle = text
            }
    }
}

private struct MaxPS4TitleInfoView: View {
    let info: MaxPS4PKGInspector.TitleInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Informations du jeu (param.sfo)")
                .font(.headline)
            row("Titre", info.title)
            row("Identifiant", info.titleID)
            row("Catégorie", info.categoryName)
            row("Version de l’application", info.appVersion)
            row("Version du contenu", info.version)
            row("Content ID", info.contentID)
            Text("Lu depuis le PKG ; authenticité non vérifiée.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 14))
    }

    @ViewBuilder
    private func row(_ label: LocalizedStringKey, _ value: String?) -> some View {
        if let value {
            HStack(alignment: .firstTextBaseline) {
                Text(label).foregroundStyle(.secondary)
                Spacer()
                Text(value).multilineTextAlignment(.trailing).textSelection(.enabled)
            }
            .font(.subheadline)
        }
    }
}

private struct MaxPS4PKGArtworkView: View {
    let path: String
    let entryID: UInt32
    @State private var artwork: UIImage?

    var body: some View {
        Group {
            if let artwork {
                Image(uiImage: artwork).resizable()
            } else {
                Color.clear
            }
        }
        .task(id: path + ":" + String(entryID)) {
            guard let data = MaxPS4PKGInspector.artwork(url: URL(fileURLWithPath: path), entryID: entryID),
                  let image = UIImage(data: data) else {
                artwork = nil
                return
            }
            artwork = image
        }
    }
}
