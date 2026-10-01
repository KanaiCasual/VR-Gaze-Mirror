using System.Globalization;
using System.Windows.Data;

namespace GazeOverlay;

/// <summary>Viewport height -> minimum height of the panel inside it: the panel fills the tab when there is room, and scrolls when there is not.</summary>
public sealed class LessMarginConverter : IValueConverter
{
    private const double VerticalMargins = 30; // The Mirror tab's panel has a 16 px top and a 14 px bottom margin.

    public object Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
        value is double height ? Math.Max(height - VerticalMargins, 0) : 0.0;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => Binding.DoNothing;
}
