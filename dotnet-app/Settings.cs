using System.Text.Json;
using System.Text.Json.Serialization;

namespace UzMeter;

/// <summary>Dastur sozlamalari — %APPDATA%/UzMeter/settings.json (macOS: ~/Library/Application Support).</summary>
public sealed class Settings
{
    public string Lang { get; set; } = "uz";
    public string Theme { get; set; } = "light";
    public bool Sound { get; set; } = true;
    public bool Autofill { get; set; } = true;
    public int Timeout { get; set; } = 2500;
    public bool Autoconnect { get; set; }
    public string LastPort { get; set; } = "";
    public int LastBaud { get; set; } = 9600;
    public bool AutoScroll { get; set; } = true;

    public static readonly int[] Timeouts = [1000, 2500, 5000, 10000];

    static readonly JsonSerializerOptions Opts = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.Never };

    static string Path =>
        System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "UzMeter", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(Path))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(Path), Opts) ?? new Settings();
        }
        catch { /* buzilgan fayl — boshlang'ich sozlamalar */ }
        return new Settings();
    }

    /// <summary>UI tekshiruv rejimida (UZMETER_SHOT) sozlamalar faylga yozilmaydi.</summary>
    public static bool ReadOnly { get; } = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("UZMETER_SHOT"));

    public void Save()
    {
        if (ReadOnly) return;
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
            File.WriteAllText(Path, JsonSerializer.Serialize(this, Opts));
        }
        catch { /* saqlab bo'lmasa — jim */ }
    }

    public void Reset()
    {
        var d = new Settings();
        Lang = d.Lang; Theme = d.Theme; Sound = d.Sound; Autofill = d.Autofill; Timeout = d.Timeout;
        Autoconnect = d.Autoconnect; LastPort = d.LastPort; LastBaud = d.LastBaud; AutoScroll = d.AutoScroll;
        Save();
    }
}
