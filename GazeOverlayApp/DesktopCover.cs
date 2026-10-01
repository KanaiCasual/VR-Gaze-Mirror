using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace GazeOverlay;

/// <summary>A rectangle in real screen pixels (the virtual desktop's coordinates).</summary>
public readonly record struct ScreenRect(int Left, int Top, int Right, int Bottom)
{
    public int Width => Right - Left;
    public int Height => Bottom - Top;
    public bool IsEmpty => Right <= Left || Bottom <= Top;

    public ScreenRect Intersect(ScreenRect other) =>
        new(Math.Max(Left, other.Left), Math.Max(Top, other.Top), Math.Min(Right, other.Right), Math.Min(Bottom, other.Bottom));
}

/// <summary>What the user chose on the Mirror page's DESKTOP COVER card.</summary>
/// <param name="Enabled">The cover is wanted at all.</param>
/// <param name="InCapture">Screen capture (OBS Display Capture, ...) gets the cover too; false = it gets the game underneath.</param>
/// <param name="Pattern">The fixed pattern of marks is drawn on the black.</param>
/// <param name="PatternBrightness">Its brightness, 5..100 percent of white.</param>
/// <param name="HotKey">Virtual-key code that switches the cover on and off together with Ctrl + Alt; 0 = none.</param>
public sealed record CoverOptions(bool Enabled, bool InCapture, bool Pattern, int PatternBrightness, int HotKey);

/// <summary>The game whose windows are covered: its process(es) and what to call it.</summary>
public sealed record CoverTarget(IReadOnlyCollection<int> ProcessIds, string Name);

/// <summary>For the card's status line.</summary>
public sealed record CoverStatus(bool Covering, string Text);

/// <summary>
/// The pattern that can be drawn on the cover: marks fixed to the MONITOR's pixels, the same every time, so that a
/// headset which finds its place by looking at the room (inside-out tracking) sees something that stays where it is
/// instead of nothing at all. Each mark is a square of 6 x 6 fields with a lit rim and 4 x 4 fields inside that differ
/// from mark to mark - plenty of corners, and no two marks alike, which a repeating checkerboard would not give.
/// Whether a headset makes use of it is up to the headset; nothing here claims that it does.
/// </summary>
public static class CoverPattern
{
    public const int Cell = 192;    // One mark per cell of this many pixels...
    public const int Field = 14;    // ...made of fields this size...
    public const int Fields = 6;    // ...this many each way (84 pixels), in the middle of its cell.
    private const int Margin = (Cell - Field * Fields) / 2;

    /// <summary>Whether field (fieldX, fieldY) of the mark in cell (cellX, cellY) is lit.</summary>
    public static bool Lit(int cellX, int cellY, int fieldX, int fieldY)
    {
        if (fieldX == 0 || fieldY == 0 || fieldX == Fields - 1 || fieldY == Fields - 1) return true; // The rim.
        // The inside: sixteen bits that depend on the cell alone (a fixed scramble, the same on every PC and every day).
        var hash = unchecked((uint)(cellX * 73856093) ^ (uint)(cellY * 19349663) ^ 0x9E3779B9u);
        hash ^= hash >> 16; hash *= 0x7FEB352Du; hash ^= hash >> 15; hash *= 0x846CA68Bu; hash ^= hash >> 16;
        return (hash >> ((fieldY - 1) * 4 + (fieldX - 1)) & 1) != 0;
    }

    /// <summary>Whether the monitor's pixel (x, y) - counted from the monitor's own top left corner - is lit.</summary>
    public static bool Lit(int x, int y)
    {
        int inCellX = x % Cell - Margin, inCellY = y % Cell - Margin;
        if (inCellX < 0 || inCellY < 0 || inCellX >= Field * Fields || inCellY >= Field * Fields) return false;
        return Lit(x / Cell, y / Cell, inCellX / Field, inCellY / Field);
    }

    /// <summary>Where field (fieldX, fieldY) of cell (cellX, cellY) lies on the monitor.</summary>
    public static ScreenRect FieldRect(int cellX, int cellY, int fieldX, int fieldY)
    {
        int left = cellX * Cell + Margin + fieldX * Field, top = cellY * Cell + Margin + fieldY * Field;
        return new ScreenRect(left, top, left + Field, top + Field);
    }

