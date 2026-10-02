using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;

namespace GazeOverlay;

/// <summary>
/// The explanation at the top of each page: what the page is for, what its parts do and how to use them, in full and
/// on the page itself (a tooltip is found by nobody who does not already know it is there). Each explanation folds
/// away at a click on its heading and stays folded; the folded ones are remembered in the app's preferences.
/// </summary>
public partial class MainWindow
{
    /// <summary>One paragraph: a few words that name its subject (may be empty), and the text.</summary>
    private readonly record struct Part(string Lead, string Text);

    private const string HowThisPageWorks = "HOW THIS PAGE WORKS";

    private void BuildExplanations()
    {
        AddExplanation(ExplainStatus, "status", HowThisPageWorks,
            new("The line at the top", "names the game the mirror shows right now and says whether eye gaze arrives. A grey dot means no VR game is running; the mirror starts with the game."),
            new("Three steps", "take a new install to a picture in OBS. Each step ticks itself off once the app has seen it happen, and the card goes away after the third."),
            new("Parts.", "The mirror has three, and each wants a green dot. The gaze mirror layer makes the picture inside OpenXR games. The SteamVR helper does the same for games that use SteamVR directly; it starts and stops with SteamVR. The OBS plugin is the \"Gaze Mirror\" source that shows the picture in OBS. Quad-Views-Foveated is optional and has no part in the mirror. A part with a fault says what to do about it."),
            new("OpenXR API layers.", "A game loads its layers in a fixed order, and the mirror has to sit below any layer that draws into the picture, so that it sees the finished picture. Open the list to see that order, to switch a layer on or off, or to move one. Fix order appears if the order breaks a rule the app knows. Windows asks for permission at each change, and a game takes the change at its next start."));

        AddExplanation(ExplainMirror, "mirror", HowThisPageWorks,
            new("The picture", "is one eye's whole image as the game drew it. The blue box on it is the part OBS and the mirror window get. Drag inside the box to move it; drag a corner or roll the mouse wheel over it to resize it."),
            new("Taking a picture.", "Refresh picture asks the running game for one. Capture from headset takes it the way you sit in play: arm it, put the headset on, look straight ahead and press the key, or let the countdown run. Find visible edges shows four bars in the headset; push each outwards until it disappears, and a dashed box then marks the part of the image your eye can see. Crop to edges fits the box to that."),
            new("Crop box and Picture.", "Crop the picture switches the box on, and the shape next to it locks the box's proportions. Picture chooses the eye and caps the size and the rate of the picture that goes out, for OBS and the mirror window alike."),
            new("Follow my gaze.", "A 16:9 box covers about half the height of the eye image, so a look down at your knees leaves the frame. With this on, the box glides up or down to keep your target in it."),
            new("Steady the picture.", "The box moves against small head movement and tracking jitter, which takes them out of the picture. It holds back no frames, so it adds no delay."),
            new("Mirror window.", "A window with the same picture for programs that capture windows, such as Discord. With the Gaze Mirror source in OBS you do not need it."),
            new("Profile.", "A profile keeps this framing under a name. Give it a game's program file and the mirror applies it at that game's start."));

        AddExplanation(ExplainGaze, "gaze", HowThisPageWorks,
            new("", "The ring shows your viewers where you look. This page sets the source of your gaze and the spot in the picture the ring lands on. If the mirror is all you want, you can skip the four ring pages."),
            new("Gaze from.", "The headset's own eye tracking comes first: exact angles and nothing to set up. Some eye-tracking software takes the eye cameras for itself (SRanibro) or feeds VRChat and nothing else, and the headset then has no gaze to give. In that case the ring falls back to SRanibro's raw gaze or to the eyes of your VRChat avatar. Fixed centre is for headsets without eye tracking: the ring stays where straight ahead lands."),
            new("Calibrate VRChat gaze", "is for the VRChat fallback alone. The avatar's eye values are no angles, so the helper measures once how they relate to where you look: with VRChat running, follow the target in the headset with your eyes for about 45 seconds. Thorough takes about two minutes and fits a finer map."),
            new("Where the ring lands.", "Most setups need no change here. If the ring sits beside your target, switch on the marker: a small reticle appears in the headset at the ring's place. Look at something small, nudge with the two sliders until the reticle sits on it, and switch the marker off again. The viewing distance matters because the mirror shows one eye, and your two eyes see close things in different places."));

        AddExplanation(ExplainRing, "ring", HowThisPageWorks,
            new("", "The look of the ring on your stream. Your headset shows none of it. The preview shows each change at once, over a cockpit, terrain, sky or black."),
            new("Presets and Mine.", "A preset sets the whole look with one click. Mine holds three looks of your own: Save stores the current one in a slot, and a click on the slot brings it back."),
            new("Ring.", "Style picks the mark: Ghost is a thin ring that stretches into a teardrop as your eyes move; the others are a plain ring, a glow, a dot, a spotlight that darkens the rest, a bubble or blob with a trail, and a heatmap. Colour, size, line thickness and edge softness shape it."),
            new("Glow and blending.", "Glow puts a soft light around the line. Blending decides how the ring meets the picture: Light adds it like a glow and the scene shows through; Paint draws it on top like ink, easier to see and hiding what lies behind. Solidity, the fill and the dark outline keep the ring readable against a bright sky."));

        AddExplanation(ExplainTail, "tail", HowThisPageWorks,
            new("", "The tail shows your viewers where your eyes came from. With the Ghost style it is a teardrop that stretches behind the ring as your eyes move and closes as they rest."),
            new("Tail.", "Strength is the tail's brightness as a share of the ring's. Lag is the time the tail takes to catch up with the ring: longer gives a longer, lazier tail, and Longest tail caps its length. Bubble / Solid trail is the trail of blobs those two styles leave. Tail follows chooses what the tail is fixed to: the world, so a turn of your head stretches it too (the choice for VR), or the screen."),
            new("Heatmap style.", "The heatmap leaves spots where you looked. Warm-up is the stare time that takes a spot from blue to red; cool-down is the time a spot takes to fade after you look away."));

        AddExplanation(ExplainMotion, "motion", HowThisPageWorks,
            new("", "Raw eye tracking jitters, and eyes close in a blink. This page sets how the ring smooths the gaze and what it does when tracking drops out."),
            new("Smoothing.", "Adaptive holds the ring still while your eye rests and lets it jump as you look elsewhere. Steadiness when fixating tunes the first: lower is steadier and lags more on slow movements. Snap on eye jumps tunes the second: higher lands jumps faster. Fixed applies one smoothing time to both."),
            new("Dwell.", "Hold your gaze on one spot for the dwell delay and the ring tightens and brightens, so viewers can tell reading a gauge from passing over it."),
            new("Blinks and tracking loss.", "Ride through blinks keeps the ring at its last place for that long after the eyes close. Fade is the time the ring takes to appear and to disappear. After the time in Tracking lost after without fresh data, the mirror treats eye tracking as lost."));

        AddExplanation(ExplainCover, "cover", "FOR HEADSETS WITH CAMERA-BASED TRACKING (INSIDE-OUT, SLAM)",
            new("", "A headset with camera-based tracking holds its place in the room by anchors: fixed points its cameras pick out around you, such as corners, edges and patterns on walls and furniture. It works out how your head moved from the way those points shift between one camera picture and the next. For that it needs points that stand still."),
            new("", "A monitor that shows the game gives it points that move. The picture is bright and full of sharp detail, so the cameras find plenty of points in it, and it turns as you turn your head. If the headset takes those points for anchors, it places your head wrong: the view can drift, shake or jump. The effect grows with the share of the cameras' view the monitor fills: close to the screen in a racing or flight rig, with several monitors, or in a dim room where the monitor is the brightest thing in sight."),
            new("", "The cover puts black over the game's window, so the cameras find no moving points there and keep to the room. It covers the game's own window and nothing else, and it stays while the game is the window in front: bring another window to the front and the cover goes at once."),
            new("", "A headset tracked by base stations (Lighthouse) does not look at the room and has no use for the cover."));

        AddExplanation(ExplainQuadViews, "quadviews", HowThisPageWorks,
            new("", "Quad-Views-Foveated makes a game render a small sharp region where you look and the rest of the view at a low resolution, which saves graphics card time. These are its own settings; the app lists this page while it is installed. It handles Direct3D 11 games: switch it off on the Status page for a Direct3D 12 game."),
            new("Apply.", "A game reads these settings at its start, so press Apply and then restart the game. The lines above the buttons give the pixels per frame for the saved settings and for the sliders as they stand."),
            new("Focus", "sets the size of the sharp region as a share of the field of view, moves it up or down, and sets the width of the blend into the surround. Without eye tracking the region stays in the middle of the view."),
            new("Resolution", "sets the pixel count of the sharp region and of the surround; most of the saving comes from the surround. Presets and your own three slots set the sliders, and Apply saves them."));
    }

