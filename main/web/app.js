'use strict';

/* ─── State ─────────────────────────────────────────────────── */
let token = localStorage.getItem('rb_token') || '';
let currentPage = 'main';
let pollTimer = null;
let currentPcState = 'offline';

/* ─── Toast ──────────────────────────────────────────────────── */
function toast(msg, type = 'info') {
  let el = document.getElementById('toast');
  if (!el) {
    el = document.createElement('div');
    el.id = 'toast';
    document.body.appendChild(el);
  }
  el.textContent = msg;
  el.style.borderColor = type === 'error' ? 'rgba(239,68,68,0.4)'
                       : type === 'ok'    ? 'rgba(34,197,94,0.4)'
                       : 'rgba(99,125,255,0.2)';
  el.classList.add('show');
  clearTimeout(el._t);
  el._t = setTimeout(() => el.classList.remove('show'), 2500);
}

/* ─── Log ────────────────────────────────────────────────────── */
function addLog(msg, type = '') {
  const box = document.getElementById('log');
  if (!box) return;
  const now = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  const entry = document.createElement('div');
  entry.className = 'log-entry';
  entry.innerHTML = `<span class="log-time">${now}</span><span class="${type}">${msg}</span>`;
  box.appendChild(entry);
  box.scrollTop = box.scrollHeight;
  // Keep max 50 entries
  while (box.children.length > 50) box.removeChild(box.firstChild);
}

function clearLog() {
  const box = document.getElementById('log');
  if (box) box.innerHTML = '';
}

