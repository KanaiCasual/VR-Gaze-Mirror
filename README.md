<p align="center"><img src="docs/logo.png" width="96" alt="VR Gaze Mirror logo"></p>

# VR Gaze Mirror

[![Latest release](https://img.shields.io/github/v/release/KanaiCasual/VR-Gaze-Mirror?label=latest%20release&color=4DB2FF)](../../releases/latest)
[![Released](https://img.shields.io/github/release-date/KanaiCasual/VR-Gaze-Mirror?label=released)](../../releases/latest)
[![Downloads, all versions](https://img.shields.io/github/downloads/KanaiCasual/VR-Gaze-Mirror/total?label=downloads%20%28all%20versions%29)](../../releases)
[![Downloads, latest version](https://img.shields.io/github/downloads/KanaiCasual/VR-Gaze-Mirror/latest/total?label=downloads%20%28latest%29)](../../releases/latest)
[![Open issues](https://img.shields.io/github/issues/KanaiCasual/VR-Gaze-Mirror)](../../issues)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-green)](LICENSE)

**A clean mirror of your VR headset for OBS and streaming. OpenXR and SteamVR games, one source, no setup.**
If your headset tracks your eyes, a ring on the mirror shows your viewers where you look.

Install it, add the source in OBS, play. You get one eye, cropped and steadied, at the size and rate you choose. The
headset shows none of it, the game stays untouched, and you have no window to keep on top.

You set the framing on a picture of your own view: press **Capture from headset**, put the headset on, press a key (or
let the countdown do it), and the crop box sits on the real image. **View edges** marks the part of that image
your eye can see, so you place the box on the picture itself and skip the rounds of headset on, headset off, nudge a
crop slider.

https://github.com/user-attachments/assets/97e8f727-dba7-481a-b9ef-01490a8ec5f4

*Your viewers see your view as you see it, framed the way you set it. With eye tracking they also get the ring that
follows your eyes, with a tail that shows where they came from. You see none of it in the headset.*

[![Watch on YouTube](https://img.shields.io/badge/Watch%20on-YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/watch?v=ixilzm8F_SY)

One installer sets up all of it:

- **The mirror layer**: an OpenXR API layer that copies the picture your headset shows in OpenXR games (DCS World,
  Assetto Corsa EVO, VAIL and the like; Direct3D 11 and 12, quad views included), then crops, frames and steadies it
  in a single pass.
- **The SteamVR helper**: the same for games that use SteamVR directly (VRChat, Half-Life: Alyx, Pavlov and the like).
  It starts and stops with SteamVR by itself.
- **The "Gaze Mirror" OBS source**: reads the picture from the graphics card, with no window capture in between. Or use
  the **mirror window** with any program that captures windows (OBS, Discord).
- **A settings app**: the framing tools. A crop box you set on a picture of your own view, taken from inside the
  headset; an edge finder that marks the part your eye can see; gaze-following and steadying; per-game crop profiles;
  picture size and rate caps. You set all of it live, with no file to edit by hand.
- **The gaze ring**: with any eye-tracked headset that feeds the standard interfaces, or through VRChat's avatar eyes
  as a fallback. The mirror shows it and the headset does not. It is optional: the mirror works without it.
- **Quad-Views-Foveated, optional**: the installer bundles Matthieu Bucchianeri's official installer and offers it at
  the end of setup, for eye-tracked foveated rendering in OpenXR games. The ring works without it.

*Up to version 1.2.2 the project was "QuadViews Gaze Mirror", and the ring sat on modified Quad-Views-Foveated and
OpenXR-Layer-OBSMirror layers. Since 2.0 this project's own code makes the whole picture, and Quad-Views-Foveated is an
optional extra.*

**Download:** the latest `VR-Gaze-Mirror-<version>.msi` is on the
[Releases page](../../releases/latest); the release notes list the changes.

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
- A VR headset. **Eye tracking is optional:** the mirror and the crop tools work without it, minus the ring. For the
  ring, the headset's software has to feed its eye tracking to the standard interface the game uses: OpenXR's eye-gaze
  extension for OpenXR games, SteamVR's eye tracking for SteamVR games (Pimax Crystal, Varjo, Quest Pro over Virtual
  Desktop, Vive Pro Eye). Software that feeds VRChat alone works too, through the ring's fallback.
- **OpenXR games** that render with **Direct3D 11 or Direct3D 12** (DCS World, Assetto Corsa EVO, VAIL), or
  **SteamVR games** (VRChat, Half-Life: Alyx) with any renderer.
- For recording through the plugin: **OBS Studio**. The mirror window needs nothing else.

The project's test rig is a Pimax Crystal Super, run through SteamVR and through Pimax's own OpenXR runtime, with DCS
World, VRChat, Assetto Corsa EVO, VAIL and Half-Life: Alyx.

## Installing, updating, removing

1. Close your game and OBS, then run the `.msi`. Windows SmartScreen warns, because the files carry no code signature:
   "More info" > "Run anyway". A first install offers a desktop shortcut.
2. The last page offers to install **Quad-Views-Foveated 1.1.3** if the PC lacks it. Say yes if you want foveated
   rendering in OpenXR games; the mirror works either way.
3. Start **VR Gaze Mirror** from the Start menu. The **Status** page should show green dots for the layer, the OBS
   plugin and the SteamVR helper.

**Coming from 1.x:** run the installer over it. It removes the two modified layers and the old "OpenXR Mirror Capture"
OBS plugin that 1.x put into OBS, carries over your settings, presets and crop pictures, and cleans up the old
"QuadViews Gaze Mirror" data folder. Replace the old source in your OBS scenes with the new **Gaze Mirror** source (the
old one shows black).

**Updating:** the app tells you about a newer release. **Download and install** saves the new installer into your
Downloads folder, checks it against the SHA-256 the release publishes, and opens it with Windows Installer; your
settings stay. You can also run the newer `.msi` over the old one yourself. **Repair / remove:** run the installer again
(or "Modify" in the installed-apps list) for a page with **Repair** and **Uninstall**. Uninstalling leaves your settings
files in place, and SteamVR forgets the helper.

## Recording and streaming: two ways

Both show the same picture: the eye you chose, cropped, framed and steadied as you set it on the **Mirror** page, with
the gaze ring on it if your headset gives gaze. You can use both at once.

### 1. OBS with the Gaze Mirror source: cheapest, best for recording

The installer puts the plugin into OBS.

1. In OBS: **Sources > + > Gaze Mirror**. Its framing settings live on the app's Mirror page.
2. Start your game. The picture appears with the VR session.
3. Select the source and press **Ctrl+F** (Transform > Fit to screen) once. From then on it keeps fitting your canvas
   at any crop box size.

OBS reads the picture from the graphics card. While OBS does not show the source, the mirror does no work.

### 2. The mirror window: for Discord and other programs that capture windows

This way needs no OBS plugin. On the **Mirror** page press **Open**: a window with the mirror picture appears on the
monitor you chose, **behind the other windows** (behind a borderless game too), and it leaves the focus alone. Window
capture still sees it there.

- **Discord:** Share Your Screen > Applications > *VR Gaze Mirror - Mirror*.
- **OBS without the plugin:** Sources > + > **Window Capture** > that window.
- **Size** and **Rate** decide what the capturing program gets (for example 1920 x 1080 at 30 fps for Discord). Smaller
  and slower is cheaper. This way costs more graphics card time than the plugin: the window redraws the picture and the
  capturing program copies it again.
- **Title bar** for capture tools that list windows with a title bar and skip the rest.

## The settings app, page by page

Each page opens with a card that explains what its parts do and how to use them. The card is open on your first visit
to a page and folded from then on; a click on its heading opens or folds it, and the app keeps that choice. Each setting
also has an **(i)** with its own explanation. On a slider, click the value to type one and double-click the slider for
its default. Changes save by themselves and **reach a running game at once** (the Quad Views page is the exception; see
there); a mark at the foot of the page list says when the app last saved. The app is small on purpose, to sit next to
OBS while you tune, and the keyboard reaches all of it.

The page list follows the program's purpose: **Status** and **Mirror** first, and most people need no more than those.
Then come the two pages of the **gaze ring** (Gaze and Ring), the **extras** (Desktop cover, and Quad Views
while Quad-Views-Foveated is installed), and About. Each page has an icon, and a narrow window shows the icons alone. A
green dot on Status means the mirror shows a game right now.

### Status

![Status page](docs/page-status.png)

- **Three steps to your mirror:** add the source in OBS, start your VR game, frame the picture. Each step ticks
  itself off once the app has seen it happen, and the card goes away after the third (or at **Hide**).
- **Parts:** the mirror layer, the OBS plugin, the SteamVR helper and (optional) Quad-Views-Foveated. While all is
  well the card says so in one line, and **Details** lists the parts; with a fault it lists them at once, each with
  what to do about it. The line at the top names the game the mirror shows and says whether gaze arrives.
- **OpenXR API layers:** folded away while the order is fine; an order problem opens it. It lists the layers on your
  PC in the order games load them, with a tick box to switch each on or off and arrows to **reorder**. Windows asks
  for permission for that one change; the app itself runs without administrator rights. The app knows the ordering
  rules of common layers and offers **Fix order** for an order that breaks one.

### Mirror

![Mirror page](docs/page-mirror-2-1.png)

*Blue frame = the recorded part; blue tint = the reach of the gaze follow; amber lines = still zone; green band = room for steadying.*

The picture takes most of the page. Above it sit the buttons that take a picture and choose what is drawn over it:
**Refresh picture**, **Capture from headset** with an arrow for its key and countdown, and **Show**. Beside it (under it in a narrow
window) stand the crop box, the view edges, the picture settings, and three sections that fold away: Follow my gaze,
Steady the picture, Mirror window.

The **Show** button lists the aids drawn over the picture, each with a switch: the crop box, a small cross at the **box's
own centre**, the follow reach, the steadying room, the view edges, a **centre** mark, and the **gaze** ring as it
was in the picture. By default the picture carries the crop box and the centre mark; the follow reach and the
steadying room show while their sections are open, and the view edges while the pointer is over their card. With a
picture of each eye, the list also chooses the one behind the box. The centre mark is a dot with a line across and a line down the whole image, at the point where
**straight ahead** lands in the eye image. That point comes from the headset's own projection with each picture, so it
fits your headset; on most headsets it sits away from the image's middle, which a faint small cross marks. Line the
box's cross up with the centre mark, or with the view edges, and you centre the framing on your view.

**Source** (for OpenXR games that run through SteamVR): **Through SteamVR**, the default, has the SteamVR helper take
the picture SteamVR shows in the headset, so SteamVR's overlays (OVR Toolkit, fpsVR, the dashboard and its desktop
view, notifications) and the app's own aids in the headset reach the stream, as they do in SteamVR games. **The game's
own** has the mirror layer take the picture inside the game, before any overlay. It is saved with the crop profile, so
one game can stay clean. Native OpenXR runtimes always give the game's own picture.

**Picture:** the mirrored **eye** (right by default), the **picture size** cap (3840 on the longest side by default,
so a 5000-pixel eye image reaches OBS scaled down) and the **picture rate** cap. These apply to OBS and the mirror
window alike.

**Crop profiles** (top of the page): save a framing under a name and give it a game's program file (DCS.exe,
VRChat.exe), and the mirror applies it at each start of that game. Edits go into the chosen profile.

**Mirror window:** Open / Close, the monitor (or an ordinary window), size, rate and the title-bar option.

**Start-up** (About page, both off unless you tick them): **Start with Windows** opens the app when you sign in, through
an entry in your own account's start-up list, no administrator rights. **Start in the tray** keeps it out of the way:
the app opens hidden in the notification area, the window's close button hides it, and the tray icon's menu opens it
again or quits. The desktop cover's hotkey and the mirror window work from the tray.

**Crop the picture:** drag a box on a picture of the whole eye image and skip the crop percentages in OBS. A shape
locks the box (16:9, 9:16, 1:1, 4:3, 4:5, 21:9 or free). OBS and the mirror window get **that box**. Drag inside the
box to move it, a corner or the mouse wheel to resize; **Centre**, **Largest**, and **Lock** against stray clicks.
With the picture selected, the arrow keys move the box and + and - resize it (Shift for larger steps).

**View edges:** the headset draws more than your eye can see through the lens, and the crop should cover the part
you see. Press **Measure in headset** with a game running and the headset on: the headset goes black, and a white block
flashes once a second on the left of the mirrored eye's view. That block is the part your eye should not see. Push it outwards
with the arrow keys (hold to keep moving, Shift for bigger steps) until it is gone from your view, press **Enter**, and
the next side comes: right, top, bottom, each a white block while you set it and a grey one once set. **Backspace** goes
back a side, **Esc** cancels. The app reads the keys and takes the focus as the measuring starts, so the game gets none
of them. The crop picture shows the result as a dashed box, and **Crop to view edges** makes the crop box the largest
one of the chosen shape inside it.

**The picture behind the box:**

- **Refresh picture** takes one from the running game.
- **Capture from headset** takes it the way you sit in play: press it (a large banner says what to do), put the headset
  on, look straight ahead and press the key. The key is **Pause** by default; the arrow next to the button changes it. You hear a
  sound, and the capture switches itself off. It takes one picture of **each eye**, and the app **keeps** that picture,
  through restarts too, until you press Refresh picture. The layer looks at the key while the capture is armed and at
  no other time. If no key reaches the game, or your hands are on the controls, choose **By itself after 5 / 10 / 15 /
  20 / 30 s** under the same arrow: the capture then takes the picture as the countdown runs out, or earlier at your
  key press. The headset shows the seconds left, large, in the middle of the view, so you know when to hold still. The
  picture also carries the **ring** at the spot you were looking at, in the ring's size and colour, so a capture shows
  the framing and the gaze together (the Show row switches it off).

**Tilt** (in Steady the picture): what a sideways tilt of your head does to the picture. **Smoothed**, the default, takes
quick tilts out with the steadying; it shares the steadying's room, so a slow lean still comes through. **Level** keeps
the horizon level: the crop box turns against your whole tilt, steadying on or off. A large box has little room to
turn, and it stops at the edge of the eye image; **Show > Turn area** draws the area it sweeps, a circle round the box
stretched along the follow's travel. **Follows the head** leaves the tilt in.

**Follow my gaze (up / down):** a 16:9 box covers about half the height of the eye image, so a look down at your knees
or up at the canopy leaves the frame. With this on, the box glides up or down far enough to keep your target in frame,
like a camera operator.

- **Still zone** (dashed amber lines): the middle part of the box, where your gaze moves nothing.
- **Reach** (blue tint): the distance the box may travel. Two dotted lines mark where the frame **ends when you look
  all the way up** and **starts when you look all the way down**.
- **Glide**, **Return home** and **Return after**: the softness of the movement, and whether and when the box goes back.

**Steady the picture:** takes small head movement and tracking jitter out of the picture. The head orientation runs
through a One-Euro filter and the box counter-moves the difference.

- It adds **no delay**: the mirror holds back no frames and compares none. It uses the head position the game rendered
  with.
- **Steadiness** (further right = steadier, but the picture trails further behind a head turn), **Follow quick turns**, and
  **Room to move**, shown as a green band around the box (and around the blue reach). The box stays out of that band
  and therefore inside the image; switching steadying on moves or shrinks the box to make room.
- It needs the crop on. It corrects turning and nodding by moving the box, and with **Steady the tilt too** a
  sideways tilt of the head by turning the box about its middle. The turn shares the room with the movement: it
  steadies a small tilt and eases after a large one.

Cropping, following, steadying and the ring happen in the one pass that makes the picture, so they cost no extra pass.

### Gaze

The first of the ring's pages: the source of the ring's gaze, and the spot where the ring lands in the picture. If the
mirror is all you want, you can skip the pages below the **Gaze ring** heading in the app.

**Gaze source:** the ring's main source is the headset itself: OpenXR's eye-gaze extension in OpenXR games, SteamVR's
eye tracking in SteamVR games. Exact angles and no setup, and while that gaze arrives the ring uses no other.

The helper has **two ways to the headset's gaze**. The first is SteamVR's own eye tracking, which the headset's SteamVR
driver feeds. If that driver passes none on, or passes on a direction no eye can look in, for two seconds, the helper
asks the headset's OpenXR runtime through the standard eye-gaze interface. It uses a session that it creates and leaves
unbegun, so the session draws nothing: the headset shows no change and the game stays untouched. The helper takes the
PC's active OpenXR runtime or, if that is SteamVR's own, the installed runtime that belongs to the headset, so the
choice of active runtime does not matter. Either way you get the headset's own gaze: exact angles, full range, no
calibration, no other software.

The runtime the game runs on comes first, frame by frame. The moment its gaze is back, the ring follows that gaze again,
and after two seconds of it the helper closes its session on the other runtime. It opens one again only after two more
seconds without. So a driver that reports the gaze to SteamVR itself is the one source in use, and the other runtime is
asked only while that driver gives none.

OpenXR games that run **through SteamVR's runtime** are in the same position as SteamVR games: their gaze, too, is the
gaze the SteamVR driver passes on. For those the helper, which runs next to SteamVR anyway, hands the headset's gaze to
the layer inside the game while the mirror shows that game and the game has no gaze of its own.

The **VRChat fallback** covers the case that the headset's gaze does not arrive. Some eye-tracking software takes the
eye cameras for itself (SRanibro), or feeds VRChat and nothing else (EyeTrackVR, Quest Pro over ALVR). The headset's
own gaze is then gone, and the one trace of where you look is the VRChat avatar's eye parameters, driven through
[VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking). For those, the SteamVR helper is an **OSCQuery
service**: VRChat finds it on the machine and sends it the avatar's parameters, and the eye ones drive the ring. You
install and set up nothing. OSC has to be on in VRChat (face-tracking users have it on) and the avatar needs eye
parameters (face-tracking avatars have them). The Gaze source card says *VRChat: eye parameters arriving* once it
works, and "Headset, else VRChat" (the default) switches to the fallback by itself while the headset gives no gaze.

The eye-tracking software decides whether you need the fallback. On a Pimax Crystal, for example, SRanibro stops the
Tobii service to take the eye cameras for itself, so the headset's own gaze is gone and the ring falls back.
[BrokenEye](https://github.com/ghostiam/BrokenEye) with the SRanipal model reads the same cameras through the Tobii
service, the headset's gaze stays available, and the ring runs without the fallback: full reach, no calibration, exact
angles.

**Fixed centre** is for headsets without eye tracking. The ring stays where straight ahead lands in each eye (the
centre mark on the Mirror page's picture). Quad-Views-Foveated puts its sharp area at that same spot when it has no eye
tracking, and with it the mirror draws the ring the size of that area (its fixed sections and offsets, read from its
settings), so viewers see where the picture is sharp. Without it, or in a game that used no quad views, the ring keeps
its size from the Ring page. The app greys the choice out once it knows the headset gives gaze.

**Raw gaze over OSC** skips the fallback. Any program that has the tracker's gaze can send it to the helper's **gaze
input port** (127.0.0.1, port 9006 by default, a box on the Gaze source card): one OSC message per eye whose address
ends in `/left` or `/right`, with the unit direction x y z in head space (x right, y up, forward -z) and, if you
have them, valid 1 or 0, openness 0 to 1 and a timestamp. SRanibro (a build newer than 0.1.9-Beta) sends exactly that when you tick **Raw gaze
OSC** in its settings; a driver of your own can send the same. The Gaze source card then says *Raw gaze: arriving*, and
the ring gets a direction from the tracker itself, with no calibration, in each game, VRChat and DCS alike. The helper
takes it ahead of the VRChat fallback while it arrives. SRanibro's channel carries the same range cap as its VRChat
output (see below).

**Calibrate VRChat gaze** (for the fallback; the headset's own eye tracking needs none): the avatar's eye values are no
angles and do not scale with them, so the helper measures once how they relate to where you look. Press the button with
the headset on and VRChat running: a small ring-and-dot target appears inside the headset at twenty places, about two
seconds each: straight ahead, then out to 35 degrees to each side, 28 up and 35 down, then the four corners. Follow it
with your eyes and keep your head still; the status line counts the targets. After about 45 seconds the target
disappears and the ring lands where you look. The helper trims the reach to what both eyes of your headset can show, so
a narrower headset gets the same pattern pulled in a little; the status line states the reach it used. Redo it after a
change of eye-tracking software or of that software's own calibration; **Forget** drops it.

**Thorough** does the same with a moving target: it glides over the whole reach, rests on 25 spots on the way, and you
follow it with your eyes. The helper records a few thousand samples where the quick run takes twenty. It measures the
delay between the target and the avatar's eyes (the eyes trail a moving target, and VRChat's path adds its own) and
takes it out, then fits a finer map: a grid over the whole field where the quick run fits a curve per axis. About two
minutes. The status line then gives the sample count, the delay it found and the fit of the map.

**Known limit of the fallback:** the ring reaches as far as the eye-tracking software lets the avatar's eyes go.
**SRanibro caps the gaze it sends to VRChat at about 30 degrees in each direction** (0.1.9-Beta; a coming build raises
it to about 40) on the headsets it serves (Pimax Crystal/Super, StarVR One, Varjo); past that the ring holds at the edge
of the range while your eyes carry on. The cap sits in SRanibro's output, outside this project.

**Where the ring lands:** most setups need no change here. If the ring sits beside your target:

- Switch on **"Show a marker inside the headset (calibration)"**: a small bracket reticle appears *in the headset* at
  the ring's place on the mirror. Look at something small and nudge with the sliders, or in the game with
  **Ctrl+Alt+arrow keys** (Shift = bigger steps; these keys exist while the marker is on). Switch it off when you are
  done; this page holds its one switch. In OpenXR games the layer draws it into the picture; in SteamVR games the
  helper shows it as a small SteamVR overlay two metres out along your line of sight.
- **Usual viewing distance** sets the distance the mirror uses to place the ring (a cockpit panel is close, the world
  is far): the mirror shows one eye, and the two eyes see close things in different places.
- **Ignore Quad-Views-Foveated's focus offsets** appears with the **Legacy** placement method, the one it belongs to.

### Ring

![Ring page](docs/page-ring.png)

The gaze ring: its look, its tail and its motion, with a **live preview** over cockpit, terrain, sky or black. The
preview stands beside the settings of the two ring pages; in a narrow window it becomes a strip above them, and
**Hide** puts it away.

For most people a preset and the first card are all the ring needs. The rest folds away in three sections, and the app
remembers the ones you leave open.

- **Presets** (Subtle, Crisp, Soft band, Neon), **Reset all**, and three slots of your own under **My presets** to save and
  compare looks.
- **Ring** (the first card): on or off, the style, the colour and the size. **Styles:** Ghost ring with teardrop tail
  (recommended), plain ring, soft glow, small dot, spotlight (darkens the rest of the picture), bubble and solid blob
  with a fading trail, and a **heatmap** (blue = glance, red = stare; spots stay where you looked and cool down).
- **More of the look:** line thickness, softness, glow, brightness, blending (light or paint), solidity, fill and a dark
  outline for bright backgrounds.
- **Tail:** the teardrop tail of the Ghost style and the trails of the other styles: strength, lag, longest tail,
  heatmap warm-up and cool-down, and the tail's anchor: **the world** (recommended in VR: a turn of your head stretches
  the tail too) or the screen.
- **Motion:** the way the ring follows your eyes. The default is a **One-Euro filter**: steady while you fixate
  something, instant on eye jumps. Also: **dwell** (hold your gaze on something and the ring tightens, so viewers can
  tell reading from glancing), riding through blinks, fade in and out, and the point at which tracking counts as lost.

### Desktop cover

For headsets with camera-based tracking (inside-out, also called SLAM) and a monitor in view.

Such a headset holds its place in the room by anchors: fixed points its cameras pick out around you, such as corners,
edges and patterns on walls and furniture. It works out how your head moved from the way those points shift between
one camera picture and the next. For that it needs points that stand still.

A monitor that shows the game gives it points that move. The picture is bright and full of sharp detail, so the
cameras find plenty of points in it, and it turns as you turn your head. If the headset takes those points for anchors,
it places your head wrong: the view can drift, shake or jump. The effect grows with the share of the cameras' view the
monitor fills: close to the screen in a racing or flight rig, with several monitors, or in a dim room where the monitor
is the brightest thing in sight.

The cover puts black over the game's window, so the cameras find no moving points there and keep to the room. A
headset tracked by base stations (Lighthouse) does not look at the room and has no use for it. The cover is **off until
you tick it**, and the page states this reason at its top.

- **Cover the game's window while in VR:** black over each window of the game in VR, on each monitor it lies on (a
  triple-screen window gets three pieces). The cover touches that game's window and leaves other programs and the
  desktop alone. It stays while the game is the window in front: bring another window to the front and the cover goes
  at once; go back to the game and it returns.
- **The game keeps the focus.** You cannot click or activate the cover, mouse and keyboard pass through it, and neither
  Alt+Tab nor the task bar lists it.
- **Hotkey:** LCtrl + LAlt + B switches it on and off. To change it, click the button and press your own combination: a
  key with any of Ctrl, Alt, Shift and the Windows key held, or a function key, Pause or Scroll Lock on its own. A
  letter or digit needs Ctrl, Alt or the Windows key with it, or typing would switch the cover. Esc for none. The
  hotkey remembers the key of each modifier you hold, and the button shows it: LCtrl is the left Ctrl key, RCtrl the
  right one, and LCtrl + RCtrl means both together. So RCtrl + B and LCtrl + B are two hotkeys. On the desktop Windows
  passes the key on. It cannot tell the sides apart, so there it takes the combination from other programs for both
  sides and the app checks the side. A game that has the keyboard stops Windows' hotkeys (DCS does), so inside a game
  the mirror looks at the key itself, once a frame, and tells the app. The game sees the key too: choose one that does
  no harm there. For SteamVR games the helper does the looking, at full pace while OBS or the mirror window shows the
  picture and twice a second otherwise (hold the key a moment).
- **Screen capture sees the cover:** unticked, the black is on your monitor alone: OBS Display Capture, Discord screen
  sharing and screenshots still get the game underneath. Ticked, they get the black too. Window and game capture of
  the game, and the Gaze Mirror source, get the game either way.
- **Tracking pattern (may help):** marks on the black that stay put, fixed to the monitor and the same each time, as
  something steady for the headset to hold on to. A slider sets their **size** (50 to 300 %), a list their brightness.
  It may help and it may make no difference; that depends on the headset, and this project promises nothing. Keep it dim in a dark room and on OLED monitors (a still picture can
  burn in).
- **Limits:** it works while this app runs (minimised is fine). A game in exclusive full screen stays uncovered,
  because no other window can sit on top of it. The cover puts nothing into the game.

### Quad Views

![Quad Views page](docs/page-quadviews.png)

The app lists this page while Quad-Views-Foveated is installed. It holds Quad-Views-Foveated's own settings, with the
same sliders and value logic as TallyMouse's *QuadViews Companion* (you can use both on the same file): focus size,
vertical offset, foveate and peripheral resolution, sharpening, transition, Turbo mode and the debug views. **A game
reads these at its start: press Apply, then restart the game.**

- **Render load:** the pixels per frame your settings make the game render, for the last game start, the saved file,
  and the result of Apply.
- **Presets:** *Give me FPS*, *TM's Favorite*, *QV defaults*, and three slots of your own.
- One deliberate difference from the Companion: Apply writes the **whole file fresh**, in the app's own layout. Every
  setting stands once at the top, which every runtime and game reads, grouped into with eye tracking, without it, and
  the rest. Headset sections such as `[Pimax]` apply only when the OpenXR runtime's name contains that word, so a Pimax
  driven through SteamVR matches none of them; the app writes no headset or per-game values of its settings, and the
  page's values count everywhere. Below the top part stay only Turbo off for SteamVR and Varjo, and lines the page has
  no setting for (un-advertise). The page lists what stays set apart under **Set apart in the file**.
- The app backs the file up before the first change of each session. **File** and **Log** open them.

### About

![About page](docs/page-about.png)

Version, the update check (as the app opens; betas optional), the changelog with the older versions folded, and
**Logs** for the folder with the app's, the layer's and the helper's logs. **Settings file** opens the file of the ring
and the mirror, and **Reload settings** reads it again.

## How it works

One core, two producers, any number of readers:

- **The mirror layer** (`XR_APILAYER_NOVENDOR_gaze_mirror`) sits in OpenXR games. Each frame it reads the game's eye
  images, the head pose and (if the runtime gives it) the eye gaze, and makes the mirror picture.
- **The SteamVR helper** (`GazeMirrorHelper.exe`) does the same for SteamVR games from SteamVR's own mirror texture,
  eye tracking and poses. SteamVR starts it with itself, and it leaves as SteamVR closes.
- **The core** they share does its work in a single pass on the game's image: crop, gaze-following, picture steadying,
  scaling and the ring. The result goes into a shared texture that the **OBS plugin** and the **mirror window** read,
  both at once if you like.
- **The settings app** writes the settings file and bumps a counter in shared memory; the producers pick the change up
  on the next frame (a memory read, no system call). With the app closed, the producers re-read nothing.
- **The desktop cover** is a set of ordinary windows of the settings app, placed over the game's window. A thread of
  its own sleeps until Windows reports another window in front or a move of the game's window, or until a producer
  announces a change of the game in VR. Switched off, it watches nothing.

**The parts wait for signals.** With no reader (OBS not showing the source, mirror window closed) the producers do no
mirror work, and the helper sleeps on an event until a reader turns up. A producer looks at a key while the feature
that uses it is on. The app listens for file-change notifications and runs no timer for them.

**The installer** is a plain, declarative MSI: none of this project's code runs during setup, and setup leaves other
products' registry entries alone. The settings app runs without administrator rights. The one exception, switching or
reordering layers on the Status page, goes through Windows' own `reg.exe` after a UAC prompt.

File locations:

| What | Where |
|---|---|
| Program, layer, helper, mirror window | `C:\Program Files\VR-Gaze-Mirror` |
| Ring, crop, follow, steadying, gaze settings | `%LocalAppData%\XR_APILAYER_NOVENDOR_OBSMirror_gaze.cfg` |
| App preferences, saved slots, crop profiles and pictures, layer-order backups | `%LocalAppData%\GazeMirror\` |
| Logs: `app.log`, `gaze-mirror-layer.log`, `gaze-mirror-helper.log` | `%LocalAppData%\GazeMirror\` |
| Quad-Views-Foveated settings and log (if installed) | `%LocalAppData%\Quad-Views-Foveated\` |

## Troubleshooting

- **Black Gaze Mirror source in OBS**: no VR session is running yet, or the layer cannot mirror the game (an OpenXR
  game on Vulkan or OpenGL). The Status page's top line names the game the mirror shows.
- **No ring**: eye tracking is off in the headset software, the software does not feed the standard interface (see
  Gaze source above), or the ring is off on the Ring page. The Status page says whether gaze arrives.
- **No ring in games that run through SteamVR** (SteamVR games, and OpenXR games with SteamVR as the runtime): their
  gaze comes from the headset's SteamVR driver, and some drivers pass none on (Pimax's own has no eye tracking in
  it). The helper then asks the headset's OpenXR runtime (see Gaze source). That needs the headset's own OpenXR
  runtime installed and the headset's software running; it need not be the active runtime. The helper's log lists the
  runtimes it found and names the one that answered. Eye-tracking software that takes the eye cameras for itself
  (SRanibro) takes the gaze away from the runtime too.
- **Ring beside what you look at**: Gaze page, calibration marker.
- **Ring stops short in VRChat**: you are on the VRChat fallback and the eye-tracking software caps the gaze it sends;
  see the known limit under Gaze source.
- **The crop does nothing**: press Ctrl+F once on the OBS source.
- **Quad Views changes do nothing**: press Apply and restart the game; check the Render load line after the next start.
- **A layer is red or the order is wrong**: Status page > Fix order, or run the installer again > Repair.
- **The mirror window takes ten seconds to appear the first time**: antivirus software is looking at a new, unsigned
  program.
- **Discord does not list the mirror window**: tick "Title bar".
- **Something odd**: About page > Logs; the three log files say what each part did.

## Building from source

Needs Visual Studio 2022 (C++ workload), the .NET 10 SDK and the GitHub CLI (`gh`, to fetch third-party files the
first time).

```powershell
git clone <this repo>
.\v2\Get-External.ps1              # once: OpenVR SDK, OBS headers, the Quad-Views-Foveated installer (git keeps their licences alone)
.\Build-Installer.ps1 -Version 2.0.0   # all parts, the offline layer test, and dist\VR-Gaze-Mirror-2.0.0.msi with its .sha256
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
| `v2/protocol/` | The shared-memory blocks the parts agree on |
| `GazeOverlayApp/` | The settings app (WPF, .NET 10) |
| `Installer/` | WiX 5 project for the MSI |
| `tools/` | The logo and installer-artwork generators, an offline ring preview, an MSI inspector, a clip reviewer |

The app has two headless checks: `GazeMirror.exe --selftest <folder>` and `--screenshots <folder> [width height]`.

**Each release, beta or not, must have its own `x.y.z` number** (tag `v2.0.0`, `v2.0.1-beta`, ...). Windows Installer
upgrades to a higher number and to no other, and the update check compares the numbers, so a beta and its final release
cannot share one. Betas are GitHub *pre-releases*; the app announces them to people who ticked "Also tell me about beta
versions". Each release must carry both the `.msi` and its `.msi.sha256`; the in-app updater refuses a release without
the checksum.

## Things to know

- **The files carry no code signature.** Windows SmartScreen warns about the installer, and games with anti-cheat may
  refuse to load the layer.
- Setup is a plain MSI on purpose: antivirus heuristics quarantined an earlier self-installing `.exe` half-way through
  an install.
- Silent install without the desktop shortcut: `msiexec /i VR-Gaze-Mirror-x.y.z.msi /qn DESKTOPSHORTCUT=0`.
- The mirror shows OpenXR games that render with Direct3D 11 or Direct3D 12. Vulkan and OpenGL games have no mirror
  yet.
- The mirror shows quad views, the four-view foveated mode (Varjo headsets; Pimax Play's own setting), as the headset
  shows them: the sharp inner image over the wide one. Quad-Views-Foveated itself handles Direct3D 11 games alone:
  switch it off on the Status page for a Direct3D 12 game, or the headset may stay black. The helper mirrors SteamVR
  games through SteamVR's own mirror, with any renderer.
- The SteamVR helper needs SteamVR and OBS on the same graphics card.
- TallyMouse's QuadViews Companion keeps working on the same settings file. Its "QV Defaults" button fails, because it
  looks for the original product's install folder (use the *QV defaults* preset here).

## Credits and licence

- [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated) by Matthieu Bucchianeri (MIT): the installer
  bundles its official, unmodified 1.1.3 installer as an optional extra.
- [OpenXR-Layer-OBSMirror](https://github.com/Jabbah/OpenXR-Layer-OBSMirror) by Jabbah (MIT): the 1.x versions of this
  project built on it, and the 2.0 layer keeps its settings file name so upgrades carry over.
- [OBS Studio](https://github.com/obsproject/obs-studio) (GPL-2): the OBS plugin builds against its headers and is
  itself GPL-2; the installer puts its licence next to it.
- [OpenVR](https://github.com/ValveSoftware/openvr) by Valve (BSD-3): the SteamVR helper's library, installed next to it.
- The layer-order rules on the Status page express the same facts as
  [OpenXR-API-Layers-GUI](https://github.com/fredemmott/OpenXR-API-Layers-GUI) by Fred Emmott (ISC).
- The Quad Views page follows the slider logic of TallyMouse's QuadViews Companion; TallyMouse did not make this app.
- Tobii Ghost inspired the ring.
- Thanks to Challenger0303, SRanibro's author, for adding the raw-gaze OSC channel that lets SRanibro talk to the helper
  directly.

MIT: see [LICENSE](LICENSE). The installer puts third-party licence files under `Licences` next to the program.
