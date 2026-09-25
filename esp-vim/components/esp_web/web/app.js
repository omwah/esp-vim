// esp-vim web interface. Every call goes to /api/ on the device; anything that
// changes state carries the session's CSRF token.
"use strict";

const $ = (id) => document.getElementById(id);
let csrf = "";
let cwd = "/fat";
let statusTimer = null;

async function api(method, url, body, raw) {
  const opt = { method, headers: {}, credentials: "same-origin" };
  if (method !== "GET") opt.headers["X-CSRF-Token"] = csrf;
  if (body !== undefined) {
    if (raw) opt.body = body;
    else { opt.body = JSON.stringify(body); opt.headers["Content-Type"] = "application/json"; }
  }
  const r = await fetch(url, opt);
  const t = r.headers.get("Content-Type") || "";
  const data = t.includes("json") ? await r.json() : null;
  if (r.status === 401 && url !== "/api/login") { showLogin(); throw new Error("not logged in"); }
  if (!r.ok) throw new Error((data && data.error) || r.statusText);
  return data;
}

const q = (p) => encodeURIComponent(p);
const join = (dir, name) => (dir === "/" ? "" : dir) + "/" + name;
function size(n) {
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
  return (n / 1048576).toFixed(1) + " MB";
}
function when(t) { return t > 0 ? new Date(t * 1000).toISOString().slice(0, 16).replace("T", " ") : ""; }
function el(tag, attrs, text) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) e[k] = v;
  if (text !== undefined) e.textContent = text;
  return e;
}

// -------------------------------------------------------------- session --

function showLogin() {
  $("login").hidden = false; $("app").hidden = true; $("logout").hidden = true;
  if (statusTimer) { clearInterval(statusTimer); statusTimer = null; }
}

async function showApp() {
  $("login").hidden = true; $("app").hidden = false; $("logout").hidden = false;
  await list(cwd);
  await loadSettings();
  await status();
  if (!statusTimer) statusTimer = setInterval(status, 1000);
}

async function start() {
  const s = await api("GET", "/api/session");
  $("who").textContent = s.chip;
  if (s.logged_in) { csrf = s.csrf; await showApp(); } else showLogin();
}

$("loginform").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  $("loginerr").textContent = "";
  try {
    const r = await api("POST", "/api/login", { password: $("password").value });
    $("password").value = "";
    csrf = r.csrf;
    await showApp();
  } catch (e) { $("loginerr").textContent = e.message; }
});

$("logout").addEventListener("click", async () => {
  try { await api("POST", "/api/logout"); } catch (e) { /* already gone */ }
  csrf = ""; showLogin();
});

// --------------------------------------------------------------- status --

async function status() {
  try {
    const s = await api("GET", "/api/status");
    const box = $("status");
    box.replaceChildren();
    const add = (label, value) => {
      const d = el("div", { className: "stat" });
      d.append(el("span", { className: "mute" }, label), el("b", {}, String(value)));
      box.append(d);
    };
    add("File", (s.file || "[No Name]") + (s.modified ? " +" : ""));
    add("Line", s.line + " / " + s.lines);
    add("Column", s.col);
    add("Words", s.words);
    add("Characters", s.chars);
    add("Type", s.filetype || "-");
    add("Mode", s.mode || "-");
  } catch (e) { /* shown on the next successful poll */ }
}

// ---------------------------------------------------------------- files --

async function list(dir) {
  $("fileerr").textContent = "";
  let r;
  try { r = await api("GET", "/api/list?path=" + q(dir)); }
  catch (e) { $("fileerr").textContent = e.message; return; }
  cwd = r.path;
  $("cwd").textContent = cwd;
  $("space").textContent = r.total ? size(r.free) + " free of " + size(r.total) + (r.readonly ? " (read-only)" : "") : "";
  const tb = $("entries");
  tb.replaceChildren();
  const rows = r.entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
  for (const e of rows) {
    const tr = el("tr");
    const name = el("td");
    const path = join(cwd, e.name);
    if (e.dir) {
      const a = el("a", { className: "dir" }, e.name + "/");
      a.addEventListener("click", () => list(path));
      name.append(a);
    } else {
      name.append(el("a", { href: "/api/file?path=" + q(path) }, e.name));
    }
    const act = el("td", { className: "act" });
    if (cwd !== "/") {
      const ren = el("button", {}, "Rename");
      ren.addEventListener("click", () => rename(path, e.name));
      const del = el("button", {}, "Delete");
      del.addEventListener("click", () => remove(path, e.name, e.dir));
      act.append(ren, " ", del);
    }
    tr.append(name, el("td", { className: "num" }, e.dir ? "" : size(e.size)),
              el("td", { className: "num" }, when(e.mtime)), act);
    tb.append(tr);
  }
}

