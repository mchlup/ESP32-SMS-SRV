// script.js – kompletní opravená verze

// --- Inicializace ---
async function init() {
  try {
    const [config, contacts, templates, users] = await Promise.all([
      fetch('config.json').then(r => r.json()),
      fetch('contacts.json').then(r => r.json()),
      fetch('sms_templates.json').then(r => r.json()),
      fetch('users.json').then(r => r.json())
    ]);

    setupMenu(config.menu);
    setupModemStatus(config.api);
    setupSections(config.api);

    loadTemplatesData(templates);
    loadUsersData(users);
    loadContactsData(contacts);
    populateRecipientsSelect(contacts);

    setupCsvImport(contacts);
    setupCsvExport(contacts);
    setupLdapSync(config.api, contacts);
    //attachFormHandlers(config.api, contacts);
    setupSmsForm(config.api);
    setupSmsStatus(config.api);
    setupSmsHistory(config.api);
    setupMqttForm(config.api);
    setupSettingsForm(config.api);  // ← now passes the real API-endpoints object
    setupAtConsole(config.api);

    setupAddContact(contacts);
  } catch (err) {
    console.error('Chyba při inicializaci webu:', err);
  }
}
document.addEventListener('DOMContentLoaded', init);

function setupSmsStatus(api) {
  loadSmsStatus(api);
  // každých 3 vteřiny
  setInterval(() => loadSmsStatus(api), 3000);
}

// --- Menu ---
function setupMenu(menuItems) {
  const menuList = document.getElementById('menu-list');
  menuList.innerHTML = '';
  menuItems.forEach(item => {
    const li = document.createElement('li');
    const a  = document.createElement('a');
    a.href = '#';
    a.textContent = item.label;
    a.onclick = () => {
      showSection(item.id);
      document.querySelectorAll('#menu-list a')
              .forEach(el => el.classList.remove('active'));
      a.classList.add('active');
    };
    li.appendChild(a);
    menuList.appendChild(li);
  });
}

function showSection(id) {
  document.querySelectorAll('main .card').forEach(sec => {
    sec.classList.toggle('hidden', sec.id !== id);
  });
}

function setupSections(api) {
  showSection('dashboard');
  document.querySelectorAll('#menu-list a').forEach(a => {
    a.addEventListener('click', () => {
      if (a.textContent.includes('SMS Historie')) {
        loadSmsHistory(api);
      }
      if (a.textContent.includes('Historie volání')) {
        loadCallLog(api);
      }
    });
  });
}

// --- Historie volání ---
async function loadCallLog(api) {
  try {
    const log = await fetch(api.callLog).then(r => r.json());
    const table = document.getElementById('call-log-table')?.querySelector('tbody');
    if (!table) return;
    table.innerHTML = '';

    if (log.length === 0) {
      table.innerHTML = '<tr><td colspan="2">Žádná data</td></tr>';
      return;
    }

    // Zobrazíme maximálně 100 záznamů (posledních)
    log.slice(-100).reverse().forEach(entry => {
      const row = document.createElement('tr');
      row.innerHTML = `
        <td>${new Date(entry.datetime).toLocaleString('cs-CZ')}</td>
        <td>${entry.number}</td>
      `;
      table.appendChild(row);
    });
  } catch (e) {
    console.warn('Nelze načíst historii volání:', e);
  }
}

// --- Modem status ---
function setupModemStatus(api) {
  const opEl   = document.getElementById('operator-name');
  const barEl  = document.getElementById('signal-bar');
  const txtEl  = document.getElementById('signal-strength');
  const mqttEl = document.getElementById('mqtt-status-indicator');
  const timeEl = document.getElementById('modem-time');

  async function update() {
    try {
      const { signal, operator, mqttConnected } = 
              await fetch(api.modemStatus).then(r => r.json());
      if (opEl)   opEl.textContent   = operator;
      if (barEl)  barEl.value        = signal;
      if (txtEl)  txtEl.textContent  = signal;
      if (mqttEl) mqttEl.className   = mqttConnected
                     ? 'status-dot connected'
                     : 'status-dot disconnected';
    } catch (e) {
      if (opEl)   opEl.textContent  = '—';
      if (barEl)  barEl.value       = 0;
      if (txtEl)  txtEl.textContent = '—';
      if (mqttEl) mqttEl.className  = 'status-dot disconnected';
    }
    if (timeEl) timeEl.textContent = new Date()
                           .toLocaleTimeString('cs-CZ');
  }
  update();
  setInterval(update, 5000);
}

