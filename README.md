# ps4chrono

NTP time synchronization payload for PS4 (GoldHEN).

## Features
- Automatic time sync via NTP on startup and periodically
- Multiple NTP servers (default: pool.ntp.org, time.google.com, time.cloudflare.com, time.windows.com)
- Add/remove custom NTP servers via web interface (user servers get highest priority)
- Duplicate server protection
- Configurable check interval (5 min / 1h / 3h / 6h / 12h / 24h)
- "Last known good time" file as fallback when NTP is unreachable
- Manual time setting via web interface
- Custom notification icon
- On-screen notifications with custom icon
- Single instance (new launch gracefully replaces old one)
- Stop via web interface (`/quit`)

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
- If NTP fails: sets time from saved file + 1 minute
- If time looks OK: still syncs with NTP to correct small drifts (>2 sec)
- Saves current time to file after every successful check
- Settings (servers, interval) persist across restarts
- Custom DNS resolver (UDP queries to 8.8.8.8 / 1.1.1.1) with hardcoded fallback IPs

## Notifications
- `ps4chrono: time synced` — NTP sync successful
- `ps4chrono: date error` — system date is wrong, attempting fix
- `ps4chrono: offline time set` — NTP unreachable, time set from saved file
- `ps4chrono: sync failed` — NTP unreachable, no saved time available
- `ps4chrono: set manually` — time set via web interface

## Data files
All stored in `/data/ps4chrono/`:
- `lastgood.dat` — last known good timestamp
- `config.dat` — NTP servers and check interval
- `ps4chrono.log` — log file

## Web interface (port 8585)
- View current system time and last good time
- Add/remove NTP servers
- Change check interval
- Set time manually (UTC format: YYYY-MM-DD HH:MM:SS)
- Trigger immediate sync
- Stop the service

## Building
Requires [ps4-payload-dev/sdk](https://github.com/john-tornblom/ps4-payload-SDK):
```
export PS4_PAYLOAD_SDK=/path/to/sdk
make
```

## Version
0.1.9

## License
GPLv3

## Authors
Created by SergioPoverony and Mr.Claude
