# Faces

Hero screens draw a face in code: a flat coloured disc and two dark pill eyes, with no mouth.
The same routine covers idle, listening, thinking, speaking before the text arrives, boot,
connecting, pairing, offline, errors, questions and short cards. Reply text still replaces the
face until it times out.

The pose is a function of the frame counter the app passes in. The renderer never reads a
clock and never draws a random number, so the same model is the same pixels.

| Screen | Face |
|---|---|
| Ready, boot, short cards | Neutral. Blinks and glances on their own, with a slow breath |
| Listening, questions | Attentive. The eyes grow with the microphone level |
| Thinking | Both eyes look up and aside, and three dots step underneath |
| Responding, while there is no text yet | The eyes bounce with the speaker level |
| Error | Confused: one eye stays open, the other flattens, and the disc turns red |
| Pairing | Curious: the two eyes tilt differently |
| Connecting, updating | The curious pose, plus the stepping dots |
| Offline | Sleepy: two small dots, on a dimmer disc |
| Ready, for about two seconds after a successful reply | Happy: the eyes flatten into short upward dashes |

A successful reply arms that smile. If the reply is text, the text stays up for its usual
20 seconds and the smile plays the next time the face is back. A voice-only reply, with the
face still on screen, smiles at once. A failure does not.

The disc is blue (`#499BF1`) on every state except an error (red, `#EC665E`) and offline
(a dimmer blue). A lighter patch sits in the upper left and a darker crescent in the lower
right. Emissive panels keep the field around the disc true black, so those pixels stay off.
The eyes are `#0D0D0D`.

While a hero face is on screen and the display is awake, the app steps a face clock about
every 33 ms (about 30 fps). Every other screen stays on the 100 ms frame. Only the rows the
eyes can touch are flushed, unless the caption or the screen changed, in which case the whole
hero band is drawn. The disc is painted inside that clip, so an eye-band flush does not leave
a trail.

The watch's QMI8658 is not read by this firmware, so the face does not parallax with tilt.

## Changing colours and timing

Colours are RGB888 constants in `firmware/core/include/hg/face.hpp`:

| Constant | What it is |
|---|---|
| `kBlueR/G/B`, `kBlueHi*`, `kBlueCore*`, `kBlueShade*` | The default disc, its highlight, the small gloss and the lower crescent |
| `kRed*` | The error disc |
| `kDim*` | The offline disc |
| `kEye*` | Both pupils |

`rgb565()` quantises them. A colour that is only a few steps from its neighbour can collapse
to the same 16-bit value.

Timings assume one face frame is 33 ms (`kFaceFrameMs` in `firmware/core/src/app.cpp`). The
schedules are in `firmware/core/src/face.cpp`:

| Constant | What it changes |
|---|---|
| `kBlinkWindow`, `kBlinkBase`, `kBlinkJitter` | Gap between blinks. With the current numbers the gap is 78 to 180 frames, 2.6 s to 6.0 s. Every fifth blink is a double |
| the `pulse()` table | Close, hold, open. The current table is 2, 1 and 4 frames, about 70 ms, 30 ms and 140 ms |
| `kGlanceWindow`, `kGlanceJitter` | Gap between glances, 36 to 104 frames, 1.2 s to 3.5 s. One glance in four stays put |
| `kEase` | How fast a glance eases in and out (`1 - exp(-dt·k)`, k ≈ 11/s) |
| `kBreathAmp` | Idle breathing added to the eye height, thousandths of the disc |
| `kSpeakAmp`, `kSpeakVoice` | Speaking bounce. The base term plus the speaker level, about 5.2 Hz |
| `kHappyFrames` in `app.cpp` | How long the smile lasts, in face frames (60 is about 2 s) |
| `kBandAbove`, `kBandBelow` | The rows a partial flush covers. A pose that leaves this band leaves a trail |

Poses are in `pose_left` and `pose_right`. `x`, `y`, `w` and `h` are thousandths of the disc
diameter, origin at the centre, and `tilt` is degrees clockwise (screen y grows down).