// --- SMS šablony ---
function loadTemplatesData(templates) {
  const c = document.getElementById('template-list');
  if (!c) return;
  c.innerHTML = '';
  templates.forEach(t => {
    const d = document.createElement('div');
    d.className = 'card template-card';
    d.innerHTML = `<h3>${t.name}</h3><p>${t.content}</p>`;
    c.appendChild(d);
  });
}

// --- Uživatelé ---
function loadUsersData(users) {
  const c = document.getElementById('user-list');
  if (!c) return;
  c.innerHTML = '';
  users.forEach(u => {
    const d = document.createElement('div');
    d.className = 'card user-card';
    d.innerHTML = `
      <h3>${u.username} (${u.name})</h3>
      <p>Role: ${u.role}</p>
      <p>Telefon: ${u.phone}</p>
      <p>Email: ${u.email}</p>
      <p>Skupiny: ${u.groups.join(', ')}</p>
    `;
    c.appendChild(d);
  });
}

// --- Kontakty ---
function loadContactsData(contacts) {
  const tbody = document.querySelector('#contact-table tbody');
  if (!tbody) return;
  tbody.innerHTML = '';
  contacts.forEach((c, index) => {
    const row = document.createElement('tr');
    row.innerHTML = `
      <td>${c.name}</td>
      <td>${c.phone}</td>
      <td>${c.email}</td>
      <td>${c.group}</td>
      <td>
        <button class="button small edit-btn" data-index="${index}">✏️</button>
        <button class="button small delete-btn" data-index="${index}">🗑️</button>
      </td>
    `;
    tbody.appendChild(row);
  });
  document.querySelectorAll('.edit-btn').forEach(btn => {
    btn.addEventListener('click', () => editContact(contacts, btn.dataset.index));
  });
  document.querySelectorAll('.delete-btn').forEach(btn => {
    btn.addEventListener('click', () => deleteContact(contacts, btn.dataset.index));
  });
}

function setupAddContact(contacts) {
  const addBtn = document.getElementById('add-contact');
  if (!addBtn) return;
  addBtn.addEventListener('click', async () => {
    const name  = prompt('Jméno:');
    const phone = prompt('Telefon:');
    if (!name || !phone) {
      alert('Jméno i telefon jsou povinné.');
      return;
    }
    const email = prompt('Email:') || '';
    const group = prompt('Skupina:') || '';

    contacts.push({ name, phone, email, group });
    try {
      await saveContacts(contacts);
      loadContactsData(contacts);
      populateRecipientsSelect(contacts);
    } catch {
      alert('Chyba při ukládání nového kontaktu.');
    }
  });
}

async function editContact(contacts, index) {
  const c = contacts[index];
  const name  = prompt('Zadej jméno:', c.name);
  const phone = prompt('Zadej telefon:', c.phone);
  if (!name || !phone) {
    alert('Jméno a telefon jsou povinné.');
    return;
  }
  if (!validatePhone(phone)) {
    alert('Neplatný formát telefonního čísla.');
    return;
  }
  const email = prompt('Zadej email:', c.email) || '';
  const group = prompt('Zadej skupinu:', c.group) || '';

  contacts[index] = { name, phone, email, group };
  try {
    await saveContacts(contacts);
    loadContactsData(contacts);
    populateRecipientsSelect(contacts);
  } catch {
    alert('Chyba při ukládání kontaktu.');
  }
}

function deleteContact(contacts, index) {
  if (confirm('Opravdu smazat tento kontakt?')) {
    contacts.splice(index, 1);
    saveContacts(contacts)
      .then(() => {
        loadContactsData(contacts);
        populateRecipientsSelect(contacts);
      });
  }
}

async function saveContacts(contacts) {
  try {
    const res = await fetch('/api/contacts', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(contacts)
    });
    if (!res.ok) throw new Error('HTTP chyba: ' + res.status);
    console.log('Kontakty uloženy');
  } catch (err) {
    alert('Chyba při ukládání kontaktů: ' + err.message);
    throw err;
  }
}

