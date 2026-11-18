//! Serial port bilan ishlash: portlar ro'yxati, doimiy ulanish va so'rov/javob almashinuvi.

use crate::protocol::{self, hex};
use serde::Serialize;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Serialize)]
pub struct PortInfo {
    pub name: String,
    pub description: String,
}

pub fn list_ports() -> Vec<PortInfo> {
    let mut ports: Vec<PortInfo> = serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        // macOS: faqat /dev/cu.* ko'rsatamiz (tty.* dublikatlarini yashiramiz)
        .filter(|p| !(cfg!(target_os = "macos") && p.port_name.starts_with("/dev/tty.")))
        .map(|p| {
            let description = match &p.port_type {
                serialport::SerialPortType::UsbPort(u) => {
                    let parts: Vec<String> = [u.manufacturer.clone(), u.product.clone()]
                        .into_iter()
                        .flatten()
                        .collect();
                    if parts.is_empty() {
                        format!("USB {:04X}:{:04X}", u.vid, u.pid)
                    } else {
                        parts.join(" ")
                    }
                }
                serialport::SerialPortType::BluetoothPort => "Bluetooth".into(),
                serialport::SerialPortType::PciPort => "PCI".into(),
                serialport::SerialPortType::Unknown => String::new(),
            };
            PortInfo { name: p.port_name, description }
        })
        .collect();
    ports.sort_by_key(|p| natural_key(&p.name));
    ports.dedup_by(|a, b| a.name == b.name);
    ports
}

/// "COM10" > "COM9" bo'lishi uchun tabiiy tartib.
fn natural_key(s: &str) -> (String, u32) {
    let digits: String = s.chars().rev().take_while(|c| c.is_ascii_digit()).collect();
    let n = digits.chars().rev().collect::<String>().parse().unwrap_or(0);
    (s[..s.len() - digits.len()].to_string(), n)
}

pub struct Exchange {
    pub tx_hex: String,
    pub rx_hex: String,
    pub cmd: Option<u8>,
    pub payload: Vec<u8>,
}

/// Ochiq ulanish (port nomi, tezlik va portning o'zi).
pub struct Connection {
    pub name: String,
    pub baud: u32,
    port: Box<dyn serialport::SerialPort>,
}

/// Dastur bo'ylab bitta doimiy ulanish. `Connect` tugmasi ochadi, `Uzish` yopadi.
#[derive(Clone, Default)]
pub struct Link(pub Arc<Mutex<Option<Connection>>>);

impl Link {
    pub fn connect(&self, name: &str, baud: u32) -> Result<(), String> {
        let port = serialport::new(name, baud)
            .timeout(Duration::from_millis(50))
            .open()
            .map_err(|e| format!("err_open|{}", e))?;
        let _ = port.clear(serialport::ClearBuffer::All);
        *self.0.lock().unwrap() = Some(Connection { name: name.to_string(), baud, port });
        Ok(())
    }

    pub fn disconnect(&self) {
        *self.0.lock().unwrap() = None;
    }

    pub fn info(&self) -> Option<(String, u32)> {
        self.0.lock().unwrap().as_ref().map(|c| (c.name.clone(), c.baud))
    }

    /// Ochiq port orqali kadrni yuboradi va `timeout_ms` ichida javob kadrini kutadi.
    /// I/O xatosida ulanish yopiladi (port yo'qolgan bo'lishi mumkin).
    pub fn transact(&self, frame: &[u8], timeout_ms: u64) -> Result<Exchange, String> {
        let mut guard = self.0.lock().unwrap();
        let conn = guard.as_mut().ok_or_else(|| "err_not_connected".to_string())?;
        match transact_on(conn.port.as_mut(), frame, timeout_ms) {
            Ok(ex) => Ok(ex),
            Err(e) => {
                if e.starts_with("err_write") || e.starts_with("err_read") {
                    *guard = None;
                }
                Err(e)
            }
        }
    }
}

fn transact_on(port: &mut dyn serialport::SerialPort, frame: &[u8], timeout_ms: u64) -> Result<Exchange, String> {
    let _ = port.clear(serialport::ClearBuffer::All);
    port.write_all(frame).map_err(|e| format!("err_write|{}", e))?;
    port.flush().ok();

    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 256];
    let mut crc_err = false;

    while Instant::now() < deadline {
        match port.read(&mut chunk) {
            Ok(n) if n > 0 => {
                buf.extend_from_slice(&chunk[..n]);
                // IR ko'prigi o'z uzatishimizni aks-sado qilib qaytarishi mumkin (0x01/0x02 kadr) —
                // uni o'tkazib yuborib, haqiqiy javob (0x81/0x82) ni kutamiz.
                loop {
                    match protocol::parse_frame(&buf) {
                        Ok(Some((cmd, payload, used))) => {
                            if cmd & 0x80 != 0 {
                                return Ok(Exchange { tx_hex: hex(frame), rx_hex: hex(&buf), cmd: Some(cmd), payload });
                            }
                            buf.drain(..used.min(buf.len()));
                            crc_err = false;
                        }
                        Ok(None) => break,
                        Err(_) => {
                            // Buzilgan kadr: darhol to'xtamaymiz — javobning qolgan qismi kelishi mumkin.
                            crc_err = true;
                            break;
                        }
                    }
                }
            }
            Ok(_) => {}
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(format!("err_read|{}", e)),
        }
    }

    if crc_err {
        return Err(format!("err_crc|{}", hex(&buf)));
    }
    Ok(Exchange { tx_hex: hex(frame), rx_hex: hex(&buf), cmd: None, payload: Vec::new() })
}
