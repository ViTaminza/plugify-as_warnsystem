# as_warnsystem (Plugify)

Warning system for the admin_system plugin, ported from the Pisex Metamod module '[AS] WarnSystem'. Admins issue warnings with a reason and an expiry ('!warn' / 'mm_warn', reason menus, offline steamid64 targets), the module bans a player automatically when the warning limit is reached, keeps the offline warning list of recently disconnected players, provides '!warns' / 'mm_warns' for the player's own warnings and registers the warning item in the admin menu category from warnsystem.ini. Warnings are stored in the shared Metamod-compatible `as_warnsystem` table (the module opens its own database connection with the settings of admin_system; config warnsystem.ini, phrases from admin_system.phrases.txt). No exported methods.

Plugify port (fork) of **WarnSystem ([AS] WarnSystem)** by ABKAM (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_warnsystem-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_warnsystem`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

