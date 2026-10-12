#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/demo_can_sovd_cda_interop.sh
#
# PURPOSE: Prove -- every run, not just once in a session transcript -- that
#          real EDS answers a real SOVD-routed diagnostic read from the real
#          Eclipse OpenSOVD Classic Diagnostic Adapter (CDA), over a real
#          ISO 15765-4 CAN transport. No simulator stands in for either side.
#
#          What it actually exercises, in order:
#            1. examples/basic_ecu built for Zephyr's native_sim board,
#               wired to a REAL host SocketCAN device (EDS#231's
#               boards/native_sim/native_sim_realcan.{conf,overlay} --
#               Zephyr's in-process can_loopback0 can only ever talk to
#               itself, so this is the one build configuration that can
#               reach anything external at all).
#            2. The real Eclipse CDA, built from its own public source,
#               pointed at its own shipped FLXC1000.mdd ODX fixture
#               (testcontainer/odx/, Apache-2.0, no ISO schema purchase
#               needed -- see xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md
#               for how that was established).
#            3. A `vxcan` virtual CAN pair joining the two, created inside a
#               Docker container via --cap-add=NET_ADMIN/NET_RAW -- this is
#               the answer to "but that needs sudo": it doesn't, it needs
#               CAP_NET_ADMIN in a network namespace, and `docker run` grants
#               that without any host privilege escalation.
#            4. A real SOVD HTTP read: authenticate against the CDA's own
#               dev-mode security plugin, then GET the VINDataIdentifier
#               resource -- which decodes to UDS ReadDataByIdentifier(0xF190)
#               on the wire, a real multi-frame ISO-TP exchange (First Frame
#               + Flow Control + two Consecutive Frames), answered by
#               basic_ecu's actual, unmodified DID handler.
#
#          This is EDS's half of the interop. The CDA's dev-mode auth plugin
#          (`cda-plugin-security`, built without its `auth` feature) performs
#          no real credential check -- expected and correct for this demo,
#          and explicitly NOT a production security posture; a deployed CDA
#          needs the `auth` feature and a real security plugin.
#
#          Full decoded wire trace and how each step was first established:
#          xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md
#
# PREREQUISITES (checked up front; the script says which is missing and
# exits rather than failing confusingly later):
#   - A working west workspace with this EDS checkout as a module and the
#     Zephyr SDK installed -- the same thing `west build` already needs for
#     any native_sim example. Run this script FROM that workspace's topdir,
#     or pass it as $1, or export WEST_TOPDIR.
#   - A host toolchain able to build native_sim: cmake, ninja, gcc, dtc.
#     (native_sim links against the HOST's own libc, not a cross-compiler --
#     if `west build -b native_sim` already works for you, this is covered.)
#   - Docker, with the invoking user able to run it without sudo (i.e.
#     already in the `docker` group) -- the same requirement `docker run`
#     has on its own.
#   - Network access on first run only, to clone and build the Eclipse CDA
#     (github.com/eclipse-opensovd/classic-diagnostic-adapter, pinned below
#     for reproducibility) inside a `rust:latest` container. Cached under
#     CDA_CACHE_DIR afterwards -- repeat runs do not rebuild it unless its
#     source changed.
#
# USAGE:
#   bash scripts/demo_can_sovd_cda_interop.sh [WEST_TOPDIR]
#
# ENV OVERRIDES:
#   WEST_TOPDIR     west workspace topdir (default: $1, or $PWD if it looks
#                   like one, or `west topdir` from $PWD)
#   CDA_CACHE_DIR   where the CDA clone + build persists between runs
#                   (default: ~/.cache/xaloqi-eds-demo/classic-diagnostic-adapter)
#   CDA_REF         git ref to build (default: the exact commit this demo
#                   was last verified against -- see CDA_REF below)
#
# EXIT CODES:
#   0  Demo completed: real EDS answered a real SOVD read from the real CDA.
#   1  A prerequisite is missing, or a build/run step failed -- message says
#      which, and where to find the failing step's log.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Pinned for reproducibility: the exact CDA commit this demo was built and
# verified against (xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md).
# Override with CDA_REF=origin/master (or any ref) to test against upstream's
# current state instead -- useful for noticing if something there changed,
# at the cost of this demo no longer being a fixed point of comparison.
CDA_REF="${CDA_REF:-b470ab059a216803a58bbae8a14baef039466f45}"
CDA_CACHE_DIR="${CDA_CACHE_DIR:-$HOME/.cache/xaloqi-eds-demo/classic-diagnostic-adapter}"

echo "============================================================"
echo "  EDS <-> real Eclipse OpenSOVD CDA, over real CAN"
echo "============================================================"
echo

# --- Resolve the west workspace topdir -------------------------------------
if [ "$#" -gt 0 ]; then
  WEST_TOPDIR="$1"
elif [ -n "${WEST_TOPDIR:-}" ]; then
  :
elif [ -d "$PWD/.west" ]; then
  WEST_TOPDIR="$PWD"
elif command -v west >/dev/null 2>&1 && west topdir >/dev/null 2>&1; then
  WEST_TOPDIR="$(west topdir)"
