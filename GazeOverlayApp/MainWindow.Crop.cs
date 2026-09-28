using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace GazeOverlay;

/// <summary>
/// The Crop tab: a box, locked to a chosen shape, that is dragged and scaled on a picture of the whole mirror image. The
/// mirror layer then hands OBS only that box, so the OBS source IS the box. The values live in the ring's settings file
/// (crop_* keys) and go live through the same signal as everything else there.
/// </summary>
public partial class MainWindow
{
    private readonly CropBox _crop = new();
    private MirrorPicture? _cropPicture;
    private int _cropFullWidth, _cropFullHeight;

    private readonly Rectangle[] _cropShades = [new(), new(), new(), new()];
    private readonly Rectangle _cropFrame = new() { StrokeThickness = 2, Fill = Brushes.Transparent };
    private readonly Rectangle[] _cropHandles = [new(), new(), new(), new()];
    private readonly Line[] _cropThirds = [new(), new(), new(), new()];
    private readonly Line[] _cropStillZone = [new(), new()];
    private readonly Rectangle[] _cropReach = [new(), new()];
    private readonly Rectangle _cropReachFrame = new() { StrokeThickness = 1, Fill = Brushes.Transparent, StrokeDashArray = [3, 3] };
    private readonly Rectangle[] _cropSteadyRoom = [new(), new(), new(), new()];
    private readonly Rectangle _cropSteadyFrame = new() { StrokeThickness = 1, Fill = Brushes.Transparent, StrokeDashArray = [3, 3] };
    private readonly Image _cropImage = new() { Stretch = Stretch.Fill };
    // The middle of the eye image: a dot, a line across and a line down the whole image.
    private readonly Line[] _cropCentreLines = [new(), new()];
    private readonly Ellipse _cropCentreDot = new() { Width = 7, Height = 7 };
    private readonly TextBlock _cropPlaceholder = new() { TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center };
    private Rect _cropShown;                 // Where the picture is drawn inside the canvas.
    private enum CropDrag { None, Move, Resize }
    private CropDrag _cropDrag;
    private Point _cropAnchor;               // Resize: the corner that stays put (fractions). Move: pointer offset from the centre.

    private static string LastPicturePath => System.IO.Path.Combine(AppSettings.Folder, "last-mirror-picture.png");

