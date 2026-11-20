# QlaudeGC

**QlaudeGC** is a custom build of [QGroundControl](https://github.com/mavlink/qgroundcontrol). It adds three things to QGC:

- a **built-in MAVLink router**, so other programs can share QGC's vehicle connection;
- **ROS bag recording** on the drone's companion computer, started and stopped from the toolbar;
- a **Claude chat sidebar** that can see the live vehicle state and, after you approve each change, adjust parameters, app settings and comm links.

All of the custom code is in [`custom/`](custom/) and uses QGC's standard custom-build plugin mechanism (`QGCCorePlugin`). The rest of the tree is upstream QGC, unchanged, so updating to a newer QGC is a normal merge from `mavlink/qgroundcontrol`.

Detailed documentation: [`custom/README.md`](custom/README.md).

---

## Features

### Built-in MAVLink router

- **Router compiled into QGC.** It runs on its own thread, so UI stalls never delay traffic.
- **mavlink-router compatible.** It reads mavlink-router's `main.conf` format: `[UdpEndpoint]` (Server/Normal), `[TcpEndpoint]`, `[UartEndpoint]`, `[General] TcpServerPort`, and `AllowMsgIdOut` / `BlockMsgIdOut` message filters. Existing mavlink-router configs can be imported as they are.
- **Same routing rules as mavlink-router.**
  - Messages with a target system go only to endpoints that have seen that system.
  - Broadcasts go everywhere except back to the source.
  - Frames are forwarded byte-for-byte, so signed frames pass through.
- **QGC keeps its own links.** Every QGC link (e.g. the drone on UDP 14550, telemetry radios) is bridged into the router. Out of the box, MAVSDK can connect on `udp://:14540` and other tools on the TCP server at port 5760 (mavproxy, a second QGC, ...).
- **Multiple vehicles.**
  - Per-endpoint vehicle filters (`AllowSrcSysOut` / `BlockSrcSysOut`) restrict an endpoint to chosen system ids.
  - A **MAVSDK per vehicle** button adds one endpoint per connected vehicle at UDP `14540 + sysid`.
  - Several MAVSDK instances can share GCS id 245.
- **Graphical config editor.** It covers endpoints, ports, serial settings, message filters and the TCP server port. It also has **Import...**, which validates a file and makes it the active config, and **Export active config...**.
- **Toolbar indicator.** It shows traffic, the number of active endpoints and config errors at a glance. Click it for per-endpoint rx/tx rates, the system ids heard on each endpoint, CRC errors, the enable switch and the log.
- **`tar_router_cli`.** The same router engine as a standalone console program. `--check file.conf` validates a config.

### ROS bag recording

- **One click from the toolbar.** The **Bag** indicator starts and stops `ros2 bag record` on the companion computer (e.g. the Jetson) over ssh with your existing key. Nothing needs to be installed on the drone.
- **Safe stop.** Stop sends SIGINT and waits, so the bag is closed cleanly with `metadata.yaml`.
- **Survives drops.** The recorder runs detached, so losing the link or closing QGC doesn't end it. A restarted QGC picks up a recording that is already running.
- **Live status.** A red blinking dot and the elapsed time while recording; the drop-down shows the bag path, size and free space on the companion. A recorder that dies on its own is reported with its last log lines.
- **Sensible defaults, all editable.** Records all topics except the raw camera and Point-LIO's high-rate odometry, on `ROS_DOMAIN_ID` 1, into `~/bags`. Works on Humble and Jazzy.
- **Claude can use it too.** The sidebar has `rosbag_status`, `rosbag_start` and `rosbag_stop`; start and stop need your approval.

### Claude chat sidebar

- **Chat panel inside QGC.** Click the chat-bubble icon in the toolbar to open it. It runs the locally installed [Claude Code](https://claude.com/claude-code) CLI. Conversations persist and can be resumed.
- **Live context.** Each message carries a snapshot of QGC's state, which can be previewed or turned off:
  - every vehicle's mode, armed and flying state, prearm errors, GPS, batteries, altitude, position and recent messages;
  - router status and the log tail;
  - ROS bag recorder status.
- **Built-in MCP server (`qgc`).** It listens on localhost and needs a per-launch token. It gives Claude tools to:
  - list vehicles, and search and read parameters with full metadata (units, ranges, defaults, enums, bitmasks, reboot-required);
  - **set parameters**, after checking type and range;
  - search, read and change **QGC app settings** (video, units, maps, fly/plan view, auto-connect, MAVLink, RTK, ADS-B, joystick, logging, ...);
  - create, edit, connect, disconnect and delete **comm links**;
  - inspect and call QGC's live object tree (`qgc_inspect`, `qgc_set_property`, `qgc_call`) for anything else.
- **Every change needs your approval.** An approval card shows old → new values, Claude's reason and warnings (vehicle armed or flying, reboot needed). Nothing is sent until you click **Approve**. Reject, Stop or a 10-minute timeout leaves the vehicle unchanged.
- **Safety limits.** Arming, flight-mode changes and other vehicle commands are not exposed. Shell tools are always blocked. Access to files is read-only by default, with an optional "can edit files" mode.
- **Quality of life.** Up/Down recalls messages you sent, Claude Code slash commands work (`/goal`, `/cost`, ...), and you can pick the model and working folder.

---

## Building (Windows)

Requirements:

- Visual Studio 2022 Build Tools (C++)
- Qt 6.11.1 `msvc2022_64` in `C:\Qt` (e.g. via aqtinstall)
- `cmake` and `ninja` from pip

Commands:

```bat
tar-build.bat            :: configure + build (RelWithDebInfo)
tar-build.bat build      :: incremental build
tar-target.bat <target>  :: build a single target, e.g. tar_router_cli
```

Output: `QlaudeGC.exe` and `tar_router_cli.exe` under `build\`.

For the chat sidebar, install Claude Code and sign in once by running `claude` in a terminal.

### Tests

```bat
python custom\test\router_test.py build\tar_router_cli.exe     :: router engine (CLI)
python custom\test\multi_vehicle_test.py build\tar_router_cli.exe
python custom\test\qgc_bridge_test.py                          :: against the running app
python custom\test\qgc_param_tools_test.py                     :: MCP parameter tools (with fake_px4_params.py)
```

---

## Upstream

QlaudeGC is a fork of [mavlink/qgroundcontrol](https://github.com/mavlink/qgroundcontrol). For general QGC usage, see the [QGC user guide](https://docs.qgroundcontrol.com/master/en/qgc-user-guide/). For building on other platforms, see the [QGC developer guide](https://docs.qgroundcontrol.com/master/en/qgc-dev-guide/).

QGroundControl and QlaudeGC are dual-licensed under Apache 2.0 and GPLv3. See [`.github/COPYING.md`](.github/COPYING.md).
