# Local multiplayer on one iPhone

No network, server or Internet connection is involved. Players 1 and 2 are PS4
users 1 and 2 on the same device.

## iOS side

The setting is in the game menu: Commandes › "Joueurs", stored per game.
**Solo is the default.**

| Mode | Player 1 | Player 2 |
|---|---|---|
| Solo | touch controls and every controller (as before) | none |
| 2 manettes | first controller and touch controls | second controller |
| Manette + tactile (hybrid) | first controller only | touch controls only |

`MaxPS4InputRoute` (in `MaxPS4ControllerSlots.swift`) decides the sources of
each player. It is pure and independent of GameController:

- each player's state is rebuilt from its own sources on every 120 Hz poll;
  nothing is merged across players;
- `MaxPS4ControllerSlots` keeps the controller assignment stable: a
  reconnected controller takes its player back, and an extra controller drives
  nobody;
- in hybrid mode, the touch controls stay on screen with a controller
  connected; the menu says when no Bluetooth controller is connected;
- a mode change releases every touch button and stick;
- when player 2 loses its source (back to Solo, or its controller
  disconnects), it is reported disconnected. `PS4Pad` then drops its
  unread presses, so no button surfaces on a later reconnection.

An older per-game setting "Multijoueur local (2 manettes)" (from #308)
becomes the "2 manettes" mode.

## PS4 side

`PS4Pad` keeps one slot per local user (1..4):

- each slot has its own handle, buttons, sticks, triggers and connection
  count;
- user 1 is always logged in;
- user N logs in on the first connected report from its controller;
- after a disconnection, the user stays logged in, its pad reads as
  disconnected, and it keeps its handle.

| PS4 call | Behaviour |
|---|---|
| `scePadOpen(user, 0, 0, NULL)` | Handle of that user's slot; `0x809b0081` for a user not logged in |
| `scePadGetHandle(user, 0, 0)` | That user's handle |
| `scePadReadState`, `scePadRead`, `scePadGetControllerInformation`, `scePadClose` | Act on the slot of the handle |
| `sceUserServiceGetLoginUserIdList` | User 1, then every logged-in user |
| `sceUserServiceGetEvent` | One LOGIN per logged-in user |
| `sceUserServiceGetUserName` | `"MaxPS4 <n>"` for users 2..4 |

With one controller, every result is the same as before.

MK Legacy Kollection imports `sceUserServiceGetLoginUserIdList`, `scePadOpen`,
`scePadGetHandle` and `scePadReadState` (it does not import GetEvent).

## Tests

- `PS4PadTest` (also covers the hybrid scenario: simultaneous presses, held
  touch buttons, mode changes without a ghost press):
  - per-user handles;
  - isolated input, including concurrent updates of both players;
  - player 2 disconnects and reconnects with the same handle;
  - stale handles after reset.
- `PS4UserServiceTest`: login list, LOGIN events and names for user 2.
- `ControllerSlotsTest.swift` (`controller-slots.yml`, macOS), without
  Bluetooth hardware: controller assignment, routes of the three modes, mode
  changes, and no source on two players.

## Mortal Kombat Trilogy: how player input reaches the game

Static analysis of the CUSA53178 eboot (addresses are eboot offsets):

1. **Pad manager (0x72a530).** It calls `sceUserServiceGetLoginUserIdList`
   and tracks up to 4 logged-in users. For each new user it calls
   `scePadGetHandle`, then `scePadOpen(user, 0, 0, NULL)` if that fails. It
   closes the pads of users that left the list, and reads every open pad with
   `scePadReadState`. With player 2 logged in (hybrid or two controllers), it
   should therefore open and read user 2's pad.
2. **libretro core.** The eboot calls `retro_set_controller_port_device` for
   port 0 and port 1 (RETRO_DEVICE_JOYPAD).
3. **`input_poll` callback (0x668950).** It builds one 16-bit libretro button
   mask per port 0..3 from four per-player words of the front-end state
   (`[state+0xa8 + 4*port]`, mapped through a 16-entry table at 0x770a20).
4. **`input_state(port, device, index, id)` callback (0x668a70).** It answers
   ports 0..3 for JOYPAD from those masks.

The path therefore distinguishes players end to end. One link is not traced
yet: which opened pad fills which per-player word at `+0xa8`. It could depend
on a "press Start to join" rule in the front-end, or on pad open order. Until
that is traced or seen on a device, player 2's control in MK Trilogy is not
established.

## Not verified yet

Nothing here has run on an iPhone in hybrid or two-controller mode. MK
Trilogy has not been seen to give player 2 its own fighter.

## #309: touch controls sometimes acting on player 1 (fixed)

**Cause.** The touch controls had an immediate path besides the 120 Hz poll.
Every press, release and stick move called `session.input(touchPad)`, which
always updates player 1, at six call sites in `MaxPS4GameView.swift`. In
hybrid mode:

- the touch state reached player 1 until the next poll, and the vblank timer
  (`tick`) re-sent it meanwhile;
- `PS4Pad` reports a press released before any read once, so player 1 still
  saw the touch press after the poll had restored the controller state;
- player 1's held controller buttons were blanked until that poll.

**Fix.** The immediate path goes through the route. `touchChanged()` sends
the touch owner's whole state, touch plus that player's own controllers, to
`MaxPS4InputRoute.touchPlayer` only: player 2 in hybrid mode, player 1
otherwise. It refreshes the route first, so the current mode applies even
before the first poll.

Opening the menu and app interruptions release player 2's buttons too
(`releaseInputs`). Before, a touch held at that moment stayed pressed on
player 2.

**Diagnostics** in the session report, written on changes, never per poll:

- `Joueurs :` the mode, each player's sources, and the number of immediate
  touch updates sent to player 1 and player 2. In hybrid mode, the count for
  player 1 must stay 0.
- `Manettes PS4 :` for each logged-in user, connected or not, the scePad
  handle the game opened for it, the number of game reads, and the current
  buttons (`maxps4_fex_engine_pad_status`). This also shows whether MK Trilogy
  reads user 2's pad at all.

**Tests.**

- `PS4PadTest`:
  - reproduces the leak with the old sequence;
  - checks the fixed sequence over 200 rounds of repeated, rapid and held
    touches with the controller held;
  - checks pad status and stable handles across mode changes.
- `ControllerSlotsTest.swift`:
  - `touchPlayer` for each mode;
  - an event simulation: the old dispatch reproduces the bug, the new one
    never puts touch on player 1 or controller input on player 2, and never
    blanks player 1's controller.
