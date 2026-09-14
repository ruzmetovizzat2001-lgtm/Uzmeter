using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;

namespace UzMeter;

/// <summary>
/// Kirish animatsiyasi: tomchi tushadi → to'lqinlar → "UZMETER" yozuvi ochiladi → shior.
/// Hamma narsa bitta taymer bilan boshqariladi (t soniya), shuning uchun har platformada bir xil.
/// </summary>
public sealed class SplashWindow : Window
{
    const double Total = 3.0;      // animatsiya davomiyligi, s
    const double FadeOut = 0.45;   // yakuniy so'nish, s
    const double LogoW = 520;      // logotip kengligi
    const double LogoH = LogoW * 414 / 1782;

    readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(15) };
    DateTime _t0 = DateTime.UtcNow;
    readonly Grid _root;
    readonly Image _mark;
    readonly TranslateTransform _markMove = new();
    readonly ScaleTransform _markScale = new();
    readonly Border _wordClip;
    readonly TranslateTransform _wordMove = new();
    readonly Ellipse[] _ripples = new Ellipse[3];
    readonly ScaleTransform[] _rippleScale = new ScaleTransform[3];
    readonly TextBlock _tag;
    readonly Border _bar;
    readonly Ellipse _glow;
    bool _soundPlayed, _done;

    public event Action? Completed;

    public SplashWindow()
    {
        Width = 800; Height = 460;
        SystemDecorations = SystemDecorations.None;
        CanResize = false;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;
        ShowInTaskbar = false;
        Topmost = true;
        Background = Brushes.Transparent;
        TransparencyLevelHint = [WindowTransparencyLevel.Transparent];

        var bg = new Border
        {
            CornerRadius = new CornerRadius(18),
            ClipToBounds = true,
            Background = new RadialGradientBrush
            {
                Center = new RelativePoint(0.5, 0.6, RelativeUnit.Relative),
                GradientOrigin = new RelativePoint(0.5, 0.6, RelativeUnit.Relative),
                RadiusX = new RelativeScalar(0.9, RelativeUnit.Relative),
                RadiusY = new RelativeScalar(0.9, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Color.Parse("#0F1A33"), 0), new GradientStop(Color.Parse("#0A1020"), 0.6), new GradientStop(Color.Parse("#070B17"), 1) },
            },
            BorderBrush = new SolidColorBrush(Color.Parse("#22304F")),
            BorderThickness = new Thickness(1),
        };

        _glow = new Ellipse
        {
            Width = 560, Height = 560, Opacity = 0,
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            Fill = new RadialGradientBrush
            {
                GradientStops = { new GradientStop(Color.Parse("#4700AEEF"), 0), new GradientStop(Color.Parse("#0000AEEF"), 0.65) },
            },
        };

        // ---- logotip ----
        var logo = new Canvas { Width = LogoW, Height = LogoH, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        var markW = LogoW * 297 / 1782;
        _mark = new Image
        {
            Source = Load("mark.png"), Width = markW, Height = LogoH, Stretch = Stretch.Uniform, Opacity = 0,
            RenderTransformOrigin = new RelativePoint(0.5, 1, RelativeUnit.Relative),
            RenderTransform = new TransformGroup { Children = { _markScale, _markMove } },
        };
        Canvas.SetLeft(_mark, 0); Canvas.SetTop(_mark, 0);

        var wordW = LogoW * 1433 / 1782;
        _wordClip = new Border
        {
            Width = 0, Height = LogoH, ClipToBounds = true, HorizontalAlignment = HorizontalAlignment.Left,
            RenderTransform = _wordMove,
            Child = new Image { Source = Load("word.png"), Width = wordW, Height = LogoH, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Left },
        };
        Canvas.SetLeft(_wordClip, LogoW * 349 / 1782); Canvas.SetTop(_wordClip, 0);

        for (var i = 0; i < 3; i++)
        {
            _rippleScale[i] = new ScaleTransform(1, 1);
            _ripples[i] = new Ellipse
            {
                Width = 20, Height = 8, Opacity = 0,
                Stroke = new SolidColorBrush(Color.Parse("#BF00AEEF")), StrokeThickness = 1.5,
                RenderTransformOrigin = RelativePoint.Center, RenderTransform = _rippleScale[i],
            };
            Canvas.SetLeft(_ripples[i], markW / 2 - 10); Canvas.SetTop(_ripples[i], LogoH * 0.96 - 4);
            logo.Children.Add(_ripples[i]);
        }
        logo.Children.Add(_mark);
        logo.Children.Add(_wordClip);

        _tag = new TextBlock
        {
            Text = I18n.T("app_subtitle").ToUpperInvariant(), FontSize = 13, LetterSpacing = 5, Opacity = 0,
            Foreground = new SolidColorBrush(Color.Parse("#9FB4D3")),
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, LogoH + 150, 0, 0),
        };

        _bar = new Border
        {
            Height = 2, Width = 0, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Bottom,
            Background = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(1, 0, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Color.Parse("#00AEEF"), 0), new GradientStop(Color.Parse("#6FD6FF"), 1) },
            },
        };
        var barTrack = new Border { Height = 2, VerticalAlignment = VerticalAlignment.Bottom, Background = new SolidColorBrush(Color.Parse("#14FFFFFF")) };

        var skip = new TextBlock
        {
            Text = I18n.T("splash_skip"), FontSize = 11, Foreground = new SolidColorBrush(Color.Parse("#59FFFFFF")),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 18, 14),
        };

        _root = new Grid { Children = { _glow, logo, _tag, barTrack, _bar, skip } };
        bg.Child = _root;
        Content = bg;

        PointerPressed += (_, _) => Finish();
        KeyDown += (_, _) => Finish();
        Opened += (_, _) => { _t0 = DateTime.UtcNow; _timer.Start(); };
        _timer.Tick += (_, _) => Tick();
    }

    static Bitmap Load(string name) => new(AssetLoader.Open(new Uri($"avares://UzMeter/Assets/{name}")));

    static double Clamp01(double x) => Math.Clamp(x, 0, 1);
    static double OutCubic(double x) => 1 - Math.Pow(1 - Clamp01(x), 3);
    static double OutBack(double x) { x = Clamp01(x); const double c = 1.4; return 1 + (c + 1) * Math.Pow(x - 1, 3) + c * Math.Pow(x - 1, 2); }
    static double InOut(double x) { x = Clamp01(x); return x < .5 ? 4 * x * x * x : 1 - Math.Pow(-2 * x + 2, 3) / 2; }

    void Tick()
    {
        var t = (DateTime.UtcNow - _t0).TotalSeconds;

        // glow
        _glow.Opacity = OutCubic((t - 0.4) / 1.6);

        // tomchi: 0.25 → 1.1 s, yuqoridan tushadi, pastga urilganda biroz yassilanadi
        var m = (t - 0.25) / 0.85;
        _mark.Opacity = Clamp01(m * 2.5);
        var e = OutBack(m);
        _markMove.Y = -46 * (1 - e);
        var squash = m is > 0.55 and < 1.0 ? 0.05 * Math.Sin((m - 0.55) / 0.45 * Math.PI) : 0;
        _markScale.ScaleX = 1 + squash * 0.6;
        _markScale.ScaleY = 1 - squash;

        // to'lqinlar: 0.95 / 1.15 / 1.35 s dan boshlab 1.4 s
        for (var i = 0; i < 3; i++)
        {
            var r = (t - (0.95 + i * 0.2)) / 1.4;
            if (r < 0) { _ripples[i].Opacity = 0; continue; }
            var k = OutCubic(r);
            _ripples[i].Opacity = 0.9 * (1 - k);
            _rippleScale[i].ScaleX = 1 + 13 * k;
            _rippleScale[i].ScaleY = 1 + 9 * k;
        }

        // yozuv: 1.05 → 1.95 s, chapdan o'ngga ochiladi
        var w = InOut((t - 1.05) / 0.9);
        _wordClip.Width = LogoW * 1433 / 1782 * w;
        _wordMove.X = -10 * (1 - w);

        // shior
        var g = OutCubic((t - 1.9) / 1.0);
        _tag.Opacity = g;
        _tag.LetterSpacing = 5 - 2.5 * g;

        // progress
        _bar.Width = Width * InOut(t / (Total - 0.3));

        if (!_soundPlayed && t >= 0.85) { _soundPlayed = true; Sound.PlayDrop(); }

        if (_done)
        {
            // yakuniy so'nish, keyin yopiladi
            _root.Opacity = 1 - Clamp01((t - _fadeStart) / FadeOut);
            if (t - _fadeStart >= FadeOut) { _timer.Stop(); Close(); }
            return;
        }
        if (t >= Total) Finish();
    }

    double _fadeStart;

    void Finish()
    {
        if (_done) return;
        _done = true;
        _fadeStart = (DateTime.UtcNow - _t0).TotalSeconds;
        Completed?.Invoke();
    }
}
