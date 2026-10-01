using System.IO;
using System.Text.Json;
using Microsoft.Win32;

namespace GazeOverlay;

public enum LayerHealth { NotActive, Active, Duplicate }

/// <param name="PerUser">Registered for this Windows user only (HKCU) rather than for the whole PC (HKLM).</param>
public sealed record LayerEntry(string JsonPath, bool Enabled, string? LayerName, string? DllPath, bool PerUser = false)
{
    /// <summary>The OpenXR extensions the layer's manifest says it provides - what ordering rules such as "above whatever provides eye tracking" go by.</summary>
    public IReadOnlyList<string> Extensions { get; init; } = [];
}

public sealed record LayerReport(LayerHealth Health, string Detail);

/// <param name="QuadViewsInstalled">Any Quad-Views-Foveated is registered (on or off); false = the bundled installer can be offered.</param>
public sealed record SystemStatus(
    LayerReport QuadViews, LayerReport GazeMirror, string? ObsPath, bool ObsPluginPresent,
    IReadOnlyList<LayerEntry> Layers, bool? OrderCorrect, bool QuadViewsInstalled);

/// <summary>
/// Read-only look at how the OpenXR layers are set up. This app never installs or changes them - the MSI does that -
/// it only explains what it finds.
/// </summary>
public static class LayerStatus
{
    public const string QuadViewsLayer = "XR_APILAYER_MBUCCHIA_quad_views_foveated";
    public const string GazeMirrorLayer = "XR_APILAYER_NOVENDOR_gaze_mirror";
    public const string ObsPluginFile = "gaze-mirror-capture.dll";
    private const string LayersKey = @"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit";

    public const string LayersKeyPath = LayersKey;

    /// <summary>The PC-wide list (HKLM, 64-bit) - where installers put layers, and the one the status checks look at.</summary>
    public static List<LayerEntry> ReadLayers() => ReadLayers(perUser: false);

