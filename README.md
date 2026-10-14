# ps4chrono

NTP time synchronization payload for PS4 (GoldHEN).

## Features
- Automatic time sync via NTP on startup and periodically
- Multiple NTP servers (default: pool.ntp.org, time.google.com, time.cloudflare.com, time.windows.com)
- Add/remove custom NTP servers via web interface (user servers get highest priority)
- Server names are cleaned up automatically (`192.168.1.1:123`, `ntp://host/` → plain name), duplicates ignored
- Configurable check interval (1h / 3h / 6h / 12h / 24h, default 6h)
- If the clock is wrong and can't be confirmed by NTP, retries every 30 seconds until NTP answers
- "Last known good time" file as fallback when NTP is unreachable
- Manual time setting via web interface
- Settings file created on first start, can be edited by hand
- On-screen notifications with custom icon
- Single instance: sending the ELF again replaces the running copy
- Restart and Stop from the web interface

## Usage
1. Disable the built-in GoldHEN time sync
2. Send `ps4chrono.elf` to your PS4 via BinLoader (port 9090):
   ```
   orbis-deploy -h <PS4_IP> -p 9090 ps4chrono.elf
   ```
3. Open web interface: `http://<PS4_IP>:8585`
4. For autostart, add to GoldHEN autoload

## How it works
- On startup, compares system time against the saved "last good time" file and the build date
- If time looks wrong (before build date or before last saved time): attempts NTP sync
- If NTP fails: sets time from saved file + 1 minute, then keeps retrying NTP every 30 seconds
- If time looks OK: still syncs with NTP to correct small drifts (>2 sec)
- Saves current time to file after every successful check
- The check interval is counted by a monotonic clock, so changing the system time doesn't shift it
- Custom DNS resolver (UDP queries to 8.8.8.8 / 1.1.1.1) with hardcoded fallback IPs
- Web server runs in its own thread: the page opens while a sync is in progress and recovers after rest mode

## Notifications
Each one is shown once per event (retries don't repeat them):
- `ps4chrono: time synced` — NTP sync successful
- `ps4chrono: date error` — system date is wrong, attempting fix
- `ps4chrono: offline time set` — NTP unreachable, time set from saved file
- `ps4chrono: sync failed` — NTP unreachable, no saved time available
- `ps4chrono: set manually` — time set via web interface

## Data files
All stored in `/data/ps4chrono/`:
- `config.dat` — NTP servers and check interval (created on first start)
- `lastgood.dat` — last known good timestamp
- `ps4chrono.log` — log file

## Editing config.dat by hand
```
interval=21600
server=192.168.1.1
server=pool.ntp.org
server=time.google.com
```
- `interval` — seconds between checks, minimum 60
- `server` — up to 8 lines, the first one is checked first
- Lines starting with `#` are ignored

After editing, press **Restart** on the web page (or send the ELF again).

## Web interface (port 8585)
- View current system time and last good time
- Add/remove NTP servers
- Change check interval
- Set time manually (UTC format: YYYY-MM-DD HH:MM:SS)
- Sync now
- Restart — re-read `config.dat` and sync
- Stop the service (`/quit`)

## Building
Requires [ps4-payload-dev/sdk](https://github.com/john-tornblom/ps4-payload-SDK):
```
export PS4_PAYLOAD_SDK=/path/to/sdk
make
```

PC test build (no PS4 needed, data in `/tmp/ps4chrono_test`, clock is not changed):
```
g++ -std=c++17 -DHOST_TEST -o chrono_host main.cpp -lpthread
```

## Version
0.2.2

## License
GPLv3

## Authors
Created by SergioPoverony and Mr.Claude