/* ─── API calls ──────────────────────────────────────────────── */
async function api(method, path, body = null) {
  const opts = {
    method,
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${token}`,
    },
  };
  if (body) opts.body = JSON.stringify(body);
  const res = await fetch(path, opts);
  const data = await res.json().catch(() => ({}));
  if (!res.ok && res.status !== 409) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

/* ─── Status polling ─────────────────────────────────────────── */
const PC_STATE_LABELS = {
  offline:       '离线',
  powering_on:   '启动中…',
  post_running:  'POST 检测中…',
  selecting_os:  '选择系统…',
  booting:       '系统加载中…',
  online:        '在线',
  shutting_down: '关机中…',
  error:         '错误',
};
const NET_STATE_LABELS = {
  unknown:     '未知',
  ok:          '已认证',
  auth_needed: '需要认证',
  error:       '认证失败',
};
const OS_LABELS = { unknown: '—', windows: 'Windows', fedora: 'Fedora 43' };

function applyStatus(st) {
  // PC dot
  const dot = document.getElementById('pc-status-dot');
  const txt  = document.getElementById('pc-status-text');
  dot.className = 'status-dot';
  const s = st.pc_state;
  currentPcState = s; // 保存当前状态
  if (s === 'online')   dot.classList.add('status-online');
  else if (s === 'offline') dot.classList.add('status-offline');
  else if (s === 'error')   dot.classList.add('status-error');
  else                      dot.classList.add('status-busy');
  txt.textContent = PC_STATE_LABELS[s] || s;

  // Net dot
  const ndot = document.getElementById('net-dot');
  const ntxt  = document.getElementById('net-status-text');
  ndot.className = 'status-dot';
  if (st.campus_net === 'ok')       ndot.classList.add('status-ok');
  else if (st.campus_net === 'error') ndot.classList.add('status-error');
  else                                ndot.classList.add('status-unknown');
  ntxt.textContent = NET_STATE_LABELS[st.campus_net] || st.campus_net;

  // OS
  document.getElementById('os-text').textContent = OS_LABELS[st.os_booted] || '—';

  // WiFi indicator
  const wifiInd = document.getElementById('wifi-indicator');
  if (st.esp_ip) {
    wifiInd.innerHTML = `<span class="dot dot-green"></span><span class="indicator-text">${st.esp_ip}</span>`;
  }

  // Button states
  const isOffline  = s === 'offline' || s === 'error';
  const isOnline   = s === 'online';
  const isBusy     = !isOffline && !isOnline;

  const qOsBtn = (id) => document.getElementById(id);
  ['btn-win','btn-fed','btn-default'].forEach(id => {
    qOsBtn(id).disabled = !isOffline;
  });
  // BIOS 按钮在离线和在线时都可用（离线=开机进BIOS，在线=重启进BIOS）
  document.getElementById('btn-bios').disabled = isBusy;
  document.getElementById('btn-shutdown').disabled = !isOnline;
  document.getElementById('btn-reboot').disabled   = !isOnline;
}

async function pollStatus() {
  try {
    const st = await api('GET', '/api/status');
    applyStatus(st);
  } catch (e) {
    document.getElementById('pc-status-text').textContent = '无法连接';
  }
}

function startPolling() {
  pollStatus();
  pollTimer = setInterval(pollStatus, 3000);
}

/* ─── Actions ────────────────────────────────────────────────── */
async function doBoot(os) {
  if (!token) { showPage('config'); toast('请先设置 API Token', 'error'); return; }
  const osLabels = { windows: 'Windows', fedora: 'Fedora 43', default: '默认系统', bios: 'BIOS' };

  const isOnline = currentPcState === 'online';
  const endpoint = isOnline ? '/api/reboot_os' : '/api/boot';
  const action = isOnline ? '重启' : '开机';

  addLog(`${action}命令 → ${osLabels[os]}`, 'log-ok');
  try {
    await api('POST', endpoint, { os });
    toast(`正在${action}：${osLabels[os]}`, 'ok');
  } catch (e) {
    toast(`${action}失败: ` + e.message, 'error');
    addLog(`${action}失败: ` + e.message, 'log-err');
  }
}

async function doAction(action) {
  if (!token) { showPage('config'); return; }
  const labels = { shutdown: '关机', reboot: '重启' };
  addLog(`${labels[action]} 命令已发送`, 'log-warn');
  try {
    await api('POST', '/api/' + action);
    toast(labels[action] + ' 命令已发送', 'ok');
  } catch (e) {
    toast('失败: ' + e.message, 'error');
    addLog('请求失败: ' + e.message, 'log-err');
  }
}

function confirmForceOff() {
  openModal(
    '⚠️ 确认强制断电',
    '将长按电源键 4 秒强制关闭电源，可能导致数据丢失。确认继续？',
    async () => {
      addLog('强制断电命令已发送', 'log-err');
      try {
        await api('POST', '/api/force_off');
        toast('已强制断电', 'ok');
      } catch (e) {
        toast('失败: ' + e.message, 'error');
      }
    }
  );
}

async function doAuthNow() {
  try {
    await api('POST', '/api/auth_now');
    toast('已触发校园网认证', 'ok');
    addLog('手动触发校园网认证', '');
  } catch (e) {
    toast('失败: ' + e.message, 'error');
  }
}

/* ─── Config ─────────────────────────────────────────────────── */
async function loadConfig() {
  try {
    const cfg = await api('GET', '/api/config');
    setVal('cfg-wifi-ssid', cfg.wifi_ssid);
    setVal('cfg-pc-ip',     cfg.pc_ip);
    setVal('cfg-grub-win',  cfg.grub_win_idx);
    setVal('cfg-grub-fed',  cfg.grub_fed_idx);
    setVal('cfg-grub-default', cfg.grub_default);
    setVal('cfg-grub-wait', cfg.grub_wait_ms);
    setVal('cfg-ping-interval', cfg.ping_interval_s);
    setVal('cfg-hdd-quiet', cfg.hdd_quiet_ms);
  } catch(e) {
    // Token not set yet or not connected
  }
}

async function saveConfig() {
  // Save token to localStorage
  const newToken = getVal('cfg-token');
  if (newToken) {
    token = newToken;
    localStorage.setItem('rb_token', token);
  }

  const payload = {};
  const s = (k, v) => { if (v !== '') payload[k] = v; };
  const n = (k, v) => { const x = parseInt(v); if (!isNaN(x)) payload[k] = x; };

  s('wifi_ssid',       getVal('cfg-wifi-ssid'));
  s('wifi_pass',       getVal('cfg-wifi-pass'));
  s('campus_card',     getVal('cfg-campus-card'));
  s('campus_pass',     getVal('cfg-campus-pass'));
  s('pc_ip',           getVal('cfg-pc-ip'));
  n('grub_win_idx',    getVal('cfg-grub-win'));
  n('grub_fed_idx',    getVal('cfg-grub-fed'));
  n('grub_default',    getVal('cfg-grub-default'));
  n('grub_wait_ms',    getVal('cfg-grub-wait'));
  n('ping_interval_s', getVal('cfg-ping-interval'));
  n('hdd_quiet_ms',    getVal('cfg-hdd-quiet'));

  if (Object.keys(payload).length > 0) {
    try {
      await api('PUT', '/api/config', payload);
      toast('配置已保存', 'ok');
      addLog('配置已更新', 'log-ok');
    } catch(e) {
      toast('保存失败: ' + e.message, 'error');
      return;
    }
  } else {
    toast('配置已保存（Token）', 'ok');
  }
}

/* ─── Helpers ────────────────────────────────────────────────── */
function getVal(id) { return (document.getElementById(id)?.value || '').trim(); }
function setVal(id, v) { const el = document.getElementById(id); if (el && v != null) el.value = v; }

function toggleToken() {
  const inp = document.getElementById('cfg-token');
  inp.type = inp.type === 'password' ? 'text' : 'password';
}

/* ─── Page routing ───────────────────────────────────────────── */
function showPage(name) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.getElementById('page-' + name).classList.add('active');
  currentPage = name;
  if (name === 'config') loadConfig();
}

/* ─── Modal ──────────────────────────────────────────────────── */
let modalCb = null;
function openModal(title, body, onConfirm) {
  document.getElementById('modal-title').textContent = title;
  document.getElementById('modal-body').textContent  = body;
  document.getElementById('modal').classList.add('open');
  modalCb = onConfirm;
  document.getElementById('modal-confirm').onclick = () => {
    closeModal();
    if (modalCb) modalCb();
  };
}
function closeModal() {
  document.getElementById('modal').classList.remove('open');
}

/* ─── Init ───────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
  // Restore token from localStorage
  const savedToken = localStorage.getItem('rb_token');
  if (savedToken) {
    token = savedToken;
    document.getElementById('cfg-token').value = savedToken;
  }

  startPolling();

  // If no token, hint user
  if (!token) {
    addLog('未设置 Token，请进入配置页面设置', 'log-warn');
  }
});
