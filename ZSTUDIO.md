# ZStudio-FFB

The Real Z Studios' fork of [Boomslangnz/FFBArcadePlugin](https://github.com/Boomslangnz/FFBArcadePlugin), forked
2026-11-10. James's instruction: *"fork and take over a zstudios version and build it out for all our future games"*.

## What upstream is, and what we keep

Upstream is a dinput8.dll proxy (`DllMain.cpp`, `IDirectInputDevice.cpp`, MinHook). It loads into an arcade game or
emulator, reads that game's state, and drives force feedback through **SDL2 haptics**:
- wheels and FFB joysticks get constant, spring, friction and sine forces;
- gamepads get rumble.

It has one module per game in `Game Files/`, and each reads that game's memory by signature scan. Its tuning lives in
`Config/` (MinForce / MaxForce, AlternativeFFB, feedback length, PowerMode, Enable and Reverse Rumble), edited by
`FFBPluginGUI.exe`.

**We keep:** the SDL haptic effect engine, the device handling, and the tuning model (strength floor and ceiling, the
low-end power curve, the one-direction wheel mode, the rumble swap).

**We add:** a **ZStudio game module** that needs no memory reading. Our own games *publish* their feedback, and this
plugin turns it into forces.

## Licence boundary (read this first)

Upstream is **GPL-3.0**, so this fork is GPL-3.0 too. Its code must never be compiled or linked into a Z Studios
game: those games are proprietary, and the GPL-3 text says *"The GNU General Public License does not permit
incorporating your program into proprietary programs."*

**The rule:** ZStudio-FFB runs as its **own process** (`ZStudioFFBHost.exe`, to be built). Games talk to it only
through the documented **ZStudio Output Protocol** below: a file or socket, never shared code. A game ships without
this plugin and works the same. The player installs ZStudio-FFB separately when they have a wheel or FFB stick.

Keep the upstream `LICENSE`, the credits and the history. Any binary we publish ships with its source.

## ZStudio Output Protocol (v1, from DEFY)

DefyEngine already writes every feedback event as NDJSON (`data/ui/feedback.json` `api.stream`, default
`artifacts/feeds/feedback.ndjson`, or `-DefyFeedbackStream=<path|off>`). One line per event:

```json
{"t":15.091,"tick":8520,"id":"fb_kill_confirm","src":"ev.kill.other","x":68.92,"y":66.85,
 "scale":1.000,"rumble":0.064,"shake":0.000,"trauma":0.000,"traumaNow":0.000,"v":1}
```

| field | meaning |
|---|---|
| `t` | game seconds |
| `id` | the game's effect id (`fb_shockwave_pass`, `fb_tsunami_wall`, `fb_thunder_clap`...) |
| `src` | what raised it (`ev.<kind>`, `hook.<name>`, `api`) |
| `x`, `y` | world position, when it has one |
| `scale` | distance and size scale |
| `rumble` | 0..1 rumble intensity actually played |
| `shake` | 0..1 camera one-shot |
| `trauma`, `traumaNow` | the trauma added, and the running total |
| `v` | protocol version |

v2, planned, for continuous effects: a shared-memory block or local UDP. It adds a steady state alongside the
events:
- `steer` (a constant force, -1..1);
- `spring` (centring strength);
- `friction` (ground drag);
- `surface` (a rough-terrain sine).

These cover vehicle games, where a wheel needs a held force rather than event pulses. Every game pins its protocol
version.

## Roadmap

1. **Host: BUILT 2026-11-10.** `ZStudioFFBHost/` is a standalone exe with no dinput8 proxy. It went straight to SDL3
   rather than wrapping the SDL2 code in `DllMain.cpp`, which is welded to the proxy's globals. It keeps upstream's
   tuning model: MinForce/MaxForce, PowerMode, Reverse, and the "1.5× to interrupt" rule.
2. **ZStudio module: BUILT 2026-11-10, in the host.** It tails the NDJSON (attach at the end, restart on truncation)
   and maps `rumble`/`shake`/`trauma` to a jolt, a sine or a rumble. Per-id rules live in `ZStudioFFB.ini` (Type,
   Force, Gain, LengthMs, PeriodMs, prefix `*`). Verified by `--selftest` and a dry-run tail test, not yet on a
   physical wheel. `ZStudioFFBHost/README.md`.
