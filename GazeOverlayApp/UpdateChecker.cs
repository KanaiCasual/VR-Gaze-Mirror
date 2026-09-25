using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GazeOverlay;

/// <summary>A file attached to a release: the installer, or the text file with its SHA-256.</summary>
public sealed record ReleaseAsset(string Name, string Url, long Size);

/// <summary>A release the app can tell the user about. Installer and Checksum are set only when the release carries both,
/// from this project's own download address; without them the app can only open the release page.</summary>
public sealed record ReleaseInfo(Version Version, string Tag, string Title, bool IsBeta, string Url, ReleaseAsset? Installer = null, ReleaseAsset? Checksum = null)
{
    public bool CanInstall => Installer != null && Checksum != null;
}

/// <summary>What the app itself remembers (not the ring settings - those live in the layer's own file).</summary>
/// <summary>One of the user's own preset slots: the values as they were when it was saved.</summary>
public sealed class SavedPreset
{
    public DateTime SavedAt { get; set; }
    public Dictionary<string, string> Values { get; set; } = [];
}

public sealed class AppSettings
{
    public const int SlotCount = 3;
    /// <summary>The user's own presets: ring look (Look + Tail tabs) and Quad Views sliders. null = empty slot.</summary>
    public SavedPreset?[] RingSlots { get; set; } = new SavedPreset?[SlotCount];
    public SavedPreset?[] QuadViewsSlots { get; set; } = new SavedPreset?[SlotCount];

    public bool CheckForUpdates { get; set; } = true;
    public bool IncludeBetas { get; set; }
    public string? SkippedVersion { get; set; }
    public bool ShowPreview { get; set; } = true;
    /// <summary>Where the window was, so it comes back beside the game where it was left. 0 = not remembered yet.</summary>
    public double WindowLeft { get; set; }
    public double WindowTop { get; set; }
    public double WindowWidth { get; set; }
    public double WindowHeight { get; set; }
    /// <summary>Size of the mirror image the last crop-tool picture was taken from (the picture itself is a file next to this one).</summary>
    public int MirrorWidth { get; set; }
    public int MirrorHeight { get; set; }
    /// <summary>Crop tab: the box cannot be moved or resized by mouse (against accidental drags).</summary>
    public bool CropLocked { get; set; }
    /// <summary>The crop picture: which eye each stored picture shows (-1 = unknown / none), and which one is looked at.</summary>
    public int MirrorPictureEye { get; set; } = -1;
    public int MirrorPictureOtherEye { get; set; } = -1;
    public bool MirrorPictureShowOther { get; set; }
    /// <summary>The picture was taken with the capture key: it is not replaced by the automatic refreshes.</summary>
    public bool MirrorPictureKept { get; set; }
    /// <summary>Virtual-key code of the key that takes the picture while a capture is armed (the mirror layer watches it). 0x13 = Pause.</summary>
    public int CaptureKey { get; set; } = 0x13;
    /// <summary>Capture from headset: 0 = only the key takes the picture; otherwise it is taken by itself after this many seconds (the key still works meanwhile).</summary>
    public int CaptureTimerSeconds { get; set; }
    /// <summary>The mirror window: 0 = an ordinary window, otherwise the monitor it fills (1 = primary); which eye it shows;
    /// whether the OBS plugin gets a blank picture while it is open; whether it keeps a title bar when filling a monitor.</summary>
    public int MirrorMonitor { get; set; } = 1;
    public bool MirrorTitled { get; set; }
    /// <summary>
    /// The helper program SteamVR was told to start with itself (the registration lives in SteamVR and names the exe by
    /// its full path). "" = not registered yet; a different path = the app moved, so it is registered again.
    /// </summary>
    public string HelperRegisteredPath { get; set; } = "";
    /// <summary>The crop profile chosen on the Mirror tab ("" = none).</summary>
    public string SelectedCropProfile { get; set; } = "";
    /// <summary>Size of the mirror window's picture in pixels = what a capture tool gets. 0 = fill the monitor.</summary>
    public int MirrorOutputWidth { get; set; }
    public int MirrorOutputHeight { get; set; }
    /// <summary>The mirror window draws at most this many pictures a second (60 or 30).</summary>
    public int MirrorFps { get; set; } = 60;

