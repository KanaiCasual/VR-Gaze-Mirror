using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace GazeOverlay;

/// <summary>
/// The desktop cover's part of the self-test. The arithmetic is checked as it is; the windows are checked for real: a
/// small stand-in "game" (this program started with --cover-dummy: a magenta window) is covered, and the screen is read
/// back the way a capture tool reads it.
/// </summary>
public partial class App
{
    /// <summary>--cover-dummy: a plain magenta window that never takes the focus, for the self-test to cover.</summary>
    private static void ShowCoverDummy()
    {
        new Window
        {
            Title = "VR Gaze Mirror - cover test", WindowStyle = WindowStyle.None, ResizeMode = ResizeMode.NoResize, Topmost = true, ShowActivated = false,
            ShowInTaskbar = false, WindowStartupLocation = WindowStartupLocation.Manual, Left = 160, Top = 160, Width = 420, Height = 300,
            Background = new SolidColorBrush(Color.FromRgb(255, 0, 255)),
        }.Show();
    }

    private static bool SelfTestCover(string outputDir, StreamWriter report)
    {
        var ok = true;
        void Check(bool good, string what)
        {
            ok &= good;
            report.WriteLine($"desktop cover - {(good ? "ok  " : "FAIL")} {what}");
        }

        // ---- one piece per monitor the window lies on
        ScreenRect[] triple = [new(0, 0, 1920, 1080), new(1920, 0, 3840, 1080), new(-1920, 0, 0, 1080)];
        var spanning = DesktopCover.Pieces(new ScreenRect(-1920, 0, 3840, 1080), triple);
        Check(spanning.Count == 3 && spanning.All(p => p.Piece == p.Monitor), "a window across three monitors gets three pieces, each a whole monitor");
        var single = DesktopCover.Pieces(new ScreenRect(100, 100, 900, 700), triple);
        Check(single.Count == 1 && single[0].Piece == new ScreenRect(100, 100, 900, 700), "a window on one monitor gets one piece, its own size");
        var straddling = DesktopCover.Pieces(new ScreenRect(1800, 200, 2200, 600), triple);
        Check(straddling.Count == 2 && straddling[0].Piece == new ScreenRect(1800, 200, 1920, 600) && straddling[1].Piece == new ScreenRect(1920, 200, 2200, 600),
            "a window across a monitor edge is split at the edge");
        Check(DesktopCover.Pieces(new ScreenRect(9000, 9000, 9100, 9100), triple).Count == 0, "a window on no monitor gets none");

        // ---- the pattern
        static int Code(int cellX, int cellY)
        {
            var code = 0;
            for (var y = 1; y <= 4; y++) for (var x = 1; x <= 4; x++) code = code << 1 | (CoverPattern.Lit(cellX, cellY, x, y) ? 1 : 0);
            return code;
        }
        var codes = Enumerable.Range(0, 20).SelectMany(x => Enumerable.Range(0, 12).Select(y => Code(x, y))).ToList(); // A 3840 x 2160 monitor's worth.
        Check(codes.Distinct().Count() >= codes.Count * 9 / 10 && Code(3, 5) == Code(3, 5), $"the marks differ from one another ({codes.Distinct().Count()} different among {codes.Count}) and are the same every time");
        Check(!CoverPattern.Lit(0, 0) && CoverPattern.Lit(54, 54) && CoverPattern.Lit(54 + 83, 54 + 83) && !CoverPattern.Lit(54 + 84, 54),
            "a mark's rim is lit, the space between marks is black");
        {
            const int width = 960, height = 540;
            var pixels = new byte[width * height * 4];
            var grey = CoverPattern.Grey(100);
            for (var y = 0; y < height; y++)
            {
                for (var x = 0; x < width; x++)
                {
                    var value = CoverPattern.Lit(x, y) ? grey : (byte)0;
                    var at = (y * width + x) * 4;
                    pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
                    pixels[at + 3] = 255;
                }
            }
            SavePng(Path.Combine(outputDir, "cover-pattern.png"), pixels, width, height);
            var lit = Enumerable.Range(0, width * height).Count(i => pixels[i * 4] != 0);
            report.WriteLine($"desktop cover: the pattern lights {100.0 * lit / (width * height):0.0} % of the screen (cover-pattern.png)");
        }

        // ---- the hotkey: Ctrl + Alt + a key (here F23, which no keyboard has), told to this app by Windows
        {
            var presses = 0;
            var withKey = new CoverOptions(Enabled: false, InCapture: false, Pattern: false, PatternBrightness: 25, HotKey: 0x86);
            // The producers' signal block and the event a producer sets, under names of the test's own.
            var names = ".Test" + Environment.ProcessId;
            using var signal = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew("GazeOverlay.SettingsSignal" + names, 48);
            using var signalView = signal.CreateViewAccessor(0, 48);
            signalView.Write(0, 0x53534F47u);
            signalView.Write(4, 6u);
            using var first = new DesktopCover(withKey, () => IntPtr.Zero, () => null, names);
            first.HotKeyPressed += () => Interlocked.Increment(ref presses);
            if (first.Status.Text.Contains("already uses"))
            {
                report.WriteLine("desktop cover: Ctrl + Alt + F23 is taken on this PC - the hotkey checks were skipped");
            }
            else
            {
                DesktopCover.Native.PostThreadMessage(first.ThreadId, DesktopCover.Native.WM_HOTKEY, new IntPtr(1), IntPtr.Zero);
                first.Snapshot(); // Waits until the cover's thread has got that far.
                using var second = new DesktopCover(withKey, () => IntPtr.Zero, () => null, names + ".second");
                Check(presses == 1, "the hotkey is registered with Windows and reaches the app");
                Check(signalView.ReadInt32(44) == 0x386, "the producers are told which key to look at, and with which modifiers (Ctrl + Alt + F23)");
                // The producer's way in: it sets the event when it sees the key go down inside the game.
                using (var fromProducer = EventWaitHandle.OpenExisting("GazeMirror2.CoverKey" + names))
                {
                    fromProducer.Set();
                    Thread.Sleep(100);
                    first.Snapshot();
                    var samePress = presses;
                    Thread.Sleep(450);
                    fromProducer.Set();
                    Thread.Sleep(100);
                    first.Snapshot();
                    Check(samePress == 1 && presses == 2, "a press seen by Windows and by the game at once counts once; a later press from the game counts");
                }
                // A producer that turns up later is told the key too.
                signalView.Write(44, 0);
                using (var producerChanged = EventWaitHandle.OpenExisting("GazeMirror2.ProducerChanged" + names))
                {
                    producerChanged.Set();
                    Thread.Sleep(100);
                    first.Snapshot();
                }
                Check(signalView.ReadInt32(44) == 0x386, "a producer that starts later is told the key as well");
                // A side asked for: Windows' hotkey fires for either Ctrl, so it counts only with the keys of that side down - and they are not.
                first.Apply(withKey with { HotKeyModifiers = CoverHotKeys.Right(CoverHotKeys.Ctrl) | CoverHotKeys.Left(CoverHotKeys.Alt) });
                first.Snapshot();
                Thread.Sleep(450); // Past the time within which two reports count as one press.
                var beforeSided = presses;
                DesktopCover.Native.PostThreadMessage(first.ThreadId, DesktopCover.Native.WM_HOTKEY, new IntPtr(1), IntPtr.Zero);
                first.Snapshot();
                Check(signalView.ReadInt32(44) == 0x21386 && presses == beforeSided, "RCtrl + LAlt + F23: the producers are told the sides, and Windows' hotkey without those keys down does not count");
                // Other modifiers: Shift + F23 instead of Ctrl + Alt + F23.
                first.Apply(withKey with { HotKeyModifiers = CoverHotKeys.Shift });
                first.Snapshot();
                Check(signalView.ReadInt32(44) == 0x486 && !first.Status.Text.Contains("already uses"), "a change of modifiers is a change of hotkey: registered anew and told to the producers");
                first.Apply(withKey);
                first.Snapshot();
                Check(second.Status.Text.Contains("already uses"), "a hotkey somebody else holds is reported, not fought over (\"" + second.Status.Text + "\")");
                first.Apply(withKey with { HotKey = 0 });
                first.Snapshot();
                second.Apply(withKey with { HotKey = 0 });
                second.Apply(withKey);
                Check(!second.Status.Text.Contains("already uses"), "and a hotkey that was given up can be taken");
                Check(signalView.ReadInt32(44) == 0, "with no hotkey, the producers are told to look at none");
            }
        }

        // ---- which combinations may be a hotkey, and how they read
        static string Name(int key) => key == 0x42 ? "B" : key == 0x78 ? "F9" : "key";
        Check(CoverHotKeys.Allowed(0x42, CoverHotKeys.Ctrl | CoverHotKeys.Alt) && CoverHotKeys.Allowed(0x42, CoverHotKeys.Win) && CoverHotKeys.Allowed(0x78, 0) && CoverHotKeys.Allowed(0x78, CoverHotKeys.Shift),
            "Ctrl + Alt + B, Win + B, F9 and Shift + F9 may be hotkeys");
        Check(!CoverHotKeys.Allowed(0x42, 0) && !CoverHotKeys.Allowed(0x42, CoverHotKeys.Shift) && !CoverHotKeys.Allowed(0, CoverHotKeys.Ctrl),
            "B and Shift + B may not: they are typing");
        Check(CoverHotKeys.Text(0x42, 3, Name) == "Ctrl + Alt + B" && CoverHotKeys.Text(0x78, CoverHotKeys.Shift, Name) == "Shift + F9" && CoverHotKeys.Text(0x78, 0, Name) == "F9"
              && CoverHotKeys.Text(0x42, CoverHotKeys.All, Name) == "Ctrl + Alt + Shift + Win + B" && CoverHotKeys.Text(0, 3, Name) == "None",
            "a combination reads as it is pressed");
        Check(CoverHotKeys.ForProducers(0x42, 3) == 0x342 && CoverHotKeys.ForProducers(0x78, 0) == 0x78 && CoverHotKeys.ForProducers(0, 3) == 0, "and is handed to the producers as key and modifiers");

        // ---- left and right modifier keys (virtual keys: LShift A0, RShift A1, LCtrl A2, RCtrl A3, LAlt A4, RAlt A5)
        var rightCtrlLeftAlt = CoverHotKeys.Right(CoverHotKeys.Ctrl) | CoverHotKeys.Left(CoverHotKeys.Alt);
        Check(CoverHotKeys.Text(0x42, rightCtrlLeftAlt, Name) == "RCtrl + LAlt + B" && CoverHotKeys.Text(0x78, CoverHotKeys.Left(CoverHotKeys.Shift), Name) == "LShift + F9"
              && CoverHotKeys.Text(0x42, CoverHotKeys.Right(CoverHotKeys.Win), Name) == "RWin + B",
            "a modifier of one side reads as that key");
        Check(CoverHotKeys.Allowed(0x42, rightCtrlLeftAlt) && CoverHotKeys.ForProducers(0x42, rightCtrlLeftAlt) == 0x21342, "it may be a hotkey, and the producers are told the side");
        Check(CoverHotKeys.WithSides(3) == 0x33 && CoverHotKeys.Text(0x42, CoverHotKeys.WithSides(3), Name) == "LCtrl + LAlt + B" && CoverHotKeys.WithSides(rightCtrlLeftAlt) == rightCtrlLeftAlt
              && new AppSettings().CoverHotKeyModifiers == 0x33,
            "a hotkey saved before the sides existed names the left keys, one with sides stays as it is, and the default is LCtrl + LAlt + B");
        var bothCtrl = CoverHotKeys.Both(CoverHotKeys.Ctrl);
        Check(CoverHotKeys.Tidy(CoverHotKeys.Left(CoverHotKeys.Ctrl) | CoverHotKeys.Right(CoverHotKeys.Ctrl)) == bothCtrl && CoverHotKeys.Text(0x42, bothCtrl, Name) == "LCtrl + RCtrl + B"
              && CoverHotKeys.ForProducers(0x42, bothCtrl) == 0x22242 && CoverHotKeys.Tidy(CoverHotKeys.Alt << 4) == 0,
            "both keys of a modifier are a combination of their own, and a side without its modifier is nothing");
        Check(CoverHotKeys.Held(bothCtrl, key => key is 0xA2 or 0xA3) && !CoverHotKeys.Held(bothCtrl, key => key == 0xA2) && !CoverHotKeys.Held(bothCtrl, key => key == 0xA3)
              && !CoverHotKeys.Held(bothCtrl, key => key is 0xA2 or 0xA3 or 0xA4),
            "LCtrl + RCtrl is held with both Ctrl keys down: not with one of them, not with Alt as well");
        Check(CoverHotKeys.Held(rightCtrlLeftAlt, key => key is 0xA3 or 0xA4) && !CoverHotKeys.Held(rightCtrlLeftAlt, key => key is 0xA2 or 0xA4)
              && !CoverHotKeys.Held(rightCtrlLeftAlt, key => key is 0xA2 or 0xA3 or 0xA4) && !CoverHotKeys.Held(rightCtrlLeftAlt, key => key is 0xA3 or 0xA4 or 0xA0),
            "RCtrl + LAlt is held with exactly those two keys: not with the other Ctrl, not with both, not with Shift as well");
        Check(CoverHotKeys.Held(3, key => key is 0xA2 or 0xA5) && CoverHotKeys.Held(3, key => key is 0xA3 or 0xA4) && !CoverHotKeys.Held(3, key => key == 0xA2),
            "without a side either key of a modifier will do");

        // ---- the real windows, over a stand-in game
        Process? dummy = null;
        try
        {
            dummy = Process.Start(new ProcessStartInfo(Environment.ProcessPath!, "--cover-dummy") { UseShellExecute = false });
            if (dummy == null) throw new InvalidOperationException("the stand-in game could not be started");
            int[] dummyIds = [dummy.Id];
            List<(IntPtr Window, ScreenRect Bounds)> found = [];
            for (var attempt = 0; attempt < 100 && found.Count == 0; attempt++)
            {
                Thread.Sleep(100);
                found = DesktopCover.WindowsOf(dummyIds);
            }
            if (found.Count == 0) throw new InvalidOperationException("the stand-in game showed no window");
            Thread.Sleep(400); // Its first picture.
            var (dummyWindow, bounds) = found[0];
            var monitors = DesktopCover.Monitors();
            var expected = DesktopCover.Pieces(bounds, monitors);
            var centre = (X: (bounds.Left + bounds.Right) / 2, Y: (bounds.Top + bounds.Bottom) / 2);

            static bool Magenta((byte R, byte G, byte B) c) => c.R > 200 && c.G < 60 && c.B > 200;
            static bool Black((byte R, byte G, byte B) c) => c.R < 16 && c.G < 16 && c.B < 16;
            if (!Magenta(Grab(bounds).At(centre.X - bounds.Left, centre.Y - bounds.Top)))
            {
                // A locked screen, a remote session without a picture, another always-on-top window in the way.
                report.WriteLine("desktop cover: the screen could not be read back (locked, or something on top of the test window) - the window checks were skipped");
                return ok;
            }

            var inFront = dummyWindow;
            var options = new CoverOptions(Enabled: true, InCapture: true, Pattern: false, PatternBrightness: 100, HotKey: 0);
            var focusBefore = DesktopCover.Native.GetForegroundWindow();
            using var cover = new DesktopCover(options, () => inFront, () => new CoverTarget(dummyIds, "the test window"), ".Test" + Environment.ProcessId + ".windows");

            bool Shown(ScreenRect window)
            {
                var wanted = DesktopCover.Pieces(window, monitors).Select(p => p.Piece).ToList();
                var shown = cover.Snapshot().Where(p => p.Visible).Select(p => p.Rect).ToList();
                return wanted.Count > 0 && wanted.Count == shown.Count && wanted.All(shown.Contains);
            }
            bool Hidden() => cover.Snapshot().All(p => !p.Visible);
            bool Eventually(Func<bool> condition)
            {
                for (var attempt = 0; attempt < 40; attempt++)
                {
                    if (condition()) return true;
                    Thread.Sleep(50);
                }
                return false;
            }

            Check(Shown(bounds) && cover.Status.Covering, $"the game in front is covered: {expected.Count} piece(s), exactly over its window ({bounds.Width} x {bounds.Height})");
            var styles = cover.Snapshot().Where(p => p.Visible).Select(p => DesktopCover.Native.GetWindowLongPtr(p.Window, DesktopCover.Native.GWL_EXSTYLE).ToInt64()).ToList();
            const long wantedStyles = DesktopCover.Native.WS_EX_TOPMOST | DesktopCover.Native.WS_EX_NOACTIVATE | DesktopCover.Native.WS_EX_TOOLWINDOW |
                                      DesktopCover.Native.WS_EX_TRANSPARENT | DesktopCover.Native.WS_EX_LAYERED;
            Check(styles.Count > 0 && styles.All(s => (s & wantedStyles) == wantedStyles), "the cover is on top, click-through, cannot be activated, and is in neither Alt+Tab nor the task bar");
            Check(DesktopCover.Native.GetForegroundWindow() == focusBefore, "showing it left the focus where it was");
            Thread.Sleep(300);
            Check(Black(Grab(bounds).At(centre.X - bounds.Left, centre.Y - bounds.Top)), "the monitor shows black where the game was");

            // Screen capture sees the cover / does not.
            cover.Apply(options with { InCapture = false });
            cover.Snapshot();
            Thread.Sleep(400);
            var affinities = cover.Snapshot().Where(p => p.Visible).Select(p => DesktopCover.Native.GetWindowDisplayAffinity(p.Window, out var a) ? a : 0u).ToList();
            Check(affinities.Count > 0 && affinities.All(a => a == DesktopCover.Native.WDA_EXCLUDEFROMCAPTURE) && Shown(bounds), "\"screen capture does not see the cover\": the cover stays on the monitor, marked as not to be captured");
            Check(Magenta(Grab(bounds).At(centre.X - bounds.Left, centre.Y - bounds.Top)), "and a capture of the screen gets the game underneath");
            cover.Apply(options);
            cover.Snapshot();
            Thread.Sleep(400);
            Check(Black(Grab(bounds).At(centre.X - bounds.Left, centre.Y - bounds.Top)), "\"screen capture sees the cover\": the capture is black again");

            // The pattern: drawn where the monitor's pixels say, wherever the window is.
            bool PatternMatches(ScreenRect window, out int lit)
            {
                var monitor = DesktopCover.Pieces(window, monitors)[0].Monitor;
                var picture = Grab(window);
                var wrong = 0;
                lit = 0;
                for (var y = 2; y < window.Height - 2; y += 3)
                {
                    for (var x = 2; x < window.Width - 2; x += 3)
                    {
                        var shouldBeLit = CoverPattern.Lit(window.Left + x - monitor.Left, window.Top + y - monitor.Top);
                        var isLit = picture.At(x, y).R > 128;
                        if (shouldBeLit) lit++;
                        if (shouldBeLit != isLit) wrong++;
                    }
                }
                return wrong == 0;
            }
            cover.Apply(options with { Pattern = true });
            cover.Snapshot();
            Thread.Sleep(400);
            var patternOk = PatternMatches(bounds, out var litPixels);
            SavePng(Path.Combine(outputDir, "cover-live.png"), Grab(bounds).Pixels, bounds.Width, bounds.Height);
            Check(patternOk && litPixels > 0, $"the pattern is on the cover, mark for mark where the monitor's pixels put it ({litPixels} lit samples checked, cover-live.png)");

            // The game's window moves: the cover follows by itself, and the marks stay where they are on the monitor.
            var moved = new ScreenRect(bounds.Left + 150, bounds.Top + 110, bounds.Right + 150, bounds.Bottom + 110);
            var windowRect = default(DesktopCover.Native.RECT);
            DesktopCover.Native.GetWindowRect(dummyWindow, out windowRect);
            DesktopCover.Native.SetWindowPos(dummyWindow, IntPtr.Zero, windowRect.Left + 150, windowRect.Top + 110, 0, 0, 0x0001 | 0x0004 | 0x0010); // No size, no z-order, no activation.
            Check(Eventually(() => Shown(moved)), "when the game's window moves, the cover follows without being told");
            Thread.Sleep(400);
            var movedOk = PatternMatches(moved, out _);
            Check(movedOk, "and the marks have not moved with it: they belong to the monitor");

            // Something else comes to the front; the game comes back; the switch goes off.
            inFront = IntPtr.Zero;
            cover.Refresh();
            Check(Hidden() && !cover.Status.Covering && cover.Status.Text.Contains("not the window in front"), "anything else in front: no cover (\"" + cover.Status.Text + "\")");
            inFront = dummyWindow;
            cover.Refresh();
            Check(Shown(moved), "the game in front again: covered again");
            cover.Apply(options with { Enabled = false });
            Check(Hidden() && cover.Status.Text.StartsWith("Off"), "switched off: no cover");
            cover.Apply(options);
            Check(Shown(moved), "switched on: covered");
            Check(DesktopCover.Native.GetForegroundWindow() == focusBefore, "through all of this the focus stayed where it was");

            // The game goes.
            dummy.Kill();
            dummy.WaitForExit(3000);
            Check(Eventually(Hidden), "the game is gone: so is the cover");
        }
        catch (Exception error)
        {
            Check(false, "the window checks could not be run: " + error.Message);
        }
        finally
        {
            try { if (dummy is { HasExited: false }) dummy.Kill(); } catch { /* Already gone. */ }
            dummy?.Dispose();
        }
        return ok;
    }

