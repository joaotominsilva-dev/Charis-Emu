// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

import { getStore } from '@netlify/blobs';
import { createApi, encodeFields, parseForm } from '../../lib/core.mjs';

// Strong consistency: a PC has to see a connection request the moment the Switch stored it.
function blobStore() {
  const store = getStore({ name: 'charis-conta', consistency: 'strong' });
  return {
    get: (key) => store.get(key, { type: 'json' }),
    set: async (key, value, options = {}) =>
      (await store.setJSON(key, value, options.onlyIfNew ? { onlyIfNew: true } : {})).modified,
    delete: (key) => store.delete(key),
    list: async (prefix) => (await store.list({ prefix })).blobs.map((blob) => blob.key),
  };
}

let api = null;

export default async (req) => {
  if (req.method !== 'POST') {
    return new Response('error=method\n', { status: 405 });
  }
  api ??= createApi(blobStore());
  const route = new URL(req.url).pathname.replace(/^\/api\//, '').replace(/\/$/, '');
  const { status, fields } = await api(route, parseForm(await req.text()));
  return new Response(encodeFields(fields), {
    status,
    headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' },
  });
};

export const config = {
  path: '/api/*',
  // Slows down password guessing and floods from one address
  rateLimit: { windowLimit: 90, windowSize: 60, aggregateBy: ['ip', 'domain'] },
};
