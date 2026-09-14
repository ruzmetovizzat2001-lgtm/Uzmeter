using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;

namespace UzMeter;

public partial class App : Application
{
    public static Settings Settings { get; } = Settings.Load();

    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        // Saqlangan til / mavzu / ovoz — oynalar yaratilishidan oldin
        I18n.SetLanguage(Settings.Lang);
        RequestedThemeVariant = Settings.Theme == "dark" ? ThemeVariant.Dark : ThemeVariant.Light;
        Sound.Enabled = Settings.Sound;

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var main = new MainWindow();
            desktop.MainWindow = main;
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;

            // Avval splash; asosiy oyna faqat animatsiya to'liq tugab, splash yopilgach ochiladi
            var splash = new SplashWindow();
            splash.Closed += (_, _) => { main.Show(); main.Activate(); main.OnShownAfterSplash(); };
            splash.Show();
        }
        base.OnFrameworkInitializationCompleted();
    }
}