    /// <summary>This app's own folder: preferences, crop profiles, crop pictures, the saved layer orders and the logs.</summary>
    public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GazeMirror");

    private static string FilePath => Path.Combine(Folder, "app.json");

    /// <summary>
    /// Earlier names left their files elsewhere: "QuadViewsGazeMirror" up to the 1.9.x builds (everything), "OpenXRGazeOverlay"
    /// up to 0.9.0 (app.json only). On the first start after a rename, whatever is not in the new folder yet is copied over;
    /// the old folder is left as it is.
    /// </summary>
    private static void AdoptOldFolder()
    {
        try
        {
            var data = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var previous = Path.Combine(data, "QuadViewsGazeMirror");
            if (Directory.Exists(previous))
            {
                // 1.x's folder: its files come over once (the first start without our own app.json), then the folder
                // goes. Its SteamVR manifest stays for the helper, which takes the registration back and then removes it.
                Directory.CreateDirectory(Folder);
                var adopting = !File.Exists(FilePath);
                foreach (var file in Directory.GetFiles(previous))
                {
                    var target = Path.Combine(Folder, Path.GetFileName(file));
                    if (adopting && !File.Exists(target)) File.Copy(file, target);
                    if (!file.EndsWith(".vrmanifest", StringComparison.OrdinalIgnoreCase)) File.Delete(file);
                }
                if (Directory.GetFileSystemEntries(previous).Length == 0) Directory.Delete(previous);
            }
            if (File.Exists(FilePath)) return;
            Directory.CreateDirectory(Folder);
            var oldest = Path.Combine(data, "OpenXRGazeOverlay", "app.json");
            if (!File.Exists(FilePath) && File.Exists(oldest)) File.Copy(oldest, FilePath);
        }
        catch
        {
            // Starting with default preferences is not worth an error.
        }
    }

    public static AppSettings Load()
    {
        AdoptOldFolder();
        try
        {
            if (File.Exists(FilePath))
            {
                var loaded = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath)) ?? new AppSettings();
                // A hand-edited or older file may hold another number of slots.
                loaded.RingSlots = [.. (loaded.RingSlots ?? []).Concat(new SavedPreset?[SlotCount]).Take(SlotCount)];
                loaded.QuadViewsSlots = [.. (loaded.QuadViewsSlots ?? []).Concat(new SavedPreset?[SlotCount]).Take(SlotCount)];
                return loaded;
            }
        }
        catch
        {
            // A damaged file just means defaults.
        }
        return new AppSettings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // Not being able to remember a preference is not worth bothering the user about.
        }
    }
}

