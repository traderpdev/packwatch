# PackWatch project todo

Record pack-station video from 16 Reolink RLC-520A cameras on a Beelink ME Pro running FreeBSD. One C binary, `packwatch`, links the FFmpeg libraries and remuxes the RTSP substream to MPEG-TS. It is run once per camera, each time with that camera's config. rc.d and `daemon(8)` supervise those processes. Returning a clip is a separate process, and that process is not designed yet.

Audio is out. Every camera records its fluent substream at 640×480, 10 fps, 256 kbps, copied as-is. Video files live on a ZFS mirror of two 2TB NVMe drives. Cameras and files are segregated by directories and filenames. There is no sqlite database and no sidecar text file for timing.

## Locked

- Host: Beelink ME Pro, Intel N150 (4 cores), 16GB LPDDR5 soldered, 1TB SSD for the OS, 3× M.2 PCIe 3.0, 2× HDD bays (unused by the recorder), 5GbE + 2.5GbE, Wi-Fi 6, Bluetooth 5.4.
- Video pool: two 2TB NVMe drives in a ZFS mirror, in the two M.2 slots that are not the OS disk. Usable space is about 1.8 TB. The HDD bays are not the camera store.
- Cameras: 16× Reolink RLC-520A, 5MP wired outdoor dome, PoE (802.3af), H.264, one camera per pack station inside a 3PL warehouse.
- When video exists: from a start event for that camera until a stop event for that camera. Packing is not 24/7/365. A camera with no open session has no `packwatch` process and writes nothing. In practice a session ends within about 8 hours.
- One file per camera per session. Each camera has one directory. There are no year, month, or day folders. A US shift crosses UTC midnight often, and the file stays one object with one start name.
- Expect the low tens of thousands of files in each camera directory. `mkdir` starts a MicroZAP. Each camera directory is created with one empty placeholder that stays there indefinitely: `PLACEHOLDER-this-directory-is-a-fatzap-do-not-remove` (51 bytes). A name of 50 bytes or more upgrades that directory to a FatZAP, and the placeholder keeps the reason visible. Leave `vfs.zfs.zap_micro_max_size` alone. Exact name lookup stays a hash.
- Audio: do not store it. The camera has a built-in microphone. The binary copies the video stream and drops the audio.
- Picture: Reolink fluent substream over RTSP. 640×480, 10 fps, 256 kbps, H.264. Stream copy into MPEG-TS. No scale and no main stream (2560×1920).
- Recorder: one C binary, built once against the FFmpeg libraries on this frozen host (`libavformat` and what it requires). Sixteen processes, one config file each. The process lifetime is the recording. It writes on start and exits on `SIGTERM`. No Go process, and no parent that execs the `ffmpeg` command.
- Path, created at the final location and never renamed, moved, or copied by the recorder:
  `tank/video/<camera-id>/YYYYMMDDTHHMMSSZ.ts`
  The filename is the UTC start. The end is unknown while the file is open, so the name has no end time.
- Time lookup, with no database: list that one camera directory, ignore the placeholder, and take the latest `YYYYMMDDTHHMMSSZ.ts` whose start is still at or before T. That file is the one that covers T. It may run past T until the stop event, including across UTC midnight. The pictures inside may be confirmed later by reading that one file's duration. No recording name at or before T means that camera was not recording.
- Clock: the host runs UTC and is NTP-synced before a recording starts. Cameras take NTP from the host on the camera NIC. The filename is the host clock at the moment the process opens the file. Packet timestamps use that same wall clock.
- A crash or a stop leaves a playable MPEG-TS, ending at the last intact packet. The recorder does not rewrite it.
- If the process dies while the session is still open, the old file stays where it is. `daemon(8) --restart` runs the binary again, and that new process opens a new file with a new start time. A stop event signals the supervisor pid, so `daemon` does not treat the exit as a crash.
- After a reboot the disk does not say which sessions were still open, because a finished file and a killed file look the same. The outside system sends start again for any camera that should still be recording.
- Retrieval is a different process. It is undefined. No query API in this phase. The directory tree is what that process will read.
- Networks: the host is dual-homed and does not forward between the NICs.
  - One NIC faces the upstream business internal network. Default route, SSH, and the start/stop API live here.
  - The other NIC faces a local private subnet that contains only the cameras. `packwatch` pulls RTSP there. The host serves NTP there. That subnet has no default route to the business network.
