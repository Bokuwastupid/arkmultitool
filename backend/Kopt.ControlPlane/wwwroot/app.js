const state = {
  token: sessionStorage.getItem('kopt_access'),
  refresh: sessionStorage.getItem('kopt_refresh'),
  view: 'overview',
  activeUser: null,
  refreshing: null
};
const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];

async function refreshSession() {
  if (!state.refresh) return false;
  if (!state.refreshing) {
    state.refreshing = fetch('/api/auth/refresh', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ refreshToken: state.refresh })
    }).then(async response => {
      if (!response.ok) return false;
      const result = await response.json();
      state.token = result.accessToken;
      state.refresh = result.refreshToken;
      sessionStorage.setItem('kopt_access', state.token);
      sessionStorage.setItem('kopt_refresh', state.refresh);
      return true;
    }).finally(() => state.refreshing = null);
  }
  return state.refreshing;
}

async function api(path, options = {}, retry = true) {
  const headers = { 'Content-Type': 'application/json', ...(options.headers || {}) };
  if (state.token) headers.Authorization = `Bearer ${state.token}`;
  const response = await fetch(path, { ...options, headers });
  if (response.status === 401 && retry && await refreshSession()) return api(path, options, false);
  if (response.status === 401) { showLogin(); throw new Error('Session expired'); }
  if (!response.ok) {
    let message = `Request failed (${response.status})`;
    try { const body = await response.json(); message = body.detail || body.title || message; } catch {}
    throw new Error(message);
  }
  if (response.status === 204) return null;
  return response.json();
}

function showLogin() {
  state.token = null; state.refresh = null;
  sessionStorage.removeItem('kopt_access'); sessionStorage.removeItem('kopt_refresh');
  $('#app').classList.add('hidden'); $('#login').classList.remove('hidden');
}
function showApp() { $('#login').classList.add('hidden'); $('#app').classList.remove('hidden'); loadView(state.view); }
function notice(text, bad = false) {
  const node = $('#notice'); node.textContent = text; node.classList.remove('hidden');
  node.style.borderColor = bad ? 'rgba(244,86,112,.35)' : '';
  setTimeout(() => node.classList.add('hidden'), 5000);
}
function cell(value) { const td = document.createElement('td'); td.textContent = value ?? '—'; return td; }
function row(values) { const tr = document.createElement('tr'); values.forEach(value => tr.append(cell(value))); return tr; }
function action(label, kind, id) {
  const button = document.createElement('button'); button.type = 'button';
  button.className = `table-action ${kind.startsWith('revoke') ? 'danger' : ''}`;
  button.textContent = label; button.dataset.action = kind; button.dataset.id = id; return button;
}
function actionCell(...buttons) {
  const td = document.createElement('td'); const wrap = document.createElement('div');
  wrap.className = 'action-row'; wrap.append(...buttons); td.append(wrap); return td;
}

