using System.Windows;
using Microsoft.Win32;

namespace GazeOverlay;

/// <summary>
/// Two choices on the About page, both off by default. Start with Windows: a value under the user's own Run key, so
/// the app opens at sign-in (no administrator rights, no service). Start in the tray: the app lives in the notification
/// area - it opens hidden, the close button hides the window, and the tray icon's menu opens it again or quits. The
/// desktop cover's hotkey and the mirror window work from the tray as they do from the window.
/// </summary>
public partial class MainWindow
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string RunValueName = "VR Gaze Mirror";
    private System.Windows.Forms.NotifyIcon? _tray;
    private bool _quitting;

    private void InitializeTrayUi()
    {
        _loading = true;
        StartWithWindowsBox.IsChecked = _appSettings.StartWithWindows;
        StartInTrayBox.IsChecked = _appSettings.StartInTray;
        _loading = false;
        if (App.Headless) return;
        // The Run value names this exe: written again at each start, so an update that moved the app keeps working.
        ApplyStartWithWindows(_appSettings.StartWithWindows);
        if (_appSettings.StartInTray) ShowTrayIcon();
        Closing += (_, e) =>
        {
            if (_quitting || _tray == null) return;
            e.Cancel = true; // Lives in the tray: the close button hides the window.
            Hide();
        };
        Closed += (_, _) => HideTrayIcon();
    }

    /// <summary>Called in place of Show(): hidden in the tray when that is wanted, else the window as usual.</summary>
    public void ShowOrStartInTray()
    {
        if (_tray != null && _appSettings.StartInTray)
        {
            AppLog.Write("Started in the tray.");
            return;
        }
        Show();
    }

    private void OnStartupPreferenceChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !IsLoaded) return;
        _appSettings.StartWithWindows = StartWithWindowsBox.IsChecked == true;
        _appSettings.StartInTray = StartInTrayBox.IsChecked == true;
        _appSettings.Save();
        ApplyStartWithWindows(_appSettings.StartWithWindows);
        if (_appSettings.StartInTray) ShowTrayIcon();
        else HideTrayIcon();
        AppLog.Write($"Start with Windows: {(_appSettings.StartWithWindows ? "on" : "off")}; start in the tray: {(_appSettings.StartInTray ? "on" : "off")}.");
    }

    private static void ApplyStartWithWindows(bool on)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath);
            if (key == null) return;
            if (on && Environment.ProcessPath is { Length: > 0 } exe) key.SetValue(RunValueName, $"\"{exe}\"");
            else key.DeleteValue(RunValueName, throwOnMissingValue: false);
        }
        catch (Exception error)
        {
            AppLog.Write("The Run entry could not be written", error);
        }
    }

    private void ShowTrayIcon()
    {
        if (_tray != null) return;
        System.Drawing.Icon icon;
        try
        {
            var stream = System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/Assets/icon.ico"))?.Stream;
            icon = stream != null ? new System.Drawing.Icon(stream) : System.Drawing.SystemIcons.Application;
        }
        catch
        {
            icon = System.Drawing.SystemIcons.Application;
        }
        var menu = new System.Windows.Forms.ContextMenuStrip();
        menu.Items.Add("Open VR Gaze Mirror", null, (_, _) => ShowFromTray());
        menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
        menu.Items.Add("Quit", null, (_, _) => QuitFromTray());
        _tray = new System.Windows.Forms.NotifyIcon { Icon = icon, Text = "VR Gaze Mirror", ContextMenuStrip = menu, Visible = true };
        _tray.MouseClick += (_, e) => { if (e.Button == System.Windows.Forms.MouseButtons.Left) ShowFromTray(); };
        _tray.DoubleClick += (_, _) => ShowFromTray();
    }

    private void HideTrayIcon()
    {
        if (_tray == null) return;
        _tray.Visible = false;
        _tray.Dispose();
        _tray = null;
    }

    internal void ShowFromTray()
    {
        Show();
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Activate();
    }

    private void QuitFromTray()
    {
        _quitting = true;
        AppLog.Write("Quit from the tray.");
        Close();
        System.Windows.Application.Current.Shutdown();
    }
}
