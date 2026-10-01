using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using Microsoft.Win32;

namespace GazeOverlay;

/// <summary>
/// Knows which OpenXR API layers have to sit above or below which, checks a list against that, suggests a fixed order and
/// writes a new order to the registry.
///
/// The ordering knowledge is the same set of facts as the linter of Fred Emmott's OpenXR-API-Layers-GUI (LayerRules.cpp,
/// ISC licence) - re-expressed here, plus this project's own layer pair. "Above" means earlier in the list = closer to
/// the game. A target can be a layer name, an extension name (= every layer whose manifest provides that extension) or a
/// "#facet" (= every layer that is known to have that trait).
/// </summary>
public static class LayerOrder
{
    private sealed record Rule(string Id, string[] Above, string[] Below, string[] Facets);

    private const string Overlay = "#CompositionLayers", TransformsPoses = "#TransformsPoses", UsesPoses = "#UsesGameWorldPoses";

    private static readonly Dictionary<string, string> Why = new()
    {
        [Overlay] = "draws an overlay", [TransformsPoses] = "modifies poses", [UsesPoses] = "uses poses",
    };

    private static readonly Rule[] Rules =
    [
        new(TransformsPoses, [], [UsesPoses], []),
        new("XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking", ["XR_EXT_hand_tracking"], [], []),
        new("XR_APILAYER_FREDEMMOTT_OpenKneeboard", [], [], [Overlay, UsesPoses]),
        new("XR_APILAYER_app_racelab_Overlay", [], [], [Overlay, UsesPoses]),
        new(LayerStatus.QuadViewsLayer, ["XR_EXT_eye_gaze_interaction"], [], []),
        new("XR_APILAYER_MBUCCHIA_toolkit", ["XR_EXT_eye_gaze_interaction", "XR_EXT_hand_tracking"], ["XR_VARJO_foveated_rendering"], [Overlay]),
        new("XR_APILAYER_NOVENDOR_motion_compensation", ["XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking"], [], [TransformsPoses]),
        new("XR_APILAYER_MBUCCHIA_vulkan_d3d12_interop", ["XR_APILAYER_MBUCCHIA_toolkit", LayerStatus.GazeMirrorLayer], [], []),
        // The mirror must see the finished picture: below everything that draws into it, and below quad views (which
        // turns four views into the two the mirror understands - and, in this project, tells it where the eyes look).
        new(LayerStatus.GazeMirrorLayer, [], [Overlay, "XR_VARJO_foveated_rendering", LayerStatus.QuadViewsLayer], []),
        new("XR_APILAYER_NOVENDOR_XRNeckSafer", ["XR_APILAYER_FREDEMMOTT_HandTrackedCockpitClicking", Overlay], [], []),
    ];

    /// <summary>"upper must be above lower", with the reason in words.</summary>
    public sealed record Constraint(LayerEntry Upper, LayerEntry Lower, string Reason);

    /// <summary>Every known constraint between the layers in the list, whether it currently holds or not.</summary>
    public static List<Constraint> Constraints(IReadOnlyList<LayerEntry> layers)
    {
        var result = new List<Constraint>();
        foreach (var layer in layers)
        {
            if (layer.LayerName == null) continue;
            var own = Rules.FirstOrDefault(r => r.Id == layer.LayerName);
            if (own == null) continue;
            // A layer also inherits the rules of its traits (e.g. anything that modifies poses).
            var applicable = new[] { own }.Concat(Rules.Where(r => own.Facets.Contains(r.Id)));
            foreach (var rule in applicable)
            {
                foreach (var target in rule.Above)
                {
                    foreach (var (other, why) in Resolve(target, layers, layer)) result.Add(new Constraint(layer, other, why));
                }
                foreach (var target in rule.Below)
                {
                    foreach (var (other, why) in Resolve(target, layers, layer)) result.Add(new Constraint(other, layer, why));
                }
            }
        }
        return result;
    }