$("up").addEventListener("click", () => {
  if (cwd !== "/") list(cwd.replace(/\/[^/]*$/, "") || "/");
});

$("mkdir").addEventListener("click", async () => {
  const name = prompt("New folder name");
  if (!name) return;
  try { await api("POST", "/api/mkdir?path=" + q(join(cwd, name))); await list(cwd); }
  catch (e) { $("fileerr").textContent = e.message; }
});

async function rename(path, old) {
  const name = prompt("Rename " + old + " to", old);
  if (!name || name === old) return;
  try { await api("POST", "/api/move?from=" + q(path) + "&to=" + q(join(cwd, name))); await list(cwd); }
  catch (e) { $("fileerr").textContent = e.message; }
}

async function remove(path, name, dir) {
  if (!confirm("Delete " + name + (dir ? " and everything in it" : "") + "?")) return;
  try { await api("POST", "/api/delete?path=" + q(path)); await list(cwd); }
  catch (e) { $("fileerr").textContent = e.message; }
}

async function upload(files) {
  $("fileerr").textContent = "";
  for (const f of files) {
    const path = join(cwd, f.name);
    try { await api("PUT", "/api/file?path=" + q(path), f, true); }
    catch (e) {
      if (/exists/.test(e.message) && confirm(f.name + " exists. Replace it?")) {
        try { await api("PUT", "/api/file?path=" + q(path) + "&overwrite=1", f, true); }
        catch (e2) { $("fileerr").textContent = e2.message; }
      } else { $("fileerr").textContent = e.message; }
    }
  }
  await list(cwd);
}

$("files").addEventListener("change", (ev) => { upload(ev.target.files); ev.target.value = ""; });
const drop = $("drop");
drop.addEventListener("dragover", (ev) => { ev.preventDefault(); drop.classList.add("over"); });
drop.addEventListener("dragleave", () => drop.classList.remove("over"));
drop.addEventListener("drop", (ev) => { ev.preventDefault(); drop.classList.remove("over"); upload(ev.dataTransfer.files); });

// ------------------------------------------------------------- settings --

async function loadSettings() {
  const s = await api("GET", "/api/settings");
  const f = $("settings");
  f.replaceChildren();
  const num = (k, min, max) => {
    const l = el("label", {}, k + " ");
    l.append(el("input", { type: "number", name: k, min, max, value: s[k] }));
    f.append(l);
  };
  const bool = (k) => {
    const l = el("label");
    l.append(el("input", { type: "checkbox", name: k, checked: s[k] }), k);
    f.append(l);
  };
  const pick = (k, opts) => {
    const l = el("label", {}, k + " ");
    const sel = el("select", { name: k });
    for (const o of opts) sel.append(el("option", { value: o, selected: o === s[k] }, o || "(default)"));
    l.append(sel);
    f.append(l);
  };
  num("tabstop", 1, 16); num("shiftwidth", 0, 16);
  bool("expandtab"); bool("number"); bool("relativenumber"); bool("wrap");
  pick("colorscheme", [""].concat(s.colorschemes)); pick("background", ["", "light", "dark"]);
}

$("save").addEventListener("click", async () => {
  $("seterr").textContent = ""; $("saved").textContent = "";
  const body = {};
  for (const i of $("settings").elements) {
    if (!i.name) continue;
    body[i.name] = i.type === "checkbox" ? i.checked : i.type === "number" ? Number(i.value) : i.value;
  }
  try { await api("POST", "/api/settings", body); $("saved").textContent = "Saved; Vim applies them within a second."; }
  catch (e) { $("seterr").textContent = e.message; }
});

start().catch((e) => { $("loginerr").textContent = e.message; showLogin(); });