    /// <summary>The card: a heading that folds it at a click, and the paragraphs.</summary>
    private void AddExplanation(Panel host, string key, string title, params Part[] parts)
    {
        var muted = (Brush)FindResource("Muted");
        var body = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
        foreach (var part in parts)
        {
            var paragraph = new TextBlock { Style = (Style)FindResource("Hint"), FontSize = 12, Margin = new Thickness(0, 0, 0, body.Children.Count == parts.Length - 1 ? 0 : 6) };
            // What the paragraph is about, a little brighter than the rest, so the eye finds the part it is after.
            if (part.Lead.Length > 0) paragraph.Inlines.Add(new Run(part.Lead + " ") { FontWeight = FontWeights.SemiBold, Foreground = (Brush)FindResource("Text") });
            paragraph.Inlines.Add(new Run(part.Text));
            body.Children.Add(paragraph);
        }

        var arrow = new TextBlock { Style = (Style)FindResource("Icon"), FontSize = 9, Foreground = muted, Width = 15 };
        var heading = new StackPanel { Orientation = Orientation.Horizontal, Background = Brushes.Transparent, Cursor = System.Windows.Input.Cursors.Hand, ToolTip = "Click to fold this explanation away or to open it" };
        heading.Children.Add(arrow);
        heading.Children.Add(new TextBlock { Text = title, Style = (Style)FindResource("CardTitle"), Margin = new Thickness(0) });

        void Show(bool open)
        {
            body.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            arrow.Text = open ? "" : "";
        }
        Show(!_appSettings.FoldedExplanations.Contains(key));
        heading.MouseLeftButtonUp += (_, _) =>
        {
            var open = body.Visibility != Visibility.Visible;
            Show(open);
            _appSettings.FoldedExplanations.Remove(key);
            if (!open) _appSettings.FoldedExplanations.Add(key);
            _appSettings.Save();
        };

        var content = new StackPanel();
        content.Children.Add(heading);
        content.Children.Add(body);
        host.Children.Add(new Border { Style = (Style)FindResource("Card"), Padding = new Thickness(14, 10, 14, 11), Child = content });
    }
}