- This is a closed box on a private network for this one job. Install FreeBSD 15.1-RELEASE amd64, apply the updates and packages once, record the versions, and do not upgrade the OS or the FFmpeg libraries afterward. GhostBSD, TrueNAS, and OPNsense are not the host OS.
- NIC drivers: 2.5GbE is Intel I226-V on the in-tree `igc` driver. 5GbE is Realtek RTL8126, brought up on 15.1 with the `realtek-re-kmod` package. The in-tree `rge` driver is on FreeBSD 16-CURRENT, so it is a later option.
- Jack plan to confirm on the bench: `igc` (2.5GbE) on the camera subnet, RTL8126 (5GbE) on the business network. Peak camera traffic is 4.1 Mbps, and only when all 16 cameras are recording.

## Open decisions

- [ ] Retention, by UTC start in the filename (proposal: delete a camera file whose start is older than 90 days).
- [ ] Where the start and stop events come from (scanner, WMS, or a small local client). A start runs this camera's supervised process. A stop signals that supervisor. The caller keeps the order number and the timestamps.
- [ ] Soft router on the host with `pf`, or a later bhyve OPNsense guest. The camera NIC stays unrouted either way. Proposal: `pf` on the host.
- [ ] Confirm the jack plan above against the real cables. Swap the roles if the 5GbE driver is the unstable one. The camera subnet still gets its own NIC.
- [ ] Fault policy when a session is still open at 8 hours. Proposal: keep writing, raise a fault, and wait for the stop event. Do not split the file.

## 0. Bench the box and one camera

- [ ] Record the ME Pro revision, `pciconf -lv` for both NICs, the three M.2 link widths, which slot holds the factory 1TB SSD, and the two 2TB NVMe ids.
- [ ] Boot a FreeBSD 15.1 installer USB once and confirm the I226-V appears as `igc0` and all three NVMe devices are visible.
- [ ] Install `realtek-re-kmod`, load it, and confirm the RTL8126 links at 5GbE to the business switch. Run iperf3 for several hours in both directions.
- [ ] Cable one NIC to the business internal network and the other to the camera subnet. Confirm each interface reaches only its own side, and that a camera address is not reachable from a business client.
- [ ] Put one RLC-520A on the camera subnet via PoE. Enable RTSP in the camera UI. Set a password with no special characters. Turn off Reolink P2P / UID cloud.
- [ ] In the camera UI, set the fluent stream to 640×480, 10 fps, max bitrate 256 kbps, H.264.
- [ ] Probe the substream and confirm `ffprobe` reports 640×480, about 10 fps, H.264:
  - `rtsp://USER:PASS@CAMERA:554/h264Preview_01_sub`
  - `rtsp://USER:PASS@CAMERA:554/Preview_01_sub`
- [ ] Record one minute with the `ffmpeg` command below and confirm the `.ts` stays near 256 kbps (about 115 MB per camera-hour), contains no audio track, and still plays after the process is killed mid-write. The C binary has to match that result.
- [ ] Point the camera at the host NTP server on the camera NIC and measure clock skew. The filename clock and the timestamps inside the file are the host's UTC.
- [ ] Soak one RTSP-to-MPEG-TS remux for a multi-hour run.

Behavior the binary must match:

```
ffmpeg -hide_banner -loglevel warning \
  -rtsp_transport tcp \
  -use_wallclock_as_timestamps 1 \
  -i rtsp://USER:PASS@CAMERA:554/h264Preview_01_sub \
  -map 0:v:0 -an \
  -c copy \
  -f mpegts \
  tank/video/<camera-id>/YYYYMMDDTHHMMSSZ.ts
```

## 1. FreeBSD installation

