using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace GazeOverlay;

public partial class MainWindow : Window
{
    private readonly string _configPath = ConfigFile.DefaultPath;
    private Dictionary<string, string> _values = Settings.Defaults();
    private readonly Dictionary<string, Action<string>> _controlSetters = [];
    private readonly Dictionary<string, ComboBox> _choiceCombos = []; // Choice rows whose items are switched on and off later.
    private bool _configLoaded; // The ring settings have been read: keys the app fills in by itself may be written.
    private readonly DispatcherTimer _saveTimer = new() { Interval = TimeSpan.FromMilliseconds(180) };
    private readonly DispatcherTimer _previewTimer = new() { Interval = TimeSpan.FromMilliseconds(25) };
    private readonly DispatcherTimer _liveTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private bool _loading;
    private readonly HashSet<string> _dirtyKeys = [];
    private FileSystemWatcher? _configWatcher;
    private DateTime _ownWriteUntil = DateTime.MinValue;
    private readonly DispatcherTimer _outsideChangeTimer = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private readonly AppSettings _appSettings = AppSettings.Load();
    private ReleaseInfo? _availableUpdate;

    public MainWindow()
    {
        InitializeComponent();

        _saveTimer.Tick += (_, _) => { _saveTimer.Stop(); SaveNow(); };
        _previewTimer.Tick += (_, _) => { _previewTimer.Stop(); RenderPreview(); };
        _liveTimer.Tick += (_, _) => UpdateLive();
        _outsideChangeTimer.Tick += (_, _) => { _outsideChangeTimer.Stop(); ApplyOutsideChange(); };

        RestoreWindowPlacement();
        BuildNav();
        BuildSettingsUi();
        BuildAboutRows();
        BuildQuadViewsUi();
        BuildCropUi();
        BuildMirrorUi();
        BuildCoverUi();
        BuildExplanations();
        LoadQuadViews();
        AboutText.Text =
            "VR Gaze Mirror mirrors your VR view for OBS and window capture, with a ring that shows where you look.\n\n" +
            "Its parts:\n" +
            "  - the mirror layer (OpenXR games) and the SteamVR helper (SteamVR games) make the picture: cropped, steadied, with the ring;\n" +
            "  - the OBS plugin \"Gaze Mirror\" and the mirror window show it;\n" +
            "  - this app holds the settings.\n" +
            "This project wrote all of that code (MIT licence; the OBS plugin under the GPL-2, as OBS plugins are). It modifies no third-party code: " +
            "Quad-Views-Foveated by Matthieu Bucchianeri is optional and installs unchanged (this app ships his official installer), " +
            "and the SteamVR helper uses Valve's OpenVR library. Please do not ask those authors for support with this app. " +
            "The licence texts are in the install folder under Program Files.\n\n" +
            "The files carry no code signature, so Windows may warn about the installer, and games with anti-cheat may refuse to load the layers.\n\n" +
            "The Quad Views page edits the same settings as QuadViews Companion by TallyMouse and follows its slider logic, so you can use both on the same file. " +
            "TallyMouse did not make this app and does not endorse it.\n\n" +
            "This app edits the two settings files below. It runs without administrator rights: " +
            "switch a layer on or off on the Status page and Windows asks for permission, then Windows' own reg.exe changes that one value. An installer (an update, Quad-Views-Foveated) starts at your click, and Windows asks there too.\n\n" +
            "Ring settings:\n  " + _configPath + "\n" +
            "Quad-Views-Foveated settings:\n  " + QuadViewsFile.DefaultPath;

        InitializeUpdateUi();
        InitializeTrayUi();
        LoadConfig();
        RefreshStatus();
        StartWatchingConfigFile();
        Tabs.SelectionChanged += (_, e) => { if (ReferenceEquals(e.OriginalSource, Tabs)) { UpdateLiveTimer(); UpdatePanels(); } };
        SizeChanged += (_, _) => Dispatcher.BeginInvoke(UpdatePanels); // after the layout pass that raised it
        Loaded += (_, _) => UpdatePanels();
        Closing += (_, _) => { SaveWindowPlacement(); AppLog.Write("Closing."); };
        Activated += (_, _) => UpdateLiveTimer();
        Deactivated += (_, _) => UpdateLiveTimer();
        UpdateLiveTimer();
    }

    /// <summary>The once-a-second gaze light only ticks while it can be seen: Status tab showing and the window active.</summary>
    private void UpdateLiveTimer()
    {
        var visible = (ReferenceEquals(Tabs.SelectedItem, StatusTab) || ReferenceEquals(Tabs.SelectedItem, GazeTab)) && IsActive;
        if (visible == _liveTimer.IsEnabled) return;
        if (visible) { UpdateLive(); _liveTimer.Start(); } else { _liveTimer.Stop(); }
    }

    // ------------------------------------------------------------------ the rail

    /// <summary>One line of the rail: a page - its name, the icon that stands for it (alone when the window is narrow), the
    /// page itself and a dot while it has something live to say - or the heading of a group of pages (no page; it cannot
    /// be chosen).</summary>
    public sealed class NavItem(string title, string icon, TabItem? tab, bool isHeading = false) : System.ComponentModel.INotifyPropertyChanged
    {
        public string Title { get; } = title;
        public string Icon { get; } = icon;
        public TabItem? Tab { get; } = tab;
        public bool IsHeading { get; } = isHeading;

        private Brush? _dot;
        public Brush? Dot
        {
            get => _dot;
            set { _dot = value; PropertyChanged?.Invoke(this, new System.ComponentModel.PropertyChangedEventArgs(nameof(Dot))); }
        }

        public event System.ComponentModel.PropertyChangedEventHandler? PropertyChanged;
    }

    // Glyphs of Segoe Fluent Icons that Segoe MDL2 Assets (Windows 10) has too.
    private static readonly Dictionary<string, string> PageIcons = new()
    {
        ["Status"] = "\uE9D9", ["Mirror"] = "\uE7A8", ["Gaze"] = "\uE890", ["Ring"] = "\uEA3A",
        ["Desktop cover"] = "\uE7F4", ["Quad Views"] = "\uE8A9", ["About"] = "\uE946",
    };

    private NavItem? _statusNav; // The Status page's line: it carries the dot that says a game is being mirrored.
    private Brush? _liveDot;

    private bool _navQuadViews = true;

    /// <summary>The rail lists the pages; it and the page control select together.</summary>
    private void BuildNav()
    {
        FillNav();
        Nav.SelectionChanged += (_, _) => { if (Nav.SelectedItem is NavItem { Tab: { } tab } && !ReferenceEquals(Tabs.SelectedItem, tab)) Tabs.SelectedItem = tab; };
        Tabs.SelectionChanged += (_, e) =>
        {
            if (!ReferenceEquals(e.OriginalSource, Tabs)) return;
            Nav.SelectedItem = Nav.Items.Cast<NavItem>().FirstOrDefault(i => ReferenceEquals(i.Tab, Tabs.SelectedItem));
        };
        RailVersion.Text = UpdateChecker.CurrentVersionText;
        Title = $"VR Gaze Mirror {UpdateChecker.CurrentVersionText}"; // The version in the title bar too (it is what people quote).
    }

    /// <summary>The Quad Views page is only listed while Quad-Views-Foveated is installed: there is nothing to set otherwise.</summary>
    private void ShowQuadViewsPage(bool show)
    {
        if (show == _navQuadViews) return;
        _navQuadViews = show;
        FillNav();
    }

    private void FillNav()
    {
        // The pages in their order, with a heading where a group begins: the mirror is what the program is for and comes
        // first; the ring's four pages stand together; what is neither comes after them.
        var items = new List<NavItem>();
        foreach (var tab in Tabs.Items.Cast<TabItem>().Where(t => _navQuadViews || !ReferenceEquals(t, QuadViewsTab)))
        {
            if (ReferenceEquals(tab, GazeTab)) items.Add(new NavItem("GAZE RING", "", null, isHeading: true));
            if (ReferenceEquals(tab, CoverTab)) items.Add(new NavItem("EXTRAS", "", null, isHeading: true));
            if (ReferenceEquals(tab, AboutTab)) items.Add(new NavItem("", "", null, isHeading: true));
            items.Add(new NavItem((string)tab.Header, PageIcons.GetValueOrDefault((string)tab.Header) ?? "\uE713", tab));
        }
        _statusNav = items.FirstOrDefault(i => ReferenceEquals(i.Tab, Tabs.Items[0]));
        if (_statusNav != null) _statusNav.Dot = _liveDot;
        Nav.ItemsSource = items;
        if (!items.Any(i => i.Tab != null && ReferenceEquals(i.Tab, Tabs.SelectedItem))) Tabs.SelectedIndex = 0;
        Nav.SelectedItem = items.FirstOrDefault(i => ReferenceEquals(i.Tab, Tabs.SelectedItem));
    }

    private bool _railCompact;

    /// <summary>Below this window width the rail shows the icons alone, so the pages keep their room.</summary>
    private const double RailNeedsWindowWidth = 720;

    private void UpdateRail()
    {
        var compact = ActualWidth < RailNeedsWindowWidth;
        if (compact == _railCompact && Nav.ItemTemplate != null) return;
        _railCompact = compact;
        Nav.ItemTemplate = (DataTemplate)FindResource(compact ? "RailShort" : "RailFull");
        Nav.MinWidth = compact ? 0 : 158;
        RailBrand.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        RailVersion.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        SaveMarkText.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        SaveMark.Margin = compact ? new Thickness(13, 6, 0, 0) : new Thickness(10, 6, 0, 0);
        Rail.Padding = compact ? new Thickness(6, 14, 6, 10) : new Thickness(10, 14, 10, 10);
    }

    // ------------------------------------------------------------------ settings UI

    /// <summary>The Gaze page's cards, one topic each; a key of its group not named here goes in the last card.</summary>
    private static readonly (string Title, string[] Keys)[] PlacementCards =
    [
        ("WHERE THE RING LANDS", ["projection", "focus_distance", "offset_x", "offset_y"]),
        ("CALIBRATION", ["headset_marker", "use_raw_gaze"]),
    ];

