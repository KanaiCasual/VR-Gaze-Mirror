# Changelog

## Next - in development (dev builds)

- Direct3D 12 games in OpenXR are mirrored too (Microsoft Flight Simulator and the like): the layer works on the game's own Direct3D 12 device through Direct3D 11-on-12, so the crop, the steadying, the ring, the capture countdown and the edge finder are the same code as for Direct3D 11 games. The offline test plays a Direct3D 12 game.
- When a game destroys a swapchain in the middle of a session, the in-headset marker lets go of its views of those images at once (it kept them until the session ended).

## 2.1.0 - 2026-09-28

- SRanibro's raw gaze over OSC: a coming SRanibro release (newer than 0.1.9-Beta, which does not have it) can send the tracker's raw gaze to the helper ("Raw gaze OSC" in its settings, 127.0.0.1, port 9005). The helper takes it ahead of the VRChat fallback: a direction, used as it comes, no calibration, in every game while SRanibro runs. "SRanibro raw gaze only" on the Gaze card forces it; the port has a box on the Gaze card. The channel carries SRanibro's own range cap (about 30 degrees today, about 40 in its coming build).
- Capture from headset can take the picture by itself after a countdown (5 to 30 s, chosen next to the key button), for setups where no key can reach the game. The key still works meanwhile. While it counts down, the seconds left are shown large in the middle of the headset view (drawn into the picture in OpenXR games, as a SteamVR overlay in SteamVR games).
- Find visible edges (Mirror page, Crop box): four bars in the headset, pushed outwards with the arrow keys until each disappears behind the edge of what you can see, Enter confirms, Backspace goes back. The result is drawn on the crop picture and "Crop to edges" fits the box to it. Works in OpenXR games (drawn into the picture) and SteamVR games (an overlay for the mirrored eye only).
- The Mirror page's picture has a Show row: the crop box, the follow reach, the steadying room, the visible edges and a new centre mark can each be shown or hidden. The centre mark (dot, line across, line down) sits where straight ahead lands in the eye image, which every picture now carries from the headset's own projection - right for any headset; a faint small cross marks the image's geometric middle.
- Gaze from = Fixed centre, for headsets without eye tracking: the ring sits where straight ahead lands in each eye and never moves. With Quad-Views-Foveated - which puts its sharp area at that spot when it has no eye tracking - the ring is drawn the size of that area, from its settings (an ellipse of its fixed sections). The choice is greyed out once the headset is known to give gaze; the mirror's shared block now says whether it does.
- The VRChat calibration reaches further - 35 degrees to each side, 28 up and 35 down - and trims that to what both eyes of the headset can show, so it fits any headset; the status line states the reach used.
- A Thorough VRChat calibration: a moving target glides over the whole reach, resting on 25 spots, and everything that arrives is recorded (a few thousand samples). The delay between the target and the avatar's eyes is measured and taken out, and a grid map over the whole field is fitted (vrchat_map_grid; the offline test fits made-up eyes). About two minutes; the quick one stays as it was.
- The window title carries the version.
- In OpenXR games the capture countdown was drawn in the middle of each eye's image, which on a headset with the usual asymmetric views are two directions tens of degrees apart, so the number never fused into one. It is now drawn where a point two metres straight ahead lands in each eye, like the SteamVR overlay.
- The Gaze card's calibration line no longer collapses to one letter a line when the window is wide (four cards in a row): the buttons sit under the text and wrap.
- The crop box carries a small cross at its own centre, to line it up with the centre mark or the visible edges (Show: Box centre).
- The picture from the headset carries the ring where you were looking when it was taken; it is drawn on the Mirror picture at the ring's size and colour (Show: Gaze). Snapshot block version 4, the ring's place after the pixels.
- Building from a fresh clone works again: the OpenXR headers are fetched by Get-External.ps1.

## 2.0.0 - 2026-09-23

- Renamed: QuadViews Gaze Mirror is now VR Gaze Mirror, since the ring no longer depends on Quad-Views-Foveated. The program is GazeMirror.exe, the install folder VR-Gaze-Mirror, the data folder GazeMirror (your settings, profiles and slots are carried over on first start and the old folder is removed; the helper registration SteamVR kept under the old name is replaced).
- The SteamVR helper's manifest ships next to it, so uninstalling leaves SteamVR nothing to start. The Status page has a row for the helper. With "Headset only" as the gaze source the Gaze card says nothing about VRChat. Reset all says what it keeps (the crop box, the VRChat calibration).
- Everything that makes the mirror picture is this project's own code. No modified third-party layers any more.
- OpenXR games and SteamVR games: the gaze mirror layer mirrors OpenXR games, a small SteamVR helper mirrors games that use SteamVR directly (OpenVR). Same crop, same ring, same OBS source. The Placement tab's in-headset marker works in both: drawn into the picture in OpenXR games, shown as a SteamVR overlay in SteamVR games.
- Gaze comes from the standard interfaces (OpenXR eye gaze, SteamVR eye tracking), so any eye-tracked headset that feeds them works. Quad-Views-Foveated is no longer needed for the ring.
- One draw per frame: crop, scale, steadying and ring in a single pass. Picture size cap (3840 by default) and picture rate cap on the Mirror page.
- New "Gaze Mirror" OBS source, with no settings of its own. The old "OpenXR Mirror Capture" plugin that 1.x shipped is removed on upgrade.
- Crop profiles: save framings, tie one to a game and it is applied when that game starts.
- The eye is chosen once on the Mirror page, for OBS and the mirror window alike. Right eye by default.
- Quad-Views-Foveated is optional: the official 1.1.3 installer is shipped and offered on the installer's last page and on the Status page.
- New look: a rail of pages on the left, cards, dark only. Look is now called Ring.
- Updates from inside the app: "Download and install" saves the new installer into Downloads, checks it against the SHA-256 published with the release, and opens it with Windows Installer.
- The Quad Views page is only listed while Quad-Views-Foveated is installed. A changelog on the About page.
- VRChat fallback: when the headset gives no gaze (eye-tracking software that takes the eye cameras for itself, such as SRanibro, or only ever feeds VRChat, such as EyeTrackVR or ALVR), the ring falls back to the VRChat avatar's eye parameters. The SteamVR helper is an OSCQuery service, so VRChat sends them to it by itself. Nothing to install or configure; OSC must be on in VRChat. The fallback is calibrated once from the app: a target shown in the headset at twenty places - along both axes out to 30 degrees to the sides, 22 up and 30 down, and in the four corners - gives the curve from the avatar values to angles, with the corners corrected separately.
- The app keeps a small log of what it did (app.log next to the layer's and the helper's logs; the Logs button on the About page opens the folder).

## 1.2.2 - 2026-09-20

- Steady the picture: the crop box holds still while the head moves.
- Capture the crop picture from inside the headset with a key.
- Follow travel markers.

## 1.2.1 - 2026-09-19

- Mirror window: capture the mirror without the OBS plugin (OBS Window Capture, Discord, ...).

## 1.1.0 - 2026-09-19

- Crop tool: a shape-locked box on the mirror image; OBS gets only that box.
- Gaze-following crop, box locking and drag rules.
- Right eye mirrored with the right eye's field of view; the crop picture shows the whole eye image.

## 1.0.0 - 2026-09-19

- First release: gaze ring on the OBS mirror, ring presets and slots, Quad Views page, layers tool with order check.
