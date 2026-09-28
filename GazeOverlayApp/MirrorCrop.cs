using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace GazeOverlay;

/// <summary>A picture of the whole mirror image, as the layer made it, and what it says about the image and the box in effect.</summary>
public sealed record MirrorPicture(BitmapSource Image, int FullWidth, int FullHeight, int CropX, int CropY, int CropWidth, int CropHeight)
{
    /// <summary>The eye <see cref="Image"/> shows: 0 = left, 1 = right, 2 = both blended, -1 = not known (older layer).</summary>
    public int Eye { get; init; } = -1;
    /// <summary>Where straight ahead lands in the picture's eye image (fractions), NaN when the game did not say.</summary>
    public double AxisU { get; init; } = double.NaN;
    public double AxisV { get; init; } = double.NaN;
    /// <summary>Where the ring was in the picture's eye image when it was taken (fractions) and its radius (of the image height); NaN when there was none.</summary>
    public double GazeU { get; init; } = double.NaN;
    public double GazeV { get; init; } = double.NaN;
    public double GazeRadius { get; init; }
    /// <summary>The other eye, taken one frame later (newer layers), and which eye that is.</summary>
    public BitmapSource? OtherImage { get; init; }
    public int OtherEye { get; init; } = -1;
}

/// <summary>
/// Asks the running game for ONE small picture of the mirror image (for the crop tool) and reads it from shared memory.
/// Nothing polls: the layer only makes a picture when the request counter in the settings-signal block changes, and this
/// side only waits - briefly, and only right after the user asked - for the answer.
/// Layout: MirrorSnapshot and SettingsSignal in eye_gaze_export.h.
/// </summary>
public static class MirrorSnapshot
{
    private const string SignalName = "GazeOverlay.SettingsSignal";
    public const string BlockName = "GazeOverlay.MirrorSnapshot";
    public const uint BlockMagic = 0x4E534F47; // 'GOSN'
    public const int PixelsOffset = 64;
    public const int MaxSize = 640;
    public const int SecondPixelsOffset = PixelsOffset + MaxSize * MaxSize * 4;
    public const int BlockSize = SecondPixelsOffset + MaxSize * MaxSize * 4;