async function loadProducts() {
  const products = await api('/api/admin/products');
  for (const selector of ['#product-id', '#release-product']) {
    $(selector).replaceChildren(...products.map(product => {
      const option = document.createElement('option'); option.value = product.id;
      option.textContent = `${product.name} · ${product.slug}`; return option;
    }));
  }
}
async function loadOverview() {
  const metrics = await api('/api/admin/dashboard');
  const items = [['Users', metrics.users, 'All accounts'], ['Active subscriptions', metrics.activeSubscriptions, `${metrics.expiringSevenDays} expire soon`], ['Active leases', metrics.activeLeases, 'Short-lived sessions'], ['Open incidents', metrics.openIncidents, `${metrics.quarantined} quarantined`]];
  const box = $('#metrics'); box.className = 'metrics';
  box.replaceChildren(...items.map(([label, value, note]) => {
    const node = document.createElement('article'); node.className = 'metric';
    const caption = document.createElement('span'); caption.textContent = label;
    const total = document.createElement('b'); total.textContent = value;
    const small = document.createElement('small'); small.textContent = note;
    node.append(caption, total, small); return node;
  }));
  await loadProducts();
}
async function loadUsers() {
  const query = encodeURIComponent($('#user-search').value || '');
  const users = await api(`/api/admin/users?page=1&query=${query}`);
  $('#users-body').replaceChildren(...users.map(user => {
    const tr = row([user.email, user.id, user.quarantined ? 'Quarantined' : 'Active']);
    tr.append(actionCell(action('Manage', 'manage-user', user.id))); return tr;
  }));
}
async function loadKeys() {
  const keys = await api('/api/admin/keys?page=1');
  $('#keys-body').replaceChildren(...keys.map(key => {
    const tr = row([key.id, key.durationDays, key.slotLimit, new Date(key.createdAt).toLocaleString(), key.revokedAt ? 'Revoked' : key.redeemedAt ? 'Redeemed' : 'Ready']);
    tr.append(key.revokedAt || key.redeemedAt ? cell('—') : actionCell(action('Revoke', 'revoke-key', key.id))); return tr;
  }));
}
async function loadIncidents() {
  const incidents = await api('/api/admin/incidents?state=open');
  $('#incidents-body').replaceChildren(...incidents.map(incident => {
    const tr = row([incident.id, incident.code, incident.riskScore, new Date(incident.createdAt).toLocaleString(), incident.resolvedAt ? 'Resolved' : 'Open']);
    tr.append(actionCell(action('Resolve', 'resolve-incident', incident.id))); return tr;
  }));
}
async function loadReleases() {
  await loadProducts();
  const releases = await api('/api/admin/releases');
  $('#releases-body').replaceChildren(...releases.map(release => {
    const tr = row([release.version, release.channel, `${release.rolloutPercent}%`, new Date(release.publishedAt).toLocaleString(), release.enabled ? 'Enabled' : 'Disabled']);
    tr.append(actionCell(action(release.enabled ? 'Disable' : 'Enable', 'release-state', release.id)));
    tr.dataset.enabled = release.enabled; tr.dataset.rollout = release.rolloutPercent; return tr;
  }));
}
async function loadAudit() {
  const events = await api('/api/admin/audit?page=1');
  $('#audit-body').replaceChildren(...events.map(event => row([new Date(event.createdAt).toLocaleString(), event.action, `${event.targetType} · ${event.targetId}`, event.reason])));
}
async function loadView(view) {
  state.view = view;
  $$('.view').forEach(node => node.classList.toggle('active', node.id === view));
  $$('#nav button').forEach(node => node.classList.toggle('active', node.dataset.view === view));
  $('#view-title').textContent = $(`#nav button[data-view="${view}"]`).textContent;
  try {
    if (view === 'overview') await loadOverview();
    if (view === 'users') await loadUsers();
    if (view === 'keys') await loadKeys();
    if (view === 'releases') await loadReleases();
    if (view === 'incidents') await loadIncidents();
    if (view === 'audit') await loadAudit();
  } catch (error) { notice(error.message, true); }
}

async function openUser(id) {
  const data = await api(`/api/admin/users/${id}`); state.activeUser = data;
  $('#user-title').textContent = data.user.email || data.user.id;
  $('#user-status').textContent = data.user.quarantined ? `Quarantined · ${data.user.quarantineReason || 'No reason'}` : 'Active';
  $('#user-quarantine').textContent = data.user.quarantined ? 'Release quarantine' : 'Quarantine';
  $('#user-subscriptions').replaceChildren(...data.subscriptions.map(subscription => {
    const tr = row([subscription.product, new Date(subscription.endsAt).toLocaleString(), subscription.slotLimit, subscription.active ? 'Active' : subscription.revokedAt ? 'Revoked' : 'Inactive']);
    tr.append(actionCell(action('+30d', 'extend-sub', subscription.id), action('-30d', 'shorten-sub', subscription.id), action('Revoke', 'revoke-sub', subscription.id))); return tr;
  }));
  $('#user-devices').replaceChildren(...data.devices.map(device => {
    const tr = row([device.displayName, new Date(device.lastSeenAt).toLocaleString(), device.revokedAt ? 'Revoked' : 'Active']);
    tr.append(actionCell(action('Reset slot', 'reset-device', device.id), action('Revoke', 'revoke-device', device.id))); return tr;
  }));
  $('#user-dialog').showModal();
}
function requiredReason() { const value = $('#user-reason').value.trim(); if (!value) throw new Error('Reason is required'); return value; }
async function userAction(kind, id) {
  const reason = requiredReason();
  if (kind === 'extend-sub' || kind === 'shorten-sub') await api(`/api/admin/subscriptions/${id}/adjust`, { method: 'POST', body: JSON.stringify({ deltaDays: kind === 'extend-sub' ? 30 : -30, reason }) });
  if (kind === 'revoke-sub') await api(`/api/admin/subscriptions/${id}/revoke`, { method: 'POST', body: JSON.stringify({ reason }) });
  if (kind === 'reset-device' || kind === 'revoke-device') await api(`/api/admin/devices/${id}/${kind === 'reset-device' ? 'reset' : 'revoke'}`, { method: 'POST', body: JSON.stringify({ reason }) });
  await openUser(state.activeUser.user.id); notice('Action applied and audited');
}

