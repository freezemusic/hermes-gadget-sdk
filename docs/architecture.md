# Architecture

```
 ┌──────────────── device (ESP32 or simulator) ───────────────┐        ┌──────────── Hermes host ───────────────────────────┐
 │                                                            │        │                                                    │
 │  drivers / HAL ──events──▶ hg::App (firmware/core, C++17)  │  WS    │  plugin/hub.py ◀──▶ plugin/adapter.py ◀──▶ gateway │
 │  Wi-Fi, WebSocket,        state machine · protocol · auth  │◀──────▶│  auth, audio,      BasePlatformAdapter     runner, │
 │  LCD, I2S mic/amp,        UI renderer · VAD · actions      │ JSON + │  pacing, images    MessageEvent · TTS seams STT,   │
 │  buttons, NVS, console                                     │ binary │                                         agent, │
 │                                                            │        │  plugin/tools.py ── gadget_* agent tools   tools   │
 └────────────────────────────────────────────────────────────┘        └────────────────────────────────────────────────────┘
```

## Design goals

1. **Hermes stays unchanged.** The SDK plugs in through the gateway's platform-plugin interface and the hooks every adapter already has; see [hermes-integration.md](hermes-integration.md).
2. **The device stays thin.**
   - Speech recognition, text-to-speech, the language model, Markdown handling and image decoding all happen on the host.
   - The device captures audio, renders text and pixels, plays PCM and runs the actions it declares.
   - Firmware therefore needs no codecs, fonts beyond ASCII, or model-specific logic, and server-side improvements reach every board without reflashing.
3. **One core, many hosts.**
   - Everything that is not a driver lives in `firmware/core`: protocol, authentication, connection lifecycle, pairing UX, push-to-talk, VAD, playback control, UI, actions, telemetry and console. It is portable C++17 with no exceptions or RTTI, compiled unchanged for the ESP32 and for the desktop simulator.
   - The Hermes side mirrors this: `plugin/hub.py` knows nothing about Hermes and runs unchanged under the gateway adapter and under the standalone development server.
4. **Small surface, explicit contracts.**
   - The device–host protocol is one WebSocket carrying JSON control frames and binary media frames ([protocol.md](protocol.md)).
   - The device–driver contract is six small C++ interfaces (`firmware/core/include/hg/hal.hpp`).

## Device core (`firmware/core`)

| Module | Responsibility |
|---|---|
| `hal.hpp` | `Display`, `AudioIn`, `AudioOut`, `Transport`, `Storage`, `System`, and the optional `Updater` (the firmware update slot): implemented per port |
| `app.hpp/.cpp` | `hg::App`, the single-threaded application. Connection phases (boot → network → connecting → handshake → online) × interaction modes (idle, listening, thinking, responding) × overlays (card, image, question). It also authorizes and checks firmware updates and confirms a new firmware once it reaches Hermes |
| `protocol.*`, `json.*`, `crypto.*` | Message helpers, a small JSON parser/serializer, SHA-256/HMAC/Base64 for authentication and update checks |
| `ui.*`, `canvas.*`, `font5x7.cpp` | Deterministic renderer: a `UiModel` is drawn in four horizontal bands (top bar, header, content, hint bar). Only bands whose inputs changed are redrawn and flushed |
| `vad.*` | Energy VAD that ends hands-free (tap) utterances on silence |

### Threading model

`App` is not thread-safe, and that is deliberate.

- Ports call `App::tick()` every 5–20 ms and deliver every asynchronous event (Wi-Fi up, WebSocket frame, microphone chunk, button edge, console line) on that same thread.
- The Hermes WebSocket is recreated for every attempt. Connect timeouts back off from 1 s to 30 s (`hg/link_recover.hpp`); three in a row ask the port to rejoin Wi-Fi, and six restart the device. A server that closes the socket just backs off.
- On the ESP32, driver tasks post to a FreeRTOS queue that the main task drains.
- In the simulator, the WebSocket thread posts to a Python queue that the UI thread drains.

As a result the core has no locks and every test is deterministic.

### Rendering

- The port owns a full RGB565 framebuffer, in PSRAM on the ESP32.
- `Ui::render()` hashes the inputs of each band and redraws only the bands that changed, flushing them as full-width rows (contiguous memory, so a panel driver can DMA them).
- Header animations (spinner, level meter) live in the header band and stay on the 100 ms frame. A hero face steps about every 33 ms and, while it is moving, flushes the whole face and its caption; see the face section below.
- The renderer reads no clocks and uses no randomness. Identical models produce identical pixels, and the simulator tests rely on that.

### The face

Every screen that isn't showing reply text uses a **hero layout**: a shaded body with two dark eyes, drawn in code, at about two thirds of the free width, with the caption below the silhouette. That covers idle, listening, thinking, speaking before the text arrives, boot, connecting, offline, setup errors, pairing, short cards and questions. [faces.md](faces.md) is the pose table, the activity mapping and the colour and timing constants.

