// Local multiplayer: which physical controller drives which player.
//
// Independent of GameController so it can be tested without Bluetooth
// hardware (Tests/ControllerSlotsTest.swift). The game view keys controllers
// by ObjectIdentifier(GCController).
//
// Rules:
//  * a connected controller keeps its player while it stays connected;
//  * a newly seen controller takes the lowest free player (0 = player 1);
//  * a disconnected controller frees its player, so a reconnection (iOS gives
//    a new GCController object) takes that player back when it is still the
//    lowest free one: player 2 reconnecting while player 1 plays is player 2
//    again;
//  * controllers beyond `players` get no player (they are ignored, never
//    mixed into another player's input).
struct MaxPS4ControllerSlots<Key: Hashable> {
    let players: Int
    private(set) var assigned: [Key: Int] = [:]

    init(players: Int = 2) { self.players = max(1, players) }

    // `connected`: the controllers present now, in the order iOS lists them.
    mutating func update(connected: [Key]) {
        let present = Set(connected)
        assigned = assigned.filter { present.contains($0.key) }
        for key in connected where assigned[key] == nil {
            let used = Set(assigned.values)
            guard let free = (0..<players).first(where: { !used.contains($0) }) else { continue }
            assigned[key] = free
        }
    }
    func player(of key: Key) -> Int? { assigned[key] }
    func controller(for player: Int) -> Key? { assigned.first { $0.value == player }?.key }
    func isConnected(player: Int) -> Bool { controller(for: player) != nil }
}

// Which input sources drive each player, per control mode. Pure, so the
// routing is tested without GameController (Tests/ControllerSlotsTest.swift).
enum MaxPS4PadMode: String, CaseIterable {
    case solo           // every controller and the touch controls: player 1
    case twoControllers // controller 1 (+ touch): player 1, controller 2: player 2
    case hybrid         // controller 1: player 1, touch controls: player 2

    // Stored value of the game menu's setting; unknown values are Solo.
    init(stored: String) { self = MaxPS4PadMode(rawValue: stored) ?? .solo }
    // How many controllers get a player of their own.
    var controllerPlayers: Int { self == .twoControllers ? 2 : 1 }
}

struct MaxPS4InputSources<Key: Hashable>: Equatable {
    var touch = false
    var controllers: [Key] = []
    var isEmpty: Bool { !touch && controllers.isEmpty }
}

struct MaxPS4InputRoute<Key: Hashable>: Equatable {
    var player1 = MaxPS4InputSources<Key>()
    // nil: no player 2 (its controller, if it had one, is reported disconnected).
    var player2: MaxPS4InputSources<Key>?

    // `connected`: controllers present now, in iOS order; `slots` keeps their
    // players stable across calls (sized with mode.controllerPlayers).
    static func route(mode: MaxPS4PadMode, connected: [Key], slots: inout MaxPS4ControllerSlots<Key>) -> Self {
        if slots.players != mode.controllerPlayers { slots = MaxPS4ControllerSlots<Key>(players: mode.controllerPlayers) }
        slots.update(connected: connected)
        let first = slots.controller(for: 0).map { [$0] } ?? []
        switch mode {
        case .solo:
            // Unchanged single-player behaviour: everything is player 1.
            return Self(player1: .init(touch: true, controllers: connected), player2: nil)
        case .twoControllers:
            let second = slots.controller(for: 1).map { [$0] } ?? []
            return Self(player1: .init(touch: true, controllers: first),
                        player2: second.isEmpty ? nil : .init(touch: false, controllers: second))
        case .hybrid:
            // Player 2 is the touch screen, always present; player 1 only ever
            // gets the first controller, never a touch.
            return Self(player1: .init(touch: false, controllers: first),
                        player2: .init(touch: true, controllers: []))
        }
    }
}
