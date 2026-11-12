// Local stand-in for the Netlify function, with an in-memory store. For tests only:
//   node dev-server.mjs [port]
import { readFile } from 'node:fs/promises';
import http from 'node:http';
import { createApi, encodeFields, parseForm } from './lib/core.mjs';

const data = new Map();
const api = createApi({
  get: async (key) => (data.has(key) ? structuredClone(data.get(key)) : null),
  set: async (key, value, options = {}) => {
    if (options.onlyIfNew && data.has(key)) return false;
    data.set(key, structuredClone(value));
    return true;
  },
  delete: async (key) => void data.delete(key),
  list: async (prefix) => [...data.keys()].filter((key) => key.startsWith(prefix)),
});

const port = Number(process.argv[2] || 8788);
http.createServer(async (req, res) => {
  let body = '';
  for await (const chunk of req) body += chunk;
  const route = new URL(req.url, 'http://x').pathname.replace(/^\/api\//, '').replace(/\/$/, '');
  if (req.method === 'GET') {
    // The web panel and its files, like Netlify serves them
    const path = new URL(req.url, 'http://x').pathname.replace(/\/$/, '/index.html');
    try {
      const file = await readFile(new URL(`./public${path}`, import.meta.url));
      const type = { html: 'text/html', js: 'text/javascript', css: 'text/css', ttf: 'font/ttf',
        svg: 'image/svg+xml', png: 'image/png' }[path.split('.').pop()] || 'application/octet-stream';
      res.writeHead(200, { 'content-type': type });
      res.end(file);
    } catch {
      res.writeHead(404);
      res.end('not found');
    }
    return;
  }
  const { status, fields, json } = req.method === 'POST'
    ? await api(route, parseForm(body))
    : { status: 405, fields: { error: 'method' } };
  if (json) {
    res.writeHead(status, { 'content-type': 'application/json' });
    res.end(JSON.stringify(json));
    return;
  }
  res.writeHead(status, { 'content-type': 'text/plain; charset=utf-8' });
  res.end(encodeFields(fields));
}).listen(port, '127.0.0.1', () => console.log(`charis-conta dev server on ${port}`));
