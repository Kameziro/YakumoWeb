#!/usr/bin/env node
// Carries the web port's ad hoc connections to a PSP ad hoc server.
//
// A browser cannot open TCP connections, so the web port's ad hoc client
// (profiles/mhp3rd/host/adhoc/client.cpp) opens a WebSocket for every TCP
// connection the native client would open, and this gateway opens that TCP
// connection for it:
//
//   <path>/ctl    -> the server's adhocctl port (27312 by default)
//   <path>/relay  -> the relay port, the adhocctl port + 1
//
// Binary messages carry the TCP byte stream unchanged, in both directions, so
// the server sees what a native client would send, and web and native players
// share its sessions. The gateway only ever connects to the one server it is
// configured with; any other path is refused.
//
//   node gateway.mjs
//
// Environment:
//   ADHOC_GATEWAY_LISTEN   address and port to listen on (127.0.0.1:27380)
//   ADHOC_SERVER           the ad hoc server, host or host:port (127.0.0.1:27312)
//   ADHOC_GATEWAY_PATH     path prefix (/adhoc)
//   ADHOC_ALLOWED_ORIGIN   comma-separated page origins allowed to connect; any when empty
//   ADHOC_MAX_PER_ADDRESS  connections at once per client address (64)
import http from "node:http";
import net from "node:net";
import { resolve as resolvePath } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";

export const kAdhocctlPort = 27312;

// Larger than the relay's largest stream block (50 KiB) with its header.
const kMaxMessage = 256 * 1024;
// Above this many bytes waiting for the browser, reading from the server stops.
const kHighWater = 1024 * 1024;
const kLowWater = 256 * 1024;
const kConnectTimeoutMs = 8000;
// Keeps proxies between the browser and the gateway from closing quiet relay
// connections.
const kPingIntervalMs = 20000;

// "host", "host:port", "[v6]" or "[v6]:port".
// Port 0 only where `anyPort` allows it, for listening on any free port.
export function parseHostPort(text, defaultPort, anyPort = false) {
  let host = text;
  let port = defaultPort;
  const bracket = /^\[([^\]]+)\](?::(\d+))?$/.exec(text);
  if (bracket) {
    host = bracket[1];
    if (bracket[2]) port = Number(bracket[2]);
  } else if (text.split(":").length === 2) {
    const [name, number] = text.split(":");
    host = name;
    port = Number(number);
  }
  if (!host || !Number.isInteger(port) || port < (anyPort ? 0 : 1) || port > 65535) throw new Error(`bad address "${text}"`);
  return { host, port };
}

function timestamp() {
  return new Date().toISOString().slice(11, 23);
}

// The client's address: the forwarding proxy's header when the connection
// comes from a proxy on this machine, otherwise the peer's own address.
function clientAddress(request) {
  const peer = request.socket.remoteAddress || "?";
  const local = peer === "127.0.0.1" || peer === "::1" || peer === "::ffff:127.0.0.1";
  if (local) {
    const real = request.headers["x-real-ip"];
    if (typeof real === "string" && real) return real;
    const forwarded = request.headers["x-forwarded-for"];
    if (typeof forwarded === "string" && forwarded) return forwarded.split(",").pop().trim();
  }
  return peer;
}

