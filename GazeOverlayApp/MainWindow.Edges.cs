using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace GazeOverlay;

/// <summary>
/// The view edges: the headset goes black, and what lies outside one edge (left, right, top, bottom of the mirrored
/// eye's image) is a white block that flashes slowly; the other edges' blocks are grey. The user pushes the white block
/// outwards with the arrow keys until it is gone from their view, which puts the edge where the lens view ends.
/// Enter confirms an edge, Backspace goes back one, Esc cancels. The four positions are kept, drawn on the crop picture
/// and can be turned into a crop box with one button - so the crop covers exactly what the eye can see.
/// The keys are read HERE, in this window (it takes the focus when the measuring starts), never by the game: the game
/// sees nothing. The blocks are drawn by the layer (OpenXR games) or shown as a SteamVR overlay (SteamVR games).
/// </summary>
public partial class MainWindow
{
    private static readonly string[] EdgeNames = ["LEFT", "RIGHT", "TOP", "BOTTOM"];
    private bool _edgeActive;
    private int _edgeBar;
    private readonly double[] _edges = [0.05, 0.95, 0.05, 0.95];
    private readonly Rectangle _edgeOutline = new()
    {
        Stroke = new SolidColorBrush(Color.FromArgb(230, 90, 220, 255)), StrokeThickness = 1.5, StrokeDashArray = [5, 3], Fill = Brushes.Transparent, IsHitTestVisible = false,
    };

    private void BuildEdgesUi()
    {
        CropCanvas.Children.Add(_edgeOutline);
        PreviewKeyDown += OnEdgeKey;
        Closed += (_, _) => { if (_edgeActive) EdgeFinish(cancelled: true); };
        UpdateEdgesUi();
    }

    private void UpdateEdgesUi()
    {
        CropEdgesButton.IsEnabled = !_edgeActive;
        CropToEdgesButton.IsEnabled = !_edgeActive && _appSettings.VisibleEdges is { Length: 4 };
        EdgeBanner.Visibility = _edgeActive ? Visibility.Visible : Visibility.Collapsed;
        if (_edgeActive)
        {
            var outward = _edgeBar switch { 0 => "Left", 1 => "Right", 2 => "Up", _ => "Down" };
            EdgeBannerTitle.Text = $"MEASURING YOUR VIEW EDGES: {_edgeBar + 1} of 4, {EdgeNames[_edgeBar]}";
            EdgeBannerDetail.Text = $"The headset is black, and the flashing white block is the part your eye should not see. Push it outwards with {outward} (hold the key to keep moving; Shift = bigger steps) until it is gone from your view, then press Enter. Backspace goes back a side, Esc cancels. Keep this window focused: the game gets none of the keys.";
            EdgeBannerValues.Text = $"left {_edges[0]:P1}, right {_edges[1]:P1}, top {_edges[2]:P1}, bottom {_edges[3]:P1} of the eye image";
        }
        CropEdgesStatus.Text = _appSettings.VisibleEdges is { Length: 4 } saved
            ? $"View edges ({(_appSettings.VisibleEdgesEye == 0 ? "left" : "right")} eye): left {saved[0]:P1}, right {saved[1]:P1}, top {saved[2]:P1}, bottom {saved[3]:P1} of the eye image."
            : "Not measured yet.";
    }

    private void OnEdgeFinderStart(object sender, RoutedEventArgs e)
    {
        if (_captureArmed) { CropStatus.Text = "Finish or cancel the capture first."; return; }
        // The bars start well inside the view, where they are plainly visible, and are pushed out from there.
        _edges[0] = 0.30; _edges[1] = 0.70; _edges[2] = 0.30; _edges[3] = 0.70;
        _edgeBar = 0;
        if (!EdgeSend())
        {
            CropStatus.Text = "No game is running, or it runs an older layer. Start the game first, then measure.";
            return;
        }
        _edgeActive = true;
        Activate();
        Focus();
        UpdateEdgesUi();
        CropStatus.Text = "Measuring the view edges: put the headset on and use the arrow keys here.";
        AppLog.Write("View edges: measuring started.");
    }

    private void OnCropToEdges(object sender, RoutedEventArgs e)
    {
        if (_appSettings.VisibleEdges is not { Length: 4 } edges) return;
        // The largest box of the chosen shape inside the visible region, centred in it.
        var regionWidth = Math.Max(edges[1] - edges[0], CropBox.MinSize);
        var regionHeight = Math.Max(edges[3] - edges[2], CropBox.MinSize);
        double width, height;
        if (_crop.Aspect > 0)
        {
            height = Math.Min(regionHeight, regionWidth * _crop.ImageAspect / _crop.Aspect);
            width = height * _crop.Aspect / _crop.ImageAspect;
        }
        else
        {
            height = regionHeight;
            width = regionWidth;
        }
        _crop.Height = height;
        _crop.FreeWidth = width;
        _crop.MoveTo((edges[0] + edges[1]) / 2, (edges[2] + edges[3]) / 2);
        CommitCrop();
        CropStatus.Text = "The crop box now covers what you can see.";
        AppLog.Write("Crop box fitted to the view edges.");
    }

