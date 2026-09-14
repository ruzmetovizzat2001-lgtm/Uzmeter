using System.Diagnostics;
using System.Runtime.InteropServices;

namespace UzMeter;

/// <summary>
/// Kichik UI ovozlari — tashqi kutubxonasiz. WAV ma'lumotlari dasturning o'zida sintez qilinadi.
/// Windows: winmm PlaySound (xotiradan). macOS: afplay. Linux: paplay/aplay.
/// </summary>
public static class Sound
{
    public static bool Enabled { get; set; } = true;

    const int Rate = 44100;

    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    static extern bool PlaySound(byte[] data, IntPtr hmod, uint flags);
    const uint SND_ASYNC = 0x0001, SND_MEMORY = 0x0004, SND_NODEFAULT = 0x0002;

    static readonly Lazy<byte[]> DropWav = new(() => Wav(Drop()));
    static readonly Lazy<byte[]> OkWav = new(() => Wav(Ok()));
    static readonly Lazy<byte[]> ErrWav = new(() => Wav(Err()));

    // Splash paytida ishlatiladigan bufer (GC yig'ib ketmasligi uchun static)
    static byte[]? _playing;

    public static void PlayDrop() => Play(DropWav.Value);
    public static void PlayOk() => Play(OkWav.Value);
    public static void PlayErr() => Play(ErrWav.Value);

    static void Play(byte[] wav)
    {
        if (!Enabled) return;
        try
        {
            if (OperatingSystem.IsWindows())
            {
                _playing = wav;
                PlaySound(_playing, IntPtr.Zero, SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
                return;
            }
            var path = Path.Combine(Path.GetTempPath(), $"uzmeter-{wav.Length}.wav");
            if (!File.Exists(path)) File.WriteAllBytes(path, wav);
            var (exe, args) = OperatingSystem.IsMacOS() ? ("afplay", path) : ("paplay", path);
            Process.Start(new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true });
        }
        catch { /* ovoz muhim emas */ }
    }

    /* ---------- sintez ---------- */

    /// <summary>Chastotasi f0→f1 ga eksponensial siljiydigan sinus, eksponensial so'nuvchi envelop.</summary>
    static void Tone(float[] buf, double f0, double f1, double dur, double gain, double delay = 0, bool triangle = false)
    {
        int start = (int)(delay * Rate), n = (int)(dur * Rate);
        double phase = 0;
        for (var i = 0; i < n && start + i < buf.Length; i++)
        {
            var t = (double)i / Rate;
            var k = Math.Min(1, t / (dur * 0.55));
            var f = f0 * Math.Pow(f1 / f0, k);
            phase += 2 * Math.PI * f / Rate;
            var env = t < 0.012 ? t / 0.012 : Math.Exp(-(t - 0.012) / (dur * 0.28));
            var s = triangle ? 2 / Math.PI * Math.Asin(Math.Sin(phase)) : Math.Sin(phase);
            buf[start + i] += (float)(s * env * gain);
        }
    }

    static float[] Drop()
    {
        var b = new float[(int)(0.6 * Rate)];
        Tone(b, 520, 1480, 0.32, 0.22);                 // asosiy tomchi
        Tone(b, 1040, 2960, 0.18, 0.04);                // garmonika
        Tone(b, 180, 120, 0.16, 0.08, 0, triangle: true); // suvga urilish
        Tone(b, 760, 1900, 0.22, 0.08, 0.19);           // ikkinchi kichik tomchi
        return b;
    }

    static float[] Ok()
    {
        var b = new float[(int)(0.4 * Rate)];
        Tone(b, 880, 880, 0.22, 0.12);
        Tone(b, 1320, 1320, 0.22, 0.12, 0.09);
        return b;
    }

    static float[] Err()
    {
        var b = new float[(int)(0.35 * Rate)];
        Tone(b, 320, 180, 0.28, 0.12, 0, triangle: true);
        return b;
    }

    static byte[] Wav(float[] samples)
    {
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        var dataLen = samples.Length * 2;
        w.Write("RIFF"u8); w.Write(36 + dataLen); w.Write("WAVE"u8);
        w.Write("fmt "u8); w.Write(16); w.Write((short)1); w.Write((short)1);
        w.Write(Rate); w.Write(Rate * 2); w.Write((short)2); w.Write((short)16);
        w.Write("data"u8); w.Write(dataLen);
        foreach (var s in samples) w.Write((short)(Math.Clamp(s, -1f, 1f) * short.MaxValue));
        return ms.ToArray();
    }
}
