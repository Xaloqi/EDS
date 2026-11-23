# canfd_ecu — CAN FD channel example

What `can.fd: true` in `diagnostics_config.yaml` changes, and nothing else
(EDS#557, EDS#546). The YAML sets `fd: true`, `fd_tx_dl: 64`, `fd_brs: true`;
codegen turns that into `isotp_cfg_t.use_fd / tx_dl / brs` in the generated
`uds_init.c`. The main loop is the same as every other example's.

| Request | On the wire (ECU side) |
|---|---|
| `22 F190` | 20-byte answer as one escape Single Frame (`00 14 …`), a 24-byte FD frame |
| `22 0100` | 67-byte answer: First Frame of 64 bytes + one Consecutive Frame |
| `22` + 32 DIDs | 65-byte request (FF + CF from the tester; the ECU's Flow Control `30 10 00` is an FD frame), 2113-byte answer: FF + 33 CFs of 64 bytes, the last padded to 48 |
| `10 03`, `2A 03 01` | 0xF201's 20-byte record every 10 ms on `0x5E8`: `[01, uptime(4), A0..AF]`, one 24-byte FD frame, no PCI, no `6A` |

Every frame is a CAN FD frame with BRS set. Build with `ISOTP_ENABLE_CAN_FD=1`
(`CMakeLists.txt`) and `CONFIG_CAN_FD_MODE=y` (`prj.conf`); on hardware the
data bit rate comes from the board's devicetree (`bitrate-data`; `bus-speed-data`
is its deprecated name).

## Run it on native_sim against Linux can-isotp

```sh
W=<EDS checkout>; E=$W/examples/canfd_ecu
ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim $E -- -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=$E/boards/native_sim/native_sim_socketcan.conf \
  -DDTC_OVERLAY_FILE=$E/boards/native_sim/native_sim_socketcan.overlay

# needs the vcan and can-isotp modules (sudo modprobe vcan can_isotp)
unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 mtu 72 up &&
                   python3 scripts/smoke_canfd_ecu.py build/zephyr/zephyr.exe zcan0'
```

The tester in `scripts/smoke_canfd_ecu.py` is the Linux kernel's ISO-TP stack
(a `CAN_ISOTP` socket with mtu 72, TX_DL 64, BRS), which only accepts CAN FD
frames; a raw FD socket checks lengths, padding and flags. CI runs it
(`example-canfd-runtime`). `boards/native_sim/native_sim.conf` is the loopback
variant the build matrix compiles.

## On the FRDM-MCXN947

`boards/frdm_mcxn947/frdm_mcxn947_canfd.overlay` sets FlexCAN0 to 500 kbit/s and
2 Mbit/s with BRS. Build it after the board's own overlay and conf:

```sh
W=<EDS checkout>; E=$W/examples/canfd_ecu
west build -p always -b frdm_mcxn947/mcxn947/cpu0 $E -- -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=$W/boards/frdm_mcxn947/frdm_mcxn947.conf \
  "-DDTC_OVERLAY_FILE=$W/boards/frdm_mcxn947/frdm_mcxn947.overlay;$E/boards/frdm_mcxn947/frdm_mcxn947_canfd.overlay"
```

The ECU keeps its DTC mirror in RAM here: this example passes no NVM partition
to `nvm_store_init()` on hardware.

**Evidence:** compile-verified, run on native_sim, and one CAN FD frame with BRS
received from the FRDM-MCXN947 (`22 F190`, 24 bytes, 500 kbit/s / 2 Mbit/s,
board 700-90818 A1, 2026-11-22). The multi-frame cases above have not run on any
board.
