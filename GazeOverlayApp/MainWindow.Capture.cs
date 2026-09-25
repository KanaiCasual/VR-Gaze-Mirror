using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace GazeOverlay;

/// <summary>
/// The crop tool's reference picture, taken the way it is needed: from inside the headset.
/// "Capture from headset" arms a one-shot: the user puts the headset on, sits as they play and presses the capture key.
/// That takes ONE picture of each eye, and the mode is over. Such a picture is kept: the automatic refreshes of the tab
/// leave it alone until "Refresh picture" is pressed.
/// The key is watched by the mirror LAYER, not by this app: the layer runs inside the game, and a game that has the focus
/// can keep system hot keys from every other program (DCS does). The app only says "armed, this key" in shared memory;
/// the layer looks at the key while that is set, takes the picture and disarms. It changes nothing in the headset.
/// </summary>
public partial class MainWindow
{
    private bool _captureArmed;
    private bool _captureKeyListening;
    private int? _captureGenerationBefore;
    private int _captureTicksSinceKey;
    private DateTime? _captureDeadline; // Countdown mode: when the picture is taken without the key.
    private bool _captureFiring;
    // While armed: says "somebody is reading the mirror picture" (so the capture also works with OBS and the mirror window
    // closed) and looks whether the layer has delivered. It runs only between arming and the picture.
    private readonly DispatcherTimer _captureTimer = new() { Interval = TimeSpan.FromMilliseconds(40) };

    private BitmapSource? _pictureFirst, _pictureOther;
    private readonly Line[] _cropTravelLines = [new(), new()];
    private readonly TextBlock[] _cropTravelLabels = [new(), new()];

    private static string OtherPicturePath => System.IO.Path.Combine(AppSettings.Folder, "last-mirror-picture-other.png");

    private void BuildCaptureUi()
    {
        var travel = new SolidColorBrush(Color.FromArgb(230, 120, 200, 255));
        for (var i = 0; i < 2; i++)
        {
            _cropTravelLines[i].Stroke = travel;
            _cropTravelLines[i].StrokeThickness = 1.5;
            _cropTravelLines[i].StrokeDashArray = [2, 3];
            _cropTravelLines[i].IsHitTestVisible = false;
            CropCanvas.Children.Add(_cropTravelLines[i]);
            _cropTravelLabels[i].Foreground = travel;
            _cropTravelLabels[i].Background = new SolidColorBrush(Color.FromArgb(170, 0, 0, 0));
            _cropTravelLabels[i].FontSize = 11;
            _cropTravelLabels[i].Padding = new Thickness(4, 0, 4, 1);
            _cropTravelLabels[i].IsHitTestVisible = false;
            CropCanvas.Children.Add(_cropTravelLabels[i]);
        }
        _cropTravelLabels[0].Text = "frame ends here when you look all the way up";
        _cropTravelLabels[1].Text = "frame starts here when you look all the way down";

        CropPictureEye.Items.Add(new ComboBoxItem { Content = "Left eye picture", Tag = 0 });
        CropPictureEye.Items.Add(new ComboBoxItem { Content = "Right eye picture", Tag = 1 });
        CropPictureEye.SelectionChanged += (_, _) =>
        {
            if (_loading || CropPictureEye.SelectedItem is not ComboBoxItem { Tag: int eye }) return;
            _appSettings.MirrorPictureShowOther = eye == _appSettings.MirrorPictureOtherEye && eye != _appSettings.MirrorPictureEye;
            _appSettings.Save();
            ShowSelectedPicture();
        };

        // The countdown choice: the key only, or the picture by itself after so many seconds.
        foreach (var seconds in new[] { 0, 5, 10, 15, 20, 30 })
        {
            CropCaptureTimer.Items.Add(new ComboBoxItem { Content = seconds == 0 ? "By key only" : $"By itself after {seconds} s", Tag = seconds });
        }
        CropCaptureTimer.SelectedItem = CropCaptureTimer.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (int)i.Tag == _appSettings.CaptureTimerSeconds) ?? CropCaptureTimer.Items[0];
        CropCaptureTimer.SelectionChanged += (_, _) =>
        {
            if (CropCaptureTimer.SelectedItem is not ComboBoxItem { Tag: int seconds }) return;
            _appSettings.CaptureTimerSeconds = seconds;
            _appSettings.Save();
            UpdateCaptureUi();
        };

