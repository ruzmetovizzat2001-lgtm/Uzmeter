using System.Diagnostics;
using System.IO.Ports;

namespace UzMeter;

public sealed record Exchange(string TxHex, string RxHex, byte? Cmd, byte[] Payload);

/// <summary>
/// Serial port bilan ishlash: portlar ro'yxati, doimiy ulanish (Ulanish/Uzish) va so'rov/javob almashinuvi.
/// </summary>
public static class SerialService
{
    static readonly object Lock = new();
    static SerialPort? _port;

    public static bool IsConnected { get { lock (Lock) return _port is { IsOpen: true }; } }
    public static string? PortName { get { lock (Lock) return _port?.PortName; } }
    public static int Baud { get { lock (Lock) return _port?.BaudRate ?? 0; } }

    public static string[] ListPorts()
    {
        var names = SerialPort.GetPortNames()
            // macOS: faqat /dev/cu.* ko'rsatamiz (tty.* dublikatlarini yashiramiz)
            .Where(n => !(OperatingSystem.IsMacOS() && n.StartsWith("/dev/tty.")))
            .Distinct()
            .ToList();
        names.Sort((a, b) =>
        {
            var (pa, na) = NaturalKey(a);
            var (pb, nb) = NaturalKey(b);
            var c = string.CompareOrdinal(pa, pb);
            return c != 0 ? c : na.CompareTo(nb);
        });
        return names.ToArray();
    }

    /// <summary>"COM10" > "COM9" bo'lishi uchun tabiiy tartib.</summary>
    static (string, int) NaturalKey(string s)
    {
        var i = s.Length;
        while (i > 0 && char.IsAsciiDigit(s[i - 1])) i--;
        return (s[..i], int.TryParse(s[i..], out var n) ? n : 0);
    }

    /// <summary>Portni ochadi va ulanishni saqlab qoladi.</summary>
    public static void Connect(string portName, int baud)
    {
        lock (Lock)
        {
            Disconnect();
            var p = new SerialPort(portName, baud, Parity.None, 8, StopBits.One) { ReadTimeout = 50, WriteTimeout = 1000 };
            try
            {
                p.Open();
                p.DiscardInBuffer();
                p.DiscardOutBuffer();
            }
            catch (Exception e)
            {
                p.Dispose();
                throw new ProtocolException("err_open", e.Message);
            }
            _port = p;
        }
    }

    public static void Disconnect()
    {
        lock (Lock)
        {
            if (_port is null) return;
            try { _port.Close(); } catch { /* port allaqachon yo'qolgan bo'lishi mumkin */ }
            _port.Dispose();
            _port = null;
        }
    }

    /// <summary>Ochiq port orqali kadrni yuboradi va <paramref name="timeoutMs"/> ichida javob kadrini kutadi.
    /// I/O xatosida ulanish yopiladi (port yo'qolgan bo'lishi mumkin).</summary>
    public static Exchange Transact(byte[] frame, int timeoutMs)
    {
        lock (Lock)
        {
            var port = _port;
            if (port is not { IsOpen: true }) { Disconnect(); throw new ProtocolException("err_not_connected"); }

            try
            {
                port.DiscardInBuffer();
                port.DiscardOutBuffer();
                port.Write(frame, 0, frame.Length);
            }
            catch (Exception e)
            {
                Disconnect();
                throw new ProtocolException("err_write", e.Message);
            }

            var buf = new List<byte>();
            var chunk = new byte[256];
            var sw = Stopwatch.StartNew();

            while (sw.ElapsedMilliseconds < timeoutMs)
            {
                int n;
                try
                {
                    n = port.Read(chunk, 0, chunk.Length);
                }
                catch (TimeoutException)
                {
                    continue;
                }
                catch (Exception e)
                {
                    Disconnect();
                    throw new ProtocolException("err_read", e.Message);
                }
                if (n <= 0) continue;

                buf.AddRange(chunk.AsSpan(0, n).ToArray());
                var span = buf.ToArray();
                try
                {
                    var parsed = Protocol.ParseFrame(span);
                    if (parsed is { } p)
                        return new Exchange(Protocol.Hex(frame), Protocol.Hex(span), p.Cmd, p.Payload);
                }
                catch (ProtocolException e)
                {
                    throw new ProtocolException(e.Key, Protocol.Hex(span));
                }
            }

            return new Exchange(Protocol.Hex(frame), Protocol.Hex(buf.ToArray()), null, []);
        }
    }
}
