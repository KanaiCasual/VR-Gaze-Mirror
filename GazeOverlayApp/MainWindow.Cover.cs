using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace GazeOverlay;

/// <summary>
/// The Desktop cover page: the on/off switch, whether screen capture sees the cover, the hotkey, a line that says what
/// the cover is doing right now, and the optional pattern with a look at it. The cover itself is DesktopCover; it
/// lives as long as this window does, so closing the app takes any cover away with it.
/// </summary>
public partial class MainWindow
{
    private static readonly (string Name, int Percent)[] CoverBrightnessChoices = [("Dim", 25), ("Medium", 50), ("Bright", 100)];
    private DesktopCover? _cover;
    private bool _coverKeyListening;

    private CoverOptions CoverChoices => new(_appSettings.CoverEnabled, _appSettings.CoverInCapture, _appSettings.CoverPattern, _appSettings.CoverPatternBrightness,
        _appSettings.CoverHotKey, _appSettings.CoverHotKeyModifiers);

    private void BuildCoverUi()
    {
        _loading = true;
        CoverEnabled.IsChecked = _appSettings.CoverEnabled;
        CoverInCapture.IsChecked = _appSettings.CoverInCapture;
        CoverPattern.IsChecked = _appSettings.CoverPattern;
        // A hotkey saved before the sides existed took either key; the button names the key, so it becomes the left one.
        var withSides = CoverHotKeys.WithSides(_appSettings.CoverHotKeyModifiers);
        if (withSides != _appSettings.CoverHotKeyModifiers)
        {
            _appSettings.CoverHotKeyModifiers = withSides;
            if (!App.Headless) _appSettings.Save();
        }
        foreach (var (name, percent) in CoverBrightnessChoices) CoverBrightness.Items.Add(new ComboBoxItem { Content = name, Tag = percent });
        CoverBrightness.SelectedItem = CoverBrightness.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (int)i.Tag == _appSettings.CoverPatternBrightness) ?? CoverBrightness.Items[0];
        _loading = false;
        UpdateCoverUi();

        CoverEnabled.Click += (_, _) => OnCoverChoice();
        CoverInCapture.Click += (_, _) => OnCoverChoice();
        CoverPattern.Click += (_, _) => OnCoverChoice();
        CoverBrightness.SelectionChanged += (_, _) => OnCoverChoice();
        PreviewKeyDown += OnCoverKeyChosen;
        // Side by side when there is room, one under the other in a narrow window.
        LayOutCards(CoverCards, sideBySide: true);
        CoverCards.SizeChanged += (_, _) => LayOutCards(CoverCards, CoverCards.ActualWidth >= 620);

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
        // What the Status page and the rail show of a running game follows the same announcement, on whatever page the app is.
        _cover.ProducerChanged += () => Dispatcher.BeginInvoke(UpdateLive);
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
        // Three marks as they are on the monitor (its top left corner), at the chosen brightness - dimmed further while the pattern is off.
        const int width = GazeOverlay.CoverPattern.Cell * 3, height = GazeOverlay.CoverPattern.Cell;
        var grey = (byte)(GazeOverlay.CoverPattern.Grey(_appSettings.CoverPatternBrightness) * (_appSettings.CoverPattern ? 1.0 : 0.35));
        var pixels = new byte[width * height];
        for (var y = 0; y < height; y++)
        {
            for (var x = 0; x < width; x++) pixels[y * width + x] = GazeOverlay.CoverPattern.Lit(x, y) ? grey : (byte)0;
        }
        CoverPreview.Source = System.Windows.Media.Imaging.BitmapSource.Create(width, height, 96, 96, System.Windows.Media.PixelFormats.Gray8, null, pixels, width);
        CoverKey.Content = _coverKeyListening ? "Press the keys..." : CoverHotKeys.Text(_appSettings.CoverHotKey, _appSettings.CoverHotKeyModifiers, KeyName);
    }

    private void OnCoverKeyClick(object sender, RoutedEventArgs e)
    {
        _coverKeyListening = !_coverKeyListening;
        CoverKeyNote.Visibility = Visibility.Collapsed;
        UpdateCoverUi();
    }

    private void OnCoverKeyChosen(object sender, KeyEventArgs e)
    {
        if (!_coverKeyListening) return;
        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftShift or Key.RightShift or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin) return;
        // The combination is whatever is held when the key goes down: any of Ctrl, Alt, Shift and the Windows key, or none -
        // each with the key of it that is down, left or right, or both of them.
        var held = Keyboard.Modifiers;
        int Modifier(ModifierKeys which, int modifier, Key left, Key right)
        {
            if (!held.HasFlag(which)) return 0;
            var sides = (Keyboard.IsKeyDown(left) ? modifier << 4 : 0) | (Keyboard.IsKeyDown(right) ? modifier << 8 : 0);
            return modifier | (sides != 0 ? sides : modifier << 4);
        }
        var modifiers = Modifier(ModifierKeys.Alt, CoverHotKeys.Alt, Key.LeftAlt, Key.RightAlt) | Modifier(ModifierKeys.Control, CoverHotKeys.Ctrl, Key.LeftCtrl, Key.RightCtrl)
                      | Modifier(ModifierKeys.Shift, CoverHotKeys.Shift, Key.LeftShift, Key.RightShift) | Modifier(ModifierKeys.Windows, CoverHotKeys.Win, Key.LWin, Key.RWin);
        var virtualKey = KeyInterop.VirtualKeyFromKey(key);
        if (key == Key.Escape && modifiers == 0)
        {
            // Esc on its own = no hotkey at all.
            _coverKeyListening = false;
            _appSettings.CoverHotKey = 0;
        }
        else if (CoverHotKeys.Allowed(virtualKey, modifiers))
        {
            _coverKeyListening = false;
            (_appSettings.CoverHotKey, _appSettings.CoverHotKeyModifiers) = (virtualKey, modifiers);
        }
        else
        {
            // Still listening: say why that one will not do.
            CoverKeyNote.Text = $"{CoverHotKeys.Text(virtualKey, modifiers, KeyName)} would switch the cover each time you type it. Hold Ctrl, Alt or the Windows key with it, or use a function key.";
            CoverKeyNote.Visibility = Visibility.Visible;
            return;
        }
        CoverKeyNote.Visibility = Visibility.Collapsed;
        _appSettings.Save();
        UpdateCoverUi();
        _cover?.Apply(CoverChoices);
    }
}