    public static byte Grey(int brightnessPercent) => (byte)(Math.Clamp(brightnessPercent, 5, 100) * 255 / 100);
}

/// <summary>
/// The desktop cover: black over the VR game's own window(s) while the game is the window in front, so that a monitor
/// in front of the headset shows nothing that moves. Opt-in.
///
/// What it covers: every visible top-level window of the game that is in VR - one cover piece for each monitor such a
/// window lies on - and nothing else: no other program's window, never the bare desktop.
/// When: only while a window of that game is the foreground window. The moment anything else is brought to the front,
/// the cover is gone, and the desktop is as usual.
/// Focus: the cover is on top of the game but never takes part in input - it cannot be activated, mouse and keyboard
/// pass through it, and it is in neither Alt+Tab nor the task bar. Showing and hiding it changes nothing about which
/// window has the focus.
/// Nothing is put into the game: these are ordinary windows of this app, so they are gone when the app is closed.
///
/// Nothing here runs on a timer. The cover has a thread of its own that sleeps until Windows reports a change of the
/// foreground window or a move of the game's window, or until a producer (the mirror layer inside an OpenXR game, the
/// SteamVR helper) announces through the ProducerChanged event that the game in VR has changed. While the cover is
/// switched off, none of these are watched at all.
/// </summary>
public sealed class DesktopCover : IDisposable
{
    private const string ProducerChangedName = "GazeMirror2.ProducerChanged";
    private const string WindowClass = "GazeMirrorDesktopCover";
    private const int HotKeyId = 1;
    private const int MinimumSide = 120; // Smaller windows of the game (tool tips, splash strips) are left alone.

    /// <summary>Raised on the cover's own thread whenever what it does (or why not) changes.</summary>
    public event Action<CoverStatus>? StatusChanged;
    /// <summary>Raised on the cover's own thread when the hotkey was pressed.</summary>
    public event Action? HotKeyPressed;

    private readonly Thread _thread;
    private readonly ConcurrentQueue<Action> _work = new();
    private readonly ManualResetEventSlim _started = new();
    private readonly Func<IntPtr>? _foregroundForTest;
    private readonly Func<CoverTarget?>? _targetForTest;
    private uint _threadId;
    private volatile bool _disposed;

    // Everything below belongs to the cover's thread.
    private CoverOptions _options;
    private readonly List<Piece> _pieces = [];
    private readonly Dictionary<IntPtr, Piece> _byWindow = [];
    private readonly List<IntPtr> _gameHooks = [];
    private int[] _hookedProcesses = [];
    private IntPtr _foregroundHook, _minimizeHook, _watchWindow, _producerChanged;
    private int _registeredHotKey;
    private bool _hotKeyTaken;
    private CoverStatus _status = new(false, "");
    private Native.WinEventProc? _eventProc; // Kept alive: Windows calls it.

    // One window class for the whole program, registered once, with a procedure that lives as long as the program:
    // Windows keeps calling it for as long as any window of the class exists, whichever cover made that window.
    private static readonly Native.WndProc ClassProc = OnClassMessage;
    private static readonly ConcurrentDictionary<IntPtr, DesktopCover> Owners = new();
    private static readonly object ClassGate = new();
    private static bool _classRegistered;

    private sealed class Piece
    {
        public IntPtr Window;
        public ScreenRect Rect, Monitor;
        public bool Visible;
        public bool InCapture = true;
    }

    /// <param name="foregroundForTest">Self-test only: stands in for "which window is in front".</param>
    /// <param name="targetForTest">Self-test only: stands in for "which game is in VR".</param>
    public DesktopCover(CoverOptions options, Func<IntPtr>? foregroundForTest = null, Func<CoverTarget?>? targetForTest = null)
    {
        _options = options;
        _foregroundForTest = foregroundForTest;
        _targetForTest = targetForTest;
        _thread = new Thread(Run) { IsBackground = true, Name = "Desktop cover" };
        _thread.Start();
        _started.Wait();
    }

    /// <summary>Takes new choices (from the card, or from the hotkey). Returns at once; the cover follows.</summary>
    public void Apply(CoverOptions options) => Post(() =>
    {
        var before = _options;
        _options = options;
        if (before.HotKey != options.HotKey) RegisterHotKey();
        if (before.Pattern != options.Pattern || before.PatternBrightness != options.PatternBrightness)
        {
            foreach (var piece in _pieces) Native.InvalidateRect(piece.Window, IntPtr.Zero, false);
        }
        Watch(options.Enabled);
        Evaluate();
    });

