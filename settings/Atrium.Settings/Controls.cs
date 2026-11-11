using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace Atrium.Settings;

// Design tokens from App.xaml, in the app's current theme.
public static class Tokens
{
    public static Microsoft.UI.Xaml.Media.Brush Brush(string key)
    {
        var res = Application.Current.Resources;
        if (res.TryGetValue(key, out var v) && v is Microsoft.UI.Xaml.Media.Brush b) return b;
        var theme = Application.Current.RequestedTheme == ApplicationTheme.Dark ? "Dark" : "Light";
        if (res.ThemeDictionaries.TryGetValue(theme, out var d) && d is ResourceDictionary rd && rd.TryGetValue(key, out var t)
            && t is Microsoft.UI.Xaml.Media.Brush tb) return tb;
        return new Microsoft.UI.Xaml.Media.SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }
}

// A sidebar entry: a glyph and a name, in the item's ink (white when chosen).
public sealed class NavLabel : StackPanel
{
    private readonly FontIcon _icon = new() { FontSize = 16, Width = 22 };
    private readonly TextBlock _text = new() { FontSize = 15, VerticalAlignment = VerticalAlignment.Center };

    public NavLabel()
    {
        Orientation = Orientation.Horizontal;
        Spacing = 12;
        Children.Add(_icon);
        Children.Add(_text);
    }

    public string Glyph { get => _icon.Glyph; set => _icon.Glyph = value; }
    public string Text { get => _text.Text; set => _text.Text = value; }

    // A part of the item above it (the bar's start menu, tray...): indented, a size smaller.
    public bool Sub
    {
        get => Margin.Left > 0;
        set
        {
            Margin = new Thickness(value ? 22 : 0, 0, 0, 0);
            _icon.FontSize = value ? 14 : 16;
            _text.FontSize = value ? 14 : 15;
        }
    }
}

// A named switch; supporting help is available on hover.
public sealed class ToggleTile : UserControl
{
    private readonly TextBlock _title = new() { Style = Res<Style>("RowTitleStyle"), VerticalAlignment = VerticalAlignment.Center };

    public ToggleSwitch Switch { get; } = new() { Style = Res<Style>("SwitchStyle") };
    public event RoutedEventHandler? Toggled;

    public ToggleTile()
    {
        IsTabStop = false;
        var grid = new Grid { ColumnSpacing = 12, MinHeight = 32 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(Switch, 1);
        grid.Children.Add(_title);
        grid.Children.Add(Switch);
        Content = _surface = new Border { Style = Res<Style>("TileStyle"), Child = grid };
        Switch.Toggled += (_, e) => Toggled?.Invoke(this, e);
    }

    private readonly Border _surface;

    // In the inspector: a plain row, no card.
    public bool Flat
    {
        get => _surface.Style == Res<Style>("FlatTileStyle");
        set => _surface.Style = Res<Style>(value ? "FlatTileStyle" : "TileStyle");
    }

    public string Title { get => _title.Text; set => _title.Text = value; }
    public string Note { get => ToolTipService.GetToolTip(Switch)?.ToString() ?? ""; set => ToolTipService.SetToolTip(Switch, value); }
    // The switch's name for UI Automation (and screen readers).
    public string SwitchName { get => AutomationProperties.GetName(Switch); set => AutomationProperties.SetName(Switch, value); }
    public bool IsOn { get => Switch.IsOn; set => Switch.IsOn = value; }

    private static T Res<T>(string key) => (T)Application.Current.Resources[key];
}

// A wrapping grid of equal-width cells: as many columns as fit MinCellWidth, each row as tall as
// its tallest card. With StretchLastRow a short last row of two or more shares the full width. Unlike a virtualising ItemsRepeater every
// card stays in the visual tree, so UI Automation can always find its controls.
public sealed class ResponsiveGrid : Panel
{
    public static readonly DependencyProperty MinCellWidthProperty =
        DependencyProperty.Register(nameof(MinCellWidth), typeof(double), typeof(ResponsiveGrid), new PropertyMetadata(320.0, (d, _) => ((ResponsiveGrid)d).InvalidateMeasure()));
    public static readonly DependencyProperty GapProperty =
        DependencyProperty.Register(nameof(Gap), typeof(double), typeof(ResponsiveGrid), new PropertyMetadata(16.0, (d, _) => ((ResponsiveGrid)d).InvalidateMeasure()));

    public double MinCellWidth { get => (double)GetValue(MinCellWidthProperty); set => SetValue(MinCellWidthProperty, value); }
    public double Gap { get => (double)GetValue(GapProperty); set => SetValue(GapProperty, value); }
    public bool StretchLastRow { get; set; }
    public int MaxColumns { get; set; } = int.MaxValue;
    public bool DistributeRows { get; set; }

    // The width of each cell in the row that starts at `start` of `count` visible children.
    private double CellWidth(double width, int cols, int start, int count)
    {
        int inRow = Math.Min(cols, count - start);
        int n = StretchLastRow && inRow >= 2 ? inRow : cols;
        return (width - (n - 1) * Gap) / n;
    }

    private int Columns(double width)
    {
        if (double.IsInfinity(width) || width <= 0) return 1;
        return Math.Max(1, Math.Min(MaxColumns, (int)Math.Floor((width + Gap) / (MinCellWidth + Gap))));
    }

    protected override Size MeasureOverride(Size available)
    {
        int cols = Columns(available.Width);
        var visible = Children.Where(c => c.Visibility != Visibility.Collapsed).ToList();
        double total = 0;
        for (int start = 0; start < visible.Count; start += cols)
        {
            double cell = double.IsInfinity(available.Width) ? MinCellWidth : CellWidth(available.Width, cols, start, visible.Count);
            double rowH = 0;
            for (int i = start; i < Math.Min(start + cols, visible.Count); i++)
            {
                visible[i].Measure(new Size(cell, double.PositiveInfinity));
                rowH = Math.Max(rowH, visible[i].DesiredSize.Height);
            }
            total += rowH + Gap;
        }
        if (visible.Count > 0) total -= Gap;
        return new Size(double.IsInfinity(available.Width) ? cols * MinCellWidth + (cols - 1) * Gap : available.Width, total);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        int cols = Columns(finalSize.Width);
        var visible = Children.Where(c => c.Visibility != Visibility.Collapsed).ToList();
        int rows = (visible.Count + cols - 1) / cols;
        // Share surplus height between rows, keeping the first and last edges aligned.
        double gap = Gap + (DistributeRows && rows > 1 ? Math.Max(0, finalSize.Height - DesiredSize.Height) / (rows - 1) : 0);
        double y = 0;
        for (int start = 0; start < visible.Count; start += cols)
        {
            int end = Math.Min(start + cols, visible.Count);
            double cell = CellWidth(finalSize.Width, cols, start, visible.Count);
            double rowH = 0;
            for (int i = start; i < end; i++) rowH = Math.Max(rowH, visible[i].DesiredSize.Height);
            for (int i = start; i < end; i++)
                visible[i].Arrange(new Rect((i - start) * (cell + Gap), y, cell, rowH));
            y += rowH + gap;
        }
        return finalSize;
    }
}
