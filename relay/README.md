# ModelCustom relay

Server-independent presence relay for ModelCustom clients. It does not alter or
proxy FiveM traffic. The client hashes the current FiveM server endpoint and
uses that digest as the room name. The first connection creates the room's
Durable Object; later clients on the same server are routed to the same object.

No hosted endpoint is configured in the release. Deploy your own Worker and use
its URL on each client that should discover the others.

Current clients open one hibernatable WebSocket for their FiveM session. The
room pushes a fresh peer snapshot when a client joins, changes model, disables
its model, or leaves. There is no periodic HTTP presence poll. WinHTTP protocol
keepalives do not invoke the Worker application.

Protocol v1 `POST /v1/sync` and `POST /v1/leave` are retained for legacy clients.
They use a 15-second SQLite-backed lease.

## Deploy

```powershell
npm ci
npm test
npm run check
npm run dry-run
npx wrangler login
npm run deploy
```

No resource is installed on the FiveM server. Set the deployed Worker URL in
`ModelCustom.ini`:

```ini
relay_url=YOUR_ENDPOINT
```

Use Node.js 22 or newer and a Cloudflare account. Run these commands from the
`relay` directory. `npm run dry-run` only bundles locally; `deploy` publishes
the Worker. Restart FiveM after changing `relay_url`.

Replace `YOUR_ENDPOINT` with the HTTPS URL returned by your deployment. No
private endpoint, account ID or credentials are included in the source.

For local development, run `npm run dev -- --port 8787` and set
`relay_url=http://127.0.0.1:8787`. `GET /health` returns the current WebSocket
protocol version (`2`).

Rooms normally follow the detected FiveM endpoint. An optional `room_key`
deliberately overrides endpoint grouping and should therefore not be shipped as
a shared default.

## WebSocket protocol v2

Connect with a WebSocket upgrade:

```text
GET /v2/connect?room=<64-lowercase-hex-sha256-room>
Upgrade: websocket
```

The client immediately sends `hello`:

```json
{
  "protocol": 2,
  "type": "hello",
  "player": 17,
  "token": "32-lowercase-hex-random-session-token",
  "modelId": "shrek",
  "modelVersion": "1.0.0",
  "packSha256": "64-lowercase-hex-model-pack-hash"
}
```

The room sends a complete snapshot to every connected client after each
presence change. A client's own player id is excluded from its snapshot:

```json
{
  "protocol": 2,
  "type": "snapshot",
  "serverTime": 1789149600000,
  "peers": [
    {
      "player": 42,
      "modelId": "goku",
      "modelVersion": "1.0.0",
      "packSha256": "...",
      "updatedAt": 1789149600000
    }
  ]
}
```

A model change or F5 toggle sends one `update` frame:

```json
{
  "protocol": 2,
  "type": "update",
  "modelId": "none",
  "modelVersion": "1.0.0",
  "packSha256": "0000000000000000000000000000000000000000000000000000000000000000"
}
```

Before a controlled disconnect, the client sends:

```json
{ "protocol": 2, "type": "leave" }
```

The Worker then closes the socket and pushes the updated snapshot. Unexpected
network disconnects are handled by the WebSocket close/error callback. The
client reconnects with exponential backoff capped at 30 seconds.

At most 256 joined sockets are accepted per room. Payloads are limited to 2048
bytes, identifiers are validated, and another live token cannot claim the same
FiveM player id. Socket attachments retain presence metadata across Durable
Object hibernation, so WebSocket presence does not require periodic SQLite
writes or alarms.

## Legacy HTTP protocol v1

`POST /v1/sync` accepts:

```json
{
  "protocol": 1,
  "room": "64-lowercase-hex-sha256-room",
  "player": 17,
  "token": "32-lowercase-hex-random-session-token",
  "modelId": "shrek",
  "modelVersion": "1.0.0",
  "packSha256": "64-lowercase-hex-model-pack-hash"
}
```

`POST /v1/leave` uses `protocol`, `room`, `player`, and `token`. WebSocket and
legacy clients in one room are included in each other's snapshots while old
clients are phased out.

Clients treat relay data only as untrusted discovery metadata. A received
`modelId` is resolved only through the local manifest and accepted only when
`modelVersion` and `packSha256` match. The relay never returns filesystem paths
and never transfers model assets.
