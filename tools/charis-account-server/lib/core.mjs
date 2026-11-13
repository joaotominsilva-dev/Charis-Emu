// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Charis account server: introduces the Charis Switch on a PC to the Charis Stream app on a
// Switch that is logged in to the same account. It never sees the video or the stream key: the
// "auth" value it receives is derived from the password on the device and only proves the login.
//
// Requests are POST with a form-encoded body; answers are text lines "key=value" with
// URL-encoded values, so the apps need no JSON parser.

import crypto from 'node:crypto';

const USER_RE = /^[a-z0-9][a-z0-9_.-]{2,23}$/;
const HEX64_RE = /^[0-9a-f]{64}$/;
const HEX16_RE = /^[0-9a-f]{16}$/;
const ADDR_RE = /^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$/;
const TOKEN_DAYS = 180;
const HOST_ONLINE_MS = 75_000; // the helper that runs all day calls every 30 s
const REQUEST_TTL_MS = 30_000;
const MAX_HOSTS = 8;
const MAX_REQUESTS = 8;
// The PC asks again after POLL_IDLE_MS, or POLL_FAST_MS while a Switch of the account was
// looking at its PC list in the last LOOKING_MS: connecting feels instant without spending
// the free function quota on a PC that nobody is looking for.
const POLL_IDLE_MS = 10_000;
const POLL_FAST_MS = 1_500;
const LOOKING_MS = 90_000;

const MAX_LOG = 300;      // history entries kept per account
const MAX_DEVICES = 16;   // Switches remembered per account

const ok = (fields = {}) => ({ status: 200, fields: { ok: '1', ...fields } });
const okJson = (json) => ({ status: 200, json });
const fail = (status, error) => ({ status, fields: { error } });

function addressList(text, max = 4) {
  const list = String(text || '').split(',').map((s) => s.trim()).filter(Boolean);
  return list.length <= max && list.every((a) => ADDR_RE.test(a)) ? list : null;
}

function cleanName(text, max = 48) {
  return String(text || '').replace(/[\u0000-\u001f]/g, ' ').trim().slice(0, max);
}

function cleanDevice(text) {
  return ['pc', 'switch', 'web'].includes(text) ? text : 'app';
}

function hashAuth(auth, salt) {
  return crypto.scryptSync(Buffer.from(auth, 'hex'), Buffer.from(salt, 'hex'), 32).toString('hex');
}

