# SD API for Release 0.03

This document defines the authoritative SD/file backend contract for release `0.03`.

Scope:

- `ESP_AIR` owns SD storage and file operations
- GND, browser, and console commands consume SD behavior through this API
- the API provides explicit status/inhibition codes
- the API does not move file logic to Teensy and does not redesign replay or transport

Non-goals:

- no schema switching
- no multi-client browser semantics
- no broad UI redesign
- no change to Teensy fast-path behavior

## Ownership

Authoritative SD/file backend:

- `ESP_AIR/src/sd_file_api.h`
- `ESP_AIR/src/sd_file_api.cpp`

Low-level media primitives:

- `ESP_AIR/src/sd_api.h`
- `ESP_AIR/src/sd_api.cpp`

Managed log implementation primitives reused by the SD API:

- `ESP_AIR/src/log_store.h`
- `ESP_AIR/src/log_store.cpp`

Rule:

- command handlers, GND forwarding, and browser control use the SD API
- they do not invent separate mode rules
- they do not enumerate SD directories independently

## Supported Operations

Current SD API operations owned by `ESP_AIR`:

- `refreshFileList`
- `getFileListPage`
- `getOrRefreshFileListPage`
- `getFileInfo`
- `deleteFile`
- `renameFile`
- `exportLogCsv`
- `getStorageStatus`
- `mountMedia`
- `ejectMedia`

Current command-system integration:

- AIR radio command handlers call the SD API for:
  - file list
  - storage status
  - mount/eject
  - delete
  - rename
  - CSV export
- AIR console commands call the SD API for:
  - `logfiles`
  - `sdstate`
  - `sdmount`
  - `sdeject`
  - `sddelete`
  - `sdrename`
  - `csvfile`

## Result / Inhibition Codes

Shared numeric status codes are defined in `ESP_AIR/src/types_shared.h` as `telem::SdApiStatusCode`.

| Code | Name | Meaning |
|---|---|---|
| `0` | `OK` | Operation succeeded |
| `1` | `INVALID_ARGUMENT` | Bad name, bad offset/limit, wrong payload shape, or other caller error |
| `2` | `SD_NOT_READY` | Card/backend not mounted or not ready |
| `3` | `BUSY_RECORDING` | Operation inhibited because recording/logging owns the SD path |
| `4` | `BUSY_REPLAY` | Operation inhibited because replay owns the file path or replay file is open |
| `5` | `NOT_SUPPORTED` | Operation exists in contract but is not implemented for the current path |
| `6` | `INVALID_HANDLE` | File-list snapshot handle/cursor is stale or missing |
| `7` | `NO_FILES` | Listing succeeded but there are no managed files |
| `8` | `IO_ERROR` | SD/media operation failed |
| `9` | `INTERNAL_ERROR` | Unexpected internal failure |
| `10` | `NOT_FOUND` | Requested file was not found |
| `11` | `ALREADY_EXISTS` | Target file already exists or conflicts |

Rule:

- inhibited operations return an explicit code
- command/browser layers must not infer failure from missing data or timeout alone

## Mode Policy

Current enforced `0.03` policy is intentionally strict to protect logging and replay integrity.

### Idle

Allowed:

- list / refresh files
- get file info
- delete file
- rename file
- export CSV
- storage status
- mount / eject media

### Recording

Allowed:

- storage status
- mount status refresh

Inhibited:

- list / refresh files
- file info
- delete / rename
- export CSV
- eject media

Returned code:

- `BUSY_RECORDING`

### Replay

Allowed:

- storage status

Inhibited:

- list / refresh files
- file info
- delete / rename
- export CSV
- eject media

Returned code:

- `BUSY_REPLAY`

Note:

- this is stricter than “read-only list during replay may be allowed”
- the stricter rule is deliberate for the current branch stage and is preferred over ad hoc stop/retry behavior

## File-List Lifecycle

The SD API owns a single current file-list snapshot on `ESP_AIR`.

Implementation form:

- cached `LogFileInfoV1[]`
- generation-based internal handle
- paged reads from the owned snapshot

Lifecycle:

1. `refreshFileList(...)`
   - enumerates managed files once on AIR
   - stores the result in an owned snapshot
   - returns `OK` or `NO_FILES` with a new generation handle
