import { DurableObject } from "cloudflare:workers";

const PROTOCOL_VERSION = 1;
const WEBSOCKET_PROTOCOL_VERSION = 2;
const CLIENT_TTL_MS = 15_000;
const MAX_ROOM_PEERS = 256;
const MAX_BODY_BYTES = 2_048;
const PEER_KEY_PREFIX = "peer:";

const ROOM_PATTERN = /^[a-f0-9]{64}$/;
const TOKEN_PATTERN = /^[a-f0-9]{32}$/;
const SHA256_PATTERN = /^[a-f0-9]{64}$/;
const MODEL_ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,63}$/;
const VERSION_PATTERN = /^[0-9A-Za-z][0-9A-Za-z._+-]{0,31}$/;

export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
}

interface SyncInput {
  protocol: 1;
  room: string;
  player: number;
  token: string;
  modelId: string;
  modelVersion: string;
  packSha256: string;
}

interface LeaveInput {
  protocol: 1;
  room: string;
  player: number;
  token: string;
}

interface WebSocketHelloInput {
  protocol: 2;
  type: "hello";
  player: number;
  token: string;
  modelId: string;
  modelVersion: string;
  packSha256: string;
}

interface WebSocketUpdateInput {
  protocol: 2;
  type: "update";
  modelId: string;
  modelVersion: string;
  packSha256: string;
}

interface PublicPeer {
  player: number;
  modelId: string;
  modelVersion: string;
  packSha256: string;
  updatedAt: number;
}

interface StoredPeer extends PublicPeer {
  token: string;
  expiresAt: number;
}

interface PendingSocket {
  joined: false;
}

interface JoinedSocket extends PublicPeer {
  joined: true;
  token: string;
}

type SocketAttachment = PendingSocket | JoinedSocket;

interface RoomResult {
  status: number;
  body: Record<string, unknown>;
}

function json(body: unknown, status = 200): Response {
  return Response.json(body, {
    status,
    headers: {
      "Cache-Control": "no-store",
      "Content-Type": "application/json; charset=utf-8",
      "X-Content-Type-Options": "nosniff",
    },
  });
}