- **Captions:** a headline and one detail line. Pairing, setup, cards and questions may wrap to three or four lines; the renderer picks the largest disc that still fits, and drops caption lines rather than shrink the face below 48 px. A question adds its two answer buttons under the caption.
- **No scrolling needed:** a card too long for a caption gets the text layout. Long replies and long cards page by themselves (about a second per line), so a board with no scroll buttons can still read everything. UP/DOWN scrolls by hand and pauses the paging for 15 s.
- **Drawing:** `hg::face` fills a dithered diagonal gradient and two tapered eyes. Listening grows the eyes with the microphone level. Speaking bounces them with the speaker level. Thinking morphs the body into three dots; a status phrase keeps the body and switches to a working or searching pose. Connecting draws swirl rings. The pose, the blink and the glance are a pure function of `UiModel::face_frame` and the face style fields.
- **Clock:** other screens stay on the 100 ms frame. While a hero face is showing and the display is awake, the app also steps `face_frame` about every 33 ms. The face clock is not read while the screen is asleep.
- **Partial redraws:** the bars still flush only when their own inputs change. A live face flushes on every face tick, and the dirty rows are the silhouette plus the caption (dots, rings, the exclamation mark and the morph included). A finished sleep dot and the error mark do not. The picture key still describes the pose; it is not the only reason a frame is pushed.
- **Reply text:** a successful reply plays the morph-out and the short smile on the face first. The text then stays up for 20 s. On boards with scroll buttons, UP brings it back. The smile is not deferred until that linger ends.
- **Old bitmaps:** `hermes-gadget face` still writes 1-bit frames to `mascot_data.cpp` (about 27 KB). The UI does not reference them, so a link that drops unused sections leaves them out. The generator's own guide is the second half of [faces.md](faces.md).

### Interaction

- **Push-to-talk (default):** hold to speak, release to send. A press under 350 ms is discarded as a tap, and recording stops at 30 s.
- **Tap mode** (`set talk_mode tap`): tap to start; the VAD (or a second tap) ends the utterance.
- **Barge-in:** pressing talk while a reply plays stops playback immediately. The new message reaches Hermes, whose default "interrupt" busy mode stops the old turn.
- **Cancel** acts on release: it discards a recording, dismisses an overlay, or stops a turn (Hermes `/stop`).
- **Holding CANCEL for 2 s** starts a new session (`session.new` → Hermes `/new`). It fires while still held, after a countdown in the hint bar. The hold is the confirmation, so the plugin answers Hermes's "Confirm /new" itself.
- **Questions** from Hermes (other confirmations, dangerous-command approvals) take over the screen: TALK answers yes, CANCEL answers no. A question that arrives while you are recording waits until you finish.

## Hermes side (`plugin/`)

| Module | Responsibility |
|---|---|
| `hub.py` | `DeviceHub` (websockets server, handshake and enrollment, heartbeats, routing) and `DeviceSession` (outbound API: replies, status, cards, images, paced `AudioOut`, `invoke_action`) |
| `adapter.py` | `GadgetAdapter`, the `HubDelegate` that maps device traffic to `MessageEvent`s and Hermes output to device frames |
| `tools.py` | Agent tools, run on agent worker threads and bridged to the hub loop with `run_coroutine_threadsafe` |
| `textfmt.py` | Markdown → plain text, typography → ASCII |
| `audio.py` | WAV packing, streaming resampler, file decoding (WAV natively, the rest through ffmpeg) |
| `imaging.py` | Any image → RGB565 that fits the device's image box (Pillow) |
| `store.py` | Device keys and pending pairing codes in the plugin's data directory |

### Audio pacing

`AudioOut` resamples whatever Hermes produces (24 kHz streaming PCM, MP3/OGG/WAV files) to the device's declared rate. It sends 40 ms frames at most 0.5 s ahead of real time. If the producer stalls (a streaming TTS provider between sentences), the pacing clock rebases, so it never bursts more than the lead. A device therefore needs about 1 s of buffer whatever the reply length.

### Security model

- **Network gate (optional):** `GADGET_ACCESS_TOKEN`, a shared secret every device presents in `hello`.
- **Device identity:**
  - Each device holds a random 32-byte key, and its id is a hash of that key.
  - The key is sent once, at enrollment. After that the device proves possession with an HMAC over a fresh server nonce, so recorded traffic cannot be replayed.
  - Another device cannot take over an enrolled id.
- **Authorization:** Hermes's own allowlists and DM pairing. Every message from a device is authorized by the gateway runner exactly like a message from a Telegram user, and pairing approval happens on the Hermes host.
- **Transport:** use `wss://` (`tls_cert`/`tls_key`) on any network you don't trust. On a home LAN, plain `ws://` exposes conversation content to anyone on the network, though not the device key after enrollment.

## Why not use an existing Hermes API instead of a plugin?

Hermes also exposes an OpenAI-compatible API server and a JSON-RPC backend for its TUI and desktop app. A thin device could post text to either one. A platform adapter is the better fit:

- **Voice:** voice messages get Hermes's STT and auto-TTS for free, including streaming PCM. The HTTP API has no audio input, and the device would need its own ASR.
- **Identity and authorization:** each device becomes a first-class chat with its own session, memory and pairing, rather than an API client sharing one key.
- **Push:** the gateway can push to the device (cron results, `send_message`, other chats driving its tools) over the open socket.
- **Reach:** the adapter gets streaming previews, status phrases, interrupts and approvals through the same paths every messaging platform uses.
