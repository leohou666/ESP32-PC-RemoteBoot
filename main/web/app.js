'use strict';

/* ─── State ─────────────────────────────────────────────────── */
let token = localStorage.getItem('rb_token') || '';
let currentPage = 'main';
let pollTimer = null;
let currentPcState = 'offline';
let uploadInFlight = false;

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
  const box = document.getElementById('log-action');
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
  const tab = document.getElementById('tab-action');
  if (tab && tab.classList.contains('active')) {
    const box = document.getElementById('log-action');
    if (box) box.innerHTML = '';
  } else {
    // Clear system log via API
    api('GET', '/api/log?clear=1').then(refreshSystemLog).catch(() => {});
  }
}

/* ─── System Log (ring buffer) ─────────────────────────────────── */
let sysLogActive = false;
let sysLogTimer = null;
let sysLogEntries = [];  // cached for filtering

async function refreshSystemLog() {
  if (!token) return;
  try {
    const data = await api('GET', '/api/log?count=500');
    sysLogEntries = data.entries || [];
    const info = document.getElementById('log-info');
    if (info) info.textContent = `共 ${data.count} / ${data.capacity} 条`;
    renderLogEntries();
  } catch (e) {
    // ignore
  }
}

function applyLogFilter() {
  renderLogEntries();
}

function renderLogEntries() {
  const box = document.getElementById('log-system-entries');
  if (!box) return;
  const filter = (document.getElementById('log-filter')?.value || '').toLowerCase();
  const lvlFilter = document.getElementById('log-lvl-filter')?.value || '*';

  const lvlClass = { E: 'log-err', W: 'log-warn', I: '', D: 'log-debug', V: 'log-debug' };
  let html = '';
  for (const e of sysLogEntries) {
    if (lvlFilter !== '*' && String.fromCharCode(e.lvl) !== lvlFilter) continue;
    const text = (e.tag + ' ' + e.msg).toLowerCase();
    if (filter && text.indexOf(filter) === -1) continue;

    const ts = new Date(e.ts);
    const time = ts.toLocaleTimeString('zh-CN', { hour12: false });
    const cls = lvlClass[String.fromCharCode(e.lvl)] || '';
    const tagEsc = escHtml(e.tag);
    const msgEsc = escHtml(e.msg);
    html += `<div class="log-entry"><span class="log-time">${time}</span><span class="${cls}">[${String.fromCharCode(e.lvl)}] ${tagEsc}: ${msgEsc}</span></div>`;
  }
  box.innerHTML = html || '<div class="log-entry"><span class="log-time">—</span>暂无匹配日志</div>';
  box.scrollTop = box.scrollHeight;
}

function escHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function switchLogTab(tab) {
  const tabAction = document.getElementById('tab-action');
  const tabSystem = document.getElementById('tab-system');
  const boxAction = document.getElementById('log-action');
  const boxSystem = document.getElementById('log-system');
  const btnClear = document.getElementById('btn-log-clear');
  const btnRefresh = document.getElementById('btn-log-refresh');

  if (tab === 'system') {
    sysLogActive = true;
    tabAction.classList.remove('active');
    tabSystem.classList.add('active');
    boxAction.style.display = 'none';
    boxSystem.style.display = '';
    btnRefresh.style.display = '';
    btnClear.style.display = '';
    refreshSystemLog();
    sysLogTimer = setInterval(refreshSystemLog, 5000);
  } else {
    sysLogActive = false;
    tabSystem.classList.remove('active');
    tabAction.classList.add('active');
    boxSystem.style.display = 'none';
    boxAction.style.display = '';
    btnRefresh.style.display = 'none';
    btnClear.style.display = '';
    if (sysLogTimer) { clearInterval(sysLogTimer); sysLogTimer = null; }
  }
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
const OTA_STATE_LABELS = {
  pending_verify: '待确认',
  valid: '已确认',
  new: '新镜像',
  invalid: '无效',
  aborted: '已回滚',
  undefined: '未跟踪',
  unknown: '未知',
};

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
    setVal('cfg-btldr-type', cfg.btldr_type != null ? cfg.btldr_type : 1);
    setVal('cfg-relay-pol',  cfg.relay_pol != null ? cfg.relay_pol : 0);
    setVal('cfg-post-settle', cfg.post_settle != null ? cfg.post_settle : 2000);
    // Show/hide GRUB section based on bootloader type
    updateGrubVisibility();
  } catch(e) {
    // Token not set yet or not connected
  }

  await refreshUpdateInfo();
}

