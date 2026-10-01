// node --test: the gateway against plain TCP servers standing in for the ad
// hoc server's adhocctl and relay ports.
import assert from "node:assert/strict";
import net from "node:net";
import { after, before, test } from "node:test";
import WebSocket from "ws";
import { parseHostPort, startGateway } from "./gateway.mjs";

// Two TCP servers on consecutive ports, as adhocctl and relay are. Each
// connection is recorded; `mode` decides what the server does with it.
async function listenPair(handler) {
  for (let attempt = 0; attempt < 50; ++attempt) {
    const port = 20000 + Math.floor(Math.random() * 40000);
    const servers = [net.createServer((s) => handler(s, "ctl")), net.createServer((s) => handler(s, "relay"))];
    try {
      for (const [i, server] of servers.entries())
        await new Promise((resolve, reject) => {
          server.once("error", reject);
          server.listen(port + i, "127.0.0.1", resolve);
        });
      return { port, close: () => Promise.all(servers.map((s) => new Promise((r) => s.close(r)))) };
    } catch {
      await Promise.all(servers.map((s) => new Promise((r) => (s.listening ? s.close(r) : r()))));
    }
  }
  throw new Error("no free port pair");
}

let backend;
let gateway;
const connections = [];
const lines = [];

before(async () => {
  backend = await listenPair((socket, kind) => {
    const record = { kind, received: [], ended: false, socket };
    connections.push(record);
    socket.on("data", (data) => {
      record.received.push(data);
      const text = data.toString();
      if (text === "close-now") socket.end();
      // Echo, tagged with the port the data arrived on.
      else socket.write(Buffer.concat([Buffer.from(`${kind}:`), data]));
    });
    socket.on("end", () => {
      record.ended = true;
    });
    socket.on("error", () => {});
  });
  gateway = await startGateway({
    listen: "127.0.0.1:0",
    server: `127.0.0.1:${backend.port}`,
    allowedOrigins: [],
    maxPerAddress: 3,
    log: (line) => lines.push(line),
  });
});

after(async () => {
  await gateway.close();
  await backend.close();
});

function open(path, options) {
  const ws = new WebSocket(`ws://127.0.0.1:${gateway.port}${path}`, options);
  ws.binaryType = "nodebuffer";
  return ws;
}

function opened(ws) {
  return new Promise((resolve, reject) => {
    ws.once("open", resolve);
    ws.once("error", reject);
  });
}

function closed(ws) {
  return new Promise((resolve) => ws.once("close", (code) => resolve(code)));
}

// Collects messages until `size` bytes arrived.
function receive(ws, size) {
  return new Promise((resolve) => {
    const parts = [];
    let total = 0;
    const take = (data) => {
      parts.push(data);
      total += data.length;
      if (total >= size) {
        ws.off("message", take);
        resolve(Buffer.concat(parts));
      }
    };
    ws.on("message", take);
  });
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 100));

test("parses server addresses", () => {
  assert.deepEqual(parseHostPort("example.org", 27312), { host: "example.org", port: 27312 });
  assert.deepEqual(parseHostPort("10.0.0.2:4000", 27312), { host: "10.0.0.2", port: 4000 });
  assert.deepEqual(parseHostPort("[fd00::1]:4000", 27312), { host: "fd00::1", port: 4000 });
  assert.deepEqual(parseHostPort("fd00::1", 27312), { host: "fd00::1", port: 27312 });
  assert.throws(() => parseHostPort("host:0", 27312));
});

test("carries bytes both ways to the adhocctl port", async () => {
  const ws = open("/adhoc/ctl");
  await opened(ws);
  const reply = receive(ws, "ctl:".length + 3);
  ws.send(Buffer.from([1, 2, 3]));
  assert.deepEqual([...(await reply)], [...Buffer.from("ctl:"), 1, 2, 3]);
  ws.close();
  await closed(ws);
});

test("carries the relay path to the next port", async () => {
  const ws = open("/adhoc/relay");
  await opened(ws);
  const reply = receive(ws, "relay:".length + 2);
  ws.send(Buffer.from([9, 8]));
  assert.equal((await reply).subarray(0, 6).toString(), "relay:");
  ws.close();
  await closed(ws);
});

test("sends what arrives before the server connection is up, in order", async () => {
  const ws = open("/adhoc/ctl");
  await opened(ws);
  for (let i = 0; i < 20; ++i) ws.send(Buffer.from([i]));
  await settle();
  const record = connections.at(-1);
  assert.deepEqual([...Buffer.concat(record.received)], [...Array(20).keys()]);
  ws.close();
  await closed(ws);
});

test("a browser close ends the server connection", async () => {
  const ws = open("/adhoc/ctl");
  await opened(ws);
  ws.send(Buffer.from([7]));
  await settle();
  const record = connections.at(-1);
  ws.close();
  await closed(ws);
  await settle();
  assert.equal(record.ended, true);
});

test("a server close closes the WebSocket after the data before it", async () => {
  const ws = open("/adhoc/ctl");
  await opened(ws);
  const done = closed(ws);
  ws.send(Buffer.from("close-now"));
  assert.equal(await done, 1000);
});

test("refuses text messages", async () => {
  const ws = open("/adhoc/ctl");
  await opened(ws);
  const done = closed(ws);
  ws.send("hello");
  assert.equal(await done, 1003);
});

test("refuses other paths", async () => {
  const ws = open("/adhoc/elsewhere");
  const status = await new Promise((resolve) => {
    ws.once("unexpected-response", (request, response) => resolve(response.statusCode));
    ws.once("error", () => {});
  });
  assert.equal(status, 404);
});

test("limits connections per address", async () => {
  await settle();
  const sockets = [open("/adhoc/ctl"), open("/adhoc/ctl"), open("/adhoc/ctl")];
  await Promise.all(sockets.map(opened));
  const extra = open("/adhoc/ctl");
  const status = await new Promise((resolve) => {
    extra.once("unexpected-response", (request, response) => resolve(response.statusCode));
    extra.once("error", () => {});
  });
  assert.equal(status, 429);
  await Promise.all(sockets.map((ws) => (ws.close(), closed(ws))));
});

test("checks the origin when origins are listed", async () => {
  const strict = await startGateway({
    listen: "127.0.0.1:0",
    server: `127.0.0.1:${backend.port}`,
    allowedOrigins: ["https://yakumo.example.com"],
    log: () => {},
  });
  try {
    const good = new WebSocket(`ws://127.0.0.1:${strict.port}/adhoc/ctl`, { origin: "https://yakumo.example.com" });
    await opened(good);
    good.close();
    await closed(good);
    const bad = new WebSocket(`ws://127.0.0.1:${strict.port}/adhoc/ctl`, { origin: "https://elsewhere.example" });
    const status = await new Promise((resolve) => {
      bad.once("unexpected-response", (request, response) => resolve(response.statusCode));
      bad.once("error", () => {});
    });
    assert.equal(status, 403);
  } finally {
    await strict.close();
  }
});

test("closes the WebSocket when the server is unreachable", async () => {
  const lonely = await startGateway({ listen: "127.0.0.1:0", server: "127.0.0.1:1", log: () => {} });
  try {
    const ws = new WebSocket(`ws://127.0.0.1:${lonely.port}/adhoc/ctl`);
    await opened(ws);
    assert.equal(await closed(ws), 1011);
  } finally {
    await lonely.close();
  }
});