    /// <summary>The Ring page's first card: what most people set (a preset does the rest).</summary>
    private static readonly string[] RingBasics = ["enabled", "style", "color", "radius"];

    /// <summary>
    /// The rest of the ring, in sections that fold away: each a few topics with a small heading. A key of a section's
    /// group that is named nowhere goes at the end of the last section of that group.
    /// </summary>
    private static readonly (string Key, string Header, string Group, (string Title, string[] Keys)[] Topics)[] RingSections =
    [
        ("ring-look", "More of the look: line, glow and blending", Settings.GroupLook,
        [
            ("LINE", ["thickness", "feather"]),
            ("GLOW AND BLENDING", ["glow", "glow_strength", "opacity", "blend", "solidity", "fill_opacity", "shadow_opacity"]),
        ]),
        ("ring-tail", "Tail: the trace behind the ring", Settings.GroupTail,
        [
            ("TAIL", ["tail_opacity", "trail_ms", "blob_trail_ms", "tail_space", "tail_max"]),
            ("HEATMAP STYLE", ["heat_ms", "heat_cool_ms"]),
        ]),
        ("ring-motion", "Motion: smoothing, dwell and blinks", Settings.GroupMotion,
        [
            ("SMOOTHING", ["filter", "filter_min_cutoff", "filter_beta", "smoothing_ms"]),
            ("DWELL", ["dwell_ms", "dwell_shrink"]),
            ("BLINKS AND TRACKING LOSS", ["hold_ms", "fade_ms", "timeout_ms"]),
        ]),
    ];

    private readonly Dictionary<string, FrameworkElement> _rowsByKey = [];

    /// <summary>"Ignore Quad-Views-Foveated's focus offsets" only means something with the Legacy placement method: it is shown with it alone.</summary>
    private void UpdateDependentRows()
    {
        if (_rowsByKey.TryGetValue("use_raw_gaze", out var row)) row.Visibility = (_values.GetValueOrDefault("projection") ?? "layer") == "producer" ? Visibility.Visible : Visibility.Collapsed;
    }

    private void BuildSettingsUi()
    {
        var placed = new HashSet<string>();
        void AddRows(Panel body, string group, IEnumerable<string> keys)
        {
            foreach (var key in keys)
            {
                var def = Settings.All.FirstOrDefault(d => d.Key == key && d.Group == group);
                if (def == null) continue;
                var row = BuildRow(def);
                _rowsByKey[key] = row;
                body.Children.Add(row);
                placed.Add(key);
            }
        }
        IEnumerable<string> NotPlaced(string group) => Settings.All.Where(d => d.Group == group && !placed.Contains(d.Key)).Select(d => d.Key).ToList();

        // ---- the Gaze page: cards, one topic each
        for (var i = 0; i < PlacementCards.Length; i++)
        {
            var body = new StackPanel();
            AddRows(body, Settings.GroupPlacement, PlacementCards[i].Keys);
            if (i == PlacementCards.Length - 1) AddRows(body, Settings.GroupPlacement, NotPlaced(Settings.GroupPlacement));
            PanelPlacement.Children.Add(NewCard(PlacementCards[i].Title, body, i == 0 ? 0 : 10));
        }

        // ---- the Ring page: the presets, one card with what most people set, and the rest in sections that fold away
        LookHeaderBar.Children.Add(BuildPresets());
        var basics = new StackPanel();
        AddRows(basics, Settings.GroupLook, RingBasics);
        PanelLook.Children.Add(NewCard("RING", basics, 0));
        for (var i = 0; i < RingSections.Length; i++)
        {
            var (key, header, group, topics) = RingSections[i];
            var body = new StackPanel();
            foreach (var (topic, keys) in topics)
            {
                body.Children.Add(new TextBlock { Text = topic, Style = (Style)FindResource("CardTitle"), Margin = new Thickness(0, body.Children.Count == 0 ? 0 : 12, 0, 4) });
                AddRows(body, group, keys);
            }
            if (i == RingSections.Length - 1 || RingSections[i + 1].Group != group) AddRows(body, group, NotPlaced(group));
            PanelLook.Children.Add(NewSection(key, header, body));
        }
    }

    /// <summary>A section that folds away under its heading; the app remembers the ones left open.</summary>
    private Expander NewSection(string key, string header, UIElement content)
    {
        var section = new Expander { Header = header, Content = content, Margin = new Thickness(0, 10, 0, 0), IsExpanded = _appSettings.OpenSections.Contains(key) || Environment.GetEnvironmentVariable("GAZE_SCREENSHOT_EXPANDED") == "1" };
        void Remember(bool open, RoutedEventArgs e)
        {
            if (!ReferenceEquals(e.OriginalSource, section) || App.Headless) return; // not a fold inside it
            _appSettings.OpenSections.Remove(key);
            if (open) _appSettings.OpenSections.Add(key);
            _appSettings.Save();
        }
        section.Expanded += (_, e) => Remember(true, e);
        section.Collapsed += (_, e) => Remember(false, e);
        return section;
    }

    /// <summary>
    /// Cards that stand side by side while there is room and one under another when there is not. Side by side they share
    /// a height; stacked, each keeps its own (a grid of uniform cells would stretch the short ones to the tallest).
    /// </summary>
    private static void LayOutCards(Grid grid, bool sideBySide)
    {
        grid.ColumnDefinitions.Clear();
        grid.RowDefinitions.Clear();
        for (var i = 0; i < grid.Children.Count; i++)
        {
            if (sideBySide) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            else grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            Grid.SetColumn(grid.Children[i], sideBySide ? i : 0);
            Grid.SetRow(grid.Children[i], sideBySide ? 0 : i);
        }
    }

    /// <summary>A card with its small-capitals title over the content.</summary>
    private Border NewCard(string title, UIElement content, double marginTop = 10)
    {
        var stack = new StackPanel();
        stack.Children.Add(new TextBlock { Text = title, Style = (Style)FindResource("CardTitle") });
        stack.Children.Add(content);
        return new Border { Style = (Style)FindResource("Card"), Child = stack, Margin = new Thickness(0, marginTop, 0, 0) };
    }

    // ---- compact rows: label | editor | value | (i). The explanation lives in the tooltip, so a tab fits without scrolling.

    private const double RowLabelWidth = 172, RowValueWidth = 104;

