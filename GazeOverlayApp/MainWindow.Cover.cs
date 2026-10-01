using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace GazeOverlay;

/// <summary>
/// The DESKTOP COVER card of the Mirror tab: the on/off switch, whether screen capture sees the cover, the optional
/// pattern, the hotkey, and a line that says what the cover is doing right now. The cover itself is DesktopCover; it
/// lives as long as this window does, so closing the app takes any cover away with it.
/// </summary>
public partial class MainWindow
{
    private static readonly (string Name, int Percent)[] CoverBrightnessChoices = [("Dim", 25), ("Medium", 50), ("Bright", 100)];
    private DesktopCover? _cover;
    private bool _coverKeyListening;

    private CoverOptions CoverChoices => new(_appSettings.CoverEnabled, _appSettings.CoverInCapture, _appSettings.CoverPattern, _appSettings.CoverPatternBrightness, _appSettings.CoverHotKey);

    private void BuildCoverUi()
    {
        _loading = true;
        CoverEnabled.IsChecked = _appSettings.CoverEnabled;
        CoverInCapture.IsChecked = _appSettings.CoverInCapture;
        CoverPattern.IsChecked = _appSettings.CoverPattern;
        foreach (var (name, percent) in CoverBrightnessChoices) CoverBrightness.Items.Add(new ComboBoxItem { Content = name, Tag = percent });
        CoverBrightness.SelectedItem = CoverBrightness.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (int)i.Tag == _appSettings.CoverPatternBrightness) ?? CoverBrightness.Items[0];
        _loading = false;
        UpdateCoverUi();

        CoverEnabled.Click += (_, _) => OnCoverChoice();
        CoverInCapture.Click += (_, _) => OnCoverChoice();
        CoverPattern.Click += (_, _) => OnCoverChoice();
        CoverBrightness.SelectionChanged += (_, _) => OnCoverChoice();
        PreviewKeyDown += OnCoverKeyChosen;

        // A self-test or screenshot run shows no cover and takes no hotkey.
        if (App.Headless) return;
        _cover = new DesktopCover(CoverChoices);
        // Both arrive on the cover's own thread.
        _cover.StatusChanged += status => Dispatcher.BeginInvoke(() => CoverStatusText.Text = status.Text);
        _cover.HotKeyPressed += () => Dispatcher.BeginInvoke(() =>
        {
            _appSettings.CoverEnabled = !_appSettings.CoverEnabled;
            _appSettings.Save();
            CoverEnabled.IsChecked = _appSettings.CoverEnabled;
            AppLog.Write($"Desktop cover: switched {(_appSettings.CoverEnabled ? "on" : "off")} with the hotkey.");
            _cover?.Apply(CoverChoices);
        });
        CoverStatusText.Text = _cover.Status.Text;
        Closed += (_, _) => { _cover?.Dispose(); _cover = null; };
    }

    private void OnCoverChoice()
    {
        if (_loading) return;
        _appSettings.CoverEnabled = CoverEnabled.IsChecked == true;
        _appSettings.CoverInCapture = CoverInCapture.IsChecked == true;
        _appSettings.CoverPattern = CoverPattern.IsChecked == true;
        _appSettings.CoverPatternBrightness = CoverBrightness.SelectedItem is ComboBoxItem { Tag: int percent } ? percent : 25;
        _appSettings.Save();
        UpdateCoverUi();
        AppLog.Write($"Desktop cover: {(_appSettings.CoverEnabled ? "on" : "off")}, screen capture {(_appSettings.CoverInCapture ? "sees it" : "does not see it")}, pattern {(_appSettings.CoverPattern ? $"on ({_appSettings.CoverPatternBrightness} %)" : "off")}.");
        _cover?.Apply(CoverChoices);
    }

    private void UpdateCoverUi()
    {
        CoverBrightness.IsEnabled = _appSettings.CoverPattern;
        CoverKey.Content = _coverKeyListening ? "Press a key..." : _appSettings.CoverHotKey > 0 ? "Ctrl + Alt + " + KeyName(_appSettings.CoverHotKey) : "None";
    }

    private void OnCoverKeyClick(object sender, RoutedEventArgs e)
    {
        _coverKeyListening = !_coverKeyListening;
        UpdateCoverUi();
    }

    private void OnCoverKeyChosen(object sender, KeyEventArgs e)
    {
        if (!_coverKeyListening) return;
        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftShift or Key.RightShift or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin) return;
        _coverKeyListening = false;
        // Esc = no hotkey at all.
        _appSettings.CoverHotKey = key != Key.Escape && KeyInterop.VirtualKeyFromKey(key) is > 0 and < 256 and var virtualKey ? virtualKey : 0;
        _appSettings.Save();
        UpdateCoverUi();
        _cover?.Apply(CoverChoices);
    }
}