    private void BuildCropUi()
    {
        var accent = (Brush)FindResource("Accent");
        _cropFrame.Stroke = accent;
        CropCanvas.Children.Add(_cropImage);
        _cropPlaceholder.Foreground = (Brush)FindResource("Muted");
        CropCanvas.Children.Add(_cropPlaceholder);
        foreach (var shade in _cropShades)
        {
            shade.Fill = new SolidColorBrush(Color.FromArgb(150, 0, 0, 0));
            shade.IsHitTestVisible = false;
            CropCanvas.Children.Add(shade);
        }
        foreach (var line in _cropThirds)
        {
            line.Stroke = new SolidColorBrush(Color.FromArgb(70, 255, 255, 255));
            line.StrokeThickness = 1;
            line.IsHitTestVisible = false;
            CropCanvas.Children.Add(line);
        }
        // How far the box may follow the gaze: a tint above and below it, inside a dashed outline.
        foreach (var reach in _cropReach)
        {
            reach.Fill = new SolidColorBrush(Color.FromArgb(70, 77, 178, 255));
            reach.IsHitTestVisible = false;
            CropCanvas.Children.Add(reach);
        }
        _cropReachFrame.Stroke = new SolidColorBrush(Color.FromArgb(190, 77, 178, 255));
        _cropReachFrame.IsHitTestVisible = false;
        CropCanvas.Children.Add(_cropReachFrame);
        // Room for the picture steadying: a band of another colour all around the box. The box cannot enter it.
        foreach (var band in _cropSteadyRoom)
        {
            band.Fill = new SolidColorBrush(Color.FromArgb(85, 70, 220, 150));
            band.IsHitTestVisible = false;
            CropCanvas.Children.Add(band);
        }
        _cropSteadyFrame.Stroke = new SolidColorBrush(Color.FromArgb(200, 70, 220, 150));
        _cropSteadyFrame.IsHitTestVisible = false;
        CropCanvas.Children.Add(_cropSteadyFrame);
        foreach (var line in _cropStillZone)
        {
            line.Stroke = new SolidColorBrush(Color.FromArgb(200, 255, 190, 60));
            line.StrokeThickness = 1.5;
            line.StrokeDashArray = [5, 4];
            line.IsHitTestVisible = false;
            CropCanvas.Children.Add(line);
        }
        foreach (var line in _cropCentreLines)
        {
            line.Stroke = new SolidColorBrush(Color.FromArgb(150, 255, 255, 255));
            line.StrokeThickness = 1;
            line.IsHitTestVisible = false;
            CropCanvas.Children.Add(line);
        }
        _cropCentreDot.Fill = new SolidColorBrush(Color.FromArgb(230, 255, 255, 255));
        _cropCentreDot.Stroke = new SolidColorBrush(Color.FromArgb(200, 0, 0, 0));
        _cropCentreDot.StrokeThickness = 1;
        _cropCentreDot.IsHitTestVisible = false;
        CropCanvas.Children.Add(_cropCentreDot);
        _cropFrame.IsHitTestVisible = false;
        CropCanvas.Children.Add(_cropFrame);
        _loading = true; // Setting the boxes fires their handlers, which must not save a half-filled row.
        ShowCropBox.IsChecked = _appSettings.ShowCropBox;
        ShowFollow.IsChecked = _appSettings.ShowFollow;
        ShowSteady.IsChecked = _appSettings.ShowSteady;
        ShowEdges.IsChecked = _appSettings.ShowEdges;
        ShowCentre.IsChecked = _appSettings.ShowCentre;
        _loading = false;
        foreach (var handle in _cropHandles)
        {
            handle.Width = handle.Height = 10;
            handle.Fill = accent;
            handle.IsHitTestVisible = false;
            CropCanvas.Children.Add(handle);
        }

        foreach (var choice in Settings.All.First(d => d.Key == "crop_aspect").Choices)
        {
            CropAspect.Items.Add(new ComboBoxItem { Content = choice.Label, Tag = choice.Value });
        }

        // The crop keys are ordinary settings: loading the file, and changes made outside the app, arrive through these.
        _controlSetters["crop_enabled"] = v => { CropEnabled.IsChecked = v != "0"; DrawCrop(); };
        _controlSetters["crop_aspect"] = v =>
        {
            CropAspect.SelectedItem = CropAspect.Items.Cast<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == v) ?? CropAspect.Items[0];
            _crop.Aspect = CropBox.ParseAspect(v);
            DrawCrop();
        };
        _controlSetters["crop_center_x"] = v => { _crop.CenterX = Settings.ParseDouble(v, 0.5); DrawCrop(); };
        _controlSetters["crop_center_y"] = v => { _crop.CenterY = Settings.ParseDouble(v, 0.5); DrawCrop(); };
        _controlSetters["crop_height"] = v => { _crop.Height = Settings.ParseDouble(v, 0.5); DrawCrop(); };
        _controlSetters["crop_width"] = v => { _crop.FreeWidth = Settings.ParseDouble(v, 1); DrawCrop(); };

        // Gaze following: ordinary rows, folded away until wanted. The picture shows the still zone.
        foreach (var def in Settings.All.Where(d => d.Key.StartsWith("crop_follow", StringComparison.Ordinal)))
        {
            var row = BuildRow(def);
            row.Margin = new Thickness(0, 1, 14, 1); // a little air between the two columns
            PanelCropFollow.Children.Add(row);
            var setter = _controlSetters[def.Key];
            _controlSetters[def.Key] = v =>
            {
                setter(v);
                DrawCrop(); // (the section stays folded until it is wanted; its header says whether following is on)
            };
        }

