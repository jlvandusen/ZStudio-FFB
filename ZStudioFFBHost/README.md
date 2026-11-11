# ZStudioFFBHost

A standalone process that plays force feedback for Z Studios games on wheels, FFB joysticks and pads, through SDL3. The
game writes its feedback events to a file (the ZStudio Output Protocol, `../ZSTUDIO.md`). This host tails that file and
turns each event into a force.

It is a separate process because this repository is GPL-3 and the games are not. A game never links this code.
ZSTUDIO.md, "Licence boundary", explains why.

## Use

```
ZStudioFFBHost.exe --list        which devices SDL sees and what effects each supports
ZStudioFFBHost.exe --test        left jolt, right jolt, sine roll, rumble on the chosen device
ZStudioFFBHost.exe               run: attach to the first stream that exists and play its events
ZStudioFFBHost.exe --dry-run     same, but only print what each event would play
ZStudioFFBHost.exe --selftest    parser and mapping checks without a device (exit 1 on failure)
  --ini <file>  --stream <file>  overrides
```

Start it before or after the game. Every 2 s it attaches to the **most recently written** candidate stream. A PC with
the Steam game and a dev checkout has two streams, and the host follows whichever one a match is writing.

## How an event becomes a force

1. **Parse.** A v1 line carries what the game itself played: `rumble`, camera `shake`, `trauma` (0..1), plus `id`,
   `scale` and the position.
2. **Strength.** It starts at `max(rumble × RumbleWeight, shake × ShakeWeight, trauma × TraumaWeight)`.
   - A per-id `Force=` replaces it, scaled by the event's distance `scale`.
   - `Gain=` multiplies it.
   - Below `Floor` the event plays nothing.
3. **Type.** The per-id `Type=` decides it. Without a rule:
   - an event the camera felt (shake or trauma) is a **jolt**;
   - a pure rumble stays a **rumble**.

   A trailing `*` in a section name matches a prefix (`[fb_hazard_*]`). An exact name beats a prefix.
4. **Tuning, the upstream FFB Arcade Plugin model.**
   - `PowerMode=1` takes the square root, which lifts weak events.
   - The result is mapped into `MinForce..MaxForce` percent.
   - `Reverse` swaps jolt sides.
5. **Play.** What plays depends on the device:
   - **jolt:** a constant force with an instant attack and a two-thirds fade. It alternates left and right, so a barrage
     feels like a shudder rather than a pull.
   - **sine:** the rolling effects (quake, tsunami, thunder roll).
   - **rumble:** a rumble.

   A device that lacks an effect falls back down that chain. A pad with no haptic interface gets
   `SDL_RumbleGamepad`, where a jolt uses the heavy motor and a rumble the light one.

   A weaker event does not cut a running one short unless it is 1.5× stronger. That rule is upstream's
   `TriggerSineEffect`.

## Streams

The game truncates its stream when a match starts and appends one line per event. The host:
- attaches at the end, so old events are history and do not replay;
- starts over from the top when the file shrinks;
- keeps a partial line until it is finished.

The handle is opened share-read/write/delete, so the game can always truncate and write.

DEFY's stream:
- in the Steam game: `<library>\steamapps\common\DEFY\artifacts\feeds\feedback.ndjson`, because the depot ships
  `data\` and the game writes beside it. The INI line `SteamGame1=DEFY|artifacts\feeds\feedback.ndjson` is tried in every
  Steam library. The host reads the Steam path from `HKCU\Software\Valve\Steam` and the libraries from
  `steamapps\libraryfolders.vdf`, so a game on any drive is found;
- only if the game finds no `data\` beside it: `Saved\Defy\feedback.ndjson`;
- in a developer checkout: `<repo>\artifacts\feeds\feedback.ndjson`;
- with `-DefyFeedbackStream=<path>`: that path.

The sample INI lists them all.

## Build

`build.cmd` builds the exe with Visual Studio's `cl` (2022 or later, C++ workload). It writes `bin\` holding
`ZStudioFFBHost.exe`, `SDL3.dll` and the sample INI. SDL3 3.4.20 (zlib) is in `../ThirdParty/SDL3`.

## Verified (2026-11-10)

- `--selftest`: 12/12.
- The dry-run tail test passes:
  - an event written before the host attached does not replay;
  - an `off` rule silences its id;
  - a line split across two writes is reassembled;
  - truncating the file restarts the read.
- **Not yet on real hardware:** the build machine has no wheel or pad attached. Run `--list` and `--test` with the
  device plugged in first.
