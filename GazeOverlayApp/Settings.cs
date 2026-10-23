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
    /// <summary>The slider runs the other way: its right end is the lowest value. For a setting named after its effect
    /// where less of the number gives more of the effect (a lower cut-off frequency is steadier).</summary>
    public bool Reversed { get; init; }

    /// <summary>The number a person typed, in the unit the value is shown in ("2.5" for 2.5 % of image), brought onto
    /// the slider's steps and into its range; null = not a number.</summary>
    public double? Parse(string text)
    {
        var digits = new string(text.Trim().TakeWhile(ch => char.IsDigit(ch) || ch is '.' or ',' or '-' or '+').ToArray()).Replace(',', '.');
        if (!double.TryParse(digits, NumberStyles.Float, CultureInfo.InvariantCulture, out var number)) return null;
        var value = Unit is ValueUnit.PercentOfImage or ValueUnit.Percent ? number / 100 : number;
        if (Step > 0) value = Math.Round(value / Step) * Step;
        return Math.Clamp(value, Min, Max);
    }

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
        [GroupLook] = "How the mirror draws the gaze ring. Your stream shows it; your headset does not.",
        [GroupTail] = "The teardrop tail behind the ring while your eyes move (Ghost style).",
        [GroupMotion] = "How the ring follows your eyes: smoothing, blinks, and a held gaze.",
        [GroupPlacement] = "The source of your gaze, and the spot in the picture the ring lands on. Change the placement if the ring sits beside your target.",
        [GroupCrop] = "Hands OBS a box of the eye image, so the OBS source is the box.",
    };

    public static readonly SettingDef[] All =
    [
        // ---------------------------------------------------------------- Look
        new()
        {
            Key = "enabled", Group = GroupLook, Kind = SettingKind.Toggle, Default = "1",
            Label = "Show the gaze ring",
            Description = "Master switch. Turn it off to hide the ring on stream; nothing gets uninstalled.",
        },
        new()
        {
            Key = "style", Group = GroupLook, Kind = SettingKind.Choice, Default = "ghost",
            Label = "Style",
            Description = "**Ghost:** a thin ring that stretches into a teardrop as your eyes move.\n\nThe other styles are plain shapes. Bubble, Solid and Heatmap leave a fading trail.",
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
            Description = "Colour of the ring. Heatmap uses its own colours.",
        },
        new()
        {
            Key = "radius", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.042",
            Min = 0.01, Max = 0.12, Step = 0.001, Unit = ValueUnit.PercentOfImage,
            Label = "Ring size",
            Description = "Radius of the ring, as a share of the full mirrored image's height before any crop.\n\nA larger ring surrounds your target; a small one sits on top of it.",
        },
        new()
        {
            Key = "thickness", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.001",
            Min = 0.0002, Max = 0.006, Step = 0.0001, Unit = ValueUnit.PercentOfImage,
            Label = "Line thickness",
            Description = "Width of the ring's line. Edge softness does not add to it.",
        },
        new()
        {
            Key = "feather", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.001",
            Min = 0, Max = 0.006, Step = 0.0001, Unit = ValueUnit.PercentOfImage, ZeroText = "Hard edge",
            Label = "Edge softness",
            Description = "Softness of the line's edges.\n\nThe fade sits inside the line, so the line's thickness caps it: set both equal for a soft line.\n\n**Bubble, Solid and Dot:** this softens the blob's edge.",
        },
        new()
        {
            Key = "glow", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.0028",
            Min = 0, Max = 0.02, Step = 0.0002, Unit = ValueUnit.PercentOfImage,
            Label = "Glow reach",
            Description = "Reach of the faint glow beyond the line (Ghost style).\n\nUse it to soften a thin line and keep the line thin.",
        },
        new()
        {
            Key = "glow_strength", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.4",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "No glow",
            Label = "Glow strength",
            Description = "Brightness of that glow, as a share of the line's (Ghost style).",
        },
        new()
        {
            Key = "opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.5",
            Min = 0.05, Max = 1, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Brightness",
            Description = "Strength of the ring.\n\n**Spotlight:** it sets how dark the surroundings get.",
        },
        new()
        {
            Key = "blend", Group = GroupLook, Kind = SettingKind.Choice, Default = "screen",
            Label = "Blending",
            Description = "How the ring mixes with the picture.\n\n**Light:** adds the ring to the picture like a glow, and the scene shows through.\n\n**Paint:** draws it on top like ink. Easier to see, and it hides the scene behind it.",
            Choices = [new("screen", "Light (recommended)"), new("normal", "Paint")],
        },
        new()
        {
            Key = "solidity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.4",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Solidity",
            Description = "Light blending.\n\n**At 0 %:** the ring is pure light. It hides nothing and can vanish against a bright sky.\n\n**Higher:** the ring covers a little of the scene, enough to survive bright backgrounds.",
        },
        new()
        {
            Key = "fill_opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0",
            Min = 0, Max = 0.5, Step = 0.01, Unit = ValueUnit.Percent, ZeroText = "Clear",
            Label = "Fill inside the ring",
            Description = "Tints the inside of the ring. Leave it clear to keep your target unobstructed.",
        },
        new()
        {
            Key = "shadow_opacity", Group = GroupLook, Kind = SettingKind.Slider, Default = "0.3",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "None",
            Label = "Dark outline",
            Description = "A soft dark outline that keeps a faint ring readable over bright backgrounds.\n\nFor Plain ring and Dot with Paint blending.",
        },

        // ---------------------------------------------------------------- Tail
        new()
        {
            Key = "tail_opacity", Group = GroupTail, Kind = SettingKind.Slider, Default = "0.7",
            Min = 0, Max = 1, Step = 0.05, Unit = ValueUnit.Percent, ZeroText = "No tail",
            Label = "Tail strength",
            Description = "Brightness of the tail, as a share of the ring's.\n\nLower it if the tail draws more attention than the ring.",
        },
        new()
        {
            Key = "trail_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "200",
            Min = 0, Max = 3000, Step = 10, Unit = ValueUnit.Milliseconds,
            Label = "Tail lag",
            Description = "Ghost style: the time the tail takes to catch up with the ring.\n\n**Longer:** a longer, lazier tail.",
        },
        new()
        {
            Key = "blob_trail_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "450",
            Min = 0, Max = 2000, Step = 10, Unit = ValueUnit.Milliseconds, ZeroText = "No trail",
            Label = "Bubble / Solid trail",
            Description = "Bubble and Solid: how long the trail of shrinking blobs behind your gaze lasts.",
        },
        new()
        {
            Key = "heat_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "2000",
            Min = 200, Max = 6000, Step = 50, Unit = ValueUnit.Milliseconds,
            Label = "Heatmap warm-up",
            Description = "Heatmap: the stare time that takes one spot from blue through green and yellow to red.",
        },
        new()
        {
            Key = "heat_cool_ms", Group = GroupTail, Kind = SettingKind.Slider, Default = "700",
            Min = 100, Max = 5000, Step = 50, Unit = ValueUnit.Milliseconds,
            Label = "Heatmap cool-down",
            Description = "Heatmap: how long a spot takes to cool.\n\n**Look away:** the spot stays put and cools over this time, and a new spot starts cold under your gaze.\n\n**Look back at a warm spot:** it carries on from there.",
        },
        new()
        {
            Key = "tail_max", Group = GroupTail, Kind = SettingKind.Slider, Default = "0.2",
            Min = 0, Max = 0.5, Step = 0.01, Unit = ValueUnit.PercentOfImage,
            Label = "Longest tail",
            Description = "Upper limit for the tail's length at any eye speed.",
        },
        new()
        {
            Key = "tail_space", Group = GroupTail, Kind = SettingKind.Choice, Default = "world",
            Label = "Tail follows",
            Description = "What counts as movement for the tail.\n\n**The world:** the tail marks the spot in the scene your gaze left, so a head turn stretches it too. Use this in VR.\n\n**The screen:** on-screen movement alone counts, like a desktop eye tracker.",
            Choices = [new("world", "The world (recommended for VR)"), new("screen", "The screen")],
        },

        // ---------------------------------------------------------------- Motion
        new()
        {
            Key = "filter", Group = GroupMotion, Kind = SettingKind.Choice, Default = "adaptive",
            Label = "Smoothing",
            Description = "How the ring is smoothed.\n\n**Adaptive:** holds the ring still while your eye rests and lets it jump as you look elsewhere.\n\n**Fixed:** one smoothing throughout.",
            Choices = [new("adaptive", "Adaptive (recommended)"), new("simple", "Fixed")],
        },
        new()
        {
            Key = "filter_min_cutoff", Group = GroupMotion, Kind = SettingKind.Slider, Default = "1.5",
            Min = 0.3, Max = 6, Step = 0.1, Unit = ValueUnit.Hertz, Reversed = true,
            Label = "Steadiness when fixating",
            Description = "Adaptive smoothing.\n\n**Further right:** the ring holds steadier during a fixation, and slow movements lag more.\n\nThe number is the filter's cut-off: steadier means fewer Hz.",
        },
        new()
        {
            Key = "filter_beta", Group = GroupMotion, Kind = SettingKind.Slider, Default = "8",
            Min = 0, Max = 20, Step = 0.5,
            Label = "Snap on eye jumps",
            Description = "Adaptive smoothing.\n\n**Higher:** the smoothing lets go sooner as your eye speeds up, so jumps land faster.\n\n**Lower:** gentler jumps. Lower it if jumps feel abrupt.",
        },
        new()
        {
            Key = "smoothing_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "60",
            Min = 0, Max = 300, Step = 5, Unit = ValueUnit.Milliseconds, ZeroText = "None (jittery)",
            Label = "Fixed smoothing time",
            Description = "Used with Smoothing set to Fixed.",
        },
        new()
        {
            Key = "dwell_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "500",
            Min = 0, Max = 2000, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "Off",
            Label = "Dwell delay",
            Description = "Hold your gaze on one spot this long and the ring tightens and brightens.\n\nViewers can tell reading a gauge from passing over it.",
        },
        new()
        {
            Key = "dwell_shrink", Group = GroupMotion, Kind = SettingKind.Slider, Default = "0.25",
            Min = 0, Max = 0.6, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Dwell tightening",
            Description = "How much the ring shrinks during a dwell.",
        },
        new()
        {
            Key = "hold_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "300",
            Min = 0, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds,
            Label = "Ride through blinks",
            Description = "Keeps the ring at its last position for this long after eye tracking drops out, so a blink does not make it flicker.",
        },
        new()
        {
            Key = "fade_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "150",
            Min = 0, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds, ZeroText = "Instant",
            Label = "Fade in / out",
            Description = "The time the ring takes to appear and to disappear.",
        },
        new()
        {
            Key = "timeout_ms", Group = GroupMotion, Kind = SettingKind.Slider, Default = "250",
            Min = 50, Max = 1000, Step = 25, Unit = ValueUnit.Milliseconds,
            Label = "Tracking lost after",
            Description = "The mirror treats eye tracking as lost after this long without fresh data.\n\nMost setups need no change.",
        },

        // ---------------------------------------------------------------- Placement
        new()
        {
            Key = "projection", Group = GroupPlacement, Kind = SettingKind.Choice, Default = "layer",
            Label = "Placement method",
            Description = "Your gaze starts between your eyes; the mirror shows one eye, a few centimetres to the side.\n\n**Accurate:** places the ring for the eye you record.\n\n**Legacy:** uses Quad-Views-Foveated's estimate, which assumes a target about 1 m away.",
            Choices = [new("layer", "Accurate, per recorded eye (recommended)"), new("producer", "Legacy")],
        },
        new()
        {
            Key = "focus_distance", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = 0, Max = 10, Step = 0.1, Unit = ValueUnit.Meters, ZeroText = "Far away",
            Label = "Usual viewing distance",
            Description = "Accurate placement. One distance cannot suit all targets.\n\n**Far away:** exact for the HUD, other aircraft and the world.\n\n**About 0.7 m:** exact for cockpit switches.",
        },
        new()
        {
            Key = "offset_x", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = -0.05, Max = 0.05, Step = 0.0005, Unit = ValueUnit.PercentOfImage,
            Label = "Nudge right / left",
            Description = "Trim for a ring that sits to one side of your target.\n\nPositive moves it right.",
        },
        new()
        {
            Key = "offset_y", Group = GroupPlacement, Kind = SettingKind.Slider, Default = "0",
            Min = -0.05, Max = 0.05, Step = 0.0005, Unit = ValueUnit.PercentOfImage,
            Label = "Nudge up / down",
            Description = "Trim for a ring that sits above or below your target.\n\nPositive moves it up.",
        },
        new()
        {
            Key = "headset_marker", Group = GroupPlacement, Kind = SettingKind.Toggle, Default = "0",
            Label = "Show a marker inside the headset (calibration)",
            Description = "Draws a bracket marker in the headset at the spot of the stream's ring, so you can centre it from inside VR. OBS must be capturing the mirror.\n\nThis switch is its one control: no key in the game can bring it up.\n\n**Nudge:** with the marker on, Ctrl+Alt+arrow keys nudge the ring in the game (hold Shift for bigger steps). The two Nudge sliders above take the nudge and show it live.\n\n**Judging it:** stare at a fixed target such as the HUD pipper and judge the marker from the corner of your eye. Look at the marker and it runs away from you.\n\nSwitch it off once you are done.",
        },
        new()
        {
            Key = "use_raw_gaze", Group = GroupPlacement, Kind = SettingKind.Toggle, Default = "0",
            Label = "Ignore Quad-Views-Foveated's focus offsets",
            Description = "Legacy placement.\n\nQuad-Views-Foveated lets you shift its sharp region; tick this to keep that shift from moving the ring.",
        },

        // ---------------------------------------------------------------- Crop (its own tab and tool)
        new()
        {
            Key = "crop_enabled", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "0",
            Label = "Crop the picture",
            Description = "Hands OBS the box below in place of the whole eye image.\n\nLeave the crop values of the OBS source at 0.",
        },
        new()
        {
            Key = "crop_aspect", Group = GroupCrop, Kind = SettingKind.Choice, Default = "16:9",
            Label = "Shape",
            Description = "Shape of the box, width:height.\n\n**0:** free shape.",
            Choices =
            [
                new("16:9", "16:9  widescreen"), new("9:16", "9:16  vertical / shorts"), new("1:1", "1:1  square"),
                new("4:3", "4:3"), new("4:5", "4:5  portrait"), new("21:9", "21:9  ultrawide"), new("0", "Free"),
            ],
        },
        new()
        {
            Key = "crop_center_x", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0, Max = 1, Step = 0.0005,
            Label = "Box centre, across", Description = "Centre of the box: 0 = left edge of the image, 1 = right edge.",
        },
        new()
        {
            Key = "crop_center_y", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0, Max = 1, Step = 0.0005,
            Label = "Box centre, down", Description = "Centre of the box: 0 = top edge of the image, 1 = bottom edge.",
        },
        new()
        {
            Key = "crop_height", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.5", Min = 0.02, Max = 1, Step = 0.0005,
            Label = "Box height", Description = "Height of the box, as a share of the image height.\n\nThe shape sets the width.",
        },
        new()
        {
            Key = "crop_width", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0.02, Max = 1, Step = 0.0005,
            Label = "Box width (free shape)", Description = "Width of the box, as a share of the image width.\n\nUsed with the free shape.",
        },
        new()
        {
            Key = "crop_follow", Group = GroupCrop, Kind = SettingKind.Choice, Default = "off",
            Label = "Follow my gaze",
            Description = "A 16:9 box covers about half the height of the eye image, so a look far up or down takes your gaze out of frame.\n\n**Off:** the box stays where you put it.\n\n**Up / down:** the box glides after your eyes like a camera operator. It rests while you look around the middle, and near the top or bottom it moves far enough to keep your gaze in frame.\n\nEye movement drives it; a head turn moves the whole picture anyway.",
            Choices = [new("off", "Off: the box stays where I put it"), new("vertical", "Up / down")],
        },
        new()
        {
            Key = "crop_follow_deadzone", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.4", Min = 0, Max = 0.9, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Still zone",
            Description = "The middle share of the box height in which your gaze moves nothing. Two dashed lines mark it on the picture.\n\n**Smaller:** the box reacts sooner and keeps your target closer to the middle.\n\n**Larger:** a calmer picture.",
        },
        new()
        {
            Key = "crop_follow_reach", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0, Max = 2, Step = 0.05, Unit = ValueUnit.Percent,
            Label = "Reach",
            Description = "How far the box may travel up or down from where you put it, as a share of its own height (100 % = one box height each way).\n\nThe tinted area above and below the box shows it. The box stays inside the image.",
        },
        new()
        {
            Key = "crop_follow_glide_ms", Group = GroupCrop, Kind = SettingKind.Slider, Default = "500", Min = 0, Max = 2500, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "Instant",
            Label = "Glide",
            Description = "How softly the box moves, out and home.\n\n**Longer:** calmer for viewers, and the box lags further behind your eyes.\n\n**Instant:** the box jumps.",
        },
        new()
        {
            Key = "crop_follow_return", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "1",
            Label = "Return home",
            Description = "Sends the box back to where you put it once your gaze allows.\n\n**Off:** the box stays where your gaze last pushed it.",
        },
        new()
        {
            Key = "crop_follow_home_ms", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1200", Min = 0, Max = 6000, Step = 50, Unit = ValueUnit.Milliseconds, ZeroText = "At once",
            Label = "Return after",
            Description = "How long your gaze has to rest inside the still zone before the box heads home.\n\n**At once:** the box stays as close to home as your gaze allows.",
        },
        new()
        {
            Key = "stabilize", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "0",
            Label = "Steady the picture",
            Description = "Takes small head movements (tremor, engine shake, breathing) out of the mirror picture.\n\n**How:** the crop box counter-moves them inside the room around it, at no extra cost. It needs the crop on, and it has as much travel as the box has room.\n\nHead turns you mean still come through. A sideways tilt has its own choice: Tilt.",
        },
        new()
        {
            Key = "stabilize_min_cutoff", Group = GroupCrop, Kind = SettingKind.Slider, Default = "1", Min = 0.1, Max = 5, Step = 0.1, Unit = ValueUnit.Hertz, Reversed = true,
            Label = "Steadiness",
            Description = "The strength of the smoothing of head movement.\n\n**Further right:** steadier, like a camera on a gimbal. The picture trails further behind your head and needs more room.\n\nThe number is the filter's cut-off, so steadier means fewer Hz.\n\n**From about 1 Hz up:** it takes out tremor, and you will hardly see it work.\n\n**0.2 to 0.4 Hz:** you can watch it smooth ordinary head sway.",
        },
        new()
        {
            Key = "stabilize_beta", Group = GroupCrop, Kind = SettingKind.Slider, Default = "8", Min = 0, Max = 40, Step = 1,
            Label = "Follow quick turns",
            Description = "How fast the steadying lets go as you turn your head on purpose.\n\n**Higher:** quick turns come through with less lag.\n\n**Lower:** smoother; the picture trails behind and catches up.",
        },
        new()
        {
            Key = "stabilize_room", Group = GroupCrop, Kind = SettingKind.Slider, Default = "0.02", Min = 0.01, Max = 0.1, Step = 0.01, Unit = ValueUnit.Percent,
            Label = "Room to move",
            Description = "How far the box may counter-move each way, as a share of the eye image.\n\nThe green band around the box shows it. The box stays out of the band, and so inside the image.\n\nA steadier picture needs more room. Once the room runs out during a head turn, the picture turns with your head until you stop.",
        },
        new()
        {
            Key = "stabilize_tilt", Group = GroupCrop, Kind = SettingKind.Choice, Default = "smooth",
            Label = "Tilt",
            Description = "What the picture does when you tilt your head sideways.\n\n**Follows the head:** the picture tilts with you.\n\n**Smoothed:** the steadying takes a quick tilt out. The turn shares the room with the movement, so a slow lean still comes through.\n\n**Level:** the box turns against your whole tilt, so the horizon stays level, steadying on or off. It turns only as far as the eye image reaches and eases off before it gets there; beyond that the horizon tilts with you rather than show black.\n\nA large crop box has little room to turn.",
            Choices = [new("head", "Follows the head"), new("smooth", "Smoothed (quick tilts out)"), new("level", "Level (horizon locked)")],
        },
        // ---------------------------------------------------------------- The picture itself (2.0; shown on the Mirror tab)
        new()
        {
            Key = "picture_source", Group = GroupCrop, Kind = SettingKind.Choice, Default = "steamvr",
            Label = "Capture",
            Description = "For OpenXR games that run through SteamVR. Saved with the crop profile, so one game can stay clean.\n\n**SteamVR capture (the default):** the SteamVR helper takes the picture SteamVR shows in the headset. SteamVR's overlays come with it (OVR Toolkit, fpsVR, the dashboard and its desktop view, notifications), and so do the app's own aids in the headset (the countdown, the view-edge blocks, the marker).\n\n**OpenXR capture:** the mirror layer takes the picture inside the game. SteamVR's overlays stay out of the stream. Panels that OpenXR tools such as OpenKneeboard add come along while Show tool panels is on.\n\nSteamVR games always use SteamVR capture. Native OpenXR runtimes, such as Pimax Play's, always use OpenXR capture.",
            Choices = [new("steamvr", "SteamVR capture (with overlays)"), new("game", "OpenXR capture (no SteamVR overlays)")],
        },
        new()
        {
            Key = "show_panels", Group = GroupCrop, Kind = SettingKind.Toggle, Default = "1",
            Label = "Show tool panels",
            Description = "With OpenXR capture: panels that other OpenXR tools lay over the game, such as OpenKneeboard's kneeboards, appear in the picture where they sit in the headset.\n\nThe tool's layer must come before the mirror layer in the OpenXR layer list on the Status page; its order check sees to that.\n\nTurn it off to keep them off the stream; the headset still shows them.",
        },
        new()
        {
            Key = "mirror_eye", Group = GroupCrop, Kind = SettingKind.Choice, Default = "right",
            Label = "Eye", Description = "The eye whose picture the mirror shows, in OBS and in the mirror window.",
            Choices = [new("right", "Right eye"), new("left", "Left eye")],
        },
        new()
        {
            Key = "output_max_side", Group = GroupCrop, Kind = SettingKind.Choice, Default = "3840",
            Label = "Picture size", Description = "Caps the longest side of the picture for OBS and the mirror window; the mirror scales a larger crop box down to it.\n\nSmaller saves video memory and work in OBS. The box itself stays as it is.",
            Choices = [new("3840", "Up to 3840 (4K)"), new("2560", "Up to 2560"), new("1920", "Up to 1920 (1080p)"), new("0", "As large as the box")],
        },
        new()
        {
            Key = "output_fps", Group = GroupCrop, Kind = SettingKind.Choice, Default = "0",
            Label = "Picture rate", Description = "The most pictures a second the mirror makes.\n\nFewer saves graphics card time; the game's frame rate stays as it is.",
            Choices = [new("0", "Every game frame"), new("60", "Up to 60"), new("30", "Up to 30")],
        },
        new()
        {
            Key = "gaze_source", Group = GroupCrop, Kind = SettingKind.Choice, Default = "auto",
            Label = "Gaze from", Description = "The source of the ring's gaze.\n\n**Headset:** the headset's own eye tracking, through OpenXR or SteamVR. Exact angles, nothing to set up. It comes first.\n\n**Raw gaze over OSC:** any program that has the tracker's gaze can send it to the helper's gaze input port as a direction per eye (SRanibro does, with \"Raw gaze OSC\" in its settings). Exact angles, no calibration, and the ring takes it ahead of VRChat.\n\n**VRChat:** the fallback. Some eye-tracking software takes the eye cameras for itself (SRanibro) or feeds VRChat alone (EyeTrackVR, ALVR); the avatar's eyes are then the one trace of where you look, and VRChat sends them over OSC to the SteamVR helper. Those values are not angles, so this needs the one-time calibration below, and VRChat needs OSC on.\n\n**Headset, else raw gaze, else VRChat (the default):** uses the headset while it gives gaze and falls back otherwise.\n\n**Fixed centre:** for headsets without eye tracking. The ring sits where straight ahead lands in each eye and stays there. Quad-Views-Foveated puts its sharp area at that same spot when it has no eye tracking, so the ring takes that area's size from its settings.\n\nThe app greys this choice out once it knows your headset gives gaze.",
            Choices = [new("auto", "Headset, else raw gaze, else VRChat"), new("headset", "Headset only"), new("sranibro", "Raw gaze over OSC only"), new("vrchat", "VRChat only"), new("fixed", "Fixed centre (no eye tracking)")],
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
        new("Crisp", "Hairline ring, sharp and solid. Easier to see.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "40,170,245", ["radius"] = "0.042",
            ["thickness"] = "0.0011", ["feather"] = "0.0005", ["glow"] = "0.0016", ["glow_strength"] = "0.3",
            ["opacity"] = "0.8", ["solidity"] = "0.8", ["tail_opacity"] = "0.7", ["fill_opacity"] = "0",
        }),
        new("Soft band", "A wider, soft band of light.", new()
        {
            ["style"] = "ghost", ["blend"] = "screen", ["color"] = "40,170,245", ["radius"] = "0.042",
            ["thickness"] = "0.002", ["feather"] = "0.002", ["glow"] = "0.003", ["glow_strength"] = "0.5",
            ["opacity"] = "0.55", ["solidity"] = "0.35", ["tail_opacity"] = "0.7", ["fill_opacity"] = "0",
        }),
        new("Neon", "Small, bright cyan ring with a wide halo.", new()
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
