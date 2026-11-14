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
