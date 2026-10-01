using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace GazeOverlay;

/// <summary>What Quad-Views-Foveated wrote to its log the last time a game started. Read once, on demand - never watched.</summary>
public sealed class QuadViewsSession
{
    public DateTime? When { get; private set; }
    public string RuntimeName { get; private set; } = "";
    public string SystemName { get; private set; } = "";
    public string AppName { get; private set; } = "";
    public string ExeName { get; private set; } = "";
    /// <summary>The settings files the layer looked for, in the order it read them (the shipped one, then the user's).</summary>
    public List<string> ConfigPaths { get; } = [];
    public bool? EyeTracking { get; private set; }
    /// <summary>The headset's full per-eye resolution, which every pixel count is derived from.</summary>
    public int StereoWidth { get; private set; }
    public int StereoHeight { get; private set; }
    public long? QuadPixels { get; private set; }
    public string? FocusResolution { get; private set; }
    public string? PeripheralResolution { get; private set; }

    public bool HasResolution => StereoWidth > 0 && StereoHeight > 0;
    public long StereoPixels => 2L * StereoWidth * StereoHeight;

    public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Quad-Views-Foveated");
    public static string LogPath => Path.Combine(Folder, "Quad-Views-Foveated.log");

    private static readonly Regex Prefix = new(@"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) [+-]\d{4}: ", RegexOptions.Compiled);

    public static QuadViewsSession? Read(string path)
    {
        string[] lines;
        try
        {
            if (!File.Exists(path)) return null;
            // The layer may have the log open for writing while a game runs.
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            lines = reader.ReadToEnd().Split('\n');
        }
        catch
        {
            return null;
        }

        var session = new QuadViewsSession();
        foreach (var rawLine in lines)
        {
            var line = rawLine.TrimEnd('\r');
            var prefix = Prefix.Match(line);
            if (!prefix.Success) continue;
            var text = line[prefix.Length..];

            if (text.Contains(" layer (") && text.EndsWith("is active"))
            {
                // A new game start: forget what an earlier one in the same file said.
                session = new QuadViewsSession();
                if (DateTime.TryParseExact(prefix.Groups[1].Value, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var when)) session.When = when;
            }
            else if (After(text, "Using OpenXR runtime: ") is { } runtime) session.RuntimeName = runtime;
            else if (After(text, "Using OpenXR system: ") is { } system) session.SystemName = system;
            else if (After(text, "Application: ") is { } application)
            {
                var open = application.LastIndexOf(" (", StringComparison.Ordinal);
                session.AppName = open > 0 ? application[..open] : application;
                session.ExeName = open > 0 ? application[(open + 2)..].TrimEnd(')') : "";
            }
            else if (After(text, "Trying to locate configuration file at '") is { } configPath)
            {
                var close = configPath.LastIndexOf('\'');
                if (close > 0) session.ConfigPaths.Add(configPath[..close]);
            }
            else if (After(text, "Eye tracking is ") is { } tracking) session.EyeTracking = tracking == "supported";
            else if (After(text, "Recommended focus resolution: ") is { } focus) session.FocusResolution = focus.Split(' ')[0];
            else if (After(text, "Recommended peripheral resolution: ") is { } peripheral) session.PeripheralResolution = peripheral.Split(' ')[0];
            else if (text.TrimStart().StartsWith("Stereo pixel count was:"))
            {
                var size = Regex.Match(text, @"\((\d+)x(\d+)\)");
                if (size.Success)
                {
                    session.StereoWidth = int.Parse(size.Groups[1].Value, CultureInfo.InvariantCulture);
                    session.StereoHeight = int.Parse(size.Groups[2].Value, CultureInfo.InvariantCulture);
                }
            }
            else if (After(text.TrimStart(), "Quad views pixel count is: ") is { } quad)
            {
                // The layer prints it with the PC's thousands separators.
                var digits = new string(quad.Where(char.IsDigit).ToArray());
                if (long.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out var count)) session.QuadPixels = count;
            }
        }
        return session.When == null && session.RuntimeName.Length == 0 ? null : session;
    }

    private static string? After(string text, string start) => text.StartsWith(start, StringComparison.Ordinal) ? text[start.Length..] : null;
}

/// <summary>The values the layer ends up with, and which section of which file had the last word on each.</summary>
public sealed record QuadViewsEffective(Dictionary<string, double> Values, Dictionary<string, string> DecidedBy)
{
    public double this[string key] => Values[key];
}

