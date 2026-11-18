mod menu;
mod protocol;
mod serial;

use protocol::{Config, DeviceData};
use serde::Serialize;
use serial::{Link, PortInfo};
use tauri::{AppHandle, Emitter, Manager, State};

const DEFAULT_TIMEOUT_MS: u64 = 2500;

#[derive(Serialize)]
pub struct WriteResult {
    tx_hex: String,
    rx_hex: String,
    /// 0 = OK, 1 = CRC, 2 = noto'g'ri qiymat; None = javob kelmadi
    ack: Option<u8>,
}

#[derive(Serialize)]
pub struct ReadResult {
    tx_hex: String,
    rx_hex: String,
    data: Option<DeviceData>,
}

#[derive(Serialize)]
pub struct ConnInfo {
    port: String,
    baud: u32,
}

#[tauri::command]
fn list_ports() -> Vec<PortInfo> {
    serial::list_ports()
}

#[tauri::command]
fn preview_frame(cfg: Config) -> Result<String, String> {
    protocol::validate(&cfg)?;
    let frame = protocol::build_frame(protocol::CMD_SET_CONFIG, &protocol::encode_config(&cfg));
    Ok(protocol::hex(&frame))
}

/// Portni ochadi va ulanishni saqlab qoladi (keyingi so'rovlar shu port orqali).
#[tauri::command]
async fn connect(link: State<'_, Link>, port: String, baud: u32) -> Result<(), String> {
    let link = link.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        link.disconnect();
        link.connect(&port, baud)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn disconnect(link: State<'_, Link>) {
    link.disconnect();
}

#[tauri::command]
fn connection(link: State<'_, Link>) -> Option<ConnInfo> {
    link.info().map(|(port, baud)| ConnInfo { port, baud })
}

#[tauri::command]
async fn write_config(link: State<'_, Link>, cfg: Config, timeout: Option<u64>) -> Result<WriteResult, String> {
    protocol::validate(&cfg)?;
    let frame = protocol::build_frame(protocol::CMD_SET_CONFIG, &protocol::encode_config(&cfg));
    let link = link.inner().clone();
    let timeout = timeout.unwrap_or(DEFAULT_TIMEOUT_MS).clamp(200, 30_000);
    tauri::async_runtime::spawn_blocking(move || {
        let ex = link.transact(&frame, timeout)?;
        let ack = match ex.cmd {
            Some(protocol::CMD_SET_ACK) => ex.payload.first().copied(),
            Some(_) => return Err(format!("err_unexpected|{}", ex.rx_hex)),
            None => None,
        };
        Ok(WriteResult { tx_hex: ex.tx_hex, rx_hex: ex.rx_hex, ack })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn read_data(link: State<'_, Link>, timeout: Option<u64>) -> Result<ReadResult, String> {
    let frame = protocol::build_frame(protocol::CMD_GET_DATA, &[]);
    let link = link.inner().clone();
    let timeout = timeout.unwrap_or(DEFAULT_TIMEOUT_MS).clamp(200, 30_000);
    tauri::async_runtime::spawn_blocking(move || {
        let ex = link.transact(&frame, timeout)?;
        let data = match ex.cmd {
            Some(protocol::CMD_DATA) => Some(protocol::decode_device_data(&ex.payload)?),
            Some(_) => return Err(format!("err_unexpected|{}", ex.rx_hex)),
            None => None,
        };
        Ok(ReadResult { tx_hex: ex.tx_hex, rx_hex: ex.rx_hex, data })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn save_text(path: String, content: String) -> Result<(), String> {
    std::fs::write(&path, content).map_err(|e| format!("err_file|{}", e))
}

#[tauri::command]
fn load_text(path: String) -> Result<String, String> {
    std::fs::read_to_string(&path).map_err(|e| format!("err_file|{}", e))
}

/// Menyu tilini almashtiradi (menyu qaytadan quriladi).
#[tauri::command]
fn set_language(app: AppHandle, lang: String) -> Result<(), String> {
    let m = menu::build(&app, &lang).map_err(|e| e.to_string())?;
    app.set_menu(m).map_err(|e| e.to_string())?;
    Ok(())
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Link::default())
        .setup(|app| {
            let m = menu::build(app.handle(), "uz")?;
            app.set_menu(m)?;
            // Oynani monitorga sig'adigan qilib cheklaymiz (kichik ekranlar uchun)
            if let Some(w) = app.get_webview_window("main") {
                if let Ok(Some(mon)) = w.primary_monitor() {
                    let sf = mon.scale_factor();
                    let s = mon.size().to_logical::<f64>(sf);
                    let width = 1280f64.min(s.width - 40.0).max(1100.0);
                    let height = 800f64.min(s.height - 110.0).max(660.0);
                    let _ = w.set_size(tauri::LogicalSize::new(width, height));
                    let _ = w.center();
                }
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            // Oyna yopilsa port bo'shatilib, jarayon to'liq tugaydi (orqada qolib ketmasin)
            if let tauri::WindowEvent::CloseRequested { .. } = event {
                window.state::<Link>().disconnect();
                window.app_handle().exit(0);
            }
        })
        .on_menu_event(|app, event| {
            let id = event.id().0.clone();
            if id == "quit" {
                app.exit(0);
                return;
            }
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.emit("menu", id);
            }
        })
        .invoke_handler(tauri::generate_handler![
            list_ports,
            preview_frame,
            connect,
            disconnect,
            connection,
            write_config,
            read_data,
            save_text,
            load_text,
            set_language
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