2. `getFileListPage(handle, offset, limit, ...)`
   - reads lightweight entries from the owned snapshot
   - does not rescan the directory
3. `invalidateFileList()`
   - called after delete, rename, export, mount, and eject
   - forces the next refresh to rebuild the snapshot

Current command-path behavior:

- browser/GND requests a page using `offset` and `limit`
- `offset == 0` refreshes the snapshot first
- later pages reuse the same owned AIR snapshot
- if the handle is stale, the API returns `INVALID_HANDLE`

This satisfies the “snapshot/handle/cursor model or equivalent” requirement while keeping the current wire contract narrow.

## File Entry Data

Default file-list entry fields returned for GUI use:

- `name`
- `size_bytes`

Not included in the basic list path:

- expensive derived metadata
- timestamps from extra parsing passes
- record analysis fields

If richer metadata is needed later, it should come from `getFileInfo` or another explicit API, not by bloating the default list path.

## Browser / GND Request-Response Path

Current browser request:

```json
{
  "type": "control",
  "req_id": 7,
  "category": "file",
  "action": "refresh",
  "offset": 32,
  "limit": 32
}
```

Current GND -> AIR command payload:

- command: `CMD_GET_LOG_FILE_LIST`
- payload: `CmdGetLogFileListV1`

```c
struct CmdGetLogFileListV1 {
  uint16_t offset;
  uint16_t limit;
};
```

AIR -> GND file-list chunk payload:

```c
struct LogFileListChunkPayloadV1 {
  uint16_t offset;
  uint16_t total_files;
  uint16_t flags;
  uint16_t entries_in_chunk;
  LogFileInfoV1 entries[kLogFileChunkEntries];
};
```

Current completion flags:

- `kLogFileListFlagComplete`
- `kLogFileListFlagTruncated`

Current browser-facing `files` JSON message from GND:

- `type`
- `refresh_requested`
- `refresh_inflight`
- `complete`
- `stored_files`
- `total_files`
- `truncated`
- `page_offset`
- `page_limit`
- `has_prev`
- `has_next`
- `files[]`

Each `files[]` entry contains:

- `name`
- `size_bytes`

## Command-System Integration Notes

Serial/console:

- `logfiles` uses `sd_file_api::filesJson(...)`
- delete/rename/export use explicit SD API result codes instead of raw booleans

Radio command handlers:

- `CMD_GET_LOG_FILE_LIST` validates payload, calls SD API paging, and returns SD API status codes
- `CMD_GET_STORAGE_STATUS` returns storage state derived from the SD API
- delete/rename/export/mount/eject are backended by the SD API

GND/browser:

- GND remains a forwarding/distribution layer
- GND does not enumerate files itself
- GND now surfaces SD API rejection details to the browser using the shared status code names

## Performance Notes

Why the old GUI path was too slow:

- AIR `logfiles` was a full enumeration primitive
- earlier GUI flows rescanned too much work before the browser got a bounded result
- later pages also became slower when AIR had to walk from the start of the directory again

Current GUI-safe approach:

- AIR owns one snapshot
- browser requests bounded `32`-entry pages
- later pages are sliced from the owned snapshot instead of rescanning

Current transport note:

- `TELEM_LOG_FILE_LIST` still carries `2` file entries per ESP-NOW chunk because file-name width dominates payload size
- that is acceptable for now because page reads are bounded and explicit

## Error Handling Rules

Required behavior:

- no silent failures
- no fake success on inhibited operations
- no GUI dependence on raw console text

Current behavior:

- file operations return explicit SD API status codes
- GND/browser receives either:
  - file page data
  - or a clear code such as `busy_recording`, `busy_replay`, `sd_not_ready`, or `invalid_argument`

## Remaining Limitations

- only one current AIR-owned file-list snapshot is maintained
- browser/GND does not yet expose a dedicated file-info command
- `NOT_SUPPORTED` exists in the contract for future operations but is not heavily used yet
- `file_count` in storage status is still computed via a managed file count call, not from extra metadata cached on media mount

## Extension Rules

Future SD/file work should:

- add new file operations to the SD API first
- document mode policy before exposing them to GND or browser
- prefer bounded request/response flows
- keep GUI-specific shaping outside the SD API
