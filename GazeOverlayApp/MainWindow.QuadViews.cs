using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GazeOverlay;

/// <summary>
/// The Quad Views tab: the settings TallyMouse's QuadViews Companion edits, plus how many pixels they make the game render.
/// Unlike the ring's settings these only take effect when a game starts, so they are saved with Apply, not as you drag.
/// </summary>
public partial class MainWindow
{
    private readonly string _quadViewsPath = QuadViewsFile.DefaultPath;
    private QuadViewsFile _quadViewsFile = QuadViewsFile.Load(QuadViewsFile.DefaultPath);
    private QuadViewsSession? _quadViewsSession;
    private readonly Dictionary<string, double> _quadViewsValues = [];
    private readonly HashSet<string> _quadViewsDirty = [];
    private readonly Dictionary<string, Action<double>> _quadViewsSetters = [];
    private bool _quadViewsLoading;
    private bool _quadViewsBackedUp;
    private CheckBox? _quadViewsTurboBox;

    private void BuildQuadViewsUi()
    {
        QuadViewsNotice.Visibility = Visibility.Collapsed;
        ShowToolTipsPatiently(QuadViewsTitle);

        // Two cards: where the sharp part is, and how much is rendered. Side by side when there is room.
        var focusIds = new HashSet<string> { "focus_h", "focus_v", "offset_v", "transition" };
        var focus = new StackPanel();
        var resolution = new StackPanel();
        var cards = new Grid { Margin = new Thickness(0, 0, -10, 0) };
        var focusCard = NewCard("FOCUS", focus, 0);
        var resolutionCard = NewCard("RESOLUTION", resolution, 0);
        focusCard.Margin = resolutionCard.Margin = new Thickness(0, 0, 10, 10);
        cards.Children.Add(focusCard);
        cards.Children.Add(resolutionCard);
        LayOutCards(cards, sideBySide: true);
        // One under the other in a narrower window, and then no wider than a row of settings reads well.
        PanelQuadViews.SizeChanged += (_, _) =>
        {
            var sideBySide = PanelQuadViews.ActualWidth >= 900;
            LayOutCards(cards, sideBySide);
            cards.HorizontalAlignment = sideBySide ? HorizontalAlignment.Stretch : HorizontalAlignment.Left;
            cards.Width = sideBySide ? double.NaN : Math.Min(PanelQuadViews.ActualWidth, 720) + 10;
        };
        PanelQuadViews.Children.Add(cards);

        foreach (var setting in QuadViewsSetting.All)
        {
            var row = NewRow(setting.Description);
            if (setting.IsToggle)
            {
                var box = new CheckBox { Content = setting.Label, FontWeight = FontWeights.SemiBold, ToolTip = WrappedToolTip(setting.Description) };
                ShowToolTipsPatiently(box);
                box.Checked += (_, _) => OnQuadViewsChanged(setting.Id, 1);
                box.Unchecked += (_, _) => OnQuadViewsChanged(setting.Id, 0);
                _quadViewsSetters[setting.Id] = v => box.IsChecked = v != 0;
                if (setting.Id == "turbo") _quadViewsTurboBox = box;
                AddRowEditor(row, box, column: 0, span: 3);
            }
            else
            {
                AddRowLabel(row, setting.Label, setting.Description);
                var slider = new Slider
                {
                    Minimum = setting.Min, Maximum = setting.Max, SmallChange = setting.Step, LargeChange = setting.Step * 5,
                    TickFrequency = setting.Step, IsSnapToTickEnabled = true,
                };
                var valueText = AddRowValue(row, text =>
                {
                    var digits = new string(text.Trim().TakeWhile(ch => char.IsDigit(ch) || ch is '.' or ',' or '-').ToArray()).Replace(',', '.');
                    if (double.TryParse(digits, NumberStyles.Float, CultureInfo.InvariantCulture, out var number)) slider.Value = Math.Clamp(Math.Round(number / setting.Step) * setting.Step, setting.Min, setting.Max);
                });
                // The default is Quad-Views-Foveated's own, read from the file it ships for this headset's runtime.
                slider.MouseDoubleClick += (_, _) => { if (QuadViewsLayerDefaults().TryGetValue(setting.Id, out var standard)) slider.Value = Math.Clamp(standard, setting.Min, setting.Max); };
                Describe(slider, setting.Label, setting.Description + "\n\nDouble-click the slider for Quad-Views-Foveated's own default. Click the value to type one.");
                slider.ValueChanged += (_, e) =>
                {
                    valueText.Text = $"{e.NewValue:0} {setting.Suffix}";
                    OnQuadViewsChanged(setting.Id, e.NewValue);
                };
                _quadViewsSetters[setting.Id] = v =>
                {
                    slider.Value = Math.Clamp(Math.Round(v / setting.Step) * setting.Step, setting.Min, setting.Max);
                    valueText.Text = $"{slider.Value:0} {setting.Suffix}";
                };
                AddRowEditor(row, slider);
            }
            (focusIds.Contains(setting.Id) ? focus : resolution).Children.Add(row);
        }

        // Presets sit in the banner at the top, next to Apply, so they are reachable without scrolling.
        foreach (var preset in QuadViewsSetting.Presets)
        {
            var button = new Button { Content = preset.Name, ToolTip = WrappedToolTip(preset.Description), Margin = new Thickness(0, 2, 6, 2) };
            button.Click += (_, _) =>
            {
                foreach (var (id, value) in preset.Values.Count > 0 ? preset.Values : QuadViewsLayerDefaults())
                {
                    _quadViewsValues[id] = value;
                    _quadViewsDirty.Add(id);
                    _quadViewsLoading = true;
                    _quadViewsSetters[id](value);
                    _quadViewsLoading = false;
                }
                RefreshQuadViewsReadout();
            };
            QuadViewsPresets.Children.Add(button);
        }

        QuadViewsSlots.Children.Add(BuildSlotBar(_appSettings.QuadViewsSlots,
            capture: () => QuadViewsSetting.All.Where(s => !s.IsToggle).ToDictionary(s => s.Id, s => _quadViewsValues.GetValueOrDefault(s.Id).ToString("R", CultureInfo.InvariantCulture)),
            apply: values =>
            {
                foreach (var setting in QuadViewsSetting.All.Where(s => !s.IsToggle))
                {
                    if (!values.TryGetValue(setting.Id, out var text) || !double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value)) continue;
                    _quadViewsValues[setting.Id] = value;
                    _quadViewsDirty.Add(setting.Id);
                    _quadViewsLoading = true;
                    _quadViewsSetters[setting.Id](value);
                    _quadViewsLoading = false;
                }
                RefreshQuadViewsReadout();
            },
            describe: values => string.Join("\n", QuadViewsSetting.All.Where(s => !s.IsToggle && values.ContainsKey(s.Id)).Select(s =>
                $"{s.Label}: {(double.TryParse(values[s.Id], NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v.ToString("0") : values[s.Id])} {s.Suffix}"))
                + "\n\nLike a preset, a slot sets the sliders; click Apply afterwards."));

