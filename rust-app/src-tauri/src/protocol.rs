//! Uz-Meter USB kadr protokoli (PROTOCOL.md ga qarang).

use serde::{Deserialize, Serialize};

pub const SOF0: u8 = 0xAA;
pub const SOF1: u8 = 0x55;

pub const CMD_SET_CONFIG: u8 = 0x01;
pub const CMD_GET_DATA: u8 = 0x02;
pub const CMD_SET_ACK: u8 = 0x81;
pub const CMD_DATA: u8 = 0x82;

pub const DEVICE_NO_LEN: usize = 15;
pub const M_NO_LEN: usize = 14;
pub const URL_MAX: usize = 128;
pub const CONFIG_FIXED_LEN: usize = 34;
pub const TELEMETRY_LEN: usize = 7;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    pub device_no: String,
    pub m_no: String,
    pub server_url: String,
    pub valve_open: bool,
    /// 0 = 1 L, 1 = 10 L, 2 = 100 L, 3 = 1000 L
    pub unit: u8,
    /// 1..24
    pub sends_per_day: u8,
    /// 0..5
    pub meter_type: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Telemetry {
    pub water_volume_l: u32,
    pub battery_mv: u16,
    pub battery_pct: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceData {
    pub config: Config,
    pub telemetry: Telemetry,
}

/// CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF).
pub fn crc16_modbus(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= b as u16;
        for _ in 0..8 {
            if crc & 1 != 0 {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    crc
}

fn all_digits(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|b| b.is_ascii_digit())
}

pub fn validate(cfg: &Config) -> Result<(), String> {
    if !all_digits(&cfg.device_no, DEVICE_NO_LEN) {
        return Err("err_device_no".into());
    }
    if !all_digits(&cfg.m_no, M_NO_LEN) {
        return Err("err_m_no".into());
    }
    if cfg.server_url.is_empty() || cfg.server_url.len() > URL_MAX || !cfg.server_url.is_ascii() {
        return Err("err_url".into());
    }
    if cfg.unit > 3 {
        return Err("err_unit".into());
    }
    if !(1..=24).contains(&cfg.sends_per_day) {
        return Err("err_sends".into());
    }
    if cfg.meter_type > 5 {
        return Err("err_type".into());
    }
    Ok(())
}

pub fn encode_config(cfg: &Config) -> Vec<u8> {
    let mut p = Vec::with_capacity(CONFIG_FIXED_LEN + cfg.server_url.len());
    p.extend_from_slice(cfg.device_no.as_bytes());
    p.extend_from_slice(cfg.m_no.as_bytes());
    p.push(cfg.valve_open as u8);
    p.push(cfg.unit);
    p.push(cfg.sends_per_day);
    p.push(cfg.meter_type);
    p.push(cfg.server_url.len() as u8);
    p.extend_from_slice(cfg.server_url.as_bytes());
    p
}

pub fn build_frame(cmd: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(payload.len() + 6);
    f.push(SOF0);
    f.push(SOF1);
    f.push(cmd);
    f.push(payload.len() as u8);
    f.extend_from_slice(payload);
    let crc = crc16_modbus(&f[2..]);
    f.push((crc & 0xFF) as u8);
    f.push((crc >> 8) as u8);
    f
}

/// Buferdan to'liq kadrni qidiradi. `Some((cmd, payload, consumed))` qaytaradi.
/// CRC noto'g'ri bo'lsa `Err`, kadr hali to'liq kelmagan bo'lsa `Ok(None)`.
pub fn parse_frame(buf: &[u8]) -> Result<Option<(u8, Vec<u8>, usize)>, String> {
    let mut crc_err = false;
    let mut i = 0;
    while i + 1 < buf.len() {
        if buf[i] == SOF0 && buf[i + 1] == SOF1 {
            if i + 4 > buf.len() {
                return Ok(None);
            }
            let cmd = buf[i + 2];
            let len = buf[i + 3] as usize;
            let end = i + 4 + len + 2;
            if end > buf.len() {
                return Ok(None);
            }
            let body = &buf[i + 2..i + 4 + len];
            let crc_rx = buf[end - 2] as u16 | ((buf[end - 1] as u16) << 8);
            if crc16_modbus(body) == crc_rx {
                return Ok(Some((cmd, buf[i + 4..i + 4 + len].to_vec(), end)));
            }
            // Noto'g'ri sinxronizatsiya bo'lishi mumkin — keyingi SOF dan davom etamiz
            crc_err = true;
        }
        i += 1;
    }
    if crc_err {
        return Err("err_crc".into());
    }
    Ok(None)
}

pub fn decode_config(p: &[u8]) -> Result<(Config, usize), String> {
    if p.len() < CONFIG_FIXED_LEN {
        return Err("err_short".into());
    }
    let url_len = p[33] as usize;
    if p.len() < CONFIG_FIXED_LEN + url_len {
        return Err("err_short".into());
    }
    let cfg = Config {
        device_no: String::from_utf8_lossy(&p[0..15]).into_owned(),
        m_no: String::from_utf8_lossy(&p[15..29]).into_owned(),
        valve_open: p[29] != 0,
        unit: p[30],
        sends_per_day: p[31],
        meter_type: p[32],
        server_url: String::from_utf8_lossy(&p[34..34 + url_len]).into_owned(),
    };
    Ok((cfg, CONFIG_FIXED_LEN + url_len))
}

pub fn decode_device_data(p: &[u8]) -> Result<DeviceData, String> {
    let (config, used) = decode_config(p)?;
    let t = &p[used..];
    if t.len() < TELEMETRY_LEN {
        return Err("err_short".into());
    }
    let telemetry = Telemetry {
        water_volume_l: u32::from_le_bytes([t[0], t[1], t[2], t[3]]),
        battery_mv: u16::from_le_bytes([t[4], t[5]]),
        battery_pct: t[6].min(100),
    };
    Ok(DeviceData { config, telemetry })
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02X}", b)).collect::<Vec<_>>().join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_known_vector() {
        // CRC-16/MODBUS("123456789") = 0x4B37
        assert_eq!(crc16_modbus(b"123456789"), 0x4B37);
    }

    #[test]
    fn resync_after_bad_sof() {
        // Buferda avval "soxta" SOF (CRC noto'g'ri), keyin haqiqiy kadr — haqiqiysi topilishi kerak
        let good = build_frame(CMD_SET_ACK, &[0]);
        let mut buf = vec![0xAA, 0x55, 0x81, 0x01, 0x00, 0x00, 0x00];
        buf.extend_from_slice(&good);
        let (cmd, p, _) = parse_frame(&buf).unwrap().unwrap();
        assert_eq!(cmd, CMD_SET_ACK);
        assert_eq!(p, vec![0]);
        // faqat buzilgan kadr bo'lsa — CRC xatosi
        assert_eq!(parse_frame(&buf[..7]).unwrap_err(), "err_crc");
        // to'liq kelmagan kadr — None
        assert!(parse_frame(&good[..4]).unwrap().is_none());
    }

    #[test]
    fn roundtrip() {
        let cfg = Config {
            device_no: "123456789012345".into(),
            m_no: "98765432109876".into(),
            server_url: "http://a.uz/".into(),
            valve_open: true,
            unit: 2,
            sends_per_day: 6,
            meter_type: 3,
        };
        validate(&cfg).unwrap();
        // .NET versiyasi (UzMeter --selftest) bilan bir xil kadr
        assert_eq!(
            hex(&build_frame(CMD_SET_CONFIG, &encode_config(&cfg))),
            "AA 55 01 2E 31 32 33 34 35 36 37 38 39 30 31 32 33 34 35 39 38 37 36 35 34 33 32 31 30 39 38 37 36 01 02 06 03 0C 68 74 74 70 3A 2F 2F 61 2E 75 7A 2F 58 DF"
        );
        let mut payload = encode_config(&cfg);
        payload.extend_from_slice(&[0x10, 0x27, 0, 0, 0xB0, 0x0E, 87]);
        let frame = build_frame(CMD_DATA, &payload);
        let (cmd, p, used) = parse_frame(&frame).unwrap().unwrap();
        assert_eq!(cmd, CMD_DATA);
        assert_eq!(used, frame.len());
        let d = decode_device_data(&p).unwrap();
        assert_eq!(d.config.device_no, cfg.device_no);
        assert_eq!(d.config.server_url, cfg.server_url);
        assert_eq!(d.telemetry.water_volume_l, 10000);
        assert_eq!(d.telemetry.battery_mv, 3760);
        assert_eq!(d.telemetry.battery_pct, 87);
    }
}