    private static ToolTip WrappedToolTip(string text) => new() { Content = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = 380 } };

    private static void ShowToolTipsPatiently(DependencyObject element)
    {
        ToolTipService.SetInitialShowDelay(element, 250);
        ToolTipService.SetShowDuration(element, 60000);
    }

    /// <summary>One settings row. The editor goes in column 1 (or 1-2 when there is no value text); column 3 is the info mark.</summary>
    private Grid NewRow(string description)
    {
        var row = new Grid { MinHeight = 30, Margin = new Thickness(0, 1, 0, 1) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(RowLabelWidth) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(RowValueWidth) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(22) });

        var info = new TextBlock
        {
            Text = "", FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"), FontSize = 14,
            Foreground = (Brush)FindResource("Muted"), Background = Brushes.Transparent, Cursor = System.Windows.Input.Cursors.Help,
            VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right, ToolTip = WrappedToolTip(description),
        };
        ShowToolTipsPatiently(info);
        Grid.SetColumn(info, 3);
        row.Children.Add(info);
        return row;
    }

    private TextBlock AddRowLabel(Grid row, string label, string description)
    {
        var text = new TextBlock
        {
            Text = label, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 0, 8, 0), ToolTip = WrappedToolTip(label + "\n\n" + description),
        };
        ShowToolTipsPatiently(text);
        row.Children.Add(text);
        return text;
    }

    /// <summary>The value next to a slider. With <paramref name="typed"/>, a click on it turns it into a box to type the
    /// number in: Enter (or leaving the box) takes it, Esc leaves the value as it was.</summary>
    private TextBlock AddRowValue(Grid row, Action<string>? typed = null)
    {
        var value = new TextBlock
        {
            Foreground = (Brush)FindResource("Muted"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Right, TextTrimming = TextTrimming.CharacterEllipsis,
        };
        Grid.SetColumn(value, 2);
        row.Children.Add(value);
        if (typed == null) return value;

        value.Background = Brushes.Transparent;
        value.Cursor = System.Windows.Input.Cursors.IBeam;
        value.ToolTip = "Click to type a value";
        var box = new TextBox
        {
            Visibility = Visibility.Collapsed, MinWidth = 0, MinHeight = 0, Padding = new Thickness(4, 1, 4, 1), FontSize = 12, Margin = new Thickness(8, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Right,
        };
        Grid.SetColumn(box, 2);
        row.Children.Add(box);
        void Close(bool take)
        {
            if (box.Visibility != Visibility.Visible) return;
            box.Visibility = Visibility.Collapsed;
            value.Visibility = Visibility.Visible;
            if (take) typed(box.Text);
        }
        value.MouseLeftButtonUp += (_, _) =>
        {
            // The number alone goes into the box; its unit stays in mind.
            box.Text = new string(value.Text.TakeWhile(ch => char.IsDigit(ch) || ch is '.' or '-').ToArray());
            value.Visibility = Visibility.Hidden;
            box.Visibility = Visibility.Visible;
            box.Focus();
            box.SelectAll();
        };
        box.KeyDown += (_, e) =>
        {
            if (e.Key is not (System.Windows.Input.Key.Enter or System.Windows.Input.Key.Escape)) return;
            Close(take: e.Key == System.Windows.Input.Key.Enter);
            e.Handled = true;
        };
        box.LostKeyboardFocus += (_, _) => Close(take: true);
        return value;
    }

    /// <summary>
    /// What a setting's own control says about itself: its name and explanation for a screen reader, and the explanation
    /// as its tooltip - which the keyboard reaches too (a tooltip shows when its control takes the focus), while the (i)
    /// at the end of the row is for the mouse. It opens under the control and after a moment, out of the way of a drag.
    /// </summary>
    private static void Describe(FrameworkElement editor, string name, string explanation)
    {
        System.Windows.Automation.AutomationProperties.SetName(editor, name);
        System.Windows.Automation.AutomationProperties.SetHelpText(editor, explanation);
        editor.ToolTip = WrappedToolTip(explanation);
        ToolTipService.SetInitialShowDelay(editor, 900);
        ToolTipService.SetShowDuration(editor, 60000);
        ToolTipService.SetPlacement(editor, System.Windows.Controls.Primitives.PlacementMode.Bottom);
    }

    private static void AddRowEditor(Grid row, FrameworkElement editor, int column = 1, int span = 1)
    {
        editor.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(editor, column);
        Grid.SetColumnSpan(editor, span);
        row.Children.Add(editor);
    }

    private FrameworkElement BuildRow(SettingDef def)
    {
        var row = NewRow(def.Description);
        switch (def.Kind)
        {
            case SettingKind.Toggle:
            {
                var box = new CheckBox { Content = def.Label, FontWeight = FontWeights.SemiBold, ToolTip = WrappedToolTip(def.Description) };
                ShowToolTipsPatiently(box);
                box.Checked += (_, _) => OnValueChanged(def.Key, "1");
                box.Unchecked += (_, _) => OnValueChanged(def.Key, "0");
                _controlSetters[def.Key] = v => box.IsChecked = v != "0";
                AddRowEditor(row, box, column: 0, span: 3);
                return row;
            }
            case SettingKind.Choice:
            {
                AddRowLabel(row, def.Label, def.Description);
                var combo = new ComboBox { HorizontalAlignment = HorizontalAlignment.Stretch };
                Describe(combo, def.Label, def.Description);
                _choiceCombos[def.Key] = combo;
                foreach (var choice in def.Choices) combo.Items.Add(new ComboBoxItem { Content = choice.Label, Tag = choice.Value });
                combo.SelectionChanged += (_, _) =>
                {
                    if (combo.SelectedItem is ComboBoxItem { Tag: string value }) OnValueChanged(def.Key, value);
                };
                _controlSetters[def.Key] = v =>
                    combo.SelectedItem = combo.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == v) ?? combo.Items[0];
                AddRowEditor(row, combo, column: 1, span: 2);
                return row;
            }
            case SettingKind.Slider:
            {
                AddRowLabel(row, def.Label, def.Description);
                var slider = new Slider
                {
                    Minimum = def.Min, Maximum = def.Max, SmallChange = def.Step, LargeChange = def.Step * 10,
                    TickFrequency = def.Step, IsSnapToTickEnabled = true,
                };
                // A setting that runs the other way sits mirrored on its slider (the same sum both ways), so that the
                // slider fills from the left like every other one while the number shown is the real value.
                double Mirrored(double value) => def.Reversed ? def.Min + def.Max - value : value;
                var standard = Math.Clamp(Settings.ParseDouble(def.Default, def.Min), def.Min, def.Max);
                var valueText = AddRowValue(row, text => { if (def.Parse(text) is { } number) slider.Value = Mirrored(number); });
                slider.MouseDoubleClick += (_, _) => slider.Value = Mirrored(standard);
                Describe(slider, def.Label, def.Description + $"\n\nDouble-click the slider for the default ({def.Format(standard)}). Click the value to type one.");
                slider.ValueChanged += (_, e) =>
                {
                    var value = Mirrored(e.NewValue);
                    valueText.Text = def.Format(value);
                    OnValueChanged(def.Key, def.Serialize(value));
                };
                _controlSetters[def.Key] = v =>
                {
                    slider.Value = Mirrored(Math.Clamp(Settings.ParseDouble(v, standard), def.Min, def.Max));
                    valueText.Text = def.Format(Mirrored(slider.Value));
                };
                AddRowEditor(row, slider);
                return row;
            }
            default:
                return BuildColorEditor(def, row);
        }
    }

    /// <summary>Quick picks in the row itself; the full picker is folded away underneath until it is wanted.</summary>
    private FrameworkElement BuildColorEditor(SettingDef def, Grid row)
    {
        AddRowLabel(row, def.Label, def.Description);
        var picker = new ColorPicker { Margin = new Thickness(0, 6, 0, 0) };
        // The colour in use, as a swatch: a click opens the picker for any colour.
        var current = new Border { Width = 34, Height = 20, CornerRadius = new CornerRadius(4), BorderBrush = (Brush)FindResource("Line"), BorderThickness = new Thickness(1) };
        var currentButton = new Button
        {
            Style = (Style)FindResource("FlatButton"), Padding = new Thickness(2), HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center,
            Content = current,
        };
        System.Windows.Automation.AutomationProperties.SetName(currentButton, $"{def.Label}: the colour in use; opens the picker");
        Grid.SetColumn(currentButton, 2);
        row.Children.Add(currentButton);
        var expander = new Expander { Header = "Any colour", Content = picker, Margin = new Thickness(RowLabelWidth, 0, 0, 4) };
        currentButton.Click += (_, _) => expander.IsExpanded = !expander.IsExpanded;
        void ShowCurrent(byte r, byte g, byte b)
        {
            current.Background = new SolidColorBrush(Color.FromRgb(r, g, b));
            currentButton.ToolTip = $"The colour in use (red {r}, green {g}, blue {b}). Click for any colour.";
        }

        void Publish(byte r, byte g, byte b)
        {
            ShowCurrent(r, g, b);
            OnValueChanged(def.Key, $"{r},{g},{b}");
        }
        picker.ColorChanged += Publish;

        var swatches = new WrapPanel();
        var presets = new (string Name, string Value)[]
        {
            ("Sky blue", "40,170,245"), ("Cyan", "0,210,255"), ("White", "255,255,255"), ("Green", "80,255,120"),
            ("Yellow", "255,235,0"), ("Orange", "255,140,0"), ("Red", "255,70,60"), ("Magenta", "235,80,255"),
        };
        foreach (var (name, value) in presets)
        {
            var (r, g, b) = Settings.ParseColor(value);
            var swatch = new Button
            {
                Style = (Style)FindResource("FlatButton"), Padding = new Thickness(2), Margin = new Thickness(-2, 0, 1, 0), ToolTip = name,
                Content = new Border
                {
                    Width = 22, Height = 18, CornerRadius = new CornerRadius(4), Background = new SolidColorBrush(Color.FromRgb(r, g, b)),
                    BorderBrush = (Brush)FindResource("Line"), BorderThickness = new Thickness(1),
                },
            };
            System.Windows.Automation.AutomationProperties.SetName(swatch, $"{def.Label}: {name}");
            swatch.Click += (_, _) => { picker.SetColor(r, g, b); Publish(r, g, b); };
            swatches.Children.Add(swatch);
        }
        AddRowEditor(row, swatches);

        _controlSetters[def.Key] = value =>
        {
            var (r, g, b) = Settings.ParseColor(value);
            picker.SetColor(r, g, b);
            ShowCurrent(r, g, b);
        };
        var panel = new StackPanel();
        panel.Children.Add(row);
        panel.Children.Add(expander);
        return panel;
    }

    private FrameworkElement BuildPresets()
    {
        var bar = new WrapPanel { VerticalAlignment = VerticalAlignment.Center };
        bar.Children.Add(new TextBlock { Text = "Presets", Style = (Style)FindResource("Hint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 8, 0) });
        foreach (var preset in Settings.Presets)
        {
            var button = new Button { Content = preset.Name, ToolTip = WrappedToolTip(preset.Description + " Presets leave motion and placement alone."), Margin = new Thickness(0, 2, 6, 2) };
            button.Click += (_, _) => ApplyValues(preset.Values);
            bar.Children.Add(button);
        }
        var slots = BuildSlotBar(_appSettings.RingSlots, CaptureRingSlot,
            values => ApplyValues(values.Where(kv => Settings.All.Any(d => d.Key == kv.Key && IsRingSlotKey(d))).ToDictionary(kv => kv.Key, kv => kv.Value)),
            DescribeRingSlot);
        slots.Margin = new Thickness(10, 0, 0, 0);
        bar.Children.Add(slots);

        var reset = new Button { Content = "Reset all", ToolTip = "Resets the ring's settings to their defaults", Margin = new Thickness(10, 2, 0, 2) };
        reset.Click += OnResetAll;
        bar.Children.Add(reset);
        return bar;
    }

    // ---- the user's own preset slots (kept in the app's own app.json, not in any settings file a layer reads)

    /// <summary>
    /// "Mine: [1] [2] [3] [Save]". A slot button applies what was saved in it; Save opens a menu to store the current values
    /// in a slot (or clear one). The tooltip of a filled slot says what is in it.
    /// </summary>
    private FrameworkElement BuildSlotBar(SavedPreset?[] slots, Func<Dictionary<string, string>> capture,
        Action<Dictionary<string, string>> apply, Func<Dictionary<string, string>, string> describe)
    {
        var bar = new WrapPanel { VerticalAlignment = VerticalAlignment.Center };
        bar.Children.Add(new TextBlock { Text = "My presets", Style = (Style)FindResource("Hint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) });
        var buttons = new Button[slots.Length];
        var save = new Button { Content = "Save", Margin = new Thickness(2, 2, 0, 2), ToolTip = "Saves the current values in one of your slots" };

        void Refresh()
        {
            for (var i = 0; i < slots.Length; i++)
            {
                buttons[i].IsEnabled = slots[i] != null;
                buttons[i].ToolTip = slots[i] is { } filled
                    ? WrappedToolTip($"Your slot {i + 1}, saved {filled.SavedAt:g}\n\n{describe(filled.Values)}")
                    : WrappedToolTip($"Slot {i + 1} is empty. Use Save to keep the current values here.");
                ToolTipService.SetShowOnDisabled(buttons[i], true);
                ShowToolTipsPatiently(buttons[i]);
            }
        }

        for (var i = 0; i < slots.Length; i++)
        {
            var index = i;
            buttons[i] = new Button { Content = (i + 1).ToString(), MinWidth = 30, Margin = new Thickness(0, 2, 4, 2) };
            buttons[i].Click += (_, _) => { if (slots[index] is { } filled) apply(new Dictionary<string, string>(filled.Values)); };
            bar.Children.Add(buttons[i]);
        }

        save.Click += (_, _) =>
        {
            var menu = new ContextMenu { PlacementTarget = save, Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom };
            for (var i = 0; i < slots.Length; i++)
            {
                var index = i;
                var item = new MenuItem { Header = slots[i] is { } filled ? $"Save to slot {i + 1}, replacing the values from {filled.SavedAt:g}" : $"Save to slot {i + 1} (empty)" };
                item.Click += (_, _) =>
                {
                    slots[index] = new SavedPreset { SavedAt = DateTime.Now, Values = capture() };
                    _appSettings.Save();
                    Refresh();
                };
                menu.Items.Add(item);
            }
            if (slots.Any(s => s != null)) menu.Items.Add(new Separator());
            for (var i = 0; i < slots.Length; i++)
            {
                if (slots[i] == null) continue;
                var index = i;
                var item = new MenuItem { Header = $"Clear slot {i + 1}" };
                item.Click += (_, _) => { slots[index] = null; _appSettings.Save(); Refresh(); };
                menu.Items.Add(item);
            }
            menu.IsOpen = true;
        };
        bar.Children.Add(save);
        Refresh();
        return bar;
    }

    /// <summary>A ring slot holds what the built-in ring presets cover: the Look and Tail tabs, but not the on/off switch.</summary>
    private static bool IsRingSlotKey(SettingDef def) => (def.Group == Settings.GroupLook || def.Group == Settings.GroupTail) && def.Key != "enabled";

    private Dictionary<string, string> CaptureRingSlot() =>
        Settings.All.Where(IsRingSlotKey).ToDictionary(d => d.Key, d => _values.GetValueOrDefault(d.Key) ?? d.Default);

    private static string DescribeRingSlot(Dictionary<string, string> values) =>
        string.Join("\n", Settings.All.Where(d => IsRingSlotKey(d) && values.ContainsKey(d.Key)).Select(d =>
            d.Kind == SettingKind.Slider ? $"{d.Label}: {d.Format(Settings.ParseDouble(values[d.Key], 0))}"
            : d.Kind == SettingKind.Choice ? $"{d.Label}: {d.Choices.FirstOrDefault(c => c.Value == values[d.Key])?.Label ?? values[d.Key]}"
            : $"{d.Label}: {values[d.Key]}"));

    // ---- layout: the preview stands beside the ring's pages, or under them in a narrow window; a page keeps a readable width

    private const double PreviewBesideNeedsWindowWidth = 860;
    private const int PreviewBesideSize = 260, PreviewStripSize = 132;
    private bool _previewStrip;

    private void UpdatePanels()
    {
        UpdateRail();
        var selected = Tabs.SelectedItem;
        // The preview only means something next to the ring's own settings (the Gaze and Ring pages).
        var ringTab = ReferenceEquals(selected, LookTab) || ReferenceEquals(selected, GazeTab);
        var showPreview = ringTab && _appSettings.ShowPreview;
        var strip = showPreview && ActualWidth < PreviewBesideNeedsWindowWidth;
        RingPanel.Visibility = showPreview ? Visibility.Visible : Visibility.Collapsed;
        PreviewShow.Visibility = ringTab && !_appSettings.ShowPreview ? Visibility.Visible : Visibility.Collapsed;
        Grid.SetColumn(RingPanel, strip ? 0 : 1);
        Grid.SetColumnSpan(RingPanel, strip ? 2 : 1);
        Grid.SetRow(RingPanel, strip ? 2 : 0);
        Grid.SetRowSpan(RingPanel, strip ? 1 : 2);
        RingPanel.Margin = strip ? new Thickness(20, 8, 20, 12) : new Thickness(0, 16, 20, 0);
        // In the strip the picture is small and its choices stand beside it.
        PreviewFrame.Width = PreviewFrame.Height = strip ? PreviewStripSize : PreviewBesideSize;
        Grid.SetRow(PreviewOptions, strip ? 1 : 2);
        Grid.SetColumn(PreviewOptions, strip ? 1 : 0);
        PreviewOptions.Margin = strip ? new Thickness(16, 0, 0, 0) : new Thickness(0, 6, 0, 0);
        PreviewOptions.VerticalAlignment = strip ? VerticalAlignment.Center : VerticalAlignment.Top;
        PreviewOptions.HorizontalAlignment = strip ? HorizontalAlignment.Left : HorizontalAlignment.Stretch;
        PreviewOptions.MinWidth = strip ? 200 : 0;
        if (strip != _previewStrip)
        {
            _previewStrip = strip;
            if (IsLoaded) RenderPreview();
        }
        // Set the column too: a Grid does not always re-measure an Auto column when its only child collapses during a layout pass.
        PreviewColumn.Width = showPreview && !strip ? GridLength.Auto : new GridLength(0);
        // A page of settings keeps a readable width in a wide window; the Mirror page gives all the room to its picture.
        TabsColumn.MaxWidth = ReferenceEquals(selected, CropTab) ? double.PositiveInfinity : ringTab ? 740 : 1040;
        (Tabs.Parent as UIElement)?.InvalidateMeasure();
    }

    private void OnPreviewHide(object sender, RoutedEventArgs e) => ShowPreview(false);

    private void OnPreviewShow(object sender, RoutedEventArgs e) => ShowPreview(true);

    private void ShowPreview(bool show)
    {
        _appSettings.ShowPreview = show;
        _appSettings.Save();
        UpdatePanels();
    }

    /// <summary>The mark at the foot of the rail: a few words on the last save, the whole sentence as its tooltip; orange when a save failed.</summary>
    private void SetSaveMark(string brief, string full, bool problem = false)
    {
        SaveMark.Visibility = Visibility.Visible;
        SaveMarkText.Text = brief;
        SaveMarkIcon.Text = problem ? "\uE7BA" : "\uE73E";
        SaveMarkIcon.Foreground = SaveMarkText.Foreground = problem ? Brushes.Orange : (Brush)FindResource("Muted");
        SaveMark.ToolTip = WrappedToolTip(full);
    }

    private void RestoreWindowPlacement()
    {
        if (App.Headless || _appSettings.WindowWidth < MinWidth || _appSettings.WindowHeight < MinHeight) return;
        // Only if it still lands on a screen (a monitor may have gone since).
        var screen = new Rect(SystemParameters.VirtualScreenLeft, SystemParameters.VirtualScreenTop, SystemParameters.VirtualScreenWidth, SystemParameters.VirtualScreenHeight);
        var wanted = new Rect(_appSettings.WindowLeft, _appSettings.WindowTop, _appSettings.WindowWidth, _appSettings.WindowHeight);
        var visible = Rect.Intersect(screen, wanted);
        if (visible.IsEmpty || visible.Width < 200 || visible.Height < 120) return;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = wanted.Left; Top = wanted.Top; Width = wanted.Width; Height = wanted.Height;
    }

    private void SaveWindowPlacement()
    {
        if (App.Headless || WindowState != WindowState.Normal) return;
        _appSettings.WindowLeft = Left; _appSettings.WindowTop = Top; _appSettings.WindowWidth = Width; _appSettings.WindowHeight = Height;
        _appSettings.Save();
    }

    private void ApplyValues(IReadOnlyDictionary<string, string> changes)
    {
        _loading = true;
        foreach (var (key, value) in changes)
        {
            _values[key] = value;
            _dirtyKeys.Add(key);
            if (_controlSetters.TryGetValue(key, out var setter)) setter(value);
        }
        _loading = false;
        _previewTimer.Stop(); _previewTimer.Start();
        _saveTimer.Stop(); _saveTimer.Start();
    }

    private void OnValueChanged(string key, string value)
    {
        if (_loading) return;
        _values[key] = value;
        _dirtyKeys.Add(key);
        if (key == "headset_marker") RefreshStatus();
        if (key == "gaze_source") RefreshMirrorLive(); // The Gaze card shows or hides its VRChat part.
        if (key == "projection") UpdateDependentRows();
        OnProfileValueChanged(key, value);
        if (key.StartsWith("crop_follow", StringComparison.Ordinal)) DrawCrop();
        if (key.StartsWith("stabilize", StringComparison.Ordinal)) ApplyCropMargin(moveBox: true);
        if (!_previewTimer.IsEnabled) _previewTimer.Start();
        _saveTimer.Stop(); _saveTimer.Start();
    }

    // ------------------------------------------------------------------ config file

    private void LoadConfig()
    {
        var existed = File.Exists(_configPath);
        _loading = true;
        _values = ConfigFile.Load(_configPath);
        foreach (var (key, setter) in _controlSetters) setter(_values.GetValueOrDefault(key) ?? "");
        _loading = false;
        _configLoaded = true;
        _dirtyKeys.Clear();
        UpdateDependentRows();
        SyncFixedFocusKeys();
        AppLog.Write(existed ? $"Ring settings loaded from {_configPath} ({_values.Count} keys)." : $"No ring settings file yet ({_configPath}); defaults shown.");
        if (existed) SetSaveMark("Saves by itself", "Loaded your current settings. Changes save by themselves and reach a running game at once.");
        else SetSaveMark("Defaults", "No settings file yet, so these are the defaults. The app creates the file at your first change.");
        RenderPreview();
    }

    /// <summary>
    /// The layer writes to this file too (Ctrl+Alt+arrow nudges in game), and so can a text editor. Anything changed on
    /// disk that the user has not just changed here wins, so this app never writes stale values back over it.
    /// </summary>
    private bool MergeFromDisk()
    {
        if (!File.Exists(_configPath)) return false;
        var changed = false;
        _loading = true;
        foreach (var (key, value) in ConfigFile.Load(_configPath))
        {
            if (_dirtyKeys.Contains(key) || _values.GetValueOrDefault(key) == value) continue;
            _values[key] = value;
            if (_controlSetters.TryGetValue(key, out var setter)) setter(value);
            changed = true;
        }
        _loading = false;
        return changed;
    }

    private void SaveNow()
    {
        try
        {
            MergeFromDisk();
            _ownWriteUntil = DateTime.UtcNow.AddMilliseconds(600);
            ConfigFile.Save(_configPath, _values);
            _dirtyKeys.Clear();
            // The game never polls this file; telling it is what makes the change go live.
            SetSaveMark($"Saved {DateTime.Now:t}", LiveLink.NotifySettingsChanged()
                ? $"Saved at {DateTime.Now:T} and applied to the running game."
                : $"Saved at {DateTime.Now:T}. No game is running; the next one reads the settings at its start.");
            RenderPreview();
        }
        catch (Exception e)
        {
            SetSaveMark("Not saved", "Could not save: " + e.Message, problem: true);
            AppLog.Write("Could not save the ring settings", e);
        }
    }

    /// <summary>
    /// Windows tells us when the settings file changes (no polling): that is how in-game calibration nudges show up here
    /// live, and how edits made in a text editor reach a running game while this app is open.
    /// </summary>
    private void StartWatchingConfigFile()
    {
        try
        {
            var folder = System.IO.Path.GetDirectoryName(_configPath)!;
            Directory.CreateDirectory(folder);
            _configWatcher = new FileSystemWatcher(folder, System.IO.Path.GetFileName(_configPath))
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
                EnableRaisingEvents = true,
            };
            FileSystemEventHandler changed = (_, _) => Dispatcher.BeginInvoke(OnConfigFileChangedOutside);
            _configWatcher.Changed += changed;
            _configWatcher.Created += changed;
            _configWatcher.Renamed += (_, _) => Dispatcher.BeginInvoke(OnConfigFileChangedOutside);
        }
        catch
        {
            // Without the watcher the app still works; outside changes then only show after "Reload from file".
        }
    }

    private void OnConfigFileChangedOutside()
    {
        // Our own save triggers the watcher too; so does a save that is about to happen.
        if (DateTime.UtcNow < _ownWriteUntil || _saveTimer.IsEnabled) return;
        _outsideChangeTimer.Stop();
        _outsideChangeTimer.Start(); // Editors write in several steps: wait for the file to settle.
    }

    private void ApplyOutsideChange()
    {
        try
        {
            if (!MergeFromDisk()) return;
            // A text editor changed it (the layer already holds the values it wrote itself): make a running game reload.
            LiveLink.NotifySettingsChanged();
            SetSaveMark($"Read {DateTime.Now:t}", $"Picked up a change made outside this app at {DateTime.Now:T}.");
            AppLog.Write("Picked up a change to the ring settings made outside the app.");
            RefreshMirrorLive(); // The helper's calibration result arrives this way.
            if (_values.ContainsKey("headset_marker")) RefreshStatus();
            RenderPreview();
        }
        catch
        {
            // The file may still be mid-write; the next change notification tries again.
        }
    }

    private void OnResetAll(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "Reset the settings to their defaults?\n\nThe crop box and the VRChat gaze calibration stay as they are.", Title, MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
        {
            // Ring settings only: the crop box belongs to the OBS scene, not to the look of the ring.
            var cropKeys = Settings.All.Where(d => d.Group == Settings.GroupCrop).Select(d => d.Key).ToHashSet();
            ApplyValues(Settings.Defaults().Where(kv => !cropKeys.Contains(kv.Key)).ToDictionary(kv => kv.Key, kv => kv.Value));
        }
    }

    private void OnOpenConfig(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(_configPath)) SaveNow();
        OpenFileOrShow(_configPath);
    }

    private void OnReloadConfig(object sender, RoutedEventArgs e) => LoadConfig();

    // ------------------------------------------------------------------ preview

    private void OnPreviewOptionChanged(object sender, RoutedEventArgs e)
    {
        if (IsLoaded) RenderPreview();
    }

    private void RenderPreview()
    {
        var background = BgTerrain.IsChecked == true ? PreviewBackground.Terrain
                       : BgSky.IsChecked == true ? PreviewBackground.Sky
                       : BgBlack.IsChecked == true ? PreviewBackground.Black
                       : PreviewBackground.Cockpit;
        PreviewImage.Source = RingPreview.Render(_values, background, PreviewMoving.IsChecked == true, _previewStrip ? PreviewStripSize : PreviewBesideSize);
    }

    // ------------------------------------------------------------------ status tab (read-only)

    private void RefreshStatus()
    {
        SystemStatus status;
        try
        {
            status = LayerStatus.Get();
        }
        catch (Exception e)
        {
            StatusRows.Children.Clear();
            _statusRowsInto = StatusRows;
            AddStatusRow("Could not read the OpenXR layer setup", e.Message, Colors.Firebrick);
            AppLog.Write("Could not read the OpenXR layer setup", e);
            return;
        }

        // GAZE_SCREENSHOT_FAULTS=1 (a screenshot run only): the page as it looks when things are wrong.
        if (App.Headless && Environment.GetEnvironmentVariable("GAZE_SCREENSHOT_FAULTS") == "1")
        {
            status = status with
            {
                GazeMirror = new LayerReport(LayerHealth.NotActive, "Not installed. Run the VR Gaze Mirror installer (.msi), or its Repair option."),
                ObsPath = status.ObsPath ?? @"C:\Program Files\obs-studio", ObsPluginPresent = false, OrderCorrect = false, Layers = [.. status.Layers.Reverse()],
            };
            SetSaveMark("Not saved", "Could not save: the settings file is in use by another program.", problem: true);
        }

        StatusRows.Children.Clear();
        ShowQuadViewsPage(status.QuadViewsInstalled);
        _obsPluginMissing = status.ObsPath != null && !status.ObsPluginPresent;
        UpdateStartSteps();
        AppLog.Write($"Status: gaze mirror layer {status.GazeMirror.Health} ({status.GazeMirror.Detail}); OBS {(status.ObsPath == null ? "not found" : status.ObsPluginPresent ? "plugin present" : "plugin missing")}; " +
                     $"Quad-Views-Foveated {(status.QuadViewsInstalled ? status.QuadViews.Health.ToString() : "not installed")}; layer order {(status.OrderCorrect switch { true => "ok", false => "WRONG", null => "n/a" })}; {status.Layers.Count} layers.");
        var helper = HelperState();
        var layerActive = status.GazeMirror.Health == LayerHealth.Active;
        var obsReady = status.ObsPath != null && status.ObsPluginPresent;
        var marker = _values.GetValueOrDefault("headset_marker") == "1";
        // While all is well, one line says so and the rows wait behind "Details"; with anything amiss they all show.
        var allWell = layerActive && obsReady && helper.Color == Colors.SeaGreen && status.QuadViews.Health != LayerHealth.Duplicate && !marker && status.OrderCorrect != false;
        _statusRowsInto = StatusRows;
        if (allWell)
        {
            var details = new StackPanel { Margin = new Thickness(0, 4, 0, 0), Visibility = _statusDetailsOpen ? Visibility.Visible : Visibility.Collapsed };
            AddStatusSummary(details);
            StatusRows.Children.Add(details);
            _statusRowsInto = details;
        }
        // An active layer needs no more than the word; where it is installed is its tooltip.
        AddStatusRow("Mirror layer", layerActive ? "Active." : status.GazeMirror.Detail, layerActive ? Colors.SeaGreen : Colors.Firebrick, tip: layerActive ? status.GazeMirror.Detail : null);
        AddStatusRow("OBS plugin",
            status.ObsPath == null ? "The app found no OBS Studio on this PC."
            : status.ObsPluginPresent ? "In OBS: add a \"Gaze Mirror\" source, then press Ctrl+F once to fit it."
            : $"Not in OBS yet ({status.ObsPath}). Run the installer again now that OBS is there, or use its Repair option.",
            obsReady ? Colors.SeaGreen : status.ObsPath == null ? Colors.Gray : Colors.Firebrick);
        AddStatusRow("SteamVR helper", helper.Text + " It mirrors games that use SteamVR directly; the layer mirrors OpenXR games.", helper.Color);
        AddStatusRow("Quad-Views-Foveated (optional)", status.QuadViews.Detail,
            status.QuadViews.Health == LayerHealth.NotActive && !status.QuadViewsInstalled ? Colors.Gray : HealthColor(status.QuadViews.Health),
            !status.QuadViewsInstalled && MirrorLive.FindQuadViewsInstaller() != null ? ("Install", OnQuadViewsInstall) : null);
        if (marker)
        {
            AddStatusRow("Calibration is ON",
                "Your headset shows a marker, and Ctrl+Alt+arrow keys nudge the ring in game. " +
                "Switch it off on the Gaze page when you are done.", Colors.DarkOrange);
        }
        if (status.OrderCorrect == false)
        {
            AddStatusRow("Layer order",
                "The mirror layer sits above Quad-Views-Foveated, so it sees the wrong picture. Press \"Fix order\" in the layer list below.", Colors.Firebrick);
        }

        BuildLayerRows(status.Layers);
    }

    // ---- the small layers tool: what is registered, in load order; on/off, move up/down, and an order check

    /// <summary>An order the user has arranged but not applied yet (per list), so several moves cost one permission prompt.</summary>
    private List<LayerEntry>? _pendingMachineOrder, _pendingUserOrder;

    private void BuildLayerRows(IReadOnlyList<LayerEntry> machineLayers)
    {
        LayerRows.Children.Clear();
        List<LayerEntry> userLayers;
        try { userLayers = LayerStatus.ReadLayers(perUser: true); } catch { userLayers = []; }

        // A pending order only survives while it still describes the same entries.
        static List<LayerEntry>? StillValid(List<LayerEntry>? pending, IReadOnlyList<LayerEntry> actual) =>
            pending != null && pending.Select(l => l.JsonPath).Order().SequenceEqual(actual.Select(l => l.JsonPath).Order())
                ? [.. pending.Select(p => actual.First(a => a.JsonPath == p.JsonPath))] : null;
        _pendingMachineOrder = StillValid(_pendingMachineOrder, machineLayers);
        _pendingUserOrder = StillValid(_pendingUserOrder, userLayers);
        var machine = _pendingMachineOrder ?? [.. machineLayers];
        var user = _pendingUserOrder ?? userLayers;
        var pending = _pendingMachineOrder != null || _pendingUserOrder != null;

        if (machine.Count == 0 && user.Count == 0)
        {
            LayerRows.Children.Add(new TextBlock { Text = "Windows lists no OpenXR API layers.", Style = (Style)FindResource("Hint"), Margin = new Thickness(0, 6, 0, 6) });
        }

        var position = 1;
        foreach (var group in new[] { machine, user })
        {
            for (var i = 0; i < group.Count; i++)
            {
                LayerRows.Children.Add(BuildLayerRow(group, i, position++, pending));
            }
        }

        // ---- order check, on the order as shown (so it clears up while arranging)
        var shown = machine.Concat(user).ToList();
        var problems = LayerOrder.Problems(shown);
        LayerProblems.Text = problems.Count == 0 ? "" : "Order problem" + (problems.Count > 1 ? "s" : "") + ":\n  - " + string.Join("\n  - ", problems);
        LayerProblems.Visibility = problems.Count == 0 ? Visibility.Collapsed : Visibility.Visible;

        var suggestion = problems.Count > 0 ? LayerOrder.Suggest(machine) : null;
        var fixable = suggestion != null && LayerOrder.Problems([.. suggestion, .. user]).Count == 0;
        LayerFix.Visibility = problems.Count > 0 && !pending ? Visibility.Visible : Visibility.Collapsed;
        LayerFix.IsEnabled = fixable;
        LayerFix.ToolTip = fixable ? "Arranges the PC-wide layers to satisfy the known rules, moving as few as possible"
            : "No automatic fix: a rule involves a layer registered for this user alone, or two rules contradict each other. Arrange them by hand.";
        LayerApply.Visibility = LayerCancel.Visibility = pending ? Visibility.Visible : Visibility.Collapsed;

        // The list is folded away while there is nothing to do in it; a problem or an order not yet applied opens it.
        LayerExpander.Header = "OpenXR API layers: " + (shown.Count == 0 ? "none"
            : shown.Count + (pending ? ", new order not applied yet" : problems.Count > 0 ? ", order problem" : shown.Count > 1 ? ", order fine" : ""));
        if (problems.Count > 0 || pending) LayerExpander.IsExpanded = true;

        if (pending) LayerNote.Text = "Order changed, not applied yet. Apply asks Windows for permission once.";
        else if (LayerNote.Text.StartsWith("Order changed", StringComparison.Ordinal) || LayerNote.Text.Length == 0)
        {
            LayerNote.Text = problems.Count == 0 && shown.Count > 1 ? "The order satisfies the rules this app knows." : "";
        }
    }

    private FrameworkElement BuildLayerRow(List<LayerEntry> group, int index, int position, bool pending)
    {
        var layer = group[index];
        var ours = layer.LayerName is LayerStatus.QuadViewsLayer or LayerStatus.GazeMirrorLayer;
        var box = new CheckBox
        {
            IsChecked = layer.Enabled, VerticalAlignment = VerticalAlignment.Top, MinWidth = 0, Margin = new Thickness(0, 0, 4, 0),
            IsEnabled = !pending, ToolTip = pending ? "Apply or cancel the new order first" : "Switch this layer on or off",
        };
        ToolTipService.SetShowOnDisabled(box, true);

        var title = new TextBlock { TextWrapping = TextWrapping.Wrap, FontWeight = FontWeights.SemiBold, Opacity = layer.Enabled ? 1 : 0.55 };
        title.Inlines.Add($"{position}. {layer.LayerName ?? "(manifest missing or unreadable)"}");
        if (layer.PerUser) title.Inlines.Add(new System.Windows.Documents.Run("   this user only") { FontWeight = FontWeights.Normal, Foreground = (Brush)FindResource("Muted") });
        if (!layer.Enabled) title.Inlines.Add(new System.Windows.Documents.Run("   off") { FontWeight = FontWeights.Normal, Foreground = new SolidColorBrush(Colors.Orange) });
        var text = new StackPanel { Margin = new Thickness(0, 5, 0, 0) };
        text.Children.Add(title);
        text.Children.Add(new TextBlock { Text = layer.JsonPath, Style = (Style)FindResource("Hint"), FontSize = 12 });

        Button Arrow(string glyph, string tip, int offset)
        {
            var target = index + offset;
            var button = new Button
            {
                Content = new TextBlock { Text = glyph, FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"), FontSize = 11 },
                Padding = new Thickness(7, 4, 7, 4), Margin = new Thickness(4, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center,
                IsEnabled = target >= 0 && target < group.Count, ToolTip = tip,
            };
            System.Windows.Automation.AutomationProperties.SetName(button, $"{tip} ({layer.LayerName})");
            button.Click += (_, _) =>
            {
                var order = group.ToList();
                (order[index], order[target]) = (order[target], order[index]);
                if (layer.PerUser) _pendingUserOrder = order; else _pendingMachineOrder = order;
                RefreshStatus();
            };
            return button;
        }

        var arrows = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        arrows.Children.Add(Arrow("\uE70E", "Move up: closer to the game", -1));
        arrows.Children.Add(Arrow("\uE70D", "Move down: closer to the headset runtime", +1));

        var row = new DockPanel { Margin = new Thickness(0, 3, 0, 3) };
        DockPanel.SetDock(box, Dock.Left);
        DockPanel.SetDock(arrows, Dock.Right);
        row.Children.Add(box);
        row.Children.Add(arrows);
        row.Children.Add(text);

        box.Click += async (_, _) =>
        {
            var enable = box.IsChecked == true;
            box.IsEnabled = false;
            try
            {
                var changed = await LayerStatus.SetEnabledAsync(layer, enable);
                AppLog.Write($"Layer {LayerOrder.Short(layer)} {(enable ? "on" : "off")}: {(changed ? "done" : "declined")}.");
                LayerNote.Text = !changed ? "Nothing changed: you declined the Windows permission prompt."
                    : $"{LayerOrder.Short(layer)} is now {(enable ? "on" : "off")}. Games pick that up at their next start." +
                      (ours && !enable ? " The gaze ring needs both of its layers on." : "");
            }
            catch (Exception error)
            {
                LayerNote.Text = "Could not change the layer: " + error.Message;
                AppLog.Write("Could not change a layer", error);
            }
            RefreshStatus(); // Rebuilds the rows from what the registry really says now.
        };
        return row;
    }

    private async void OnLayerApplyOrder(object sender, RoutedEventArgs e)
    {
        LayerApply.IsEnabled = LayerCancel.IsEnabled = false;
        try
        {
            var done = true;
            if (_pendingUserOrder != null) done &= await LayerOrder.ApplyAsync(perUser: true, _pendingUserOrder);
            if (done && _pendingMachineOrder != null) done &= await LayerOrder.ApplyAsync(perUser: false, _pendingMachineOrder);
            if (done)
            {
                _pendingMachineOrder = _pendingUserOrder = null;
                LayerNote.Text = "New order applied. Games use it from their next start. The app saved the previous order as a .reg file next to its preferences.";
                AppLog.Write("Layer order applied.");
            }
            else
            {
                LayerNote.Text = "Nothing changed: you declined the Windows permission prompt. The new order is still waiting.";
            }
        }
        catch (Exception error)
        {
            _pendingMachineOrder = _pendingUserOrder = null;
            LayerNote.Text = "Could not change the order: " + error.Message;
            AppLog.Write("Could not change the layer order", error);
        }
        LayerApply.IsEnabled = LayerCancel.IsEnabled = true;
        var note = LayerNote.Text;
        RefreshStatus();
        LayerNote.Text = note;
    }

    private void OnLayerCancelOrder(object sender, RoutedEventArgs e)
    {
        _pendingMachineOrder = _pendingUserOrder = null;
        LayerNote.Text = "";
        RefreshStatus();
    }

    private void OnLayerFixOrder(object sender, RoutedEventArgs e)
    {
        if (LayerOrder.Suggest(LayerStatus.ReadLayers()) is { } suggestion) _pendingMachineOrder = suggestion;
        RefreshStatus();
    }

    private static Color HealthColor(LayerHealth health) => health switch
    {
        LayerHealth.Active => Colors.SeaGreen,
        LayerHealth.Duplicate => Colors.Firebrick,
        _ => Colors.Gray,
    };

    /// <summary>One line: health dot, name, what is known about it, and a button when there is something to do.</summary>
    private Panel _statusRowsInto = null!; // The card itself, or the part of it behind "Details".
    private bool _statusDetailsOpen;

    /// <summary>All is well: one line that says so, and a button that shows the parts one by one.</summary>
    private void AddStatusSummary(StackPanel details)
    {
        var row = new Grid { Margin = new Thickness(0, 2, 0, 4) };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = (Brush)FindResource("Good"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 0, 0) });
        var name = new TextBlock { Text = "All parts working", FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0) };
        Grid.SetColumn(name, 1);
        row.Children.Add(name);
        var text = new TextBlock { Text = "The mirror layer, the OBS plugin and the SteamVR helper are in place.", Style = (Style)FindResource("Hint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(text, 2);
        row.Children.Add(text);
        var toggle = new Button { Content = _statusDetailsOpen ? "Hide details" : "Details", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
        toggle.Click += (_, _) =>
        {
            _statusDetailsOpen = !_statusDetailsOpen;
            details.Visibility = _statusDetailsOpen ? Visibility.Visible : Visibility.Collapsed;
            toggle.Content = _statusDetailsOpen ? "Hide details" : "Details";
        };
        Grid.SetColumn(toggle, 3);
        row.Children.Add(toggle);
        StatusRows.Children.Add(row);
    }

    private void AddStatusRow(string title, string detail, Color color, (string Label, RoutedEventHandler Click)? button = null, string? tip = null)
    {
        var row = new Grid { Margin = new Thickness(0, 6, 0, 6) };
        if (tip != null) row.ToolTip = WrappedToolTip(tip);
        row.Background = Brushes.Transparent;
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto, MaxWidth = 230 });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = new SolidColorBrush(color), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 0, 0) });
        var name = new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0), TextTrimming = TextTrimming.CharacterEllipsis };
        Grid.SetColumn(name, 1);
        row.Children.Add(name);
        var text = new TextBlock { Text = detail, Style = (Style)FindResource("Hint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        Grid.SetColumn(text, 2);
        row.Children.Add(text);
        if (button is { } action)
        {
            var control = new Button { Content = action.Label, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
            control.Click += action.Click;
            Grid.SetColumn(control, 3);
            row.Children.Add(control);
        }
        var into = _statusRowsInto ?? StatusRows;
        if (into.Children.Count > 0 || !ReferenceEquals(into, StatusRows))
        {
            into.Children.Add(new Border { Height = 1, Background = (Brush)FindResource("Line") });
        }
        into.Children.Add(row);
    }

    // ------------------------------------------------------------------ about

    private void BuildAboutRows()
    {
        var rows = new (string What, string Where)[]
        {
            ("OpenXR games", "the mirror layer takes the picture and the gaze from the game itself"),
            ("SteamVR games", "the helper takes the picture and the gaze from SteamVR"),
            ("OBS", "the \"Gaze Mirror\" source shows whichever is live; the mirror window shows the same picture"),
        };
        foreach (var (what, where) in rows)
        {
            if (AboutRows.Children.Count > 0) AboutRows.Children.Add(new Border { Height = 1, Background = (Brush)FindResource("Line") });
            var row = new Grid { Margin = new Thickness(0, 6, 0, 6) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(18) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(130) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = (Brush)FindResource("Accent"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 1, 0, 0) });
            var name = new TextBlock { Text = what, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(name, 1);
            row.Children.Add(name);
            var text = new TextBlock { Text = where, Style = (Style)FindResource("Hint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
            Grid.SetColumn(text, 2);
            row.Children.Add(text);
            AboutRows.Children.Add(row);
        }
        LayOutCards(AboutCards, sideBySide: true);
        AboutCards.SizeChanged += (_, _) => LayOutCards(AboutCards, AboutCards.ActualWidth >= 640);
        BuildChangelog();
    }

    /// <summary>The changelog shipped inside the app (CHANGELOG.md at the repository root): version headings and their points.</summary>
    private void BuildChangelog()
    {
        string text;
        try
        {
            using var stream = typeof(MainWindow).Assembly.GetManifestResourceStream("CHANGELOG.md");
            if (stream == null) return;
            using var reader = new StreamReader(stream);
            text = reader.ReadToEnd();
        }
        catch { return; }

        // The newest version's points are shown; each older version is a line that unfolds at a click.
        var first = true;
        Panel points = ChangelogRows;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd();
            if (line.StartsWith("## ", StringComparison.Ordinal))
            {
                if (first)
                {
                    ChangelogRows.Children.Add(new TextBlock { Text = line[3..], FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 3) });
                    first = false;
                    continue;
                }
                var folded = new StackPanel { Visibility = Visibility.Collapsed, Margin = new Thickness(0, 2, 0, 4) };
                var arrow = new TextBlock { Text = "\uE76C", Style = (Style)FindResource("Icon"), FontSize = 9, Foreground = (Brush)FindResource("Muted"), Width = 14 };
                var label = new StackPanel { Orientation = Orientation.Horizontal };
                label.Children.Add(arrow);
                label.Children.Add(new TextBlock { Text = line[3..], FontWeight = FontWeights.SemiBold });
                var header = new Button { Style = (Style)FindResource("FlatButton"), Content = label, Margin = new Thickness(-2, 6, 0, -1) };
                System.Windows.Automation.AutomationProperties.SetName(header, "Version " + line[3..] + ": show or fold its changes");
                header.Click += (_, _) =>
                {
                    var open = folded.Visibility != Visibility.Visible;
                    folded.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
                    arrow.Text = open ? "\uE70D" : "\uE76C";
                };
                ChangelogRows.Children.Add(header);
                ChangelogRows.Children.Add(folded);
                points = folded;
            }
            else if (line.Length > 4 && line.StartsWith("**", StringComparison.Ordinal) && line.EndsWith("**", StringComparison.Ordinal) && line.IndexOf("**", 2, StringComparison.Ordinal) == line.Length - 2)
            {
                // A group inside a version (**New**, **Changed**, **Fixed**).
                points.Children.Add(new TextBlock { Text = line[2..^2], FontSize = 12, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 2) });
            }
            else if (line.StartsWith("- ", StringComparison.Ordinal))
            {
                var row = new Grid { Margin = new Thickness(0, 1, 0, 1) };
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(14) });
                row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                row.Children.Add(new Ellipse { Width = 5, Height = 5, Fill = (Brush)FindResource("Muted"), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 7, 0, 0) });
                var point = new TextBlock { Style = (Style)FindResource("Hint"), FontSize = 12, TextWrapping = TextWrapping.Wrap };
                // The changelog marks a name with ** on both sides: every second piece is such a name, shown in semi-bold.
                var pieces = line[2..].Split("**");
                for (var i = 0; i < pieces.Length; i++)
                {
                    if (pieces[i].Length == 0) continue;
                    point.Inlines.Add(new System.Windows.Documents.Run(pieces[i]) { FontWeight = i % 2 == 1 && pieces.Length % 2 == 1 ? FontWeights.SemiBold : FontWeights.Normal });
                }
                Grid.SetColumn(point, 1);
                row.Children.Add(point);
                points.Children.Add(row);
            }
        }
    }

    private void OnOpenRepo(object sender, RoutedEventArgs e)
    {
        if (UpdateChecker.IsConfigured) OpenInBrowser($"https://github.com/{UpdateChecker.Repository}");
    }

    /// <summary>
    /// Opens a file in the program Windows has for its type - the user's own choice for .cfg or .log, set in Windows' "Open
    /// with". A type without one opens a File Explorer window at the file's folder with the file selected.
    /// </summary>
    internal static void OpenFileOrShow(string path)
    {
        try
        {
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            AppLog.Write($"Opened {path} in the program Windows has for {System.IO.Path.GetExtension(path)} files.");
            return;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // No program for this type of file: the folder, with the file selected.
        }
        try
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
            AppLog.Write($"No program for {System.IO.Path.GetExtension(path)} files: showed {path} in its folder.");
        }
        catch
        {
            // Nothing to show it with.
        }
    }

    private static void OpenFolder(string path)
    {
        if (!Directory.Exists(path)) return;
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true }); } catch { /* nothing to show it with */ }
    }

    /// <summary>The licence texts sit next to the app under Program Files; in a development build, in the repository.</summary>
    private void OnOpenLicences(object sender, RoutedEventArgs e)
    {
        var installed = System.IO.Path.Combine(AppContext.BaseDirectory, "Licences");
        OpenFolder(Directory.Exists(installed) ? installed : AppContext.BaseDirectory);
    }

    private void OnOpenLogs(object sender, RoutedEventArgs e) => OpenFolder(AppSettings.Folder);

    /// <summary>Starts the official Quad-Views-Foveated installer shipped with this app (Windows Installer asks for permission itself).</summary>
    private void OnQuadViewsInstall(object sender, RoutedEventArgs e)
    {
        if (!MirrorLive.RunQuadViewsInstaller()) LiveText.Text = "The Quad-Views-Foveated installer did not start.";
    }

    private void UpdateLive()
    {
        var (state, text) = GazeLive.Read();
        LiveText.Text = text;
        LiveDot.Fill = new SolidColorBrush(state switch
        {
            GazeLiveState.Tracking => Colors.SeaGreen,
            GazeLiveState.EyesNotTracked or GazeLiveState.Stale => Colors.DarkOrange,
            _ => Colors.Gray,
        });
        // The rail's dot: a game is being mirrored, whatever page is open.
        _liveDot = state == GazeLiveState.NoGame ? null : (Brush)FindResource("Good");
        if (_statusNav != null) _statusNav.Dot = _liveDot;
        UpdateStartSteps();
        StatusReadyHint.Visibility = state == GazeLiveState.NoGame && StartCard.Visibility != Visibility.Visible ? Visibility.Visible : Visibility.Collapsed;
        if (ReferenceEquals(Tabs.SelectedItem, GazeTab)) RefreshGazeNow();
    }

    // ---- the first steps: what a new user does, each ticked off by what the app sees; gone once all three are done

    private bool _obsPluginMissing;
    private string _startStepsShown = "";

    private void OnStartHide(object sender, RoutedEventArgs e)
    {
        _appSettings.SetupDismissed = true;
        _appSettings.Save();
        UpdateStartSteps();
    }

    private void UpdateStartSteps()
    {
        var live = MirrorLive.Read();
        var producing = live is { Producing: true };
        var reading = producing && live!.Width > 0; // a reader has asked for the picture: the OBS source or the mirror window
        var seenBefore = (_appSettings.SetupSawGame, _appSettings.SetupSawReader);
        _appSettings.SetupSawGame |= producing;
        _appSettings.SetupSawReader |= reading;
        if (seenBefore != (_appSettings.SetupSawGame, _appSettings.SetupSawReader) && !App.Headless) _appSettings.Save();
        var framed = File.Exists(LastPicturePath);

        var done = _appSettings.SetupSawGame && _appSettings.SetupSawReader && framed;
        StartCard.Visibility = done || _appSettings.SetupDismissed ? Visibility.Collapsed : Visibility.Visible;
        if (StartCard.Visibility != Visibility.Visible) return;
        // Built anew only when something about the steps has changed (this runs once a second while the page shows).
        var shown = $"{_appSettings.SetupSawReader} {reading} {_obsPluginMissing} {_appSettings.SetupSawGame} {(producing ? live!.Program : "")} {framed}";
        if (shown == _startStepsShown) return;
        _startStepsShown = shown;
        StartSteps.Children.Clear();
        StartSteps.Children.Add(StartStep(1, "Add the source in OBS",
            _obsPluginMissing ? "The OBS plugin is missing. Run the installer again and choose Repair."
            : reading ? "A reader shows the picture now."
            : "Sources > + > Gaze Mirror, then press Ctrl+F once. Without OBS, open the mirror window on the Mirror page.",
            _appSettings.SetupSawReader));
        StartSteps.Children.Add(StartStep(2, "Start your VR game",
            producing ? $"Mirroring {live!.Program} now." : "The mirror starts with it. OpenXR and SteamVR games both work.", _appSettings.SetupSawGame));
        StartSteps.Children.Add(StartStep(3, "Frame the picture", "On the Mirror page, take a picture from the headset and set the crop box on it.", framed,
            ("Open Mirror", (_, _) => Tabs.SelectedItem = CropTab)));
        LayOutCards(StartSteps, StartCard.ActualWidth is 0 or >= 640);
    }

    /// <summary>One step: its number in a circle (a tick on green once done), what to do, and a button where there is somewhere to go.</summary>
    private FrameworkElement StartStep(int number, string title, string detail, bool done, (string Label, RoutedEventHandler Click)? button = null)
    {
        var good = (Brush)FindResource("Good");
        var muted = (Brush)FindResource("Muted");
        var mark = new Border
        {
            Width = 22, Height = 22, CornerRadius = new CornerRadius(11), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 0, 10, 0),
            Background = done ? good : Brushes.Transparent, BorderBrush = done ? good : muted, BorderThickness = new Thickness(1.5),
            Child = done
                ? new TextBlock { Text = "\uE73E", Style = (Style)FindResource("Icon"), FontSize = 11, FontWeight = FontWeights.Bold, Foreground = (Brush)FindResource("Rail"), HorizontalAlignment = HorizontalAlignment.Center }
                : new TextBlock { Text = number.ToString(), FontSize = 11, FontWeight = FontWeights.SemiBold, Foreground = muted, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
        };
        var text = new StackPanel();
        text.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 2, 0, 2), Foreground = done ? muted : (Brush)FindResource("Text") });
        text.Children.Add(new TextBlock { Text = detail, Style = (Style)FindResource("Hint"), FontSize = 12 });
        if (button is { } action && !done)
        {
            var control = new Button { Content = action.Label, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6, 0, 0) };
            control.Click += action.Click;
            text.Children.Add(control);
        }
        var step = new DockPanel { Margin = new Thickness(0, 0, 12, 6) };
        DockPanel.SetDock(mark, Dock.Left);
        step.Children.Add(mark);
        step.Children.Add(text);
        return step;
    }

    private void OnRefresh(object sender, RoutedEventArgs e)
    {
        RefreshStatus();
        UpdateLive();
    }
    // ------------------------------------------------------------------ updates (notify only - never downloads)

    private void InitializeUpdateUi()
    {
        VersionText.Text = UpdateChecker.CurrentVersionText;
        _loading = true;
        CheckUpdatesBox.IsChecked = _appSettings.CheckForUpdates;
        IncludeBetasBox.IsChecked = _appSettings.IncludeBetas;
        _loading = false;

        if (!UpdateChecker.IsConfigured)
        {
            // Builds made before the project has a public home carry no repository name.
            CheckUpdatesBox.IsEnabled = IncludeBetasBox.IsEnabled = CheckNowButton.IsEnabled = ReleasesButton.IsEnabled = RepoButton.IsEnabled = false;
            UpdateStatus.Text = "This build has no update check: nobody gave it a release page to look at.";
            return;
        }
        UpdateStatus.Text = "Looks at " + UpdateChecker.ReleasesPage;
        if (_appSettings.CheckForUpdates) _ = CheckForUpdatesAsync(userAsked: false);
    }

    private async Task CheckForUpdatesAsync(bool userAsked)
    {
        if (!UpdateChecker.IsConfigured) return;
        CheckNowButton.IsEnabled = false;
        if (userAsked) UpdateStatus.Text = "Checking...";
        try
        {
            var update = await UpdateChecker.CheckAsync(UpdateChecker.Repository, _appSettings.IncludeBetas);
            AppLog.Write(update == null ? "Update check: nothing newer than this build." : $"Update check: {update.Version} ({update.Tag}){(update.IsBeta ? ", beta" : "")}{(update.CanInstall ? ", installer with checksum" : ", release page only")}.");
            if (update == null)
            {
                UpdateBanner.Visibility = Visibility.Collapsed;
                UpdateStatus.Text = $"You have the latest version ({UpdateChecker.CurrentVersionText}). Checked at {DateTime.Now:t}.";
            }
            else
            {
                UpdateStatus.Text = $"Version {update.Version.ToString(3)}{(update.IsBeta ? " (beta)" : "")} is available. Checked at {DateTime.Now:t}.";
                // A skipped version stays quiet on start-up, but "Check now" always shows what it found.
                if (userAsked || _appSettings.SkippedVersion != update.Version.ToString(3)) ShowUpdate(update);
            }
        }
        catch (Exception e)
        {
            // Offline, rate-limited, no releases yet... never worth a pop-up. Only say so if the user asked.
            if (userAsked) UpdateStatus.Text = "Could not check for updates: " + e.Message;
            AppLog.Write("Update check failed", e);
        }
        finally
        {
            CheckNowButton.IsEnabled = true;
        }
    }

    private void ShowUpdate(ReleaseInfo update)
    {
        _availableUpdate = update;
        UpdateTitle.Text = update.IsBeta
            ? $"A beta version is available: {update.Version.ToString(3)}"
            : $"A new version is available: {update.Version.ToString(3)}";
        UpdateDetail.Text = update.CanInstall
            ? $"You have {UpdateChecker.CurrentVersionText}. \"{update.Title}\" upgrades in place; close the game and OBS first."
            : $"You have {UpdateChecker.CurrentVersionText}. \"{update.Title}\": download the .msi from the release page and run it. It upgrades in place.";
        UpdateInstallButton.Visibility = update.CanInstall ? Visibility.Visible : Visibility.Collapsed;
        UpdateBanner.Visibility = Visibility.Visible;
    }

    private bool _updating;

    /// <summary>
    /// Fetches the new installer and hands it to Windows Installer. Nothing of ours may be in use while it replaces
    /// files, so a running game or OBS stops it here with a plain message; the mirror window and the SteamVR helper
    /// are closed by the app itself, and the app closes last.
    /// </summary>
    private async void OnInstallUpdate(object sender, RoutedEventArgs e)
    {
        if (_availableUpdate is not { CanInstall: true } update || _updating) return;
        if (GazeLive.Read().State != GazeLiveState.NoGame)
        {
            UpdateDetail.Text = "A VR game is still running. Close it, then press Download and install again.";
            return;
        }
        if (Process.GetProcessesByName("obs64").Length > 0 || Process.GetProcessesByName("obs32").Length > 0)
        {
            UpdateDetail.Text = "OBS is still running (the installer replaces its Gaze Mirror plugin). Close OBS, then press Download and install again.";
            return;
        }

        _updating = true;
        UpdateInstallButton.IsEnabled = UpdateSkipButton.IsEnabled = UpdateLaterButton.IsEnabled = false;
        try
        {
            var name = update.Installer!.Name;
            AppLog.Write($"Update: downloading {name} from {update.Installer.Url} ({update.Installer.Size:N0} bytes).");
            var progress = new Progress<double>(p => UpdateDetail.Text = $"Downloading {name} into your Downloads folder... {p:P0}");
            UpdateDetail.Text = $"Downloading {name} into your Downloads folder...";
            var path = await UpdateChecker.DownloadInstallerAsync(update, progress);
            AppLog.Write($"Update: {path} downloaded and checked; starting Windows Installer and closing.");
            UpdateDetail.Text = "Checked against the release's SHA-256. Closing the mirror window and the SteamVR helper, then opening the installer...";
            if (MirrorWindowControl.IsRunning()) MirrorWindowControl.Close();
            MirrorLive.StopHelper();
            await Task.Delay(500);
            if (!UpdateChecker.StartInstaller(path))
            {
                UpdateDetail.Text = $"Windows Installer did not start. The checked installer is in your Downloads folder: {name}. Open it from there.";
                return;
            }
            Close();
        }
        catch (Exception error)
        {
            UpdateDetail.Text = "The download failed: " + error.Message + " The app installed nothing. View release still works.";
            AppLog.Write("Update download failed", error);
        }
        finally
        {
            _updating = false;
            UpdateInstallButton.IsEnabled = UpdateSkipButton.IsEnabled = UpdateLaterButton.IsEnabled = true;
        }
    }

    private static void OpenInBrowser(string url)
    {
        // Only ever this project's own pages on github.com.
        if (!url.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase)) return;
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { /* no browser: nothing to do */ }
    }

    private void OnViewUpdate(object sender, RoutedEventArgs e)
    {
        if (_availableUpdate != null) OpenInBrowser(_availableUpdate.Url);
    }

    private void OnSkipUpdate(object sender, RoutedEventArgs e)
    {
        if (_availableUpdate != null)
        {
            _appSettings.SkippedVersion = _availableUpdate.Version.ToString(3);
            _appSettings.Save();
        }
        UpdateBanner.Visibility = Visibility.Collapsed;
    }

    private void OnDismissUpdate(object sender, RoutedEventArgs e) => UpdateBanner.Visibility = Visibility.Collapsed;

    private void OnUpdatePreferenceChanged(object sender, RoutedEventArgs e)
    {
        if (_loading || !IsLoaded) return;
        _appSettings.CheckForUpdates = CheckUpdatesBox.IsChecked == true;
        _appSettings.IncludeBetas = IncludeBetasBox.IsChecked == true;
        _appSettings.Save();
    }

    private async void OnCheckNow(object sender, RoutedEventArgs e) => await CheckForUpdatesAsync(userAsked: true);

    private void OnOpenReleases(object sender, RoutedEventArgs e) => OpenInBrowser(UpdateChecker.ReleasesPage);
}