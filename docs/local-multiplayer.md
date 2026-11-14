# Local multiplayer (two controllers on one iPhone)

No network, server or Internet connection is involved. Two Bluetooth
controllers drive PS4 users 1 and 2 on the same device.

## iOS side

The setting is in the game menu: Commandes › "Multijoueur local (2 manettes)",
stored per game and **off by default**.

- **Off:** every controller and the touch controls drive player 1, exactly as
  before.
- **On:** `MaxPS4ControllerSlots` assigns controllers to players, and
  `MaxPS4ControllerRouter` in `MaxPS4GameView.swift` applies it:
  - a connected controller keeps its player while it stays connected;
  - a new controller takes the lowest free player;
  - a controller that reconnects after a disconnection takes that player back;
  - a third controller is ignored;
  - the controller's player light is set (`playerIndex`).
- Player 1 gets the first controller plus the touch controls. Player 2 gets
  the second controller, sent with `maxps4_fex_engine_pad_update_player(…, 1, …)`.
- The menu shows each player's controller, or "aucune manette".

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

- `PS4PadTest`:
  - per-user handles;
  - isolated input, including concurrent updates of both players;
  - player 2 disconnects and reconnects with the same handle;
  - stale handles after reset.
- `PS4UserServiceTest`: login list, LOGIN events and names for user 2.
- `ControllerSlotsTest.swift` (`controller-slots.yml`, macOS): controller
  assignment without Bluetooth hardware.

## Not verified yet

Nothing here has run on an iPhone with two controllers. No game has been
checked to actually open user 2's pad, including MK Trilogy through
libretro/pancake.