/// <summary>
/// Looks at the project's GitHub releases to tell the user when a newer version exists, and can fetch the new installer
/// for them. Kept as plain as a browser download so that antivirus software has nothing to object to: the .msi is
/// saved into the user's Downloads folder, from this project's own release address only, checked against the SHA-256
/// the release publishes next to it, and then handed to Windows Installer (msiexec) like a double-clicked file. The app
/// never elevates, never unpacks anything and never runs code it downloaded.
/// </summary>
public static partial class UpdateChecker
{
    /// <summary>"owner/name", baked in at build time (-p:GitHubRepository=...). Empty = update checks are unavailable.</summary>
    public static string Repository { get; } =
        Assembly.GetExecutingAssembly().GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "GitHubRepository")?.Value?.Trim() ?? "";

    public static bool IsConfigured => RepositoryPattern().IsMatch(Repository);

    /// <summary>e.g. "0.4.0" or "0.4.0-beta.1" (whatever -p:Version was at build time).</summary>
    public static string CurrentVersionText { get; } =
        (Assembly.GetExecutingAssembly().GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0").Split('+')[0];

    public static Version CurrentVersion => ParseVersion(CurrentVersionText) ?? new Version(0, 0, 0);

    public static string ReleasesPage => $"https://github.com/{Repository}/releases";

    /// <summary>The newest release that is newer than this build, or null. Throws on network/API problems.</summary>
    public static async Task<ReleaseInfo?> CheckAsync(string repository, bool includeBetas, CancellationToken cancellation = default)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("VR-Gaze-Mirror/" + CurrentVersion.ToString(3));
        http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        var json = await http.GetStringAsync($"https://api.github.com/repos/{repository}/releases?per_page=30", cancellation);
        return PickUpdate(ParseReleases(json, repository), CurrentVersion, includeBetas);
    }

    public static ReleaseInfo? PickUpdate(IEnumerable<ReleaseInfo> releases, Version current, bool includeBetas) =>
        releases.Where(r => (includeBetas || !r.IsBeta) && r.Version > current)
                .OrderByDescending(r => r.Version)
                .FirstOrDefault();

    public static List<ReleaseInfo> ParseReleases(string json, string repository)
    {
        var releases = new List<ReleaseInfo>();
        using var doc = JsonDocument.Parse(json);
        foreach (var item in doc.RootElement.EnumerateArray())
        {
            if (item.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
            var tag = item.GetProperty("tag_name").GetString() ?? "";
            var version = ParseVersion(tag);
            if (version == null) continue;

            var prerelease = item.TryGetProperty("prerelease", out var pre) && pre.GetBoolean();
            var title = item.TryGetProperty("name", out var name) ? name.GetString() : null;
            // Only ever offer a link back into this project's own releases on github.com, whatever the API returned.
            var url = item.TryGetProperty("html_url", out var html) ? html.GetString() : null;
            if (url == null || !url.StartsWith($"https://github.com/{repository}/", StringComparison.OrdinalIgnoreCase))
            {
                url = $"https://github.com/{repository}/releases";
            }
            var (installer, checksum) = PickAssets(item, repository);
            releases.Add(new ReleaseInfo(version, tag, string.IsNullOrWhiteSpace(title) ? tag : title!, prerelease || tag.Contains('-'), url, installer, checksum));
        }
        return releases;
    }

    /// <summary>
    /// The installer and its checksum file among a release's attachments. Only an .msi named like this project's own
    /// builds, only from this project's own download address, and only with its "name.msi.sha256" next to it.
    /// </summary>
    private static (ReleaseAsset? Installer, ReleaseAsset? Checksum) PickAssets(JsonElement release, string repository)
    {
        if (!release.TryGetProperty("assets", out var assets) || assets.ValueKind != JsonValueKind.Array) return (null, null);
        var prefix = $"https://github.com/{repository}/releases/download/";
        var found = new List<ReleaseAsset>();
        foreach (var asset in assets.EnumerateArray())
        {
            var assetName = asset.TryGetProperty("name", out var n) ? n.GetString() : null;
            var assetUrl = asset.TryGetProperty("browser_download_url", out var u) ? u.GetString() : null;
            var size = asset.TryGetProperty("size", out var s) && s.TryGetInt64(out var bytes) ? bytes : 0;
            if (assetName == null || assetUrl == null || !assetUrl.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
            found.Add(new ReleaseAsset(assetName, assetUrl, size));
        }
        var installer = found.FirstOrDefault(a => InstallerNamePattern().IsMatch(a.Name) && a.Size > 0);
        if (installer == null) return (null, null);
        var checksum = found.FirstOrDefault(a => string.Equals(a.Name, installer.Name + ".sha256", StringComparison.OrdinalIgnoreCase) && a.Size is > 0 and < 4096);
        return (installer, checksum);
    }

    /// <summary>The user's Downloads folder, where a browser would put the file too.</summary>
    public static string DownloadFolder
    {
        get
        {
            var downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
            return Directory.Exists(downloads) ? downloads : Path.GetTempPath();
        }
    }

    /// <summary>
    /// Fetches the release's installer into the Downloads folder and checks it: the size the release lists, and the
    /// SHA-256 from the checksum file published with it. Returns the path of the checked file. Throws when anything
    /// does not match; nothing unchecked is left behind under the final name.
    /// </summary>
    public static async Task<string> DownloadInstallerAsync(ReleaseInfo release, IProgress<double>? progress, CancellationToken cancellation = default)
    {
        if (release.Installer is not { } installer || release.Checksum is not { } checksum) throw new InvalidOperationException("This release has no checked installer to download.");
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("VR-Gaze-Mirror/" + CurrentVersion.ToString(3));

        var expected = ParseChecksum(await http.GetStringAsync(checksum.Url, cancellation), installer.Name)
            ?? throw new InvalidDataException("The release's checksum file does not name this installer.");

        var final = Path.Combine(DownloadFolder, installer.Name);
        var partial = final + ".partial";
        try
        {
            using (var response = await http.GetAsync(installer.Url, HttpCompletionOption.ResponseHeadersRead, cancellation))
            {
                response.EnsureSuccessStatusCode();
                await using var source = await response.Content.ReadAsStreamAsync(cancellation);
                await using var target = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, useAsync: true);
                var buffer = new byte[1 << 16];
                long done = 0;
                int read;
                while ((read = await source.ReadAsync(buffer, cancellation)) > 0)
                {
                    await target.WriteAsync(buffer.AsMemory(0, read), cancellation);
                    done += read;
                    if (done > installer.Size) throw new InvalidDataException("The download is larger than the release says.");
                    progress?.Report((double)done / installer.Size);
                }
            }
            if (new FileInfo(partial).Length != installer.Size) throw new InvalidDataException("The download is not the size the release says.");
            var actual = await Sha256Async(partial, cancellation);
            if (!string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("The download does not match the SHA-256 the release publishes.");
            File.Move(partial, final, overwrite: true);
            return final;
        }
        catch
        {
            try { File.Delete(partial); } catch { /* nothing more to do */ }
            throw;
        }
    }

    /// <summary>The hash for a file name out of a "hash  name" checksum file (sha256sum style; a lone hash counts too).</summary>
    public static string? ParseChecksum(string text, string fileName)
    {
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.Trim();
            var match = ChecksumLinePattern().Match(line);
            if (!match.Success) continue;
            var named = match.Groups[2].Value.TrimStart('*').Trim();
            if (named.Length == 0 || string.Equals(named, fileName, StringComparison.OrdinalIgnoreCase)) return match.Groups[1].Value.ToLowerInvariant();
        }
        return null;
    }

    public static async Task<string> Sha256Async(string path, CancellationToken cancellation = default)
    {
        await using var stream = File.OpenRead(path);
        var hash = await System.Security.Cryptography.SHA256.HashDataAsync(stream, cancellation);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    /// <summary>Hands the downloaded installer to Windows Installer, as opening the file would. Windows asks for permission itself.</summary>
    public static bool StartInstaller(string path)
    {
        if (!File.Exists(path) || !Path.GetExtension(path).Equals(".msi", StringComparison.OrdinalIgnoreCase)) return false;
        try
        {
            var msiexec = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe");
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(msiexec, $"/i \"{path}\"") { UseShellExecute = true });
            return true;
        }
        catch
        {
            return false;
        }
    }

    [GeneratedRegex(@"^VR-Gaze-Mirror-\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?\.msi$")]
    private static partial Regex InstallerNamePattern();

    [GeneratedRegex(@"^([A-Fa-f0-9]{64})(?:\s+(.*))?$")]
    private static partial Regex ChecksumLinePattern();

    /// <summary>Takes the first x.y.z (or x.y) found in a tag like "v0.4.0-beta.1". Betas are told apart by GitHub's flag, not by number.</summary>
    public static Version? ParseVersion(string text)
    {
        var match = VersionPattern().Match(text);
        if (!match.Success) return null;
        var patch = match.Groups[3].Success ? int.Parse(match.Groups[3].Value) : 0;
        return new Version(int.Parse(match.Groups[1].Value), int.Parse(match.Groups[2].Value), patch);
    }

    [GeneratedRegex(@"(\d+)\.(\d+)(?:\.(\d+))?")]
    private static partial Regex VersionPattern();

    [GeneratedRegex(@"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})/[A-Za-z0-9._-]{1,100}$")]
    private static partial Regex RepositoryPattern();
}