- [ ] Download the FreeBSD 15.1-RELEASE amd64 installer image and check the signed checksums from freebsd.org.
- [ ] Install onto the factory 1TB SSD as a ZFS root. Leave both 2TB NVMe drives untouched by the installer. Hostname, UTC clock, and a wheel account with SSH keys.
- [ ] Apply `freebsd-update` once. Install `pkg`, then `ffmpeg`, `ca_root_nss`, `smartmontools`, `tmux`, and `realtek-re-kmod`. Record `freebsd-version` and `pkg info ffmpeg` in the install doc. Do not upgrade them again.
- [ ] Load the Realtek module from `loader.conf`. Configure the business NIC with its upstream address and the default route. Configure the camera NIC with a static address on the camera subnet and no default route.
- [ ] Enable `ntpd` as the clock source for the host. The cameras use the host on the camera NIC. `packwatch` exits without opening a file while the host clock is unsynced.
- [ ] Enable `zfs`, periodic scrubs, and `smartd` for the OS SSD and both NVMe drives.
- [ ] Leave Wi-Fi and Bluetooth off.
- [ ] Write the install steps that were actually run into `docs/install-freebsd-15.1.md` so the next box can be rebuilt without this session.
- [ ] Snapshot the fresh root dataset before any daemon work (`zfs snapshot`).

## 2. Disks and the two networks

- [ ] Create the video pool as a mirror: `zpool create` with `ashift=12` on the two 2TB NVMe drives. Proposed name: `tank`.
- [ ] Create `tank/video`. Under it, one directory per camera id and nothing deeper. On each new camera directory, create an empty `PLACEHOLDER-this-directory-is-a-fatzap-do-not-remove` and leave it there. `packwatch` creates the `.ts` file in that directory when its process starts.
- [ ] Set a quota on `tank/video` so a full recording pool cannot fill `tank` to the point ZFS stalls.
- [ ] On `tank/video`: `recordsize=1M`, `compression=lz4`, `atime=off`, `primarycache=metadata`, `checksum=on`, `sync=standard`, `dnodesize=legacy`. The metadata cache has to hold dnodes for the low tens of thousands of files per camera. The recordings themselves stay out of the ARC.
- [ ] Camera subnet: DHCP reservations or static addresses for 16 cameras, PoE switch, no route upstream. The host is their NTP server and their only RTSP client.
- [ ] `pf` on the host: no forwarding between the NICs. The start/stop API and SSH listen on the business address only. The host may open RTSP and NTP to the camera subnet. Cameras may not open sessions to the business network.
- [ ] Size a PoE switch for 16× 802.3af domes plus spare ports (a 24-port PoE switch). Budget switch power against the camera draw.
- [ ] Leave the two HDD bays empty until the media-library or private-cloud phase needs them.

Capacity at 256 kbps, video only: about 115 MB per camera-hour. The mirror holds about 17,000 camera-hours (about 1.8 TB usable). Sixteen cameras recording through an 8-hour shift is about 15 GB. Five such shifts a week is about 74 GB, on the order of six months on this mirror, and longer whenever a camera is idle. Peak wire rate on the camera NIC is 4.1 Mbps, and only while every camera is recording.

## 3. Camera fleet

- [ ] Name the 16 cameras. The directory name is that id. Each camera has its own config file, mode 0600, outside the video tree: id, IP, RTSP path, password. The video directory is not a config store.
- [ ] Common firmware level on all 16. Export and file the working camera setup (stream, ports, NTP, P2P off).
- [ ] Fluent stream on every camera: 640×480, 10 fps, 256 kbps, H.264. Main stream left unused by PackWatch.
- [ ] Confirm each camera answers from the camera NIC and does not have a route onto the business network.
- [ ] Label cables and switch ports to match the camera ids.
- [ ] IR check at the real pack-station mount. The 640×480 picture still has to show the pack surface.

## 4. Recorder binary

`packwatch` is a C program linked to `libavformat`. One binary is installed. A running copy is given one camera config and does only that camera's remux. rc.d starts a `daemon(8) --restart` supervisor per camera that should be recording, using `--supervisor-pidfile` so a stop does not look like a crash.