    /// <summary>In registry order, which is the order the OpenXR loader uses: first = closest to the game.</summary>
    public static List<LayerEntry> ReadLayers(bool perUser)
    {
        var layers = new List<LayerEntry>();
        using var hive = RegistryKey.OpenBaseKey(perUser ? RegistryHive.CurrentUser : RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = hive.OpenSubKey(LayersKey);
        if (key == null) return layers;

        foreach (var name in key.GetValueNames())
        {
            var enabled = key.GetValue(name) is int flag && flag == 0;
            string? layerName = null, dllPath = null;
            var extensions = new List<string>();
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(name));
                var layer = doc.RootElement.GetProperty("api_layer");
                layerName = layer.GetProperty("name").GetString();
                if (layer.TryGetProperty("instance_extensions", out var listed) && listed.ValueKind == JsonValueKind.Array)
                {
                    foreach (var extension in listed.EnumerateArray())
                    {
                        if (extension.TryGetProperty("name", out var extensionName) && extensionName.GetString() is { } text) extensions.Add(text);
                    }
                }
                var library = layer.GetProperty("library_path").GetString();
                if (library != null) dllPath = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(name)!, library));
            }
            catch
            {
                // Missing or unreadable manifest: still list it so the order is visible.
            }
            layers.Add(new LayerEntry(name, enabled, layerName, dllPath, perUser) { Extensions = extensions });
        }
        return layers;
    }

    /// <summary>
    /// Switches one registered layer on or off (the value's data: 0 = on, 1 = off - the loader's own convention, and
    /// what the "OpenXR API Layers" tool does). A per-user entry is simply written. A PC-wide entry needs administrator
    /// rights, and this app never runs elevated: Windows' own reg.exe is started with a UAC prompt to change that one
    /// value. Returns false when the user declined the prompt; throws when the change failed.
    /// </summary>
    public static async Task<bool> SetEnabledAsync(LayerEntry layer, bool enabled)
    {
        var data = enabled ? 0 : 1;
        if (layer.PerUser)
        {
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
            using var key = hive.OpenSubKey(LayersKey, writable: true) ?? throw new InvalidOperationException("The layer list is no longer there.");
            key.SetValue(layer.JsonPath, data, RegistryValueKind.DWord);
            return true;
        }

        // The name goes on a command line inside quotes; refuse the (never legitimate) names that could break out of them.
        if (layer.JsonPath.Contains('"') || layer.JsonPath.EndsWith('\\') || layer.JsonPath.Contains('\n') || layer.JsonPath.Contains('\r'))
        {
            throw new InvalidOperationException("This entry's name holds characters the app cannot pass on safely.");
        }
        var start = new System.Diagnostics.ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "reg.exe"),
            $"add \"HKLM\\{LayersKey}\" /v \"{layer.JsonPath}\" /t REG_DWORD /d {data} /f /reg:64")
        {
            UseShellExecute = true, Verb = "runas", WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden,
        };
        System.Diagnostics.Process process;
        try
        {
            process = System.Diagnostics.Process.Start(start) ?? throw new InvalidOperationException("Windows did not start reg.exe.");
        }
        catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223)
        {
            return false; // "The operation was canceled by the user": No on the UAC prompt.
        }
        using (process)
        {
            await process.WaitForExitAsync();
            if (process.ExitCode != 0) throw new InvalidOperationException($"reg.exe reported an error (exit code {process.ExitCode}).");
        }
        return true;
    }

    public static SystemStatus Get()
    {
        var layers = ReadLayers();
        // Quad-Views-Foveated is optional and Matthieu Bucchianeri's own (unmodified) product: any copy is fine.
        var quadViews = Describe(layers, QuadViewsLayer, "Quad-Views-Foveated",
            "Not installed, and optional: foveated rendering and the Quad Views page need it; the mirror and the ring work without it. Press Install to set up the official version that ships with this app.");
        var gazeMirror = Describe(layers, GazeMirrorLayer, "the gaze mirror layer", "Not installed. Run the VR Gaze Mirror installer (.msi), or its Repair option.");

        var obsPath = FindObs();
        var pluginPresent = obsPath != null && File.Exists(Path.Combine(obsPath, "obs-plugins", "64bit", ObsPluginFile));

        // Quad views must sit closer to the game than the mirror, which only understands the final stereo image.
        var enabled = layers.Where(l => l.Enabled).ToList();
        var quadIndex = enabled.FindIndex(l => l.LayerName == QuadViewsLayer);
        var mirrorIndex = enabled.FindIndex(l => l.LayerName == GazeMirrorLayer);
        bool? orderCorrect = quadIndex >= 0 && mirrorIndex >= 0 ? quadIndex < mirrorIndex : null;

        return new SystemStatus(quadViews, gazeMirror, obsPath, pluginPresent, layers, orderCorrect, layers.Any(l => l.LayerName == QuadViewsLayer));
    }

    private static LayerReport Describe(List<LayerEntry> layers, string layerName, string friendlyName, string notInstalled)
    {
        var active = layers.Where(l => l.Enabled && l.LayerName == layerName).ToList();
        if (active.Count == 0)
        {
            return new LayerReport(LayerHealth.NotActive, layers.Any(l => l.LayerName == layerName)
                ? $"Installed but switched off, so games do not load it (see the layer list below)."
                : notInstalled);
        }
        if (active.Count > 1)
        {
            return new LayerReport(LayerHealth.Duplicate,
                $"{active.Count} copies of {friendlyName} are active at once, which breaks it. Keep one:\n" +
                string.Join("\n", active.Select(l => "  " + Path.GetDirectoryName(l.JsonPath))));
        }
        var entry = active[0];
        var present = entry.DllPath != null && File.Exists(entry.DllPath);
        return present
            ? new LayerReport(LayerHealth.Active, $"Active ({Path.GetDirectoryName(entry.JsonPath)}).")
            : new LayerReport(LayerHealth.NotActive, $"Registered, but its file is missing ({entry.DllPath}). Run the installer again.");
    }

    public static string? FindObs()
    {
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
            using var key = hklm.OpenSubKey(@"SOFTWARE\OBS Studio");
            if (key?.GetValue(null) is string path && Directory.Exists(path)) return path;
        }
        var fallback = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "obs-studio");
        return Directory.Exists(fallback) ? fallback : null;
    }
}
