using System.Globalization;
using System.Text;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Platform.Storage;
using Avalonia.Styling;
using Avalonia.Threading;

namespace UzMeter;

public sealed record LogEntry(DateTime Ts, string Level, string Msg);

public partial class MainWindow : Window
{
    static readonly string[] Views = ["home", "config", "device", "log", "settings", "help"];
    static readonly Dictionary<string, string> LevelNames = new() { ["info"] = "INFO", ["tx"] = "TX", ["rx"] = "RX", ["warn"] = "WARN", ["error"] = "ERROR" };
    static readonly Dictionary<string, string> LevelBrushes = new() { ["info"] = "InfoBrush", ["tx"] = "TxBrush", ["rx"] = "RxBrush", ["warn"] = "WarnBrush", ["error"] = "DangerBrush" };

    readonly Settings _s = App.Settings;
    int _valve = 1;
    int _unit;
    DeviceData? _loaded;
    DateTime? _loadedAt;
    bool _busy;
    bool _suppress; // dasturiy o'zgarishlar paytida hodisalarni to'xtatish
    string _view = "home";
    string _frameTab = "hex";
    byte[]? _lastFrame;
    DateTime? _connSince;
    string[]? _knownPorts;
    readonly List<LogEntry> _logs = [];
    string _logFilter = "all";
    string _logSearch = "";
    string? _statusMsg;
    readonly DispatcherTimer _clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly DispatcherTimer _portPoll = new() { Interval = TimeSpan.FromSeconds(3) };

    public MainWindow()
    {
        InitializeComponent();

        _suppress = true;
        BaudBox.ItemsSource = Protocol.BaudRates.Select(b => b.ToString()).ToArray();
        BaudBox.SelectedIndex = Math.Max(0, Array.IndexOf(Protocol.BaudRates, _s.LastBaud));
        TimeoutBox.ItemsSource = Settings.Timeouts.Select(t => t % 1000 == 0 ? $"{t / 1000} s" : $"{t / 1000.0:0.#} s").ToArray();
        SoundSwitch.IsChecked = _s.Sound;
        AutofillSwitch.IsChecked = _s.Autofill;
        AutoconnectSwitch.IsChecked = _s.Autoconnect;
        TimeoutBox.SelectedIndex = Math.Max(0, Array.IndexOf(Settings.Timeouts, _s.Timeout));
        AutoScroll.IsChecked = _s.AutoScroll;
        _suppress = false;

        I18n.LanguageChanged += ApplyTexts;
        ApplyTexts();
        ApplyThemeUi();
        SyncSegs();
        UpdatePreview();
        RefreshPorts(manual: true);

        _clock.Tick += (_, _) => Clock.Text = DateTime.Now.ToString("dd.MM.yyyy HH:mm");
        _clock.Start();
        Clock.Text = DateTime.Now.ToString("dd.MM.yyyy HH:mm");
        _portPoll.Tick += (_, _) => { if (!_busy) RefreshPorts(manual: false); };
        _portPoll.Start();

        SizeChanged += (_, e) => ApplyCompact(e.NewSize.Width, e.NewSize.Height);
        KeyDown += OnKey;

        // Oynani monitorga sig'adigan qilib cheklaymiz (kichik ekranlar uchun)
        Opened += (_, _) =>
        {
            var wa = Screens.Primary?.WorkingArea;
            if (wa is null) return;
            var scale = Screens.Primary!.Scaling;
            var maxW = wa.Value.Width / scale - 30;
            var maxH = wa.Value.Height / scale - 60;
            if (Width > maxW || Height > maxH)
            {
                Width = Math.Max(MinWidth, Math.Min(Width, maxW));
                Height = Math.Max(MinHeight, Math.Min(Height, maxH));
                Position = new PixelPoint(
                    wa.Value.X + (int)((wa.Value.Width - Width * scale) / 2),
                    wa.Value.Y + (int)((wa.Value.Height - Height * scale) / 2));
            }
            ApplyCompact(Height);
        };
        Closed += (_, _) => { _clock.Stop(); _portPoll.Stop(); SerialService.Disconnect(); };
    }

    /// <summary>Splash yopilib, asosiy oyna ko'rsatilgach chaqiriladi.</summary>
    public void OnShownAfterSplash()
    {
        if (_s.Autoconnect && _s.LastPort.Length > 0 && SelectedPort == _s.LastPort) OnConnect(null, null!);
        DebugScreenshot();
    }

