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
- Reference project for proven PS4 techniques: https://github.com/sergiopoverony/ps4torrent

## Current state: v0.2.2
- NTP sync: queries up to 8 servers, stops at first success
- Custom UDP DNS resolver (8.8.8.8, 1.1.1.1) + hardcoded fallback IPs
- Last good time file: /data/ps4chrono/lastgood.dat
- Config file: /data/ps4chrono/config.dat — created on first start with comments, editable by hand
- Config is cleaned on load (port/scheme removed, bad names and duplicates dropped) and rewritten if anything was fixed
- Web interface on port 8585: status, add/remove servers, interval, manual time, Sync now, Restart, Stop
- Restart = re-read config.dat + sync (a payload can't re-exec itself)
- User-added servers go to top of list; names cleaned (`192.168.1.1:123` -> `192.168.1.1`), invalid rejected
- Custom notification icon (embedded in ELF, served via HTTP at /notify.png)
- Notifications shown once per event, retries don't repeat them
- Default check interval: 6 hours; counted by CLOCK_MONOTONIC (setting the clock doesn't fire/delay it)
- Retry every 30 sec while clock is bad or was only set from lastgood.dat (network is often not up right after boot)
- Build date used as minimum valid time threshold

## Single instance (changed in v0.2.2)
- Same as ps4torrent: the web port is the lock. No SO_REUSEPORT.
- New copy: bind fails with EADDRINUSE -> writes quit.flag every 0.5 s and retries bind, up to 60 s.
- Running copy checks quit.flag every 0.2 s and between NTP servers, exits, its web thread closes the port.
- Old fcntl lock removed: on PS4 it did not block a second copy, and the new copy deleted quit.flag
  immediately, so old copies never exited (seen in tester logs: several copies doing periodic checks).

## Web server (changed in v0.2.2)
- Own thread (select + accept), one short detached thread per request, max 8 at once
- Client sockets set blocking with 5 s timeouts (on FreeBSD accepted sockets inherit O_NONBLOCK —
  this lost requests in v0.2.1)
- Listening socket recreated if accept/select keep failing (happens after rest mode, see ps4torrent)
- Shared state: g_cfg_mx (servers, interval, config file), g_log_mx (log)

## Testing
- HOST_TEST build: `g++ -std=c++17 -DHOST_TEST -o chrono_host main.cpp -lpthread`
  data in /tmp/ps4chrono_test, notifications go to the log, settimeofday is skipped
- Fake NTP for tests: small Python UDP server on 127.0.0.2:123 answering real time + offset
- v0.2.2 tested on PC: config auto-create, name cleanup, Restart with hand-edited config,
  30 s retry with once-only notifications, takeover of running copy, /quit, 30 parallel requests during sync
- Not tested on PS4 yet: rest mode recovery, takeover of v0.2.1 copies

## Tester log findings (v0.1.x–v0.2.0)
- Multiple copies alive at once (fixed in v0.2.2)
- `192.168.1.1:123` typed as server -> DNS failure (fixed: name cleanup)
- At cold boot the router answers NTP only after a few minutes (fixed: 30 s retry)
- pool.ntp.org via 8.8.8.8 resolves to 192.168.1.1: tester's router intercepts DNS and serves NTP itself
- lastgood.dat = 0 after reboot: tester deleted it on purpose (not a bug)

## Not yet done
- Python fake NTP server as a committed test tool
- Phase 2/3 polish

## Key decisions
- Timezone: we sync UTC only, PS4 displays local time from its own settings
- Port 8585 for web (8787 is ps4torrent, no conflict)
- GoldHEN's built-in time sync will be disabled, ps4chrono runs alone
- PKG installation: closed topic, do not suggest
- sceHttp does not work from payload; NTP uses raw UDP sockets
- DNS via custom UDP resolver (getaddrinfo fails from payload)
- Notification icon served via HTTP (file paths don't work for PS4 notifications)
- Safe file writes (fsync+rename) discussed and NOT added — user didn't ask

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
- Deliver elf + src zip + README.md + HANDOFF.md ALL TOGETHER, never piecemeal
- Do not add features the user didn't ask for; don't start work while user is only discussing
- Git author: "Claude <noreply@anthropic.com>", no session links in commits/PRs
- Push to GitHub only when the user says so