        // Stabilisation: the same kind of fold-away section.
        foreach (var def in Settings.All.Where(d => d.Key.StartsWith("stabilize", StringComparison.Ordinal)))
        {
            var row = BuildRow(def);
            row.Margin = new Thickness(0, 1, 14, 1);
            PanelStabilize.Children.Add(row);
            var setter = _controlSetters[def.Key];
            _controlSetters[def.Key] = v =>
            {
                setter(v);
                ApplyCropMargin(moveBox: false); // loading: show it; the box is only moved when the user changes something
            };
        }
        StabilizeExpander.SizeChanged += (_, _) => PanelStabilize.Columns = StabilizeExpander.ActualWidth >= 780 ? 2 : 1;
        MirrorSections.SizeChanged += (_, _) =>
        {
            var sideBySide = MirrorSections.ActualWidth >= 760;
            MirrorSections.Columns = sideBySide ? 2 : 1;
            CropFollowExpander.Margin = new Thickness(0, 0, sideBySide ? 6 : 0, 6);
        };

        CropEnabled.Click += (_, _) => { OnValueChanged("crop_enabled", CropEnabled.IsChecked == true ? "1" : "0"); DrawCrop(); };
        CropAspect.SelectionChanged += (_, _) =>
        {
            if (_loading || CropAspect.SelectedItem is not ComboBoxItem { Tag: string value }) return;
            // Going from a locked shape to "free" keeps the box as it is.
            if (value == "0") _crop.FreeWidth = _crop.Size().Width;
            _crop.Aspect = CropBox.ParseAspect(value);
            _crop.Normalize();
            OnValueChanged("crop_aspect", value);
            CommitCrop();
        };

        CropCanvas.SizeChanged += (_, _) => DrawCrop();
        CropFollowExpander.SizeChanged += (_, _) => PanelCropFollow.Columns = CropFollowExpander.ActualWidth >= 780 ? 2 : 1;
        CropCanvas.MouseLeftButtonDown += OnCropMouseDown;
        CropCanvas.MouseMove += OnCropMouseMove;
        CropCanvas.MouseLeftButtonUp += (_, _) => { _cropDrag = CropDrag.None; CropCanvas.ReleaseMouseCapture(); };
        CropCanvas.MouseWheel += (_, e) =>
        {
            // Only over the box itself, and only when it is not locked: scrolling past must not resize it.
            if (CropLocked.IsChecked == true || !IsInsideCropBox(ToFraction(e.GetPosition(CropCanvas)))) return;
            _crop.Scale(e.Delta > 0 ? 1.04 : 1 / 1.04);
            CommitCrop();
            e.Handled = true;
        };

        _loading = true;
        CropLocked.IsChecked = _appSettings.CropLocked;
        _loading = false;
        void LockChanged()
        {
            CropCentreButton.IsEnabled = CropLargestButton.IsEnabled = CropAspect.IsEnabled = CropLocked.IsChecked != true;
            if (_loading || !IsLoaded) return;
            _appSettings.CropLocked = CropLocked.IsChecked == true;
            _appSettings.Save();
            DrawCrop();
        }
        CropLocked.Checked += (_, _) => LockChanged();
        CropLocked.Unchecked += (_, _) => LockChanged();
        LockChanged();