// store: get(key) -> object | null, set(key, value, { onlyIfNew }) -> boolean (written),
// delete(key), list(prefix) -> string[] of keys
export function createApi(store) {
  let secretPromise = null;
  async function secret() {
    secretPromise ??= (async () => {
      await store.set('config/secret', { value: crypto.randomBytes(32).toString('hex') },
                      { onlyIfNew: true });
      return (await store.get('config/secret')).value;
    })();
    return secretPromise;
  }

  async function makeToken(user) {
    const body = Buffer.from(JSON.stringify({ u: user, t: Date.now() })).toString('base64url');
    const mac = crypto.createHmac('sha256', await secret()).update(body).digest('base64url');
    return `${body}.${mac}`;
  }

  async function userFromToken(token) {
    const [body, mac] = String(token || '').split('.');
    if (!body || !mac) return null;
    const expected = crypto.createHmac('sha256', await secret()).update(body).digest('base64url');
    const a = Buffer.from(mac);
    const b = Buffer.from(expected);
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
    try {
      const { u, t } = JSON.parse(Buffer.from(body, 'base64url').toString());
      if (!USER_RE.test(u) || Date.now() - t > TOKEN_DAYS * 86_400_000) return null;
      return u;
    } catch {
      return null;
    }
  }

  // History of the account: logins, play sessions on the Switch, games played on the PC
  async function addLog(user, entry) {
    const now = Date.now();
    const id = `${String(now).padStart(14, '0')}-${crypto.randomBytes(3).toString('hex')}`;
    await store.set(`log/${user}/${id}`, { at: now, ...entry });
    if (Math.random() < 0.125) {
      const keys = (await store.list(`log/${user}/`)).sort();
      for (const old of keys.slice(0, Math.max(0, keys.length - MAX_LOG))) {
        await store.delete(old);
      }
    }
  }

  async function touchDevice(user, clientId, name) {
    if (!HEX16_RE.test(clientId || '')) return;
    const key = `device/${user}/${clientId}`;
    const now = Date.now();
    const current = await store.get(key);
    if (!current) {
      if ((await store.list(`device/${user}/`)).length >= MAX_DEVICES) return;
    } else if (now - current.last < 60_000 && current.name === (cleanName(name, 32) || current.name)) {
      return;
    }
    await store.set(key, {
      name: cleanName(name, 32) || current?.name || 'Switch',
      first: current?.first ?? now,
      last: now,
    });
  }

  const routes = {
    async register(p) {
      const user = String(p.user || '').toLowerCase();
      if (!USER_RE.test(user) || !HEX64_RE.test(p.auth || '')) return fail(400, 'invalid');
      const salt = crypto.randomBytes(16).toString('hex');
      const written = await store.set(`user/${user}`,
        { salt, hash: hashAuth(p.auth, salt), created: Date.now() }, { onlyIfNew: true });
      if (!written) return fail(409, 'exists');
      await addLog(user, { kind: 'created', device: cleanDevice(p.device), name: cleanName(p.name, 32) });
      return ok({ token: await makeToken(user), user });
    },

    async login(p) {
      const user = String(p.user || '').toLowerCase();
      if (!USER_RE.test(user) || !HEX64_RE.test(p.auth || '')) return fail(400, 'invalid');
      const record = await store.get(`user/${user}`);
      // Same work whether the user exists or not
      const hash = hashAuth(p.auth, record ? record.salt : '00'.repeat(16));
      const good = record && crypto.timingSafeEqual(Buffer.from(hash), Buffer.from(record.hash));
      if (!good) return fail(403, 'wrong');
      await addLog(user, { kind: 'login', device: cleanDevice(p.device), name: cleanName(p.name, 32) });
      return ok({ token: await makeToken(user), user });
    },

    // The PC calls this every few seconds while it waits for the Switch: it keeps the PC's
    // addresses fresh and hands over the connection requests waiting for it.
    async 'host/poll'(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      const local = addressList(p.local);
      const pub = p.public ? (ADDR_RE.test(p.public) ? p.public : null) : '';
      if (!HEX16_RE.test(p.host_id || '') || !local || pub === null) return fail(400, 'invalid');

      const key = `host/${user}/${p.host_id}`;
      const name = cleanName(p.name) || 'PC';
      const now = Date.now();
      const current = await store.get(key);
      if (!current) {
        const count = (await store.list(`host/${user}/`)).length;
        if (count >= MAX_HOSTS) return fail(409, 'full');
      }
      const game = cleanName(p.game, 80);
      const changed = !current || current.name !== name || current.public !== pub ||
        current.local.join() !== local.join() || (current.game || '') !== game;
      if (changed || now - current.seen > 15_000) {
        const since = current && (current.game || '') === game ? current.game_since : now;
        await store.set(key, { name, local, public: pub, seen: now, game, game_since: since });
      }

      const fields = {};
      const looking = await store.get(`looking/${user}`);
      fields.next = String(looking && now - looking.at < LOOKING_MS ? POLL_FAST_MS : POLL_IDLE_MS);
      let n = 0;
      for (const requestKey of await store.list(`req/${user}/${p.host_id}/`)) {
        const request = await store.get(requestKey);
        await store.delete(requestKey);
        if (!request || now - request.at > REQUEST_TTL_MS || n >= MAX_REQUESTS) continue;
        fields[`req.${n}.client`] = requestKey.split('/').pop();
        fields[`req.${n}.name`] = request.name;
        fields[`req.${n}.public`] = request.public;
        fields[`req.${n}.local`] = request.local.join(',');
        n += 1;
      }
      fields.count = String(n);
      return ok(fields);
    },

    async 'host/remove'(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      if (!HEX16_RE.test(p.host_id || '')) return fail(400, 'invalid');
      await store.delete(`host/${user}/${p.host_id}`);
      return ok();
    },

    async hosts(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      const now = Date.now();
      const looking = await store.get(`looking/${user}`);
      if (!looking || now - looking.at > 20_000) {
        await store.set(`looking/${user}`, { at: now });
      }
      await touchDevice(user, p.client_id, p.name);
      const fields = {};
      let n = 0;
      for (const key of await store.list(`host/${user}/`)) {
        const host = await store.get(key);
        if (!host) continue;
        fields[`host.${n}.id`] = key.split('/').pop();
        fields[`host.${n}.name`] = host.name;
        fields[`host.${n}.local`] = host.local.join(',');
        fields[`host.${n}.public`] = host.public;
        fields[`host.${n}.online`] = now - host.seen < HOST_ONLINE_MS ? '1' : '0';
        n += 1;
      }
      fields.count = String(n);
      return ok(fields);
    },

    // The Switch asks a PC to open the way for it (both sides then send to each other).
    async connect(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      const local = addressList(p.local);
      const pub = p.public ? (ADDR_RE.test(p.public) ? p.public : null) : '';
      if (!HEX16_RE.test(p.host_id || '') || !HEX16_RE.test(p.client_id || '') || !local ||
          pub === null) {
        return fail(400, 'invalid');
      }
      if (!(await store.get(`host/${user}/${p.host_id}`))) return fail(404, 'nohost');
      const now = Date.now();
      await store.set(`req/${user}/${p.host_id}/${p.client_id}`,
        { name: cleanName(p.name, 32) || 'Switch', public: pub, local, at: now });
      await store.set(`looking/${user}`, { at: now });
      await touchDevice(user, p.client_id, p.name);
      return ok();
    },

    // The PC tells what happened: a play session on the Switch ended, or a game was closed.
    async 'host/report'(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      if (!HEX16_RE.test(p.host_id || '')) return fail(400, 'invalid');
      const host = await store.get(`host/${user}/${p.host_id}`);
      const pc = host?.name || 'PC';
      const seconds = Math.max(0, Math.min(86_400 * 2, Number.parseInt(p.seconds, 10) || 0));
      if (p.kind === 'session') {
        await addLog(user, {
          kind: 'session', pc, device: cleanName(p.client, 32) || 'Switch',
          via: p.via === 'casa' ? 'casa' : 'internet', seconds, game: cleanName(p.game, 80),
        });
        return ok();
      }
      if (p.kind === 'game') {
        const title = cleanName(p.title, 80);
        if (!title) return fail(400, 'invalid');
        const program = /^[0-9a-f]{16}$/.test(p.program || '') ? p.program : null;
        const gameKey = `game/${user}/${program || crypto.createHash('sha1').update(title).digest('hex').slice(0, 16)}`;
        const current = (await store.get(gameKey)) || { seconds: 0, sessions: 0 };
        await store.set(gameKey, {
          title, program, pc, seconds: current.seconds + seconds,
          sessions: current.sessions + 1, last: Date.now(),
        });
        await addLog(user, { kind: 'game', pc, title, seconds, streamed: p.streamed === '1' });
        return ok();
      }
      return fail(400, 'invalid');
    },

    // Everything for the web panel, as JSON
    async panel(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      const now = Date.now();
      const record = await store.get(`user/${user}`);
      const load = async (prefix) => {
        const items = [];
        for (const key of await store.list(prefix)) {
          const value = await store.get(key);
          if (value) items.push({ id: key.split('/').pop(), ...value });
        }
        return items;
      };
      const hosts = (await load(`host/${user}/`)).map((h) => ({
        id: h.id, name: h.name, seen: h.seen, online: now - h.seen < HOST_ONLINE_MS,
        game: now - h.seen < HOST_ONLINE_MS ? h.game || '' : '', game_since: h.game_since || null,
        home: h.local.some((a) => /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(a)),
        router: h.local.some((a) => !/^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.)/.test(a)),
        internet: Boolean(h.public),
      }));
      const devices = (await load(`device/${user}/`)).sort((a, b) => b.last - a.last);
      const games = (await load(`game/${user}/`)).sort((a, b) => b.last - a.last);
      const logKeys = (await store.list(`log/${user}/`)).sort().reverse().slice(0, 120);
      const log = [];
      for (const key of logKeys) {
        const value = await store.get(key);
        if (value) log.push(value);
      }
      return okJson({ user, created: record?.created ?? null, now, hosts, devices, games, log });
    },

    async 'device/remove'(p) {
      const user = await userFromToken(p.token);
      if (!user) return fail(401, 'auth');
      if (!HEX16_RE.test(p.client_id || '')) return fail(400, 'invalid');
      await store.delete(`device/${user}/${p.client_id}`);
      return ok();
    },
  };

  return async function handle(route, params) {
    const handler = Object.hasOwn(routes, route) ? routes[route] : null;
    if (!handler) return fail(404, 'notfound');
    try {
      return await handler(params);
    } catch (error) {
      console.error('charis-conta', route, error);
      return fail(500, 'server');
    }
  };
}

export function encodeFields(fields) {
  return Object.entries(fields)
    .map(([key, value]) => `${key}=${encodeURIComponent(String(value ?? ''))}`)
    .join('\n') + '\n';
}

export function parseForm(text) {
  const params = {};
  for (const [key, value] of new URLSearchParams(String(text || '').slice(0, 4096))) {
    params[key] = value;
  }
  return params;
}