    private void OnEdgeKey(object sender, KeyEventArgs e)
    {
        if (!_edgeActive) return;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        var step = (Keyboard.Modifiers & ModifierKeys.Shift) != 0 ? 0.04 : 0.01; // Held keys repeat, so a bar crosses the view in a second or two.
        var handled = true;
        switch (key)
        {
            case Key.Left: EdgeNudge(_edgeBar == 0 ? -step : _edgeBar == 1 ? -step : 0); break;
            case Key.Right: EdgeNudge(_edgeBar == 0 ? step : _edgeBar == 1 ? step : 0); break;
            case Key.Up: EdgeNudge(_edgeBar == 2 ? -step : _edgeBar == 3 ? -step : 0); break;
            case Key.Down: EdgeNudge(_edgeBar == 2 ? step : _edgeBar == 3 ? step : 0); break;
            case Key.Enter:
                if (_edgeBar < 3) { _edgeBar++; EdgeSend(); UpdateEdgesUi(); }
                else EdgeFinish(cancelled: false);
                break;
            case Key.Back:
                if (_edgeBar > 0) { _edgeBar--; EdgeSend(); UpdateEdgesUi(); }
                break;
            case Key.Escape: EdgeFinish(cancelled: true); break;
            default: handled = false; break;
        }
        e.Handled = handled;
    }

    /// <summary>Moves the current bar by `delta` along its own axis (left/top negative = outwards for the first of each pair).</summary>
    private void EdgeNudge(double delta)
    {
        if (delta == 0) return;
        var i = _edgeBar;
        _edges[i] = Math.Clamp(_edges[i] + delta, 0, 1);
        // The pairs never cross: a bar stops a little short of its partner.
        if (i == 0) _edges[0] = Math.Min(_edges[0], _edges[1] - 0.05);
        if (i == 1) _edges[1] = Math.Max(_edges[1], _edges[0] + 0.05);
        if (i == 2) _edges[2] = Math.Min(_edges[2], _edges[3] - 0.05);
        if (i == 3) _edges[3] = Math.Max(_edges[3], _edges[2] + 0.05);
        EdgeSend();
        UpdateEdgesUi();
    }

    private bool EdgeSend() => MirrorSnapshot.SetEdgeTool(_edgeBar + 1, (float)_edges[0], (float)_edges[1], (float)_edges[2], (float)_edges[3]);

    private void OnEdgeCancel(object sender, RoutedEventArgs e) => EdgeFinish(cancelled: true);

    private void EdgeFinish(bool cancelled)
    {
        MirrorSnapshot.SetEdgeTool(0, 0, 0, 0, 0);
        _edgeActive = false;
        if (!cancelled)
        {
            _appSettings.VisibleEdges = (double[])_edges.Clone();
            _appSettings.VisibleEdgesEye = _values.GetValueOrDefault("mirror_eye") == "left" ? 0 : 1;
            _appSettings.Save();
            System.Media.SystemSounds.Asterisk.Play();
            CropStatus.Text = "View edges saved. \"Crop to view edges\" fits the box to them.";
            AppLog.Write($"View edges saved: left {_edges[0]:0.###}, right {_edges[1]:0.###}, top {_edges[2]:0.###}, bottom {_edges[3]:0.###}.");
        }
        else CropStatus.Text = "Measuring cancelled.";
        UpdateEdgesUi();
        DrawCrop();
    }

    /// <summary>The saved visible region, dashed, on the crop picture (called from DrawCrop with the picture's placement).</summary>
    private void PlaceEdgeOutline(Rect shown)
    {
        var edges = _edgeActive ? _edges : _appSettings.VisibleEdges;
        if (edges is not { Length: 4 })
        {
            _edgeOutline.Visibility = Visibility.Collapsed;
            return;
        }
        _edgeOutline.Visibility = Visibility.Visible;
        Canvas.SetLeft(_edgeOutline, shown.Left + edges[0] * shown.Width);
        Canvas.SetTop(_edgeOutline, shown.Top + edges[2] * shown.Height);
        _edgeOutline.Width = Math.Max(1, (edges[1] - edges[0]) * shown.Width);
        _edgeOutline.Height = Math.Max(1, (edges[3] - edges[2]) * shown.Height);
    }
}