- [ ] On start, refuse to open a file if NTP is unsynced or the probed stream is not 640×480 H.264. Open `tank/video/<camera-id>/YYYYMMDDTHHMMSSZ.ts` at that final path.
- [ ] RTSP over TCP. Copy the video stream, including the H.264 parameter sets. Drop audio. Stamp packets from the host wall clock.
- [ ] Set a read timeout and an interrupt callback. A stalled camera must make the process exit, so `daemon` can start it again. A process that blocks forever is invisible to the supervisor.
- [ ] `SIGTERM` sets a flag. The interrupt callback returns, the process finishes the current packet, and it exits. Do not call into the FFmpeg libraries from the signal handler.
- [ ] Leave the `.ts` where it was created. Do not rename, move, or copy it.
- [ ] A crash or a stall exits. `daemon --restart` runs the binary again after its delay, and the new process creates a new start-named file. The previous file stays.
- [ ] A second start while that camera's supervisor is already running does not open another file.
- [ ] rc.d script reads the camera list from `rc.conf`, starts one supervisor per recording camera after `zfs`, `ntpd`, and both NICs, and stops a camera by signaling the supervisor pid. The service account can create files under `tank/video`.
- [ ] Soak: start and stop several cameras, run two at once, kill one `packwatch` and see a second file appear beside the first, confirm the first file still plays, and confirm a supervisor stop leaves the camera idle. Pass when an idle camera writes nothing and `ffprobe` shows no audio track.
- [ ] Disk-full behavior: the writing process exits with an error. `daemon` will restart it, so the supervisor for that camera is stopped when the pool is full, and the health fault stays raised.

## 5. Finding a time

No query is implemented in this phase. The layout is the index.

- [ ] Document the rule: one directory per camera, ignore the placeholder, take the latest `YYYYMMDDTHHMMSSZ.ts` at or before T. The file may contain later video through the stop event, including past UTC midnight.
- [ ] Confirm with a fixture: two sessions on one camera, a session whose wall time crosses UTC midnight and still has one file, a camera with no file, and a session whose process was restarted (two start times, two files, neither one renamed). The placeholder is present and ignored.

## 6. Retrieval

Retrieval is a separate process. It is not part of `packwatch`, and it is not specified yet.

- [ ] A later process reads the `.ts` tree. It can remux a chosen file, or a time range inside one file, into a playable MP4 when someone asks.
- [ ] Do not add a media endpoint, a clip assembler, or a second daemon until that process is defined.

## 7. Operate it

- [ ] Log `packwatch` and `daemon` to syslog: starts, stops, restarts, stalls, NTP refusals, disk full. Logs are not stored in the video tree.
- [ ] Alert when a recording process stays silent, a session passes 8 hours, the mirror is past 80%, NTP is unsynced, or a scrub finds an error.
- [ ] Retention deletes recording files whose UTC start, taken from the filename, is older than the chosen keep. It never removes the placeholder.
- [ ] Restore drill: reinstall FreeBSD onto the 1TB SSD from the install doc, import `tank`, and confirm a known camera-and-time resolves to the same `.ts` by the path rule.
- [ ] Admin notes for "camera went dark during a session", "restart opened a second file", "stop never arrived", "clock stepped", and "one NVMe replaced, mirror resilvered". Do not upgrade the frozen OS or FFmpeg libraries as a repair step.

## 8. Same host, after the recorder is stable

These roles share the box. They wait until a start event produces one growing `.ts` per camera and a stop event leaves that file playable and unmoved.

- [ ] 4K media library on its own dataset and quota, using the HDD bays if it needs bulk space. It does not use `tank/video`. Playback stays separate from the sixteen `packwatch` processes.
- [ ] Private cloud (file sync) on its own dataset and quota. User files never land on `tank/video`.
- [ ] Soft router work, if it is still wanted, must keep the camera NIC unrouted. Camera traffic stays on the private subnet. The business NIC keeps the default route.
- [ ] Wi-Fi 6 stays off unless a handheld admin path is required.

## Done means

- FreeBSD 15.1 is installed on the 1TB SSD, `tank` is a mirror of the two 2TB NVMe drives, and the install doc can rebuild the host.
- The business NIC and the camera NIC do not forward into each other. Cameras are reachable only from the host.
- A start event for a camera runs one `packwatch` with that camera's config. It pulls RTSP and writes one MPEG-TS at its final path until the supervisor is stopped. Each camera has its own directory. The filename is the UTC start and nothing else.
- The file is 640×480, 10 fps, 256 kbps, video only, and it still plays if the process is killed. Nothing renames, moves, or copies it.
- A time on a camera resolves to a file by listing that camera's one directory. No database, no sidecar, and no day folder.
- A full mirror, a dead camera mid-session, a missing stop, a killed `packwatch`, and a `daemon` restart each have a tested behavior.
- Retrieval is not part of this definition of done.