else
  echo "ERROR: couldn't find a west workspace." >&2
  echo "  Run this from your west workspace topdir, pass it as an argument," >&2
  echo "  or export WEST_TOPDIR. It needs this EDS checkout as a module and" >&2
  echo "  the Zephyr SDK installed -- the same workspace 'west build -b" >&2
  echo "  native_sim examples/basic_ecu' already needs to work in." >&2
  exit 1
fi
WEST_TOPDIR="$(cd "$WEST_TOPDIR" && pwd)"
if [ ! -d "$WEST_TOPDIR/.west" ]; then
  echo "ERROR: $WEST_TOPDIR does not look like a west workspace (no .west/)." >&2
  exit 1
fi
echo "West workspace : $WEST_TOPDIR"

# --- Check the rest of the prerequisites up front ---------------------------
for tool in docker west cmake ninja gcc dtc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: required tool not found on PATH: $tool" >&2
    exit 1
  fi
done
if ! docker version >/dev/null 2>&1; then
  echo "ERROR: 'docker version' failed. Docker must be running and usable" >&2
  echo "  without sudo (the invoking user in the 'docker' group)." >&2
  exit 1
fi
echo "Docker         : OK"
echo

# --- Step 1: build basic_ecu for native_sim against a REAL host CAN iface --
echo "--- [1/4] Building examples/basic_ecu for native_sim (real SocketCAN) ---"
EDS_BUILD_DIR="$WEST_TOPDIR/build_demo_can_sovd_interop"
# `west` detects its workspace by walking up from the CWD, not from any path
# given on the command line -- so this must run with CWD inside WEST_TOPDIR
# even though every path below it is already absolute.
(
  cd "$WEST_TOPDIR"
  west build -b native_sim "$REPO_ROOT/examples/basic_ecu" -d "$EDS_BUILD_DIR" -- \
    -DEXTRA_CONF_FILE="$REPO_ROOT/boards/native_sim/native_sim_realcan.conf" \
    -DDTC_OVERLAY_FILE="$REPO_ROOT/boards/native_sim/native_sim_realcan.overlay"
) > "$EDS_BUILD_DIR.log" 2>&1 \
  || { echo "ERROR: west build failed -- see $EDS_BUILD_DIR.log" >&2; tail -40 "$EDS_BUILD_DIR.log" >&2; exit 1; }
EDS_BINARY="$EDS_BUILD_DIR/zephyr/zephyr.exe"
if [ ! -x "$EDS_BINARY" ]; then
  echo "ERROR: build reported success but $EDS_BINARY is missing." >&2
  exit 1
fi
# `west build` re-runs codegen.py, which re-stamps its own "Generated:"
# comment with the current time -- a real, separately-worth-fixing
# reproducibility gap (codegen.py doesn't honour SOURCE_DATE_EPOCH the way
# testgen.py does post-EDS#224), but not this script's problem to fix.
# Revert that timestamp-only noise so running this demo never leaves the
# repo looking dirty.
git -C "$REPO_ROOT" checkout -- examples/basic_ecu/generated/ 2>/dev/null || true
echo "  built: $EDS_BINARY"
echo

# --- Step 2: clone + build the real CDA, cached across runs ----------------
echo "--- [2/4] Building the real Eclipse CDA (cached after first run) ---"
mkdir -p "$(dirname "$CDA_CACHE_DIR")"
if [ ! -d "$CDA_CACHE_DIR/.git" ]; then
  git clone https://github.com/eclipse-opensovd/classic-diagnostic-adapter.git "$CDA_CACHE_DIR" \
    > "$CDA_CACHE_DIR.clone.log" 2>&1 \
    || { echo "ERROR: git clone failed -- see $CDA_CACHE_DIR.clone.log" >&2; exit 1; }
fi
git -C "$CDA_CACHE_DIR" fetch origin --quiet
git -C "$CDA_CACHE_DIR" checkout --quiet "$CDA_REF"

docker run --rm \
  -v "$CDA_CACHE_DIR":/work \
  rust:latest bash -c "
    git config --global --add safe.directory /work
    cd /work && cargo build --release --features can -p opensovd-cda
  " > "$CDA_CACHE_DIR.build.log" 2>&1 \
  || { echo "ERROR: CDA build failed -- see $CDA_CACHE_DIR.build.log" >&2; tail -40 "$CDA_CACHE_DIR.build.log" >&2; exit 1; }
CDA_BINARY="$CDA_CACHE_DIR/target/release/opensovd-cda"
if [ ! -x "$CDA_BINARY" ]; then
  echo "ERROR: CDA build reported success but $CDA_BINARY is missing." >&2
  exit 1
fi
echo "  built: $CDA_BINARY"
echo

# --- Step 3: run both, over a vxcan pair, inside one container -------------
echo "--- [3/4] Standing up the vxcan link and starting both sides ---"

# Match the run container's glibc to the host's, rather than hardcoding a
# version -- native_sim's binary links against the host's own libc, and an
# older container glibc than the host's will refuse to run it.
RUN_IMAGE="ubuntu:latest"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  if [ "${ID:-}" = "ubuntu" ] && [ -n "${VERSION_ID:-}" ]; then
    RUN_IMAGE="ubuntu:${VERSION_ID}"
  fi
