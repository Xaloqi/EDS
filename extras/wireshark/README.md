# Xaloqi EDS — Wireshark Lua Dissector

Decodes UDS (ISO 14229-1), ISO-TP (ISO 15765-2), and DoIP (ISO 13400-2) traffic inline in Wireshark packet captures.

## Installation

1. Copy `eds.lua` to your Wireshark plugins directory:
   - **Linux/macOS:** `~/.local/lib/wireshark/plugins/` or `~/.wireshark/plugins/`
   - **Windows:** `%APPDATA%\Wireshark\plugins\`
2. In Wireshark: **Analyze → Reload Lua Plugins** (or restart Wireshark).
3. Verify: open **Help → About Wireshark → Plugins** — `eds.lua` should appear in the list.

Alternatively, load it without installing:

```bash
wireshark -Xlua,script:extras/wireshark/eds.lua
```

## Usage

### DoIP captures (Ethernet / TCP)

DoIP on TCP or UDP port 13400 is decoded automatically. Open any `.pcap` or `.pcapng` file containing DoIP traffic and the EDS-DoIP tree appears immediately in the packet details pane.

Decoded fields: protocol version, payload type name, source/target ECU addresses, UDS service name, NRC description.

### SocketCAN captures (CAN bus / vcan)

ISO-TP frames on CAN require manual `Decode As` because CAN arbitration IDs are application-defined:

1. Open a `.pcap` captured with `candump -l` or `tcpdump` on a `vcan` interface.
2. Select a CAN frame carrying ISO-TP traffic.
3. **Analyze → Decode As…** → set the `can.subdissector` row to `eds_isotp`.
4. Click **OK** — all frames decode as ISO-TP with UDS payload for Single Frames.

### Command-line / automated

```bash
# Decode a DoIP capture non-interactively and print the decode tree
tshark -Xlua,script:extras/wireshark/eds.lua -r capture.pcap -V -Y eds_doip
```
