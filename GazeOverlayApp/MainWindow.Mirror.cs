using System.Windows;
using System.Windows.Controls;

namespace GazeOverlay;

/// <summary>
/// The "Mirror window" row of the Mirror tab: opens and steers MirrorWindow.exe, the capturable window that shows the mirror
/// picture (for OBS Window Capture, Discord, ...). The window normally fills a monitor BEHIND the game, where it cannot be
/// reached, so its settings live here. Choices are remembered in app.json and applied live when the window is open.
/// </summary>
public partial class MainWindow
{
    private static readonly (int Width, int Height)[] MirrorSizes = [(3840, 2160), (2560, 1440), (1920, 1080), (1600, 900), (1280, 720)];
    private string _mirrorArguments = "";

    private void BuildMirrorUi()
    {
        _loading = true;
        MirrorPlacement.Items.Add(new ComboBoxItem { Content = "An ordinary window", Tag = 0 });
        foreach (var monitor in MirrorWindowControl.Monitors())
        {
            MirrorPlacement.Items.Add(new ComboBoxItem { Content = $"Monitor {monitor.Number} ({monitor.Width} x {monitor.Height}), behind everything", Tag = monitor.Number });
        }
        MirrorPlacement.SelectedItem = MirrorPlacement.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (int)i.Tag == _appSettings.MirrorMonitor) ?? MirrorPlacement.Items[0];

        MirrorTitled.IsChecked = _appSettings.MirrorTitled;