// Starts a gateway. Resolves to { port, close() } once it listens.
export function startGateway({
  listen = "127.0.0.1:27380",
  server = `127.0.0.1:${kAdhocctlPort}`,
  path = "/adhoc",
  allowedOrigins = [],
  maxPerAddress = 64,
  log = (line) => console.log(`${timestamp()} ${line}`),
} = {}) {
  const target = parseHostPort(server, kAdhocctlPort);
  const relayPort = target.port === 65535 ? 27313 : target.port + 1;
  const ports = { [`${path}/ctl`]: target.port, [`${path}/relay`]: relayPort };
  const bind = parseHostPort(listen, 27380, true);
  const perAddress = new Map();
  const sockets = new Set();

  const httpServer = http.createServer((request, response) => {
    response.writeHead(426, { "Content-Type": "text/plain" });
    response.end("WebSocket only\n");
  });
  const wss = new WebSocketServer({ noServer: true, maxPayload: kMaxMessage, perMessageDeflate: false });

  const refuse = (socket, status, reason) => {
    socket.end(`HTTP/1.1 ${status} ${reason}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  };

  httpServer.on("upgrade", (request, socket, head) => {
    const address = clientAddress(request);
    const route = new URL(request.url, "http://gateway").pathname;
    const port = ports[route];
    if (port === undefined) return refuse(socket, 404, "Not Found");
    const origin = request.headers.origin;
    if (allowedOrigins.length > 0 && !allowedOrigins.includes(origin)) {
      log(`refused ${address}: origin ${origin ?? "none"}`);
      return refuse(socket, 403, "Forbidden");
    }
    const count = perAddress.get(address) ?? 0;
    if (count >= maxPerAddress) {
      log(`refused ${address}: ${count} connections already`);
      return refuse(socket, 429, "Too Many Requests");
    }
    wss.handleUpgrade(request, socket, head, (ws) => carry(ws, address, route.slice(path.length + 1), port));
  });

  function carry(ws, address, kind, port) {
    perAddress.set(address, (perAddress.get(address) ?? 0) + 1);
    sockets.add(ws);
    const name = `${kind} ${address}`;
    let bytesIn = 0;
    let bytesOut = 0;
    let connected = false;
    let finished = false;
    const pending = [];

    const tcp = net.connect({ host: target.host, port });
    tcp.setNoDelay(true);
    const connectTimer = setTimeout(() => {
      log(`${name}: the server did not answer`);
      tcp.destroy();
    }, kConnectTimeoutMs);

    let alive = true;
    const pinger = setInterval(() => {
      if (!alive) return ws.terminate();
      alive = false;
      ws.ping();
    }, kPingIntervalMs);
    ws.on("pong", () => {
      alive = true;
    });

    const finish = (why) => {
      if (finished) return;
      finished = true;
      clearTimeout(connectTimer);
      clearInterval(pinger);
      sockets.delete(ws);
      const left = (perAddress.get(address) ?? 1) - 1;
      if (left > 0) perAddress.set(address, left);
      else perAddress.delete(address);
      log(`${name} closed (${why}), ${bytesIn} bytes in, ${bytesOut} out`);
    };

    tcp.on("connect", () => {
      clearTimeout(connectTimer);
      connected = true;
      for (const data of pending) tcp.write(data);
      pending.length = 0;
    });
    tcp.on("data", (data) => {
      bytesOut += data.length;
      ws.send(data, { binary: true }, () => {
        if (tcp.isPaused() && ws.bufferedAmount < kLowWater) tcp.resume();
      });
      if (ws.bufferedAmount > kHighWater) tcp.pause();
    });
    tcp.on("drain", () => ws.resume());
    tcp.on("error", (error) => {
      log(`${name}: ${error.code ?? error.message}`);
    });
    tcp.on("close", () => {
      // Data already queued for the browser goes out before the close frame.
      if (ws.readyState === ws.OPEN) ws.close(connected ? 1000 : 1011, connected ? "" : "server unreachable");
      finish(connected ? "server closed" : "server unreachable");
    });

    ws.on("message", (data, isBinary) => {
      if (!isBinary) {
        ws.close(1003, "binary only");
        return;
      }
      bytesIn += data.length;
      if (!connected) {
        pending.push(data);
        return;
      }
      if (!tcp.write(data)) ws.pause();
    });
    ws.on("error", () => tcp.destroy());
    ws.on("close", () => {
      // Lets the server deliver what it was sent before the connection ends.
      tcp.end();
      setTimeout(() => tcp.destroy(), 5000).unref();
      finish("browser closed");
    });
    log(`${name} opened`);
  }

  return new Promise((resolve, reject) => {
    httpServer.once("error", reject);
    httpServer.listen(bind.port, bind.host, () => {
      httpServer.off("error", reject);
      resolve({
        port: httpServer.address().port,
        close: () =>
          new Promise((done) => {
            for (const ws of sockets) ws.terminate();
            wss.close();
            httpServer.close(() => done());
          }),
      });
    });
  });
}

async function main() {
  const env = process.env;
  const options = {
    listen: env.ADHOC_GATEWAY_LISTEN || "127.0.0.1:27380",
    server: env.ADHOC_SERVER || `127.0.0.1:${kAdhocctlPort}`,
    path: (env.ADHOC_GATEWAY_PATH || "/adhoc").replace(/\/+$/, ""),
    allowedOrigins: (env.ADHOC_ALLOWED_ORIGIN || "").split(",").map((s) => s.trim()).filter(Boolean),
    maxPerAddress: Number(env.ADHOC_MAX_PER_ADDRESS || 64),
  };
  const gateway = await startGateway(options);
  console.log(
    `${timestamp()} ad hoc gateway on ${options.listen}: ${options.path}/ctl and ${options.path}/relay ` +
      `to ${options.server} and the port after it`,
  );
  const stop = () => gateway.close().then(() => process.exit(0));
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolvePath(process.argv[1])) {
  main().catch((error) => {
    console.error(`ad hoc gateway: ${error.message}`);
    process.exit(1);
  });
}
