using System.Globalization;

namespace GazeOverlay;

public enum SettingKind { Slider, Choice, Toggle, Color }

public enum ValueUnit { Plain, PercentOfImage, Percent, Milliseconds, Hertz, Meters }

public sealed record ChoiceOption(string Value, string Label);

/// <summary>One option of gaze.cfg: the key the layer reads plus everything needed to present it to a person.</summary>
public sealed class SettingDef
{
    public required string Key { get; init; }
    public required string Group { get; init; }
    public required string Label { get; init; }
    public required string Description { get; init; }
    public required SettingKind Kind { get; init; }
    public required string Default { get; init; }
    public double Min { get; init; }
    public double Max { get; init; }
    public double Step { get; init; }
    public ValueUnit Unit { get; init; } = ValueUnit.Plain;
    public ChoiceOption[] Choices { get; init; } = [];
    /// <summary>Text shown instead of the number at a special value (e.g. 0 = "Off").</summary>
    public string? ZeroText { get; init; }

    public string Format(double value)
    {
        if (ZeroText != null && value == 0) return ZeroText;
        var c = CultureInfo.InvariantCulture;
        return Unit switch
        {
            ValueUnit.PercentOfImage => (value * 100).ToString("0.##", c) + " % of image",
            ValueUnit.Percent => (value * 100).ToString("0", c) + " %",
            ValueUnit.Milliseconds => value.ToString("0", c) + " ms",
            ValueUnit.Hertz => value.ToString("0.0", c) + " Hz",
            ValueUnit.Meters => value.ToString("0.0", c) + " m",
            _ => value.ToString("0.###", c),
        };
    }

    /// <summary>How the value is written to the config file.</summary>
    public string Serialize(double value)
    {
        var decimals = Step >= 1 ? 0 : (int)Math.Min(6, Math.Ceiling(-Math.Log10(Step)) + 1);
        return Math.Round(value, decimals).ToString("0." + new string('#', decimals), CultureInfo.InvariantCulture);
    }
}

public sealed record Preset(string Name, string Description, Dictionary<string, string> Values);

public static class Settings
{
    public const string GroupLook = "Look";
    public const string GroupTail = "Tail";
    public const string GroupMotion = "Motion";
    public const string GroupPlacement = "Placement";
    /// <summary>Edited on the Crop tab with its own tool, not as generic rows.</summary>
    public const string GroupCrop = "Crop";

    public static readonly string[] Groups = [GroupLook, GroupTail, GroupMotion, GroupPlacement, GroupCrop];

    public static readonly Dictionary<string, string> GroupIntro = new()
    {
        [GroupLook] = "How the gaze indicator is drawn. Only your OBS mirror shows it - it is never visible in the headset.",
        [GroupTail] = "The teardrop tail that stretches behind the ring when your eyes move (Ghost style).",
        [GroupMotion] = "How the ring follows your eyes: smoothing, blinks, and what happens when you hold your gaze.",
        [GroupPlacement] = "Where the ring lands in the mirrored image. Only change these if the ring looks off-target.",
        [GroupCrop] = "Hands OBS only a box of the mirror image, so the OBS source is the box.",
    };