        BuildCaptureUi();
        LoadLastCropPicture();
        Tabs.SelectionChanged += async (_, e) =>
        {
            // Opening the tab asks the running game for a fresh picture once - an event, not a timer.
            if (ReferenceEquals(e.OriginalSource, Tabs) && ReferenceEquals(Tabs.SelectedItem, CropTab)) await RefreshCropPictureAsync(quietWhenNoGame: true, automatic: true);
        };
        // Coming back to the app with this tab open (from the game, from OBS): a fresh picture too. Still an event.
        Activated += async (_, _) =>
        {
            if (ReferenceEquals(Tabs.SelectedItem, CropTab) && CropRefresh.IsEnabled) await RefreshCropPictureAsync(quietWhenNoGame: true, automatic: true);
        };
    }

    private async Task RefreshCropPictureAsync(bool quietWhenNoGame, bool automatic = false)
    {
        // A picture captured with the key was set up on purpose: only the button replaces it. Nor while a capture is armed.
        if (automatic && (_appSettings.MirrorPictureKept || _captureArmed)) return;
        CropRefresh.IsEnabled = false;
        var (picture, message) = await MirrorSnapshot.RequestAsync();
        CropRefresh.IsEnabled = true;
        if (picture == null)
        {
            if (!quietWhenNoGame || _cropImage.Source == null) CropStatus.Text = message + (_cropImage.Source == null ? " The box can still be set; it just has no picture behind it." : "");
            return;
        }

        ShowNewPicture(picture, kept: false);
        CropStatus.Text = $"Picture taken at {DateTime.Now:T}.";
    }

    /// <summary>Development aid for --screenshots: a made-up picture, so the tab can be looked at without a game.</summary>
    internal void ShowCropTestPicture()
    {
        const int size = 320;
        var pixels = new byte[size * size * 4];
        for (var y = 0; y < size; y++)
        {
            for (var x = 0; x < size; x++)
            {
                var i = (y * size + x) * 4;
                var sky = y < size * 0.55;
                var lens = Math.Sqrt(Math.Pow(x - size / 2.0, 2) + Math.Pow(y - size / 2.0, 2)) < size * 0.48;
                pixels[i] = (byte)(lens ? (sky ? 200 - y / 3 : 60) : 8);
                pixels[i + 1] = (byte)(lens ? (sky ? 150 - y / 4 : 90 + (x + y) % 40) : 8);
                pixels[i + 2] = (byte)(lens ? (sky ? 90 : 70) : 8);
            }
        }
        var image = BitmapSource.Create(size, size, 96, 96, PixelFormats.Bgr32, null, pixels, size * 4);
        image.Freeze();
        _cropImage.Source = _pictureFirst = image;
        _cropFullWidth = _cropFullHeight = 8192;
        _crop.ImageAspect = 1;
        CropStatus.Text = "(made-up picture for the screenshot)";
        // Show the gaze-following parts too. Nothing here goes through OnValueChanged, so nothing is saved.
        CropEnabled.IsChecked = true;
        _values["crop_follow"] = "vertical";
        CropFollowExpander.IsExpanded = false;
        _values["stabilize"] = "1";
        _crop.MoveTo(0, _crop.CenterY); // against the left wall: the steadying room has to hold it off
        ApplyCropMargin(moveBox: false);
        ShowSelectedPicture();
        if (Environment.GetEnvironmentVariable("GAZE_SCREENSHOT_ARMED") == "1")
        {
            _captureArmed = true; // only the look of it: nothing is armed anywhere
            UpdateCaptureUi();
        }
    }

    private async void OnCropRefresh(object sender, RoutedEventArgs e) => await RefreshCropPictureAsync(quietWhenNoGame: false);

    private void OnCropCentre(object sender, RoutedEventArgs e)
    {
        _crop.MoveTo(0.5, 0.5);
        CommitCrop();
    }

    private void OnCropFill(object sender, RoutedEventArgs e)
    {
        _crop.Height = 1;
        _crop.FreeWidth = 1;
        _crop.MoveTo(0.5, 0.5);
        CommitCrop();
    }

    /// <summary>Writes the box to the settings (saved and sent to the game by the usual debounced save) and redraws it.</summary>
    /// <summary>
    /// With the picture steadying on, the box keeps its "room to move" free on every side: that band is part of the box as
    /// far as the edges of the image are concerned. Switching it on (or widening it) pushes or shrinks the box to fit.
    /// </summary>
    private void ApplyCropMargin(bool moveBox)
    {
        _crop.Margin = _values.GetValueOrDefault("stabilize") == "1"
            ? Math.Clamp(Settings.ParseDouble(_values.GetValueOrDefault("stabilize_room"), 0.02), 0, 0.25) : 0;
        var before = (_crop.CenterX, _crop.CenterY, _crop.Height, _crop.FreeWidth);
        if (moveBox)
        {
            _crop.Normalize();
            if (before != (_crop.CenterX, _crop.CenterY, _crop.Height, _crop.FreeWidth))
            {
                CommitCrop();
                return;
            }
        }
        DrawCrop();
    }

    private void CommitCrop()
    {
        var c = CultureInfo.InvariantCulture;
        _crop.Normalize();
        OnValueChanged("crop_center_x", _crop.CenterX.ToString("0.####", c));
        OnValueChanged("crop_center_y", _crop.CenterY.ToString("0.####", c));
        OnValueChanged("crop_height", _crop.Height.ToString("0.####", c));
        if (_crop.Aspect <= 0) OnValueChanged("crop_width", _crop.FreeWidth.ToString("0.####", c));
        DrawCrop();
    }

    // ------------------------------------------------------------------ drawing

    private void DrawCrop()
    {
        if (CropCanvas.ActualWidth < 4 || CropCanvas.ActualHeight < 4) return;

        // The picture, as large as fits.
        var aspect = _crop.ImageAspect;
        var width = Math.Min(CropCanvas.ActualWidth, CropCanvas.ActualHeight * aspect);
        var height = width / aspect;
        _cropShown = new Rect((CropCanvas.ActualWidth - width) / 2, (CropCanvas.ActualHeight - height) / 2, width, height);
        Place(_cropImage, _cropShown);
        _cropPlaceholder.Visibility = _cropImage.Source == null ? Visibility.Visible : Visibility.Collapsed;
        _cropPlaceholder.Text = "No picture yet.\nStart your game with OBS showing the OpenXR Mirror Capture source, then press \"Refresh picture\".";
        _cropPlaceholder.Width = Math.Max(width - 40, 60);
        Canvas.SetLeft(_cropPlaceholder, _cropShown.Left + 20);
        Canvas.SetTop(_cropPlaceholder, _cropShown.Top + height / 2 - 30);

        var (left, top, boxWidth, boxHeight) = _crop.Rect();
        var box = new Rect(_cropShown.Left + left * width, _cropShown.Top + top * height, boxWidth * width, boxHeight * height);
        var active = CropEnabled.IsChecked == true;

        Place(_cropShades[0], new Rect(_cropShown.Left, _cropShown.Top, width, Math.Max(box.Top - _cropShown.Top, 0)));
        Place(_cropShades[1], new Rect(_cropShown.Left, box.Bottom, width, Math.Max(_cropShown.Bottom - box.Bottom, 0)));
        Place(_cropShades[2], new Rect(_cropShown.Left, box.Top, Math.Max(box.Left - _cropShown.Left, 0), box.Height));
        Place(_cropShades[3], new Rect(box.Right, box.Top, Math.Max(_cropShown.Right - box.Right, 0), box.Height));
        foreach (var shade in _cropShades) shade.Opacity = active ? 1 : 0.35;

        Place(_cropFrame, box);
        _cropFrame.StrokeDashArray = active ? null : [4, 3];
        PlaceEdgeOutline(_cropShown);
        var corners = new[] { box.TopLeft, box.TopRight, box.BottomLeft, box.BottomRight };
        for (var i = 0; i < 4; i++)
        {
            Canvas.SetLeft(_cropHandles[i], corners[i].X - 5);
            Canvas.SetTop(_cropHandles[i], corners[i].Y - 5);
        }
        for (var i = 0; i < 2; i++)
        {
            var x = box.Left + box.Width * (i + 1) / 3;
            (_cropThirds[i].X1, _cropThirds[i].Y1, _cropThirds[i].X2, _cropThirds[i].Y2) = (x, box.Top, x, box.Bottom);
            var y = box.Top + box.Height * (i + 1) / 3;
            (_cropThirds[i + 2].X1, _cropThirds[i + 2].Y1, _cropThirds[i + 2].X2, _cropThirds[i + 2].Y2) = (box.Left, y, box.Right, y);
        }

        var locked = CropLocked.IsChecked == true;
        foreach (var handle in _cropHandles) handle.Visibility = locked ? Visibility.Collapsed : Visibility.Visible;

        // Gaze following: the tinted area is how far the box may travel, the two dashed lines bound the zone in which the
        // gaze moves nothing.
        var following = active && _values.GetValueOrDefault("crop_follow") == "vertical";
        CropFollowExpander.Header = "Follow my gaze (up / down): " +
            (_values.GetValueOrDefault("crop_follow") != "vertical" ? "off" : active ? "on" : "on, but the crop is off");
        StabilizeExpander.Header = "Steady the picture: " +
            (_values.GetValueOrDefault("stabilize") != "1" ? "off" : active ? "on" : "on, but the crop is off");
        // The steadying's room to move lies OUTSIDE everywhere the box can be: around the box, and - when the box follows
        // the gaze - around the whole reach. So the reach stops short of the picture's top and bottom by that room (the
        // layer does the same), and the band never leaves the picture.
        var steadying = active && _crop.Margin > 0;
        var steadyX = steadying ? _crop.Margin * _cropShown.Width : 0;
        var steadyY = steadying ? _crop.Margin * _cropShown.Height : 0;
        var reachShare = Math.Clamp(Settings.ParseDouble(_values.GetValueOrDefault("crop_follow_reach"), 1), 0, 10);
        var reachTop = Math.Min(Math.Max(box.Top - reachShare * box.Height, _cropShown.Top + steadyY), box.Top);
        var reachBottom = Math.Max(Math.Min(box.Bottom + reachShare * box.Height, _cropShown.Bottom - steadyY), box.Bottom);
        var travelTop = following && reachShare > 0 ? reachTop : box.Top;
        var travelBottom = following && reachShare > 0 ? reachBottom : box.Bottom;
        var outer = new Rect(new Point(Math.Max(box.Left - steadyX, _cropShown.Left), Math.Max(travelTop - steadyY, _cropShown.Top)),
                             new Point(Math.Min(box.Right + steadyX, _cropShown.Right), Math.Min(travelBottom + steadyY, _cropShown.Bottom)));
        Place(_cropSteadyRoom[0], new Rect(outer.Left, outer.Top, outer.Width, Math.Max(travelTop - outer.Top, 0)));
        Place(_cropSteadyRoom[1], new Rect(outer.Left, travelBottom, outer.Width, Math.Max(outer.Bottom - travelBottom, 0)));
        Place(_cropSteadyRoom[2], new Rect(outer.Left, travelTop, Math.Max(box.Left - outer.Left, 0), Math.Max(travelBottom - travelTop, 0)));
        Place(_cropSteadyRoom[3], new Rect(box.Right, travelTop, Math.Max(outer.Right - box.Right, 0), Math.Max(travelBottom - travelTop, 0)));
        Place(_cropSteadyFrame, outer);
        foreach (var band in _cropSteadyRoom) band.Visibility = steadying ? Visibility.Visible : Visibility.Collapsed;
        _cropSteadyFrame.Visibility = steadying ? Visibility.Visible : Visibility.Collapsed;
        Place(_cropReach[0], new Rect(box.Left, reachTop, box.Width, Math.Max(box.Top - reachTop, 0)));
        Place(_cropReach[1], new Rect(box.Left, box.Bottom, box.Width, Math.Max(reachBottom - box.Bottom, 0)));
        Place(_cropReachFrame, new Rect(box.Left, reachTop, box.Width, Math.Max(reachBottom - reachTop, 0)));
        _cropReach[0].Visibility = _cropReach[1].Visibility = _cropReachFrame.Visibility =
            following && reachShare > 0 ? Visibility.Visible : Visibility.Collapsed;
        var zone = Math.Clamp(Settings.ParseDouble(_values.GetValueOrDefault("crop_follow_deadzone"), 0.4), 0, 0.9);
        for (var i = 0; i < 2; i++)
        {
            var y = box.Top + box.Height * (i == 0 ? (1 - zone) / 2 : (1 + zone) / 2);
            (_cropStillZone[i].X1, _cropStillZone[i].Y1, _cropStillZone[i].X2, _cropStillZone[i].Y2) = (box.Left, y, box.Right, y);
            _cropStillZone[i].Visibility = following ? Visibility.Visible : Visibility.Collapsed;
        }
        DrawCaptureOverlays(box, reachTop, reachBottom, following && reachShare > 0);

        // The middle of the eye image.
        var centre = new Point(_cropShown.Left + _cropShown.Width / 2, _cropShown.Top + _cropShown.Height / 2);
        (_cropCentreLines[0].X1, _cropCentreLines[0].Y1, _cropCentreLines[0].X2, _cropCentreLines[0].Y2) = (centre.X, _cropShown.Top, centre.X, _cropShown.Bottom);
        (_cropCentreLines[1].X1, _cropCentreLines[1].Y1, _cropCentreLines[1].X2, _cropCentreLines[1].Y2) = (_cropShown.Left, centre.Y, _cropShown.Right, centre.Y);
        Canvas.SetLeft(_cropCentreDot, centre.X - 3.5);
        Canvas.SetTop(_cropCentreDot, centre.Y - 3.5);
        ApplyPictureToggles(locked);

        // What OBS will get, in pixels - worked out the way the layer does (even sizes).
        if (_cropFullWidth > 0 && _cropFullHeight > 0)
        {
            var pixelWidth = (int)(boxWidth * _cropFullWidth) & ~1;
            var pixelHeight = (int)(boxHeight * _cropFullHeight) & ~1;
            var share = 100.0 * pixelWidth * pixelHeight / ((double)_cropFullWidth * _cropFullHeight);
            CropReadout.Text = active
                ? $"OBS gets {pixelWidth} x {pixelHeight} px - {share:0} % of the {_cropFullWidth} x {_cropFullHeight} mirror image."
                : $"Crop is off: OBS gets the whole {_cropFullWidth} x {_cropFullHeight} image. The box would be {pixelWidth} x {pixelHeight} px.";
        }
        else
        {
            CropReadout.Text = active ? "Crop is on." : "Crop is off: OBS gets the whole mirror image.";
        }
    }

    /// <summary>The Show toggles above the picture: each switched-off help is hidden, whatever DrawCrop worked out for it.</summary>
    private void ApplyPictureToggles(bool locked)
    {
        static void Hide(bool show, params FrameworkElement[] elements)
        {
            if (show) return;
            foreach (var element in elements) element.Visibility = Visibility.Collapsed;
        }
        var showBox = ShowCropBox.IsChecked == true;
        foreach (var shade in _cropShades) shade.Visibility = showBox ? Visibility.Visible : Visibility.Collapsed;
        _cropFrame.Visibility = showBox ? Visibility.Visible : Visibility.Collapsed;
        foreach (var third in _cropThirds) third.Visibility = showBox ? Visibility.Visible : Visibility.Collapsed;
        foreach (var handle in _cropHandles) handle.Visibility = showBox && !locked ? Visibility.Visible : Visibility.Collapsed;
        Hide(ShowFollow.IsChecked == true, _cropReach[0], _cropReach[1], _cropReachFrame, _cropStillZone[0], _cropStillZone[1],
             _cropTravelLines[0], _cropTravelLines[1], _cropTravelLabels[0], _cropTravelLabels[1]);
        Hide(ShowSteady.IsChecked == true, _cropSteadyRoom[0], _cropSteadyRoom[1], _cropSteadyRoom[2], _cropSteadyRoom[3], _cropSteadyFrame);
        Hide(ShowEdges.IsChecked == true, _edgeOutline);
        var showCentre = ShowCentre.IsChecked == true;
        foreach (var line in _cropCentreLines) line.Visibility = showCentre ? Visibility.Visible : Visibility.Collapsed;
        _cropCentreDot.Visibility = showCentre ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnPictureToggle(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _appSettings.ShowCropBox = ShowCropBox.IsChecked == true;
        _appSettings.ShowFollow = ShowFollow.IsChecked == true;
        _appSettings.ShowSteady = ShowSteady.IsChecked == true;
        _appSettings.ShowEdges = ShowEdges.IsChecked == true;
        _appSettings.ShowCentre = ShowCentre.IsChecked == true;
        _appSettings.Save();
        DrawCrop();
    }

    private static void Place(FrameworkElement element, Rect rect)
    {
        Canvas.SetLeft(element, rect.Left);
        Canvas.SetTop(element, rect.Top);
        element.Width = Math.Max(rect.Width, 0);
        element.Height = Math.Max(rect.Height, 0);
    }

    // ------------------------------------------------------------------ dragging

    private Point ToFraction(Point canvasPoint) => new(
        Math.Clamp((canvasPoint.X - _cropShown.Left) / Math.Max(_cropShown.Width, 1), 0, 1),
        Math.Clamp((canvasPoint.Y - _cropShown.Top) / Math.Max(_cropShown.Height, 1), 0, 1));

    private bool IsInsideCropBox(Point fraction)
    {
        var (left, top, width, height) = _crop.Rect();
        return fraction.X >= left && fraction.X <= left + width && fraction.Y >= top && fraction.Y <= top + height;
    }

    private void OnCropMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (CropLocked.IsChecked == true) return;
        var pointer = e.GetPosition(CropCanvas);
        var (left, top, width, height) = _crop.Rect();
        var corners = new[] { new Point(left, top), new Point(left + width, top), new Point(left, top + height), new Point(left + width, top + height) };
        var fraction = ToFraction(pointer);

        // A corner (within 14 px): resize, with the opposite corner as the anchor.
        for (var i = 0; i < 4; i++)
        {
            var corner = new Point(_cropShown.Left + corners[i].X * _cropShown.Width, _cropShown.Top + corners[i].Y * _cropShown.Height);
            if ((corner - pointer).Length > 14) continue;
            _cropDrag = CropDrag.Resize;
            _cropAnchor = corners[3 - i];
            CropCanvas.CaptureMouse();
            return;
        }

        // Only a drag that starts inside the box moves it; a click anywhere else does nothing.
        if (!IsInsideCropBox(fraction)) return;
        _cropDrag = CropDrag.Move;
        _cropAnchor = new Point(fraction.X - _crop.CenterX, fraction.Y - _crop.CenterY);
        CropCanvas.CaptureMouse();
    }

    private void OnCropMouseMove(object sender, MouseEventArgs e)
    {
        var pointer = e.GetPosition(CropCanvas);
        if (_cropDrag == CropDrag.None)
        {
            // Show what a click here would do.
            if (CropLocked.IsChecked == true)
            {
                CropCanvas.Cursor = Cursors.Arrow;
                return;
            }
            var (left, top, width, height) = _crop.Rect();
            var corners = new[] { new Point(left, top), new Point(left + width, top), new Point(left, top + height), new Point(left + width, top + height) };
            var cursor = IsInsideCropBox(ToFraction(pointer)) ? Cursors.SizeAll : Cursors.Arrow;
            for (var i = 0; i < 4; i++)
            {
                var corner = new Point(_cropShown.Left + corners[i].X * _cropShown.Width, _cropShown.Top + corners[i].Y * _cropShown.Height);
                if ((corner - pointer).Length <= 14) cursor = i is 0 or 3 ? Cursors.SizeNWSE : Cursors.SizeNESW;
            }
            CropCanvas.Cursor = cursor;
            return;
        }

        var fraction = ToFraction(pointer);
        if (_cropDrag == CropDrag.Move) _crop.MoveTo(fraction.X - _cropAnchor.X, fraction.Y - _cropAnchor.Y);
        else _crop.ResizeFromCorner(_cropAnchor.X, _cropAnchor.Y, fraction.X, fraction.Y);
        CommitCrop();
    }
}