function validatePhone(phone) {
  // Jednoduchá validace: číslo může začínat +, obsahuje jen číslice a mezery
  const phoneRegex = /^\+?\d[\d\s]*$/;
  return phoneRegex.test(phone.trim());
}
// --- CSV Import ---
function setupCsvImport(contactsData) {
  const fileInput = document.getElementById('import-csv');
  if (!fileInput) return;
  fileInput.addEventListener('change', e => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = evt => {
      const lines = evt.target.result.split('\n').filter(l => l.trim());
      const [, ...rows] = lines; // přeskočit hlavičku
      let newContactsAdded = 0;
      rows.forEach(line => {
        const [name, phone, email, group] = line.split(',').map(s => s?.trim() ?? '');
        if (!name || !phone) return; // validace
        if (!contactsData.some(c => c.phone === phone)) { // kontrola duplicit
          contactsData.push({ name, phone, email, group });
          newContactsAdded++;
        }
      });
      if (newContactsAdded > 0) {
        saveContacts(contactsData).then(() => {
          loadContactsData(contactsData);
          populateRecipientsSelect(contactsData);
        });
      }
    };
    reader.readAsText(file);
  });
}

// --- CSV Export ---
function setupCsvExport(contactsData) {
  const exportBtn = document.getElementById('export-csv');
  if (!exportBtn) return;
  exportBtn.addEventListener('click', () => {
    let csv = 'name,phone,email,group\n';
    contactsData.forEach(cn => {
      csv += `${cn.name},${cn.phone},${cn.email},${cn.group}\n`;
    });
    const blob = new Blob([csv], { type: 'text/csv' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = 'contacts.csv';
    a.click();
    URL.revokeObjectURL(url);
  });
}

// --- LDAP Sync ---
function setupLdapSync(api, contactsData) {
  const syncBtn = document.getElementById('sync-ldap');
  if (!syncBtn) return;
  syncBtn.addEventListener('click', async () => {
    try {
      const newContacts = await fetch(api.ldapSync).then(r => r.json());
      contactsData.splice(0, contactsData.length, ...newContacts);
      saveContacts(contactsData);
      loadContactsData(contactsData);
    } catch {
      alert('Chyba při synchronizaci LDAP');
    }
  });
}

// --- SMS Form ---
function populateRecipientsSelect(contacts) {
  const select = document.getElementById('recipients');
  if (!select) return;

  select.innerHTML = ''; // Vyčistit předchozí
  contacts.forEach(c => {
    const option = document.createElement('option');
    option.value = c.phone;
    option.textContent = `${c.name} (${c.phone})`;
    select.appendChild(option);
  });
}

function setupCharCount() {
  const textarea = document.getElementById('message-text');
  const counter  = document.getElementById('char-count');
  if (!textarea || !counter) return;
  textarea.addEventListener('input', () => {
    counter.textContent = `${textarea.value.length}/160`;
  });
}

async function loadSmsStatus(api) {
  try {
    const res  = await fetch(api.smsStatus);
    const { queue } = await res.json();
    const tbody = document.querySelector('#sms-status-table tbody');
    tbody.innerHTML = '';

    queue.forEach(item => {
      // badge + spinner pokud sending
      let badge;
      if (item.state === 'sending') {
        badge = `<span class="badge sending">
                   <span class="spinner"></span>Odesílá se
                 </span>`;
      } else {
        const cls = `badge ${item.state}`;
        const txt = item.state === 'idle' ? 'Čeká' :
                    item.state === 'sent' ? 'Odesláno' :
                    'Chyba';
        badge = `<span class="${cls}">${txt}</span>`;
      }

      const row = `
        <tr>
          <td>${item.id}</td>
          <td>${item.recipients}</td>
          <td>${item.message}</td>
          <td>${badge}</td>
        </tr>`;
      tbody.insertAdjacentHTML('beforeend', row);
    });
  } catch(e) {
    console.error('Chyba při načítání stavů SMS:', e);
  }
}

function attachFormHandlers(api, contactsData) {
  const form = document.getElementById('sms-form');
  if (!form) return;

  populateRecipientsSelect(contactsData);
  setupCharCount();

  form.addEventListener('submit', async (e) => {
    e.preventDefault();

    const recipientsSelect = document.getElementById('recipients');
    const messageInput = document.getElementById('message-text');
    const alertBox = document.getElementById('sms-alert');

    if (!recipientsSelect || !messageInput) return;

    const recipients = Array.from(recipientsSelect.selectedOptions).map(opt => opt.value);
    const message = messageInput.value.trim();

    if (recipients.length === 0) {
      alert('Vyberte alespoň jednoho příjemce');
      return;
    }
    if (message.length === 0) {
      alert('Zadejte text zprávy');
      return;
    }

    form.querySelector('button[type="submit"]').disabled = true;

    try {
      const res = await fetch(api.smsSend, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ numbers: recipients, message, sendTime })
      });

      if (!res.ok) throw new Error('Chyba serveru při odeslání SMS');

      const result = await res.json();
      const datetime = new Date().toLocaleString('cs-CZ');

      const table = document.getElementById('sent-sms-table');
      const tableBody = table ? table.querySelector('tbody') : null;

      recipients.forEach(phone => {
        if (tableBody) {
          const row = document.createElement('tr');
          row.innerHTML = `<td>${datetime}</td><td>${phone}</td><td>${message}</td>`;
          tableBody.prepend(row);
        }
      });

      // ✅ Zobrazit úspěšné odeslání
      if (alertBox) {
        alertBox.className = 'alert alert-success';
        alertBox.textContent = `SMS zpráva úspěšně odeslána ${recipients.length} příjemcům.`;
        alertBox.style.display = 'block';
      }

      messageInput.value = '';
      recipientsSelect.selectedIndex = -1;
      const charCount = document.getElementById('char-count');
      if (charCount) charCount.textContent = '0/160';

    } catch (err) {
      if (alertBox) {
        alertBox.className = 'alert alert-danger';
        alertBox.textContent = 'Nepodařilo se odeslat SMS: ' + err.message;
        alertBox.style.display = 'block';
      } else {
        alert('Nepodařilo se odeslat SMS: ' + err.message);
      }
    } finally {
      form.querySelector('button[type="submit"]').disabled = false;
    }
  });
}