async function saveConfig() {
  syncTokenFromInput(true);

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
  n('btldr_type',      getVal('cfg-btldr-type'));
  n('relay_pol',       getVal('cfg-relay-pol'));
  n('post_settle',     getVal('cfg-post-settle'));

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

  await refreshUpdateInfo();
}

function updateGrubVisibility() {
  const btldrEl = document.getElementById('cfg-btldr-type');
  const grubSection = document.getElementById('grub-section');
  if (btldrEl && grubSection) {
    const isGrub = btldrEl.value === '0';
    grubSection.style.display = isGrub ? '' : 'none';
  }
}

/* ─── Helpers ────────────────────────────────────────────────── */
function getVal(id) { return (document.getElementById(id)?.value || '').trim(); }
function setVal(id, v) { const el = document.getElementById(id); if (el && v != null) el.value = v; }
function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}
function setClassState(id, className, active) {
  const el = document.getElementById(id);
  if (el) el.classList.toggle(className, active);
}
function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let idx = 0;
  while (value >= 1024 && idx < units.length - 1) {
    value /= 1024;
    idx++;
  }
  return `${value.toFixed(idx === 0 ? 0 : 1)} ${units[idx]}`;
}

function setUpdateInfoReadStatus(message, tone = '') {
  setText('fw-read-status', message);
  setClassState('fw-read-status', 'is-ok', tone === 'ok');
  setClassState('fw-read-status', 'is-loading', tone === 'loading');
  setClassState('fw-read-status', 'is-error', tone === 'error');
}

function setUpdateInfoVisualState(tone = '') {
  setClassState('fw-state', 'is-loading', tone === 'loading');
  setClassState('fw-state', 'is-error', tone === 'error');
}

function setUpdateInfoLoading() {
  setText('fw-version', '—');
  setText('fw-build', '—');
  setText('fw-partitions', '—');
  setText('fw-state', '读取中…');
  setUpdateInfoVisualState('loading');
  setUpdateInfoReadStatus('正在读取固件信息…', 'loading');
}

function setUpdateInfoPlaceholder(message, detail = '—') {
  setText('fw-version', '—');
  setText('fw-build', detail || '—');
  setText('fw-partitions', '—');
  setText('fw-state', message);
  setUpdateInfoVisualState(message === '读取失败' ? 'error' : '');
}

function describeUpdateInfoError(err) {
  if (!err || !err.message) return '读取失败';
  if (err.message === 'unauthorized') return 'Token 无效或未设置';
  if (err.message === 'ota_info_failed') return '设备未返回 OTA 信息';
  return err.message;
}

function syncTokenFromInput(persist = false) {
  token = getVal('cfg-token');
  if (persist) {
    if (token) localStorage.setItem('rb_token', token);
    else localStorage.removeItem('rb_token');
  }
}

function applyUpdateInfo(info) {
  const version = info.version || '—';
  const state = OTA_STATE_LABELS[info.ota_state] || info.ota_state || '未知';
  const build = [info.project_name, info.build_date, info.build_time, info.idf_ver].filter(Boolean).join(' · ');
  const partitions = info.update_supported
    ? `${info.running_partition || '—'} → ${info.next_partition || '—'}`
    : '当前分区布局不支持 OTA';
  const now = new Date().toLocaleTimeString('zh-CN', { hour12: false });

  setText('fw-version', version);
  setText('fw-build', build || '—');
  setText('fw-partitions', partitions);
  setText('fw-state', info.rollback_pending ? `${state} / 等待验活` : state);
  setUpdateInfoVisualState('');
  setUpdateInfoReadStatus(`已于 ${now} 刷新，当前启动分区 ${info.boot_partition || '—'}`, 'ok');
}