/// <summary>
/// Quad-Views-Foveated's settings.cfg, edited line by line so comments, sections and keys this app does not know survive.
/// The parsing rules copy the layer's own (ParseConfigurationStatement in its layer.cpp) - including that nothing is trimmed.
/// </summary>
public sealed class QuadViewsFile
{
    public static string DefaultPath => Path.Combine(QuadViewsSession.Folder, "settings.cfg");

    public const string AddedMarker = "# Added by VR Gaze Mirror: set here, above the headset sections, so they apply under every OpenXR runtime.";
    /// <summary>What the marker said while the product was called OpenXR Gaze Overlay (up to 0.9.0).</summary>
    private const string OldAddedMarker2 = "# Added by QuadViews Gaze Mirror: set here, above the headset sections, so they apply under every OpenXR runtime.";
    private const string OldAddedMarker = "# Added by OpenXR Gaze Overlay: set here, above the headset sections, so they apply under every OpenXR runtime.";

    public List<string> Lines { get; private init; } = [];
    public bool Existed { get; private init; }

    /// <summary>The layer's built-in values (the members' initialisers in its layer.cpp).</summary>
    public static readonly Dictionary<string, double> BuiltIn = new()
    {
        ["peripheral_multiplier"] = 0.5, ["focus_multiplier"] = 1.0,
        ["horizontal_fixed_section"] = 0.5, ["vertical_fixed_section"] = 0.45,
        ["horizontal_focus_section"] = 0.35, ["vertical_focus_section"] = 0.35,
        ["horizontal_fixed_offset"] = 0, ["vertical_fixed_offset"] = 0, ["vertical_focus_offset"] = 0,
        ["smoothen_focus_view_edges"] = 0.2, ["sharpen_focus_view"] = 0.7,
        ["turbo_mode"] = 1, ["debug_eye_gaze"] = 0, ["debug_focus_view"] = 0,
    };

    public static QuadViewsFile Load(string path)
    {
        if (File.Exists(path))
        {
            return new QuadViewsFile { Lines = [.. File.ReadAllText(path).Split('\n').Select(l => l.TrimEnd('\r'))], Existed = true };
        }
        // The file shipped with the layer is read first and switches Turbo off where it is unsafe; a user file that sets
        // turbo_mode for everyone would undo that, so a new file repeats those two guards.
        return new QuadViewsFile
        {
            Lines =
            [
                "# Quad-Views-Foveated settings, written by VR Gaze Mirror.",
                "# Settings in this first part apply to every headset and OpenXR runtime.",
                "",
                "[Varjo]",
                "# Turbo mode is incompatible with Varjo's deferred swapchain release. Use OpenXR Toolkit Turbo instead.",
                "turbo_mode=0",
                "",
                "[SteamVR]",
                "# Turbo Mode causes unexplained errors with SteamVR.",
                "turbo_mode=0",
                "",
            ],
        };
    }

    public static QuadViewsFile Parse(string text) => new() { Lines = [.. text.Split('\n').Select(l => l.TrimEnd('\r'))], Existed = true };

    public string Text => string.Join("\r\n", Lines);

    private static bool IsComment(string line) => line.StartsWith('#') || line.StartsWith("//", StringComparison.Ordinal);
    private static bool IsSection(string line) => line.Length > 1 && line[0] == '[' && line[^1] == ']';
    private static bool IsAppSection(string? section) => section != null && (section.StartsWith("[app:", StringComparison.Ordinal) || section.StartsWith("[exe:", StringComparison.Ordinal));

    /// <summary>Every name=value line with the section header it sits under (null = the common part at the top).</summary>
    public IEnumerable<(int Index, string? Section, string Key, string Value)> Entries()
    {
        string? section = null;
        for (var i = 0; i < Lines.Count; i++)
        {
            var line = Lines[i];
            if (line.Length == 0 || IsComment(line)) continue;
            if (IsSection(line)) { section = line; continue; }
            var separator = line.IndexOf('=');
            if (separator > 0) yield return (i, section, line[..separator], line[(separator + 1)..]);
        }
    }

    /// <summary>What the file means a setting to be: the first place it is set outside per-game sections.</summary>
    public string? Intended(string key) =>
        Entries().Where(e => e.Key == key && !IsAppSection(e.Section)).Select(e => e.Value).FirstOrDefault();