function setupSmsForm(api) {
  const form = document.getElementById('sms-form');
  const alertBox = document.getElementById('sms-alert');

  form.addEventListener('submit', async (e) => {
    e.preventDefault();

    const selectedOptions = [...document.getElementById('recipients').selectedOptions];
    const selectedNumbers = selectedOptions.map(o => o.value);
    const manualInput = document.getElementById('manual-numbers').value;
    const message = document.getElementById('message-text').value;
    const sendTime = document.getElementById('send-time').value;

    // Zpracuj ručně zadaná čísla (oddělena čárkami)
    const manualNumbers = manualInput
      .split(',')
      .map(n => n.trim())
      .filter(n => n.length > 0);

    const recipients = [...selectedNumbers, ...manualNumbers];

    if (recipients.length === 0) {
      showAlert('Musíte zadat alespoň jedno telefonní číslo.', 'error');
      return;
    }

    if (message.trim().length === 0) {
      showAlert('Zpráva nemůže být prázdná.', 'error');
      return;
    }

    // Sestavení JSON požadavku
    const payload = {
      recipients,
      message,
    };

    if (sendTime) {
      payload.sendTime = sendTime; // volitelně čas
    }

    try {
      const res = await fetch(api.sendSms, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify(payload)
      });

      if (!res.ok) throw new Error(await res.text());

      showAlert('Zpráva byla odeslána nebo naplánována.', 'success');
      form.reset();
    } catch (err) {
      showAlert(`Chyba při odesílání zprávy: ${err}`, 'error');
    }
  });

  function showAlert(text, type = 'info') {
    if (!alertBox) return;
    alertBox.textContent = text;
    alertBox.style.display = 'block';
    alertBox.className = `alert ${type}`;
    setTimeout(() => alertBox.style.display = 'none', 5000);
  }
}

async function loadSmsConfig(api) {
  const res = await fetch(api.config);
  const cfg = await res.json();
  document.getElementById('sms-history-limit').value = cfg.smsHistoryMaxCount;
}
async function saveSmsConfig(api) {
  const v = parseInt(document.getElementById('sms-history-limit').value,10);
  await fetch(api.config, {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ smsHistoryMaxCount: v })
  });
  showAlert('Konfigurace uložena', 'success');
}

