# Changelog

## 2.3.0 - not released yet

**New**
- **Desktop cover** (own page, off by default): black over the VR game's window on your monitor while the game is in front. Headsets with camera-based tracking can be thrown off by a monitor whose picture moves with your head. Hotkey LCtrl + LAlt + B or your own, with left and right modifier keys told apart; it works inside games too. A switch decides whether screen capture sees the black, and optional tracking marks may help such a headset hold its place. The cover needs the app running and cannot cover a game in exclusive full screen.
- **Tilt**: a choice in Steady the picture for what a sideways tilt of the head does. Smoothed (the default) takes quick tilts out with the steadying; Level keeps the horizon level through a lean, steadying on or off. The box turns only as far as the eye image reaches and eases off before it gets there, so no black reaches the stream; beyond that the horizon tilts with you. Follows the head leaves the tilt in. With Level, Show > Turn limit draws how far the box can turn.
- **Start with Windows** and **Start in the tray**, on the About page, both off unless ticked.
- **Gaze input port**: the raw gaze channel takes any sender, not only SRanibro. New installs use port 9006; an update from an earlier version keeps 9005, so SRanibro's Raw gaze OSC carries on as set up.

**Changed**
- **Pages**: Status and Mirror; then Gaze and Ring for the ring; then Desktop cover and Quad Views; then About. The Gaze page holds the source, placement and calibration, and names the source in use right now. The Ring page holds the whole ring, with the finer look, the tail and the motion in sections that fold away.
- **Mirror page**: the picture comes first, with its buttons in one row above it and the settings beside it. The picture shows the crop box and the centre mark; the other aids appear while their section is open.
- **View edges** (was Find visible edges) has its own card. Measuring now turns both eyes black and flashes a white block on the side you set, easy to see in any scene.
- **SteamVR overlays in OpenXR games**: an OpenXR game that runs through SteamVR is now mirrored from SteamVR's compositor by the helper, as SteamVR games are, so overlays such as OVR Toolkit, fpsVR or the dashboard reach the stream. **Capture** on the Picture card switches a game to OpenXR capture, the game only, per crop profile.
- Each page opens with a short explanation, folded after your first visit.
- Sliders: click the value to type one, double-click to reset. Every control works by keyboard and has a screen-reader name.
- Smaller touches: page icons, a green dot on Status while the mirror runs, a colour swatch that opens the picker, "My presets", Quad Views' load in plain words, missing parts shown in red, and file buttons that open your own editor for that file type (or the file's folder with it selected).
- **Quad Views**: Apply writes the whole settings file fresh, in the app's own layout: every setting once at the top, where every runtime and game reads it, grouped into with eye tracking, without it, and the rest. Headset and per-game copies of the page's settings are gone, so the sliders count everywhere. Turbo-off guards for SteamVR and Varjo stay, as do lines the page has no setting for, and the page lists what remains set apart.
- Plainer wording throughout the app, the installer and the README.

**Fixed**
- When the headset's own gaze dropped out mid-game, the fallback never took over again. Now the game's runtime comes first, and the fallback steps in after two seconds without it and steps back when it returns.
- The capture countdown in OpenXR games was three times too big on wide-view headsets. It now matches the SteamVR version.

## 2.2.1 - 2026-09-29