    private static IEnumerable<(LayerEntry Layer, string Why)> Resolve(string target, IReadOnlyList<LayerEntry> layers, LayerEntry self)
    {
        foreach (var other in layers)
        {
            if (ReferenceEquals(other, self) || other.LayerName == null || other.LayerName == self.LayerName) continue;
            if (target.StartsWith('#'))
            {
                if (Rules.Any(r => r.Id == other.LayerName && r.Facets.Contains(target))) yield return (other, $"{Short(other)} {Why[target]}");
            }
            else if (target.StartsWith("XR_APILAYER_", StringComparison.Ordinal))
            {
                if (other.LayerName == target) yield return (other, "they are known to need this order");
            }
            else if (other.Extensions.Contains(target))
            {
                yield return (other, $"{Short(other)} provides {target}");
            }
        }
    }

    /// <summary>The constraints the given order breaks, among layers that are switched on (a layer that is off cannot hurt).</summary>
    public static List<string> Problems(IReadOnlyList<LayerEntry> order)
    {
        var index = new Dictionary<LayerEntry, int>(ReferenceEqualityComparer.Instance);
        for (var i = 0; i < order.Count; i++) index[order[i]] = i;
        return [.. Constraints(order)
            .Where(c => c.Upper.Enabled && c.Lower.Enabled && index[c.Upper] > index[c.Lower])
            .DistinctBy(c => (index[c.Upper], index[c.Lower]))
            .Select(c => $"{Short(c.Upper)} must be above {Short(c.Lower)} ({c.Reason}).")
            .Distinct()];
    }

    /// <summary>
    /// The order with the fewest moves that satisfies every known constraint: layers keep their relative order unless a
    /// constraint forces otherwise. null when the constraints contradict each other.
    /// </summary>
    public static List<LayerEntry>? Suggest(IReadOnlyList<LayerEntry> order)
    {
        var constraints = Constraints(order);
        var remaining = order.ToList();
        var result = new List<LayerEntry>();
        while (remaining.Count > 0)
        {
            var next = remaining.FirstOrDefault(candidate =>
                !constraints.Any(c => ReferenceEquals(c.Lower, candidate) && remaining.Any(r => ReferenceEquals(r, c.Upper))));
            if (next == null) return null;
            remaining.Remove(next);
            result.Add(next);
        }
        return result;
    }

    public static string Short(LayerEntry layer)
    {
        var name = layer.LayerName ?? Path.GetFileName(layer.JsonPath);
        return name.StartsWith("XR_APILAYER_", StringComparison.Ordinal) ? name["XR_APILAYER_".Length..] : name;
    }

    // ------------------------------------------------------------------ writing an order

    private static string DataFolder => AppSettings.Folder;

    /// <summary>
    /// A .reg file that removes the given values and creates them again in the given order. The loader goes by the order
    /// the values were created in, so re-creating them is the only way to reorder (it is what the OpenXR API Layers tool
    /// does too). Importing the file for the CURRENT order restores it - that is the backup.
    /// </summary>
    public static string RegFile(bool perUser, IReadOnlyList<(string Name, int Data)> ordered, string? keyPath = null)
    {
        var key = $"[{(perUser ? "HKEY_CURRENT_USER" : "HKEY_LOCAL_MACHINE")}\\{keyPath ?? LayerStatus.LayersKeyPath}]";
        static string Quote(string name) => "\"" + name.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        var text = new StringBuilder("Windows Registry Editor Version 5.00\r\n\r\n");
        text.Append(key).Append("\r\n");
        foreach (var (name, _) in ordered) text.Append(Quote(name)).Append("=-\r\n");
        text.Append("\r\n").Append(key).Append("\r\n");
        foreach (var (name, data) in ordered) text.Append(Quote(name)).Append($"=dword:{data:x8}\r\n");
        return text.ToString();
    }