fi

CONTAINER_NAME="eds-cda-demo-$$"
cleanup() {
  docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker run -d --name "$CONTAINER_NAME" --cap-add=NET_ADMIN --cap-add=NET_RAW \
  -v "$CDA_CACHE_DIR":/work:ro \
  -v "$CDA_CACHE_DIR/testcontainer/odx/FLXC1000.mdd":/fixture/FLXC1000.mdd:ro \
  "$RUN_IMAGE" sleep infinity > /dev/null

docker exec "$CONTAINER_NAME" bash -c '
  set -e
  dpkg --add-architecture i386
  apt-get update -qq
  apt-get install -y -qq iproute2 can-utils curl libc6:i386 > /dev/null
' > "$WEST_TOPDIR/build_demo_can_sovd_interop.container-setup.log" 2>&1 \
  || { echo "ERROR: container package setup failed -- see $WEST_TOPDIR/build_demo_can_sovd_interop.container-setup.log" >&2; exit 1; }

docker cp "$EDS_BINARY" "$CONTAINER_NAME:/eds-demo-eds.exe"
docker exec "$CONTAINER_NAME" chmod +x /eds-demo-eds.exe

docker exec "$CONTAINER_NAME" bash -c '
  set -e
  ip link add dev vxcan0 type vxcan peer name vxcan1
  ip link set vxcan0 up
  ip link set vxcan1 up
  ip link property add dev vxcan1 altname zcan0

  mkdir -p /cda-run/mdds /cda-run/flash
  cp /fixture/FLXC1000.mdd /cda-run/mdds/
  cp /work/opensovd-cda-can.toml /cda-run/cda.toml
  # The shipped example overrides [ecu.FLXC1000] protocol = "UDS_CAN", which
  # does not match this fixture (its real embedded protocol is the DoIP-
  # shaped global default -- see eclipse-opensovd/classic-diagnostic-adapter#619,
  # filed upstream). Dropping the override lets it resolve correctly.
  sed -i "/\[ecu.FLXC1000\]/,/protocol = \"UDS_CAN\"/d" /cda-run/cda.toml
'

docker exec -d "$CONTAINER_NAME" bash -c "/eds-demo-eds.exe > /tmp/eds.log 2>&1"
sleep 1
docker exec -d "$CONTAINER_NAME" bash -c "
  cd /cda-run && CDA_CONFIG_FILE=/cda-run/cda.toml RUST_LOG=info /work/target/release/opensovd-cda > /tmp/cda.log 2>&1
"

echo "  waiting for the CDA to discover real EDS on the CAN bus..."
ONLINE=0
for _ in $(seq 1 20); do
  sleep 1
  if docker exec "$CONTAINER_NAME" curl -s http://127.0.0.1:20002/Vehicle/v15/components/flxc1000 \
       2>/dev/null | grep -q '"state":"Online"'; then
    ONLINE=1
    break
  fi
done
if [ "$ONLINE" -ne 1 ]; then
  echo "ERROR: FLXC1000 never reached 'Online'. Logs:" >&2
  echo "--- real EDS ---" >&2
  docker exec "$CONTAINER_NAME" cat /tmp/eds.log >&2 || true
  echo "--- real CDA ---" >&2
  docker exec "$CONTAINER_NAME" cat /tmp/cda.log >&2 || true
  exit 1
fi
echo "  real EDS discovered and held Online by the real CDA."
echo

# --- Step 4: a real SOVD-routed diagnostic read -----------------------------
echo "--- [4/4] Reading VINDataIdentifier (DID 0xF190) via SOVD HTTP ---"
TOKEN="$(docker exec "$CONTAINER_NAME" curl -s -X POST \
  http://127.0.0.1:20002/vehicle/v15/authorize \
  -H 'Content-Type: application/json' \
  -d '{"client_id":"demo","client_secret":"demo"}' \
  | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')"
if [ -z "$TOKEN" ]; then
  echo "ERROR: did not get an access token from the CDA's dev-mode /authorize endpoint." >&2
  exit 1
fi

RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/data/VINDataIdentifier)"
echo "$RESULT"
echo

if ! echo "$RESULT" | grep -q "HTTP_STATUS:200"; then
  echo "ERROR: the SOVD data read did not return 200. See output above." >&2
  exit 1
fi

echo "============================================================"
echo "  Real EDS answered a real SOVD-routed UDS read, over real CAN,"
echo "  from the real Eclipse OpenSOVD CDA. Nothing above was mocked."
echo "============================================================"
echo
echo "The VIN above reads as all zero bytes -- basic_ecu's example DID"
echo "handler returns a placeholder, which is correct for an unconfigured"
echo "example, not a defect. On the wire this was a complete, correct"
echo "ISO-TP multi-frame exchange (First Frame + Flow Control + two"
echo "Consecutive Frames, 20 bytes) -- see xaloqi-knowledge"
echo "campaigns/2026-10-12-cda-can-spike.md for the decoded trace."