function error(code: string, message: string, status: number): Response {
  return json({ error: code, message }, status);
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isProtocol(value: unknown): value is 1 {
  return value === PROTOCOL_VERSION;
}

function isPlayerId(value: unknown): value is number {
  return Number.isInteger(value) && Number(value) >= 1 && Number(value) <= 65_535;
}

function parseSync(value: unknown): SyncInput | null {
  if (!isObject(value) ||
      !isProtocol(value.protocol) ||
      typeof value.room !== "string" || !ROOM_PATTERN.test(value.room) ||
      !isPlayerId(value.player) ||
      typeof value.token !== "string" || !TOKEN_PATTERN.test(value.token) ||
      typeof value.modelId !== "string" || !MODEL_ID_PATTERN.test(value.modelId) ||
      typeof value.modelVersion !== "string" || !VERSION_PATTERN.test(value.modelVersion) ||
      typeof value.packSha256 !== "string" || !SHA256_PATTERN.test(value.packSha256)) {
    return null;
  }

  return {
    protocol: 1,
    room: value.room,
    player: value.player,
    token: value.token,
    modelId: value.modelId,
    modelVersion: value.modelVersion,
    packSha256: value.packSha256,
  };
}

function parseLeave(value: unknown): LeaveInput | null {
  if (!isObject(value) ||
      !isProtocol(value.protocol) ||
      typeof value.room !== "string" || !ROOM_PATTERN.test(value.room) ||
      !isPlayerId(value.player) ||
      typeof value.token !== "string" || !TOKEN_PATTERN.test(value.token)) {
    return null;
  }

  return {
    protocol: 1,
    room: value.room,
    player: value.player,
    token: value.token,
  };
}

function hasValidModel(value: Record<string, unknown>): boolean {
  return typeof value.modelId === "string" && MODEL_ID_PATTERN.test(value.modelId) &&
    typeof value.modelVersion === "string" && VERSION_PATTERN.test(value.modelVersion) &&
    typeof value.packSha256 === "string" && SHA256_PATTERN.test(value.packSha256);
}

function parseWebSocketHello(value: unknown): WebSocketHelloInput | null {
  if (!isObject(value) || value.protocol !== WEBSOCKET_PROTOCOL_VERSION || value.type !== "hello" ||
      !isPlayerId(value.player) || typeof value.token !== "string" || !TOKEN_PATTERN.test(value.token) ||
      !hasValidModel(value)) {
    return null;
  }

  return {
    protocol: 2,
    type: "hello",
    player: value.player,
    token: value.token,
    modelId: value.modelId as string,
    modelVersion: value.modelVersion as string,
    packSha256: value.packSha256 as string,
  };
}

function parseWebSocketUpdate(value: unknown): WebSocketUpdateInput | null {
  if (!isObject(value) || value.protocol !== WEBSOCKET_PROTOCOL_VERSION || value.type !== "update" ||
      !hasValidModel(value)) {
    return null;
  }

  return {
    protocol: 2,
    type: "update",
    modelId: value.modelId as string,
    modelVersion: value.modelVersion as string,
    packSha256: value.packSha256 as string,
  };
}

async function readJson(request: Request): Promise<unknown> {
  const declaredLength = Number(request.headers.get("Content-Length") ?? "0");
  if (Number.isFinite(declaredLength) && declaredLength > MAX_BODY_BYTES) {
    throw new RangeError("body_too_large");
  }

  const text = await request.text();
  if (new TextEncoder().encode(text).byteLength > MAX_BODY_BYTES) {
    throw new RangeError("body_too_large");
  }
  return JSON.parse(text) as unknown;
}

export class Room extends DurableObject<Env> {
  private readonly peers = new Map<number, StoredPeer>();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      const stored = await ctx.storage.list<StoredPeer>({ prefix: PEER_KEY_PREFIX });
      for (const peer of stored.values()) {
        this.peers.set(peer.player, peer);
      }
      await this.cleanupExpired(Date.now());
      await this.scheduleAlarm();
    });
  }

  async sync(input: SyncInput): Promise<RoomResult> {
    const now = Date.now();
    await this.cleanupExpired(now);

    const existing = this.peers.get(input.player);
    if (existing && existing.token !== input.token) {
      return {
        status: 409,
        body: {
          error: "player_claimed",
          message: "This player id is already claimed by another live session.",
          retryAfterMs: Math.max(0, existing.expiresAt - now),
        },
      };
    }

    const socketClaim = this.joinedSockets()
      .find(({ attachment }) => attachment.player === input.player)?.attachment;
    if (socketClaim && socketClaim.token !== input.token) {
      return {
        status: 409,
        body: {
          error: "player_claimed",
          message: "This player id is already claimed by another live session.",
        },
      };
    }

    if (!existing && !socketClaim && this.currentPeers(-1, now).length >= MAX_ROOM_PEERS) {
      return {
        status: 429,
        body: {
          error: "room_full",
          message: "This room reached its peer limit.",
        },
      };
    }

    const peer: StoredPeer = {
      player: input.player,
      token: input.token,
      modelId: input.modelId,
      modelVersion: input.modelVersion,
      packSha256: input.packSha256,
      updatedAt: now,
      expiresAt: now + CLIENT_TTL_MS,
    };

    this.peers.set(peer.player, peer);
    await this.ctx.storage.put(`${PEER_KEY_PREFIX}${peer.player}`, peer);
    await this.scheduleAlarm();

    this.broadcastSnapshots();

    const peers = this.currentPeers(input.player, now);

    return {
      status: 200,
      body: {
        protocol: PROTOCOL_VERSION,
        serverTime: now,
        ttlMs: CLIENT_TTL_MS,
        peers,
      },
    };
  }

  async leave(input: LeaveInput): Promise<RoomResult> {
    const now = Date.now();
    await this.cleanupExpired(now);

    const existing = this.peers.get(input.player);
    if (existing && existing.token !== input.token) {
      return {
        status: 403,
        body: {
          error: "token_mismatch",
          message: "The player claim belongs to another session.",
        },
      };
    }

    if (existing) {
      this.peers.delete(input.player);
      await this.ctx.storage.delete(`${PEER_KEY_PREFIX}${input.player}`);
    }
    await this.scheduleAlarm();
    this.broadcastSnapshots();

    return {
      status: 200,
      body: { protocol: PROTOCOL_VERSION, removed: Boolean(existing) },
    };
  }

  async alarm(): Promise<void> {
    await this.cleanupExpired(Date.now());
    await this.scheduleAlarm();
    this.broadcastSnapshots();
  }

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return error("upgrade_required", "This endpoint requires a WebSocket upgrade.", 426);
    }

    if (this.ctx.getWebSockets().length >= MAX_ROOM_PEERS) {
      console.log(`[Relay:WS] Room full (${this.ctx.getWebSockets().length}/${MAX_ROOM_PEERS}), rejecting connection`);
      return error("room_full", "This room reached its peer limit.", 429);
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.serializeAttachment({ joined: false } satisfies PendingSocket);
    this.ctx.acceptWebSocket(server);

    console.log(`[Relay:WS] Accepted new WebSocket connection (total sockets: ${this.ctx.getWebSockets().length})`);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message !== "string" || new TextEncoder().encode(message).byteLength > MAX_BODY_BYTES) {
      ws.close(1008, "invalid_message");
      return;
    }

    let value: unknown;
    try {
      value = JSON.parse(message) as unknown;
    } catch {
      ws.close(1008, "invalid_json");
      return;
    }

    const attachment = ws.deserializeAttachment() as SocketAttachment | null;
    if (!attachment?.joined) {
      const hello = parseWebSocketHello(value);
      if (!hello) {
        console.log("[Relay:WS] Rejected invalid hello payload");
        ws.close(1008, "invalid_hello");
        return;
      }

      await this.cleanupExpired(Date.now());
      const legacy = this.peers.get(hello.player);
      if (legacy && legacy.token !== hello.token) {
        console.log(`[Relay:WS] Player ${hello.player} token mismatch with legacy session`);
        ws.close(1008, "player_claimed");
        return;
      }

      const existingSockets = this.joinedSockets()
        .filter(({ attachment: peer }) => peer.player === hello.player);
      if (existingSockets.some(({ attachment: peer }) => peer.token !== hello.token)) {
        console.log(`[Relay:WS] Player ${hello.player} token mismatch with existing socket`);
        ws.close(1008, "player_claimed");
        return;
      }

      if (existingSockets.length === 0 && !legacy && this.currentPeers(-1).length >= MAX_ROOM_PEERS) {
        console.log(`[Relay:WS] Room full for new player ${hello.player}`);
        ws.close(1008, "room_full");
        return;
      }

      for (const existing of existingSockets) {
        existing.socket.close(1000, "replaced");
      }

      if (legacy) {
        this.peers.delete(hello.player);
        await this.ctx.storage.delete(`${PEER_KEY_PREFIX}${hello.player}`);
        await this.scheduleAlarm();
      }

      const joined: JoinedSocket = {
        joined: true,
        player: hello.player,
        token: hello.token,
        modelId: hello.modelId,
        modelVersion: hello.modelVersion,
        packSha256: hello.packSha256,
        updatedAt: Date.now(),
      };
      ws.serializeAttachment(joined);
      console.log(`[Relay:WS] Player ${hello.player} JOINED | model=${hello.modelId} (pack=${hello.packSha256.slice(0, 8)}...)`);
      this.broadcastSnapshots();
      return;
    }

    if (isObject(value) && value.protocol === WEBSOCKET_PROTOCOL_VERSION && value.type === "leave") {
      console.log(`[Relay:WS] Player ${attachment.player} requested LEAVE`);
      ws.close(1000, "left");
      return;
    }

    const update = parseWebSocketUpdate(value);
    if (!update) {
      console.log(`[Relay:WS] Player ${attachment.player} sent invalid update payload`);
      ws.close(1008, "invalid_update");
      return;
    }


    if (
      attachment.modelId === update.modelId &&
      attachment.modelVersion === update.modelVersion &&
      attachment.packSha256 === update.packSha256
    ) {
      return;
    }



    const now = Date.now();
    ws.serializeAttachment({
      ...attachment,
      modelId: update.modelId,
      modelVersion: update.modelVersion,
      packSha256: update.packSha256,
      updatedAt: now,
    } satisfies JoinedSocket);
    console.log(`[Relay:WS] Player ${attachment.player} UPDATED model -> ${update.modelId}`);
    this.broadcastSnapshots();
  }

  webSocketClose(ws: WebSocket): void {


    ws.close(1000, "closed");
    const attachment = ws.deserializeAttachment() as SocketAttachment | null;
    if (attachment?.joined) {
      console.log(`[Relay:WS] Player ${attachment.player} DISCONNECTED (socket closed)`);
    } else {
      console.log("[Relay:WS] Unjoined socket closed");
    }
    this.broadcastSnapshots(ws);
  }

  webSocketError(ws: WebSocket): void {
    ws.close(1011, "socket_error");
    const attachment = ws.deserializeAttachment() as SocketAttachment | null;
    if (attachment?.joined) {
      console.log(`[Relay:WS] Player ${attachment.player} socket error`);
    }
    this.broadcastSnapshots(ws);
  }

  private async cleanupExpired(now: number): Promise<void> {
    const expiredKeys: string[] = [];
    for (const [player, peer] of this.peers) {
      if (peer.expiresAt <= now) {
        this.peers.delete(player);
        expiredKeys.push(`${PEER_KEY_PREFIX}${player}`);
      }
    }

    if (expiredKeys.length > 0) {
      await this.ctx.storage.delete(expiredKeys);
    }
  }

  private async scheduleAlarm(): Promise<void> {
    let earliest = Number.POSITIVE_INFINITY;
    for (const peer of this.peers.values()) {
      earliest = Math.min(earliest, peer.expiresAt);
    }

    const current = await this.ctx.storage.getAlarm();
    if (Number.isFinite(earliest)) {
      if (current === null || current !== earliest) {
        await this.ctx.storage.setAlarm(earliest);
      }
    } else if (current !== null) {
      await this.ctx.storage.deleteAlarm();
    }
  }

  private joinedSockets(excluded?: WebSocket): Array<{ socket: WebSocket; attachment: JoinedSocket }> {
    const result: Array<{ socket: WebSocket; attachment: JoinedSocket }> = [];
    for (const socket of this.ctx.getWebSockets()) {
      if (socket === excluded || socket.readyState !== WebSocket.OPEN) {
        continue;
      }
      const attachment = socket.deserializeAttachment() as SocketAttachment | null;
      if (attachment?.joined) {
        result.push({ socket, attachment });
      }
    }
    return result;
  }

  private currentPeers(excludedPlayer: number, now = Date.now(), excludedSocket?: WebSocket): PublicPeer[] {
    const byPlayer = new Map<number, PublicPeer>();
    for (const peer of this.peers.values()) {
      if (peer.player !== excludedPlayer && peer.expiresAt > now) {
        byPlayer.set(peer.player, {
          player: peer.player,
          modelId: peer.modelId,
          modelVersion: peer.modelVersion,
          packSha256: peer.packSha256,
          updatedAt: peer.updatedAt,
        });
      }
    }
    for (const { attachment } of this.joinedSockets(excludedSocket)) {
      if (attachment.player !== excludedPlayer) {
        byPlayer.set(attachment.player, {
          player: attachment.player,
          modelId: attachment.modelId,
          modelVersion: attachment.modelVersion,
          packSha256: attachment.packSha256,
          updatedAt: attachment.updatedAt,
        });
      }
    }
    return [...byPlayer.values()].sort((left, right) => left.player - right.player);
  }

  private broadcastSnapshots(excludedSocket?: WebSocket): void {
    const sockets = this.joinedSockets(excludedSocket);
    for (const { socket, attachment } of sockets) {
      try {
        socket.send(JSON.stringify({
          protocol: WEBSOCKET_PROTOCOL_VERSION,
          type: "snapshot",
          serverTime: Date.now(),
          peers: this.currentPeers(attachment.player, Date.now(), excludedSocket),
        }));
      } catch {

      }
    }
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    console.log(`[Relay:HTTP] ${request.method} ${url.pathname}`);

    if (request.method === "GET" && url.pathname === "/health") {
      return json({ ok: true, service: "modelcustom-relay", protocol: WEBSOCKET_PROTOCOL_VERSION });
    }

    if (request.method === "GET" && url.pathname === "/v2/connect") {
      const room = url.searchParams.get("room");
      if (!room || !ROOM_PATTERN.test(room)) {
        return error("invalid_room", "The room identifier is invalid.", 400);
      }
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
        return error("upgrade_required", "This endpoint requires a WebSocket upgrade.", 426);
      }
      return env.ROOMS.getByName(room).fetch(request);
    }

    if (request.method !== "POST") {
      return error("not_found", "Route not found.", 404);
    }

    try {
      const value = await readJson(request);

      if (url.pathname === "/v1/sync") {
        const input = parseSync(value);
        if (!input) {
          return error("invalid_request", "The sync payload is invalid.", 400);
        }

        const result = await env.ROOMS.getByName(input.room).sync(input) as RoomResult;
        return json(result.body, result.status);
      }

      if (url.pathname === "/v1/leave") {
        const input = parseLeave(value);
        if (!input) {
          return error("invalid_request", "The leave payload is invalid.", 400);
        }

        const result = await env.ROOMS.getByName(input.room).leave(input) as RoomResult;
        return json(result.body, result.status);
      }

      return error("not_found", "Route not found.", 404);
    } catch (caught) {
      if (caught instanceof RangeError && caught.message === "body_too_large") {
        return error("body_too_large", "The request body exceeds 2048 bytes.", 413);
      }
      if (caught instanceof SyntaxError) {
        return error("invalid_json", "The request body is not valid JSON.", 400);
      }

      return error("internal_error", "The relay could not process the request.", 500);
    }
  },
} satisfies ExportedHandler<Env>;
