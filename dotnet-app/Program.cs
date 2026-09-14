using Avalonia;

namespace UzMeter;

static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        // `UzMeter --selftest` — protokolni GUI siz tekshirish (CI uchun)
        if (args.Contains("--selftest")) return SelfTest.Run();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