    /// <summary>
    /// Arms "capture with key" in the running game: the LAYER then watches that key (virtual-key code) and takes one picture
    /// when it is pressed - it runs inside the game, where the game's hold on the keyboard does not matter. 0 disarms.
    /// False = no game, or a layer that cannot do it.
    /// </summary>
    public static bool ArmCaptureKey(int virtualKey)
    {
        try
        {
            using var mapping = MemoryMappedFile.OpenExisting(SignalName, MemoryMappedFileRights.ReadWrite);
            using var view = mapping.CreateViewAccessor(0, 20, MemoryMappedFileAccess.ReadWrite);
            if (view.ReadUInt32(0) != 0x53534F47u || view.ReadUInt32(4) < 3) return false;
            view.Write(16, virtualKey);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// The seconds left on the capture countdown, for the game to show in the headset (0 = none). Layers before signal
    /// version 4 have no such field and simply show nothing.
    /// </summary>
    public static void SetCountdown(int seconds)
    {
        try
        {
            using var mapping = MemoryMappedFile.OpenExisting(SignalName, MemoryMappedFileRights.ReadWrite);
            using var view = mapping.CreateViewAccessor(0, 24, MemoryMappedFileAccess.ReadWrite);
            if (view.ReadUInt32(0) != 0x53534F47u || view.ReadUInt32(4) < 4) return;
            view.Write(20, seconds);
        }
        catch
        {
            // No game: nothing to show it in.
        }
    }

    /// <summary>
    /// The visible-edge finder's state for the game to draw: the bar being set (1 left, 2 right, 3 top, 4 bottom; 0 = off)
    /// and the four bars as fractions of the mirrored eye's image. False = no game, or a layer too old to draw them.
    /// </summary>
    public static bool SetEdgeTool(int bar, float left, float right, float top, float bottom)
    {
        try
        {
            using var mapping = MemoryMappedFile.OpenExisting(SignalName, MemoryMappedFileRights.ReadWrite);
            using var view = mapping.CreateViewAccessor(0, 44, MemoryMappedFileAccess.ReadWrite);
            if (view.ReadUInt32(0) != 0x53534F47u || view.ReadUInt32(4) < 5) return false;
            view.Write(28, left);
            view.Write(32, right);
            view.Write(36, top);
            view.Write(40, bottom);
            view.Write(24, bar);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>True while the layer is still waiting for the key, false once it has taken it back, null = no game any more.</summary>
    public static bool? IsCaptureKeyArmed()
    {
        try
        {
            using var mapping = MemoryMappedFile.OpenExisting(SignalName, MemoryMappedFileRights.Read);
            using var view = mapping.CreateViewAccessor(0, 20, MemoryMappedFileAccess.Read);
            return view.ReadInt32(16) != 0;
        }
        catch
        {
            return null;
        }
    }

    public static async Task<(MirrorPicture? Picture, string Message)> RequestAsync()
    {
        try
        {
            var before = ReadGeneration();
            using (var mapping = MemoryMappedFile.OpenExisting(SignalName, MemoryMappedFileRights.ReadWrite))
            using (var view = mapping.CreateViewAccessor(0, 16, MemoryMappedFileAccess.ReadWrite))
            {
                if (view.ReadUInt32(0) != 0x53534F47u) return (null, "No game is running.");
                if (view.ReadUInt32(4) < 2) return (null, "The game is running an older OBS Mirror layer that cannot make pictures. Restart the game after updating.");
                view.Write(12, view.ReadInt32(12) + 1);
            }

            // The layer answers on its next frame - if OBS is showing the mirror source (it does nothing otherwise).
            for (var attempt = 0; attempt < 40; attempt++)
            {
                await Task.Delay(50);
                if (ReadGeneration() is { } now && now != before) return (Read(), "");
            }
            return (null, "The game did not answer. Is it showing a picture in the headset?");
        }
        catch (FileNotFoundException)
        {
            return (null, "No game is running.");
        }
        catch (Exception e)
        {
            return (null, "Could not get a picture: " + e.Message);
        }
    }

    public static int? ReadGeneration()
    {
        try
        {
            using var mapping = MemoryMappedFile.OpenExisting(BlockName, MemoryMappedFileRights.Read);
            using var view = mapping.CreateViewAccessor(0, PixelsOffset, MemoryMappedFileAccess.Read);
            return view.ReadUInt32(0) == BlockMagic ? view.ReadInt32(8) : null;
        }
        catch (FileNotFoundException)
        {
            return null;
        }
    }

    public static MirrorPicture? Read()
    {
        using var mapping = MemoryMappedFile.OpenExisting(BlockName, MemoryMappedFileRights.Read);
        using var header = mapping.CreateViewAccessor(0, PixelsOffset, MemoryMappedFileAccess.Read);
        if (header.ReadUInt32(0) != BlockMagic) return null;
        int width = (int)header.ReadUInt32(12), height = (int)header.ReadUInt32(16);
        if (width < 1 || height < 1 || width > 640 || height > 640) return null;

        var pixels = new byte[width * height * 4];
        using (var body = mapping.CreateViewAccessor(PixelsOffset, pixels.Length, MemoryMappedFileAccess.Read))
        {
            body.ReadArray(0, pixels, 0, pixels.Length);
        }
        var image = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgr32, null, pixels, width * 4);
        image.Freeze();
        var picture = new MirrorPicture(image, (int)header.ReadUInt32(20), (int)header.ReadUInt32(24),
            (int)header.ReadUInt32(28), (int)header.ReadUInt32(32), (int)header.ReadUInt32(36), (int)header.ReadUInt32(40));
        if (header.ReadUInt32(4) < 2) return picture;
        // Version 3: where straight ahead lands in this eye's image.
        if (header.ReadUInt32(4) >= 3 && header.ReadUInt32(60) != 0)
        {
            picture = picture with { AxisU = header.ReadSingle(52), AxisV = header.ReadSingle(56) };
        }
        // Version 4: the ring's place when the picture was taken, kept after both eyes' pixels.
        if (header.ReadUInt32(4) >= 4)
        {
            using var tail = mapping.CreateViewAccessor(SecondPixelsOffset + 640 * 640 * 4, 16, MemoryMappedFileAccess.Read);
            if (tail.ReadUInt32(8) != 0) picture = picture with { GazeU = tail.ReadSingle(0), GazeV = tail.ReadSingle(4), GazeRadius = tail.ReadSingle(12) };
        }

        // Version 2: which eye it is, and the other eye's picture.
        BitmapSource? other = null;
        var otherEye = (int)header.ReadUInt32(48);
        if (otherEye is 0 or 1)
        {
            var otherPixels = new byte[pixels.Length];
            using var body = mapping.CreateViewAccessor(SecondPixelsOffset, otherPixels.Length, MemoryMappedFileAccess.Read);
            body.ReadArray(0, otherPixels, 0, otherPixels.Length);
            other = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgr32, null, otherPixels, width * 4);
            other.Freeze();
        }
        return picture with { Eye = (int)header.ReadUInt32(44), OtherImage = other, OtherEye = other != null ? otherEye : -1 };
    }
}

/// <summary>
/// The crop box, in fractions of the mirror image (0..1 across and down). With a shape chosen, the height is the free
/// value and the width follows from it; the layer works it out the same way (updateCrop in dx11mirror.cpp).
/// </summary>
public sealed class CropBox
{
    public double CenterX = 0.5, CenterY = 0.5, Height = 0.5, FreeWidth = 1;
    /// <summary>Box width / height in pixels; 0 = free shape.</summary>
    public double Aspect = 16.0 / 9.0;
    /// <summary>Mirror image width / height in pixels (1 for the usual square eye image).</summary>
    public double ImageAspect = 1;
    /// <summary>Share of the image kept free on every side of the box (room for the picture steadying). The box cannot enter it.</summary>
    public double Margin;

    public const double MinSize = 0.04;

    /// <summary>Width and height as fractions of the image, after fitting the box into it.</summary>
    public (double Width, double Height) Size()
    {
        var most = 1 - 2 * Margin;
        var height = Math.Clamp(Height, MinSize, most);
        var width = Aspect > 0 ? height * Aspect / ImageAspect : Math.Clamp(FreeWidth, MinSize, most);
        if (width > most)
        {
            if (Aspect > 0) height *= most / width;
            width = most;
        }
        return (width, height);
    }

    public (double Left, double Top, double Width, double Height) Rect()
    {
        var (width, height) = Size();
        var left = Math.Clamp(CenterX - width / 2, Margin, 1 - Margin - width);
        var top = Math.Clamp(CenterY - height / 2, Margin, 1 - Margin - height);
        return (left, top, width, height);
    }

    /// <summary>Keeps the stored values to what is really shown: size fitted, box inside the image.</summary>
    public void Normalize()
    {
        var (left, top, width, height) = Rect();
        Height = height;
        if (Aspect <= 0) FreeWidth = width;
        CenterX = left + width / 2;
        CenterY = top + height / 2;
    }

    public void MoveTo(double centerX, double centerY)
    {
        CenterX = centerX;
        CenterY = centerY;
        Normalize();
    }

    /// <summary>Scales around the centre (mouse wheel, size slider).</summary>
    public void Scale(double factor)
    {
        Height = Math.Clamp(Height * factor, MinSize, 1);
        if (Aspect <= 0) FreeWidth = Math.Clamp(FreeWidth * factor, MinSize, 1);
        Normalize();
    }

    /// <summary>Dragging a corner: the opposite corner stays where it is, the shape is kept, the box stays inside the image.</summary>
    public void ResizeFromCorner(double anchorX, double anchorY, double pointerX, double pointerY)
    {
        var right = pointerX >= anchorX;
        var down = pointerY >= anchorY;
        var roomX = Math.Max((right ? 1 - anchorX : anchorX) - Margin, 0);
        var roomY = Math.Max((down ? 1 - anchorY : anchorY) - Margin, 0);
        var dx = Math.Min(Math.Abs(pointerX - anchorX), roomX);
        var dy = Math.Min(Math.Abs(pointerY - anchorY), roomY);

        double width, height;
        if (Aspect > 0)
        {
            var widthPerHeight = Aspect / ImageAspect;
            height = Math.Max(dy, dx / widthPerHeight);                       // follow whichever way the pointer went further
            height = Math.Min(height, Math.Min(roomY, roomX / widthPerHeight));
            height = Math.Max(height, MinSize);
            width = height * widthPerHeight;
        }
        else
        {
            width = Math.Max(dx, MinSize);
            height = Math.Max(dy, MinSize);
            FreeWidth = width;
        }
        Height = height;
        CenterX = anchorX + (right ? width : -width) / 2;
        CenterY = anchorY + (down ? height : -height) / 2;
        Normalize();
    }

    /// <summary>"16:9" or a plain number; "0" / "free" = free shape.</summary>
    public static double ParseAspect(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return 16.0 / 9.0;
        var parts = text.Split(':');
        var c = CultureInfo.InvariantCulture;
        if (parts.Length == 2 && double.TryParse(parts[0], NumberStyles.Float, c, out var w) && double.TryParse(parts[1], NumberStyles.Float, c, out var h) && h > 0)
        {
            return w / h;
        }
        return double.TryParse(text, NumberStyles.Float, c, out var ratio) && ratio > 0 ? ratio : 0;
    }
}