    public static readonly SettingDef[] All =
    [
        // ---------------------------------------------------------------- Look
        new()
        {
            Key = "enabled", Group = GroupLook, Kind = SettingKind.Toggle, Default = "1",
            Label = "Show the gaze indicator",
            Description = "Master switch. Turn off to hide the indicator on stream without uninstalling anything.",
        },
        new()
        {
            Key = "style", Group = GroupLook, Kind = SettingKind.Choice, Default = "ghost",
            Label = "Style",
            Description = "Ghost is the recommended look: a thin ring that deforms into a teardrop as your eyes move. " +
                          "The other styles are simpler shapes; Bubble, Solid and Heatmap leave a fading trail.",
            Choices =
            [
                new("ghost", "Ghost ring with teardrop tail (recommended)"),
                new("ring", "Plain ring"),
                new("glow", "Soft glow, no edges"),
                new("dot", "Small dot"),
                new("spotlight", "Spotlight (darkens everything else)"),
                new("bubble", "Bubble with fading trail"),
                new("solid", "Solid blob with fading trail"),
                new("heatmap", "Heatmap (blue = glance, red = stare)"),
            ],
        },
        new()
        {
            Key = "color", Group = GroupLook, Kind = SettingKind.Color, Default = "40,170,245",
            Label = "Colour",
            Description = "Colour of the indicator. Heatmap uses its own colours.",
        },
        new()
        {
            Key = "radius", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.042",
            Min = 0.01, Max = 0.12, Step = 0.001, Unit = ValueUnit.PercentOfImage,
            Label = "Ring size",
            Description = "Radius of the ring, relative to the height of the full mirrored image (before any cropping in OBS). " +
                          "A larger ring surrounds what you look at instead of sitting on top of it.",
        },
        new()
        {
            Key = "thickness", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.001",
            Min = 0.0002, Max = 0.006, Step = 0.0001, Unit = ValueUnit.PercentOfImage,
            Label = "Line thickness",
            Description = "How wide the ring's line looks. This alone sets the width - edge softness never makes it wider.",
        },
        new()
        {
            Key = "feather", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.001",
            Min = 0, Max = 0.006, Step = 0.0001, Unit = ValueUnit.PercentOfImage, ZeroText = "Hard edge",
            Label = "Edge softness",
            Description = "How soft the line's edges are. The fade happens inside the line, so it cannot be softer than the line " +
                          "is thick (set it equal to Line thickness for a fully soft line). For Bubble/Solid/Dot this is the blob's edge.",
        },
        new()
        {
            Key = "glow", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.0028",
            Min = 0, Max = 0.02, Step = 0.0002, Unit = ValueUnit.PercentOfImage,
            Label = "Glow reach",
            Description = "How far the faint glow extends beyond the line (Ghost style). This is the way to soften a thin line " +
                          "without making the line itself thicker.",
        },
        new()
        {
            Key = "glow_strength", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.4",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "No glow",
            Label = "Glow strength",
            Description = "Brightness of that glow relative to the line (Ghost style).",
        },
        new()
        {
            Key = "opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.5",
            Min = 0.05, Max = 1, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Brightness",
            Description = "Overall strength of the indicator. For Spotlight, this is how much the surroundings are darkened.",
        },
        new()
        {
            Key = "blend", Group = GroupLook, Kind = SettingKind.Choice, Default = "screen",
            Label = "Blending",
            Description = "Light: the indicator adds to the image like a glow and the scene stays visible through it. " +
                          "Paint: drawn on top like ink - more visible, but it covers what is behind it.",
            Choices = [new("screen", "Light (recommended)"), new("normal", "Paint")],
        },
        new()
        {
            Key = "solidity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.4",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Solidity",
            Description = "Light blending only. 0 % = pure light: never hides anything, but can vanish against a bright sky. " +
                          "Higher values also cover a little of the scene so the ring survives bright backgrounds.",
        },
        new()
        {
            Key = "fill_opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0",
            Min = 0, Max = 0.5, Step = 0.01, Unit = ValueUnit.Percent, ZeroText = "Clear",
            Label = "Fill inside the ring",
            Description = "Tints the inside of the ring. Leave clear to keep what you are looking at unobstructed.",
        },
        new()
        {
            Key = "shadow_opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.3",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "None",
            Label = "Dark outline",
            Description = "Plain ring and Dot styles with Paint blending only: a soft dark outline so a faint indicator stays readable " +
                          "over bright backgrounds.",
        },

        // ---------------------------------------------------------------- Tail
        new()
        {
            Key = "tail_opacity", Group = GroupTail, Kind = SettingKind.Slider, Default = "0.7",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "No tail",
            Label = "Tail strength",
            Description = "Brightness of the tail relative to the ring. Lower it if the tail draws more attention than the ring.",
        },
        new()
        {
            Key = "trail_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "200",
            Min = 0, Max = 3000, Step = 10, Unit = ValueUnit.Milliseconds,
            Label = "Tail lag",
            Description = "Ghost style: how long the tail takes to catch up with the ring. Longer = a longer, lazier tail.",
        },
        new()
        {
            Key = "blob_trail_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "450",
            Min = 0, Max = 2000, Step = 10, Unit = ValueUnit.Milliseconds, ZeroText = "No trail",
            Label = "Bubble / Solid trail",
            Description = "Bubble and Solid styles: how long the trail of shrinking, fading blobs behind the gaze lasts.",
        },
        new()
        {
            Key = "heat_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "2000",
            Min = 200, Max = 6000, Step = 50, Unit = ValueUnit.Milliseconds,
            Label = "Heatmap warm-up",
            Description = "Heatmap style: how long you have to stare at one place for it to go from blue through green and yellow to red.",
        },
        new()
        {
            Key = "heat_cool_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "700",
            Min = 100, Max = 5000, Step = 50, Unit = ValueUnit.Milliseconds,
            Label = "Heatmap cool-down",
            Description = "Heatmap style: when you look away, the spot stays where it was and cools down over this time, while a new spot starts cold under your gaze. Looking back at a spot that is still warm carries on from there.",
        },
        new()
        {
            Key = "tail_max", Group = GroupTail, Kind = SettingKind.Slider, Default = "0.2",
            Min = 0, Max = 0.5, Step = 0.01, Unit = ValueUnit.PercentOfImage,
            Label = "Longest tail",
            Description = "Upper limit for the tail's length, however fast your eyes move.",
        },
        new()
        {
            Key = "tail_space", Group = GroupTail, Kind = SettingKind.Choice, Default = "world",
            Label = "Tail follows",
            Description = "The world: the tail marks where your gaze was in the scene, so looking around by turning your head " +
                          "stretches it too (right for VR). The screen: only on-screen movement counts, like a desktop eye tracker.",
            Choices = [new("world", "The world (recommended for VR)"), new("screen", "The screen")],
        },

        // ---------------------------------------------------------------- Motion
        new()
        {
            Key = "filter", Group = GroupMotion, Kind = SettingKind.Choice, Default = "adaptive",
            Label = "Smoothing",
            Description = "Adaptive keeps the ring rock-steady while your eye is still and lets it jump instantly when you look " +
                          "elsewhere. Fixed applies the same smoothing all the time.",
            Choices = [new("adaptive", "Adaptive (recommended)"), new("simple", "Fixed")],
        },
        new()
        {
            Key = "filter_min_cutoff", Group = GroupMotion, Kind = SettingKind.Slider, Default = "1.5",
            Min = 0.3, Max = 6, Step = 0.1, Unit = ValueUnit.Hertz,
            Label = "Steadiness when fixating",
            Description = "Adaptive smoothing. Lower = steadier ring during a fixation, but it follows slow movements with more lag.",
        },
        new()
        {
            Key = "filter_beta", Group = GroupMotion, Kind = SettingKind.Slider, Default = "8",
            Min = 0, Max = 20, Step = 0.5,
            Label = "Snap on eye jumps",
            Description = "Adaptive smoothing. Higher = the smoothing lets go sooner as your eye speeds up, so jumps land faster. " +
                          "Lower it if jumps feel too abrupt.",
        },
        new()
        {
            Key = "smoothing_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "60",
            Min = 0, Max = 300, Step = 5, Unit = ValueUnit.Milliseconds, ZeroText = "None (jittery)",
            Label = "Fixed smoothing time",
            Description = "Only used when Smoothing is set to Fixed.",
        },
        new()
        {
            Key = "dwell_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "500",
            Min = 0, Max = 2000, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "Off",
            Label = "Dwell delay",
            Description = "Hold your gaze on one spot this long and the ring tightens and brightens slightly, so viewers can tell " +
                          "\"reading that gauge\" from \"eyes passing over it\".",
        },
        new()
        {
            Key = "dwell_shrink", Group = GroupMotion, Kind = SettingKind.Slider, Default = "0.25",
            Min = 0, Max = 0.6, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Dwell tightening",
            Description = "How much the ring shrinks while dwelling.",
        },
        new()
        {
            Key = "hold_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "300",
            Min = 0, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds,
            Label = "Ride through blinks",
            Description = "Keep showing the ring at its last position for this long when eye tracking drops out, so a blink does not " +
                          "make it flicker.",
        },
        new()
        {
            Key = "fade_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "150",
            Min = 0, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds, ZeroText = "Instant",
            Label = "Fade in / out",
            Description = "How gently the ring appears and disappears.",
        },
        new()
        {
            Key = "timeout_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "250",
            Min = 50, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds,
            Label = "Tracking lost after",
            Description = "Treat eye tracking as lost when no fresh data arrived for this long. Rarely needs changing.",
        },

        // ---------------------------------------------------------------- Placement
        new()
        {
            Key = "projection", Group = GroupPlacement, Kind = SettingKind.Choice, Default = "layer",
            Label = "Placement method",
            Description = "Your gaze starts between your eyes, but the mirror shows a single eye sitting a few centimetres to the " +
                          "side. Accurate places the ring for the eye you actually record. Legacy uses Quad-Views-Foveated's own " +
                          "estimate (assumes you look at something about 1 m away).",
            Choices = [new("layer", "Accurate, per recorded eye (recommended)"), new("producer", "Legacy")],
        },
        new()
        {
            Key = "focus_distance", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = 0, Max = 10, Step = 0.1, Unit = ValueUnit.Meters, ZeroText = "Far away",
            Label = "What you usually look at is",
            Description = "Accurate placement only. No single distance is right for everything: \"Far away\" is exact for the HUD, " +
                          "other aircraft and the world; about 0.7 m is exact for cockpit switches instead.",
        },
        new()
        {
            Key = "offset_x", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = -0.05, Max = 0.05, Step = 0.0005, Unit = ValueUnit.PercentOfImage,
            Label = "Nudge right / left",
            Description = "Manual trim if the ring sits consistently to one side of what you look at. Positive moves it right.",
        },
        new()
        {
            Key = "offset_y", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = -0.05, Max = 0.05, Step = 0.0005, Unit = ValueUnit.PercentOfImage,
            Label = "Nudge up / down",
            Description = "Manual trim if the ring sits consistently above or below what you look at. Positive moves it up.",
        },
        new()
        {
            Key = "headset_marker", Group = GroupPlacement, Kind = SettingKind.Toggle, Default = "0",
            Label = "Show a marker inside the headset (calibration)",
            Description = "Draws a bracket marker in the headset exactly where the stream's ring is, so you can centre it from inside VR. " +
                          "It can only be switched on and off here - there is no key for it in game, so nothing you press while playing " +
                          "can make it appear. While it is on, Ctrl+Alt+arrow keys nudge the ring in game (hold Shift for bigger steps); " +
                          "the nudge is saved into the two Nudge sliders above and shows up here live. Tips: OBS must be capturing the " +
                          "mirror; stare at a fixed target such as the HUD pipper and judge the marker from the corner of your eye - " +
                          "if you look at the marker it runs away from you. Switch it off again when you are done.",
        },
        new()
        {
            Key = "use_raw_gaze", Group = GroupPlacement, Kind = SettingKind.Toggle, Default = "0",
            Label = "Ignore Quad-Views-Foveated's focus offsets",
            Description = "Legacy placement only. Quad-Views-Foveated lets you shift its sharp region; tick this so that shift does " +
                          "not move the ring as well.",
        },

        // ---------------------------------------------------------------- Crop (its own tab and tool)
        new()
        {
            Key = "crop_enabled", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "0",
            Label = "Crop the mirror image",
            Description = "Hand OBS only the box below instead of the whole eye image. Leave the crop values of the OBS source at 0.",
        },
        new()
        {
            Key = "crop_aspect", Group = GroupCrop, Kind = SettingKind.Choice, Default = "16:9",
            Label = "Shape",
            Description = "Shape of the box, width:height. 0 = free.",
            Choices =
            [
                new("16:9", "16:9  widescreen"), new("9:16", "9:16  vertical / shorts"), new("1:1", "1:1  square"),
                new("4:3", "4:3"), new("4:5", "4:5  portrait"), new("21:9", "21:9  ultrawide"), new("0", "Free"),
            ],
        },
        new()
        {
            Key = "crop_center_x", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0, Max = 1, Step = 0.0005,
            Label = "Box centre, across", Description = "Centre of the box, 0 = left edge of the image, 1 = right edge.",
        },
        new()
        {
            Key = "crop_center_y", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0, Max = 1, Step = 0.0005,
            Label = "Box centre, down", Description = "Centre of the box, 0 = top edge of the image, 1 = bottom edge.",
        },
        new()
        {
            Key = "crop_height", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0.02, Max = 1, Step = 0.0005,
            Label = "Box height", Description = "Height of the box as a fraction of the image height. The width follows from the shape.",
        },
        new()
        {
            Key = "crop_width", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0.02, Max = 1, Step = 0.0005,
            Label = "Box width (free shape)", Description = "Width of the box as a fraction of the image width. Only used with the free shape.",
        },
        new()
        {
            Key = "crop_follow", Group = GroupCrop, Kind = SettingKind.Choice, Default = "off",
            Label = "Follow my gaze",
            Description = "A 16:9 box only covers about half the height of the eye image, so looking far up or down takes your gaze out of frame. " +
                          "With Up / down, the box glides after your eyes like a camera operator: nothing moves while you look around the middle, " +
                          "and when your gaze nears the top or bottom the box moves just far enough to keep it in frame. Only eye movement does this - turning your head already moves the whole picture.",
            Choices = [new("off", "Off - the box stays where I put it"), new("vertical", "Up / down")],
        },
        new()
        {
            Key = "crop_follow_deadzone", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.4", Min = 0, Max = 0.9, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Still zone",
            Description = "The middle share of the box height in which your gaze moves nothing (shown as two dashed lines on the picture). Smaller = the box reacts sooner and keeps what you look at closer to the middle; larger = a calmer picture.",
        },
        new()
        {
            Key = "crop_follow_reach", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0, Max = 2, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Reach",
            Description = "How far the box may travel up or down from where you put it, as a share of its own height (100 % = one whole box height each way). Shown on the picture as the tinted area above and below the box. It never leaves the image.",
        },
        new()
        {
            Key = "crop_follow_glide_ms", Group = GroupCrop, Kind = SettingKind.Slider, Default = "500", Min = 0, Max = 2500, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "Instant",
            Label = "Glide",
            Description = "How softly the box moves - on the way out and on the way home. Longer is calmer for viewers but lags further behind your eyes. Instant = the box jumps.",
        },
        new()
        {
            Key = "crop_follow_return", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "1",
            Label = "Return home",
            Description = "Go back to where you put the box once your gaze allows it. Off = the box stays wherever it was last pushed.",
        },
        new()
        {
            Key = "crop_follow_home_ms", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1200", Min = 0, Max = 6000, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "At once",
            Label = "Return after",
            Description = "How long your gaze has to rest inside the still zone before the box heads home. At once = it is always as close to home as your gaze allows.",
        },
        new()
        {
            Key = "stabilize", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "0",
            Label = "Steady the picture",
            Description = "Takes small head movements (tremor, engine shake, breathing) out of the mirror picture: the crop box counter-moves them inside the room around it, which costs nothing. Needs the crop to be on, and only has as much travel as the box has room. Deliberate head turns still come through. Tilting the head sideways is not corrected.",
        },
        new()
        {
            Key = "stabilize_min_cutoff", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0.1, Max = 5, Step = 0.1, Unit = ValueUnit.Hertz,
            Label = "Steadiness",
            Description = "How strongly head movement is smoothed (One-Euro filter). LOWER = steadier, more like a camera on a gimbal, but the picture trails further behind your head and needs more room to move. Around 1 Hz and above only takes out tremor and is hard to see; 0.2 - 0.4 Hz smooths ordinary head sway visibly.",
        },
        new()
        {
            Key = "stabilize_beta", Group = GroupCrop, Kind = SettingKind.Slider, Default = "8", Min = 0, Max = 40, Step = 1,
            Label = "Follow quick turns",
            Description = "How strongly the steadying lets go when you turn your head on purpose. Higher = quick turns come through with less lag; lower = smoother, but the picture trails behind and catches up.",
        },
        new()
        {
            Key = "stabilize_room", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.02", Min = 0.01, Max = 0.1, Step = 0.01, Unit = ValueUnit.Percent,
            Label = "Room to move",
            Description = "How far the box may counter-move each way, as a share of the mirror image. Shown on the picture as the green band around the box; the box cannot enter it, so it always stays inside the image. The steadier you make the picture, the more room it needs: when the room runs out during a head turn, the picture simply turns with your head until you stop.",
        },
        // ---------------------------------------------------------------- The picture itself (2.0; shown on the Mirror tab)
        new()
        {
            Key = "mirror_eye", Group = GroupCrop, Kind = SettingKind.Choice, Default = "right",
            Label = "Eye", Description = "Which eye's picture the mirror shows - in OBS and in the mirror window alike.",
            Choices = [new("right", "Right eye"), new("left", "Left eye")],
        },
        new()
        {
            Key = "output_max_side", Group = GroupCrop, Kind = SettingKind.Choice, Default = "3840",
            Label = "Picture size", Description = "The longest side of the picture handed to OBS and the mirror window is scaled down to this if the crop box is larger. Smaller = less video memory and less work for OBS; the box itself does not change.",
            Choices = [new("3840", "Up to 3840 (4K)"), new("2560", "Up to 2560"), new("1920", "Up to 1920 (1080p)"), new("0", "As large as the box")],
        },
        new()
        {
            Key = "output_fps", Group = GroupCrop, Kind = SettingKind.Choice, Default = "0",
            Label = "Picture rate", Description = "How many pictures a second the mirror makes at most. Fewer = less graphics card time; the game's own frame rate is not affected.",
            Choices = [new("0", "Every game frame"), new("60", "Up to 60"), new("30", "Up to 30")],
        },
        new()
        {
            Key = "gaze_source", Group = GroupCrop, Kind = SettingKind.Choice, Default = "auto",
            Label = "Gaze from", Description = "Where the ring gets the eyes from. The headset's own eye tracking, through OpenXR or SteamVR, is the main source: exact angles, nothing to set up. VRChat is the fallback for when that gaze is not there - eye-tracking software that takes the eye cameras for itself (SRanibro) or only ever feeds VRChat (EyeTrackVR, ALVR, ...) leaves the avatar's eyes as the only trace of where you look, and VRChat sends those out over OSC to the SteamVR helper. Those values are not angles, so the fallback needs the one-time calibration below; VRChat must have OSC on. SRanibro can also send its raw gaze straight to the helper (\"Raw gaze OSC\" in its settings, port 9005): exact angles, full range, no calibration - taken ahead of VRChat whenever it arrives. Headset, else SRanibro, else VRChat (the default) uses the headset whenever it gives gaze and falls back otherwise.",
            Choices = [new("auto", "Headset, else SRanibro, else VRChat"), new("headset", "Headset only"), new("sranibro", "SRanibro raw gaze only"), new("vrchat", "VRChat only")],
        },
    ];

