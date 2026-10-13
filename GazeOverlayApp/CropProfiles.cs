using System.IO;
using System.Text;

namespace GazeOverlay;

/// <summary>One saved framing: a name, the game it is for (program file such as "DCS.exe", or empty = by hand only) and the crop keys.</summary>
public sealed class CropProfile
{
    public required string Name { get; set; }
    public string Game { get; set; } = "";
    public Dictionary<string, string> Values { get; } = new(StringComparer.Ordinal);
}

/// <summary>
/// Crop profiles: as many framings as the user likes, each optionally tied to a game. The mirror layer and the SteamVR
/// helper read the same file when a game starts and apply the profile made for it on top of the settings file - so the
/// switch happens in the game's own start-up, and nothing in this app has to watch for games.
/// File: %LocalAppData%\GazeMirror\crop-profiles.ini - "[name]" sections with "game=" and the crop keys.
/// </summary>
public static class CropProfiles
{
    /// <summary>The environment variable is for tests (the mirror honours the same one).</summary>
    public static string FilePath => Environment.GetEnvironmentVariable("GAZE_MIRROR_PROFILES_FILE") is { Length: > 0 } overridden
        ? overridden : Path.Combine(AppSettings.Folder, "crop-profiles.ini");

    /// <summary>The keys a profile holds: the framing (crop, follow, steadying) - not the eye or the output size.</summary>
    public static bool IsProfileKey(string key) =>
        key.StartsWith("crop_", StringComparison.Ordinal) || key.StartsWith("stabilize", StringComparison.Ordinal) || key == "picture_source";

    public static List<CropProfile> Load()
    {
        var profiles = new List<CropProfile>();
        if (!File.Exists(FilePath)) return profiles;
        CropProfile? current = null;
        foreach (var raw in File.ReadAllLines(FilePath))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
            if (line[0] == '[' && line[^1] == ']')
            {
                current = new CropProfile { Name = line[1..^1].Trim() };
                profiles.Add(current);
                continue;
            }
            var equals = line.IndexOf('=');
            if (equals < 0 || current == null) continue;
            var key = line[..equals].Trim();
            var value = line[(equals + 1)..].Trim();
            if (key == "game") current.Game = value;
            else if (IsProfileKey(key)) current.Values[key] = value;
        }
        return profiles;
    }

    public static void Save(IReadOnlyList<CropProfile> profiles)
    {
        var text = new StringBuilder("# Crop profiles of VR Gaze Mirror. The mirror applies a profile at the start of the game whose program file\n# the profile names. Edit them on the app's Mirror page.\n");
        foreach (var profile in profiles)
        {
            text.Append('\n').Append('[').Append(profile.Name.Replace("]", "")).Append("]\n");
            text.Append("game=").Append(profile.Game).Append('\n');
            foreach (var (key, value) in profile.Values.OrderBy(v => v.Key, StringComparer.Ordinal)) text.Append(key).Append('=').Append(value).Append('\n');
        }
        Directory.CreateDirectory(AppSettings.Folder);
        var temporary = FilePath + ".tmp";
        File.WriteAllText(temporary, text.ToString(), new UTF8Encoding(false));
        File.Move(temporary, FilePath, overwrite: true);
    }
}
