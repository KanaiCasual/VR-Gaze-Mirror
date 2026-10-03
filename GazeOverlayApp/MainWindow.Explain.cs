using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;

namespace GazeOverlay;

/// <summary>
/// The explanation at the top of each page: what the page is for, what its parts do and how to use them, in full and
/// on the page itself (a tooltip is found by nobody who does not already know it is there).
///
/// An explanation is open the first time its page is looked at and folded from then on, so that it is read once and
/// does not stand in the way afterwards. A click on its heading opens or folds it, and that choice is kept: one opened
/// by hand stays open. What has been seen, opened and folded is remembered in the app's preferences.
/// </summary>
public partial class MainWindow
{
    /// <summary>One paragraph: a few words that name its subject (may be empty), and the text.</summary>
    private readonly record struct Part(string Lead, string Text);

    private const string HowThisPageWorks = "HOW THIS PAGE WORKS";
    private static readonly Part AboutSliders = new("Sliders.", "Click a value to type one. Double-click a slider for its default.");

    private readonly List<(TabItem Page, string Key, Action<bool> Show)> _explanations = [];
    private TabItem? _pageShown;

    private void BuildExplanations()
    {
        AddExplanation(StatusTab, ExplainStatus, "status", HowThisPageWorks,
            new("The line at the top", "names the game the mirror shows right now and says whether eye gaze arrives. A grey dot means no VR game is running; the mirror starts with the game."),
            new("Three steps", "take a new install to a picture in OBS. Each step ticks itself off once the app has seen it happen, and the card goes away after the third."),
            new("Parts.", "The mirror has three. The mirror layer makes the picture inside OpenXR games. The SteamVR helper does the same for games that use SteamVR directly; it starts and stops with SteamVR. The OBS plugin is the \"Gaze Mirror\" source that shows the picture in OBS. Quad-Views-Foveated is optional and has no part in the mirror. While all is well the card says so in one line, and Details lists the parts; a part with a fault says what to do about it."),
            new("OpenXR API layers.", "A game loads its layers in a fixed order, and the mirror layer has to sit below any layer that draws into the picture, so that it sees the finished picture. Open the list to see that order, to switch a layer on or off, or to move one. Fix order appears if the order breaks a rule the app knows. Windows asks for permission at each change, and a game takes the change at its next start."));

        AddExplanation(CropTab, ExplainMirror, "mirror", HowThisPageWorks,
            new("The picture", "is one eye's whole image as the game drew it. The blue box on it is the part OBS and the mirror window get. Drag inside the box to move it; drag a corner or roll the mouse wheel over it to resize it. With the picture selected, the arrow keys move the box and + and - resize it."),
            new("Taking a picture.", "Refresh picture asks the running game for one. Capture from headset takes it the way you sit in play: arm it, put the headset on, look straight ahead and press the key, or let the countdown run. Find visible edges shows four bars in the headset; push each outwards until it disappears, and a dashed box then marks the part of the image your eye can see. Crop to edges fits the box to that."),
            new("Crop box and Picture.", "Crop the picture switches the box on, and the shape next to it locks the box's proportions. Picture chooses the eye and caps the size and the rate of the picture that goes out, for OBS and the mirror window alike."),
            new("Follow my gaze.", "A 16:9 box covers about half the height of the eye image, so a look down at your knees leaves the frame. With this on, the box glides up or down to keep your target in it."),
            new("Steady the picture.", "The box moves against small head movement and tracking jitter, and turns against a sideways tilt of your head, which takes them out of the picture. It holds back no frames, so it adds no delay."),
            new("Mirror window.", "A window with the same picture for programs that capture windows, such as Discord. With the Gaze Mirror source in OBS you do not need it."),
            new("Profile.", "A profile keeps this framing under a name. Give it a game's program file and the mirror applies it at that game's start."),
            AboutSliders);

        AddExplanation(GazeTab, ExplainGaze, "gaze", HowThisPageWorks,
            new("", "The ring shows your viewers where you look. This page sets the source of your gaze and the spot in the picture the ring lands on. If the mirror is all you want, you can skip the two ring pages."),
            new("Gaze from.", "The headset's own eye tracking comes first: exact angles and nothing to set up. Some eye-tracking software takes the eye cameras for itself (SRanibro) or feeds VRChat and nothing else, and the headset then has no gaze to give. In that case the ring falls back to SRanibro's raw gaze or to the eyes of your VRChat avatar. Fixed centre is for headsets without eye tracking: the ring stays where straight ahead lands."),
            new("Calibrate VRChat gaze", "is for the VRChat fallback alone. The avatar's eye values are no angles, so the helper measures once how they relate to where you look: with VRChat running, follow the target in the headset with your eyes for about 45 seconds. Thorough takes about two minutes and fits a finer map."),
            new("Where the ring lands.", "Most setups need no change here. If the ring sits beside your target, switch on the marker: a small reticle appears in the headset at the ring's place. Look at something small, nudge with the two sliders until the reticle sits on it, and switch the marker off again. The viewing distance matters because the mirror shows one eye, and your two eyes see close things in different places."));

        AddExplanation(LookTab, ExplainRing, "ring", HowThisPageWorks,
            new("", "The look of the ring on your stream. Your headset shows none of it. The preview shows each change at once, over a cockpit, terrain, sky or black."),
            new("Presets and Mine.", "A preset sets the whole look with one click. Mine holds three looks of your own: Save stores the current one in a slot, and a click on the slot brings it back. For most people a preset and the first card are all the ring needs; the three sections under it fold away."),
            new("Ring.", "Style picks the mark: Ghost is a thin ring that stretches into a teardrop as your eyes move; the others are a plain ring, a glow, a dot, a spotlight that darkens the rest, a bubble or blob with a trail, and a heatmap. Colour and size shape it."),
            new("More of the look.", "Line thickness and edge softness, a glow around the line, and the blending: Light adds the ring like a glow and the scene shows through; Paint draws it on top like ink, easier to see and hiding what lies behind. Solidity, the fill and the dark outline keep the ring readable against a bright sky."),
            new("Tail.", "The tail shows your viewers where your eyes came from: with the Ghost style a teardrop that stretches behind the ring as your eyes move and closes as they rest. Strength, lag and the longest length shape it, and Tail follows fixes it to the world (the choice for VR) or to the screen. The heatmap's warm-up and cool-down set the stare time that turns a spot red and the time the spot takes to fade."),
            new("Motion.", "Raw eye tracking jitters, and eyes close in a blink. Adaptive smoothing holds the ring still while your eye rests and lets it jump as you look elsewhere. Dwell tightens the ring after a held gaze, so viewers can tell reading a gauge from passing over it. The last three settings say what the ring does in a blink and when tracking counts as lost."),
            AboutSliders);

        AddExplanation(CoverTab, ExplainCover, "cover", "FOR HEADSETS WITH CAMERA-BASED TRACKING (INSIDE-OUT, SLAM)",
            new("", "A headset with camera-based tracking holds its place in the room by anchors: fixed points its cameras pick out around you, such as corners, edges and patterns on walls and furniture. It works out how your head moved from the way those points shift between one camera picture and the next. For that it needs points that stand still."),
            new("", "A monitor that shows the game gives it points that move. The picture is bright and full of sharp detail, so the cameras find plenty of points in it, and it turns as you turn your head. If the headset takes those points for anchors, it places your head wrong: the view can drift, shake or jump. The effect grows with the share of the cameras' view the monitor fills: close to the screen in a racing or flight rig, with several monitors, or in a dim room where the monitor is the brightest thing in sight."),
            new("", "The cover puts black over the game's window, so the cameras find no moving points there and keep to the room. It covers the game's own window and nothing else, and it stays while the game is the window in front: bring another window to the front and the cover goes at once."),
            new("", "A headset tracked by base stations (Lighthouse) does not look at the room and has no use for the cover."));

        AddExplanation(QuadViewsTab, ExplainQuadViews, "quadviews", HowThisPageWorks,
            new("", "Quad-Views-Foveated makes a game render a small sharp region where you look and the rest of the view at a low resolution, which saves graphics card time. These are its own settings; the app lists this page while it is installed. It handles Direct3D 11 games: switch it off on the Status page for a Direct3D 12 game."),
            new("Apply.", "A game reads these settings at its start, so press Apply and then restart the game. The lines above the buttons give the pixels per frame for the saved settings and for the sliders as they stand."),
            new("Focus", "sets the size of the sharp region as a share of the field of view, moves it up or down, and sets the width of the blend into the surround. Without eye tracking the region stays in the middle of the view."),
            new("Resolution", "sets the pixel count of the sharp region and of the surround; most of the saving comes from the surround. Presets and your own three slots set the sliders, and Apply saves them."),
            AboutSliders);

        // A page that has been looked at folds its explanation as the user moves on (or closes the app).
        _pageShown = Tabs.SelectedItem as TabItem;
        Tabs.SelectionChanged += (_, e) =>
        {
            if (!ReferenceEquals(e.OriginalSource, Tabs)) return;
            LeavePage(_pageShown);
            _pageShown = Tabs.SelectedItem as TabItem;
        };
        Closing += (_, _) => LeavePage(_pageShown);
    }

