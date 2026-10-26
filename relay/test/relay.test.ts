import { exports } from "cloudflare:workers";
import { afterEach, describe, expect, it } from "vitest";

const ROOM_A = "a".repeat(64);
const ROOM_B = "b".repeat(64);
const PACK = "c".repeat(64);
const TOKEN_1 = "1".repeat(32);
const TOKEN_2 = "2".repeat(32);
const TOKEN_3 = "3".repeat(32);
const TEST_ENDPOINT = "https://your-endpoint.invalid";
const sockets: WebSocket[] = [];
const worker = (exports as unknown as {
  default: {
    fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response>;
  };
}).default;

function post(path: string, body: unknown): Promise<Response> {
  return worker.fetch(`${TEST_ENDPOINT}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

function sync(room: string, player: number, token: string, modelId = "shrek") {
  return post("/v1/sync", {
    protocol: 1,
    room,
    player,
    token,
    modelId,
    modelVersion: "1.0.0",
    packSha256: PACK,
  });
}

async function openSocket(room: string): Promise<WebSocket> {
  const response = await worker.fetch(`${TEST_ENDPOINT}/v2/connect?room=${room}`, {
    headers: { Upgrade: "websocket" },
  });
  expect(response.status).toBe(101);
  const socket = response.webSocket;
  expect(socket).not.toBeNull();
  socket!.accept();
  sockets.push(socket!);
  return socket!;
}

function closeSocket(socket: WebSocket): Promise<void> {
  if (socket.readyState === WebSocket.CLOSED) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out closing WebSocket")), 2_000);
    socket.addEventListener("close", () => {
      clearTimeout(timer);
      resolve();
    }, { once: true });
    if (socket.readyState === WebSocket.OPEN) socket.close(1000, "test complete");
  });
}

afterEach(async () => {
  await Promise.all(sockets.splice(0).map(closeSocket));
});

function nextMessage(socket: WebSocket): Promise<Record<string, unknown>> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out waiting for WebSocket message")), 2_000);
    socket.addEventListener("message", (event) => {
      clearTimeout(timer);
      resolve(JSON.parse(String(event.data)) as Record<string, unknown>);
    }, { once: true });
  });
}

function hello(player: number, token: string, modelId = "shrek"): string {
  return JSON.stringify({
    protocol: 2,
    type: "hello",
    player,
    token,
    modelId,
    modelVersion: "1.0.0",
    packSha256: PACK,
  });
}

describe("ModelCustom relay", () => {
  it("reports health without touching a room", async () => {
    const response = await worker.fetch(`${TEST_ENDPOINT}/health`);
    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toMatchObject({ ok: true, protocol: 2 });
  });

  it("returns compatible peers in the same room and excludes self", async () => {
    const first = await sync(ROOM_A, 10, TOKEN_1);
    expect(first.status).toBe(200);
    await expect(first.json()).resolves.toMatchObject({ peers: [] });

    const second = await sync(ROOM_A, 20, TOKEN_2, "tung-tung");
    expect(second.status).toBe(200);
    await expect(second.json()).resolves.toMatchObject({
      peers: [{ player: 10, modelId: "shrek", packSha256: PACK }],
    });

    const refreshed = await sync(ROOM_A, 10, TOKEN_1);
    await expect(refreshed.json()).resolves.toMatchObject({
      peers: [{ player: 20, modelId: "tung-tung", packSha256: PACK }],
    });
  });

  it("isolates different FiveM server rooms", async () => {
    const response = await sync(ROOM_B, 30, TOKEN_3);
    expect(response.status).toBe(200);
    await expect(response.json()).resolves.toMatchObject({ peers: [] });
  });

  it("rejects a second live claim for the same player id", async () => {
    const room = "d".repeat(64);
    expect((await sync(room, 40, TOKEN_1)).status).toBe(200);
    const collision = await sync(room, 40, TOKEN_2);
    expect(collision.status).toBe(409);
    await expect(collision.json()).resolves.toMatchObject({ error: "player_claimed" });
  });

  it("removes a peer only when its token matches", async () => {
    const room = "e".repeat(64);
    expect((await sync(room, 50, TOKEN_1)).status).toBe(200);

    const denied = await post("/v1/leave", {
      protocol: 1,
      room,
      player: 50,
      token: TOKEN_2,
    });
    expect(denied.status).toBe(403);

    const removed = await post("/v1/leave", {
      protocol: 1,
      room,
      player: 50,
      token: TOKEN_1,
    });
    expect(removed.status).toBe(200);
    await expect(removed.json()).resolves.toMatchObject({ removed: true });

    const observer = await sync(room, 51, TOKEN_3);
    await expect(observer.json()).resolves.toMatchObject({ peers: [] });
  });

  it("rejects malformed identifiers", async () => {
    const response = await sync("not-a-room", 1, TOKEN_1);
    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toMatchObject({ error: "invalid_request" });
  });

  it("pushes room snapshots only when WebSocket presence changes", async () => {
    const room = "f".repeat(64);
    const first = await openSocket(room);
    const firstJoined = nextMessage(first);
    first.send(hello(60, TOKEN_1));
    await expect(firstJoined).resolves.toMatchObject({
      protocol: 2,
      type: "snapshot",
      peers: [],
    });

    const firstSeesSecond = nextMessage(first);
    const second = await openSocket(room);
    const secondJoined = nextMessage(second);
    second.send(hello(61, TOKEN_2, "tung-tung"));
    await Promise.all([
      expect(secondJoined).resolves.toMatchObject({
        peers: [{ player: 60, modelId: "shrek" }],
      }),
      expect(firstSeesSecond).resolves.toMatchObject({
        peers: [{ player: 61, modelId: "tung-tung" }],
      }),
    ]);

    const firstSeesUpdate = nextMessage(first);
    const secondSeesUpdate = nextMessage(second);
    second.send(JSON.stringify({
      protocol: 2,
      type: "update",
      modelId: "sonic",
      modelVersion: "1.0.0",
      packSha256: PACK,
    }));
    await Promise.all([
      expect(firstSeesUpdate).resolves.toMatchObject({
        peers: [{ player: 61, modelId: "sonic" }],
      }),
      expect(secondSeesUpdate).resolves.toMatchObject({
        peers: [{ player: 60, modelId: "shrek" }],
      }),
    ]);

    const firstSeesLeave = nextMessage(first);
    second.close(1000, "test complete");
    await expect(firstSeesLeave).resolves.toMatchObject({ peers: [] });
    first.close(1000, "test complete");
  });
});
