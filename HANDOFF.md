# ps4chrono — Handoff Document

## Project
NTP time synchronization payload for PS4 running GoldHEN. Replaces GoldHEN's built-in time sync with configurable servers, intervals, and a web interface.

## Repository
https://github.com/sergiopoverony/ps4chrono

## Environment
- PS4 firmware 13.52, GoldHEN v2.4b18
- BinLoader port 9090 for payload delivery
- SDK: ps4-payload-dev/sdk (john-tornblom), commit 573b4a0
- SDK install: /tmp/sdk_new (see PAYLOAD_TOOLKIT.md section 3)

## Current state: v0.1.9 (Phase 1)
- NTP sync working: queries up to 8 servers, stops at first success
- Custom UDP DNS resolver (8.8.8.8, 1.1.1.1) + hardcoded fallback IPs
- Last good time file: /data/ps4chrono/lastgood.dat
- Config file: /data/ps4chrono/config.dat (servers + interval, persists)
- Web interface on port 8585: status, add/remove servers, set interval, manual time, force sync, stop
- User-added servers go to top of list (highest priority, checked first)
- Duplicate server protection (case-insensitive)
- Custom notification icon (embedded in ELF, served via HTTP at /notify.png)
- Notifications: "time synced", "date error", "offline time set", "sync failed", "set manually"
- Non-blocking web: sync shows "please wait" page instead of 302 redirect
- Single instance via flock + quit flag
- Stop via /quit endpoint
- Default check interval: 5 min (for testing, user will change later)
- Default NTP servers: pool.ntp.org, time.google.com, time.cloudflare.com, time.windows.com
- Build date used as minimum valid time threshold (not hardcoded year)

## Not yet done
- HOST_TEST build (g++ on PC with mocked syscalls)
- Python fake NTP server for testing
- Phase 2/3 polish: better error handling, web interface improvements

## Key decisions
- Timezone: we sync UTC only, PS4 displays local time from its own settings
- Port 8585 for web (8787 taken by ps4torrent)
- GoldHEN's built-in time sync will be disabled, ps4chrono runs alone
- PKG installation: closed topic, do not suggest
- sceHttp does not work from payload; NTP uses raw UDP sockets
- DNS via custom UDP resolver (getaddrinfo fails from payload)
- Notification icon served via HTTP (file paths don't work for PS4 notifications)

## Files
- main.cpp — single-file implementation
- icon_data.h — notification icon PNG as C array (generated from icon.png)
- icon.png — source notification icon (64x64 PNG)
- Makefile — standard PS4 payload build
- Data dir on PS4: /data/ps4chrono/

## User rules
- Russian language, non-programmer, call assistant "Mr.Claude"
- Signature: "Created by SergioPoverony and Mr.Claude"
- License: GPLv3, both authors
- User tests on console, sends back log
- Increment APP_VERSION on every rebuild
- Do not build until user gives command
- Git author: "Claude <noreply@anthropic.com>", no session links in commits/PRs