    /// <summary>The value set in the common part at the top of the file, which every runtime reads.</summary>
    public string? InCommon(string key) => Entries().Where(e => e.Section == null && e.Key == key).Select(e => e.Value).LastOrDefault();

    /// <summary>
    /// The file as Apply would write it. Controls the user changed are written everywhere; untouched ones are only copied
    /// into the common part when they are missing there, so a value the file already means reaches every runtime.
    /// </summary>
    public static QuadViewsFile Plan(QuadViewsFile current, IReadOnlyDictionary<string, double> values, IReadOnlySet<string> changed)
    {
        var planned = new QuadViewsFile { Lines = [.. current.Lines], Existed = current.Existed };
        foreach (var setting in QuadViewsSetting.All)
        {
            var value = values.GetValueOrDefault(setting.Id);
            foreach (var key in setting.Keys)
            {
                if (changed.Contains(setting.Id)) planned.Set(key, setting.IsToggle ? (value != 0 ? "1" : "0") : setting.ToFile(value), setting.CommonOnly);
                else if (!setting.CommonOnly && planned.InCommon(key) == null && current.Intended(key) is { } intended) planned.Set(key, intended, commonOnly: true);
            }
        }
        return planned;
    }

    /// <summary>
    /// Sets a key in the common part (adding it there if needed) and, unless commonOnly, wherever a headset section repeats it.
    /// Per-game sections ([app:...], [exe:...]) are somebody's deliberate exception and are left alone.
    /// </summary>
    public void Set(string key, string value, bool commonOnly)
    {
        var inCommon = false;
        foreach (var entry in Entries().ToList())
        {
            if (entry.Key != key) continue;
            if (entry.Section == null) inCommon = true;
            else if (commonOnly || IsAppSection(entry.Section)) continue;
            Lines[entry.Index] = $"{key}={value}";
        }
        if (inCommon) return;

        // After the last setting of the common part; in a file that has none, just above the first section.
        var firstSection = Lines.FindIndex(IsSection);
        if (firstSection < 0) firstSection = Lines.Count;
        var insertAt = firstSection;
        while (insertAt > 0 && (Lines[insertAt - 1].Length == 0 || IsComment(Lines[insertAt - 1]))) insertAt--;
        if (insertAt == 0) insertAt = firstSection;

        if (!Lines.Contains(AddedMarker) && !Lines.Contains(OldAddedMarker) && !Lines.Contains(OldAddedMarker2))
        {
            Lines.Insert(insertAt++, "");
            Lines.Insert(insertAt++, AddedMarker);
        }
        Lines.Insert(insertAt++, $"{key}={value}");
        if (insertAt < Lines.Count && Lines[insertAt].Length != 0) Lines.Insert(insertAt, "");
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp";
        File.WriteAllText(temp, Text, new UTF8Encoding(false));
        File.Move(temp, path, overwrite: true);
    }

    /// <summary>
    /// Works out what the layer ends up with, the way it does: built-in values, then each file top to bottom, where a
    /// [section] only counts if its name is part of the runtime's or system's name ([app:]/[exe:]: of the game's).
    /// Without a session (no game has run yet) only the common parts count.
    /// </summary>
    public static QuadViewsEffective Evaluate(IEnumerable<(string Name, QuadViewsFile File)> files, QuadViewsSession? session)
    {
        var values = new Dictionary<string, double>(BuiltIn);
        var decidedBy = new Dictionary<string, string>();
        foreach (var (name, file) in files)
        {
            foreach (var entry in file.Entries())
            {
                if (!BuiltIn.ContainsKey(entry.Key) || !SectionApplies(entry.Section, session)) continue;
                if (!double.TryParse(entry.Value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number)) continue;
                values[entry.Key] = Clamp(entry.Key, number);
                decidedBy[entry.Key] = $"{entry.Section ?? "the common part"} of {name}";
            }
        }
        return new QuadViewsEffective(values, decidedBy);
    }

    private static bool SectionApplies(string? section, QuadViewsSession? session)
    {
        if (section == null) return true;
        if (session == null) return false;
        if (section.StartsWith("[app:", StringComparison.Ordinal)) return session.AppName.Contains(section[5..^1], StringComparison.Ordinal);
        if (section.StartsWith("[exe:", StringComparison.Ordinal)) return session.ExeName.Contains(section[5..^1], StringComparison.Ordinal);
        var name = section[1..^1];
        return session.RuntimeName.Contains(name, StringComparison.Ordinal) || session.SystemName.Contains(name, StringComparison.Ordinal);
    }

