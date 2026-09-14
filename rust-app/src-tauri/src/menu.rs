//! Dastur menyusi: File / Edit / View / Help — 3 tilda.

use tauri::menu::{Menu, MenuBuilder, MenuItemBuilder, PredefinedMenuItem, SubmenuBuilder};
use tauri::{AppHandle, Runtime};

struct Labels {
    file: &'static str,
    open: &'static str,
    save: &'static str,
    quit: &'static str,
    edit: &'static str,
    reset: &'static str,
    copy_hex: &'static str,
    clear_log: &'static str,
    view: &'static str,
    language: &'static str,
    theme: &'static str,
    dark: &'static str,
    light: &'static str,
    sound: &'static str,
    help: &'static str,
    protocol: &'static str,
    about: &'static str,
}

const UZ: Labels = Labels {
    file: "Fayl",
    open: "Sozlamani ochish…",
    save: "Sozlamani saqlash…",
    quit: "Chiqish",
    edit: "Tahrirlash",
    reset: "Formani tozalash",
    copy_hex: "HEX kadrni nusxalash",
    clear_log: "Jurnalni tozalash",
    view: "Ko'rinish",
    language: "Til",
    theme: "Mavzu",
    dark: "Qorong'i",
    light: "Yorug'",
    sound: "Ovoz (yoqish/o'chirish)",
    help: "Yordam",
    protocol: "Protokol tavsifi",
    about: "Dastur haqida",
};

const RU: Labels = Labels {
    file: "Файл",
    open: "Открыть конфигурацию…",
    save: "Сохранить конфигурацию…",
    quit: "Выход",
    edit: "Правка",
    reset: "Очистить форму",
    copy_hex: "Копировать HEX-кадр",
    clear_log: "Очистить журнал",
    view: "Вид",
    language: "Язык",
    theme: "Тема",
    dark: "Тёмная",
    light: "Светлая",
    sound: "Звук (вкл/выкл)",
    help: "Справка",
    protocol: "Описание протокола",
    about: "О программе",
};

const EN: Labels = Labels {
    file: "File",
    open: "Open configuration…",
    save: "Save configuration…",
    quit: "Quit",
    edit: "Edit",
    reset: "Reset form",
    copy_hex: "Copy HEX frame",
    clear_log: "Clear log",
    view: "View",
    language: "Language",
    theme: "Theme",
    dark: "Dark",
    light: "Light",
    sound: "Sound (on/off)",
    help: "Help",
    protocol: "Protocol description",
    about: "About",
};

pub fn build<R: Runtime>(app: &AppHandle<R>, lang: &str) -> tauri::Result<Menu<R>> {
    let l = match lang {
        "ru" => &RU,
        "en" => &EN,
        _ => &UZ,
    };

    let file = SubmenuBuilder::new(app, l.file)
        .item(&MenuItemBuilder::with_id("open", l.open).accelerator("CmdOrCtrl+O").build(app)?)
        .item(&MenuItemBuilder::with_id("save", l.save).accelerator("CmdOrCtrl+S").build(app)?)
        .separator()
        .item(&MenuItemBuilder::with_id("quit", l.quit).accelerator("CmdOrCtrl+Q").build(app)?)
        .build()?;

    let edit = SubmenuBuilder::new(app, l.edit)
        .item(&PredefinedMenuItem::cut(app, None)?)
        .item(&PredefinedMenuItem::copy(app, None)?)
        .item(&PredefinedMenuItem::paste(app, None)?)
        .item(&PredefinedMenuItem::select_all(app, None)?)
        .separator()
        .item(&MenuItemBuilder::with_id("reset", l.reset).build(app)?)
        .item(&MenuItemBuilder::with_id("copy_hex", l.copy_hex).build(app)?)
        .item(&MenuItemBuilder::with_id("clear_log", l.clear_log).build(app)?)
        .build()?;

    let language = SubmenuBuilder::new(app, l.language)
        .item(&MenuItemBuilder::with_id("lang_uz", "O'zbekcha").build(app)?)
        .item(&MenuItemBuilder::with_id("lang_ru", "Русский").build(app)?)
        .item(&MenuItemBuilder::with_id("lang_en", "English").build(app)?)
        .build()?;

    let theme = SubmenuBuilder::new(app, l.theme)
        .item(&MenuItemBuilder::with_id("theme_dark", l.dark).build(app)?)
        .item(&MenuItemBuilder::with_id("theme_light", l.light).build(app)?)
        .build()?;

    let view = SubmenuBuilder::new(app, l.view)
        .item(&language)
        .item(&theme)
        .separator()
        .item(&MenuItemBuilder::with_id("sound", l.sound).build(app)?)
        .build()?;

    let help = SubmenuBuilder::new(app, l.help)
        .item(&MenuItemBuilder::with_id("protocol", l.protocol).accelerator("F1").build(app)?)
        .separator()
        .item(&MenuItemBuilder::with_id("about", l.about).build(app)?)
        .build()?;

    let mut mb = MenuBuilder::new(app);

    // macOS da birinchi menyu — dastur menyusi bo'lishi shart.
    #[cfg(target_os = "macos")]
    {
        let app_menu = SubmenuBuilder::new(app, "Uz-Meter")
            .item(&MenuItemBuilder::with_id("about", l.about).build(app)?)
            .separator()
            .item(&PredefinedMenuItem::hide(app, None)?)
            .item(&PredefinedMenuItem::hide_others(app, None)?)
            .item(&PredefinedMenuItem::show_all(app, None)?)
            .separator()
            .item(&MenuItemBuilder::with_id("quit", l.quit).accelerator("CmdOrCtrl+Q").build(app)?)
            .build()?;
        mb = mb.item(&app_menu);
    }

    mb = mb.item(&file).item(&edit).item(&view).item(&help);
    mb.build()
}