$('#login-form').addEventListener('submit', async event => {
  event.preventDefault(); $('#login-error').textContent = '';
  try {
    const result = await api('/api/auth/login?useCookies=false', { method: 'POST', body: JSON.stringify({ email: $('#email').value, password: $('#password').value }) });
    state.token = result.accessToken; state.refresh = result.refreshToken;
    sessionStorage.setItem('kopt_access', state.token); sessionStorage.setItem('kopt_refresh', state.refresh);
    $('#password').value = ''; showApp();
  } catch (error) { $('#login-error').textContent = error.message; }
});
$('#nav').addEventListener('click', event => { const button = event.target.closest('button[data-view]'); if (button) loadView(button.dataset.view); });
$('#refresh').addEventListener('click', () => loadView(state.view));
$('#logout').addEventListener('click', showLogin);
$('#user-search-button').addEventListener('click', loadUsers);
document.addEventListener('click', async event => {
  const button = event.target.closest('button[data-action]'); if (!button) return;
  try {
    if (button.dataset.action === 'manage-user') await openUser(button.dataset.id);
    else if (button.dataset.action === 'revoke-key') {
      const reason = prompt('Audit reason'); if (!reason) return;
      await api(`/api/admin/keys/${button.dataset.id}/revoke`, { method: 'POST', body: JSON.stringify({ reason }) }); await loadKeys();
    } else if (button.dataset.action === 'resolve-incident') {
      const reason = prompt('Resolution and audit reason'); if (!reason) return;
      await api(`/api/admin/incidents/${button.dataset.id}/resolve`, { method: 'POST', body: JSON.stringify({ resolution: reason, releaseQuarantine: false, revokeDevice: false, reason }) }); await loadIncidents();
    } else if (button.dataset.action === 'release-state') {
      const reason = prompt('Audit reason'); if (!reason) return;
      const tr = button.closest('tr'); const enabled = tr.dataset.enabled !== 'true';
      const entered = prompt('Rollout percent (0..100)', tr.dataset.rollout); if (entered === null) return;
      await api(`/api/admin/releases/${button.dataset.id}/state`, { method: 'POST', body: JSON.stringify({ enabled, rolloutPercent: +entered, reason }) }); await loadReleases();
    } else await userAction(button.dataset.action, button.dataset.id);
  } catch (error) { notice(error.message, true); }
});
$('#key-form').addEventListener('submit', async event => {
  event.preventDefault();
  try {
    const result = await api('/api/admin/keys/generate', { method: 'POST', body: JSON.stringify({ productId: $('#product-id').value, count: +$('#key-count').value, durationDays: +$('#key-days').value, slots: +$('#key-slots').value, reason: $('#key-reason').value }) });
    $('#generated-keys').textContent = result.keys.join('\n'); $('#keys-dialog').showModal(); await loadOverview();
  } catch (error) { notice(error.message, true); }
});
$('#release-form').addEventListener('submit', async event => {
  event.preventDefault();
  try {
    await api('/api/admin/releases', { method: 'POST', body: JSON.stringify({
      productId: $('#release-product').value, channel: $('#release-channel').value,
      version: $('#release-version').value, minimumLoaderVersion: $('#release-min-loader').value,
      manifestUri: $('#release-uri').value, manifestSha256: $('#release-sha').value,
      manifestSignature: $('#release-signature').value.trim(), rolloutPercent: +$('#release-rollout').value,
      changelog: $('#release-changelog').value, knownIssues: $('#release-known-issues').value,
      reason: $('#release-reason').value
    }) });
    notice('Signed release metadata published'); await loadReleases();
  } catch (error) { notice(error.message, true); }
});
$('#close-dialog').addEventListener('click', () => $('#keys-dialog').close());
$('#copy-keys').addEventListener('click', async () => { await navigator.clipboard.writeText($('#generated-keys').textContent); notice('Keys copied'); });
$('#close-user-dialog').addEventListener('click', () => $('#user-dialog').close());
$('#user-logout').addEventListener('click', async () => { try { await api(`/api/admin/users/${state.activeUser.user.id}/logout`, { method: 'POST', body: JSON.stringify({ reason: requiredReason() }) }); notice('All active leases revoked'); } catch (error) { notice(error.message, true); } });
$('#user-quarantine').addEventListener('click', async () => {
  try {
    const enabled = !state.activeUser.user.quarantined;
    await api(`/api/admin/users/${state.activeUser.user.id}/quarantine`, { method: 'POST', body: JSON.stringify({ enabled, reason: requiredReason() }) });
    await openUser(state.activeUser.user.id); notice(enabled ? 'User quarantined' : 'Quarantine released');
  } catch (error) { notice(error.message, true); }
});
setInterval(() => $('#clock').textContent = new Date().toLocaleString(), 1000);
$('#clock').textContent = new Date().toLocaleString();
if (state.token) showApp(); else showLogin();