    /// <summary>The same limits the layer applies when it reads a value.</summary>
    public static double Clamp(string key, double value) => key switch
    {
        "peripheral_multiplier" or "focus_multiplier" => Math.Max(0.1, value),
        "horizontal_fixed_section" or "vertical_fixed_section" or "horizontal_focus_section" or "vertical_focus_section" => Math.Clamp(value, 0.1, 0.9),
        "horizontal_fixed_offset" or "vertical_fixed_offset" or "vertical_focus_offset" => Math.Clamp(value, -0.5, 0.5),
        "smoothen_focus_view_edges" => Math.Clamp(value, 0, 0.5),
        "sharpen_focus_view" => Math.Clamp(value, 0, 1),
        _ => value != 0 ? 1 : 0,
    };
}

/// <summary>The layer's own arithmetic for the image sizes it asks the game to render (xrEnumerateViewConfigurationViews).</summary>
public static class QuadViewsPixels
{
    public sealed record Result(int PeripheralWidth, int PeripheralHeight, int FocusWidth, int FocusHeight)
    {
        public long Total => 2L * ((long)PeripheralWidth * PeripheralHeight + (long)FocusWidth * FocusHeight);
    }

    public static Result Compute(int stereoWidth, int stereoHeight, double peripheralMultiplier, double focusMultiplier, double horizontalSection, double verticalSection)
    {
        // Single precision and truncation on purpose: that is what the layer does, and it decides the last pixel.
        float peripheral = (float)peripheralMultiplier, focus = (float)focusMultiplier;
        return new Result(
            Align2((uint)(float)(peripheral * stereoWidth)), Align2((uint)(float)(peripheral * stereoHeight)),
            Align2((uint)(float)((float)(focus * (float)horizontalSection) * stereoWidth)),
            Align2((uint)(float)((float)(focus * (float)verticalSection) * stereoHeight)));
    }

    /// <summary>Share of the full-resolution pixel count, for when the headset's resolution is not known yet.</summary>
    public static double Fraction(double peripheralMultiplier, double focusMultiplier, double horizontalSection, double verticalSection) =>
        peripheralMultiplier * peripheralMultiplier + focusMultiplier * focusMultiplier * horizontalSection * verticalSection;

    private static int Align2(uint n) => (int)((n + 1) & ~1u);
}

public enum QuadViewsScale { Fraction, PixelShare }

/// <summary>One control of the Quad Views tab. A slider can drive two keys: the eye-tracked one and its fixed fallback.</summary>
public sealed class QuadViewsSetting
{
    public required string Id { get; init; }
    public required string Label { get; init; }
    public required string Suffix { get; init; }
    public required string Description { get; init; }
    public required string[] Keys { get; init; }
    public bool IsToggle { get; init; }
    /// <summary>Only ever written in the common part: headset sections that override it do so on purpose (Turbo).</summary>
    public bool CommonOnly { get; init; }
    public double Min { get; init; }
    public double Max { get; init; }
    public double Step { get; init; } = 1;
    public QuadViewsScale Scale { get; init; }
    public bool NeedsRestartNote { get; init; }

    /// <summary>File value -> what the slider shows (percent).</summary>
    public double ToSlider(double fileValue) => Scale == QuadViewsScale.PixelShare ? fileValue * fileValue * 100 : fileValue * 100;

    /// <summary>Slider percent -> the text written to the file. Resolution is a share of the pixel count, so the multiplier
    /// (which scales width and height alike) is its square root - the same logic as TallyMouse's QuadViews Companion.</summary>
    public string ToFile(double sliderValue) => Scale == QuadViewsScale.PixelShare
        ? Math.Round(Math.Sqrt(sliderValue / 100), 3).ToString("0.###", CultureInfo.InvariantCulture)
        : Math.Round(sliderValue / 100, 2).ToString("0.##", CultureInfo.InvariantCulture);

    public double ToFileNumber(double sliderValue) => double.Parse(ToFile(sliderValue), CultureInfo.InvariantCulture);