    /// <summary>Open: by the user's own hand, or because the page has not been looked at yet (and was not folded by hand).</summary>
    private bool ExplanationOpen(string key) =>
        _appSettings.OpenExplanations.Contains(key) || !_appSettings.SeenExplanations.Contains(key) && !_appSettings.FoldedExplanations.Contains(key);

    private void LeavePage(TabItem? page)
    {
        if (page == null || App.Headless) return; // A self-test or a screenshot run leaves the preferences alone.
        foreach (var (_, key, show) in _explanations.Where(x => ReferenceEquals(x.Page, page)))
        {
            if (_appSettings.SeenExplanations.Contains(key)) continue;
            _appSettings.SeenExplanations.Add(key);
            _appSettings.Save();
            show(ExplanationOpen(key));
        }
    }

    /// <summary>The card: a heading that opens or folds it, and the paragraphs.</summary>
    private void AddExplanation(TabItem page, Panel host, string key, string title, params Part[] parts)
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

        // The heading is a button, so the keyboard reaches it and a screen reader says what it does.
        var arrow = new TextBlock { Style = (Style)FindResource("Icon"), FontSize = 9, Foreground = muted, Width = 15 };
        var label = new StackPanel { Orientation = Orientation.Horizontal };
        label.Children.Add(arrow);
        label.Children.Add(new TextBlock { Text = title, Style = (Style)FindResource("CardTitle"), Margin = new Thickness(0) });
        var heading = new Button { Style = (Style)FindResource("FlatButton"), Content = label, ToolTip = "Opens this explanation or folds it away" };

        void Show(bool open)
        {
            body.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
            arrow.Text = open ? "" : "";
            System.Windows.Automation.AutomationProperties.SetName(heading, title + (open ? ", open: fold it away" : ", folded: open it"));
        }
        Show(ExplanationOpen(key));
        heading.Click += (_, _) =>
        {
            var open = body.Visibility != Visibility.Visible;
            Show(open);
            _appSettings.OpenExplanations.Remove(key);
            _appSettings.FoldedExplanations.Remove(key);
            (open ? _appSettings.OpenExplanations : _appSettings.FoldedExplanations).Add(key);
            _appSettings.Save();
        };
        _explanations.Add((page, key, Show));

        var content = new StackPanel();
        content.Children.Add(heading);
        content.Children.Add(body);
        host.Children.Add(new Border { Style = (Style)FindResource("Card"), Padding = new Thickness(14, 10, 14, 11), Child = content });
    }
}