    /// <summary>UZMETER_SHOT=path.png — oynani PNG ga chizib, dasturdan chiqadi (UI tekshiruvi uchun).
    /// UZMETER_VIEW, UZMETER_SIZE=WxH, UZMETER_THEME, UZMETER_LANG, UZMETER_DEMO=1 qo'shimcha.</summary>
    void DebugScreenshot()
    {
        var path = Environment.GetEnvironmentVariable("UZMETER_SHOT");
        if (string.IsNullOrEmpty(path)) return;
        if (Environment.GetEnvironmentVariable("UZMETER_SIZE") is { } sz && sz.Split('x') is [var w, var h]) { Width = double.Parse(w); Height = double.Parse(h); }
        if (Environment.GetEnvironmentVariable("UZMETER_THEME") is { } th) SetTheme(th);
        if (Environment.GetEnvironmentVariable("UZMETER_LANG") is { } lg) SetLang(lg);
        if (Environment.GetEnvironmentVariable("UZMETER_DEMO") == "1")
        {
            SetConfig(new DeviceConfig { DeviceNo = "123456789012345", MNo = "12345678901234", ServerUrl = "https://example.uz/api" });
            _loaded = new DeviceData(GetConfig(), new Telemetry(12458600, 3620, 82)); _loadedAt = DateTime.Now;
            Log("info", "COM3 topildi"); Log("tx", "CMD 0x01 yuborildi: AA 55 01 2E 31 32 33"); Log("rx", "AA 55 81 01 00 12 34");
            Log("info", "Konfiguratsiya muvaffaqiyatli yuborildi"); Log("warn", "Batareya quvvati past: 15%"); Log("error", "Qurilmadan javob kelmadi (timeout)");
            RenderDevice();
        }
        if (Environment.GetEnvironmentVariable("UZMETER_VIEW") is { } v) SetView(v);
        if (Environment.GetEnvironmentVariable("UZMETER_TAB") == "struct") OnFrameTab(TabStruct, null!);
        var t = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1500) };
        t.Tick += (_, _) =>
        {
            t.Stop();
            var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
            using var rtb = new RenderTargetBitmap(size, new Vector(96, 96));
            rtb.Render(this);
            rtb.Save(path);
            Close();
        };
        t.Start();
    }

    /// <summary>Kichik oynalar: balandlik &lt; 720 — ixcham rejim; kenglik &lt; 1180 — tor rejim.</summary>
    void ApplyCompact(double h) => ApplyCompact(Bounds.Width, h);
    void ApplyCompact(double w, double h)
    {
        var compact = h < 720;
        var narrow = w < 1180;
        var home = _view == "home";
        Classes.Set("compact", compact);
        SearchWrap.IsVisible = !(compact && home);
        LAutoScroll.IsVisible = !((compact || narrow) && home);
        PortBox.Width = narrow ? 190 : 250;
        BaudBox.Width = narrow ? 110 : 130;
        ConnectBtn.MinWidth = DisconnectBtn.MinWidth = narrow ? 120 : 150;
        ConnSub.MaxWidth = narrow ? 200 : 280;
        LCopyHex.IsVisible = LRecalc.IsVisible = !narrow;
        LClear.IsVisible = LCopyLog.IsVisible = LSaveLog.IsVisible = !(narrow && home);
        _shortChips = narrow && home;
        RenderLogCounts();
    }
    bool _shortChips;

    /* ---------- til ---------- */

    void ApplyTexts()
    {
        _suppress = true;

        AppSubtitle.Text = I18n.T("app_subtitle");
        Tagline.Text = I18n.T("tagline");
        MFile.Header = I18n.T("menu_file"); MOpen.Header = I18n.T("menu_open"); MSave.Header = I18n.T("menu_save"); MQuit.Header = I18n.T("menu_quit");
        MEdit.Header = I18n.T("menu_edit"); MReset.Header = I18n.T("menu_reset"); MCopyHex.Header = I18n.T("menu_copy_hex"); MClearLog.Header = I18n.T("menu_clear_log");
        MView.Header = I18n.T("menu_view"); MLanguage.Header = I18n.T("menu_language"); MTheme.Header = I18n.T("menu_theme");
        MThemeDark.Header = I18n.T("theme_dark"); MThemeLight.Header = I18n.T("theme_light"); MSound.Header = I18n.T("menu_sound");
        MHelp.Header = I18n.T("menu_help"); MProtocol.Header = I18n.T("menu_protocol"); MAbout.Header = I18n.T("menu_about");

        LNavHome.Text = I18n.T("nav_home"); LNavConfig.Text = I18n.T("nav_config"); LNavDevice.Text = I18n.T("nav_device");
        LNavLog.Text = I18n.T("nav_log"); LNavSettings.Text = I18n.T("nav_settings"); LNavHelp.Text = I18n.T("nav_help");
        ToolTip.SetTip(SettingsBtn, I18n.T("nav_settings")); ToolTip.SetTip(HelpBtn, I18n.T("nav_help"));
        ToolTip.SetTip(ThemeLightBtn, I18n.T("theme_light")); ToolTip.SetTip(ThemeDarkBtn, I18n.T("theme_dark"));

        LPort.Text = I18n.T("port"); LBaud.Text = I18n.T("baud"); LRefresh.Text = I18n.T("refresh");
        LConnect.Text = I18n.T("connect"); LDisconnect.Text = I18n.T("disconnect");

        LConfig.Text = I18n.T("config_title");
        LSec1.Text = I18n.T("sec_ident"); LSec2.Text = I18n.T("sec_server"); LSec3.Text = I18n.T("sec_params"); LSec4.Text = I18n.T("sec_sending"); LSec5.Text = I18n.T("sec_type");
        LDeviceNo.Text = I18n.T("device_no"); LDeviceNoHint.Text = I18n.T("hint_device_no");
        LMNo.Text = I18n.T("m_no"); LMNoHint.Text = I18n.T("hint_m_no");
        LServerUrl.Text = I18n.T("server_url"); LUrlHint.Text = I18n.T("hint_url"); ToolTip.SetTip(UrlInfo, I18n.T("err_url"));
        LValve.Text = I18n.T("valve"); ValveOpenBtn.Content = I18n.T("valve_open"); ValveClosedBtn.Content = I18n.T("valve_closed");
        LUnit.Text = I18n.T("unit");
        LSends.Text = I18n.T("sends"); LMeterType.Text = I18n.T("meter_type");
        LFramePreview.Text = I18n.T("frame_preview"); TabStruct.Content = I18n.T("frame_struct");
        LCopyHex.Text = I18n.T("copy"); LRecalc.Text = I18n.T("recalc"); LFrameNote.Text = I18n.T("frame_note");
        ToolTip.SetTip(CopyHexBtn, I18n.T("copy")); ToolTip.SetTip(RecalcBtn, I18n.T("recalc"));
        LSend.Text = I18n.T("send");

        LDeviceData.Text = I18n.T("device_data"); LRead.Text = I18n.T("read"); LApply.Text = I18n.T("apply_to_form");
        LWater.Text = I18n.T("water_volume"); LBattV.Text = I18n.T("battery_v"); LBattP.Text = I18n.T("battery_pct");
        KDeviceNo.Text = I18n.T("device_no"); KMNo.Text = I18n.T("m_no"); KUrl.Text = I18n.T("server_url"); KType.Text = I18n.T("meter_type");
        KValve.Text = I18n.T("valve"); KUnit.Text = I18n.T("unit"); KSends.Text = I18n.T("sends"); KLast.Text = I18n.T("last_contact"); KState.Text = I18n.T("state");

        LLog.Text = I18n.T("log_title"); LClear.Text = I18n.T("clear"); LCopyLog.Text = I18n.T("copy"); LSaveLog.Text = I18n.T("save_file");
        LogSearch.Watermark = I18n.T("search"); LAutoScroll.Text = I18n.T("autoscroll");

        LSettings.Text = I18n.T("nav_settings");
        LSLang.Text = I18n.T("s_language"); LSLangSub.Text = I18n.T("s_language_sub");
        LSTheme.Text = I18n.T("s_theme"); LSThemeSub.Text = I18n.T("s_theme_sub"); SThemeLight.Content = I18n.T("theme_light"); SThemeDark.Content = I18n.T("theme_dark");
        LSSound.Text = I18n.T("s_sound"); LSSoundSub.Text = I18n.T("s_sound_sub");
        LSAutofill.Text = I18n.T("s_autofill"); LSAutofillSub.Text = I18n.T("s_autofill_sub");
        LSTimeout.Text = I18n.T("s_timeout"); LSTimeoutSub.Text = I18n.T("s_timeout_sub");
        LSAutoconnect.Text = I18n.T("s_autoconnect"); LSAutoconnectSub.Text = I18n.T("s_autoconnect_sub");
        LSReset.Text = I18n.T("s_reset"); LSResetSub.Text = I18n.T("s_reset_sub"); LSResetBtn.Text = I18n.T("s_reset_btn");

        LHelp.Text = I18n.T("nav_help"); AboutText.Text = I18n.T("about_text"); LProtocol.Text = I18n.T("protocol_title"); ProtocolBody.Text = ProtocolText;

        var sendsIdx = SendsBox.SelectedIndex < 0 ? Array.IndexOf(Protocol.SendsPerDayOptions, 6) : SendsBox.SelectedIndex;
        SendsBox.ItemsSource = Protocol.SendsPerDayOptions
            .Select(n => $"{n} {I18n.T("per_day")} — {I18n.T("every_h", ("h", 24.0 / n))}").ToArray();
        SendsBox.SelectedIndex = sendsIdx;

        var typeIdx = Math.Max(0, TypeBox.SelectedIndex);
        TypeBox.ItemsSource = I18n.MeterTypes;
        TypeBox.SelectedIndex = typeIdx;

        foreach (var b in new[] { LangUz, LangRu, LangEn, SLangUz, SLangRu, SLangEn })
            b.Classes.Set("on", (string?)b.Tag == I18n.Lang);

        if (_knownPorts is { Length: 0 }) { PortBox.ItemsSource = new[] { I18n.T("no_ports") }; PortBox.SelectedIndex = 0; } // "Port topilmadi" matnini yangi tilga o'tkazish

        _suppress = false;
        RenderDevice();
        RenderConn();
        RenderStatus();
        RenderLog();
        UpdatePreview();
    }

    void OnLangButton(object? s, RoutedEventArgs e) => SetLang((string)((Button)s!).Tag!);
    void OnLangMenu(object? s, RoutedEventArgs e) => SetLang((string)((MenuItem)s!).Tag!);
    void SetLang(string l) { _s.Lang = l; _s.Save(); I18n.SetLanguage(l); }

    /* ---------- mavzu / ovoz ---------- */

    void SetTheme(string t)
    {
        _s.Theme = t; _s.Save();
        Application.Current!.RequestedThemeVariant = t == "light" ? ThemeVariant.Light : ThemeVariant.Dark;
        ApplyThemeUi();
    }
    void ApplyThemeUi()
    {
        var light = Application.Current!.RequestedThemeVariant != ThemeVariant.Dark;
        ThemeLightBtn.Classes.Set("on", light); ThemeDarkBtn.Classes.Set("on", !light);
        SThemeLight.Classes.Set("on", light); SThemeDark.Classes.Set("on", !light);
        AboutLogo.Source = new Bitmap(AssetLoader.Open(new Uri($"avares://UzMeter/Assets/{(light ? "logo_dark.png" : "logo.png")}")));
    }
    void OnThemeButton(object? s, RoutedEventArgs e) => SetTheme((string)((Button)s!).Tag!);
    void OnThemeMenu(object? s, RoutedEventArgs e) => SetTheme((string)((MenuItem)s!).Tag!);

    void OnSoundToggle(object? s, RoutedEventArgs e) => SetSound(!Sound.Enabled);
    void OnSoundSwitch(object? s, RoutedEventArgs e) { if (!_suppress) SetSound(SoundSwitch.IsChecked == true); }
    void SetSound(bool on)
    {
        Sound.Enabled = on; _s.Sound = on; _s.Save();
        _suppress = true; SoundSwitch.IsChecked = on; _suppress = false;
        SetStatus(I18n.T(on ? "sound_on" : "sound_off"));
        if (on) Sound.PlayDrop();
    }

    /* ---------- ko'rinishlar ---------- */

    void OnNav(object? s, RoutedEventArgs e) => SetView((string)((Button)s!).Tag!);
    void OnHelpMenu(object? s, RoutedEventArgs e) => SetView("help");

    void SetView(string v)
    {
        if (!Views.Contains(v)) return;
        _view = v;
        foreach (var b in new[] { NavHome, NavConfig, NavDevice, NavLog, NavSettings, NavHelp }) b.Classes.Set("on", (string?)b.Tag == v);
        SettingsBtn.Classes.Set("on", v == "settings");
        HelpBtn.Classes.Set("on", v == "help");

        var home = v == "home";
        ConnBar.IsVisible = v is "home" or "config" or "device" or "log";
        ConfigCard.IsVisible = v is "home" or "config";
        DeviceCard.IsVisible = v is "home" or "device";
        LogCard.IsVisible = v is "home" or "log";
        SettingsCard.IsVisible = v == "settings";
        HelpCard.IsVisible = v == "help";

        // joylashuv: home — 2 ustun; qolganlari — bitta karta butun kenglikda
        Grid.SetColumnSpan(ConfigCard, home ? 1 : 2);
        Grid.SetRowSpan(ConfigCard, 2);
        ConfigCard.MaxWidth = home ? double.PositiveInfinity : 900;
        Grid.SetColumn(DeviceCard, home ? 1 : 0); Grid.SetColumnSpan(DeviceCard, home ? 1 : 2);
        Grid.SetRowSpan(DeviceCard, home ? 1 : 2);
        DeviceCard.MaxWidth = home ? double.PositiveInfinity : 900;
        DeviceCard.VerticalAlignment = home ? Avalonia.Layout.VerticalAlignment.Stretch : Avalonia.Layout.VerticalAlignment.Top;
        Grid.SetColumn(LogCard, home ? 1 : 0); Grid.SetColumnSpan(LogCard, home ? 1 : 2);
        Grid.SetRow(LogCard, home ? 2 : 1); Grid.SetRowSpan(LogCard, home ? 1 : 2);
        MainArea.RowDefinitions = home
            ? new RowDefinitions("Auto,Auto,*")
            : v is "settings" or "help" ? new RowDefinitions("*,0,0") : new RowDefinitions("Auto,*,0");
        ApplyCompact(Bounds.Height);
        if (v == "log") ScrollLog(force: true);
    }

    void OnKey(object? s, KeyEventArgs e)
    {
        if (e.Key == Key.F1) { SetView("help"); e.Handled = true; return; }
        if (e.KeyModifiers.HasFlag(KeyModifiers.Control) && e.Key is >= Key.D1 and <= Key.D6)
        {
            SetView(Views[e.Key - Key.D1]); e.Handled = true;
        }
    }

    /* ---------- yordamchilar ---------- */

    void SetStatus(string? msg, string kind = "")
    {
        _statusMsg = msg;
        StatusText.Text = msg ?? I18n.T("status_ready");
        StatusDot.Classes.Set("ok", kind is "" or "ok");
        StatusDot.Classes.Set("err", kind == "err");
        StatusDot.Classes.Set("warn", kind == "warn");
    }
    void RenderStatus()
    {
        StatusText.Text = _statusMsg ?? I18n.T("status_ready");
        var on = SerialService.IsConnected;
        StatusConn.Text = I18n.T(on ? "status_connected" : "status_disconnected");
        StatusConnDot.Classes.Set("ok", on);
        StatusPort.Text = on ? SerialService.PortName : SelectedPort ?? "—";
        StatusBaud.Text = $"{(on ? SerialService.Baud : SelectedBaud)} baud";
    }

    string? SelectedPort => _knownPorts is { Length: > 0 } && PortBox.SelectedItem is string p ? p : null;
    int SelectedBaud => Protocol.BaudRates[Math.Max(0, BaudBox.SelectedIndex)];
    CultureInfo Culture => I18n.Lang == "en" ? CultureInfo.GetCultureInfo("en-US") : CultureInfo.GetCultureInfo("ru-RU");
    string FmtM3(uint liters) => (liters / 1000.0).ToString("N1", Culture);

    /* ---------- jurnal ---------- */

    bool Matches(LogEntry e) => (_logFilter == "all" || e.Level == _logFilter) && (_logSearch.Length == 0 || e.Msg.Contains(_logSearch, StringComparison.OrdinalIgnoreCase));

    void Log(string level, string msg)
    {
        var e = new LogEntry(DateTime.Now, level, msg);
        _logs.Add(e);
        if (_logs.Count > 2000) _logs.RemoveAt(0);
        if (Matches(e))
        {
            if (LogRows.Children.Count == 1 && LogRows.Children[0] is TextBlock { Classes: var c } && c.Contains("empty")) LogRows.Children.Clear();
            LogRows.Children.Add(LogRow(e));
            ScrollLog();
        }
        RenderLogCounts();
    }

    Control LogRow(LogEntry e)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("62,44,*"), ColumnSpacing = 10 };
        g.Classes.Add("logrow");
        var t = new TextBlock { Text = e.Ts.ToString("HH:mm:ss") }; t.Classes.Add("t");
        var lv = new TextBlock { Text = LevelNames[e.Level] }; lv.Classes.Add("lv");
        lv.Bind(ForegroundProperty, lv.GetResourceObservable(LevelBrushes[e.Level]));
        var m = new TextBlock { Text = e.Msg }; m.Classes.Add("m");
        if (e.Level == "error") m.Bind(ForegroundProperty, m.GetResourceObservable("DangerBrush"));
        Grid.SetColumn(lv, 1); Grid.SetColumn(m, 2);
        g.Children.Add(t); g.Children.Add(lv); g.Children.Add(m);
        return g;
    }

    void RenderLog()
    {
        LogRows.Children.Clear();
        var rows = _logs.Where(Matches).ToList();
        if (rows.Count == 0)
        {
            var empty = new TextBlock { Text = I18n.T("log_empty"), Margin = new Thickness(0, 6) };
            empty.Classes.Add("empty"); empty.Classes.Add("muted");
            LogRows.Children.Add(empty);
        }
        else foreach (var e in rows) LogRows.Children.Add(LogRow(e));
        RenderLogCounts();
        ScrollLog(force: true);
    }

    void RenderLogCounts()
    {
        int info = 0, tx = 0, rx = 0, warn = 0, err = 0;
        foreach (var e in _logs)
            switch (e.Level) { case "info": info++; break; case "tx": tx++; break; case "rx": rx++; break; case "warn": warn++; break; case "error": err++; break; }
        var pairs = new (Button, string, int)[] { (ChipAll, I18n.T("log_all"), _logs.Count), (ChipInfo, "INFO", info), (ChipTx, "TX", tx), (ChipRx, "RX", rx), (ChipWarn, "WARN", warn), (ChipError, "ERROR", err) };
        foreach (var (chip, name, n) in pairs)
        {
            chip.Content = _shortChips ? name : $"{name} ({n})";
            ToolTip.SetTip(chip, $"{name}: {n}");
        }
        NavLogBadge.IsVisible = _logs.Count > 0;
        NavLogCount.Text = (err + warn > 0 ? err + warn : _logs.Count).ToString();
    }

    void ScrollLog(bool force = false)
    {
        if (force || _s.AutoScroll) Dispatcher.UIThread.Post(() => LogScroll.ScrollToEnd(), DispatcherPriority.Background);
    }

    void OnChip(object? s, RoutedEventArgs e)
    {
        var b = (Button)s!;
        _logFilter = (string)b.Tag!;
        foreach (var c in new[] { ChipAll, ChipInfo, ChipTx, ChipRx, ChipWarn, ChipError }) c.Classes.Set("on", c == b);
        RenderLog();
    }
    void OnLogSearch(object? s, TextChangedEventArgs e) { _logSearch = (LogSearch.Text ?? "").Trim(); RenderLog(); }
    void OnAutoScroll(object? s, RoutedEventArgs e) { if (_suppress) return; _s.AutoScroll = AutoScroll.IsChecked == true; _s.Save(); ScrollLog(); }
    void OnClearLog(object? s, RoutedEventArgs e) { _logs.Clear(); RenderLog(); }

    string LogText() => string.Join("\n", _logs.Where(Matches).Select(e => $"{e.Ts:dd.MM.yyyy HH:mm:ss}  {LevelNames[e.Level],-5}  {e.Msg}"));

    async void OnCopyLog(object? s, RoutedEventArgs e)
    {
        if (Clipboard is { } cb) { await cb.SetTextAsync(LogText()); SetStatus(I18n.T("copied"), "ok"); }
    }

    async void OnSaveLog(object? s, RoutedEventArgs e)
    {
        try
        {
            var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                SuggestedFileName = $"uz-meter-log-{DateTime.Now:yyyyMMdd}.txt",
                DefaultExtension = "txt",
                FileTypeChoices = [new FilePickerFileType("Text") { Patterns = ["*.txt"] }],
            });
            if (file is null) return;
            await using var stream = await file.OpenWriteAsync();
            await using var w = new StreamWriter(stream, new UTF8Encoding(false));
            await w.WriteAsync(LogText() + "\n");
            SetStatus(I18n.T("log_saved"), "ok");
        }
        catch (Exception ex) { SetStatus($"{I18n.T("err_file")}: {ex.Message}", "err"); }
    }

    /* ---------- ulanish ---------- */

    void RenderConn()
    {
        var on = SerialService.IsConnected;
        ConnDot.Classes.Set("on", on && !_busy); ConnDot.Classes.Set("busy", _busy);
        ConnRing.Classes.Set("on", on && !_busy); ConnRing.Classes.Set("busy", _busy);
        ConnTitle.Classes.Set("on", on && !_busy);
        ConnectBtn.IsVisible = !on; DisconnectBtn.IsVisible = on;
        var port = on ? SerialService.PortName : SelectedPort;
        if (_busy) { ConnTitle.Text = I18n.T("conn_busy_title"); ConnSub.Text = I18n.T("conn_busy_sub", ("port", port ?? "—")); }
        else if (on) { ConnTitle.Text = I18n.T("conn_on_title"); ConnSub.Text = I18n.T("conn_on_sub", ("port", port ?? "—")); }
        else { ConnTitle.Text = I18n.T("conn_off_title"); ConnSub.Text = I18n.T("conn_off_sub"); }
        ConnSince.Text = on && _connSince is { } t ? I18n.T("connected_at", ("t", t.ToString("dd.MM.yyyy HH:mm:ss"))) : " ";
        ConnectBtn.IsEnabled = !_busy && SelectedPort is not null;
        DisconnectBtn.IsEnabled = !_busy;
        SendBtn.IsEnabled = !_busy;
        ReadBtn.IsEnabled = !_busy;
        RefreshBtn.IsEnabled = !_busy && !on;
        PortBox.IsEnabled = !on; BaudBox.IsEnabled = !on;
        RenderStatus();
    }

    void SetBusy(bool b) { _busy = b; RenderConn(); }

    void RefreshPorts(bool manual)
    {
        var prev = SelectedPort ?? _s.LastPort;
        if (manual) Log("info", I18n.T("log_searching"));
        var ports = SerialService.ListPorts();
        var changed = _knownPorts is null || !ports.SequenceEqual(_knownPorts);
        _knownPorts = ports;
        if (changed || manual)
        {
            _suppress = true;
            PortBox.ItemsSource = ports.Length == 0 ? [I18n.T("no_ports")] : ports;
            PortBox.SelectedIndex = Array.IndexOf(ports, prev) is var i and >= 0 ? i : 0;
            _suppress = false;
            Log(ports.Length == 0 ? "warn" : "info", ports.Length == 0 ? I18n.T("log_none") : I18n.T("log_found", ("ports", string.Join(", ", ports))));
        }
        // ulangan port yo'qolgan bo'lsa — uzamiz
        if (SerialService.IsConnected && !ports.Contains(SerialService.PortName) && !_busy)
        {
            Log("warn", I18n.T("log_port_lost"));
            SerialService.Disconnect();
            SetStatus(I18n.T("status_disconnected"), "warn");
        }
        RenderConn();
    }

    void OnRefreshPorts(object? s, RoutedEventArgs e) => RefreshPorts(manual: true);
    void OnPortChanged(object? s, SelectionChangedEventArgs e)
    {
        if (_suppress) return;
        if (s == BaudBox) Log("info", I18n.T("log_baud", ("baud", SelectedBaud)));
        RenderConn();
    }

    async void OnConnect(object? s, RoutedEventArgs e)
    {
        var port = SelectedPort;
        if (port is null) { SetStatus(I18n.T("select_port"), "err"); Sound.PlayErr(); return; }
        var baud = SelectedBaud;
        SetBusy(true);
        ConnTitle.Text = I18n.T("connecting");
        try
        {
            await Task.Run(() => SerialService.Connect(port, baud));
            _connSince = DateTime.Now;
            _s.LastPort = port; _s.LastBaud = baud; _s.Save();
            Log("info", I18n.T("log_connected", ("port", port), ("baud", baud)));
            SetStatus(I18n.T("status_connected"), "ok");
            Sound.PlayOk();
        }
        catch (Exception ex)
        {
            var msg = I18n.Error(ex);
            Log("error", msg); SetStatus(msg, "err"); Sound.PlayErr();
        }
        finally { SetBusy(false); }
    }

    void OnDisconnect(object? s, RoutedEventArgs e)
    {
        var port = SerialService.PortName;
        SerialService.Disconnect();
        if (port is not null) Log("info", I18n.T("log_disconnected", ("port", port)));
        SetStatus(I18n.T("status_disconnected"));
        RenderConn();
    }

    /* ---------- forma ---------- */

    DeviceConfig GetConfig() => new()
    {
        DeviceNo = DeviceNoBox.Text?.Trim() ?? "",
        MNo = MNoBox.Text?.Trim() ?? "",
        ServerUrl = UrlBox.Text?.Trim() ?? "",
        ValveOpen = _valve == 1,
        Unit = (byte)_unit,
        SendsPerDay = (byte)Protocol.SendsPerDayOptions[Math.Max(0, SendsBox.SelectedIndex)],
        MeterType = (byte)Math.Max(0, TypeBox.SelectedIndex),
    };

    void SetConfig(DeviceConfig c)
    {
        _suppress = true;
        DeviceNoBox.Text = c.DeviceNo;
        MNoBox.Text = c.MNo;
        UrlBox.Text = c.ServerUrl;
        _valve = c.ValveOpen ? 1 : 0;
        _unit = Math.Clamp((int)c.Unit, 0, 3);
        var si = Array.IndexOf(Protocol.SendsPerDayOptions, (int)c.SendsPerDay);
        SendsBox.SelectedIndex = si >= 0 ? si : Array.IndexOf(Protocol.SendsPerDayOptions, 6);
        TypeBox.SelectedIndex = Math.Clamp((int)c.MeterType, 0, 5);
        _suppress = false;
        SyncSegs();
        UpdatePreview();
    }

    void SyncSegs()
    {
        ValveOpenBtn.Classes.Set("on", _valve == 1);
        ValveClosedBtn.Classes.Set("on", _valve == 0);
        var units = new[] { Unit0, Unit1, Unit2, Unit3 };
        for (var i = 0; i < units.Length; i++) units[i].Classes.Set("on", i == _unit);
    }

    void OnValve(object? s, RoutedEventArgs e) { _valve = int.Parse((string)((Button)s!).Tag!); SyncSegs(); UpdatePreview(); }
    void OnUnit(object? s, RoutedEventArgs e) { _unit = int.Parse((string)((Button)s!).Tag!); SyncSegs(); UpdatePreview(); }

    void OnDigitsChanged(object? s, TextChangedEventArgs e)
    {
        if (_suppress) return;
        var tb = (TextBox)s!;
        var filtered = new string((tb.Text ?? "").Where(char.IsAsciiDigit).ToArray());
        if (filtered != tb.Text)
        {
            var caret = Math.Min(tb.CaretIndex, filtered.Length);
            tb.Text = filtered;
            tb.CaretIndex = caret;
            return; // TextChanged qayta chaqiriladi
        }
        UpdatePreview();
    }

    void OnFormChanged(object? s, RoutedEventArgs e) { if (!_suppress) UpdatePreview(); }

    void UpdateCounters()
    {
        Counter(DeviceNoBox, DeviceNoCnt, DeviceNoCntIn, DeviceNoOk, 15, exact: true);
        Counter(MNoBox, MNoCnt, MNoCntIn, MNoOk, 14, exact: true);
        Counter(UrlBox, UrlCnt, UrlCntIn, UrlOk, 128, exact: false);

        static void Counter(TextBox tb, TextBlock cnt, TextBlock cntIn, Border ok, int max, bool exact)
        {
            var v = tb.Text ?? "";
            var len = v.Length;
            var good = exact ? len == max : len > 0 && len <= max && v.All(ch => ch >= 0x20 && ch <= 0x7E);
            cnt.Text = cntIn.Text = $"{len} / {max}";
            cnt.Classes.Set("ok", good); cntIn.Classes.Set("ok", good);
            tb.Classes.Set("invalid", len > 0 && !good);
            ok.IsVisible = good;
        }
    }

    void UpdatePreview()
    {
        if (_suppress) return;
        UpdateCounters();
        try
        {
            var cfg = GetConfig();
            Protocol.Validate(cfg);
            _lastFrame = Protocol.BuildFrame(Protocol.CmdSetConfig, Protocol.EncodeConfig(cfg));
            FramePreview.Text = Protocol.Hex(_lastFrame);
            FramePreview.Classes.Set("err", false);
            FrameLen.Text = $"{_lastFrame.Length} {I18n.T("bytes")}";
        }
        catch (Exception ex)
        {
            _lastFrame = null;
            FramePreview.Text = I18n.Error(ex);
            FramePreview.Classes.Set("err", true);
            FrameLen.Text = "";
        }
        if (_frameTab == "struct") RenderStruct();
    }

    void OnRecalc(object? s, RoutedEventArgs e) { UpdatePreview(); SetStatus(I18n.T("recalc") + " ✓", "ok"); }
    void OnReset(object? s, RoutedEventArgs e) => SetConfig(new DeviceConfig());

    void OnFrameTab(object? s, RoutedEventArgs e)
    {
        _frameTab = (string)((Button)s!).Tag!;
        TabHex.Classes.Set("on", _frameTab == "hex"); TabStruct.Classes.Set("on", _frameTab == "struct");
        HexBox.IsVisible = _frameTab == "hex"; StructBox.IsVisible = _frameTab == "struct";
        if (_frameTab == "struct") RenderStruct();
    }

    /// <summary>HEX kadrni maydonlarga ajratib jadval ko'rinishida chizadi (PROTOCOL.md → CONFIG).</summary>
    void RenderStruct()
    {
        StructRows.Children.Clear();
        if (_lastFrame is null)
        {
            var err = new TextBlock { Text = FramePreview.Text, Margin = new Thickness(10, 8), TextWrapping = TextWrapping.Wrap };
            err.Bind(ForegroundProperty, err.GetResourceObservable("DangerBrush"));
            StructRows.Children.Add(err);
            return;
        }
        var f = _lastFrame;
        string Hx(int off, int len) => Protocol.Hex(f.AsSpan(off, len));
        string Ascii(int off, int len) => Encoding.ASCII.GetString(f, off, len);
        int urlLen = f[37];
        var rows = new (string, int, int, string, string)[]
        {
            (I18n.T("st_sof"), 0, 2, Hx(0, 2), "AA 55"),
            (I18n.T("st_cmd"), 2, 1, Hx(2, 1), "SET_CONFIG"),
            (I18n.T("st_lenf"), 3, 1, Hx(3, 1), $"{f[3]} {I18n.T("bytes")}"),
            (I18n.T("device_no"), 4, 15, Hx(4, 15), Ascii(4, 15)),
            (I18n.T("m_no"), 19, 14, Hx(19, 14), Ascii(19, 14)),
            (I18n.T("valve"), 33, 1, Hx(33, 1), f[33] == 1 ? I18n.T("valve_open") : I18n.T("valve_closed")),
            (I18n.T("unit"), 34, 1, Hx(34, 1), f[34] < 4 ? Protocol.UnitNames[f[34]] : f[34].ToString()),
            (I18n.T("sends"), 35, 1, Hx(35, 1), $"{f[35]} {I18n.T("per_day")}"),
            (I18n.T("meter_type"), 36, 1, Hx(36, 1), f[36] < 6 ? I18n.MeterTypes[f[36]] : f[36].ToString()),
            (I18n.T("st_url_len"), 37, 1, Hx(37, 1), urlLen.ToString()),
            (I18n.T("server_url"), 38, urlLen, Hx(38, urlLen), Ascii(38, urlLen)),
            (I18n.T("st_crc"), 38 + urlLen, 2, Hx(f.Length - 2, 2), $"0x{f[^1]:X2}{f[^2]:X2}"),
        };
        StructRows.Children.Add(StructRow(I18n.T("st_field"), I18n.T("st_off"), I18n.T("st_len"), I18n.T("st_bytes"), I18n.T("st_value"), head: true));
        foreach (var (n, o, l, b, v) in rows) StructRows.Children.Add(StructRow(n, o.ToString(), l.ToString(), b, v, head: false));
    }

    static Control StructRow(string name, string off, string len, string bytes, string val, bool head)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("150,50,60,1.2*,*") };
        g.Classes.Add("strow"); if (head) g.Classes.Add("head");
        var cells = new[] { name, off, len, bytes, val };
        for (var i = 0; i < 5; i++)
        {
            var tb = new TextBlock { Text = cells[i] };
            if (!head && i is 1 or 2) tb.Classes.Add("n");
            if (!head && i == 3) { tb.Classes.Add("b"); ToolTip.SetTip(tb, bytes); }
            Grid.SetColumn(tb, i);
            g.Children.Add(tb);
        }
        var line = new Border(); line.Classes.Add("line"); Grid.SetColumnSpan(line, 5);
        g.Children.Add(line);
        return g;
    }

    /* ---------- USB: yuborish ---------- */

    async void OnSend(object? s, RoutedEventArgs e)
    {
        if (!SerialService.IsConnected) { SetStatus(I18n.T("not_connected"), "err"); Sound.PlayErr(); return; }
        byte[] frame;
        try
        {
            var cfg = GetConfig();
            Protocol.Validate(cfg);
            frame = Protocol.BuildFrame(Protocol.CmdSetConfig, Protocol.EncodeConfig(cfg));
        }
        catch (Exception ex)
        {
            SetStatus(I18n.Error(ex), "err"); Log("error", I18n.Error(ex)); Sound.PlayErr();
            return;
        }

        SetBusy(true);
        SetStatus(I18n.T("status_sending"));
        try
        {
            var timeout = _s.Timeout;
            var ex = await Task.Run(() => SerialService.Transact(frame, timeout));
            Log("tx", $"{I18n.T("log_cmd_sent", ("cmd", "0x01"))}: {ex.TxHex}");
            if (ex.RxHex.Length > 0) Log("rx", ex.RxHex);
            switch (ex.Cmd)
            {
                case Protocol.CmdSetAck when ex.Payload.Length > 0 && ex.Payload[0] == 0:
                    Log("info", I18n.T("ack_ok")); Log("info", I18n.T("ok_written")); SetStatus(I18n.T("ok_written"), "ok"); Sound.PlayOk(); break;
                case Protocol.CmdSetAck when ex.Payload.Length > 0 && ex.Payload[0] == 1:
                    Log("error", I18n.T("ack_crc")); SetStatus(I18n.T("ack_crc"), "err"); Sound.PlayErr(); break;
                case Protocol.CmdSetAck:
                    Log("error", I18n.T("ack_invalid")); SetStatus(I18n.T("ack_invalid"), "err"); Sound.PlayErr(); break;
                case null:
                    Log("warn", I18n.T("no_ack")); SetStatus(I18n.T("no_ack"), "warn"); break;
                default:
                    Log("error", $"{I18n.T("err_unexpected")}: {ex.RxHex}"); SetStatus($"{I18n.T("err_unexpected")}: {ex.RxHex}", "err"); Sound.PlayErr(); break;
            }
        }
        catch (Exception ex)
        {
            Log("error", I18n.Error(ex)); SetStatus(I18n.Error(ex), "err"); Sound.PlayErr();
        }
        finally { SetBusy(false); }
    }

    /* ---------- USB: o'qish ---------- */

    async void OnRead(object? s, RoutedEventArgs e)
    {
        if (!SerialService.IsConnected) { SetStatus(I18n.T("not_connected"), "err"); Sound.PlayErr(); return; }
        var frame = Protocol.BuildFrame(Protocol.CmdGetData, []);

        SetBusy(true);
        SetStatus(I18n.T("status_reading"));
        try
        {
            var timeout = _s.Timeout;
            var ex = await Task.Run(() => SerialService.Transact(frame, timeout));
            Log("tx", $"{I18n.T("log_cmd_sent", ("cmd", "0x02"))}: {ex.TxHex}");
            if (ex.RxHex.Length > 0) Log("rx", ex.RxHex);
            if (ex.Cmd == Protocol.CmdData)
            {
                _loaded = Protocol.DecodeDeviceData(ex.Payload);
                _loadedAt = DateTime.Now;
                var m = _loaded.Telemetry;
                Log("info", I18n.T("ok_read"));
                Log("info", I18n.T("log_telemetry", ("w", FmtM3(m.WaterVolumeL)), ("v", (m.BatteryMv / 1000.0).ToString("F2", CultureInfo.InvariantCulture)), ("p", m.BatteryPct)));
                if (m.BatteryPct < 20) Log("warn", I18n.T("log_batt_low", ("p", m.BatteryPct)));
                RenderDevice();
                if (_s.Autofill) { SetConfig(_loaded.Config); SetStatus(I18n.T("loaded_to_form"), "ok"); }
                else SetStatus(I18n.T("ok_read"), "ok");
                Sound.PlayOk();
            }
            else if (ex.Cmd is null) { Log("warn", I18n.T("no_response")); SetStatus(I18n.T("no_response"), "err"); Sound.PlayErr(); }
            else { Log("error", $"{I18n.T("err_unexpected")}: {ex.RxHex}"); SetStatus($"{I18n.T("err_unexpected")}: {ex.RxHex}", "err"); Sound.PlayErr(); }
        }
        catch (Exception ex)
        {
            Log("error", I18n.Error(ex)); SetStatus(I18n.Error(ex), "err"); Sound.PlayErr();
        }
        finally { SetBusy(false); }
    }

    void OnApply(object? s, RoutedEventArgs e)
    {
        if (_loaded is null) return;
        SetConfig(_loaded.Config);
        SetStatus(I18n.T("loaded_to_form"), "ok");
        if (_view == "device") SetView("config");
    }

    void RenderDevice()
    {
        var d = _loaded;
        var cells = new[] { DDeviceNo, DMNo, DUrl, DType, DValve, DUnit, DSends, DLast };
        ApplyBtn.IsEnabled = d is not null;
        if (d is null)
        {
            foreach (var c in cells) { c.Text = "—"; c.Classes.Set("empty", true); ToolTip.SetTip(c, null); }
            TWater.Text = "—"; TBattV.Text = "—"; TBattP.Text = "—"; BattBar.Value = 0;
            DState.Text = I18n.T("state_none"); StateDot.Classes.Set("ok", false); StateDot.Classes.Set("warn", false);
            return;
        }
        var c0 = d.Config; var m = d.Telemetry;
        var vals = new[]
        {
            c0.DeviceNo, c0.MNo, c0.ServerUrl,
            c0.MeterType < 6 ? I18n.MeterTypes[c0.MeterType] : c0.MeterType.ToString(),
            c0.ValveOpen ? I18n.T("valve_open") : I18n.T("valve_closed"),
            c0.Unit < 4 ? Protocol.UnitNames[c0.Unit] : c0.Unit.ToString(),
            $"{c0.SendsPerDay} {I18n.T("per_day")}",
            _loadedAt?.ToString("dd.MM.yyyy HH:mm:ss") ?? "—",
        };
        for (var i = 0; i < cells.Length; i++) { cells[i].Text = vals[i]; cells[i].Classes.Set("empty", false); ToolTip.SetTip(cells[i], vals[i]); }

        TWater.Text = FmtM3(m.WaterVolumeL);
        TBattV.Text = (m.BatteryMv / 1000.0).ToString("F2", CultureInfo.InvariantCulture);
        TBattP.Text = m.BatteryPct.ToString();
        BattBar.Value = m.BatteryPct;
        BattBar.Classes.Set("low", m.BatteryPct < 20);
        BattBar.Classes.Set("mid", m.BatteryPct is >= 20 and < 50);
        var low = m.BatteryPct < 20;
        DState.Text = I18n.T(low ? "state_low" : "state_ok");
        StateDot.Classes.Set("ok", !low); StateDot.Classes.Set("warn", low);
    }

    /* ---------- fayl ---------- */

    static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true, PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };

    async void OnSave(object? s, RoutedEventArgs e)
    {
        try
        {
            var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
            {
                SuggestedFileName = "uz-meter-config.json",
                DefaultExtension = "json",
                FileTypeChoices = [new FilePickerFileType("JSON") { Patterns = ["*.json"] }],
            });
            if (file is null) return;
            await using var stream = await file.OpenWriteAsync();
            await JsonSerializer.SerializeAsync(stream, GetConfig(), JsonOpts);
            SetStatus(I18n.T("saved"), "ok");
            Log("info", $"{I18n.T("saved")}: {file.Name}");
        }
        catch (Exception ex)
        {
            SetStatus($"{I18n.T("err_file")}: {ex.Message}", "err");
        }
    }

    async void OnOpen(object? s, RoutedEventArgs e)
    {
        try
        {
            var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
            {
                AllowMultiple = false,
                FileTypeFilter = [new FilePickerFileType("JSON") { Patterns = ["*.json"] }],
            });
            if (files.Count == 0) return;
            await using var stream = await files[0].OpenReadAsync();
            var cfg = await JsonSerializer.DeserializeAsync<DeviceConfig>(stream, JsonOpts);
            if (cfg is null) throw new ProtocolException("err_file");
            SetConfig(cfg);
            SetStatus(I18n.T("loaded"), "ok");
            Log("info", $"{I18n.T("loaded")}: {files[0].Name}");
            SetView("config");
        }
        catch (Exception ex)
        {
            SetStatus($"{I18n.T("err_file")}: {ex.Message}", "err");
        }
    }

    void OnQuit(object? s, RoutedEventArgs e) => Close();

    /* ---------- sozlamalar ---------- */

    void OnSettingChanged(object? s, RoutedEventArgs e)
    {
        if (_suppress) return;
        _s.Autofill = AutofillSwitch.IsChecked == true;
        _s.Autoconnect = AutoconnectSwitch.IsChecked == true;
        _s.Timeout = Settings.Timeouts[Math.Max(0, TimeoutBox.SelectedIndex)];
        _s.Save();
    }

    void OnResetSettings(object? s, RoutedEventArgs e)
    {
        _s.Reset();
        _suppress = true;
        SoundSwitch.IsChecked = _s.Sound; AutofillSwitch.IsChecked = _s.Autofill; AutoconnectSwitch.IsChecked = _s.Autoconnect;
        TimeoutBox.SelectedIndex = Math.Max(0, Array.IndexOf(Settings.Timeouts, _s.Timeout));
        AutoScroll.IsChecked = _s.AutoScroll;
        _suppress = false;
        Sound.Enabled = _s.Sound;
        Application.Current!.RequestedThemeVariant = _s.Theme == "dark" ? ThemeVariant.Dark : ThemeVariant.Light;
        ApplyThemeUi();
        I18n.SetLanguage(_s.Lang);
        SetStatus(I18n.T("settings_reset_done"), "ok");
    }

    /* ---------- boshqa menyu ---------- */

    async void OnCopyHex(object? s, RoutedEventArgs e)
    {
        if (_lastFrame is null) { SetStatus(FramePreview.Text, "err"); return; }
        if (Clipboard is { } cb)
        {
            await cb.SetTextAsync(Protocol.Hex(_lastFrame));
            SetStatus($"HEX — {I18n.T("copied")}", "ok");
        }
    }

    const string ProtocolText = """
        FRAME:  AA 55 | CMD | LEN | PAYLOAD(LEN) | CRC16 lo hi
        CRC-16/MODBUS over CMD + LEN + PAYLOAD

        CMD   Dir            Payload
        0x01  PC -> device   CONFIG
        0x02  PC -> device   (empty)
        0x81  device -> PC   status: 0=OK 1=CRC 2=invalid
        0x82  device -> PC   CONFIG + TELEMETRY

        CONFIG
        off  size  field
        0    15    device_no      ASCII digits
        15   14    m_no           ASCII digits
        29   1     valve          0=closed 1=open
        30   1     unit           0=1L 1=10L 2=100L 3=1000L
        31   1     sends_per_day  1..24
        32   1     meter_type     0..5
        33   1     url_len        N (<=128)
        34   N     server_url     ASCII

        TELEMETRY (after CONFIG in 0x82)
        0    4     water_volume   u32 LE, liters
        4    2     battery_mv     u16 LE
        6    1     battery_pct    0..100
        """;
}