- No ring in an OpenXR game that does not sync its input actions (seen in Assetto Corsa EVO through SteamVR's runtime): the layer's eye-gaze action travels with the game's sync, and a game that skips the sync left the action unsynced, which SteamVR's runtime answers with no gaze. After twenty frames without the game's sync the layer syncs its action itself.
- The headset's own gaze from its OpenXR runtime no longer needs that runtime to be the active one: if the active runtime is SteamVR's, the helper goes through the installed runtimes and takes the one that belongs to the headset. It picks by name and leaves a runtime of another headset alone, since loading it may start that headset's software.
- OpenXR games that run through SteamVR's runtime get that gaze too: while the mirror shows such a game and the game has no gaze of its own, the helper hands the headset's gaze to the layer inside the game. It counts as the headset's gaze ("Headset only" includes it).
- While an OpenXR game runs next to SteamVR, the helper stands by without work: it used to try to start twice a second, reading the settings and writing two log lines each time.
- The update check offers a test build ("2.2.0-dev.rev5") the release of its own number: the build keeps that number while it leads up to the release, so the check saw no newer version.

## 2.2.0 - 2026-09-29

- The mirror shows Direct3D 12 games in OpenXR too (tried with Assetto Corsa EVO and VAIL): the layer works on the game's own Direct3D 12 device through Direct3D 11-on-12, so the crop, the steadying, the ring, the capture countdown and the edge finder run the same code as for Direct3D 11 games. The offline test plays a Direct3D 12 game.
- Quad views (the four-view foveated mode of Varjo headsets, which Pimax's runtime offers too): the mirror lays the sharp inner image over the wide one, the way the runtime does in the headset, so the picture is sharp where the game renders sharp and has the full resolution. Before, the mirror showed the wide, coarse image alone. The layer draws the in-headset marker, the countdown and the edge finder into the inner image as well.
- SteamVR games get the headset's own gaze even if its SteamVR driver passes none on (Pimax's own driver has no eye tracking in it): the helper keeps a dormant session with the active OpenXR runtime (created, not begun, drawing nothing) and reads the standard eye-gaze interface from it. Real angles, the full range, no calibration and no other software. The helper uses it once SteamVR has gone two seconds without a gaze that makes sense, if the active runtime is not SteamVR's own; tried with Pimax OpenXR while Half-Life: Alyx ran in SteamVR.
- SteamVR games: the helper leaves out eye tracking that a headset's driver reports as valid but that points where no eye can look (Pimax's own SteamVR driver does: 50 to 70 degrees down and to the side, drifting). The ring then falls back to SRanibro's raw gaze or VRChat's avatar eyes and stays on the picture. The helper judges the direction (within 45 degrees of straight ahead, both eyes agreeing) and believes the source once it has made sense for a second.
- If a game destroys a swapchain in the middle of a session, the in-headset marker lets go of its views of those images at once (it kept them until the session ended).

## 2.1.0 - 2026-09-28

- SRanibro's raw gaze over OSC: a coming SRanibro release (newer than 0.1.9-Beta, which lacks it) can send the tracker's raw gaze to the helper ("Raw gaze OSC" in its settings, 127.0.0.1, port 9005). The helper takes it ahead of the VRChat fallback: a direction, used as it comes, no calibration, in each game while SRanibro runs. "SRanibro raw gaze only" on the Gaze card forces it; the port has a box on the Gaze card. The channel carries SRanibro's own range cap (about 30 degrees today, about 40 in its coming build).
- Capture from headset can take the picture by itself after a countdown (5 to 30 s, chosen next to the key button), for setups where no key reaches the game. The key still works meanwhile. During the countdown the headset shows the seconds left, large, in the middle of the view (drawn into the picture in OpenXR games, as a SteamVR overlay in SteamVR games).
- Find visible edges (Mirror page, Crop box): four bars in the headset. Push each outwards with the arrow keys until it disappears behind the edge of your view; Enter confirms, Backspace goes back. The crop picture shows the result and "Crop to edges" fits the box to it. Works in OpenXR games (drawn into the picture) and SteamVR games (an overlay for the mirrored eye).
- The Mirror page's picture has a Show row: you can show or hide the crop box, the follow reach, the steadying room, the visible edges and a new centre mark. The centre mark (dot, line across, line down) sits where straight ahead lands in the eye image, which each picture now carries from the headset's own projection, so it fits any headset; a faint small cross marks the image's geometric middle.
- Gaze from = Fixed centre, for headsets without eye tracking: the ring stays where straight ahead lands in each eye. With Quad-Views-Foveated, which puts its sharp area at that spot when it has no eye tracking, the mirror draws the ring the size of that area, from its settings (an ellipse of its fixed sections). The app greys the choice out once it knows the headset gives gaze; the mirror's shared block now says whether it does.
- The VRChat calibration reaches further (35 degrees to each side, 28 up and 35 down) and trims that to what both eyes of the headset can show, so it fits any headset; the status line states the reach it used.
- A Thorough VRChat calibration: a moving target glides over the whole reach, resting on 25 spots, and the helper records a few thousand samples. It measures the delay between the target and the avatar's eyes, takes it out, and fits a grid map over the whole field (vrchat_map_grid; the offline test fits made-up eyes). About two minutes; the quick one stays as it was.
- The window title carries the version.
- In OpenXR games the layer drew the capture countdown in the middle of each eye's image. On a headset with the usual asymmetric views those are two directions tens of degrees apart, so the number did not fuse into one. The layer now draws it where a point two metres straight ahead lands in each eye, like the SteamVR overlay.
- The Gaze card's calibration line no longer collapses to one letter a line in a wide window (four cards in a row): the buttons sit under the text and wrap.
- The crop box carries a small cross at its own centre, to line it up with the centre mark or the visible edges (Show: Box centre).
- The picture from the headset carries the ring at the spot you were looking at; the Mirror picture shows it at the ring's size and colour (Show: Gaze). Snapshot block version 4, the ring's place after the pixels.
- Building from a fresh clone works again: Get-External.ps1 fetches the OpenXR headers.