        // Pick up what another tool (the Companion, a text editor) saved - when the tab is opened, not on a timer.
        Tabs.SelectionChanged += (_, e) =>
        {
            if (ReferenceEquals(e.OriginalSource, Tabs) && ReferenceEquals(Tabs.SelectedItem, QuadViewsTab) && _quadViewsDirty.Count == 0) LoadQuadViews();
        };
        // Coming back from the game (or the Companion) with this tab open: show the new game start, again without a timer.
        // Under Gaze from = Fixed centre the new game start matters on every page (the sharp area follows it).
        Activated += (_, _) =>
        {
            var fixedCentre = _values.GetValueOrDefault("gaze_source") == "fixed";
            if ((ReferenceEquals(Tabs.SelectedItem, QuadViewsTab) || fixedCentre) && _quadViewsDirty.Count == 0) LoadQuadViews(keepStatus: true);
        };
    }

    /// <summary>Slider values for "QV defaults": the shipped settings file alone, as the layer reads it for this runtime.</summary>
    private Dictionary<string, double> QuadViewsLayerDefaults()
    {
        var files = new List<(string, QuadViewsFile)>();
        var shipped = _quadViewsSession?.ConfigPaths.FirstOrDefault(p => !string.Equals(p, _quadViewsPath, StringComparison.OrdinalIgnoreCase));
        if (shipped != null && File.Exists(shipped))
        {
            try { files.Add(("the file shipped with the layer", QuadViewsFile.Load(shipped))); } catch { /* unreadable: the built-in values remain */ }
        }
        var defaults = QuadViewsFile.Evaluate(files, _quadViewsSession);
        return QuadViewsSetting.All.Where(s => !s.IsToggle).ToDictionary(s => s.Id, s => s.ToSlider(defaults[s.Keys[0]]));
    }

    private void OnQuadViewsChanged(string id, double value)
    {
        if (_quadViewsLoading) return;
        _quadViewsValues[id] = value;
        _quadViewsDirty.Add(id);
        RefreshQuadViewsReadout();
    }

    private void LoadQuadViews(bool keepStatus = false)
    {
        _quadViewsSession = QuadViewsSession.Read(QuadViewsSession.LogPath);
        _quadViewsFile = QuadViewsFile.Load(_quadViewsPath);
        _quadViewsDirty.Clear();

        var effective = EvaluateQuadViews(_quadViewsFile);
        var common = EvaluateQuadViews(_quadViewsFile, commonOnly: true);
        _quadViewsLoading = true;
        foreach (var setting in QuadViewsSetting.All)
        {
            // What the file says it wants; where it says nothing, what the layer falls back to.
            var key = setting.Keys[0];
            var fileValue = ParseQuadViewsValue(key, setting.CommonOnly ? _quadViewsFile.InCommon(key) : _quadViewsFile.Intended(key))
                            ?? (setting.CommonOnly ? common[key] : effective[key]);
            var shown = setting.IsToggle ? fileValue : setting.ToSlider(fileValue);
            _quadViewsValues[setting.Id] = shown;
            _quadViewsSetters[setting.Id](shown);
        }
        _quadViewsLoading = false;

        if (!keepStatus) SetQuadViewsStatus(_quadViewsFile.Existed ? "" : "No settings file yet, so the game uses Quad-Views-Foveated's defaults. Apply creates one.");
        RefreshQuadViewsReadout();
        SyncFixedFocusKeys(); // The fixed centre's sharp area follows the file.
    }

    private static double? ParseQuadViewsValue(string key, string? text) =>
        text != null && double.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number) ? QuadViewsFile.Clamp(key, number) : null;

    private QuadViewsFile PlannedQuadViewsFile() => QuadViewsFile.Plan(_quadViewsFile, _quadViewsValues, _quadViewsDirty);

    private QuadViewsEffective EvaluateQuadViews(QuadViewsFile userFile, bool commonOnly = false)
    {
        var files = new List<(string, QuadViewsFile)>();
        // The file shipped with the layer is read first; the log says where it is.
        var shipped = _quadViewsSession?.ConfigPaths.FirstOrDefault(p => !string.Equals(p, _quadViewsPath, StringComparison.OrdinalIgnoreCase));
        if (shipped != null && File.Exists(shipped))
        {
            try { files.Add(("the file shipped with the layer", QuadViewsFile.Load(shipped))); } catch { /* unreadable: evaluate without it */ }
        }
        files.Add(("your settings file", userFile));
        return QuadViewsFile.Evaluate(files, commonOnly ? null : _quadViewsSession);
    }

    private QuadViewsPixels.Result? QuadViewsPixelsFor(QuadViewsEffective effective)
    {
        if (_quadViewsSession is not { HasResolution: true } session) return null;
        var (horizontal, vertical) = QuadViewsSections(effective);
        return QuadViewsPixels.Compute(session.StereoWidth, session.StereoHeight, effective["peripheral_multiplier"], effective["focus_multiplier"], horizontal, vertical);
    }

    /// <summary>With eye tracking the layer sizes the sharp region from the focus keys, without it from the fixed ones.</summary>
    private (double Horizontal, double Vertical) QuadViewsSections(QuadViewsEffective effective) =>
        _quadViewsSession?.EyeTracking == false
            ? (effective["horizontal_fixed_section"], effective["vertical_fixed_section"])
            : (effective["horizontal_focus_section"], effective["vertical_focus_section"]);

    private void RefreshQuadViewsReadout()
    {
        var session = _quadViewsSession;
        var saved = EvaluateQuadViews(_quadViewsFile);
        var planned = PlannedQuadViewsFile();
        var next = EvaluateQuadViews(planned);
        var culture = CultureInfo.CurrentCulture;

        QuadViewsApply.IsEnabled = planned.Text != _quadViewsFile.Text;
        QuadViewsRevert.IsEnabled = _quadViewsDirty.Count > 0;

        // ---- render load
        if (session is { HasResolution: true })
        {
            var savedPixels = QuadViewsPixelsFor(saved)!;
            var nextPixels = QuadViewsPixelsFor(next)!;
            var lastDetail = session.QuadPixels is { } last
                ? $"Last game start {session.When:g}: {last.ToString("N0", culture)} px/frame ({100.0 * last / session.StereoPixels:0.0} % of the full {session.StereoWidth}x{session.StereoHeight} per eye), " +
                  $"sharp {session.FocusResolution}, surround {session.PeripheralResolution}. Runtime: {session.RuntimeName}."
                : $"Last game start {session.When:g}: the game did not use quad views. Full resolution is {session.StereoWidth}x{session.StereoHeight} per eye.";
            var gameIsBehind = session.QuadPixels is { } ran && ran != savedPixels.Total;
            QuadViewsLoadLast.Text = gameIsBehind ? "The game last ran with other settings. Restart it to use the saved ones." : "";
            QuadViewsLoadLast.Visibility = gameIsBehind ? Visibility.Visible : Visibility.Collapsed;
            var savedShare = (double)savedPixels.Total / session.StereoPixels;
            var nextShare = (double)nextPixels.Total / session.StereoPixels;
            static string Fewer(double share) => share > 0 && share < 0.95 ? $", about {1 / share:0.#} times fewer pixels" : "";
            QuadViewsLoadSaved.ToolTip = WrappedToolTip($"Saved: {savedPixels.Total.ToString("N0", culture)} pixels a frame.\n\n" + lastDetail +
                                                         "\n\nPixels are a guide to GPU load. A game that waits on the CPU gains no frame rate from fewer pixels.");
            QuadViewsLoadSaved.Text = $"Saved settings: the game draws {100 * savedShare:0} % of the full picture{Fewer(savedShare)}.";
            var change = 100.0 * (nextPixels.Total - savedPixels.Total) / savedPixels.Total;
            QuadViewsLoadNext.ToolTip = WrappedToolTip($"After Apply: {nextPixels.Total.ToString("N0", culture)} pixels a frame: the sharp region {nextPixels.FocusWidth} x {nextPixels.FocusHeight} " +
                                                        $"and the surround {nextPixels.PeripheralWidth} x {nextPixels.PeripheralHeight} per eye.");
            QuadViewsLoadNext.Text = Math.Abs(change) < 0.05
                ? "After Apply: no change."
                : $"After Apply: {100 * nextShare:0} % of the full picture, {(change > 0 ? "more" : "fewer")} pixels than now ({(change > 0 ? "+" : "")}{change:0} %).";
            QuadViewsLoadNext.Foreground = Math.Abs(change) < 0.05 ? (Brush)FindResource("Muted") : new SolidColorBrush(change > 0 ? Colors.Orange : Colors.MediumSeaGreen);
        }
        else
        {
            double Share(QuadViewsEffective e) { var (h, v) = QuadViewsSections(e); return 100 * QuadViewsPixels.Fraction(e["peripheral_multiplier"], e["focus_multiplier"], h, v); }
            QuadViewsLoadLast.Text = "Quad-Views-Foveated has logged no game start yet, so the app does not know your headset's resolution. Run the game once to see real pixel counts here.";
            QuadViewsLoadLast.Visibility = Visibility.Visible;
            QuadViewsLoadSaved.Text = $"Saved settings: about {Share(saved):0.0} % of the full-resolution pixel count.";
            QuadViewsLoadNext.Text = $"After Apply: about {Share(next):0.0} %.";
            QuadViewsLoadNext.Foreground = (Brush)FindResource("Muted");
        }

        // ---- what Apply tidies, and what the file sets apart from the sliders
        if (planned.Text != _quadViewsFile.Text && _quadViewsDirty.Count == 0)
        {
            QuadViewsLoadNext.Text += " Apply writes the whole file fresh, in the app's own layout.";
        }
        var overrides = planned.Overrides().Select(o =>
        {
            var section = o.Section[1..^1];
            section = section.StartsWith("app:", StringComparison.Ordinal) || section.StartsWith("exe:", StringComparison.Ordinal) ? section[4..] : section;
            var setting = QuadViewsSetting.All.FirstOrDefault(s => s.Keys.Contains(o.Key));
            if (setting == null || !double.TryParse(o.Value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var number)) return null;
            var shown = setting.IsToggle ? (number != 0 ? "on" : "off") : $"{setting.ToSlider(number):0.#} {setting.Suffix}";
            return $"{section}: {setting.Label.ToLowerInvariant()} {shown}";
        }).Where(text => text != null).Distinct().ToList();
        QuadViewsOverrides.Text = overrides.Count > 0 ? "Set apart in the file: " + string.Join("; ", overrides) + "." : "";
        QuadViewsOverrides.Visibility = overrides.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        // ---- settings the file sets, but not for this runtime
        var problems = new List<string>();
        if (session != null)
        {
            foreach (var setting in QuadViewsSetting.All.Where(s => !s.IsToggle))
            {
                var key = session.EyeTracking == false && setting.Keys.Length > 1 ? setting.Keys[1] : setting.Keys[0];
                if (ParseQuadViewsValue(key, _quadViewsFile.Intended(key)) is not { } wanted || Math.Abs(wanted - saved[key]) < 0.0005) continue;
                var source = saved.DecidedBy.TryGetValue(key, out var decidedBy) ? decidedBy : "the layer's built-in value";
                problems.Add($"{setting.Label}: the file says {setting.ToSlider(wanted):0} %, the game gets {setting.ToSlider(saved[key]):0} % (from {source}).");
            }
        }
        var turboNote = session != null && _quadViewsValues.GetValueOrDefault("turbo") != 0 && next["turbo_mode"] == 0 && next.DecidedBy.TryGetValue("turbo_mode", out var turboBy)
            ? $"Turbo mode stays off with your runtime: {turboBy} switches it off on purpose." : null;

        if (_quadViewsTurboBox != null)
        {
            _quadViewsTurboBox.Content = turboNote != null ? "Turbo mode (stays off with your runtime)" : "Turbo mode";
            _quadViewsTurboBox.ToolTip = WrappedToolTip((turboNote != null ? turboNote + "\n\n" : "") + QuadViewsSetting.All.First(s => s.Id == "turbo").Description);
        }
        if (problems.Count > 0)
        {
            QuadViewsNoticeText.Text =
                $"Some settings in the file do not reach your game. The file holds them under headset sections, and none of those match your OpenXR runtime (\"{session!.RuntimeName}\"):\n  - " +
                string.Join("\n  - ", problems) + "\nApply also writes them to the common part of the file, which every runtime reads." +
                (turboNote != null ? "\n" + turboNote : "");
            QuadViewsNotice.Visibility = Visibility.Visible;
        }
        else
        {
            QuadViewsNotice.Visibility = Visibility.Collapsed;
        }
    }

    private void OnQuadViewsApply(object sender, RoutedEventArgs e)
    {
        try
        {
            // Somebody else may have saved the file since it was loaded: apply the changes to what is there now.
            _quadViewsFile = QuadViewsFile.Load(_quadViewsPath);
            var planned = PlannedQuadViewsFile();

            string? backup = null;
            if (_quadViewsFile.Existed && !_quadViewsBackedUp)
            {
                backup = Path.Combine(QuadViewsSession.Folder, $"settings.before-gaze-overlay-{DateTime.Now:yyyyMMdd-HHmmss}.cfg");
                File.Copy(_quadViewsPath, backup, overwrite: false);
                _quadViewsBackedUp = true;
            }
            planned.Save(_quadViewsPath);
            AppLog.Write($"Quad Views settings applied to {_quadViewsPath}" + (backup != null ? $" (previous kept as {Path.GetFileName(backup)})" : "") + ".");
            LoadQuadViews();
            SetQuadViewsStatus($"Saved at {DateTime.Now:T}. The game uses it from its next start." +
                                   (backup != null ? $" The app kept your previous file as {Path.GetFileName(backup)}." : ""));
        }
        catch (Exception error)
        {
            SetQuadViewsStatus("Could not save: " + error.Message);
            AppLog.Write("Quad Views settings could not be saved", error);
        }
    }

    private void SetQuadViewsStatus(string text)
    {
        QuadViewsStatus.Text = text;
        QuadViewsStatus.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnQuadViewsRevert(object sender, RoutedEventArgs e) => LoadQuadViews();

    private void OnQuadViewsOpenFile(object sender, RoutedEventArgs e) => OpenQuadViewsFile(_quadViewsPath);

    private void OnQuadViewsOpenLog(object sender, RoutedEventArgs e) => OpenQuadViewsFile(QuadViewsSession.LogPath);

    private void OpenQuadViewsFile(string path)
    {
        if (!File.Exists(path))
        {
            SetQuadViewsStatus("That file does not exist yet: " + path);
            return;
        }
        OpenFileOrShow(path);
    }
}