    public static readonly Preset[] Presets =
    [
        new("Subtle", "Thin, translucent ring with a soft skirt. The default.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "40,170,245", ["radius"] = "0.042",
            ["thickness"] = "0.001", ["feather"] = "0.001", ["glow"] = "0.0028", ["glow_strength"] = "0.4",
            ["opacity"] = "0.5", ["solidity"] = "0.4", ["tail_opacity"] = "0.7", ["fill_opacity"] = "0",
        }),
        new("Crisp", "Hairline ring, sharper and more solid. Easier to see, more present.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "40,170,245", ["radius"] = "0.042",
            ["thickness"] = "0.0011", ["feather"] = "0.0005", ["glow"] = "0.0016", ["glow_strength"] = "0.3",
            ["opacity"] = "0.8", ["solidity"] = "0.8", ["tail_opacity"] = "0.7", ["fill_opacity"] = "0",
        }),
        new("Soft band", "Wider, fully soft band of light.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "40,170,245", ["radius"] = "0.042",
            ["thickness"] = "0.002", ["feather"] = "0.002", ["glow"] = "0.003", ["glow_strength"] = "0.5",
            ["opacity"] = "0.55", ["solidity"] = "0.35", ["tail_opacity"] = "0.7", ["fill_opacity"] = "0",
        }),
        new("Neon", "Small, bright cyan ring with a wide halo. Bold.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "0,210,255", ["radius"] = "0.035",
            ["thickness"] = "0.0016", ["feather"] = "0.0016", ["glow"] = "0.009", ["glow_strength"] = "0.45",
            ["opacity"] = "0.7", ["solidity"] = "0.5", ["tail_opacity"] = "0.8", ["fill_opacity"] = "0",
        }),
    ];

    public static Dictionary<string, string> Defaults() => All.ToDictionary(d => d.Key, d => d.Default);

    public static double ParseDouble(string? text, double fallback) =>
        double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : fallback;

    public static (byte R, byte G, byte B) ParseColor(string? text)
    {
        var parts = (text ?? "").Split(',');
        if (parts.Length == 3 && byte.TryParse(parts[0].Trim(), out var r) && byte.TryParse(parts[1].Trim(), out var g) &&
            byte.TryParse(parts[2].Trim(), out var b))
        {
            return (r, g, b);
        }
        return (40, 170, 245);
    }
}