async function loadSmsHistory(api) {
  try {
    const res = await fetch(api.smsHistory);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    // podpora obou variant: { history: […] } i čistého pole […]
    const history = Array.isArray(data) ? data : data.history || [];
    const tbody = document.querySelector('#sms-history-table tbody');
    tbody.innerHTML = '';
    history.forEach(e => {
      const dt  = new Date(e.timestamp * 1000).toLocaleString('cs-CZ');
      const row = `<tr>
                     <td>${dt}</td>
                     <td>${e.recipient}</td>
                     <td>${e.message}</td>
                   </tr>`;
      tbody.insertAdjacentHTML('beforeend', row);
    });
  } catch (err) {
    console.error('Chyba při načítání SMS historie:', err);
  }
}

function setupSmsHistory(api) {
  loadSmsConfig(api);
  loadSmsHistory(api);
  document.getElementById('sms-history-limit-save')
    .addEventListener('click', () => saveSmsConfig(api));
}

// Jednoduchá validace telefonu (čísla s mezinárodní předvolbou +...)
function validatePhone(phone) {
  return /^\+?\d{6,15}$/.test(phone.replace(/\s+/g, ''));
}

// --- MQTT ---
function setupMqttForm(api) {
  loadMqttConfig(api);
  const form = document.getElementById('mqtt-form');
  const testBtn = document.getElementById('mqtt-test-btn');
  const status = document.getElementById('mqtt-status');
  const alertBox = document.getElementById('mqtt-alert');

  if (!form || !testBtn || !status) return;

  form.onsubmit = async e => {
    e.preventDefault();

    const cfg = {
      clientId:     document.getElementById('mqtt-clientId').value,
      username:     document.getElementById('mqtt-username').value,
      password:     document.getElementById('mqtt-password').value,
      broker:       document.getElementById('mqtt-broker').value,
      port:         parseInt(document.getElementById('mqtt-port').value, 10),
      keepalive:    parseInt(document.getElementById('mqtt-keepalive').value, 10),
      cleanSession: document.getElementById('mqtt-clean-session').checked,
      statusTopic:  document.getElementById('mqtt-status-topic').value,
      smsTopic:     document.getElementById('mqtt-sms-topic').value,
      callerTopic:  document.getElementById('mqtt-caller-topic').value,
      pubTopic:     document.getElementById('mqtt-pubTopic').value
    };

    try {
      const res = await fetch(api.mqttConfig, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(cfg)
      });

      if (!res.ok) throw new Error('Chyba při ukládání konfigurace');

      if (alertBox) {
        alertBox.className = 'alert alert-success';
        alertBox.textContent = 'MQTT nastavení bylo úspěšně uloženo.';
        alertBox.style.display = 'block';
      } else {
        alert('MQTT nastavení uloženo');
      }

    } catch (err) {
      if (alertBox) {
        alertBox.className = 'alert alert-danger';
        alertBox.textContent = 'Chyba při ukládání MQTT konfigurace: ' + err.message;
        alertBox.style.display = 'block';
      } else {
        alert('Chyba při ukládání MQTT konfigurace: ' + err.message);
      }
    }
  };

  testBtn.onclick = async () => {
    status.textContent = 'Testuji…';

    try {
      const res = await fetch(api.mqttTest, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          clientId:  document.getElementById('mqtt-clientId').value,
          username:  document.getElementById('mqtt-username').value,
          password:  document.getElementById('mqtt-password').value,
          broker:    document.getElementById('mqtt-broker').value,
          port:      parseInt(document.getElementById('mqtt-port').value, 10),
          keepalive: parseInt(document.getElementById('mqtt-keepalive').value, 10)
        })
      });

      const json = await res.json();
      status.textContent = json.success ? '✅ OK – připojeno' : '❌ Chyba: ' + (json.error || 'neznámá');

    } catch {
      status.textContent = '❌ Chyba spojení';
    }
  };
}

async function loadMqttConfig(api) {
  try {
    const cfg = await fetch(api.mqttConfig).then(r => r.json());
    ['clientId','username','password','broker','port','keepalive','pubTopic']
      .forEach(k => {
        const el = document.getElementById(`mqtt-${k}`);
        if (el) el.value = cfg[k] || '';
      });
    document.getElementById('mqtt-clean-session').checked = !!cfg.cleanSession;
    document.getElementById('mqtt-status-topic').value = cfg.statusTopic || '';
    document.getElementById('mqtt-sms-topic').value    = cfg.smsTopic || '';
    document.getElementById('mqtt-caller-topic').value = cfg.callerTopic || '';
  } catch (e) {
    console.warn('Nelze načíst MQTT konfiguraci', e);
  }
}

