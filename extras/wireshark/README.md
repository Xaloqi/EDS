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
wireshark -X lua_script:extras/wireshark/eds.lua
```

The option is `-X lua_script:<file>`. `-Xlua,script:<file>` does not load the script, and the
`eds_*` protocols are then unknown.

Wireshark does not load Lua plugins when it runs as root (it prints "Running as user root … This
could be dangerous" and `eds_*` is missing). Capture as root if you need to, decode as a normal user.

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

On the command line the same Decode As is `-d can.subdissector,eds_isotp` (no `==` selector: the
table has none):

```bash
tshark -X lua_script:extras/wireshark/eds.lua -d can.subdissector,eds_isotp -r capture.pcap -Y eds_isotp
```

### Command-line / automated

```bash
# Decode a DoIP capture non-interactively and print the decode tree
tshark -X lua_script:extras/wireshark/eds.lua -r capture.pcap -V -Y eds_doip
```
