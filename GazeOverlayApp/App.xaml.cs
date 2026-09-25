using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;

namespace GazeOverlay;

/// <summary>
/// The settings app. It never installs anything, never asks for administrator rights and never touches Program Files or
/// the system registry - the MSI does all of that. All it writes is two settings files in the user's profile: the ring's,
/// and (only when Apply is clicked on the Quad Views tab) Quad-Views-Foveated's.
/// One user-driven exception: ticking a layer on or off on the Status tab. The app itself still never runs elevated -
/// it asks Windows (UAC) to let the system's own reg.exe change that single value (see LayerStatus.SetEnabledAsync).
/// </summary>
public partial class App : Application
{
    /// <summary>True for --selftest and --screenshots: nothing about the window is remembered from those runs.</summary>
    public static bool Headless { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var args = e.Args;
        Headless = args.Length >= 1 && args[0] is "--selftest" or "--screenshots";
        AppLog.Start(Headless);
        AppLog.Write($"VR Gaze Mirror {UpdateChecker.CurrentVersionText} starting" + (args.Length > 0 ? " with " + string.Join(" ", args) : "") +
                     $"; app folder {AppContext.BaseDirectory}; settings folder {AppSettings.Folder}");
        // Anything that would otherwise close the app silently is written down first; Windows' own crash handling still follows.
        DispatcherUnhandledException += (_, error) => AppLog.Write("Unhandled error on the UI thread", error.Exception);
        AppDomain.CurrentDomain.UnhandledException += (_, error) => AppLog.Write("Unhandled error", error.ExceptionObject as Exception ?? new Exception(error.ExceptionObject?.ToString() ?? "unknown"));
        TaskScheduler.UnobservedTaskException += (_, error) => AppLog.Write("Unobserved task error", error.Exception);

        if (args.Length >= 2 && args[0] == "--selftest")
        {
            Shutdown(SelfTest(args[1]));
            return;
        }

        if (args.Length >= 2 && args[0] == "--screenshots")
        {
            // Development aid: renders every tab of the real window to PNG files, off-screen.
            var outputDir = args[1];
            Directory.CreateDirectory(outputDir);
            var window = new MainWindow { WindowStartupLocation = WindowStartupLocation.Manual, Left = -20000, Top = 0, ShowInTaskbar = false };
            // Optional window size (--screenshots <dir> <width> <height>), to check the layout when it is docked small.
            if (args.Length >= 4 && double.TryParse(args[2], out var width) && double.TryParse(args[3], out var height))
            {
                window.Width = width;
                window.Height = height;
            }
            window.Show();
            window.ShowCropTestPicture();
            window.Dispatcher.InvokeAsync(() =>
            {
                void Snapshot(string name)
                {
                    window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                    var root = (FrameworkElement)window.Content;
                    var visual = new System.Windows.Media.DrawingVisual();
                    using (var context = visual.RenderOpen())
                    {
                        var bounds = new Rect(0, 0, root.ActualWidth, root.ActualHeight);
                        context.DrawRectangle((System.Windows.Media.Brush)window.FindResource("WindowBack"), null, bounds);
                        context.DrawRectangle(new System.Windows.Media.VisualBrush(root), null, bounds);
                    }
                    var bitmap = new RenderTargetBitmap((int)root.ActualWidth, (int)root.ActualHeight, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
                    bitmap.Render(visual);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using var file = File.Create(Path.Combine(outputDir, $"{name}.png"));
                    encoder.Save(file);
                }
                for (var i = 0; i < window.Tabs.Items.Count; i++)
                {
                    window.Tabs.SelectedIndex = i;
                    window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                    var root = (FrameworkElement)window.Content;
                    File.AppendAllText(Path.Combine(outputDir, "layout.txt"),
                        $"tab{i}: window {window.ActualWidth:0}x{window.ActualHeight:0}, root {root.ActualWidth:0}, tabs {window.Tabs.ActualWidth:0}, preview {window.RingPanel.Visibility} {window.RingPanel.ActualWidth:0}, column {window.PreviewColumn.ActualWidth:0} ({window.PreviewColumn.Width}), inner grid {((FrameworkElement)window.Tabs.Parent).ActualWidth:0}, tabs margin {window.Tabs.Margin}\n");
                    Snapshot($"tab{i}");
                }
                // GAZE_SCREENSHOT_PROFILES=1 (with GAZE_MIRROR_PROFILES_FILE pointing at a scratch file): exercise the crop profiles on the Mirror tab.
                if (Environment.GetEnvironmentVariable("GAZE_SCREENSHOT_PROFILES") == "1")
                {
                    window.Tabs.SelectedItem = window.CropTab;
                    window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
                    File.WriteAllText(Path.Combine(outputDir, "profiles.txt"), window.ExerciseProfiles(Snapshot));
                }
                Shutdown(0);
            }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
            return;
        }

        new MainWindow().Show();
    }

    /// <summary>The crop box arithmetic, and the picture request answered by a stand-in for the mirror layer.</summary>
    private static bool SelfTestCrop(StreamWriter report)
    {
        static bool Near(double a, double b) => Math.Abs(a - b) < 1e-6;

        var box = new CropBox { Aspect = 16.0 / 9.0, ImageAspect = 1, Height = 0.5 };
        var (w1, h1) = box.Size();
        box.Height = 1;                                   // a full-height 16:9 box does not fit a square image
        var (w2, h2) = box.Size();
        box.Height = 0.5; box.MoveTo(0.95, 0.02);         // pushed into the top right corner
        var corner = box.Rect();
        var vertical = new CropBox { Aspect = 9.0 / 16.0, ImageAspect = 1, Height = 1 };
        var resized = new CropBox { Aspect = 1, ImageAspect = 1 };
        resized.ResizeFromCorner(0.2, 0.2, 0.9, 0.5);     // pointer went further across than down: the square follows it
        var free = new CropBox { Aspect = 0, ImageAspect = 1 };
        free.ResizeFromCorner(0.1, 0.1, 0.6, 0.3);
        // Room kept free for the picture steadying: the box cannot enter it, whatever is done to it.
        var roomy = new CropBox { Aspect = 16.0 / 9.0, ImageAspect = 1, Height = 0.4, Margin = 0.02 };
        roomy.MoveTo(0, 0);                               // pushed against the left wall and the top
        var pushed = roomy.Rect();
        roomy.Height = 1; roomy.Normalize();              // "Largest"
        var largest = roomy.Rect();
        var roomyResize = new CropBox { Aspect = 1, ImageAspect = 1, Margin = 0.05 };
        roomyResize.ResizeFromCorner(0.5, 0.5, 1.5, 1.5); // dragged far out of the picture

        // Picture request: play the layer (blocks + answering the request counter).
        string pictureResult;
        try
        {
            using var existing = System.IO.MemoryMappedFiles.MemoryMappedFile.OpenExisting("GazeOverlay.SettingsSignal");
            pictureResult = "skipped (a game is running)";
        }
        catch (FileNotFoundException)
        {
            using var signal = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew("GazeOverlay.SettingsSignal", 16);
            using var signalView = signal.CreateViewAccessor(0, 16);
            signalView.Write(0, 0x53534F47u);
            signalView.Write(4, 2u);
            const int width = 4, height = 2;
            using var block = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew(MirrorSnapshot.BlockName, MirrorSnapshot.BlockSize);
            using var blockView = block.CreateViewAccessor();
            var layer = Task.Run(async () =>
            {
                for (var i = 0; i < 100 && signalView.ReadInt32(12) == 0; i++) await Task.Delay(10);
                blockView.Write(0, MirrorSnapshot.BlockMagic);
                blockView.Write(12, (uint)width); blockView.Write(16, (uint)height);
                blockView.Write(20, 8192u); blockView.Write(24, 4096u);
                blockView.Write(28, 100u); blockView.Write(32, 200u); blockView.Write(36, 1920u); blockView.Write(40, 1080u);
                for (var i = 0; i < width * height * 4; i++) blockView.Write(MirrorSnapshot.PixelsOffset + i, (byte)(i * 7));
                // Version 2: the right eye first, the left eye as the second picture.
                blockView.Write(4, 2u);
                blockView.Write(44, 1u); blockView.Write(48, 0u);
                for (var i = 0; i < width * height * 4; i++) blockView.Write(MirrorSnapshot.SecondPixelsOffset + i, (byte)(i * 3));
                blockView.Write(8, blockView.ReadInt32(8) + 1);
            });
            var (picture, message) = Task.Run(MirrorSnapshot.RequestAsync).GetAwaiter().GetResult();
            layer.Wait();
            var pixels = new byte[width * height * 4];
            picture?.Image.CopyPixels(pixels, width * 4, 0);
            var otherPixels = new byte[width * height * 4];
            picture?.OtherImage?.CopyPixels(otherPixels, width * 4, 0);
            pictureResult = picture is { FullWidth: 8192, FullHeight: 4096, CropX: 100, CropWidth: 1920, Eye: 1, OtherEye: 0 } && picture.Image.PixelWidth == width && pixels[5] == 35
                            && otherPixels[5] == 15
                ? "ok" : "wrong: " + message;
        }

        var checks = new (string Name, bool Ok)[]
        {
            ("a 16:9 box half the height of a square image is 0.889 of its width", Near(w1, 0.5 * 16 / 9) && Near(h1, 0.5)),
            ("a box too big for the image is shrunk, keeping its shape", Near(w2, 1) && Near(h2, 9.0 / 16)),
            ("a box pushed into a corner stays inside the image", Near(corner.Left + corner.Width, 1) && Near(corner.Top, 0)),
            ("a 9:16 box can take the full height", Near(vertical.Size().Height, 1) && Near(vertical.Size().Width, 9.0 / 16)),
            ("dragging a corner keeps the opposite corner and the shape", Near(resized.Rect().Left, 0.2) && Near(resized.Rect().Top, 0.2) && Near(resized.Rect().Width, 0.7) && Near(resized.Rect().Height, 0.7)),
            ("with room kept free, a box pushed into a corner stops short of the edges", Near(pushed.Left, 0.02) && Near(pushed.Top, 0.02)),
            ("with room kept free, the largest box leaves that room on both sides", Near(largest.Left, 0.02) && Near(largest.Width, 0.96) && Near(largest.Height, 0.96 * 9 / 16)),
            ("with room kept free, dragging a corner stops at the room", Near(roomyResize.Rect().Left, 0.5) && Near(roomyResize.Rect().Width, 0.45) && Near(roomyResize.Rect().Height, 0.45)),
            ("the free shape follows the pointer both ways", Near(free.Rect().Width, 0.5) && Near(free.Rect().Height, 0.2)),
            ("\"16:9\", \"0\" and plain numbers are understood as shapes", Near(CropBox.ParseAspect("16:9"), 16.0 / 9) && CropBox.ParseAspect("0") == 0 && Near(CropBox.ParseAspect("1.5"), 1.5)),
            ("a picture request is answered and read back, both eyes (" + pictureResult + ")", pictureResult == "ok" || pictureResult.StartsWith("skipped")),
        };
        foreach (var (name, ok) in checks) report.WriteLine($"crop - {(ok ? "ok  " : "FAIL")} {name}");
        return checks.All(c => c.Ok);
    }

    /// <summary>
    /// The layers tool: the order rules on made-up lists, and the reordering mechanism itself - a .reg file imported by
    /// reg.exe - on a scratch key of this app's own under HKCU (no elevation, nothing of OpenXR's is touched).
    /// </summary>
    private static bool SelfTestLayerOrder(string outputDir, StreamWriter report)
    {
        LayerEntry Layer(string name, params string[] extensions) => new($@"C:\Layers\{name}.json", true, name, null) { Extensions = extensions };
        var mirror = Layer(LayerStatus.GazeMirrorLayer);
        var toolkit = Layer("XR_APILAYER_MBUCCHIA_toolkit");
        var quadViews = Layer(LayerStatus.QuadViewsLayer, "XR_VARJO_quad_views", "XR_VARJO_foveated_rendering");
        var eyeTracking = Layer("XR_APILAYER_EXAMPLE_eye_tracker", "XR_EXT_eye_gaze_interaction");
        var unknown = Layer("XR_APILAYER_EXAMPLE_unknown");

        var bad = new List<LayerEntry> { mirror, unknown, eyeTracking, toolkit, quadViews };
        var problems = LayerOrder.Problems(bad);
        var suggested = LayerOrder.Suggest(bad);
        var good = new List<LayerEntry> { quadViews, toolkit, mirror };
        var mirrorOff = new List<LayerEntry> { mirror with { Enabled = false }, quadViews };

        // The mechanism: values are re-created in the wanted order by one import.
        const string scratch = @"Software\OpenXRGazeOverlay\SelfTestLayerOrder";
        var names = new[] { @"C:\Program Files\A\a.json", @"C:\B ""quoted""\b.json", @"C:\C\c.json" };
        var wanted = new[] { names[2], names[0], names[1] };
        List<string> after = [];
        string mechanism;
        var observable = true;
        try
        {
            // Control first: does deleting values and creating them again reorder them in THIS registry view? In the real
            // registry it does (a new value goes to the end of the list). A process started from inside a packaged (MSIX)
            // app sees a virtualised view in which a re-created value returns to its old place, so nothing can be observed.
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(scratch, throwOnMissingSubKey: false);
            using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(scratch))
            {
                foreach (var name in names) key.SetValue(name, 0, Microsoft.Win32.RegistryValueKind.DWord);
                foreach (var name in names) key.DeleteValue(name);
                foreach (var name in wanted) key.SetValue(name, 0, Microsoft.Win32.RegistryValueKind.DWord);
                observable = key.GetValueNames().SequenceEqual(wanted);
            }
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(scratch, throwOnMissingSubKey: false);
            using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(scratch))
            {
                for (var i = 0; i < names.Length; i++) key.SetValue(names[i], i % 2, Microsoft.Win32.RegistryValueKind.DWord);
            }
            var regPath = Path.Combine(outputDir, "layer-order-test.reg");
            File.WriteAllText(regPath, LayerOrder.RegFile(perUser: true, [.. wanted.Select(n => (n, Array.IndexOf(names, n) % 2))], scratch), System.Text.Encoding.Unicode);
            using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(
                Path.Combine(Environment.SystemDirectory, "reg.exe"), $"import \"{regPath}\"") { UseShellExecute = false, CreateNoWindow = true })!;
            process.WaitForExit();
            using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(scratch)!)
            {
                after = [.. key.GetValueNames()];
                mechanism = after.SequenceEqual(wanted) && after.All(n => (int)key.GetValue(n)! == Array.IndexOf(names, n) % 2) ? "ok" : "order or data wrong";
            }
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(scratch, throwOnMissingSubKey: false);
        }
        catch (Exception e)
        {
            mechanism = e.Message;
        }

        var checks = new (string Name, bool Ok)[]
        {
            ("a bad order is reported (mirror under quad views and under the overlay layer, toolkit under quad views, quad views and toolkit above the eye tracker)",
                problems.Count == 5 && problems.Any(p => p.StartsWith("MBUCCHIA_quad_views_foveated must be above NOVENDOR_gaze_mirror")) &&
                problems.Any(p => p.StartsWith("MBUCCHIA_quad_views_foveated must be above EXAMPLE_eye_tracker"))),
            ("the suggested order satisfies every rule and leaves unrelated layers where they were",
                suggested != null && LayerOrder.Problems(suggested).Count == 0 && suggested.IndexOf(unknown) < suggested.IndexOf(eyeTracking)),
            ("a good order has no problems", LayerOrder.Problems(good).Count == 0),
            ("a layer that is switched off is not complained about", LayerOrder.Problems(mirrorOff).Count == 0),
            (observable ? "reg.exe import re-creates the values in the wanted order, names with spaces and quotes included (" + mechanism + ")"
                        : "reordering by reg.exe import: NOT TESTABLE HERE - this registry view is virtualised and always sorted; run the selftest from a normal terminal",
                !observable || mechanism == "ok"),
        };
        foreach (var (name, ok) in checks) report.WriteLine($"layer order - {(ok ? "ok  " : "FAIL")} {name}");
        if (problems.Count != 5) foreach (var p in problems) report.WriteLine("    reported: " + p);
        if (observable && mechanism != "ok") report.WriteLine("    after import: " + string.Join(" | ", after));
        return checks.All(c => c.Ok);
    }

    /// <summary>
    /// Quad Views tab logic against a canned copy of a real case: a Pimax driven through SteamVR, with a settings file
    /// written by the QuadViews Companion (focus size only under headset sections that SteamVR does not match).
    /// </summary>
    private static bool SelfTestQuadViews(string outputDir, StreamWriter report)
    {
        const string cannedLog = """
            2026-09-18 22:19:00 +0100: XR_APILAYER_MBUCCHIA_quad_views_foveated layer (v1.1.4) is active
            2026-09-18 22:19:01 +0100: Application: DCS (DCS.exe)
            2026-09-18 22:19:01 +0100: Using OpenXR runtime: SteamVR/OpenXR 2.17.10
            2026-09-18 22:19:01 +0100: Using OpenXR system: SteamVR/OpenXR : lighthouse
            2026-09-18 22:19:01 +0100: Trying to locate configuration file at 'Z:\nowhere\settings.cfg'...
            2026-09-18 22:19:01 +0100: Eye tracking is supported
            2026-09-18 22:19:01 +0100: Recommended peripheral resolution: 2270x2372 (0.320x density)
            2026-09-18 22:19:01 +0100: Recommended focus resolution: 3042x3178 (1.225x density)
            2026-09-18 22:19:01 +0100:   Stereo pixel count was: 105,191,104 (7096x7412)
            2026-09-18 22:19:01 +0100:   Quad views pixel count is: 30,103,832
            2026-09-18 22:19:01 +0100:   Savings: -71.4%
            """;
        const string cannedFile = """
            # QuadViews configuration file created using TallyMouse's QuadViews Companion App.

            # Common settings for all headsets (unless overriden below).
            smoothen_focus_view_edges=0.2
            sharpen_focus_view=0
            turbo_mode=1
            peripheral_multiplier=0.32
            focus_multiplier=1.225

            [Pimax]
            # Dynamic Foveated Rendering settings
            horizontal_focus_section=0.2
            vertical_focus_section=0.2
            peripheral_multiplier=0.32

            [SteamVR]
            # Turbo Mode causes unexplained errors with SteamVR.
            turbo_mode=0

            [app:SomeGame]
            horizontal_focus_section=0.6
            """;

        var logPath = Path.Combine(outputDir, "quadviews-canned.log");
        File.WriteAllText(logPath, cannedLog);
        var session = QuadViewsSession.Read(logPath);
        var file = QuadViewsFile.Parse(cannedFile.Replace("\r\n", "\n"));

        long Pixels(QuadViewsFile f)
        {
            var e = QuadViewsFile.Evaluate([("test", f)], session);
            return QuadViewsPixels.Compute(session!.StereoWidth, session.StereoHeight, e["peripheral_multiplier"], e["focus_multiplier"], e["horizontal_focus_section"], e["vertical_focus_section"]).Total;
        }

        var before = QuadViewsFile.Evaluate([("test", file)], session);
        var sliders = QuadViewsSetting.All.ToDictionary(s => s.Id, _ => 0.0);
        var untouched = QuadViewsFile.Plan(file, sliders, new HashSet<string>());
        var after = QuadViewsFile.Evaluate([("test", untouched)], session);
        var again = QuadViewsFile.Plan(untouched, sliders, new HashSet<string>());

        sliders["focus_h"] = 30; sliders["peripheral_res"] = 16; sliders["turbo"] = 1;
        var edited = QuadViewsFile.Plan(untouched, sliders, new HashSet<string> { "focus_h", "peripheral_res", "turbo" });
        var editedValues = QuadViewsFile.Evaluate([("test", edited)], session);
        File.WriteAllText(Path.Combine(outputDir, "quadviews-planned.cfg"), edited.Text);

        var checks = new (string Name, bool Ok)[]
        {
            ("log: runtime, game and resolution are read", session is { RuntimeName: "SteamVR/OpenXR 2.17.10", AppName: "DCS", ExeName: "DCS.exe", StereoWidth: 7096, StereoHeight: 7412, QuadPixels: 30103832, EyeTracking: true }),
            ("a [Pimax] focus size does not reach a SteamVR runtime (the layer's 35 % applies)", before["horizontal_focus_section"] == 0.35),
            ("pixel count equals what the layer logged (30,103,832)", Pixels(file) == 30103832),
            ("Apply with nothing changed copies the intended 20 % into the common part", after["horizontal_focus_section"] == 0.2 && after["vertical_focus_section"] == 0.2),
            ("...which gives 17,081,296 pixels", Pixels(untouched) == 17081296),
            ("...and leaves values that already reach the game as they were written", untouched.InCommon("peripheral_multiplier") == "0.32"),
            ("a second Apply changes nothing", again.Text == untouched.Text),
            ("a changed slider is written to the common part and the headset sections", editedValues["horizontal_focus_section"] == 0.3 && edited.Entries().Count(e => e.Key == "horizontal_focus_section" && e.Value == "0.3") == 2),
            ("16 % of the pixels is a 0.4 multiplier", edited.InCommon("peripheral_multiplier") == "0.4" && editedValues["peripheral_multiplier"] == 0.4),
            ("per-game sections are left alone", edited.Entries().Any(e => e.Section == "[app:SomeGame]" && e.Value == "0.6")),
            ("the [SteamVR] Turbo guard is left alone", editedValues["turbo_mode"] == 0 && edited.InCommon("turbo_mode") == "1"),
        };
        foreach (var (name, ok) in checks) report.WriteLine($"quad views - {(ok ? "ok  " : "FAIL")} {name}");

        // This PC's real files, for information only (they change whenever the user changes a setting).
        var real = QuadViewsSession.Read(QuadViewsSession.LogPath);
        if (real is { HasResolution: true })
        {
            var files = new List<(string, QuadViewsFile)>();
            var shipped = real.ConfigPaths.FirstOrDefault(p => !string.Equals(p, QuadViewsFile.DefaultPath, StringComparison.OrdinalIgnoreCase));
            if (shipped != null && File.Exists(shipped)) files.Add(("shipped", QuadViewsFile.Load(shipped)));
            files.Add(("user", QuadViewsFile.Load(QuadViewsFile.DefaultPath)));
            var e = QuadViewsFile.Evaluate(files, real);
            var sections = real.EyeTracking == false ? ("horizontal_fixed_section", "vertical_fixed_section") : ("horizontal_focus_section", "vertical_focus_section");
            var computed = QuadViewsPixels.Compute(real.StereoWidth, real.StereoHeight, e["peripheral_multiplier"], e["focus_multiplier"], e[sections.Item1], e[sections.Item2]);
            report.WriteLine($"quad views (this PC): runtime '{real.RuntimeName}', {real.StereoWidth}x{real.StereoHeight}; layer logged {real.QuadPixels}, computed from the files {computed.Total} " +
                             $"(focus {computed.FocusWidth}x{computed.FocusHeight} vs logged {real.FocusResolution}); focus size in effect {e[sections.Item1]}x{e[sections.Item2]}");
        }
        else
        {
            report.WriteLine("quad views (this PC): no usable Quad-Views-Foveated log");
        }
        return checks.All(c => c.Ok);
    }

    /// <summary>Exercises everything without showing a window, writing the results into a folder.</summary>
    /// <summary>The SHA-256 of an empty file, through the same code the updater checks downloads with.</summary>
    private static string ChecksumOfEmptyFile(string outputDir)
    {
        var path = Path.Combine(outputDir, "empty.bin");
        File.WriteAllBytes(path, []);
        return Task.Run(() => UpdateChecker.Sha256Async(path)).GetAwaiter().GetResult();
    }

    private static int SelfTest(string outputDir)
    {
        Directory.CreateDirectory(outputDir);
        using var report = new StreamWriter(Path.Combine(outputDir, "selftest.txt"));
        try
        {
            var status = LayerStatus.Get();
            report.WriteLine($"quad views: {status.QuadViews.Health} - {status.QuadViews.Detail}");
            report.WriteLine($"gaze mirror: {status.GazeMirror.Health} - {status.GazeMirror.Detail}");
            report.WriteLine($"obs: {status.ObsPath ?? "(not found)"} plugin present={status.ObsPluginPresent}");
            report.WriteLine($"order correct: {status.OrderCorrect?.ToString() ?? "n/a"}");
            foreach (var layer in status.Layers) report.WriteLine($"  [{(layer.Enabled ? "on " : "off")}] {layer.LayerName ?? "?"}  {layer.JsonPath}");

            var (liveState, liveText) = GazeLive.Read();
            report.WriteLine($"live: {liveState} - {liveText}");

            // Config round trip.
            var configPath = Path.Combine(outputDir, "roundtrip.cfg");
            var values = ConfigFile.Load(ConfigFile.DefaultPath);
            ConfigFile.Save(configPath, values);
            var reloaded = ConfigFile.Load(configPath);
            var mismatches = values.Where(kv => reloaded.GetValueOrDefault(kv.Key) != kv.Value).Select(kv => kv.Key).ToList();
            report.WriteLine($"config round trip: {values.Count} keys, mismatches: {(mismatches.Count == 0 ? "none" : string.Join(", ", mismatches))}");

            // Previews.
            var index = 0;
            foreach (var (background, moving) in new[] { (PreviewBackground.Cockpit, false), (PreviewBackground.Terrain, false), (PreviewBackground.Sky, false), (PreviewBackground.Cockpit, true) })
            {
                var bitmap = RingPreview.Render(values, background, moving, 340);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using var file = File.Create(Path.Combine(outputDir, $"preview{index++}.png"));
                encoder.Save(file);
            }
            // The styles that are drawn from recorded places, with the eyes moving: trail behind the blob, cooling heat spot.
            foreach (var trailStyle in new[] { "bubble", "solid", "heatmap" })
            {
                var styled = new Dictionary<string, string>(values) { ["style"] = trailStyle };
                var bitmap = RingPreview.Render(styled, PreviewBackground.Cockpit, moving: true, 260);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using var file = File.Create(Path.Combine(outputDir, $"preview-{trailStyle}-moving.png"));
                encoder.Save(file);
            }
            report.WriteLine("previews: ok");

            // Live link: play the layer's part (it creates the signal block) and check that the app's notify bumps it.
            var liveOk = true;
            try
            {
                using var existing = System.IO.MemoryMappedFiles.MemoryMappedFile.OpenExisting("GazeOverlay.SettingsSignal");
                report.WriteLine("live link: a game is running, so its real signal block exists - simulated test skipped (notify returned " + LiveLink.NotifySettingsChanged() + ")");
            }
            catch (FileNotFoundException)
            {
                var withoutGame = LiveLink.NotifySettingsChanged();
                using var block = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew("GazeOverlay.SettingsSignal", 12);
                using var view = block.CreateViewAccessor(0, 12);
                view.Write(0, 0x53534F47u);
                view.Write(4, 1u);
                var first = LiveLink.NotifySettingsChanged();
                var second = LiveLink.NotifySettingsChanged();
                var generation = view.ReadInt32(8);
                liveOk = !withoutGame && first && second && generation == 2;
                report.WriteLine($"live link - {(liveOk ? "ok  " : "FAIL")} no game -> {withoutGame}; with a (simulated) game -> {first}, {second}; counter = {generation} (expected 2)");
            }

            // The VRChat gaze block: written here the way the helper writes it (by offset), read back through the app's reader.
            try
            {
                using var block = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateOrOpen("GazeMirror2.ExternalGaze", 400);
                using var view = block.CreateViewAccessor(0, 400);
                view.Write(4, 1u); view.Write(8, 400u); view.Write(0, 0x58324D47u);
                view.Write(32, Environment.ProcessId); view.Write(36, 2);
                var name = System.Text.Encoding.UTF8.GetBytes("selftest writer"); var padded = new byte[32]; Array.Copy(name, padded, name.Length);
                view.WriteArray(80, padded, 0, 32);
                view.Write(16, 2L); view.Write(40, 1); view.Write(44, 1);
                view.Write(48, -0.25f); view.Write(52, 0.5f); view.Write(56, -0.2f); view.Write(60, 0.45f);
                view.Write(24, Environment.TickCount64);
                var fresh = MirrorLive.ReadExternalGaze();
                view.Write(24, Environment.TickCount64 - 5000);
                var stale = MirrorLive.ReadExternalGaze();
                view.Write(16, 3L); // A write in progress: the reader must not take it.
                var torn = MirrorLive.ReadExternalGaze();
                view.Write(16, 4L); view.Write(32, 0); // Gone: the block stays (Windows keeps it while anyone holds it), the writer does not.
                var externalOk = fresh is { Fresh: true, Writer: "selftest writer", LeftX: -0.25f, RightY: 0.45f } && stale is { Fresh: false, AgeMs: >= 4900 } && torn == null;
                liveOk &= externalOk;
                report.WriteLine($"vrchat block - {(externalOk ? "ok  " : "FAIL")} fresh sample read back ({fresh?.Writer}, left {fresh?.LeftX} {fresh?.LeftY}); a 5 s old one is stale; a torn read is refused");
            }
            catch (Exception e)
            {
                liveOk = false;
                report.WriteLine("vrchat block - FAIL " + e.Message);
            }

            // The SRanibro raw-gaze block: the same layout under its own name, per-eye validity, z fields at 112/116.
            try
            {
                using var block = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateOrOpen("GazeMirror2.RawGaze", 400);
                using var view = block.CreateViewAccessor(0, 400);
                view.Write(4, 1u); view.Write(8, 400u); view.Write(0, 0x58324D47u);
                view.Write(32, Environment.ProcessId); view.Write(36, 3);
                var name = System.Text.Encoding.UTF8.GetBytes("selftest raw"); var padded = new byte[32]; Array.Copy(name, padded, name.Length);
                view.WriteArray(80, padded, 0, 32);
                view.Write(16, 2L); view.Write(40, 1); view.Write(44, 0); // Left eye only.
                view.Write(48, 0.3f); view.Write(52, 0.1f); view.Write(112, -0.95f);
                view.Write(24, Environment.TickCount64);
                var oneEye = MirrorLive.ReadRawGaze();
                view.Write(16, 4L); view.Write(32, 0);
                var rawOk = oneEye is { Fresh: true, Writer: "selftest raw", LeftX: 0.3f };
                liveOk &= rawOk;
                report.WriteLine($"raw block - {(rawOk ? "ok  " : "FAIL")} a one-eye sample counts as fresh ({oneEye?.Writer}, left {oneEye?.LeftX} {oneEye?.LeftY})");
            }
            catch (Exception e)
            {
                liveOk = false;
                report.WriteLine("raw block - FAIL " + e.Message);
            }

            var quadViewsOk = SelfTestQuadViews(outputDir, report);
            quadViewsOk &= SelfTestLayerOrder(outputDir, report);
            quadViewsOk &= SelfTestCrop(report);

            // Mirror window: what this PC offers, and the command line the settings turn into.
            var mirrorArguments = MirrorWindowControl.Arguments(new AppSettings { MirrorMonitor = 2, MirrorTitled = true, MirrorOutputWidth = 1920, MirrorOutputHeight = 1080, MirrorFps = 30 });
            var mirrorOk = mirrorArguments == "--monitor 2 --titled 1 --size 1920x1080 --fps 30" &&
                           MirrorWindowControl.Arguments(new AppSettings { MirrorMonitor = 0 }) == "--windowed --size fill --fps 60";
            report.WriteLine($"mirror window - {(mirrorOk ? "ok  " : "FAIL")} settings become the expected command line");
            report.WriteLine($"mirror window: program {MirrorWindowControl.FindProgram() ?? "(not found)"}, running={MirrorWindowControl.IsRunning()}, monitors: " +
                             string.Join(", ", MirrorWindowControl.Monitors().Select(m => $"{m.Number}={m.Width}x{m.Height}")));
            quadViewsOk &= mirrorOk;

            // The user's own preset slots survive a trip through app.json's format (exact numbers, empty slots stay empty).
            var withSlot = new AppSettings();
            withSlot.QuadViewsSlots[1] = new SavedPreset { SavedAt = new DateTime(2026, 9, 19, 7, 0, 0), Values = new() { ["peripheral_res"] = 4.84.ToString("R", System.Globalization.CultureInfo.InvariantCulture) } };
            var slotsBack = System.Text.Json.JsonSerializer.Deserialize<AppSettings>(System.Text.Json.JsonSerializer.Serialize(withSlot));
            var slotsOk = slotsBack is { QuadViewsSlots.Length: AppSettings.SlotCount } && slotsBack.QuadViewsSlots[0] == null &&
                          slotsBack.QuadViewsSlots[1]?.Values["peripheral_res"] == "4.84" && slotsBack.RingSlots.All(s => s == null);
            report.WriteLine($"preset slots - {(slotsOk ? "ok  " : "FAIL")} saved values and empty slots round-trip");
            quadViewsOk &= slotsOk;

            // Update check: version parsing and picking, against a canned release list (no network).
            const string canned = """
                [
                  {"tag_name":"v0.9.0-beta.1","name":"0.9 beta","prerelease":true,"draft":false,"html_url":"https://github.com/someone/project/releases/tag/v0.9.0-beta.1",
                   "assets":[{"name":"VR-Gaze-Mirror-0.9.0.msi","size":1234,"browser_download_url":"https://github.com/someone/project/releases/download/v0.9.0-beta.1/VR-Gaze-Mirror-0.9.0.msi"},
                             {"name":"VR-Gaze-Mirror-0.9.0.msi.sha256","size":100,"browser_download_url":"https://github.com/someone/project/releases/download/v0.9.0-beta.1/VR-Gaze-Mirror-0.9.0.msi.sha256"}]},
                  {"tag_name":"v0.5.2","name":"0.5.2","prerelease":false,"draft":false,"html_url":"https://github.com/someone/project/releases/tag/v0.5.2",
                   "assets":[{"name":"VR-Gaze-Mirror-0.5.2.msi","size":1234,"browser_download_url":"https://evil.example/VR-Gaze-Mirror-0.5.2.msi"},
                             {"name":"VR-Gaze-Mirror-0.5.2.msi.sha256","size":100,"browser_download_url":"https://github.com/someone/project/releases/download/v0.5.2/VR-Gaze-Mirror-0.5.2.msi.sha256"}]},
                  {"tag_name":"v0.6.0","name":"unfinished","prerelease":false,"draft":true,"html_url":"https://github.com/someone/project/releases/tag/v0.6.0"},
                  {"tag_name":"v0.5.10","name":"0.5.10","prerelease":false,"draft":false,"html_url":"https://evil.example/not-github",
                   "assets":[{"name":"VR-Gaze-Mirror-0.5.10.msi","size":1234,"browser_download_url":"https://github.com/someone/project/releases/download/v0.5.10/VR-Gaze-Mirror-0.5.10.msi"},
                             {"name":"setup.exe","size":9999,"browser_download_url":"https://github.com/someone/project/releases/download/v0.5.10/setup.exe"}]},
                  {"tag_name":"nightly","name":"no version in tag","prerelease":false,"draft":false,"html_url":"https://github.com/someone/project/releases/tag/nightly"}
                ]
                """;
            var releases = UpdateChecker.ParseReleases(canned, "someone/project");
            var stable = UpdateChecker.PickUpdate(releases, new Version(0, 5, 2), includeBetas: false);
            var beta = UpdateChecker.PickUpdate(releases, new Version(0, 5, 2), includeBetas: true);
            var none = UpdateChecker.PickUpdate(releases, new Version(1, 0, 0), includeBetas: true);
            var checks = new (string Name, bool Ok)[]
            {
                ("drafts and unversioned tags are ignored", releases.Count == 3),
                ("0.5.10 is newer than 0.5.2 (numeric, not text)", stable?.Version == new Version(0, 5, 10)),
                ("betas only when asked for", beta?.Version == new Version(0, 9, 0) && beta.IsBeta),
                ("nothing offered when already newer", none == null),
                ("foreign links are replaced by the project's releases page", stable?.Url == "https://github.com/someone/project/releases"),
                ("an installer with its checksum from the project's own address can be installed", beta is { CanInstall: true, Installer.Name: "VR-Gaze-Mirror-0.9.0.msi", Checksum.Size: 100 }),
                ("an installer from a foreign address is not offered", releases.First(r => r.Tag == "v0.5.2") is { Installer: null, CanInstall: false }),
                ("an installer without a checksum, and an .exe, are not offered", stable is { Installer: not null, Checksum: null, CanInstall: false }),
                ("a sha256sum-style checksum file is read for the right name",
                    UpdateChecker.ParseChecksum("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef *Other.msi\nABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789  VR-Gaze-Mirror-0.9.0.msi\n", "VR-Gaze-Mirror-0.9.0.msi")
                        == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
                    && UpdateChecker.ParseChecksum("not a hash", "x.msi") == null),
                ("a file's SHA-256 is computed as sha256sum would", ChecksumOfEmptyFile(outputDir) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
            };
            foreach (var (name, ok) in checks) report.WriteLine($"update check - {(ok ? "ok  " : "FAIL")} {name}");
            report.WriteLine($"this build: version {UpdateChecker.CurrentVersionText}, repository '{UpdateChecker.Repository}' (configured: {UpdateChecker.IsConfigured})");

            // Optional live read of a public repository's release list, to prove the network path (--selftest <dir> <owner/name>).
            var liveRepo = Environment.GetCommandLineArgs().Skip(3).FirstOrDefault();
            if (liveRepo != null)
            {
                try
                {
                    // Task.Run: blocking the UI thread on an await that wants to resume on it would deadlock.
                    var found = Task.Run(() => UpdateChecker.CheckAsync(liveRepo, includeBetas: true)).GetAwaiter().GetResult();
                    report.WriteLine($"live check of {liveRepo}: " + (found == null ? "reachable, nothing newer than this build" : $"reachable, newest is {found.Version} ({found.Tag}) -> {found.Url}"));
                }
                catch (Exception liveError)
                {
                    report.WriteLine($"live check of {liveRepo} failed: {liveError.Message}");
                }
            }
            return checks.All(c => c.Ok) && liveOk && quadViewsOk ? 0 : 1;
        }
        catch (Exception ex)
        {
            report.WriteLine("SELFTEST FAILED: " + ex);
            return 1;
        }
    }
}
