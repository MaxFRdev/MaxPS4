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

        // Routing per mode (MaxPS4InputRoute): which sources drive each player.
        typealias Route = MaxPS4InputRoute<String>
        var routing = MaxPS4ControllerSlots<String>(players: 1)

        // Solo: touch and every controller on player 1, no player 2 (unchanged).
        var r = Route.route(mode: .solo, connected: ["A", "B"], slots: &routing)
        check(r.player1.touch && r.player1.controllers == ["A", "B"] && r.player2 == nil, "solo")
        r = Route.route(mode: .solo, connected: [], slots: &routing)
        check(r.player1.touch && r.player1.controllers.isEmpty && r.player2 == nil, "solo without controller")

        // Hybrid: controller 1 only on player 1 (never touch), touch only on player 2.
        r = Route.route(mode: .hybrid, connected: ["A"], slots: &routing)
        check(!r.player1.touch && r.player1.controllers == ["A"], "hybrid player 1 is the controller")
        check(r.player2 == MaxPS4InputSources(touch: true, controllers: []), "hybrid player 2 is touch only")
        // A second controller in hybrid mode drives nobody (never merged).
        r = Route.route(mode: .hybrid, connected: ["A", "B"], slots: &routing)
        check(r.player1.controllers == ["A"] && r.player2?.controllers == [] && r.player2?.touch == true, "hybrid extra controller ignored")
        // No controller: player 1 has no source (idle), player 2 still touch.
        r = Route.route(mode: .hybrid, connected: [], slots: &routing)
        check(r.player1.isEmpty && r.player2?.touch == true, "hybrid without controller")
        // The controller reconnects (new object): player 1 again.
        r = Route.route(mode: .hybrid, connected: ["A2"], slots: &routing)
        check(r.player1.controllers == ["A2"] && !r.player1.touch, "hybrid reconnect")

        // Two controllers: one per player, touch with player 1.
        r = Route.route(mode: .twoControllers, connected: ["A2", "B"], slots: &routing)
        check(r.player1.touch && r.player1.controllers == ["A2"] && r.player2 == MaxPS4InputSources(touch: false, controllers: ["B"]), "two controllers")
        r = Route.route(mode: .twoControllers, connected: ["A2"], slots: &routing)
        check(r.player2 == nil, "two controllers, player 2 gone")

        // Mode changes in a session: two controllers -> hybrid -> solo.
        r = Route.route(mode: .twoControllers, connected: ["A2", "B"], slots: &routing)
        r = Route.route(mode: .hybrid, connected: ["A2", "B"], slots: &routing)
        check(r.player1.controllers.count == 1 && !r.player1.touch && r.player2?.touch == true && r.player2?.controllers == [], "to hybrid")
        r = Route.route(mode: .solo, connected: ["A2", "B"], slots: &routing)
        check(r.player2 == nil && r.player1.touch && r.player1.controllers == ["A2", "B"], "back to solo")

        // No source ever appears on both players, in any mode.
        for mode in MaxPS4PadMode.allCases {
            for connected in [[], ["A"], ["A", "B"], ["A", "B", "C"]] {
                let route = Route.route(mode: mode, connected: connected, slots: &routing)
                let p2 = route.player2 ?? MaxPS4InputSources()
                check(!(route.player1.touch && p2.touch), "touch on one player only (\(mode))")
                check(Set(route.player1.controllers).isDisjoint(with: p2.controllers), "controller on one player only (\(mode))")
            }
        }
        check(MaxPS4PadMode(stored: "unknown") == .solo && MaxPS4PadMode(stored: "hybrid") == .hybrid, "stored mode")

        print("PASS controller slots: none, one, two, order, extra, disconnect, reconnect, solo; routes: solo, hybrid, two controllers, mode changes, no mixing")
    }
}