async function loadSettings(api) {
  try {
    const cfg = await fetch(api.settings).then(r => r.json());
    document.getElementById('ntp-server').value      = cfg.ntpServer || '';
    document.getElementById('ntp-server-port').value = cfg.ntpPort || 123;
    document.getElementById('ntp-local-port').value  = cfg.localPort || 2390;
    document.getElementById('ntp-retry').value       = cfg.retryInterval || 10000;
    document.getElementById('tz-string').value       = cfg.tzString || '';
    document.getElementById('baud-rate').value       = cfg.baudRate || 115200;
    document.getElementById('modem-auto-time').checked   = !!cfg.atctzu;
    document.getElementById('modem-manual-time').checked = !!cfg.atctr;
    document.getElementById('modem-callerid').checked    = !!cfg.atclip;
    document.getElementById('timeout-prompt').value   = cfg.smsPromptTimeout || 10000;
    document.getElementById('timeout-sms').value      = cfg.smsTimeout || 15000;
    document.getElementById('cmd-interval').value     = cfg.cmdInterval || 200;
    document.getElementById('ring-count').value       = cfg.maxRingCount || 1;
  } catch (e) {
    console.warn('Nelze načíst nastavení', e);
  }
}

function setupSettingsForm(api) {
  loadSettings(api);
  document.getElementById('settings-form').onsubmit = async e => {
    e.preventDefault();
    const cfg = {
      ntpServer:      document.getElementById('ntp-server').value,
      ntpPort:        parseInt(document.getElementById('ntp-server-port').value, 10),
      localPort:      parseInt(document.getElementById('ntp-local-port').value, 10),
      retryInterval:  parseInt(document.getElementById('ntp-retry').value, 10),
      tzString:       document.getElementById('tz-string').value,
      baudRate:       parseInt(document.getElementById('baud-rate').value, 10),
      atctzu:         document.getElementById('modem-auto-time').checked,
      atctr:          document.getElementById('modem-manual-time').checked,
      atclip:         document.getElementById('modem-callerid').checked,
      smsPromptTimeout: parseInt(document.getElementById('timeout-prompt').value, 10),
      smsTimeout:     parseInt(document.getElementById('timeout-sms').value, 10),
      cmdInterval:    parseInt(document.getElementById('cmd-interval').value, 10),
      maxRingCount:   parseInt(document.getElementById('ring-count').value, 10)
    };
    const res = await fetch(api.settings, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cfg)
    });
    if (res.ok) {
      alert('Nastavení uloženo');
    } else {
      alert('Chyba při ukládání nastavení');
    }
  };
}

function setupAtConsole(api) {
  const sendBtn = document.getElementById('send-at-btn');
  const inputEl = document.getElementById('at-command');
  const outputEl = document.getElementById('at-response');
  const refreshBtn = document.getElementById('refresh-response-btn');
  const autoRefreshEl = document.getElementById('auto-refresh');

  let refreshInterval = null;

  if (!sendBtn || !inputEl || !outputEl) return;

  sendBtn.onclick = async () => {
    const command = inputEl.value.trim();
    if (!command) return;
    try {
      const res = await fetch(api.sendAtCommand, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command })
      });
      if (!res.ok) throw new Error('Chyba při odeslání AT příkazu');
      const data = await res.text();
      outputEl.value = data;
    } catch (err) {
      outputEl.value = '❌ Chyba: ' + err.message;
    }
  };

  refreshBtn.onclick = fetchLatestAtResponse;

  autoRefreshEl.addEventListener('change', () => {
    if (autoRefreshEl.checked) {
      refreshInterval = setInterval(fetchLatestAtResponse, 3000);
    } else {
      clearInterval(refreshInterval);
    }
  });

  async function fetchLatestAtResponse() {
    try {
      const res = await fetch(api.atResponseLog);
      const text = await res.text();
      outputEl.value = text;
    } catch (err) {
      outputEl.value = '❌ Chyba při čtení odpovědi: ' + err.message;
    }
  }
}

