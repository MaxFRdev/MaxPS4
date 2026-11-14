// MaxPS4ControllerSlots: local multiplayer controller assignment, without
// GameController or Bluetooth hardware. Keys stand for controller objects.
// swiftc -parse-as-library MaxPS4-iOS/Sources/MaxPS4ControllerSlots.swift MaxPS4-iOS/Tests/ControllerSlotsTest.swift
import Foundation

@main
struct ControllerSlotsTest {
    static func check(_ condition: Bool, _ message: String, line: Int = #line) {
        if !condition { print("FAIL line \(line): \(message)"); exit(1) }
    }
    static func main() {
        var slots = MaxPS4ControllerSlots<String>(players: 2)

        // No controller: nobody connected (touch controls stay player 1's).
        slots.update(connected: [])
        check(!slots.isConnected(player: 0) && !slots.isConnected(player: 1), "no controller")

        // One controller: player 1 only (single-player games unchanged).
        slots.update(connected: ["A"])
        check(slots.player(of: "A") == 0 && slots.controller(for: 0) == "A" && !slots.isConnected(player: 1), "one controller")

        // Two controllers: A stays player 1, B becomes player 2.
        slots.update(connected: ["A", "B"])
        check(slots.player(of: "A") == 0 && slots.player(of: "B") == 1, "two controllers")
        // iOS may list them in another order: assignments do not move.
        slots.update(connected: ["B", "A"])
        check(slots.player(of: "A") == 0 && slots.player(of: "B") == 1, "stable across list order")

        // A third controller gets no player and never shares one.
        slots.update(connected: ["B", "C", "A"])
        check(slots.player(of: "C") == nil && slots.controller(for: 0) == "A" && slots.controller(for: 1) == "B", "third ignored")

        // Player 2 disconnects: player 1 is untouched, player 2 is free...
        slots.update(connected: ["A"])
        check(slots.player(of: "A") == 0 && !slots.isConnected(player: 1) && slots.player(of: "B") == nil, "player 2 gone")
        // ...and its reconnection (a new object for iOS) is player 2 again.
        slots.update(connected: ["A", "B2"])
        check(slots.player(of: "B2") == 1 && slots.player(of: "A") == 0, "player 2 reconnects as player 2")

        // Player 1 disconnects while player 2 plays: player 2 keeps its slot;
        // the next controller takes the free player 1 slot.
        slots.update(connected: ["B2"])
        check(slots.player(of: "B2") == 1 && !slots.isConnected(player: 0), "player 2 keeps its slot")
        slots.update(connected: ["B2", "A2"])
        check(slots.player(of: "A2") == 0 && slots.player(of: "B2") == 1, "new controller fills player 1")

        // Everything disconnected, then one controller: player 1.
        slots.update(connected: [])
        check(slots.assigned.isEmpty, "all gone")
        slots.update(connected: ["Z"])
        check(slots.player(of: "Z") == 0, "fresh start is player 1")

        // One-player configuration: a second controller gets no player.
        var solo = MaxPS4ControllerSlots<String>(players: 1)
        solo.update(connected: ["A", "B"])
        check(solo.player(of: "A") == 0 && solo.player(of: "B") == nil, "one player")

        print("PASS controller slots: none, one, two, order, extra, disconnect, reconnect, solo")
    }
}