    /// <summary>
    /// Puts one list (PC-wide or this user's) into the given order. Returns false if the Windows permission prompt was
    /// declined; throws with a readable message when it cannot or did not work. The PC-wide list is changed by Windows'
    /// own reg.exe importing a .reg file after a UAC prompt - this app never runs elevated.
    /// </summary>
    public static async Task<bool> ApplyAsync(bool perUser, IReadOnlyList<LayerEntry> wanted)
    {
        var current = ReadRaw(perUser);
        if (current.Any(v => v.Data == null)) throw new InvalidOperationException("The list holds an entry that is not a number (DWORD). Fix that with the OpenXR API Layers tool first.");
        if (!current.Select(v => v.Name).Order().SequenceEqual(wanted.Select(l => l.JsonPath).Order()))
        {
            throw new InvalidOperationException("The list changed in the meantime. Press Refresh and try again.");
        }
        if (wanted.Any(l => l.JsonPath.Contains('\n') || l.JsonPath.Contains('\r'))) throw new InvalidOperationException("A .reg file cannot hold this entry's name.");

        var data = current.ToDictionary(v => v.Name, v => v.Data!.Value);
        Directory.CreateDirectory(DataFolder);
        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
        var backupPath = Path.Combine(DataFolder, $"layers-before-{stamp}.reg");
        File.WriteAllText(backupPath, RegFile(perUser, [.. current.Select(v => (v.Name, v.Data!.Value))]), Encoding.Unicode);

        if (perUser)
        {
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
            using var key = hive.OpenSubKey(LayerStatus.LayersKeyPath, writable: true) ?? throw new InvalidOperationException("The list is no longer there.");
            foreach (var layer in wanted) key.DeleteValue(layer.JsonPath, throwOnMissingValue: false);
            foreach (var layer in wanted) key.SetValue(layer.JsonPath, data[layer.JsonPath], RegistryValueKind.DWord);
        }
        else
        {
            var applyPath = Path.Combine(DataFolder, $"layers-apply-{stamp}.reg");
            File.WriteAllText(applyPath, RegFile(false, [.. wanted.Select(l => (l.JsonPath, data[l.JsonPath]))]), Encoding.Unicode);
            try
            {
                // Held open without write/delete sharing while the elevated reg.exe reads it, so nothing can swap the file.
                using var guard = new FileStream(applyPath, FileMode.Open, FileAccess.Read, FileShare.Read);
                var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "reg.exe"), $"import \"{applyPath}\" /reg:64")
                {
                    UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden,
                };
                Process process;
                try
                {
                    process = Process.Start(start) ?? throw new InvalidOperationException("Windows did not start reg.exe.");
                }
                catch (Win32Exception e) when (e.NativeErrorCode == 1223)
                {
                    return false;
                }
                using (process)
                {
                    await process.WaitForExitAsync();
                    if (process.ExitCode != 0) throw new InvalidOperationException($"reg.exe reported an error (exit code {process.ExitCode}). {backupPath} holds your previous order.");
                }
            }
            finally
            {
                try { File.Delete(applyPath); } catch { /* a leftover .reg file is harmless */ }
            }
        }

        var after = ReadRaw(perUser).Select(v => v.Name).ToList();
        if (!after.SequenceEqual(wanted.Select(l => l.JsonPath)))
        {
            throw new InvalidOperationException($"The order did not come out as asked. {backupPath} holds your previous order (double-click it to restore).");
        }
        return true;
    }

    private static List<(string Name, int? Data)> ReadRaw(bool perUser)
    {
        using var hive = RegistryKey.OpenBaseKey(perUser ? RegistryHive.CurrentUser : RegistryHive.LocalMachine, RegistryView.Registry64);
        using var key = hive.OpenSubKey(LayerStatus.LayersKeyPath);
        if (key == null) return [];
        return [.. key.GetValueNames().Select(name => (name, key.GetValueKind(name) == RegistryValueKind.DWord ? (int?)(int)key.GetValue(name)! : null))];
    }
}