## 2.0.0 - 2026-09-23

- Renamed: QuadViews Gaze Mirror is now VR Gaze Mirror, since the ring no longer depends on Quad-Views-Foveated. The program is GazeMirror.exe, the install folder VR-Gaze-Mirror, the data folder GazeMirror. The first start carries over your settings, profiles and slots, removes the old folder, and replaces the helper registration SteamVR kept under the old name.
- The SteamVR helper's manifest ships next to it, so uninstalling leaves SteamVR nothing to start. The Status page has a row for the helper. With "Headset only" as the gaze source the Gaze card says nothing about VRChat. Reset all names the things it keeps (the crop box, the VRChat calibration).
- This project's own code makes the whole mirror picture. The modified third-party layers are gone.
- OpenXR games and SteamVR games: the mirror layer mirrors OpenXR games, a small SteamVR helper mirrors games that use SteamVR directly (OpenVR). Same crop, same ring, same OBS source. The Placement tab's in-headset marker works in both: drawn into the picture in OpenXR games, shown as a SteamVR overlay in SteamVR games.
- Gaze comes from the standard interfaces (OpenXR eye gaze, SteamVR eye tracking), so any eye-tracked headset that feeds them works. The ring no longer needs Quad-Views-Foveated.
- One draw per frame: crop, scale, steadying and ring in a single pass. Picture size cap (3840 by default) and picture rate cap on the Mirror page.
- New "Gaze Mirror" OBS source, with no settings of its own. An upgrade removes the old "OpenXR Mirror Capture" plugin that 1.x shipped.
- Crop profiles: save framings, tie one to a game and the mirror applies it at that game's start.
- You choose the eye once on the Mirror page, for OBS and the mirror window alike. Right eye by default.
- Quad-Views-Foveated is optional: the installer ships the official 1.1.3 installer and offers it on its last page and on the Status page.
- New look: a rail of pages on the left, cards, dark theme. Look is now called Ring.
- Updates from inside the app: "Download and install" saves the new installer into Downloads, checks it against the SHA-256 the release publishes, and opens it with Windows Installer.
- The app lists the Quad Views page while Quad-Views-Foveated is installed. A changelog on the About page.
- VRChat fallback: if the headset gives no gaze (eye-tracking software that takes the eye cameras for itself, such as SRanibro, or feeds VRChat and nothing else, such as EyeTrackVR or ALVR), the ring falls back to the VRChat avatar's eye parameters. The SteamVR helper is an OSCQuery service, so VRChat sends them to it by itself. You install and configure nothing; OSC must be on in VRChat. You calibrate the fallback once from the app: a target in the headset at twenty places (along both axes out to 30 degrees to the sides, 22 up and 30 down, and in the four corners) gives the curve from the avatar values to angles, with a separate correction for the corners.
- The app keeps a small log of its actions (app.log next to the layer's and the helper's logs; the Logs button on the About page opens the folder).

## 1.2.2 - 2026-09-20

- Steady the picture: the crop box holds still while the head moves.
- Capture the crop picture from inside the headset with a key.
- Follow travel markers.

## 1.2.1 - 2026-09-19

- Mirror window: capture the mirror without the OBS plugin (OBS Window Capture, Discord).

## 1.1.0 - 2026-09-19

- Crop tool: a shape-locked box on the mirror image; OBS gets that box.
- Gaze-following crop, box locking and drag rules.
- Right eye mirrored with the right eye's field of view; the crop picture shows the whole eye image.

## 1.0.0 - 2026-09-19

- First release: gaze ring on the OBS mirror, ring presets and slots, Quad Views page, layers tool with order check.