        UpdateCaptureUi();
        _captureTimer.Tick += OnCaptureTick;
        PreviewKeyDown += OnCaptureKeyChosen;
        Closed += (_, _) => DisarmCapture();
    }

    // ------------------------------------------------------------------ the pictures

    private void LoadLastCropPicture()
    {
        static BitmapSource? Load(string path)
        {
            try
            {
                if (!File.Exists(path)) return null;
                var image = new BitmapImage();
                image.BeginInit();
                image.CacheOption = BitmapCacheOption.OnLoad; // Do not keep the file open: it is replaced by the next picture.
                image.UriSource = new Uri(path);
                image.EndInit();
                image.Freeze();
                return image;
            }
            catch
            {
                return null; // No picture is fine: the box still works on an empty frame.
            }
        }

        if (_appSettings.MirrorWidth < 1 || _appSettings.MirrorHeight < 1) return;
        _pictureFirst = Load(LastPicturePath);
        if (_pictureFirst == null) return;
        _pictureOther = _appSettings.MirrorPictureOtherEye is 0 or 1 ? Load(OtherPicturePath) : null;
        _cropFullWidth = _appSettings.MirrorWidth;
        _cropFullHeight = _appSettings.MirrorHeight;
        _crop.ImageAspect = (double)_cropFullWidth / _cropFullHeight;
        CropStatus.Text = _appSettings.MirrorPictureKept
            ? "Showing the picture you captured from the headset. It is kept until you press Refresh picture."
            : "Showing the last picture that was taken. Refresh picture gets a new one from the running game.";
        ShowSelectedPicture();
    }

    /// <summary>A new picture from the game: show it and keep it for next time.</summary>
    private void ShowNewPicture(MirrorPicture picture, bool kept)
    {
        _cropPicture = picture;
        _pictureFirst = picture.Image;
        _pictureOther = picture.OtherImage;
        _cropFullWidth = picture.FullWidth;
        _cropFullHeight = picture.FullHeight;
        _crop.ImageAspect = (double)picture.FullWidth / picture.FullHeight;
        _crop.Normalize();

        _appSettings.MirrorWidth = picture.FullWidth;
        _appSettings.MirrorHeight = picture.FullHeight;
        _appSettings.MirrorPictureEye = picture.Eye;
        _appSettings.MirrorPictureOtherEye = picture.OtherEye;
        _appSettings.MirrorPictureKept = kept;
        // Keep looking at the same eye as before, if there is a picture of it.
        var wanted = CropPictureEye.SelectedItem is ComboBoxItem { Tag: int eye } ? eye : picture.Eye;
        _appSettings.MirrorPictureShowOther = picture.OtherImage != null && wanted == picture.OtherEye;
        ShowSelectedPicture();

        try
        {
            Directory.CreateDirectory(AppSettings.Folder);
            static void Save(BitmapSource image, string path)
            {
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(image));
                using var file = File.Create(path);
                encoder.Save(file);
            }
            Save(picture.Image, LastPicturePath);
            if (picture.OtherImage != null) Save(picture.OtherImage, OtherPicturePath);
            _appSettings.Save();
        }
        catch
        {
            // Not being able to keep the picture for next time is not worth bothering anyone about.
        }
    }

    private void ShowSelectedPicture()
    {
        var other = _appSettings.MirrorPictureShowOther && _pictureOther != null;
        _cropImage.Source = other ? _pictureOther : _pictureFirst;

        var both = _pictureOther != null && _appSettings.MirrorPictureEye is 0 or 1 && _appSettings.MirrorPictureOtherEye is 0 or 1;
        _loading = true;
        CropPictureEye.Visibility = both ? Visibility.Visible : Visibility.Collapsed;
        CropPictureEye.SelectedIndex = both ? (other ? _appSettings.MirrorPictureOtherEye : _appSettings.MirrorPictureEye) : -1;
        _loading = false;
        DrawCrop();
    }

    /// <summary>Called by DrawCrop: the travel markers inside the box.</summary>
    private void DrawCaptureOverlays(Rect box, double reachTop, double reachBottom, bool following)
    {
        // Looking all the way up moves the frame up by the upper reach: its bottom edge then sits that much higher, and
        // what is below that line drops out of the frame. The same the other way round for looking down.
        var edges = new[] { box.Bottom - (box.Top - reachTop), box.Top + (reachBottom - box.Bottom) };
        for (var i = 0; i < 2; i++)
        {
            var y = Math.Clamp(edges[i], box.Top, box.Bottom);
            var moved = Math.Abs(y - (i == 0 ? box.Bottom : box.Top)) >= 1.5;
            var visibility = following && moved ? Visibility.Visible : Visibility.Collapsed;
            (_cropTravelLines[i].X1, _cropTravelLines[i].Y1, _cropTravelLines[i].X2, _cropTravelLines[i].Y2) = (box.Left, y, box.Right, y);
            _cropTravelLines[i].Visibility = visibility;
            _cropTravelLabels[i].Visibility = box.Width >= 300 ? visibility : Visibility.Collapsed;
            Canvas.SetLeft(_cropTravelLabels[i], box.Left + 6);
            Canvas.SetTop(_cropTravelLabels[i], i == 0 ? y - 17 : y + 2); // above the "ends here" line, below the "starts here" one
        }
    }

    // ------------------------------------------------------------------ capture from the headset

    private static string KeyName(int virtualKey) => KeyInterop.KeyFromVirtualKey(virtualKey) is var key && key != Key.None ? key.ToString() : $"key {virtualKey}";

    private void UpdateCaptureUi()
    {
        var key = KeyName(_appSettings.CaptureKey);
        CropCaptureKey.Content = _captureKeyListening ? "Press a key..." : "Key: " + key;
        CropCaptureKey.IsEnabled = !_captureArmed;
        CropCaptureTimer.IsEnabled = !_captureArmed;
        CropCapture.IsEnabled = !_captureArmed;
        CaptureBanner.Visibility = _captureArmed ? Visibility.Visible : Visibility.Collapsed;
        CaptureBannerKey.Text = key;
        if (_captureDeadline is { } deadline)
        {
            var left = Math.Max(0, (int)Math.Ceiling((deadline - DateTime.UtcNow).TotalSeconds));
            CaptureBannerTitle.Text = $"CAPTURE ARMED - picture in {left} s (or press {key})";
            CaptureBannerTail.Text = $", or just wait: the picture is taken by itself in {left} s.  A sound tells you it was taken; this then switches itself off.";
        }
        else
        {
            CaptureBannerTitle.Text = "CAPTURE ARMED - waiting for the key";
            CaptureBannerTail.Text = ".  A sound tells you the picture was taken; this then switches itself off.";
        }
        CropFrameBorder.BorderBrush = _captureArmed ? (Brush)new SolidColorBrush(Color.FromRgb(0xE0, 0x9A, 0x2B)) : (Brush)FindResource("Line");
    }

    private void OnCaptureKeyClick(object sender, RoutedEventArgs e)
    {
        _captureKeyListening = !_captureKeyListening;
        UpdateCaptureUi();
    }

    private void OnCaptureKeyChosen(object sender, KeyEventArgs e)
    {
        if (!_captureKeyListening) return;
        e.Handled = true;
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        if (key is Key.LeftCtrl or Key.RightCtrl or Key.LeftShift or Key.RightShift or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin) return;
        _captureKeyListening = false;
        if (key != Key.Escape && KeyInterop.VirtualKeyFromKey(key) is > 0 and < 256 and var virtualKey)
        {
            _appSettings.CaptureKey = virtualKey;
            _appSettings.Save();
        }
        UpdateCaptureUi();
    }

    private void OnCaptureArm(object sender, RoutedEventArgs e)
    {
        _captureKeyListening = false;
        _captureGenerationBefore = MirrorSnapshot.ReadGeneration();
        if (!MirrorSnapshot.ArmCaptureKey(_appSettings.CaptureKey))
        {
            CropStatus.Text = "No game is running (or it still runs an older layer): start the game first, then arm the capture.";
            UpdateCaptureUi();
            return;
        }
        _captureArmed = true;
        _captureTicksSinceKey = 0;
        _captureFiring = false;
        _captureDeadline = _appSettings.CaptureTimerSeconds > 0 ? DateTime.UtcNow.AddSeconds(_appSettings.CaptureTimerSeconds) : null;
        _captureTimer.Start();
        UpdateCaptureUi();
        CropStatus.Text = _captureDeadline != null ? $"Capture armed: the picture is taken in {_appSettings.CaptureTimerSeconds} s, or when you press the key." : "Capture armed.";
        AppLog.Write(_captureDeadline != null ? $"Capture from headset armed, countdown {_appSettings.CaptureTimerSeconds} s." : "Capture from headset armed.");
    }

    /// <summary>The countdown ran out: the picture is asked for the way Refresh picture asks, and kept like a key capture.</summary>
    private async void FireCaptureCountdown()
    {
        _captureFiring = true;
        _captureTimer.Stop();
        MirrorSnapshot.ArmCaptureKey(0); // The key is no longer wanted; the picture comes from the request below.
        var (picture, message) = await MirrorSnapshot.RequestAsync();
        _captureArmed = false;
        _captureDeadline = null;
        _captureFiring = false;
        UpdateCaptureUi();
        if (picture == null)
        {
            System.Media.SystemSounds.Hand.Play();
            CropStatus.Text = "The countdown ran out, but no picture came: " + message;
            AppLog.Write("Capture countdown: no picture (" + message + ").");
            return;
        }
        System.Media.SystemSounds.Asterisk.Play();
        ShowNewPicture(picture, kept: true);
        CropStatus.Text = $"Captured from the headset at {DateTime.Now:T} by the countdown" + (picture.OtherImage != null ? " (both eyes)" : "") + ". Kept until you press Refresh picture.";
        AppLog.Write("Captured the crop picture from the headset by the countdown" + (picture.OtherImage != null ? " (both eyes)." : "."));
    }

    private void OnCaptureCancel(object sender, RoutedEventArgs e)
    {
        DisarmCapture();
        CropStatus.Text = "Capture cancelled.";
    }

    private void DisarmCapture()
    {
        _captureTimer.Stop();
        if (_captureArmed) MirrorSnapshot.ArmCaptureKey(0);
        _captureArmed = false;
        _captureDeadline = null;
        UpdateCaptureUi();
    }

    private void OnCaptureTick(object? sender, EventArgs e)
    {
        if (_captureFiring) return;
        if (_captureDeadline is { } deadline)
        {
            UpdateCaptureUi(); // The seconds left, on the banner.
            if (DateTime.UtcNow >= deadline)
            {
                FireCaptureCountdown();
                return;
            }
        }
        if (MirrorSnapshot.ReadGeneration() is { } generation && generation != _captureGenerationBefore)
        {
            // The layer has delivered.
            MirrorPicture? picture = null;
            try { picture = MirrorSnapshot.Read(); } catch { /* reported below */ }
            DisarmCapture();
            if (picture == null)
            {
                System.Media.SystemSounds.Hand.Play();
                CropStatus.Text = "The key was seen, but the picture could not be read.";
                return;
            }
            System.Media.SystemSounds.Asterisk.Play();
            ShowNewPicture(picture, kept: true);
            CropStatus.Text = $"Captured from the headset at {DateTime.Now:T}" + (picture.OtherImage != null ? " (both eyes)" : "") + ". Kept until you press Refresh picture.";
            AppLog.Write("Captured the crop picture from the headset" + (picture.OtherImage != null ? " (both eyes)." : "."));
            return;
        }

        // The layer takes the key back when it has seen it. No picture two seconds later (or no game any more): give up.
        var stillArmed = MirrorSnapshot.IsCaptureKeyArmed();
        if (stillArmed == true) return;
        if (stillArmed == null || ++_captureTicksSinceKey > 50)
        {
            DisarmCapture();
            System.Media.SystemSounds.Hand.Play();
            CropStatus.Text = stillArmed == null ? "The game was closed while the capture was armed." : "The key was seen, but the game made no picture.";
        }
    }
}
