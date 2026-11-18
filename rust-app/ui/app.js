/* UZMETER — frontend mantiq (Tauri 2) */
(() => {
  const { invoke } = window.__TAURI__.core;
  const { listen } = window.__TAURI__.event;
  const dialog = window.__TAURI__.dialog;

  const $ = (id) => document.getElementById(id);
  const SENDS = [1, 2, 3, 4, 6, 8, 12, 24];
  const PULSE_TYPE = 4; // "Tur 5 — Impulsli"
  const ULTRA_TYPE = 2; // "Tur 3 — Ultratovushli": qurilma raqami = klapan manzili, aynan 14 ta raqam
  const DEV_MAX = 15, DEV_ULTRA = 14;
  const isUltra = () => Number($("typeSel").value) === ULTRA_TYPE;
  const UNITS = ["1 L", "10 L", "100 L", "1000 L"];
  const VIEWS = ["home", "config", "device", "log", "settings", "help"];
  const SPLASH_MS = 3000;
  const PORT_POLL_MS = 3000;

  /* ===================== SOZLAMALAR (localStorage) ===================== */
  const DEFAULTS = { lang: "uz", theme: "light", sound: true, autofill: true, timeout: 2500, autoconnect: false, lastPort: "", lastBaud: "2400", autoScroll: true,
    defaultUrl: "https://cabinet.uzmeter.uz/dictionary/water",
    autoDev: false, devMin: "", devMax: "", devNext: "",
    autoM: false, mMin: "", mMax: "", mNext: "" };
  const loadSettings = () => {
    try { return { ...DEFAULTS, ...JSON.parse(localStorage.getItem("settings") || "{}") }; } catch (_) { return { ...DEFAULTS }; }
  };
  const settings = loadSettings();
  const saveSettings = () => localStorage.setItem("settings", JSON.stringify(settings));

  const state = {
    valve: 1,
    unit: 0,
    loaded: null,
    loadedAt: null,
    busy: false,
    conn: null,          // { port, baud, since }
    view: "home",
    logs: [],
    logFilter: "all",
    logSearch: "",
    frameTab: "hex",
    lastHex: "",
    knownPorts: null,
  };

  const t = (key, vars) => {
    let s = (window.I18N[settings.lang] || window.I18N.uz)[key];
    if (s === undefined) s = window.I18N.uz[key];
    if (s === undefined) s = key;
    if (vars && typeof s === "string") for (const k in vars) s = s.split(`{${k}}`).join(vars[k]);
    return s;
  };

  /* ===================== OVOZ (Web Audio, faylsiz) ===================== */
  let audioCtx = null;
  function ctx() {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === "suspended") audioCtx.resume().catch(() => {});
    return audioCtx;
  }
  /** Suv tomchisi: qisqa ko'tariluvchi "plink" + yumshoq past ohang */
  function playDrop(vol = 1) {
    if (!settings.sound) return;
    try {
      const ac = ctx(), t0 = ac.currentTime;
      const master = ac.createGain();
      master.gain.value = 0.22 * vol;
      master.connect(ac.destination);
      const tone = (f0, f1, dur, g, delay = 0, type = "sine") => {
        const o = ac.createOscillator(), e = ac.createGain();
        o.type = type;
        o.frequency.setValueAtTime(f0, t0 + delay);
        o.frequency.exponentialRampToValueAtTime(f1, t0 + delay + dur * 0.55);
        e.gain.setValueAtTime(0.0001, t0 + delay);
        e.gain.exponentialRampToValueAtTime(g, t0 + delay + 0.012);
        e.gain.exponentialRampToValueAtTime(0.0001, t0 + delay + dur);
        o.connect(e).connect(master);
        o.start(t0 + delay);
        o.stop(t0 + delay + dur + 0.05);
      };
      tone(520, 1480, 0.32, 1.0);
      tone(1040, 2960, 0.18, 0.18);
      tone(180, 120, 0.16, 0.35, 0, "triangle");
      tone(760, 1900, 0.22, 0.35, 0.19);
    } catch (_) { /* ovoz muhim emas */ }
  }
  function playOk() {
    if (!settings.sound) return;
    try {
      const ac = ctx(), t0 = ac.currentTime;
      [[880, 0], [1320, 0.09]].forEach(([f, d]) => {
        const o = ac.createOscillator(), e = ac.createGain();
        o.type = "sine"; o.frequency.value = f;
        e.gain.setValueAtTime(0.0001, t0 + d);
        e.gain.exponentialRampToValueAtTime(0.12, t0 + d + 0.01);
        e.gain.exponentialRampToValueAtTime(0.0001, t0 + d + 0.22);
        o.connect(e).connect(ac.destination); o.start(t0 + d); o.stop(t0 + d + 0.25);
      });
    } catch (_) {}
  }
  function playErr() {
    if (!settings.sound) return;
    try {
      const ac = ctx(), t0 = ac.currentTime;
      const o = ac.createOscillator(), e = ac.createGain();
      o.type = "triangle"; o.frequency.setValueAtTime(320, t0); o.frequency.exponentialRampToValueAtTime(180, t0 + 0.25);
      e.gain.setValueAtTime(0.0001, t0); e.gain.exponentialRampToValueAtTime(0.12, t0 + 0.01); e.gain.exponentialRampToValueAtTime(0.0001, t0 + 0.28);
      o.connect(e).connect(ac.destination); o.start(t0); o.stop(t0 + 0.3);
    } catch (_) {}
  }

  /* ===================== SPLASH ===================== */
  // Animatsiya tugamaguncha ilova ko'rinmaydi: avval splash to'liq so'nadi, keyin ilova ochiladi.
  let splashDone = false;
  function finishSplash() {
    if (splashDone) return;
    splashDone = true;
    const splash = $("splash");
    splash.classList.add("hide");
    let shown = false;
    const showApp = () => {
      if (shown) return;
      shown = true;
      splash.remove();
      $("app").classList.add("ready");
      if (settings.autoconnect && settings.lastPort) autoConnect();
    };
    splash.addEventListener("transitionend", showApp, { once: true });
    setTimeout(showApp, 650); // transitionend kelmasa ham ochilsin
  }
  function runSplash() {
    setTimeout(() => playDrop(1), 850); // tomchi "tushgan" payt
    setTimeout(finishSplash, SPLASH_MS);
    $("splash").addEventListener("click", finishSplash);
    document.addEventListener("keydown", (e) => { if (!splashDone && (e.key === "Escape" || e.key === "Enter" || e.key === " ")) finishSplash(); });
  }

  /* ===================== TIL / MAVZU ===================== */
  function applyLang() {
    document.documentElement.lang = settings.lang;
    document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
    document.querySelectorAll("[data-i18n-title]").forEach((el) => { el.title = t(el.dataset.i18nTitle); });
    document.querySelectorAll("[data-i18n-placeholder]").forEach((el) => { el.placeholder = t(el.dataset.i18nPlaceholder); });
    document.querySelectorAll("#langSeg button, #langSeg2 button").forEach((b) => b.classList.toggle("active", b.dataset.lang === settings.lang));

    const sendsSel = $("sendsSel");
    const prevSends = sendsSel.value || "6";
    sendsSel.innerHTML = SENDS.map((n) => `<option value="${n}">${n} ${t("per_day")} — ${t("every_h", { h: 24 / n })}</option>`).join("");
    if (!SENDS.includes(Number(prevSends))) addSendsOption(Number(prevSends));
    sendsSel.value = prevSends;

    const typeSel = $("typeSel");
    const prevType = typeSel.value || "0";
    typeSel.innerHTML = t("meter_types").map((n, i) => `<option value="${i}">${n}</option>`).join("");
    typeSel.value = prevType;

    if ($("portSel").options.length && !$("portSel").value) $("portSel").options[0].textContent = t("no_ports");
    renderProtocol();
    updateInitField();
    renderAutoSub();
    renderDevice();
    renderConn();
    renderStatus();
    renderLog();
    if (state.frameTab === "struct") renderStruct();
    invoke("set_language", { lang: settings.lang }).catch(() => {});
  }
  function setLang(l) { settings.lang = l; saveSettings(); applyLang(); }

  function applyTheme() {
    document.documentElement.dataset.theme = settings.theme;
    document.querySelectorAll("#themeSeg button, #themeSeg2 button").forEach((b) => b.classList.toggle("active", b.dataset.theme === settings.theme));
    $("aboutLogo").src = settings.theme === "light" ? "assets/logo_dark.png" : "assets/logo.png";
  }
  function setTheme(th) { settings.theme = th; saveSettings(); applyTheme(); }

  function applySettingsUI() {
    $("soundChk").checked = settings.sound;
    $("autofillChk").checked = settings.autofill;
    $("autoconnectChk").checked = settings.autoconnect;
    $("timeoutSel").value = String(settings.timeout);
    $("autoScroll").checked = settings.autoScroll;
    $("defaultUrl").value = settings.defaultUrl;
  }
  function toggleSound(on) {
    settings.sound = on ?? !settings.sound;
    saveSettings();
    $("soundChk").checked = settings.sound;
    toast(t(settings.sound ? "sound_on" : "sound_off"));
    if (settings.sound) playDrop(0.7);
  }

  /* ===================== KO'RINISHLAR ===================== */
  function setView(v) {
    if (!VIEWS.includes(v)) return;
    state.view = v;
    $("main").dataset.view = v;
    document.querySelectorAll(".nav").forEach((b) => b.classList.toggle("active", b.dataset.view === v));
    $("settingsBtn").classList.toggle("active", v === "settings");
    $("helpBtn").classList.toggle("active", v === "help");
    if (v === "log") scrollLog(true);
  }

  /* ===================== YORDAMCHILAR ===================== */
  let toastTimer;
  function toast(msg, kind = "") {
    const el = $("toast");
    el.textContent = msg;
    el.className = `toast show ${kind}`;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove("show"), 3200);
  }
  let statusKind = "";
  let statusMsg = null; // null → "Tizim tayyor"
  function setStatus(msg, kind = "") {
    statusMsg = msg;
    statusKind = kind;
    renderStatus();
  }
  function renderStatus() {
    $("statusText").textContent = statusMsg ?? t("status_ready");
    $("statusDot").className = "s-dot " + (statusKind || "ok");
    $("statusConn").textContent = t(state.conn ? "status_connected" : "status_disconnected");
    $("statusConnDot").className = "s-dot " + (state.conn ? "ok" : "");
    $("statusPort").textContent = state.conn ? state.conn.port : ($("portSel").value || "—");
    $("statusBaud").textContent = `${state.conn ? state.conn.baud : $("baudSel").value} baud`;
  }
  const escapeHtml = (s) => String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
  const pad2 = (n) => String(n).padStart(2, "0");
  const fmtTime = (d) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  const fmtDate = (d) => `${pad2(d.getDate())}.${pad2(d.getMonth() + 1)}.${d.getFullYear()}`;
  const fmtDateTime = (d) => `${fmtDate(d)} ${fmtTime(d)}`;

  function translateError(e) {
    const s = String(e);
    const [key, ...rest] = s.split("|");
    const detail = rest.join("|");
    if (window.I18N.uz[key] === undefined) return s;
    return detail ? `${t(key)}: ${detail}` : t(key);
  }

  /* ===================== JURNAL ===================== */
  const LEVELS = { info: "INFO", tx: "TX", rx: "RX", warn: "WARN", error: "ERROR" };
  function log(level, msg) {
    const entry = { ts: new Date(), level, msg: String(msg) };
    state.logs.push(entry);
    if (state.logs.length > 2000) state.logs.shift();
    if (matches(entry)) {
      const el = $("log");
      const empty = el.querySelector(".empty");
      if (empty) empty.remove();
      el.appendChild(logRow(entry));
      scrollLog();
    }
    renderLogCounts();
  }
  const matches = (e) => (state.logFilter === "all" || e.level === state.logFilter) && (!state.logSearch || e.msg.toLowerCase().includes(state.logSearch));
  function logRow(e) {
    const row = document.createElement("div");
    row.className = `row ${e.level}`;
    row.innerHTML = `<span class="t">${fmtTime(e.ts)}</span><span class="lv">${LEVELS[e.level]}</span><span class="m">${escapeHtml(e.msg)}</span>`;
    return row;
  }
  function renderLog() {
    const el = $("log");
    const rows = state.logs.filter(matches);
    el.innerHTML = rows.length ? "" : `<div class="empty">${escapeHtml(t("log_empty"))}</div>`;
    const frag = document.createDocumentFragment();
    rows.forEach((e) => frag.appendChild(logRow(e)));
    el.appendChild(frag);
    renderLogCounts();
    scrollLog(true);
  }
  function renderLogCounts() {
    const c = { all: state.logs.length, info: 0, tx: 0, rx: 0, warn: 0, error: 0 };
    state.logs.forEach((e) => c[e.level]++);
    $("cAll").textContent = c.all; $("cInfo").textContent = c.info; $("cTx").textContent = c.tx;
    $("cRx").textContent = c.rx; $("cWarn").textContent = c.warn; $("cError").textContent = c.error;
    const badge = $("navLogCount");
    badge.textContent = c.error || c.warn ? c.error + c.warn : c.all;
    badge.hidden = c.all === 0;
    badge.style.color = c.error ? "var(--danger)" : "";
  }
  function scrollLog(force = false) {
    if (force || settings.autoScroll) { const el = $("log"); el.scrollTop = el.scrollHeight; }
  }
  function clearLog() { state.logs = []; renderLog(); }
  const logText = () => state.logs.filter(matches).map((e) => `${fmtDateTime(e.ts)}  ${LEVELS[e.level].padEnd(5)}  ${e.msg}`).join("\n");
  async function copyLog() {
    try { await navigator.clipboard.writeText(logText()); toast(t("copied"), "ok"); } catch (e) { toast(String(e), "err"); }
  }
  async function saveLog() {
    try {
      const d = new Date();
      const path = await dialog.save({ defaultPath: `uz-meter-log-${d.getFullYear()}${pad2(d.getMonth() + 1)}${pad2(d.getDate())}.txt`, filters: [{ name: "Text", extensions: ["txt"] }] });
      if (!path) return;
      await invoke("save_text", { path, content: logText() + "\n" });
      toast(t("log_saved"), "ok");
    } catch (e) { toast(translateError(e), "err"); }
  }

  /* ===================== ULANISH ===================== */
  function renderConn() {
    const bar = $("connbar");
    const c = state.conn;
    bar.classList.toggle("on", !!c && !state.busy);
    bar.classList.toggle("busy", state.busy);
    $("connectBtn").hidden = !!c;
    $("disconnectBtn").hidden = !c;
    if (state.busy) {
      $("connTitle").textContent = t("conn_busy_title");
      $("connSub").textContent = t("conn_busy_sub", { port: c ? c.port : "—" });
    } else if (c) {
      $("connTitle").textContent = t("conn_on_title");
      $("connSub").textContent = t("conn_on_sub", { port: c.port });
    } else {
      $("connTitle").textContent = t("conn_off_title");
      $("connSub").textContent = t("conn_off_sub");
    }
    $("connSince").innerHTML = c ? escapeHtml(t("connected_at", { t: fmtDateTime(c.since) })) : "&nbsp;";
    $("connectBtn").disabled = state.busy || !$("portSel").value;
    $("disconnectBtn").disabled = state.busy;
    $("sendBtn").disabled = state.busy;
    $("readBtn").disabled = state.busy;
    $("refreshBtn").disabled = state.busy || !!c;
    renderStatus();
  }
  function setBusy(b) { state.busy = b; renderConn(); }

  async function refreshPorts(manual = false) {
    const sel = $("portSel");
    const prev = sel.value || settings.lastPort;
    try {
      if (manual) log("info", t("log_searching"));
      const ports = await invoke("list_ports");
      const names = ports.map((p) => p.name);
      const changed = !state.knownPorts || names.join() !== state.knownPorts.join();
      state.knownPorts = names;
      if (changed || manual) {
        sel.innerHTML = "";
        if (!ports.length) {
          sel.innerHTML = `<option value="">${escapeHtml(t("no_ports"))}</option>`;
        } else {
          for (const p of ports) {
            const o = document.createElement("option");
            o.value = p.name;
            o.textContent = p.description ? `${p.name} — ${p.description}` : p.name;
            sel.appendChild(o);
          }
          if (prev && names.includes(prev)) sel.value = prev;
        }
        if (changed || manual) log(ports.length ? "info" : "warn", ports.length ? t("log_found", { ports: names.join(", ") }) : t("log_none"));
      }
      // ulangan port yo'qolgan bo'lsa — uzamiz
      if (state.conn && !names.includes(state.conn.port) && !state.busy) {
        log("warn", t("log_port_lost"));
        await disconnect(true);
      }
    } catch (e) {
      log("error", translateError(e));
    }
    renderConn();
  }

  async function connect() {
    const port = $("portSel").value;
    if (!port) { toast(t("select_port"), "err"); playErr(); return; }
    const baud = Number($("baudSel").value);
    setBusy(true);
    $("connTitle").textContent = t("connecting");
    try {
      await invoke("connect", { port, baud });
      state.conn = { port, baud, since: new Date() };
      settings.lastPort = port; settings.lastBaud = String(baud); saveSettings();
      log("info", t("log_connected", { port, baud }));
      setStatus(t("status_connected"), "ok");
      playOk();
    } catch (e) {
      const msg = translateError(e);
      log("error", msg); toast(msg, "err"); setStatus(msg, "err"); playErr();
    } finally {
      setBusy(false);
    }
  }
  async function disconnect(silent = false) {
    const c = state.conn;
    try { await invoke("disconnect"); } catch (_) {}
    state.conn = null;
    if (c && !silent) log("info", t("log_disconnected", { port: c.port }));
    setStatus(t("status_disconnected"));
    renderConn();
  }
  async function autoConnect() {
    await refreshPorts();
    if ($("portSel").value === settings.lastPort) {
      $("baudSel").value = settings.lastBaud;
      await connect();
    }
  }

  /* ===================== AVTO RAQAM (oraliq bo'yicha, localStorage'da eslab qolinadi) ===================== */
  const AUTO = {
    Dev: { len: 15, varLen: true, field: "deviceNo", on: "autoDev", min: "devMin", max: "devMax", next: "devNext" },
    M: { len: 14, field: "mNo", on: "autoM", min: "mMin", max: "mMax", next: "mNext" },
  };
  const AUTO_KEYS = Object.values(AUTO).flatMap((a) => [a.on, a.min, a.max, a.next]);
  const big = (v) => BigInt(v);
  const padNo = (n, len) => n.toString().padStart(len, "0");
  /** null = oraliq kiritilmagan/noto'g'ri */
  function autoRange(a) {
    const mn = settings[a.min], mx = settings[a.max];
    if (!mn.length || mn.length > a.len || mx.length !== mn.length || big(mn) > big(mx)) return null;
    return [big(mn), big(mx)];
  }
  /** Avto raqam uzunligi: M_NO — doim 14; qurilma raqami — oraliq (min) uzunligi */
  const autoLen = (a) => (a.varLen && settings[a.min].length ? settings[a.min].length : a.len);
  /** Keyingi beriladigan raqam: "ok" | "exhausted" | "bad" */
  function autoPeek(a) {
    const r = autoRange(a);
    if (!r) return { st: "bad" };
    const nx = settings[a.next];
    // max dan keyingi qiymat (len+1 xona) ham saqlanadi — shunda oraliq tugaganini aniq bilamiz
    let n = /^\d{1,16}$/.test(nx) ? big(nx) : r[0];
    if (n < r[0]) n = r[0];
    if (n > r[1]) return { st: "exhausted" };
    return { st: "ok", value: padNo(n, autoLen(a)), n };
  }
  function renderAutoSub() {
    for (const [k, a] of Object.entries(AUTO)) {
      const el = $(`auto${k}Sub`);
      const p = autoPeek(a);
      const has = settings[a.min] || settings[a.max];
      el.textContent = p.st === "ok" ? `→ ${p.value}` : p.st === "exhausted" ? t("range_end") : has ? t("err_range") : "";
      el.style.color = p.st === "ok" || !has ? "" : "var(--danger)";
    }
  }
  /** Yoqilgan avto raqamlarni maydonlarga yozadi */
  function autoFillForm(quiet = false) {
    let changed = false;
    for (const a of Object.values(AUTO)) {
      if (!settings[a.on]) continue;
      const p = autoPeek(a);
      if (p.st === "ok") { $(a.field).value = p.value; changed = true; }
      else if (!quiet) toast(t(p.st === "exhausted" ? "range_end" : "err_range"), "err");
    }
    renderAutoSub();
    if (changed) onFormChange();
  }
  /** Raqamni 1 ga oshiradi (eslab qolinadi) */
  function autoAdvance(a) {
    const p = autoPeek(a);
    if (p.st !== "ok") return;
    settings[a.next] = padNo(p.n + 1n, autoLen(a));
    saveSettings();
    $(`auto${a === AUTO.Dev ? "Dev" : "M"}Next`).value = settings[a.next];
  }
  /** Yozish muvaffaqiyatli bo'lgach: formadagi raqam avto raqam bo'lsa — keyingisiga o'tamiz */
  function autoAfterWrite() {
    let any = false;
    for (const a of Object.values(AUTO)) {
      if (!settings[a.on]) continue;
      const p = autoPeek(a);
      if (p.st === "ok" && $(a.field).value === p.value) { autoAdvance(a); any = true; }
    }
    if (any) {
      autoFillForm(true);
      // oraliq tugagan bo'lsa — foydalanuvchi bilishi shart (maydonda oxirgi raqam qoladi)
      if (Object.values(AUTO).some((a) => settings[a.on] && autoPeek(a).st === "exhausted")) toast(t("range_end"), "err");
    }
  }
  function nextNumber() {
    const on = Object.values(AUTO).filter((a) => settings[a.on]);
    if (!on.length) { toast(t("s_autoDev"), "err"); setView("settings"); return; }
    on.forEach(autoAdvance);
    autoFillForm();
  }
  function initAutoUI() {
    for (const [k, a] of Object.entries(AUTO)) {
      $(`auto${k}Chk`).checked = settings[a.on];
      for (const key of ["min", "max", "next"]) {
        const el = $(`auto${k}${key[0].toUpperCase()}${key.slice(1)}`);
        el.maxLength = a.len;
        el.value = settings[a[key]];
        el.addEventListener("input", () => {
          el.value = el.value.replace(/\D/g, "").slice(0, a.len);
          settings[a[key]] = el.value;
          if (key !== "next" && autoRange(a) && (settings[a.next].length !== autoLen(a) || big(settings[a.next]) < autoRange(a)[0])) {
            settings[a.next] = settings[a.min];
            $(`auto${k}Next`).value = settings[a.next];
          }
          saveSettings();
          renderAutoSub();
        });
        el.addEventListener("change", () => autoFillForm(true));
      }
      $(`auto${k}Chk`).addEventListener("change", (e) => {
        settings[a.on] = e.target.checked;
        saveSettings();
        if (settings[a.on]) autoFillForm();
      });
    }
    renderAutoSub();
  }

  /* ===================== FORMA ===================== */
  function getConfig() {
    return {
      device_no: $("deviceNo").value.trim(),
      m_no: $("mNo").value.trim(),
      server_url: $("serverUrl").value.trim(),
      valve_open: state.valve === 1,
      unit: state.unit,
      sends_per_day: Number($("sendsSel").value),
      meter_type: Number($("typeSel").value),
      initial_value: initialValue(),
    };
  }
  /** Boshlang'ich qiymat (litr): faqat impulsli tur (4) va maydon to'ldirilgan bo'lsa */
  function initialValue() {
    if (Number($("typeSel").value) !== PULSE_TYPE) return null;
    const v = $("initialValue").value;
    return v === "" ? null : Math.min(4294967295, Number(v));
  }
  function updateInitField() {
    const pulse = Number($("typeSel").value) === PULSE_TYPE;
    $("initialValue").disabled = !pulse;
    $("initHint").textContent = t(pulse ? "hint_initial" : "hint_initial_off");
  }
  function addSendsOption(n) {
    const sel = $("sendsSel");
    if ([...sel.options].some((o) => Number(o.value) === n)) return;
    const o = document.createElement("option");
    o.value = n;
    o.textContent = `${n} ${t("per_day")}`;
    sel.appendChild(o);
  }
  function setConfig(c) {
    $("deviceNo").value = c.device_no || "";
    $("mNo").value = c.m_no || "";
    $("serverUrl").value = c.server_url || "";
    state.valve = c.valve_open ? 1 : 0;
    state.unit = Math.min(3, Math.max(0, c.unit | 0));
    const sends = Number(c.sends_per_day) || 6;
    if (!SENDS.includes(sends) && sends >= 1 && sends <= 24) addSendsOption(sends);
    $("sendsSel").value = String(sends);
    $("typeSel").value = String(Math.min(5, Math.max(0, c.meter_type | 0)));
    if (c.initial_value !== undefined) $("initialValue").value = c.initial_value == null ? "" : String(c.initial_value);
    syncSegs();
    onFormChange();
  }
  function syncSegs() {
    document.querySelectorAll("#valveSeg button").forEach((b) => b.classList.toggle("active", Number(b.dataset.val) === state.valve));
    document.querySelectorAll("#unitSeg button").forEach((b) => b.classList.toggle("active", Number(b.dataset.val) === state.unit));
  }
  const digitsOnly = (input, max) => { input.value = input.value.replace(/\D/g, "").slice(0, max); };

  function updateCounters() {
    const ultra = isUltra();
    // [id, max, rule]: "exact" — aynan max ta, "digits" — 1..max ta raqam, "url" — 1..max ta belgi
    for (const [id, max, rule] of [["deviceNo", ultra ? DEV_ULTRA : DEV_MAX, ultra ? "exact" : "digits"], ["mNo", 14, "exact"], ["serverUrl", 128, "url"]]) {
      const inp = $(id), v = inp.value;
      const ok = rule === "exact" ? v.length === max
        : rule === "digits" ? v.length > 0 && v.length <= max
        : v.length > 0 && v.length <= max && /^[!#-[\]-~]+$/.test(v);
      document.querySelectorAll(`[data-cnt="${id}"]`).forEach((c) => { c.textContent = `${v.length} / ${max}`; c.classList.toggle("ok", ok); });
      inp.classList.toggle("invalid", v.length > 0 && !ok);
      inp.parentElement.classList.toggle("ok", ok);
    }
  }
  let previewTimer;
  /** Qurilma raqami qoidasi tanlangan turga bog'liq: ultratovushlida aynan 14 ta raqam */
  function updateDevField() {
    const ultra = isUltra();
    const hint = document.querySelector('[data-i18n^="hint_device_no"]');
    hint.dataset.i18n = ultra ? "hint_device_no_ultra" : "hint_device_no";
    hint.textContent = t(hint.dataset.i18n);
    $("deviceNo").placeholder = "0".repeat(ultra ? DEV_ULTRA : DEV_MAX);
  }
  function onFormChange() {
    updateDevField();
    updateCounters();
    updateInitField();
    clearTimeout(previewTimer);
    previewTimer = setTimeout(recalc, 100);
  }
  async function recalc() {
    const pre = $("framePreview");
    try {
      const hex = await invoke("preview_frame", { cfg: getConfig() });
      state.lastHex = hex;
      pre.textContent = hex;
      pre.classList.remove("err");
      $("frameLen").textContent = `${hex.split(" ").length} ${t("bytes")}`;
    } catch (e) {
      state.lastHex = "";
      pre.textContent = translateError(e);
      pre.classList.add("err");
      $("frameLen").textContent = "";
    }
    if (state.frameTab === "struct") renderStruct();
  }
  const resetForm = () => setConfig({ device_no: "", m_no: "", server_url: settings.defaultUrl, valve_open: true, unit: 0, sends_per_day: 6, meter_type: 0, initial_value: null });
  const resetFormAuto = () => { resetForm(); autoFillForm(true); };
  /** "Tozalash": forma va o'qilgan qurilma ma'lumotlari tozalanadi (avto raqam o'zi to'ldirilmaydi) */
  function clearAll() {
    resetForm();
    state.loaded = null; state.loadedAt = null;
    renderDevice();
    setStatus(null);
    toast(t("clear_done"), "ok");
  }

  /** HEX kadrni maydonlarga ajratib jadval ko'rinishida chizadi (PROTOCOL.md → CONFIG). */
  function renderStruct() {
    const box = $("frameStruct");
    if (!state.lastHex) { box.innerHTML = `<div class="empty" style="padding:8px 10px;color:var(--danger)">${escapeHtml($("framePreview").textContent)}</div>`; return; }
    const b = state.lastHex.split(" ");
    // 0x00 — qisqa raqamning to'ldiruvchisi, ko'rsatilmaydi
    const ascii = (arr) => arr.map((h) => String.fromCharCode(parseInt(h, 16))).join("").replace(/\0+$/, "");
    const urlLen = parseInt(b[37], 16);
    const extra = b.length - (38 + urlLen) - 2; // CRC dan oldingi qo'shimcha baytlar (boshlang'ich qiymat)
    const rows = [
      [t("st_sof"), 0, 2, b.slice(0, 2), "AA 55"],
      [t("st_cmd"), 2, 1, b.slice(2, 3), "SET_CONFIG"],
      [t("st_lenf"), 3, 1, b.slice(3, 4), `${parseInt(b[3], 16)} ${t("bytes")}`],
      [t("device_no"), 4, 15, b.slice(4, 19), ascii(b.slice(4, 19))],
      [t("m_no"), 19, 14, b.slice(19, 33), ascii(b.slice(19, 33))],
      [t("valve"), 33, 1, b.slice(33, 34), b[33] === "01" ? t("valve_open") : t("valve_closed")],
      [t("unit"), 34, 1, b.slice(34, 35), UNITS[parseInt(b[34], 16)] ?? b[34]],
      [t("sends"), 35, 1, b.slice(35, 36), `${parseInt(b[35], 16)} ${t("per_day")}`],
      [t("meter_type"), 36, 1, b.slice(36, 37), t("meter_types")[parseInt(b[36], 16)] ?? b[36]],
      [t("st_url_len"), 37, 1, b.slice(37, 38), String(urlLen)],
      [t("server_url"), 38, urlLen, b.slice(38, 38 + urlLen), ascii(b.slice(38, 38 + urlLen))],
      ...(extra === 4 ? [[t("initial_value"), 38 + urlLen, 4, b.slice(38 + urlLen, 42 + urlLen), String(parseInt(b.slice(38 + urlLen, 42 + urlLen).reverse().join(""), 16))]] : []),
      [t("st_crc"), 38 + urlLen + (extra === 4 ? 4 : 0), 2, b.slice(-2), `0x${b[b.length - 1]}${b[b.length - 2]}`],
    ];
    box.innerHTML = `<table><thead><tr><th>${t("st_field")}</th><th>${t("st_off")}</th><th>${t("st_len")}</th><th>${t("st_bytes")}</th><th>${t("st_value")}</th></tr></thead><tbody>` +
      rows.map(([n, o, l, bytes, v]) => `<tr><td>${escapeHtml(n)}</td><td class="n">${o}</td><td class="n">${l}</td><td class="b" title="${escapeHtml(bytes.join(" "))}">${escapeHtml(bytes.join(" "))}</td><td>${escapeHtml(v)}</td></tr>`).join("") +
      `</tbody></table>`;
  }
  function setFrameTab(tab) {
    state.frameTab = tab;
    document.querySelectorAll("#frameTabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
    $("framePreview").hidden = tab !== "hex";
    $("frameStruct").hidden = tab !== "struct";
    if (tab === "struct") renderStruct();
  }
  async function copyHex() {
    if (!state.lastHex) { toast($("framePreview").textContent, "err"); return; }
    try { await navigator.clipboard.writeText(state.lastHex); toast(`HEX — ${t("copied")}`, "ok"); } catch (e) { toast(String(e), "err"); }
  }

  /* ===================== USB: YUBORISH ===================== */
  async function sendConfig() {
    if (!state.conn) { toast(t("not_connected"), "err"); playErr(); return; }
    setBusy(true);
    setStatus(t("status_sending"));
    try {
      const r = await invoke("write_config", { cfg: getConfig(), timeout: settings.timeout });
      log("tx", `${t("log_cmd_sent", { cmd: "0x01" })}: ${r.tx_hex}`);
      if (r.rx_hex) log("rx", r.rx_hex);
      if (r.ack === 0) { log("info", t("ack_ok")); log("info", t("ok_written")); toast(t("ok_written"), "ok"); setStatus(t("ok_written"), "ok"); playOk(); autoAfterWrite();
        // Boshlang'ich qiymat bir martalik: keyingi yozishda hisoblagich qayta o'rnatilib ketmasin
        if (getConfig().initial_value != null) { $("initialValue").value = ""; onFormChange(); log("info", t("log_initial_cleared")); } }
      else if (r.ack === 1) { log("error", t("ack_crc")); toast(t("ack_crc"), "err"); setStatus(t("ack_crc"), "err"); playErr(); }
      else if (r.ack === 2) { log("error", t("ack_invalid")); toast(t("ack_invalid"), "err"); setStatus(t("ack_invalid"), "err"); playErr(); }
      else { log("warn", t("no_ack")); toast(t("no_ack")); setStatus(t("no_ack"), "warn"); }
    } catch (e) {
      await handleIoError(e);
    } finally {
      setBusy(false);
    }
  }

  /* ===================== USB: O'QISH ===================== */
  async function readData() {
    if (!state.conn) { toast(t("not_connected"), "err"); playErr(); return; }
    setBusy(true);
    setStatus(t("status_reading"));
    try {
      const r = await invoke("read_data", { timeout: settings.timeout });
      log("tx", `${t("log_cmd_sent", { cmd: "0x02" })}: ${r.tx_hex}`);
      if (r.rx_hex) log("rx", r.rx_hex);
      if (r.data) {
        state.loaded = r.data;
        state.loadedAt = new Date();
        const m = r.data.telemetry;
        log("info", t("ok_read"));
        log("info", t("log_telemetry", { w: fmtM3(m.water_volume_l), r: m.reverse_volume_l == null ? "—" : fmtM3(m.reverse_volume_l), v: (m.battery_mv / 1000).toFixed(2), p: m.battery_pct }));
        if (m.battery_pct < 20) log("warn", t("log_batt_low", { p: m.battery_pct }));
        renderDevice(true);
        if (settings.autofill) { setConfig(r.data.config); toast(t("loaded_to_form"), "ok"); }
        else toast(t("ok_read"), "ok");
        setStatus(t("ok_read"), "ok");
        playOk();
      } else {
        log("warn", t("no_response"));
        toast(t("no_response"), "err"); setStatus(t("no_response"), "err"); playErr();
      }
    } catch (e) {
      await handleIoError(e);
    } finally {
      setBusy(false);
    }
  }
  async function handleIoError(e) {
    const msg = translateError(e);
    log("error", msg); toast(msg, "err"); setStatus(msg, "err"); playErr();
    const key = String(e).split("|")[0];
    if (key === "err_not_connected" || key === "err_write" || key === "err_read" || key === "err_open") {
      state.conn = null; // port yo'qolgan — backend ham uzgan
      renderConn();
    }
  }

  const fmtM3 = (l) => (l / 1000).toLocaleString(settings.lang === "en" ? "en-US" : "ru-RU", { minimumFractionDigits: 3, maximumFractionDigits: 3 });

  function renderDevice(fresh = false) {
    const d = state.loaded;
    const cells = ["dDeviceNo", "dMNo", "dUrl", "dType", "dValve", "dUnit", "dSends", "dLast"];
    $("applyBtn").disabled = !d;
    if (!d) {
      cells.forEach((id) => { $(id).textContent = "—"; $(id).classList.add("empty"); $(id).classList.remove("fresh"); $(id).title = ""; });
      $("tWater").textContent = "—"; $("tRev").textContent = "—"; $("tBattV").textContent = "—"; $("tBattP").textContent = "—";
      $("battBar").style.width = "0%";
      $("dState").textContent = t("state_none"); $("stateDot").className = "state-dot";
      return;
    }
    const c = d.config, m = d.telemetry;
    const vals = {
      dDeviceNo: c.device_no, dMNo: c.m_no, dUrl: c.server_url,
      dType: t("meter_types")[c.meter_type] ?? String(c.meter_type),
      dValve: c.valve_open ? t("valve_open") : t("valve_closed"),
      dUnit: UNITS[c.unit] ?? String(c.unit),
      dSends: `${c.sends_per_day} ${t("per_day")}`,
      dLast: state.loadedAt ? fmtDateTime(state.loadedAt) : "—",
    };
    for (const id of cells) { const el = $(id); el.textContent = vals[id]; el.title = vals[id]; el.classList.remove("empty"); el.classList.toggle("fresh", fresh); }
    $("tWater").textContent = fmtM3(m.water_volume_l);
    // Eski proshivka teskari oqimni yubormaydi — "—"
    $("tRev").textContent = m.reverse_volume_l == null ? "—" : fmtM3(m.reverse_volume_l);
    $("tBattV").textContent = (m.battery_mv / 1000).toFixed(2);
    $("tBattP").textContent = m.battery_pct;
    const bar = $("battBar");
    bar.style.width = `${m.battery_pct}%`;
    bar.className = "bar-fill " + (m.battery_pct < 20 ? "low" : m.battery_pct < 50 ? "mid" : "");
    const low = m.battery_pct < 20;
    $("dState").textContent = low ? t("state_low") : t("state_ok");
    $("stateDot").className = "state-dot " + (low ? "warn" : "ok");
  }

  /* ===================== FAYL ===================== */
  async function saveToFile() {
    try {
      const path = await dialog.save({ defaultPath: "uz-meter-config.json", filters: [{ name: "JSON", extensions: ["json"] }] });
      if (!path) return;
      await invoke("save_text", { path, content: JSON.stringify(getConfig(), null, 2) });
      toast(t("saved"), "ok"); log("info", `${t("saved")}: ${path}`);
    } catch (e) { toast(translateError(e), "err"); }
  }
  async function loadFromFile() {
    try {
      const path = await dialog.open({ multiple: false, filters: [{ name: "JSON", extensions: ["json"] }] });
      if (!path) return;
      const text = await invoke("load_text", { path });
      setConfig(JSON.parse(text));
      toast(t("loaded"), "ok"); log("info", `${t("loaded")}: ${path}`);
      setView("config");
    } catch (e) { toast(translateError(e), "err"); }
  }

  /* ===================== YORDAM ===================== */
  function renderProtocol() {
    const rows = (arr) => arr.map((r) => `<tr>${r.map((c, i) => (i === 0 ? `<td><code>${c}</code></td>` : `<td>${c}</td>`)).join("")}</tr>`).join("");
    $("protocolBody").innerHTML =
      `<b>Frame</b>: <code>AA 55</code> <code>CMD</code> <code>LEN</code> <code>PAYLOAD…</code> <code>CRC16 lo hi</code> (CRC-16/MODBUS over CMD+LEN+PAYLOAD)
<table><tr><th>CMD</th><th>Dir</th><th>Payload</th></tr>${rows([["0x01", "PC → device", "CONFIG"], ["0x02", "PC → device", "—"], ["0x81", "device → PC", "status: 0=OK 1=CRC 2=invalid"], ["0x82", "device → PC", "CONFIG + TELEMETRY"]])}</table><b>CONFIG</b>
<table><tr><th>Offset</th><th>Size</th><th>Field</th></tr>${rows([["0", "15", "device_no (ASCII, 1..15 digits, 0x00-padded; ultrasonic: 14 = valve address)"], ["15", "14", "m_no (ASCII digits)"], ["29", "1", "valve: 0=closed 1=open"], ["30", "1", "unit: 0=1L 1=10L 2=100L 3=1000L"], ["31", "1", "sends_per_day: 1..24"], ["32", "1", "meter_type: 0..5"], ["33", "1", "url_len N (≤128)"], ["34", "N", "server_url (ASCII)"], ["34+N", "4", "initial_value u32 LE, liters (optional, only 0x01 and meter_type=4 pulse)"]])}</table><b>TELEMETRY</b> (after CONFIG in 0x82)
<table><tr><th>Offset</th><th>Size</th><th>Field</th></tr>${rows([["0", "4", "water_volume, u32 LE, liters"], ["4", "2", "battery_mv, u16 LE"], ["6", "1", "battery_pct 0..100"], ["7", "4", "reverse_volume, u32 LE, liters (optional)"]])}</table>`;
  }

  /* ===================== MENYU (Rust dan) ===================== */
  listen("menu", (ev) => {
    const map = {
      open: loadFromFile, save: saveToFile, reset: resetFormAuto,
      copy_hex: copyHex,
      clear_log: clearLog,
      lang_uz: () => setLang("uz"), lang_ru: () => setLang("ru"), lang_en: () => setLang("en"),
      theme_dark: () => setTheme("dark"),
      theme_light: () => setTheme("light"),
      sound: () => toggleSound(),
      protocol: () => setView("help"), about: () => setView("help"),
    };
    map[ev.payload]?.();
  });

  /* ===================== HODISALAR ===================== */
  document.querySelectorAll("#langSeg, #langSeg2").forEach((seg) => seg.addEventListener("click", (e) => { const b = e.target.closest("button[data-lang]"); if (b) setLang(b.dataset.lang); }));
  document.querySelectorAll("#themeSeg, #themeSeg2").forEach((seg) => seg.addEventListener("click", (e) => { const b = e.target.closest("button[data-theme]"); if (b) setTheme(b.dataset.theme); }));
  document.querySelectorAll("[data-view]").forEach((b) => { if (b.tagName === "BUTTON") b.addEventListener("click", () => setView(b.dataset.view)); });

  $("refreshBtn").addEventListener("click", () => refreshPorts(true));
  $("portSel").addEventListener("change", renderConn);
  $("baudSel").addEventListener("change", () => { log("info", t("log_baud", { baud: $("baudSel").value })); renderConn(); });
  $("connectBtn").addEventListener("click", connect);
  $("disconnectBtn").addEventListener("click", () => disconnect());

  $("deviceNo").addEventListener("input", (e) => { digitsOnly(e.target, isUltra() ? DEV_ULTRA : DEV_MAX); onFormChange(); });
  $("mNo").addEventListener("input", (e) => { digitsOnly(e.target, 14); onFormChange(); });
  $("serverUrl").addEventListener("input", onFormChange);
  $("sendsSel").addEventListener("change", onFormChange);
  $("typeSel").addEventListener("change", onFormChange);
  $("initialValue").addEventListener("input", (e) => { digitsOnly(e.target, 10); onFormChange(); });
  $("nextNoBtn").addEventListener("click", nextNumber);
  $("clearBtn").addEventListener("click", clearAll);
  $("defaultUrl").addEventListener("input", (e) => {
    const old = settings.defaultUrl;
    const v = e.target.value.trim();
    settings.defaultUrl = v;
    saveSettings();
    // forma bo'sh yoki eski standart manzilda turgan bo'lsa — yangisiga almashtiramiz
    const cur = $("serverUrl").value.trim();
    if (cur === "" || cur === old) { $("serverUrl").value = v; onFormChange(); }
  });
  $("valveSeg").addEventListener("click", (e) => { const b = e.target.closest("button[data-val]"); if (!b) return; state.valve = Number(b.dataset.val); syncSegs(); onFormChange(); });
  $("unitSeg").addEventListener("click", (e) => { const b = e.target.closest("button[data-val]"); if (!b) return; state.unit = Number(b.dataset.val); syncSegs(); onFormChange(); });
  $("frameTabs").addEventListener("click", (e) => { const b = e.target.closest("button[data-tab]"); if (b) setFrameTab(b.dataset.tab); });
  $("copyHexBtn").addEventListener("click", copyHex);
  $("recalcBtn").addEventListener("click", () => { recalc(); toast(t("recalc") + " ✓"); });

  $("sendBtn").addEventListener("click", sendConfig);
  $("readBtn").addEventListener("click", readData);
  $("applyBtn").addEventListener("click", () => { if (state.loaded) { setConfig(state.loaded.config); toast(t("loaded_to_form"), "ok"); setView(state.view === "device" ? "config" : state.view); } });

  $("clearLogBtn").addEventListener("click", clearLog);
  $("copyLogBtn").addEventListener("click", copyLog);
  $("saveLogBtn").addEventListener("click", saveLog);
  $("logFilters").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-level]"); if (!b) return;
    state.logFilter = b.dataset.level;
    document.querySelectorAll("#logFilters .chip").forEach((c) => c.classList.toggle("active", c === b));
    renderLog();
  });
  $("logSearch").addEventListener("input", (e) => { state.logSearch = e.target.value.trim().toLowerCase(); renderLog(); });
  $("autoScroll").addEventListener("change", (e) => { settings.autoScroll = e.target.checked; saveSettings(); scrollLog(); });

  $("soundChk").addEventListener("change", (e) => toggleSound(e.target.checked));
  $("autofillChk").addEventListener("change", (e) => { settings.autofill = e.target.checked; saveSettings(); });
  $("autoconnectChk").addEventListener("change", (e) => { settings.autoconnect = e.target.checked; saveSettings(); });
  $("timeoutSel").addEventListener("change", (e) => { settings.timeout = Number(e.target.value); saveSettings(); });
  $("resetSettingsBtn").addEventListener("click", () => {
    // Avto raqam oraliqlari va oxirgi raqamlar o'chirilmaydi (ular qurilmalar tartibini saqlaydi)
    const keep = Object.fromEntries(AUTO_KEYS.map((k) => [k, settings[k]]));
    Object.assign(settings, DEFAULTS, keep); saveSettings();
    applyTheme(); applySettingsUI(); applyLang(); toast(t("settings_reset_done"), "ok");
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "F1") { e.preventDefault(); setView("help"); }
    if ((e.ctrlKey || e.metaKey) && e.key >= "1" && e.key <= "6") { e.preventDefault(); setView(VIEWS[Number(e.key) - 1]); }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") { e.preventDefault(); saveToFile(); }
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "o") { e.preventDefault(); loadFromFile(); }
  });
  document.addEventListener("contextmenu", (e) => { if (!e.target.closest("input, pre, .log, .v, .struct, .hex")) e.preventDefault(); });

  /* ===================== SOAT ===================== */
  const tick = () => { const d = new Date(); $("clock").textContent = `${fmtDate(d)} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`; };
  tick(); setInterval(tick, 1000);

  /* ===================== ISHGA TUSHIRISH ===================== */
  applyTheme();
  applySettingsUI();
  initAutoUI();
  applyLang();
  syncSegs();
  $("sendsSel").value = "6";
  $("baudSel").value = settings.lastBaud;
  resetForm();            // har safar toza boshlanadi (server manzili sozlamadan)
  autoFillForm(true);     // faqat foydalanuvchi yoqqan avto raqamlar
  onFormChange();
  setStatus(null);
  refreshPorts(true);
  setInterval(() => { if (!state.busy) refreshPorts(false); }, PORT_POLL_MS);
  runSplash();
})();