    public static readonly QuadViewsSetting[] All =
    [
        new()
        {
            Id = "focus_h", Label = "Horizontal focus size", Suffix = "% of FOV", Min = 10, Max = 90,
            Keys = ["horizontal_focus_section", "horizontal_fixed_section"],
            Description = "Width of the sharp region that follows your eyes, as a share of the headset's field of view. Smaller is faster; too small and you see the blurry surround.",
        },
        new()
        {
            Id = "focus_v", Label = "Vertical focus size", Suffix = "% of FOV", Min = 10, Max = 90,
            Keys = ["vertical_focus_section", "vertical_fixed_section"],
            Description = "Height of the sharp region.",
        },
        new()
        {
            Id = "offset_v", Label = "Vertical focus offset", Suffix = "% of FOV", Min = -50, Max = 50,
            Keys = ["vertical_focus_offset", "vertical_fixed_offset"],
            Description = "Moves the sharp region up (positive) or down. Use it if the region keeps sitting above or below your gaze.",
        },
        new()
        {
            Id = "focus_res", Label = "Foveate resolution", Suffix = "% of native", Min = 50, Max = 400, Scale = QuadViewsScale.PixelShare,
            Keys = ["focus_multiplier"],
            Description = "Pixel count of the sharp region compared to the headset's normal resolution. Above 100 % is supersampling.",
        },
        new()
        {
            Id = "peripheral_res", Label = "Peripheral resolution", Suffix = "% of native", Min = 1, Max = 100, Scale = QuadViewsScale.PixelShare,
            Keys = ["peripheral_multiplier"],
            Description = "Pixel count of the area outside the sharp region. Most of the saving comes from here.",
        },
        new()
        {
            Id = "sharpen", Label = "Foveate sharpness", Suffix = "%", Min = 0, Max = 100, Step = 5,
            Keys = ["sharpen_focus_view"],
            Description = "Sharpening filter on the sharp region. 0 switches it off (and saves a little GPU time).",
        },
        new()
        {
            Id = "transition", Label = "Transition thickness", Suffix = "%", Min = 0, Max = 50,
            Keys = ["smoothen_focus_view_edges"],
            Description = "Width of the blend between the sharp region and the surround. 0 gives a hard edge.",
        },
        new()
        {
            Id = "turbo", Label = "Turbo mode", Suffix = "", IsToggle = true, CommonOnly = true,
            Keys = ["turbo_mode"],
            Description = "Lets the game start its next frame early, which can raise the frame rate. Quad-Views-Foveated switches it off for SteamVR and Varjo runtimes because it causes errors there, and this app leaves those exceptions in place.",
        },
        new()
        {
            Id = "debug_gaze", Label = "Debug: show eye gaze", Suffix = "", IsToggle = true,
            Keys = ["debug_eye_gaze"],
            Description = "Draws a small square in the headset where Quad-Views-Foveated thinks you are looking.",
        },
        new()
        {
            Id = "debug_focus", Label = "Debug: show focus view", Suffix = "", IsToggle = true,
            Keys = ["debug_focus_view"],
            Description = "Tints the sharp region in the headset so you can see its size and position.",
        },
    ];

    /// <summary>Slider values by Id. They never touch Turbo or the debug switches.</summary>
    public static readonly (string Name, string Description, Dictionary<string, double> Values)[] Presets =
    [
        // Read off QuadViews Companion 1.0.17. Its resolution presets are round multipliers (0.75, 0.2) that it shows as a
        // share of the pixel count (56 %, 4 %), so they are given here unrounded to write the same multipliers.
        ("Give me FPS", "The Companion's frame-rate preset: a small sharp region, low resolutions, strong sharpening.", new()
        {
            ["focus_h"] = 30, ["focus_v"] = 30, ["offset_v"] = -10, ["focus_res"] = 56.25, ["peripheral_res"] = 4,
            ["sharpen"] = 75, ["transition"] = 20,
        }),
        ("TM's Favorite", "TallyMouse's own pick in the Companion: a supersampled sharp region with a low-resolution surround.", new()
        {
            ["focus_h"] = 33, ["focus_v"] = 32, ["offset_v"] = -10, ["focus_res"] = 200, ["peripheral_res"] = 4.84, // 0.22, as the Companion writes it
            ["sharpen"] = 80, ["transition"] = 30,
        }),
        // No values of its own: the window fills it with what the layer does when there is no user file at all - the file
        // shipped with the layer, read for this PC's runtime. (The Companion's button reads that shipped file too, from
        // mbucchia's original install folder - which is why it fails once this package has replaced that install.)
        ("QV defaults", "Quad-Views-Foveated's own behaviour on this PC with nothing set: its shipped settings file, read for your headset's runtime.", new()),
    ];
}
