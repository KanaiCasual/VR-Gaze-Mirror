using System.IO;
using System.Text;

namespace GazeOverlay;

/// <summary>Reads and writes the gaze overlay config that the OBSMirror layer reads at start, and again whenever LiveLink tells it to.</summary>
public static class ConfigFile
{
    public static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "XR_APILAYER_NOVENDOR_OBSMirror_gaze.cfg");

    /// <summary>Keys the app fills in by itself (no row of their own): the sharp area for Gaze from = Fixed centre.</summary>
    public static readonly string[] Managed = ["fixed_focus_width", "fixed_focus_height", "fixed_focus_offset_x", "fixed_focus_offset_y"];

    /// <summary>Known keys get their stored value or the default; keys this app does not know are kept as-is.</summary>
    public static Dictionary<string, string> Load(string path)
    {
        var values = Settings.Defaults();
        if (!File.Exists(path)) return values;

        foreach (var rawLine in File.ReadAllLines(path))
        {
            var line = rawLine;
            var comment = line.IndexOf('#');
            if (comment >= 0) line = line[..comment];
            var separator = line.IndexOf('=');
            if (separator <= 0) continue;
            var key = line[..separator].Trim();
            var value = line[(separator + 1)..].Trim();
            if (key.Length > 0) values[key] = value;
        }
        return values;
    }

    public static void Save(string path, IReadOnlyDictionary<string, string> values)
    {
        var text = new StringBuilder();
        text.AppendLine("# Eye gaze indicator drawn on the OBS mirror only (never visible in the headset).");
        text.AppendLine("# Written by VR Gaze Mirror (GazeMirror.exe). You can edit it by hand too:");
        text.AppendLine("# the game reads it when it starts. It never polls this file while running: edits go live through the");
        text.AppendLine("# settings app (or, for a hand edit, while the settings app is open).");

        var known = new HashSet<string>();
        foreach (var group in Settings.Groups)
        {
            text.AppendLine();
            text.AppendLine($"# ===== {group} =====");
            foreach (var def in Settings.All.Where(d => d.Group == group))
            {
                known.Add(def.Key);
                text.AppendLine();
                foreach (var commentLine in Wrap($"{def.Label}: {def.Description}", 96))
                {
                    text.AppendLine("# " + commentLine);
                }
                if (def.Kind == SettingKind.Choice)
                {
                    text.AppendLine("#   options: " + string.Join(", ", def.Choices.Select(c => c.Value)));
                }
                text.AppendLine($"{def.Key}={(values.TryGetValue(def.Key, out var v) ? v : def.Default)}");
            }
        }

        // Keys the app fills in by itself.
        text.AppendLine();
        text.AppendLine("# ===== Filled in by the app =====");
        text.AppendLine("# Gaze from = Fixed centre: the sharp area Quad-Views-Foveated uses without eye tracking (its fixed sections and");
        text.AppendLine("# offsets, copied from its settings; 0 = none known, the ring keeps its own size). The ring is drawn that size.");
        foreach (var key in Managed)
        {
            known.Add(key);
            text.AppendLine($"{key}={(values.TryGetValue(key, out var managed) ? managed : "0")}");
        }

        // ("mouse_": settings of the gaze mouse, an experiment that was taken out again - not carried along.)
        var unknown = values.Where(kv => !known.Contains(kv.Key) && !kv.Key.StartsWith("mouse_", StringComparison.Ordinal)).ToList();
        if (unknown.Count > 0)
        {
            text.AppendLine();
            text.AppendLine("# ===== Other (not managed by the app) =====");
            foreach (var kv in unknown) text.AppendLine($"{kv.Key}={kv.Value}");
        }

        // Write-then-swap, so the layer never reads a half-written file.
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp";
        File.WriteAllText(temp, text.ToString(), new UTF8Encoding(false));
        File.Move(temp, path, overwrite: true);
    }

    private static IEnumerable<string> Wrap(string text, int width)
    {
        var line = new StringBuilder();
        foreach (var word in text.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            if (line.Length > 0 && line.Length + 1 + word.Length > width)
            {
                yield return line.ToString();
                line.Clear();
            }
            if (line.Length > 0) line.Append(' ');
            line.Append(word);
        }
        if (line.Length > 0) yield return line.ToString();
    }
}