3. **Protocol v2 (continuous)** for driving or flying games, then a small C header in `ZStudioProtocol/` that games
   implement themselves (the header is the spec, not GPL code; it is written fresh and licensed MIT so games can use
   it).
4. **SDL3:** done for the host (3.4.20, `ThirdParty/SDL3`, zlib). The legacy dinput8 plugin stays on SDL2.
5. **GUI:** keep FFBPluginGUI and add a "Z Studios games" page.

## Relationship to DEFY's built-in pad layer

DEFY's `FDefyPadFX` (SDL3, in DEFY's own repo) drives gamepads: Xbox trigger motors, DualSense triggers and rumble.
It is **separate code, not derived from this fork**. ZStudio-FFB is for what a game should not carry itself: wheels,
FFB sticks, cockpit rigs. Both read the same feedback ids, so one tuning table drives both.

## Nightmarch (2026-11-13)

James: *"we will want to fork and take over for him and build all our games into this library for physical force
feedback on the board etc"*. He chose, in Nightmarch's DEV01 session: a separate program (the licence boundary above),
this fork on his account, and a video layer of every kind (Nightmarch's side, not this repo).

- **The game publishes v1** from `Nightmarch/src/Nightmarch.RulesEngine/Play/Feedback.cs`, one home for force: a pulse
  for every felt event, `fb_nm_<event type>`, derived from its sound cue. The playtest writes
  `artifacts/feeds/feedback.ndjson`, and the Unity front-end will write the same file.
- **`ZStudioFFB.ini`** finds the Steam install (`SteamGame2=Nightmarch|...`) and the dev checkouts, and tunes the
  `fb_nm_*` ids: the door-engine and a march unmade roll, a banner falling kicks, the quiet beds are off.
- **The board output (built 2026-11-13, `ZStudioFFBHost/board.h`).** James: *"support our adaption of the ESP32 and
  motor drivers for rumble and for led rgb lighting effects"*. The host now also drives Nightmarch's physical board: a
  bridge ESP32 on USB serial (`[Board] Port=COMn`, 921600 baud) fans frames out over ESP-NOW to the props, each with
  WS2812/SK6812 lights (RMT) and a vibration motor on a PWM motor driver (20 kHz, `Nightmarch/firmware/src/node/`).
  - **Wire:** Nightmarch hardware protocol v1 (`Nightmarch/firmware/protocol/protocol.toml`), LED 0x07 and RUMBLE 0x08,
    plus HELLO when the port opens; it prints the bridge's HELLO_ACK, SAFE mode and log lines. **No firmware change:** a
    node's motor takes the newest RUMBLE frame, so a motor shape is a train of 50 ms frames (each lasting 150 ms, the
    last one 0), and the node still clamps every rumble to 2.5 s.
  - **Cues:** Nightmarch's `Board.cs` is the one home: per event, the prop (a territory wire code from
    `territories.csv`, 0 = every prop), a light (a firmware effect and a palette colour: ember for the mythic, cyan for
    the Tower, red for loss, moonlight for the night) and a motor (strength, length, shape: steady, fade, swell, pulse,
    heartbeat). They ride on the v1 line as flat extra fields (`terr fx rgb lightMs pri motor motorMs shape periodMs`),
    which any older v1 reader ignores. The shapes match `Board.cs` exactly (the same vectors in `--selftest` and in
    Nightmarch's `BoardTests`).
  - **Any game:** `[Board] ForceToMotor=1` plays the wheel force of a game with no board fields on every prop's motor.
  - **Try it:** `--board-test` (ember, cyan gears, a red fade, a gold swell, a heartbeat on every prop); `--dry-run`
    prints each cue and its first frame in hex; `--board COM5` overrides the INI.
  - **Next (hardware lane):** a per-prop ADDRESS for the Tower, which moves (it is reached as every-prop today), and
    the motor driver's real stall floor (40/255 is assumed) measured on the printed props.