async function loadUpdateInfo() {
  if (!token) {
    setUpdateInfoPlaceholder('未配置 Token');
    setUpdateInfoReadStatus('填写 Token 后可读取 OTA 信息');
    return;
  }

  try {
    setUpdateInfoLoading();
    const info = await api('GET', '/api/update/info');
    applyUpdateInfo(info);
  } catch (e) {
    const detail = describeUpdateInfoError(e);
    setUpdateInfoPlaceholder('读取失败', detail);
    setUpdateInfoReadStatus(detail, 'error');
  }
}

function refreshUpdateInfo() {
  syncTokenFromInput(false);
  return loadUpdateInfo();
}

function setFirmwareProgress(percent, text) {
  const fill = document.getElementById('fw-progress-fill');
  const label = document.getElementById('fw-progress-text');
  if (fill) fill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  if (label) label.textContent = text;
}

function setFirmwareUploadBusy(busy) {
  uploadInFlight = busy;
  document.getElementById('btn-upload-fw').disabled = busy;
  document.getElementById('fw-file').disabled = busy;
}

function onFirmwareFileSelected() {
  const file = document.getElementById('fw-file')?.files?.[0];
  const meta = document.getElementById('fw-file-meta');
  if (!meta) return;

  if (!file) {
    meta.textContent = '未选择文件';
    setFirmwareProgress(0, '待上传');
    return;
  }

  meta.textContent = `${file.name} · ${formatBytes(file.size)}`;
  setFirmwareProgress(0, '待上传');
}

function uploadFirmware() {
  if (!token) {
    toast('请先设置 API Token', 'error');
    return;
  }
  if (uploadInFlight) return;

  const file = document.getElementById('fw-file')?.files?.[0];
  if (!file) {
    toast('请先选择固件文件', 'error');
    return;
  }

  openModal(
    '确认 OTA 升级',
    `将上传 ${file.name}（${formatBytes(file.size)}），设备随后会自动重启。确认继续？`,
    () => startFirmwareUpload(file)
  );
}

function startFirmwareUpload(file) {
  setFirmwareUploadBusy(true);
  setFirmwareProgress(1, '开始上传…');
  addLog(`开始 OTA 上传: ${file.name} (${formatBytes(file.size)})`, 'log-warn');

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/update');
  xhr.setRequestHeader('Authorization', `Bearer ${token}`);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  xhr.upload.onprogress = (evt) => {
    if (!evt.lengthComputable) return;
    const percent = Math.max(1, Math.round((evt.loaded / evt.total) * 100));
    setFirmwareProgress(percent, `上传中 ${percent}%`);
  };

  xhr.onload = async () => {
    setFirmwareUploadBusy(false);

    let data = {};
    try { data = JSON.parse(xhr.responseText || '{}'); } catch (_) {}

    if (xhr.status >= 200 && xhr.status < 300) {
      setFirmwareProgress(100, '上传完成，设备正在重启…');
      toast('固件已写入，设备正在重启', 'ok');
      addLog('OTA 上传完成，等待设备重启', 'log-ok');
      setTimeout(() => loadUpdateInfo(), 8000);
      return;
    }

    const msg = data.error || `HTTP ${xhr.status}`;
    setFirmwareProgress(0, `升级失败: ${msg}`);
    toast(`升级失败: ${msg}`, 'error');
    addLog(`OTA 上传失败: ${msg}`, 'log-err');
    await loadUpdateInfo();
  };

  xhr.onerror = async () => {
    setFirmwareUploadBusy(false);
    setFirmwareProgress(0, '连接中断');
    toast('上传中断，请检查网络后重试', 'error');
    addLog('OTA 上传中断', 'log-err');
    await loadUpdateInfo();
  };

  xhr.send(file);
}

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
  document.getElementById('cfg-token')?.addEventListener('change', refreshUpdateInfo);

  onFirmwareFileSelected();
  loadUpdateInfo();
  startPolling();

  // If no token, hint user
  if (!token) {
    addLog('未设置 Token，请进入配置页面设置', 'log-warn');
  }
});
