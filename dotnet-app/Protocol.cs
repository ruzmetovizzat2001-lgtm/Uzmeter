using System.Text;

namespace UzMeter;

/// <summary>Qurilma sozlamalari (PROTOCOL.md → CONFIG).</summary>
public sealed class DeviceConfig
{
    public string DeviceNo { get; set; } = "";
    public string MNo { get; set; } = "";
    public string ServerUrl { get; set; } = "";
    public bool ValveOpen { get; set; } = true;
    /// <summary>0 = 1 L, 1 = 10 L, 2 = 100 L, 3 = 1000 L</summary>
    public byte Unit { get; set; }
    /// <summary>1..24</summary>
    public byte SendsPerDay { get; set; } = 6;
    /// <summary>0..5</summary>
    public byte MeterType { get; set; }
}

public sealed record Telemetry(uint WaterVolumeL, ushort BatteryMv, byte BatteryPct);

public sealed record DeviceData(DeviceConfig Config, Telemetry Telemetry);

public sealed class ProtocolException(string key, string? detail = null) : Exception(key)
{
    public string Key { get; } = key;
    public string? Detail { get; } = detail;
}

/// <summary>Uz-Meter USB kadr protokoli (PROTOCOL.md ga qarang).</summary>
public static class Protocol
{
    public const byte Sof0 = 0xAA;
    public const byte Sof1 = 0x55;

    public const byte CmdSetConfig = 0x01;
    public const byte CmdGetData = 0x02;
    public const byte CmdSetAck = 0x81;
    public const byte CmdData = 0x82;

    public const int DeviceNoLen = 15;
    public const int MNoLen = 14;
    public const int UrlMax = 128;
    public const int ConfigFixedLen = 34;
    public const int TelemetryLen = 7;

    public static readonly int[] BaudRates = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
    public static readonly int[] SendsPerDayOptions = [1, 2, 3, 4, 6, 8, 12, 24];
    public static readonly string[] UnitNames = ["1 L", "10 L", "100 L", "1000 L"];

    /// <summary>CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF).</summary>
    public static ushort Crc16Modbus(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++)
                crc = (crc & 1) != 0 ? (ushort)((crc >> 1) ^ 0xA001) : (ushort)(crc >> 1);
        }
        return crc;
    }

    static bool AllDigits(string s, int n) => s.Length == n && s.All(char.IsAsciiDigit);

    public static void Validate(DeviceConfig c)
    {
        if (!AllDigits(c.DeviceNo, DeviceNoLen)) throw new ProtocolException("err_device_no");
        if (!AllDigits(c.MNo, MNoLen)) throw new ProtocolException("err_m_no");
        if (c.ServerUrl.Length == 0 || c.ServerUrl.Length > UrlMax || !Ascii.IsValid(c.ServerUrl))
            throw new ProtocolException("err_url");
        if (c.Unit > 3) throw new ProtocolException("err_unit");
        if (c.SendsPerDay is < 1 or > 24) throw new ProtocolException("err_sends");
        if (c.MeterType > 5) throw new ProtocolException("err_type");
    }

    public static byte[] EncodeConfig(DeviceConfig c)
    {
        var p = new List<byte>(ConfigFixedLen + c.ServerUrl.Length);
        p.AddRange(Encoding.ASCII.GetBytes(c.DeviceNo));
        p.AddRange(Encoding.ASCII.GetBytes(c.MNo));
        p.Add((byte)(c.ValveOpen ? 1 : 0));
        p.Add(c.Unit);
        p.Add(c.SendsPerDay);
        p.Add(c.MeterType);
        p.Add((byte)c.ServerUrl.Length);
        p.AddRange(Encoding.ASCII.GetBytes(c.ServerUrl));
        return p.ToArray();
    }

    public static byte[] BuildFrame(byte cmd, ReadOnlySpan<byte> payload)
    {
        var f = new byte[payload.Length + 6];
        f[0] = Sof0;
        f[1] = Sof1;
        f[2] = cmd;
        f[3] = (byte)payload.Length;
        payload.CopyTo(f.AsSpan(4));
        var crc = Crc16Modbus(f.AsSpan(2, payload.Length + 2));
        f[^2] = (byte)(crc & 0xFF);
        f[^1] = (byte)(crc >> 8);
        return f;
    }

    /// <summary>
    /// Buferdan to'liq kadrni qidiradi. Kadr hali to'liq emas — null.
    /// CRC xato — ProtocolException("err_crc").
    /// </summary>
    public static (byte Cmd, byte[] Payload, int Consumed)? ParseFrame(ReadOnlySpan<byte> buf)
    {
        var crcErr = false;
        for (var i = 0; i + 1 < buf.Length; i++)
        {
            if (buf[i] != Sof0 || buf[i + 1] != Sof1) continue;
            if (i + 4 > buf.Length) return null;
            var cmd = buf[i + 2];
            int len = buf[i + 3];
            var end = i + 4 + len + 2;
            if (end > buf.Length) return null;
            var body = buf.Slice(i + 2, len + 2);
            var crcRx = (ushort)(buf[end - 2] | (buf[end - 1] << 8));
            if (Crc16Modbus(body) == crcRx) return (cmd, buf.Slice(i + 4, len).ToArray(), end);
            crcErr = true; // noto'g'ri sinxronizatsiya bo'lishi mumkin — keyingi SOF dan davom etamiz
        }
        if (crcErr) throw new ProtocolException("err_crc");
        return null;
    }

    public static (DeviceConfig Config, int Used) DecodeConfig(ReadOnlySpan<byte> p)
    {
        if (p.Length < ConfigFixedLen) throw new ProtocolException("err_short");
        int urlLen = p[33];
        if (p.Length < ConfigFixedLen + urlLen) throw new ProtocolException("err_short");
        var cfg = new DeviceConfig
        {
            DeviceNo = Encoding.ASCII.GetString(p[..15]),
            MNo = Encoding.ASCII.GetString(p.Slice(15, 14)),
            ValveOpen = p[29] != 0,
            Unit = p[30],
            SendsPerDay = p[31],
            MeterType = p[32],
            ServerUrl = Encoding.ASCII.GetString(p.Slice(34, urlLen)),
        };
        return (cfg, ConfigFixedLen + urlLen);
    }

    public static DeviceData DecodeDeviceData(ReadOnlySpan<byte> p)
    {
        var (cfg, used) = DecodeConfig(p);
        var t = p[used..];
        if (t.Length < TelemetryLen) throw new ProtocolException("err_short");
        var tele = new Telemetry(
            System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(t[..4]),
            System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(t.Slice(4, 2)),
            Math.Min(t[6], (byte)100));
        return new DeviceData(cfg, tele);
    }

    public static string Hex(ReadOnlySpan<byte> bytes)
    {
        var sb = new StringBuilder(bytes.Length * 3);
        for (var i = 0; i < bytes.Length; i++)
        {
            if (i > 0) sb.Append(' ');
            sb.Append(bytes[i].ToString("X2"));
        }
        return sb.ToString();
    }
}