    /// <summary>Looks again at which game is in VR and which window is in front (the self-test's stand-ins have no events).</summary>
    public void Refresh() => Post(Evaluate);

    /// <summary>The cover pieces as they are now: window, place, shown or not.</summary>
    public IReadOnlyList<(IntPtr Window, ScreenRect Rect, bool Visible)> Snapshot() =>
        Invoke(() => (IReadOnlyList<(IntPtr, ScreenRect, bool)>)_pieces.Select(p => (p.Window, p.Rect, p.Visible)).ToList());

    public CoverStatus Status => Invoke(() => _status);

    /// <summary>The cover's own thread (the self-test plays Windows' part and posts it a hotkey message).</summary>
    internal uint ThreadId => _threadId;

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Native.PostThreadMessage(_threadId, Native.WM_QUIT, IntPtr.Zero, IntPtr.Zero);
        _thread.Join(3000);
    }

    // ------------------------------------------------------------------ pure parts (also used by the self-test)

    /// <summary>One piece per monitor the window lies on: the part of the window on that monitor.</summary>
    public static List<(ScreenRect Piece, ScreenRect Monitor)> Pieces(ScreenRect window, IEnumerable<ScreenRect> monitors)
    {
        var pieces = new List<(ScreenRect, ScreenRect)>();
        foreach (var monitor in monitors)
        {
            var part = window.Intersect(monitor);
            if (!part.IsEmpty) pieces.Add((part, monitor));
        }
        return pieces;
    }

    public static List<ScreenRect> Monitors()
    {
        var monitors = new List<ScreenRect>();
        Native.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr _, IntPtr _, ref Native.RECT rect, IntPtr _) =>
        {
            monitors.Add(new ScreenRect(rect.Left, rect.Top, rect.Right, rect.Bottom));
            return true;
        }, IntPtr.Zero);
        return monitors;
    }

    /// <summary>The visible top-level windows of these processes that are worth covering, with their real (drawn) bounds.</summary>
    public static List<(IntPtr Window, ScreenRect Bounds)> WindowsOf(IReadOnlyCollection<int> processIds)
    {
        var windows = new List<(IntPtr, ScreenRect)>();
        Native.EnumWindows((window, _) =>
        {
            Native.GetWindowThreadProcessId(window, out var processId);
            if (!processIds.Contains((int)processId) || !Native.IsWindowVisible(window) || Native.IsIconic(window)) return true;
            if ((Native.GetWindowLongPtr(window, Native.GWL_EXSTYLE).ToInt64() & Native.WS_EX_TOOLWINDOW) != 0) return true;
            if (Native.DwmGetWindowAttribute(window, Native.DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0) return true;
            // The frame as it is drawn: GetWindowRect includes the invisible border a resizable window has around it.
            if (Native.DwmGetWindowAttribute(window, Native.DWMWA_EXTENDED_FRAME_BOUNDS, out Native.RECT frame, Marshal.SizeOf<Native.RECT>()) != 0)
            {
                if (!Native.GetWindowRect(window, out frame)) return true;
            }
            var bounds = new ScreenRect(frame.Left, frame.Top, frame.Right, frame.Bottom);
            if (bounds.Width >= MinimumSide && bounds.Height >= MinimumSide) windows.Add((window, bounds));
            return true;
        }, IntPtr.Zero);
        return windows;
    }

    /// <summary>The game in VR, from the block the producers publish: the game's own process for an OpenXR game (the
    /// layer runs inside it), the processes of the named program for a SteamVR game (the helper runs next to it).</summary>
    public static CoverTarget? TargetFromProducer()
    {
        if (MirrorLive.ReadProducer() is not { Kind: not 0 } producer) return null;
        if (producer.Kind == MirrorLive.ProducerOpenXR)
        {
            return producer.ProcessId > 0 ? new CoverTarget([producer.ProcessId], producer.Program.Length > 0 ? producer.Program : $"process {producer.ProcessId}") : null;
        }
        if (producer.Program.Length == 0) return null; // SteamVR without a game.
        try
        {
            var found = Process.GetProcessesByName(Path.GetFileNameWithoutExtension(producer.Program));
            var ids = found.Select(p => p.Id).Where(id => id != Environment.ProcessId).ToArray();
            foreach (var process in found) process.Dispose();
            return ids.Length > 0 ? new CoverTarget(ids, producer.Program) : null;
        }
        catch
        {
            return null;
        }
    }

    // ------------------------------------------------------------------ the cover's thread

    private void Post(Action action)
    {
        if (_disposed) return;
        _work.Enqueue(action);
        Native.PostThreadMessage(_threadId, Native.WM_APP, IntPtr.Zero, IntPtr.Zero);
    }

    private T Invoke<T>(Func<T> function)
    {
        if (Native.GetCurrentThreadId() == _threadId) return function();
        T result = default!;
        using var done = new ManualResetEventSlim();
        Post(() => { result = function(); done.Set(); });
        done.Wait(3000);
        return result;
    }

    private void Run()
    {
        _threadId = Native.GetCurrentThreadId();
        Native.PeekMessage(out _, IntPtr.Zero, 0, 0, 0); // Makes the thread's message queue, so that posts arrive.
        _eventProc = OnWinEvent;
        lock (ClassGate)
        {
            if (!_classRegistered)
            {
                var windowClass = new Native.WNDCLASS
                {
                    lpfnWndProc = Marshal.GetFunctionPointerForDelegate(ClassProc),
                    hInstance = Native.GetModuleHandle(null),
                    lpszClassName = WindowClass,
                };
                Native.RegisterClass(ref windowClass);
                _classRegistered = true;
            }
        }
        // An ordinary (never shown) window: it is told when monitors come, go or change.
        _watchWindow = MakeWindow(Native.WS_EX_TOOLWINDOW | Native.WS_EX_NOACTIVATE, "");
        _producerChanged = Native.CreateEvent(IntPtr.Zero, false, false, ProducerChangedName);
        RegisterHotKey();
        Watch(_options.Enabled);
        Evaluate();
        _started.Set();

        var handles = new IntPtr[1];
        while (true)
        {
            // Sleeps until a message arrives or - only while the cover is on - a producer announces a change.
            var watching = _options.Enabled && _producerChanged != IntPtr.Zero;
            handles[0] = _producerChanged;
            var woke = Native.MsgWaitForMultipleObjectsEx(watching ? 1u : 0u, handles, Native.INFINITE, Native.QS_ALLINPUT, Native.MWMO_INPUTAVAILABLE);
            if (watching && woke == 0) Evaluate();
            var quit = false;
            while (Native.PeekMessage(out var message, IntPtr.Zero, 0, 0, Native.PM_REMOVE))
            {
                if (message.message == Native.WM_QUIT) { quit = true; break; }
                if (message.message == Native.WM_HOTKEY && message.hwnd == IntPtr.Zero)
                {
                    try { HotKeyPressed?.Invoke(); } catch (Exception error) { AppLog.Write("Desktop cover: hotkey handler", error); }
                    continue;
                }
                if (message.message == Native.WM_APP && message.hwnd == IntPtr.Zero)
                {
                    while (_work.TryDequeue(out var action))
                    {
                        try { action(); } catch (Exception error) { AppLog.Write("Desktop cover", error); }
                    }
                    continue;
                }
                Native.DispatchMessage(ref message);
            }
            if (quit) break;
        }

        Watch(false);
        if (_registeredHotKey != 0) Native.UnregisterHotKey(IntPtr.Zero, HotKeyId);
        foreach (var piece in _pieces) DropWindow(piece.Window);
        _pieces.Clear();
        _byWindow.Clear();
        if (_watchWindow != IntPtr.Zero) DropWindow(_watchWindow);
        if (_producerChanged != IntPtr.Zero) Native.CloseHandle(_producerChanged);
    }

    private IntPtr MakeWindow(uint exStyle, string title)
    {
        var window = Native.CreateWindowEx(exStyle, WindowClass, title, Native.WS_POPUP, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, Native.GetModuleHandle(null), IntPtr.Zero);
        if (window != IntPtr.Zero) Owners[window] = this;
        return window;
    }

    private static void DropWindow(IntPtr window)
    {
        Native.DestroyWindow(window);
        Owners.TryRemove(window, out _);
    }

    private static IntPtr OnClassMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam) =>
        Owners.TryGetValue(window, out var owner) ? owner.OnWindowMessage(window, message, wParam, lParam) : Native.DefWindowProc(window, message, wParam, lParam);

    /// <summary>Ctrl + Alt + the chosen key, anywhere in Windows. A key somebody else holds is reported, not fought over.</summary>
    private void RegisterHotKey()
    {
        if (_registeredHotKey != 0) Native.UnregisterHotKey(IntPtr.Zero, HotKeyId);
        _registeredHotKey = 0;
        _hotKeyTaken = false;
        if (_options.HotKey is <= 0 or >= 256) return;
        if (Native.RegisterHotKey(IntPtr.Zero, HotKeyId, Native.MOD_CONTROL | Native.MOD_ALT | Native.MOD_NOREPEAT, (uint)_options.HotKey)) _registeredHotKey = _options.HotKey;
        else _hotKeyTaken = true;
    }

    /// <summary>Starts or stops listening to Windows: which window is in front, and windows being minimised or restored.</summary>
    private void Watch(bool on)
    {
        if (on && _foregroundHook == IntPtr.Zero)
        {
            _foregroundHook = Native.SetWinEventHook(Native.EVENT_SYSTEM_FOREGROUND, Native.EVENT_SYSTEM_FOREGROUND, IntPtr.Zero, _eventProc!, 0, 0, Native.WINEVENT_OUTOFCONTEXT);
            _minimizeHook = Native.SetWinEventHook(Native.EVENT_SYSTEM_MINIMIZESTART, Native.EVENT_SYSTEM_MINIMIZEEND, IntPtr.Zero, _eventProc!, 0, 0, Native.WINEVENT_OUTOFCONTEXT);
        }
        else if (!on)
        {
            if (_foregroundHook != IntPtr.Zero) Native.UnhookWinEvent(_foregroundHook);
            if (_minimizeHook != IntPtr.Zero) Native.UnhookWinEvent(_minimizeHook);
            _foregroundHook = _minimizeHook = IntPtr.Zero;
            WatchGame([]);
        }
    }

    /// <summary>The game's own windows moving, resizing, appearing and going: watched for that game's processes only.</summary>
    private void WatchGame(int[] processIds)
    {
        if (processIds.SequenceEqual(_hookedProcesses)) return;
        foreach (var hook in _gameHooks) Native.UnhookWinEvent(hook);
        _gameHooks.Clear();
        foreach (var processId in processIds)
        {
            var hook = Native.SetWinEventHook(Native.EVENT_OBJECT_DESTROY, Native.EVENT_OBJECT_LOCATIONCHANGE, IntPtr.Zero, _eventProc!, (uint)processId, 0, Native.WINEVENT_OUTOFCONTEXT);
            if (hook != IntPtr.Zero) _gameHooks.Add(hook);
        }
        _hookedProcesses = processIds;
    }

    private void OnWinEvent(IntPtr hook, uint eventType, IntPtr window, int objectId, int childId, uint thread, uint time)
    {
        if (eventType is Native.EVENT_SYSTEM_FOREGROUND or Native.EVENT_SYSTEM_MINIMIZESTART or Native.EVENT_SYSTEM_MINIMIZEEND)
        {
            Evaluate();
            return;
        }
        // From the game: only its windows themselves matter (not the mouse pointer, a caret or a control inside), and
        // only their coming, going and moving.
        if (objectId != Native.OBJID_WINDOW || childId != 0 || window == IntPtr.Zero) return;
        if (eventType is not (Native.EVENT_OBJECT_DESTROY or Native.EVENT_OBJECT_SHOW or Native.EVENT_OBJECT_HIDE or Native.EVENT_OBJECT_LOCATIONCHANGE)) return;
        if (Native.GetAncestor(window, Native.GA_ROOT) != window && eventType != Native.EVENT_OBJECT_DESTROY) return;
        Evaluate();
    }

    private IntPtr OnWindowMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam)
    {
        switch (message)
        {
            case Native.WM_NCHITTEST:
                return Native.HTTRANSPARENT;
            case Native.WM_MOUSEACTIVATE:
                return Native.MA_NOACTIVATE;
            case Native.WM_ERASEBKGND:
                return 1;
            case Native.WM_DISPLAYCHANGE:
                if (window == _watchWindow) Evaluate();
                break;
            case Native.WM_PAINT:
                if (_byWindow.TryGetValue(window, out var piece))
                {
                    Paint(piece);
                    return IntPtr.Zero;
                }
                break;
        }
        return Native.DefWindowProc(window, message, wParam, lParam);
    }

    private void Paint(Piece piece)
    {
        var dc = Native.BeginPaint(piece.Window, out var paint);
        Native.FillRect(dc, ref paint.rcPaint, Native.GetStockObject(Native.BLACK_BRUSH));
        if (_options.Pattern)
        {
            var grey = CoverPattern.Grey(_options.PatternBrightness);
            var brush = Native.CreateSolidBrush((uint)(grey | grey << 8 | grey << 16));
            // The marks belong to the monitor: the piece's own top left corner is this far into it.
            int offsetX = piece.Rect.Left - piece.Monitor.Left, offsetY = piece.Rect.Top - piece.Monitor.Top;
            int firstX = (paint.rcPaint.Left + offsetX) / CoverPattern.Cell, lastX = (paint.rcPaint.Right - 1 + offsetX) / CoverPattern.Cell;
            int firstY = (paint.rcPaint.Top + offsetY) / CoverPattern.Cell, lastY = (paint.rcPaint.Bottom - 1 + offsetY) / CoverPattern.Cell;
            for (var cellY = firstY; cellY <= lastY; cellY++)
            {
                for (var cellX = firstX; cellX <= lastX; cellX++)
                {
                    for (var fieldY = 0; fieldY < CoverPattern.Fields; fieldY++)
                    {
                        for (var fieldX = 0; fieldX < CoverPattern.Fields; fieldX++)
                        {
                            if (!CoverPattern.Lit(cellX, cellY, fieldX, fieldY)) continue;
                            var field = CoverPattern.FieldRect(cellX, cellY, fieldX, fieldY);
                            var rect = new Native.RECT { Left = field.Left - offsetX, Top = field.Top - offsetY, Right = field.Right - offsetX, Bottom = field.Bottom - offsetY };
                            Native.FillRect(dc, ref rect, brush);
                        }
                    }
                }
            }
            Native.DeleteObject(brush);
        }
        Native.EndPaint(piece.Window, ref paint);
    }

    /// <summary>The one place that decides: is there a game in VR, is it in front, and where are its windows.</summary>
    private void Evaluate()
    {
        var hotKey = _hotKeyTaken ? "  The hotkey is already used by another program." : "";
        if (!_options.Enabled)
        {
            HideAll();
            Report(false, "Off." + hotKey);
            return;
        }
        var target = _targetForTest != null ? _targetForTest() : TargetFromProducer();
        WatchGame(target?.ProcessIds.OrderBy(id => id).ToArray() ?? []);
        if (target == null)
        {
            HideAll();
            Report(false, "On. No game is in VR right now, so nothing is covered." + hotKey);
            return;
        }
        var foreground = _foregroundForTest != null ? _foregroundForTest() : Native.GetForegroundWindow();
        Native.GetWindowThreadProcessId(foreground, out var foregroundProcess);
        if (foreground == IntPtr.Zero || !target.ProcessIds.Contains((int)foregroundProcess))
        {
            HideAll();
            Report(false, $"On. {target.Name} is in VR but not the window in front, so nothing is covered." + hotKey);
            return;
        }
        var monitors = Monitors();
        var wanted = new List<(ScreenRect Piece, ScreenRect Monitor)>();
        foreach (var (_, bounds) in WindowsOf(target.ProcessIds)) wanted.AddRange(Pieces(bounds, monitors));
        if (wanted.Count == 0)
        {
            HideAll();
            Report(false, $"On. {target.Name} has no window to cover." + hotKey);
            return;
        }

        var realForeground = Native.GetForegroundWindow();
        while (_pieces.Count < wanted.Count)
        {
            // Click-through, never active, on top, in neither Alt+Tab nor the task bar.
            var window = MakeWindow(Native.WS_EX_TOPMOST | Native.WS_EX_NOACTIVATE | Native.WS_EX_TOOLWINDOW | Native.WS_EX_TRANSPARENT | Native.WS_EX_LAYERED,
                "VR Gaze Mirror - desktop cover");
            if (window == IntPtr.Zero) break;
            Native.SetLayeredWindowAttributes(window, 0, 255, Native.LWA_ALPHA);
            var piece = new Piece { Window = window };
            _pieces.Add(piece);
            _byWindow[window] = piece;
        }
        for (var i = 0; i < _pieces.Count; i++)
        {
            var piece = _pieces[i];
            if (i >= wanted.Count)
            {
                Hide(piece);
                continue;
            }
            var (rect, monitor) = wanted[i];
            if (piece.InCapture != _options.InCapture)
            {
                // "Excluded from capture": on the monitor as always, absent from anything that records the screen.
                Native.SetWindowDisplayAffinity(piece.Window, _options.InCapture ? Native.WDA_NONE : Native.WDA_EXCLUDEFROMCAPTURE);
                piece.InCapture = _options.InCapture;
            }
            if (piece.Visible && piece.Rect == rect && piece.Monitor == monitor) continue;
            var repaint = piece.Monitor != monitor || piece.Rect.Left - piece.Monitor.Left != rect.Left - monitor.Left || piece.Rect.Top - piece.Monitor.Top != rect.Top - monitor.Top;
            piece.Rect = rect;
            piece.Monitor = monitor;
            Native.SetWindowPos(piece.Window, Native.HWND_TOPMOST, rect.Left, rect.Top, rect.Width, rect.Height, Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW);
            if (repaint) Native.InvalidateRect(piece.Window, IntPtr.Zero, false);
            piece.Visible = true;
        }
        // The cover must never be what has the focus. It cannot be by its styles; should Windows ever have made one of
        // its windows the foreground window all the same, the window that was in front gets the focus back - once.
        var now = Native.GetForegroundWindow();
        if (now != realForeground && _byWindow.ContainsKey(now) && realForeground != IntPtr.Zero)
        {
            Native.SetForegroundWindow(realForeground);
            AppLog.Write("Desktop cover: a cover window had become the foreground window; the focus was given back.");
        }
        var count = wanted.Select(w => w.Monitor).Distinct().Count();
        Report(true, $"Covering {target.Name} on {count} monitor{(count == 1 ? "" : "s")}." + hotKey);
    }

    private void Hide(Piece piece)
    {
        if (!piece.Visible) return;
        Native.ShowWindow(piece.Window, Native.SW_HIDE);
        piece.Visible = false;
    }

    private void HideAll()
    {
        foreach (var piece in _pieces) Hide(piece);
    }

    private void Report(bool covering, string text)
    {
        if (_status.Covering == covering && _status.Text == text) return;
        if (_status.Covering != covering || covering) AppLog.Write("Desktop cover: " + text);
        _status = new CoverStatus(covering, text);
        try { StatusChanged?.Invoke(_status); } catch (Exception error) { AppLog.Write("Desktop cover: status handler", error); }
    }

    // ------------------------------------------------------------------ Windows

    internal static class Native
    {
        public const uint WM_QUIT = 0x0012, WM_PAINT = 0x000F, WM_ERASEBKGND = 0x0014, WM_MOUSEACTIVATE = 0x0021, WM_NCHITTEST = 0x0084,
            WM_DISPLAYCHANGE = 0x007E, WM_HOTKEY = 0x0312, WM_APP = 0x8000;
        public const uint WS_POPUP = 0x80000000;
        public const uint WS_EX_TOPMOST = 0x8, WS_EX_TRANSPARENT = 0x20, WS_EX_TOOLWINDOW = 0x80, WS_EX_LAYERED = 0x80000, WS_EX_NOACTIVATE = 0x08000000;
        public const int GWL_EXSTYLE = -20;
        public const uint LWA_ALPHA = 0x2;
        public const uint SWP_NOACTIVATE = 0x0010, SWP_SHOWWINDOW = 0x0040;
        public const int SW_HIDE = 0;
        public static readonly IntPtr HWND_TOPMOST = new(-1);
        public static readonly IntPtr HTTRANSPARENT = new(-1);
        public static readonly IntPtr MA_NOACTIVATE = new(3);
        public const uint WDA_NONE = 0x0, WDA_EXCLUDEFROMCAPTURE = 0x11;
        public const uint EVENT_SYSTEM_FOREGROUND = 0x0003, EVENT_SYSTEM_MINIMIZESTART = 0x0016, EVENT_SYSTEM_MINIMIZEEND = 0x0017;
        public const uint EVENT_OBJECT_DESTROY = 0x8001, EVENT_OBJECT_SHOW = 0x8002, EVENT_OBJECT_HIDE = 0x8003, EVENT_OBJECT_LOCATIONCHANGE = 0x800B;
        public const uint WINEVENT_OUTOFCONTEXT = 0;
        public const int OBJID_WINDOW = 0;
        public const uint GA_ROOT = 2;
        public const uint MOD_ALT = 0x1, MOD_CONTROL = 0x2, MOD_NOREPEAT = 0x4000;
        public const uint QS_ALLINPUT = 0x04FF, MWMO_INPUTAVAILABLE = 0x0004, INFINITE = 0xFFFFFFFF, PM_REMOVE = 0x0001;
        public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9, DWMWA_CLOAKED = 14;
        public const int BLACK_BRUSH = 4;

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int X, Y; }

        [StructLayout(LayoutKind.Sequential)]
        public struct MSG
        {
            public IntPtr hwnd;
            public uint message;
            public IntPtr wParam, lParam;
            public uint time;
            public POINT pt;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PAINTSTRUCT
        {
            public IntPtr hdc;
            public int fErase;
            public RECT rcPaint;
            public int fRestore, fIncUpdate;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] rgbReserved;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct WNDCLASS
        {
            public uint style;
            public IntPtr lpfnWndProc;
            public int cbClsExtra, cbWndExtra;
            public IntPtr hInstance, hIcon, hCursor, hbrBackground;
            public string? lpszMenuName;
            public string lpszClassName;
        }

        public delegate IntPtr WndProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        public delegate void WinEventProc(IntPtr hook, uint eventType, IntPtr window, int objectId, int childId, uint thread, uint time);
        public delegate bool EnumWindowsProc(IntPtr window, IntPtr lParam);
        public delegate bool MonitorEnumProc(IntPtr monitor, IntPtr dc, ref RECT rect, IntPtr data);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern ushort RegisterClass(ref WNDCLASS windowClass);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateWindowEx(uint exStyle, string className, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
        [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr window);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr DefWindowProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG message, IntPtr window, uint first, uint last, uint remove);
        [DllImport("user32.dll")] public static extern IntPtr DispatchMessage(ref MSG message);
        [DllImport("user32.dll")] public static extern bool PostThreadMessage(uint thread, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] public static extern uint MsgWaitForMultipleObjectsEx(uint count, IntPtr[] handles, uint milliseconds, uint wakeMask, uint flags);
        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
        [DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr window, uint colorKey, byte alpha, uint flags);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool SetWindowDisplayAffinity(IntPtr window, uint affinity);
        [DllImport("user32.dll")] public static extern bool GetWindowDisplayAffinity(IntPtr window, out uint affinity);
        [DllImport("user32.dll")] public static extern bool InvalidateRect(IntPtr window, IntPtr rect, bool erase);
        [DllImport("user32.dll")] public static extern IntPtr BeginPaint(IntPtr window, out PAINTSTRUCT paint);
        [DllImport("user32.dll")] public static extern bool EndPaint(IntPtr window, ref PAINTSTRUCT paint);
        [DllImport("user32.dll")] public static extern int FillRect(IntPtr dc, ref RECT rect, IntPtr brush);
        [DllImport("gdi32.dll")] public static extern IntPtr GetStockObject(int index);
        [DllImport("gdi32.dll")] public static extern IntPtr CreateSolidBrush(uint color);
        [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr handle);
        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
        [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr window, int index);
        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorEnumProc callback, IntPtr data);
        [DllImport("user32.dll")] public static extern IntPtr SetWinEventHook(uint eventMin, uint eventMax, IntPtr module, WinEventProc callback, uint process, uint thread, uint flags);
        [DllImport("user32.dll")] public static extern bool UnhookWinEvent(IntPtr hook);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint key);
        [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr window, int id);
        [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out RECT value, int size);
        [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr window, int attribute, out int value, int size);
        [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandle(string? name);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateEvent(IntPtr security, bool manualReset, bool initialState, string name);
        [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    }
}
