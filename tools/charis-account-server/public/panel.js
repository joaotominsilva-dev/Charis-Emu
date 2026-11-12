// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Charis Conta web panel. The password never leaves the browser: it is turned into the same
// "auth" code the apps send (Argon2id, then keyed BLAKE2b), so the server checks the login
// without seeing the password or the stream key.

(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const store = {
    get(key) { try { return localStorage.getItem(key); } catch { return null; } },
    set(key, value) { try { localStorage.setItem(key, value); } catch { /* private mode */ } },
    remove(key) { try { localStorage.removeItem(key); } catch { /* private mode */ } },
  };

  // ---------- Talking to the account server ----------

  function parseFields(text) {
    const fields = {};
    for (const line of text.split('\n')) {
      const at = line.indexOf('=');
      if (at > 0) fields[line.slice(0, at)] = decodeURIComponent(line.slice(at + 1));
    }
    return fields;
  }

  async function call(route, params) {
    let response;
    try {
      response = await fetch(`/api/${route}`, { method: 'POST', body: new URLSearchParams(params) });
    } catch {
      throw new Error('Sem conexão com o servidor da conta. Confira a internet.');
    }
    if ((response.headers.get('content-type') || '').includes('application/json')) {
      return response.json();
    }
    const fields = parseFields(await response.text());
    if (fields.ok) return fields;
    const messages = {
      wrong: 'Usuário ou senha incorretos.',
      invalid: 'Esse usuário não é válido.',
      auth: 'O login expirou. Entre de novo.',
    };
    const error = new Error(messages[fields.error] || 'O servidor da conta respondeu com um erro. Tente de novo em instantes.');
    error.code = fields.error;
    throw error;
  }

  async function deriveAuth(user, password) {
    const saltHex = await hashwasm.blake2b(`charis-stream-account-v1:${user}`, 128);
    const salt = new Uint8Array(saltHex.match(/../g).map((h) => parseInt(h, 16)));
    const master = await hashwasm.argon2id({
      password, salt, parallelism: 1, iterations: 3, memorySize: 8192, hashLength: 32,
      outputType: 'binary',
    });
    return hashwasm.blake2b('auth', 256, master);
  }

  // ---------- Formatting ----------

  const locale = 'pt-BR';
  function duration(seconds) {
    const minutes = Math.round(seconds / 60);
    if (minutes < 1) return 'menos de 1 min';
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60);
    const rest = minutes % 60;
    return rest ? `${hours} h ${rest} min` : `${hours} h`;
  }
  function ago(at, now) {
    const seconds = Math.max(0, (now - at) / 1000);
    if (seconds < 60) return 'agora mesmo';
    if (seconds < 3600) return `há ${Math.round(seconds / 60)} min`;
    if (seconds < 86400) return `há ${Math.round(seconds / 3600)} h`;
    const days = Math.round(seconds / 86400);
    return days === 1 ? 'ontem' : `há ${days} dias`;
  }
  function dayLabel(at, now) {
    const day = new Date(at).toDateString();
    if (day === new Date(now).toDateString()) return 'Hoje';
    if (day === new Date(now - 86400000).toDateString()) return 'Ontem';
    const text = new Date(at).toLocaleDateString(locale, { weekday: 'long', day: 'numeric', month: 'long' });
    return text.charAt(0).toUpperCase() + text.slice(1);
  }
  function hour(at) {
    return new Date(at).toLocaleTimeString(locale, { hour: '2-digit', minute: '2-digit' });
  }
  function el(tag, props = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props)) {
      if (key === 'class') node.className = value;
      else if (key === 'html') node.innerHTML = value; // only for our own fixed icons
      else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
      else node.setAttribute(key, value);
    }
    for (const child of children.flat()) {
      if (child != null) node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
  }

  const icons = {
    session: '<svg viewBox="0 0 24 24" fill="none" stroke="#1D1E19" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="6" width="18" height="12" rx="4"/><path d="M8 10v4M6 12h4"/><circle cx="16" cy="11" r="1"/><circle cx="17.5" cy="14" r="1"/></svg>',
    game: '<svg viewBox="0 0 24 24" fill="none" stroke="#1D1E19" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polygon points="8 5 19 12 8 19 8 5"/></svg>',
    login: '<svg viewBox="0 0 24 24" fill="none" stroke="#1D1E19" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c1.5-4 4.5-6 8-6s6.5 2 8 6"/></svg>',
    created: '<svg viewBox="0 0 24 24" fill="none" stroke="#1D1E19" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 5v14M5 12h14"/></svg>',
  };
  const deviceName = { pc: 'PC', switch: 'Switch', web: 'painel', app: 'app' };

  function describe(entry) {
    switch (entry.kind) {
      case 'session':
        return {
          what: `${entry.device} jogou no ${entry.pc}`,
          detail: [duration(entry.seconds), entry.via === 'casa' ? 'em casa' : 'pela internet', entry.game || null]
            .filter(Boolean).join(' · '),
        };
      case 'game':
        return {
          what: entry.title,
          detail: [`${duration(entry.seconds)} no ${entry.pc}`, entry.streamed ? 'jogado pelo Switch' : null]
            .filter(Boolean).join(' · '),
        };
      case 'created':
        return { what: 'Conta criada', detail: entry.name ? `no ${entry.name}` : `pelo ${deviceName[entry.device] || 'app'}` };
      default:
        return { what: `Entrou pelo ${deviceName[entry.device] || 'app'}`, detail: entry.name || '' };
    }
  }

  // ---------- Screens ----------

  let token = store.get('charis-token');
  let refreshTimer = null;

  function showLogin(message) {
    clearInterval(refreshTimer);
    $('panel').hidden = true;
    $('logout').hidden = true;
    $('login-form').hidden = false;
    $('eyebrow').textContent = 'Charis Switch e Charis Stream';
    $('heading').textContent = 'Entre na sua conta';
    $('login-error').hidden = !message;
    $('login-error').textContent = message || '';
    const last = store.get('charis-user');
    if (last && !$('user').value) $('user').value = last;
    ($('user').value ? $('password') : $('user')).focus();
  }

  async function load() {
    let data;
    try {
      data = await call('panel', { token });
    } catch (error) {
      if (error.code === 'auth') {
        token = null;
        store.remove('charis-token');
        showLogin(error.message);
      } else {
        $('foot').textContent = error.message;
      }
      return;
    }
    render(data);
  }

  function render(data) {
    const { now } = data;
    $('login-form').hidden = true;
    $('panel').hidden = false;
    $('logout').hidden = false;
    $('eyebrow').textContent = data.created
      ? `Conta criada em ${new Date(data.created).toLocaleDateString(locale, { day: 'numeric', month: 'long', year: 'numeric' })}`
      : 'Charis Conta';
    $('heading').textContent = `Olá, ${data.user}`;

    // Right now
    const playing = data.hosts.find((h) => h.online && h.game);
    const online = data.hosts.filter((h) => h.online);
    const nowBox = $('now');
    nowBox.replaceChildren();
    if (playing) {
      nowBox.append(el('span', { class: 'dot on' }), el('div', {},
        el('div', { class: 'title' }, `Jogando ${playing.game}`),
        el('div', { class: 'sub' }, `no ${playing.name}${playing.game_since ? `, ${ago(playing.game_since, now)}` : ''}`)));
    } else if (online.length) {
      nowBox.append(el('span', { class: 'dot on' }), el('div', {},
        el('div', { class: 'title' }, `${online.map((h) => h.name).join(', ')} ${online.length > 1 ? 'estão ligados' : 'está ligado'}`),
        el('div', { class: 'sub' }, 'Pronto para jogar no Switch: abra o Charis Stream.')));
    } else {
      nowBox.append(el('span', { class: 'dot' }), el('div', {},
        el('div', { class: 'title' }, 'Nenhum PC ligado agora'),
        el('div', { class: 'sub' }, 'Abra o Charis Switch no PC para ele aparecer aqui e no Switch.')));
    }

    // PCs
    const pcs = $('pcs');
    pcs.replaceChildren();
    if (!data.hosts.length) {
      pcs.append(el('div', { class: 'card empty' }, 'Nenhum PC ainda. Entre na conta pelo Charis Switch no PC.'));
    }
    for (const host of data.hosts.sort((a, b) => b.seen - a.seen)) {
      const chip = (label, yes) => el('span', { class: `chip${yes ? ' yes' : ''}` }, `${yes ? '✓' : '·'} ${label}`);
      pcs.append(el('div', { class: 'card pc' },
        el('div', { class: 'row' },
          el('div', { class: 'grow' }, el('h3', {}, host.name)),
          el('span', { class: `status${host.online ? ' on' : ''}` }, host.online ? 'Ligado' : `Visto ${ago(host.seen, now)}`)),
        host.game ? el('div', { class: 'small' }, `Jogando agora: ${host.game}`) : null,
        el('div', { class: 'chips' },
          chip('Rede de casa', host.home),
          chip('Porta do roteador aberta', host.router),
          chip('Endereço na internet', host.internet)),
        el('div', {}, el('button', {
          class: 'link',
          onclick: async () => {
            if (!confirm(`Tirar ${host.name} da conta? Ele volta sozinho quando o Charis Switch for aberto de novo com a conta.`)) return;
            try { await call('host/remove', { token, host_id: host.id }); } catch (e) { alert(e.message); }
            load();
          },
        }, 'Tirar da conta'))));
    }

    // Games
    const games = $('games');
    games.replaceChildren();
    if (!data.games.length) {
      games.append(el('div', { class: 'empty' }, 'Os jogos aparecem aqui depois que você jogar no Charis Switch.'));
    }
    const most = Math.max(1, ...data.games.map((g) => g.seconds));
    for (const game of data.games.slice(0, 12)) {
      games.append(el('div', { class: 'game' },
        el('div', {}, el('div', { style: 'font-weight:700' }, game.title),
          el('div', { class: 'small muted' }, `${game.sessions} ${game.sessions === 1 ? 'vez' : 'vezes'} · última ${ago(game.last, now)}`)),
        el('div', { class: 'time' }, duration(game.seconds)),
        el('div', { class: 'bar' }, el('span', { style: `width:${Math.max(3, Math.round(game.seconds / most * 100))}%` }))));
    }

    // Switches
    const devices = $('devices');
    devices.replaceChildren();
    if (!data.devices.length) {
      devices.append(el('div', { class: 'empty' }, 'Os Switches aparecem aqui quando entram na conta pelo Charis Stream.'));
    }
    for (const device of data.devices) {
      devices.append(el('div', { class: 'row', style: 'padding:6px 0' },
        el('div', { class: 'grow' }, el('div', { style: 'font-weight:700' }, device.name),
          el('div', { class: 'small muted' }, `Usado ${ago(device.last, now)}`)),
        el('button', {
          class: 'link',
          onclick: async () => {
            try { await call('device/remove', { token, client_id: device.id }); } catch (e) { alert(e.message); }
            load();
          },
        }, 'Esquecer')));
    }

    // History, grouped by day
    const history = $('history');
    history.replaceChildren();
    if (!data.log.length) {
      history.append(el('div', { class: 'empty' }, 'Nada ainda. O que você jogar aparece aqui.'));
    }
    let currentDay = '';
    for (const entry of data.log) {
      const day = dayLabel(entry.at, now);
      if (day !== currentDay) {
        currentDay = day;
        history.append(el('div', { class: 'day' }, day));
      }
      const { what, detail } = describe(entry);
      history.append(el('div', { class: 'event' },
        el('div', { class: 'hour' }, hour(entry.at)),
        el('div', { class: 'icon', html: icons[entry.kind] || icons.login }),
        el('div', {}, el('div', { class: 'what' }, what), detail ? el('div', { class: 'detail' }, detail) : null)));
    }
    $('foot').textContent = `Atualizado às ${hour(now)}. A página se atualiza sozinha.`;
  }

  $('login-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const user = $('user').value.trim().toLowerCase().replace(/ /g, '');
    const password = $('password').value;
    $('submit').disabled = true;
    $('submit').textContent = 'Entrando...';
    $('login-error').hidden = true;
    try {
      const auth = await deriveAuth(user, password);
      const fields = await call('login', { user, auth, device: 'web', name: 'Navegador' });
      token = fields.token;
      store.set('charis-token', token);
      store.set('charis-user', user);
      $('password').value = '';
      await load();
      clearInterval(refreshTimer);
      refreshTimer = setInterval(load, 20000);
    } catch (error) {
      showLogin(error.message);
    } finally {
      $('submit').disabled = false;
      $('submit').textContent = 'Entrar';
    }
  });

  $('logout').addEventListener('click', () => {
    token = null;
    store.remove('charis-token');
    showLogin('');
  });

  if (token) {
    load();
    refreshTimer = setInterval(load, 20000);
  } else {
    showLogin('');
  }
})();
