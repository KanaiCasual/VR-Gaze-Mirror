<p align="center"><img src="docs/logo.png" width="96" alt="VR Gaze Mirror logo"></p>

# VR Gaze Mirror

[![Latest release](https://img.shields.io/github/v/release/KanaiCasual/VR-Gaze-Mirror?label=latest%20release&color=4DB2FF)](../../releases/latest)
[![Released](https://img.shields.io/github/release-date/KanaiCasual/VR-Gaze-Mirror?label=released)](../../releases/latest)
[![Downloads, all versions](https://img.shields.io/github/downloads/KanaiCasual/VR-Gaze-Mirror/total?label=downloads%20%28all%20versions%29)](../../releases)
[![Downloads, latest version](https://img.shields.io/github/downloads/KanaiCasual/VR-Gaze-Mirror/latest/total?label=downloads%20%28latest%29)](../../releases/latest)
[![Open issues](https://img.shields.io/github/issues/KanaiCasual/VR-Gaze-Mirror)](../../issues)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-green)](LICENSE)

**A clean mirror of your VR headset for OBS and streaming. Any OpenXR or SteamVR game, one source, no setup** -
and if your headset tracks your eyes, a ring on the mirror shows your viewers where you are looking.

Install it, add the source in OBS, play. One eye, cropped and steadied, at the size and rate you choose. Nothing is
drawn in the headset, nothing in the game is touched, and there is no layer order to fix or window to keep on top.

The framing is set from a picture of your own view, not by guesswork: press **Capture from headset**, put the headset on,
press a key (or let the countdown do it), and the crop box sits on the real image. **Find visible edges** shows exactly
what your eye can see of that image, so the box goes where the picture actually is - no taking the headset on and off
to check crop sliders that may or may not line up.

https://github.com/user-attachments/assets/97e8f727-dba7-481a-b9ef-01490a8ec5f4

*What your viewers see: your view as you see it, framed the way you set it - and, with eye tracking, the ring that
follows your eyes, its tail showing where they came from. You see none of it in the headset.*

[![Watch on YouTube](https://img.shields.io/badge/Watch%20on-YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/watch?v=ixilzm8F_SY)

One installer sets up everything:

- **The mirror layer** - an OpenXR API layer that copies what your headset shows in OpenXR games (DCS World, Assetto
  Corsa EVO, VAIL and the like; Direct3D 11 and 12, quad views included), crops, frames and steadies it in a single
  pass.
- **The SteamVR helper** - the same for games that use SteamVR directly (VRChat, Half-Life: Alyx, Pavlov and the like).
  It starts and stops with SteamVR by itself.
- **The "Gaze Mirror" OBS source** - reads the picture straight off the graphics card, no capture of a window. Or use the
  **mirror window** with anything that can capture one (OBS, Discord, ...).
- **A settings app** - the framing tools: a crop box you set from a picture of your own view, taken from inside the
  headset; an edge finder that shows what your eye can actually see; gaze-following and steadying; per-game crop
  profiles; picture size and rate caps. Everything is set live, with nothing to edit by hand.
- **The gaze ring** - with any eye-tracked headset that feeds the standard interfaces, or through VRChat's avatar eyes
  as a fallback. Drawn on the mirror only, never in the headset. Optional: the mirror works without it.
- **Quad-Views-Foveated, optionally** - Matthieu Bucchianeri's official installer is bundled and offered at the end of
  setup, for eye-tracked foveated rendering in OpenXR games. The ring does not need it.

*Called "QuadViews Gaze Mirror" up to version 1.2.2, when the ring was built on modified Quad-Views-Foveated and
OpenXR-Layer-OBSMirror layers. Since 2.0 everything that makes the picture is this project's own code, and Quad-Views-Foveated
is an optional extra.*

**Download:** the latest `VR-Gaze-Mirror-<version>.msi` is on the
[Releases page](../../releases/latest); the release notes say what changed.

---

## Contents

- [What you need](#what-you-need)
- [Installing, updating, removing](#installing-updating-removing)
- [Recording and streaming: two ways](#recording-and-streaming-two-ways)
- [The settings app, page by page](#the-settings-app-page-by-page)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [Things to know](#things-to-know)
- [Credits and licence](#credits-and-licence)

---

## What you need

- Windows 10 or 11, 64-bit.
- A VR headset. **Eye tracking is optional:** the mirror, the crop tools and everything else work without it; there
  is just no ring. For the ring, the headset's software has to feed its eye tracking to the standard interface the
  game uses - OpenXR's eye-gaze extension for OpenXR games, SteamVR's eye tracking for SteamVR games (Pimax Crystal,
  Varjo, Quest Pro over Virtual Desktop, Vive Pro Eye, ...) - or feed VRChat, which the ring can fall back to.
- **OpenXR games** that render with **Direct3D 11 or Direct3D 12** (DCS World, Assetto Corsa EVO, VAIL, ...), or
  **SteamVR games** (VRChat, Half-Life: Alyx, ...), whatever they render with.
- For recording through the plugin: **OBS Studio**. For the mirror window: nothing else.

Developed and tested with a Pimax Crystal Super, through SteamVR and through Pimax's own OpenXR runtime, in DCS World,
VRChat, Assetto Corsa EVO, VAIL and Half-Life: Alyx.

## Installing, updating, removing

1. Close your game and OBS, then run the `.msi`. Windows SmartScreen will warn, because nothing here is code-signed:
   "More info" > "Run anyway". A first install offers a desktop shortcut.
2. The last page offers to install **Quad-Views-Foveated 1.1.3** if it is not on the PC. Say yes if you want foveated
   rendering in OpenXR games; the mirror works either way.
3. Start **VR Gaze Mirror** from the Start menu. The **Status** page should show green dots for the layer, the OBS
   plugin and the SteamVR helper.

**Coming from 1.x:** just run the installer over it. The two modified layers and the old "OpenXR Mirror Capture" OBS
plugin that 1.x put into OBS are removed, your settings, presets and crop pictures are carried over, and the old
"QuadViews Gaze Mirror" data folder is cleaned up. Replace the old source in your OBS scenes with the new **Gaze Mirror**
source (the old one shows black).

**Updating:** the app tells you when a newer release exists. **Download and install** saves the new installer into your
Downloads folder, checks it against the SHA-256 published with the release, and opens it with Windows Installer; your
settings stay. Or run the newer `.msi` over the old one yourself. **Repair / remove:** run the installer again (or
"Modify" in the installed-apps list) for a page with **Repair** and **Uninstall**. Uninstalling leaves your settings
files in place, and SteamVR forgets the helper.

## Recording and streaming: two ways

Both show the same picture: the eye you chose, cropped, framed and steadied the way you set it up on the **Mirror**
page - with the gaze ring on it, if your headset gives gaze. Both can be used at the same time.

### 1. OBS with the Gaze Mirror source - the cheapest, best for recording

The installer puts the plugin into OBS.

1. In OBS: **Sources > + > Gaze Mirror**. It has no settings of its own; everything is on the Mirror page.
2. Start your game. The picture appears once a VR session is running.
3. Select the source and press **Ctrl+F** (Transform > Fit to screen) once. After that it keeps fitting your canvas,
   whatever size the crop box has.

OBS reads the picture straight off the graphics card. While OBS is not showing the source, no mirror work is done at all.

### 2. The mirror window - for Discord, or anything else that captures windows

No OBS plugin involved. On the **Mirror** page press **Open**: a window with the mirror picture appears on the monitor
you chose, **behind every other window** (behind a borderless game too) and without ever taking the focus. Window
capture still sees it there.

- **Discord:** Share Your Screen > Applications > *VR Gaze Mirror - Mirror*.
- **OBS without the plugin:** Sources > + > **Window Capture** > that window.
- **Size** and **Rate** decide what the capturing program gets (for example 1920 x 1080 at 30 fps for Discord). Smaller
  and slower is cheaper: this way costs more graphics card time than the plugin, because the window has to redraw the
  picture and the capturing program copies it again.
- **Title bar** for capture tools that only list windows that have one.

## The settings app, page by page

Every setting has an **(i)** with an explanation; changes save automatically and **reach a running game at once** (except
the Quad Views page - see there). The app is small on purpose: it is meant to sit next to OBS while you tune.

The pages are listed in the order of what the program is for: **Status** and **Mirror** first - the mirror is all most
people need - then the four pages of the **gaze ring** (Gaze, Ring, Tail, Motion), then the **extras** (Desktop
cover, and Quad Views when Quad-Views-Foveated is installed), and About.

### Status

![Status page](docs/page-status.png)

- **Components:** the mirror layer, the OBS plugin, the SteamVR helper and (optionally) Quad-Views-Foveated, each
  with what is wrong if something is. The line at the top says what is being mirrored right now and whether gaze
  arrives.
- **OpenXR API layers:** every layer on your PC in the order games load them, with a tick box to switch each on or off
  and arrows to **reorder** (Windows asks for permission for that one change; the app itself never runs as
  administrator). The app knows the ordering rules of common layers and offers **Fix order** when something is wrong.

### Mirror

![Mirror page](docs/page-mirror-2-1.png)

*Blue frame = what is recorded; blue tint = how far it follows the gaze; amber lines = still zone; green band = room for steadying.*

The **Show** row above the picture switches each of these helps on or off - the crop box, a small cross at the **box's
own centre**, the follow reach, the steadying room, the visible edges, a **centre** mark, and the **gaze** ring as it
was when the picture was taken. The centre mark is a dot with a line across and a line down the whole image at the
point where **straight ahead** lands in the eye image. That point comes from the headset's own projection with every
picture, so it is right for whatever headset you have; on most it is not the middle of the image, which a faint small
cross marks. Line the box's cross up with it, or with the visible edges, and the framing is centred on what you see.

**Picture:** which **eye** is mirrored (right by default), the **picture size** cap (3840 on the longest side by default,
so a 5000-pixel eye image does not go to OBS at full size) and the **picture rate** cap. These apply to OBS and the
mirror window alike.

**Crop profiles** (top of the page): save a framing under a name, and give it a game's program file (DCS.exe,
VRChat.exe, ...) to have it applied whenever that game starts. Edits go into the chosen profile.

**Mirror window:** Open / Close, which monitor (or an ordinary window), size, rate and the title-bar option.

**Crop the picture:** instead of typing crop percentages into OBS, drag a box - locked to a shape (16:9, 9:16, 1:1,
4:3, 4:5, 21:9 or free) - on a picture of the whole eye image. The picture handed out **is that box**. Drag inside the
box to move it, a corner or the mouse wheel to resize, **Centre**, **Largest**, and **Lock** against stray clicks.

**Find visible edges:** the headset shows more than the eye can see through the lenses, and the crop should cover only
what you see. Press the button with a game running and the headset on: a bright bar appears in the mirrored eye's
view, left of centre. Push it outwards with the arrow keys (hold to keep moving, Shift for bigger steps) until it just disappears, press
**Enter**, and the next bar comes - right, top, bottom. **Backspace** goes back a bar, **Esc** cancels. The keys are read
by the app, which takes the focus when the finder starts, so the game gets none of them. The result is drawn on the crop
picture as a dashed box, and **Crop to edges** makes the crop box the largest one of the chosen shape inside it.

**The picture behind the box:**

- **Refresh picture** takes one from the running game right now.
- **Capture from headset** takes it the way you actually sit: press it (a large banner says what to do), put the headset
  on, look straight ahead and press the key - **Pause** by default, changeable with the key button. You hear a sound, and
  it switches itself off. It takes one picture of **each eye**, and that picture is **kept** - through restarts too -
  until you press Refresh picture. The key is only looked at while armed. If no key can reach the game, or your hands
  are on the controls, choose **By itself after 5 / 10 / 15 / 20 / 30 s** next to the key button: the picture is then
  taken when the countdown runs out, or earlier if you press the key. The seconds left are shown large in the middle of
  the headset view while it counts down, so you know when to hold still. The picture also carries the **ring** where
  you were looking as it was taken, drawn at the ring's size and colour, so a capture shows the framing and the gaze
  together (the Show row switches it off).

**Follow my gaze (up / down):** a 16:9 box only covers about half the height of the eye image, so looking down at your
knees or up at the canopy leaves the frame. With this on, the box glides up or down just far enough to keep what you
look at in frame - like a camera operator.

- **Still zone** (dashed amber lines): the middle part of the box in which your gaze moves nothing.
- **Reach** (blue tint): how far the box may travel. Two dotted lines show where the frame **ends when you look all the
  way up** and **starts when you look all the way down**.
- **Glide**, **Return home** and **Return after**: how softly it moves, and whether and when it goes back.

**Steady the picture:** takes small head movement and tracking jitter out of the picture. The head orientation goes
through a One-Euro filter and the box counter-moves the difference.

- It adds **no delay**: no frames are held back or compared - it only uses the head position the game rendered with.
- **Steadiness** (lower = steadier, but the picture trails further behind a head turn), **Follow quick turns**, and
  **Room to move** - shown as a green band around the box (and around the blue reach). The box cannot enter that band, so
  it always stays inside the image; switching steadying on moves or shrinks the box to make room.
- It needs the crop to be on, and corrects turning and nodding - not tilting the head sideways.

Cropping, following, steadying and the ring happen in the one pass that makes the picture, so they cost nothing extra.

### Gaze

The first of the ring's pages: where the ring gets your eyes from, and where it lands in the picture. If you only
want the mirror, nothing below the **Gaze ring** heading in the app concerns you.

**Gaze source:** the ring's main source is the headset itself - OpenXR's eye-gaze extension in OpenXR games, SteamVR's
eye tracking in SteamVR games. Exact angles, nothing to set up, and as long as that gaze arrives it is all the ring uses.

The helper has **two ways to the headset's gaze**. First SteamVR's own eye tracking, which the headset's SteamVR
driver feeds. Where that driver passes none on, or passes on a direction no eye can look in, the helper asks the
headset's OpenXR runtime through the standard eye-gaze interface, from a session that is never begun and never
draws, so nothing shows in the headset and the game is not touched. It takes the PC's active OpenXR runtime, or -
when that is SteamVR's own - the installed runtime that belongs to the headset, so it does not matter which runtime
is set as the active one. That is the headset's own gaze either way: exact angles, full range, nothing to calibrate,
no other software.

OpenXR games that run **through SteamVR's runtime** are in the same position as SteamVR games - their gaze, too, is
only what the SteamVR driver passes on. For those the helper, which runs next to SteamVR anyway, hands the headset's
gaze to the layer inside the game while the game is mirrored and has none of its own.

The **VRChat fallback** is for when it does not arrive. Some eye-tracking software takes the eye cameras for itself
(SRanibro), or only ever feeds VRChat (EyeTrackVR, Quest Pro over ALVR, ...); the headset's own gaze is then gone, and
the only trace of where you look is the VRChat avatar's eye parameters, driven through
[VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking). For those, the SteamVR helper is an **OSCQuery
service**: VRChat finds it on the machine and sends it the avatar's parameters, and the eye ones drive the ring. Nothing
to install or set up - OSC has to be on in VRChat (it is, for every face-tracking user) and the avatar needs eye
parameters (face-tracking avatars have them). The Gaze source card says *VRChat: eye parameters arriving* once it works, and
"Headset, else VRChat" (the default) switches to the fallback by itself whenever the headset gives no gaze.

Whether the fallback is needed at all depends on the eye-tracking software, not on the headset. On a Pimax Crystal,
for example, SRanibro stops the Tobii service to take the eye cameras for itself, so the headset's own gaze is gone and
the ring falls back; [BrokenEye](https://github.com/ghostiam/BrokenEye) with the SRanipal model reads the same cameras
through the Tobii service instead, the headset's gaze stays available, and the ring never needs the fallback - full
reach, no calibration, exact angles.

**Fixed centre** is for headsets without eye tracking at all. The ring then sits where straight ahead lands in each
eye (the Mirror picture's centre mark) and never moves. With Quad-Views-Foveated, which puts its sharp area at that
same spot when it has no eye tracking, the ring is drawn the size of that area - its fixed sections and offsets, read
from its settings - so viewers see where the picture is sharp. Without it, or when the game did not use quad views,
the ring keeps the size set on the Ring page. The choice is greyed out once the headset is known to give gaze.

**SRanibro's raw gaze** skips the fallback altogether. A coming SRanibro release (newer than 0.1.9-Beta, which does not
have it yet) can send the tracker's raw gaze straight to the helper: tick **Raw gaze OSC** in SRanibro's settings
(127.0.0.1, port 9005, Apply).
The Gaze source card then says *SRanibro: raw gaze arriving*, and the ring gets a direction straight from the tracker, no
calibration, in every game while SRanibro runs - VRChat and DCS alike. It is taken ahead of the VRChat fallback whenever
it arrives. (If port 9005 is taken on your PC, change it in the port box on the Gaze source card and set the same port in
SRanibro.) That channel carries the same range cap as SRanibro's VRChat output (see below).

**Calibrate VRChat gaze** (for the fallback only - the headset's own eye tracking needs none): the avatar's eye values
are not angles, and not in proportion to them either, so the helper measures once how they relate to where you look.
Press the button with the headset on and VRChat running: a small ring-and-dot target appears inside the headset at
twenty places, about two seconds each - straight ahead, then out to 35 degrees to each side, 28 up and 35 down, then
the four corners. Follow it with your eyes only, head still; the status line counts the targets. After about 45 seconds
the target disappears and the ring lands where you look. The reach is trimmed to what both eyes of your headset can
show, so a narrower headset gets the same pattern pulled in a little; the status line states the reach used. Redo it
after changing eye-tracking software or its own calibration; **Forget** drops it.

**Thorough** does the same with a moving target: it glides slowly over the whole reach, resting on 25 spots on the
way, and you follow it with your eyes. Everything that arrives is recorded - a few thousand samples instead of twenty -
the delay between the target and the avatar's eyes (the eyes trail a moving target, and VRChat's path adds its own)
is measured and taken out, and a finer map is fitted, a grid over the whole field instead of a curve per axis. About
two minutes. The status line then says how many samples were used, the delay found and how closely the map fits.

**Known limit of the fallback:** the ring can only go as far as the eye-tracking software lets the avatar's eyes go.
**SRanibro caps the gaze it sends to VRChat at about 30 degrees in every direction** (0.1.9-Beta; a coming build raises
it to about 40) on the headsets it serves (Pimax Crystal/Super, StarVR One, Varjo); past that the ring holds at the edge
of the range while your eyes carry on. The cap is in SRanibro's output, not in this project.

**Where the ring lands:** normally nothing to do. If the ring sits beside what you look at:

- Switch on **"Show a marker inside the headset (calibration)"** - a small bracket reticle appears *in the headset*
  where the ring is on the mirror. Look at something small and nudge with the sliders, or in the game with
  **Ctrl+Alt+arrow keys** (Shift = bigger steps; these keys only exist while the marker is on). Switch it off again when
  done - it is only ever switched from this page. In OpenXR games the layer draws it into the picture; in SteamVR games
  the helper shows it as a small SteamVR overlay two metres out along your line of sight.
- **What you usually look at is ...** sets the distance used to place the ring (a cockpit panel is close, the world is
  far): the mirror shows one eye, and the two eyes see close things in different places.
- **Ignore Quad-Views-Foveated's focus offsets** only appears with the **Legacy** placement method, the one it belongs to.

### Ring

![Ring page](docs/page-ring.png)

How the gaze indicator is drawn, with a **live preview** over cockpit, terrain, sky or black.

- **Styles:** Ghost ring with teardrop tail (recommended), plain ring, soft glow, small dot, spotlight (darkens
  everything else), bubble and solid blob with a fading trail, and a **heatmap** (blue = glance, red = stare; spots stay
  where you looked and cool down).
- Colour, size, line thickness, softness, glow, brightness, blending (light or paint), solidity, fill and a dark outline
  for bright backgrounds.
- **Presets** (Subtle, Crisp, Soft band, Neon), **Reset all**, and three slots of your own under **Mine** to save and
  compare looks.

### Tail

![Tail page](docs/page-tail.png)

The teardrop tail of the Ghost style and the trails of the other styles: strength, lag, longest tail, heatmap warm-up and
cool-down, and whether the tail is anchored to **the world** (recommended in VR: looking around by turning your head
stretches it too) or to the screen.

### Motion

![Motion page](docs/page-motion.png)

How the ring follows your eyes. By default a **One-Euro filter**: very steady while you fixate something, instant on eye
jumps. Also: **dwell** (hold your gaze on something and the ring tightens, so viewers can tell reading from glancing),
riding through blinks, fade in and out, and when tracking counts as lost.

### Desktop cover

For headsets that track by looking at the room (inside-out tracking) with a monitor in view: the game's own picture
on that monitor moves with every head movement, which is the last thing such tracking wants to look at. The cover
puts black over it. It is **off until you tick it**.

- **Cover the game's window while in VR:** black over every window of the game that is in VR, on every monitor it
  lies on (a triple-screen window gets three pieces). Only that game's window - never another program, never the
  desktop - and only while the game is the window in front: bring anything else to the front and the cover is gone
  at once; go back to the game and it returns.
- **The game keeps the focus.** The cover cannot be clicked or activated, mouse and keyboard pass through it, and it
  appears in neither Alt+Tab nor the task bar.
- **Hotkey:** Ctrl + Alt + B switches it on and off (click the button to choose another key, Esc for none). On the
  desktop Windows passes the key on. A game that has the keyboard keeps Windows' hotkeys from working - DCS does -
  so inside a game the mirror looks at the key itself, once a frame, and tells the app. The game sees the key too:
  choose one that does no harm there. For SteamVR games the helper does the looking, at full pace while OBS or the
  mirror window shows the picture and twice a second otherwise (hold the key a moment).
- **Screen capture sees the cover:** unticked, the black is on your monitor only - OBS Display Capture, Discord
  screen sharing and screenshots still get the game underneath. Ticked, they get the black too. Window and game
  capture of the game, and the Gaze Mirror source, get the game either way.
- **Tracking pattern (may help):** marks on the black that never move - fixed to the monitor, the same every time -
  as something steady for the headset to hold on to. It may help and it may make no difference; that depends on the
  headset, and nothing is promised. Keep it dim in a dark room and on OLED monitors (a picture that never changes
  can burn in).
- **Limits:** it works while this app is running (minimised is fine) and not for games in exclusive full screen,
  which no other window can cover. Nothing is put into the game.

### Quad Views

![Quad Views page](docs/page-quadviews.png)

Only listed while Quad-Views-Foveated is installed. Its own settings, with the same sliders and value logic as
TallyMouse's *QuadViews Companion* (both can be used on the same file): focus size, vertical offset, foveate and
peripheral resolution, sharpening, transition, Turbo mode and the debug views. **These are read when a game starts:
press Apply, then restart the game.**

- **Render load:** how many pixels per frame your settings make the game render - for the last game start, the saved
  file, and what Apply would give.
- **Presets:** *Give me FPS*, *TM's Favorite*, *QV defaults*, and three slots of your own.
- One deliberate difference from the Companion: values are also written to the **common** part of the file. Headset
  sections such as `[Pimax]` only apply when the OpenXR runtime's name contains that word - a Pimax driven through
  SteamVR matches none of them and silently gets the built-in 35 % focus size otherwise.
- A backup of the file is made before the first change of each session. **File** and **Log** open them.

### About

![About page](docs/page-about.png)

Version, the update check (on opening; optional betas), the changelog, and **Logs** for the folder with the app's, the
layer's and the helper's logs.

## How it works

One core, two producers, any number of readers:

- **The mirror layer** (`XR_APILAYER_NOVENDOR_gaze_mirror`) sits in OpenXR games. Each frame it reads the game's eye
  images, the head pose and - when the runtime gives it - the eye gaze, and makes the mirror picture.
- **The SteamVR helper** (`GazeMirrorHelper.exe`) does the same for SteamVR games from SteamVR's own mirror texture,
  eye tracking and poses. SteamVR starts it with itself, and it leaves when SteamVR closes.
- **The core** they share does everything in a single pass straight from the game's image: crop, gaze-following, picture
  steadying, scaling and the ring. The result goes into a shared texture that the **OBS plugin** and the **mirror
  window** read - both at once, if you like.
- **The settings app** writes the settings file and bumps a counter in shared memory; the producers pick the change up
  on the next frame (a memory read, no system call). With the app closed, nothing is ever re-read.
- **The desktop cover** is a set of ordinary windows of the settings app, placed over the game's window. A thread of
  its own sleeps until Windows reports another window in front or the game's window moving, or until a producer
  announces that the game in VR has changed. Switched off, it watches nothing.

**Nothing polls.** With no reader - OBS not showing the source, mirror window closed - no mirror work is done. The
helper sleeps on an event until a reader turns up. Keys are only looked at while the feature that uses them is on. The
app listens for file-change notifications rather than checking on a timer.

**The installer** is a plain, declarative MSI: no code of ours runs during setup, and it never touches other products'
registry entries. The settings app never runs as administrator. The one exception - switching or reordering layers on the
Status page - is done by Windows' own `reg.exe` after a UAC prompt.

Where things are:

| What | Where |
|---|---|
| Program, layer, helper, mirror window | `C:\Program Files\VR-Gaze-Mirror` |
| Ring, crop, follow, steadying, gaze settings | `%LocalAppData%\XR_APILAYER_NOVENDOR_OBSMirror_gaze.cfg` |
| App preferences, saved slots, crop profiles and pictures, layer-order backups | `%LocalAppData%\GazeMirror\` |
| Logs: `app.log`, `gaze-mirror-layer.log`, `gaze-mirror-helper.log` | `%LocalAppData%\GazeMirror\` |
| Quad-Views-Foveated settings and log (if installed) | `%LocalAppData%\Quad-Views-Foveated\` |

## Troubleshooting

- **Black Gaze Mirror source in OBS** - no VR session is running yet, or the game is one the layer cannot mirror (an
  OpenXR game on Vulkan or OpenGL). The Status page's top line says what is being mirrored.
- **No ring** - eye tracking is off in the headset software, the software does not feed the standard interface (see
  Gaze source above), or the ring is switched off on the Ring page. The Status page says whether gaze arrives.
- **No ring in games that run through SteamVR** (SteamVR games, and OpenXR games with SteamVR as the runtime) - their
  gaze comes from the headset's SteamVR driver, and some drivers pass none on (Pimax's own has no eye tracking in
  it). The helper then asks the headset's OpenXR runtime instead (see Gaze source). That needs the headset's own
  OpenXR runtime to be installed and the headset's software to be running; it need not be the active runtime. The
  helper's log says which runtimes it found and which one answered. Eye-tracking software that takes the eye cameras
  for itself (SRanibro) takes the gaze away from the runtime too.
- **Ring beside what you look at** - Gaze page, calibration marker.
- **Ring stops short in VRChat** - you are on the VRChat fallback and the eye-tracking software caps the gaze it sends;
  see the known limit under Gaze source.
- **The crop does nothing** - press Ctrl+F once on the OBS source.
- **Quad Views changes do nothing** - press Apply and restart the game; check the Render load line after the next start.
- **A layer is red or the order is wrong** - Status page > Fix order, or run the installer again > Repair.
- **The mirror window takes ten seconds to appear the first time** - antivirus software looking at a new, unsigned
  program.
- **Discord does not list the mirror window** - tick "Title bar".
- **Something odd** - About page > Logs; the three log files say what each part did.

## Building from source

Needs Visual Studio 2022 (C++ workload), the .NET 10 SDK and the GitHub CLI (`gh`, for fetching third-party files the
first time).

```powershell
git clone <this repo>
.\v2\Get-External.ps1              # once: OpenVR SDK, OBS headers, the Quad-Views-Foveated installer (only licences are kept in git)
.\Build-Installer.ps1 -Version 2.0.0   # everything, the offline layer test, and dist\VR-Gaze-Mirror-2.0.0.msi with its .sha256
```

| Path | What |
|---|---|
| `v2/core/` | The shared core: settings, crop and framing, steadying, ring, renderer, publisher |
| `v2/layer/` | The OpenXR API layer |
| `v2/openvr-helper/` | The SteamVR helper: OpenVR mirror, VRChat OSC, the calibration, the in-headset marker |
| `v2/obs-plugin/` | The "Gaze Mirror" OBS source |
| `v2/mirror-window/` | The capturable mirror window |
| `v2/viewer/` | A small viewer of the shared picture, for development |
| `v2/test/` | The offline layer test: plays game and runtime, checks the picture, the crop tool and the marker |
| `v2/protocol/` | The shared-memory blocks every part agrees on |
| `GazeOverlayApp/` | The settings app (WPF, .NET 10) |
| `Installer/` | WiX 5 project for the MSI |
| `tools/` | The logo and installer-artwork generators, an offline ring preview, an MSI inspector, a clip reviewer |

The app has two headless checks: `GazeMirror.exe --selftest <folder>` and `--screenshots <folder> [width height]`.

**Every release - beta or not - must have its own `x.y.z` number** (tag `v2.0.0`, `v2.0.1-beta`, ...). Windows Installer
only upgrades to a higher number, and the update check compares the numbers, so a beta and its final release cannot share
one. Betas are GitHub *pre-releases* and are only announced to people who ticked "Also tell me about beta versions".
Every release must carry both the `.msi` and its `.msi.sha256`; the in-app updater refuses a release without the checksum.

## Things to know

- **Nothing is code-signed.** Windows SmartScreen warns about the installer, and games with anti-cheat may refuse to load
  the layer.
- Setup is a plain MSI on purpose: an earlier self-installing `.exe` was quarantined by antivirus heuristics half-way
  through an install.
- Silent install without the desktop shortcut: `msiexec /i VR-Gaze-Mirror-x.y.z.msi /qn DESKTOPSHORTCUT=0`.
- OpenXR games are mirrored when they render with Direct3D 11 or Direct3D 12. Vulkan and OpenGL games are not mirrored
  yet.
- Quad views, the four-view foveated mode (Varjo headsets; Pimax Play's own setting), are mirrored as the headset shows
  them: the sharp inner image over the wide one. Quad-Views-Foveated itself handles Direct3D 11 games only: switch it
  off on the Status page for a Direct3D 12 game, or the headset may stay black.
  SteamVR games are mirrored through SteamVR's own mirror, whatever they render with.
- The SteamVR helper needs SteamVR and OBS on the same graphics card.
- TallyMouse's QuadViews Companion keeps working on the same settings file; only its "QV Defaults" button fails, because
  it looks for the original product's install folder (use the *QV defaults* preset here instead).

## Credits and licence

- [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated) by Matthieu Bucchianeri (MIT): its official,
  unmodified 1.1.3 installer is bundled as an optional extra.
- [OpenXR-Layer-OBSMirror](https://github.com/Jabbah/OpenXR-Layer-OBSMirror) by Jabbah (MIT): the 1.x versions of this
  project were built on it, and the 2.0 layer keeps its settings file name so upgrades carry over.
- [OBS Studio](https://github.com/obsproject/obs-studio) (GPL-2): the OBS plugin is built against its headers and is
  itself GPL-2; its licence is installed next to it.
- [OpenVR](https://github.com/ValveSoftware/openvr) by Valve (BSD-3): the SteamVR helper's library, installed next to it.
- The layer-order rules on the Status page express the same facts as
  [OpenXR-API-Layers-GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI) by Fred Emmott (ISC).
- The Quad Views page follows the slider logic of TallyMouse's QuadViews Companion; it is not their app.
- The ring was inspired by Tobii Ghost.
- Thanks to Challenger0303, SRanibro's author, for adding the raw-gaze OSC channel that lets SRanibro talk to the helper
  directly.

MIT - see [LICENSE](LICENSE). Third-party licence files are installed under `Licences` next to the program.