    // ------------------------------------------------------------------ reading the screen, as a capture tool does

    private sealed record Picture(byte[] Pixels, int Width, int Height)
    {
        public (byte R, byte G, byte B) At(int x, int y)
        {
            var at = (Math.Clamp(y, 0, Height - 1) * Width + Math.Clamp(x, 0, Width - 1)) * 4;
            return (Pixels[at + 2], Pixels[at + 1], Pixels[at]);
        }
    }

    private static Picture Grab(ScreenRect area)
    {
        var screen = GetDC(IntPtr.Zero);
        var memory = CreateCompatibleDC(screen);
        var bitmap = CreateCompatibleBitmap(screen, area.Width, area.Height);
        var before = SelectObject(memory, bitmap);
        BitBlt(memory, 0, 0, area.Width, area.Height, screen, area.Left, area.Top, 0x00CC0020 | 0x40000000); // SRCCOPY | CAPTUREBLT
        SelectObject(memory, before);
        var info = new BITMAPINFOHEADER { biSize = 40, biWidth = area.Width, biHeight = -area.Height, biPlanes = 1, biBitCount = 32 };
        var pixels = new byte[area.Width * area.Height * 4];
        GetDIBits(memory, bitmap, 0, (uint)area.Height, pixels, ref info, 0);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(IntPtr.Zero, screen);
        return new Picture(pixels, area.Width, area.Height);
    }

    private static void SavePng(string path, byte[] bgra, int width, int height)
    {
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgr32, null, bgra, width * 4)));
        using var file = File.Create(path);
        encoder.Save(file);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct BITMAPINFOHEADER
    {
        public uint biSize;
        public int biWidth, biHeight;
        public ushort biPlanes, biBitCount;
        public uint biCompression, biSizeImage;
        public int biXPelsPerMeter, biYPelsPerMeter;
        public uint biClrUsed, biClrImportant;
    }

    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
    [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr handle);
    [DllImport("gdi32.dll")] private static extern bool BitBlt(IntPtr target, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(IntPtr dc, IntPtr bitmap, uint start, uint lines, byte[] bits, ref BITMAPINFOHEADER info, uint usage);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr handle);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
}
