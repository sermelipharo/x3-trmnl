# BYOS compatibility matrix

The firmware implements the BYOD device-side contract to the
intersection of headers and response fields every maintained BYOS
speaks. This doc records what we've actually verified on hardware,
and the variance we found in the wild so others don't have to
rediscover it.

The BYOD contract itself is at
<https://docs.trmnl.com/go/diy/byod>. The reference client is
[usetrmnl/firmware](https://github.com/usetrmnl/firmware).

## Tested

| Server | Repo | Result |
|---|---|---|
| **LaraPaper** | [usetrmnl/larapaper](https://github.com/usetrmnl/larapaper) | End-to-end OK. Setup + display + render confirmed on a self-hosted instance. |
| **BYOS Next.js** | [usetrmnl/byos_next](https://github.com/usetrmnl/byos_next) | End-to-end OK. Auto-provisions unknown MACs when the `Model` header is sent. Device screen size (`screen_width` / `screen_height`) is stored per-device in Postgres and reflected in the bitmap URL, so a 792×528 panel is served 792×528 BMPs. |

## Untested but protocol-compatible

We shaped the client around the union of behaviours documented across
these servers. Any deviation would be a bug in the server or a gap in
our implementation — file an issue.

- **Terminus** — [usetrmnl/terminus](https://github.com/usetrmnl/terminus)
  (Ruby, auto-provisions)
- **Inker** — [usetrmnl/inker](https://github.com/usetrmnl/inker)
  (Ruby, auto-provisions)
- **BYOS FastAPI** — [usetrmnl/byos_fastapi](https://github.com/usetrmnl/byos_fastapi)
  (Python, auto-provisions)
- **BYOS Django** — [usetrmnl/byos_django](https://github.com/usetrmnl/byos_django)
  (Python, requires MAC pre-seeded in DB)
- **BYOS Phoenix** — [usetrmnl/byos_phoenix](https://github.com/usetrmnl/byos_phoenix)
  (Elixir, requires MAC pre-seeded in DB)
- **usetrmnl.com cloud** — the reference backend

## Known variance across servers

Informing the client's defensive parsing.

### Request side

- **`Model` header is spec-optional but sometimes mandatory.**
  BYOS Next.js returns HTTP 200 with JSON `status: 400, message: "Model
  header is required"` on `/api/setup` when it's missing. Terminus /
  LaraPaper / Inker ignore it. Safer to always send.
- **`Refresh-Rate` should reflect the last server-supplied interval**,
  not a static default. We persist `lastRefreshRate` in NVS and send
  it back.
- **`Access-Token` header capitalization.** Laravel (LaraPaper) is
  forgiving (normalizes any case). The spec and the reference firmware
  use `Access-Token`; we match. A server that does strict
  case-sensitive header matching would reject a lowercase
  `access-token` which some older firmwares send.

### Response side

- **`status` field — int vs int-but-pretends-to-be-HTTP-status.**
  Some servers return `status: 0` on success (firmware convention),
  others return `status: 200`. The client accepts both as OK, `202`
  as "awaiting link", anything else as error.
- **Optional fields.** Django / Phoenix / Next.js may omit
  `reset_firmware`, `update_firmware`, `firmware_url` entirely. The
  client defaults them to `false`/`null` so absence is indistinguishable
  from `false`/`null` being set.
- **`special_function`** is not in the public BYOD contract even though
  some forks surface it; the client intentionally does not parse it.
- **`/api/setup` status convention.** Either HTTP 200 with JSON
  `status: 200` + populated `api_key`/`friendly_id` (success) or
  `status: 404` (unknown MAC). Some servers use a real HTTP 404
  instead of the JSON field for unknown MACs — the client parses the
  body on both `200` and `404` responses.

### Image format

- **BMP3 1-bpp is the primary format** most BYOS servers serve
  (Terminus, LaraPaper, Inker, BYOS Next.js, Phoenix). PNG is an
  option on newer releases. Our decoder sniffs the magic bytes
  (`BM` vs `\x89PNG`) and dispatches.
- Upstream TRMNL panels are 800×480; our X3 is 792×528. The decoder
  accepts oversized BMPs and clips at write time rather than
  rejecting. BYOS Next.js honors per-device `screen_width` /
  `screen_height`; the others return a fixed 800×480 regardless.

## Auto-provisioning expectations

Terminus / LaraPaper / Inker / BYOS Next.js / BYOS FastAPI will
create a device record on the first `/api/setup` from an unknown MAC.
`friendly_id` is the 6-char identifier the user sees; `api_key` is
persisted by the device in NVS.

BYOS Django / BYOS Phoenix require manual device seeding in their
admin UI before `/api/setup` will succeed. In that case the firmware
sees `status: 404`, renders a "Setup failed" splash, and exponentially
backs off — 1800 s × 2^n, capped at 24 h — so an unknown device
doesn't hammer the server.

## Reporting incompatibility

If you hit a BYOS server this firmware doesn't work against, please
open an issue with:

- Server repo + version
- Full `/api/setup` and `/api/display` request + response (redact
  `api_key` / `friendly_id`)
- Serial log from the device around the failure (build with
  `-DENABLE_SERIAL_LOG`, catch output with `scripts/flash.sh` or
  `pio device monitor`)