        MirrorSize.Items.Add(new ComboBoxItem { Content = "Fill the monitor", Tag = "fill" });
        foreach (var (width, height) in MirrorSizes)
        {
            MirrorSize.Items.Add(new ComboBoxItem { Content = $"{width} x {height}", Tag = $"{width}x{height}" });
        }
        MirrorSize.Items.Add(new ComboBoxItem { Content = "Custom", Tag = "custom" });
        var savedSize = _appSettings.MirrorOutputWidth >= 64 && _appSettings.MirrorOutputHeight >= 64 ? $"{_appSettings.MirrorOutputWidth}x{_appSettings.MirrorOutputHeight}" : "fill";
        MirrorSize.SelectedItem = MirrorSize.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == savedSize) ?? MirrorSize.Items[^1];
        MirrorCustomWidth.Text = (_appSettings.MirrorOutputWidth >= 64 ? _appSettings.MirrorOutputWidth : 1920).ToString();
        MirrorCustomHeight.Text = (_appSettings.MirrorOutputHeight >= 64 ? _appSettings.MirrorOutputHeight : 1080).ToString();
        MirrorCustomSize.Visibility = MirrorSize.SelectedItem is ComboBoxItem { Tag: "custom" } ? Visibility.Visible : Visibility.Collapsed;

        MirrorRate.Items.Add(new ComboBoxItem { Content = "60 fps", Tag = 60 });
        MirrorRate.Items.Add(new ComboBoxItem { Content = "30 fps", Tag = 30 });
        MirrorRate.SelectedIndex = _appSettings.MirrorFps <= 30 ? 1 : 0;
        _loading = false;
        _mirrorArguments = MirrorWindowControl.Arguments(_appSettings);

        MirrorPlacement.SelectionChanged += (_, _) => OnMirrorSettingChanged();
        MirrorTitled.Click += (_, _) => OnMirrorSettingChanged();
        MirrorSize.SelectionChanged += (_, _) => OnMirrorSettingChanged();
        MirrorRate.SelectionChanged += (_, _) => OnMirrorSettingChanged();
        foreach (var box in new[] { MirrorCustomWidth, MirrorCustomHeight })
        {
            box.LostFocus += (_, _) => OnMirrorSettingChanged();
            box.KeyDown += (_, e) => { if (e.Key == System.Windows.Input.Key.Enter) OnMirrorSettingChanged(); };
        }

        // Whether the window is open is looked up when it can have changed and somebody is looking - not on a timer.
        // (The Gaze page shows what arrives from the headset, SRanibro and VRChat: the same look, on the same occasions.)
        bool ShowsLiveState() => ReferenceEquals(Tabs.SelectedItem, CropTab) || ReferenceEquals(Tabs.SelectedItem, GazeTab);
        Tabs.SelectionChanged += (_, e) => { if (ReferenceEquals(e.OriginalSource, Tabs) && ShowsLiveState()) RefreshMirrorState(); };
        Activated += (_, _) => { if (ShowsLiveState()) RefreshMirrorState(); };
        BuildOutputAndProfilesUi();
        // The three cards go side by side when there is room, two and one below that, and one under another in a narrow window.
        MirrorCards.SizeChanged += (_, _) => MirrorCards.Columns = MirrorCards.ActualWidth >= 900 ? 3 : MirrorCards.ActualWidth >= 620 ? 2 : 1;
        RefreshMirrorState();
    }

    private void OnMirrorSettingChanged()
    {
        if (_loading) return;
        _appSettings.MirrorMonitor = MirrorPlacement.SelectedItem is ComboBoxItem { Tag: int monitor } ? monitor : 0;
        _appSettings.MirrorTitled = MirrorTitled.IsChecked == true;
        _appSettings.MirrorFps = MirrorRate.SelectedItem is ComboBoxItem { Tag: int fps } ? fps : 60;

        var size = MirrorSize.SelectedItem is ComboBoxItem { Tag: string tag } ? tag : "fill";
        MirrorCustomSize.Visibility = size == "custom" ? Visibility.Visible : Visibility.Collapsed;
        if (size == "custom")
        {
            // Typed numbers: anything unusable keeps the last good size (and is put back in the boxes).
            if (int.TryParse(MirrorCustomWidth.Text.Trim(), out var width) && int.TryParse(MirrorCustomHeight.Text.Trim(), out var height)
                && width is >= 64 and <= 16384 && height is >= 64 and <= 16384)
            {
                (_appSettings.MirrorOutputWidth, _appSettings.MirrorOutputHeight) = (width, height);
            }
            else if (_appSettings.MirrorOutputWidth < 64 || _appSettings.MirrorOutputHeight < 64)
            {
                (_appSettings.MirrorOutputWidth, _appSettings.MirrorOutputHeight) = (1920, 1080);
            }
            MirrorCustomWidth.Text = _appSettings.MirrorOutputWidth.ToString();
            MirrorCustomHeight.Text = _appSettings.MirrorOutputHeight.ToString();
        }
        else
        {
            var parts = size.Split('x');
            (_appSettings.MirrorOutputWidth, _appSettings.MirrorOutputHeight) = parts.Length == 2 ? (int.Parse(parts[0]), int.Parse(parts[1])) : (0, 0);
        }
        var arguments = MirrorWindowControl.Arguments(_appSettings);
        if (arguments == _mirrorArguments) return; // e.g. a size box lost the focus with nothing changed
        _mirrorArguments = arguments;
        _appSettings.Save();
        if (MirrorWindowControl.IsRunning()) MirrorWindowControl.Apply(_appSettings); // live
        RefreshMirrorState();
    }

    private void RefreshMirrorState(string? note = null)
    {
        RefreshMirrorLive();
        RefreshProfileUi();
        var program = MirrorWindowControl.FindProgram();
        var running = program != null && MirrorWindowControl.IsRunning();
        MirrorOpen.IsEnabled = program != null && !running;
        MirrorClose.IsEnabled = running;
        MirrorStatus.Text = note ?? (program == null ? "MirrorWindow.exe was not found next to this app."
            : running ? "The mirror window is open. Capture \"VR Gaze Mirror - Mirror\" with OBS Window Capture, Discord, ..."
            : "The mirror window is closed.");
    }

    private async void OnMirrorOpen(object sender, RoutedEventArgs e)
    {
        if (!MirrorWindowControl.Apply(_appSettings))
        {
            RefreshMirrorState("The mirror window could not be started.");
            AppLog.Write("Mirror window: could not be started.");
            return;
        }
        AppLog.Write("Mirror window: opened (" + MirrorWindowControl.Arguments(_appSettings) + ").");
        MirrorOpen.IsEnabled = false;
        // A new, unsigned program is scanned by antivirus software on its first start; that can take several seconds.
        for (var attempt = 0; attempt < 40 && !MirrorWindowControl.IsRunning(); attempt++)
        {
            RefreshMirrorState("Starting the mirror window...");
            MirrorOpen.IsEnabled = false;
            await Task.Delay(500);
        }
        RefreshMirrorState();
    }

    private async void OnMirrorClose(object sender, RoutedEventArgs e)
    {
        AppLog.Write("Mirror window: closed.");
        MirrorWindowControl.Close();
        for (var attempt = 0; attempt < 10 && MirrorWindowControl.IsRunning(); attempt++) await Task.Delay(200);
        RefreshMirrorState();
    }
}
