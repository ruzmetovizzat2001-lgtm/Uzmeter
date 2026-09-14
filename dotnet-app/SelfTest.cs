namespace UzMeter;

/// <summary>Protokolning GUI siz tekshiruvi: <c>UzMeter --selftest</c>.</summary>
public static class SelfTest
{
    public static int Run()
    {
        var fails = 0;
        void Check(string name, bool ok)
        {
            Console.WriteLine($"{(ok ? "ok  " : "FAIL")} {name}");
            if (!ok) fails++;
        }

        // CRC-16/MODBUS("123456789") = 0x4B37
        Check("crc16 known vector", Protocol.Crc16Modbus("123456789"u8) == 0x4B37);

        var cfg = new DeviceConfig
        {
            DeviceNo = "123456789012345",
            MNo = "98765432109876",
            ServerUrl = "http://a.uz/",
            ValveOpen = true,
            Unit = 2,
            SendsPerDay = 6,
            MeterType = 3,
        };
        Protocol.Validate(cfg);

        var payload = Protocol.EncodeConfig(cfg).Concat(new byte[] { 0x10, 0x27, 0, 0, 0xB0, 0x0E, 87 }).ToArray();
        var frame = Protocol.BuildFrame(Protocol.CmdData, payload);
        var parsed = Protocol.ParseFrame(frame);
        Check("parse frame", parsed is { Cmd: Protocol.CmdData } && parsed.Value.Consumed == frame.Length);
        var d = Protocol.DecodeDeviceData(parsed!.Value.Payload);
        Check("device_no roundtrip", d.Config.DeviceNo == cfg.DeviceNo);
        Check("url roundtrip", d.Config.ServerUrl == cfg.ServerUrl);
        Check("telemetry", d.Telemetry is { WaterVolumeL: 10000, BatteryMv: 3760, BatteryPct: 87 });

        // Rust versiyasi bilan bir xil kadr (cargo test dagi bilan solishtirish uchun)
        var setFrame = Protocol.BuildFrame(Protocol.CmdSetConfig, Protocol.EncodeConfig(cfg));
        Console.WriteLine("SET_CONFIG frame: " + Protocol.Hex(setFrame));

        // soxta SOF (CRC noto'g'ri) dan keyin haqiqiy kadr — haqiqiysi topilishi kerak
        var good = Protocol.BuildFrame(Protocol.CmdSetAck, [0]);
        var noisy = new byte[] { 0xAA, 0x55, 0x81, 0x01, 0x00, 0x00, 0x00 }.Concat(good).ToArray();
        Check("resync after bad SOF", Protocol.ParseFrame(noisy) is { Cmd: Protocol.CmdSetAck });
        try { Protocol.ParseFrame(noisy.AsSpan(0, 7)); Check("crc error reported", false); }
        catch (ProtocolException e) { Check("crc error reported", e.Key == "err_crc"); }
        Check("incomplete frame -> null", Protocol.ParseFrame(good.AsSpan(0, 4)) is null);

        try { Protocol.Validate(new DeviceConfig { DeviceNo = "123", MNo = cfg.MNo, ServerUrl = "x" }); Check("validate rejects short device_no", false); }
        catch (ProtocolException e) { Check("validate rejects short device_no", e.Key == "err_device_no"); }

        Console.WriteLine(fails == 0 ? "ALL OK" : $"{fails} FAILED");
        return fails == 0 ? 0 : 1;
    }
}