The blink spacing, the one-in-five double blink, the glance spacing and the exponential ease
are adapted from [zongzhu-bot](https://github.com/Liu-zhongxian/zongzhu-bot) (MIT). The
drawing is original. See [NOTICE](../NOTICE).

Host renders of each state, from the 410×486 watch window:

| Ready | Listening | Thinking | Speaking |
|---|---|---|---|
| ![Ready](images/face/ready.png) | ![Listening](images/face/listening.png) | ![Thinking](images/face/thinking.png) | ![Speaking](images/face/speaking.png) |

| Error | Pairing | Connecting | Offline |
|---|---|---|---|
| ![Error](images/face/error.png) | ![Pairing](images/face/pairing.png) | ![Connecting](images/face/connecting.png) | ![Offline](images/face/offline.png) |

| Happy | Blink | Question | Boot |
|---|---|---|---|
| ![Happy](images/face/happy.png) | ![Blink](images/face/blink.png) | ![Question](images/face/prompt.png) | ![Boot](images/face/boot.png) |

Idle, including a glance and a blink, at the face clock:

![Idle face](images/face/idle.gif)

## The bitmap generator

`hermes-gadget face` still turns a picture into 1-bit frames in
`firmware/core/src/mascot_data.cpp`. **The screen does not draw those frames.** The hero
layout calls `hg::face` instead, and a build that drops unreferenced sections does not link
the bitmaps. The generator and its tests stay so the old artwork can still be reproduced.
What follows is that generator. It does not change the face on the device.

Artwork in this repository, in examples and in tests is original artwork. A picture of a real
person belongs on your own device and nowhere else, and this project is independent of Nous
Research, so nothing here should look like an endorsement of one.

## What a drawing has to be

The screen is one bit deep: a pixel is ink or it is nothing. Whatever you feed the command
gets reduced to that, so the art has to survive the reduction:

- **Flat two-tone works.** Shading, gradients and photographic detail turn to mud, because
  everything between black and white has to go one way or the other.
- **Silhouette plus one distinguishing feature survives.** At 64 px a hair shape, a pair of
  headphones, glasses or a cap read. A likeness does not.
- **The head should fill the frame.** A face that is 6% of the frame gives an eye about 11 px
  wide on a 240x240 screen, and a blink on an 11 px eye is invisible. Cropping tighter is the
  biggest single improvement you can make.
- **Both eyes have to be drawn.** An eye in shadow, behind a visor or under a mask leaves the
  blink with nothing to close, and the blink has to be invented.

## Ask an image generator for this

```
Square 1:1 image, at least 1024x1024. A bold two-tone stencil portrait of [SUBJECT], head
and shoulders filling the frame, facing the camera or in three-quarter view. Pure black
background, the subject drawn in pure white only. Flat shapes, thick clean edges, no grey,
no gradients, no shading, no background detail, no text, no watermark, no border. Both eyes
clearly visible as light shapes, and the mouth visible.
```

Image generators hand back flat files, usually JPEG, so there is no transparency in them.
That is what `--mask` is for. Asking for a square image saves you a crop.

## Generating a face

```
hermes-gadget face my-face.png --mask bright --report
```

Three things come out of it:

| Output | What it is |
|---|---|
| `firmware/core/src/mascot_data.cpp` | The frames, ready to build. `--out` puts them elsewhere. |
| `build/face-preview.png` | The three frames at all four sizes, on the device's background |
| `build/face-check.png` | Your art with the eye boxes, lid lines, mouth and effect anchors on it |

**Look at the check image first.** It is the only way to see whether the marks landed on the
features before spending a flash.

The project's own mascot is the worked example on this page: `hermes-gadget face` with no picture
regenerates it, and every number in the tables below is hers. Running the picker on
`assets/mascot/nous-girl-white-1024.png` puts the marks on art whose answers are already known,
which is the quickest way to see what they do.

`--mask` says where the ink is in your picture:

| Value | Meaning |
|---|---|
| `alpha` | The picture carries its own transparency |
| `bright` | Ink is the light part: white art on a dark background |
| `dark` | Ink is the dark part: black art on a light background |

Generated images and screenshots need `bright` or `dark`. With none of them you get one solid
block, and the command says so rather than writing it.

Then build and flash as in [hardware.md](hardware.md), or install your build over the air from
the Hermes computer:

```
hermes gadget update "Kitchen" firmware/esp32/.pio/build/<board>/firmware.bin
```

Either way the device keeps its settings, so it stays paired and keeps its Wi-Fi, its volume
and its device key.

**Updates bring back the default face.** `hermes gadget update --latest` and the browser
installer install release firmware, which carries the shipped mascot. To keep your face on a
newer release, build that release with your `mascot_data.cpp` and install the build as above.

## Placing the eye and the mouth

The device blinks and talks, so the generator has to know where the face's features are. It
starts from the drawing's own outline and works in fractions of it, so the numbers travel
between images:

| Flag | Meaning | Default |
|---|---|---|
| `--face-x` | Horizontal centre of the face | 0.50 |
| `--eye-line` | Height of the eye centres | 0.40 |
| `--eye-span` | Distance between eye centres | 0.22 |
| `--eye-size` | Eye radius | 0.05 |
| `--eye-ratio` | Eye height against its width | 0.60 |
| `--mouth-x`, `--mouth-y` | Mouth centre | face x, 0.62 |
| `--mouth-w`, `--mouth-h` | Mouth size | 0.10, 0.045 |
| `--ear-cup` | Where the listening waves start | 0.62 0.30 |
| `--think-dot` | Where the thinking dots go | 0.82 0.11 |

A centred portrait needs nothing but the picture. These four cases need more:

- **A face that is off centre**, because the drawing's outline includes hair or shoulders:
  move `--face-x`, and `--mouth-x` if the mouth is not under the eyes.
- **A three-quarter view where the two eyes differ**, for example a far eye half the width of
  the near one: give each eye its own box. The four numbers are centre x, centre y, width and
  height, as fractions of the outline, and both eyes must be given or neither:

  ```
  --eye-left  0.2197 0.4602 0.0482 0.0746
  --eye-right 0.3826 0.4349 0.1404 0.0835
  ```

  Those are the mascot's own eyes, whose far one is a third the width of the near one and sits
  lower. They are what `hermes-gadget face` uses with no arguments.
- **An eye drawn as shadow or a mask**: `--blink dark` darkens the eye and draws the lid as a
  light line. The default, `--blink light`, covers a light eye and cuts the lid out of the
  cover, which on a dark socket reads as a white blob over the face.
- **An eye that is small in the frame**: `--eye-grow` scales the blink patch about the centre
  you placed, so the blink is still visible at 64 px. Raise it until `--report` puts you in
  the same range as the shipped mascot.

A logo or an object, with nothing to blink, can be generated as a still picture with
`--plain`.

Crop before you measure, not after: `--crop X0 Y0 X1 Y1` trims the source first, which moves
the outline and therefore every fraction above.

### Clicking the features instead of measuring them

When the numbers are hard to guess, the window takes them off the picture:

```
hermes-gadget face my-face.png --mask bright --pick
```

It opens as the device sees it, with the marks the current numbers would draw on it. Click the
centre of the near eye, then the far eye, then the mouth; press Return to leave the listening
waves and the thinking dots where the geometry put them. A click moves that mark and keeps the
size the measurement found, so a click only has to land on the feature. The window prints the
flag line for a real run, and shows it in the window to copy:

```
hermes-gadget face my-face.png --mask bright --eye-left 0.2197 0.4602 0.0482 0.0746 ...
```

R starts over, Q closes the window. `--pick` writes nothing, so a mis-click cannot touch the
artwork. It needs Tk, the same dependency the simulator window has.

The copied command uses the full image path and keeps the crop, threshold, blink mode, and
anchor positions shown in the picker. Enlarged eye and mouth sizes are already included in
the coordinates; do not add the growth flags again. On Windows, paste the command into
PowerShell. On macOS and Linux, use a POSIX shell such as Bash or Zsh. Add `--out`, `--preview`,
or `--check` to choose output files before running it.

With the project's mascot image, explicit feature coordinates generate your selected geometry.
Run `hermes-gadget face --mascot` to reproduce the original shipped frames.

## Will the blink read?

```
hermes-gadget face my-face.png --mask bright --report
```

`--report` prints how many bits each frame changes against idle, per size. The number that
decides it is the 192 px one, because that is the largest the face is drawn on a 240x240
screen. The shipped mascot changes **226 bits at 192 px** on a blink and 16 on talk, and the
test suite asserts those two numbers. Anything in that range reads on the screen; much less
and the blink is a flicker nobody notices.

## Putting the mascot back

```
git checkout -- firmware/core/src/mascot_data.cpp
```

Then build and flash. The shipped artwork is the default, and the generator reproduces it
exactly, so there is nothing to keep in sync.

## How it works

The frames are 1-bit bitmaps: row major, most significant bit first, rows padded to a whole
byte, a set bit meaning ink. Four sizes and three frames take about 27 KB. The UI no longer
reads them; [architecture.md](architecture.md) describes the face the screen does draw.

The blink and talk frames are drawn on top of the idle frame: a blink covers each eye and cuts
a closed lid into it, and a talking mouth is a hole cut into the ink. Both come from the
feature positions above, which is why those positions matter.

The effect anchors are written as thousandths of the drawn size. They belonged to the old
bitmap face. The procedural face does not read them.
