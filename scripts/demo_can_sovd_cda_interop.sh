#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/demo_can_sovd_cda_interop.sh
#
# PURPOSE: Prove -- every run, not just once in a session transcript -- that
#          real EDS answers real diagnostic requests, routed over SOVD, from
#          the real Eclipse OpenSOVD Classic Diagnostic Adapter (CDA), over a
#          real ISO 15765-4 CAN transport. No simulator stands in for either
#          side, and nothing it exercises could be exercised against a
#          fictional ODX fixture describing an ECU that doesn't run.
#
#          What it actually exercises, in order, each against real EDS code
#          in core/, transport/ and platform/ -- none of it modified for
#          this demo:
#            1. A real SOVD-routed, multi-frame ISO-TP read
#               (ReadDataByIdentifier, DID 0xF190 -- First Frame, Flow
#               Control, two Consecutive Frames on the wire).
#            2. Session control (UDS SID 0x10) -- required before step 3,
#               since core/uds_services/service_0x27.c refuses SecurityAccess
#               outside Extended/Programming session.
#            3. SecurityAccess (SID 0x27) with real cryptography -- EDS's own
#               AES-128-CMAC seed/key exchange (core/uds_security_algo.c),
#               computed independently by this script from the real seed
#               bytes it receives over the wire, not faked or pre-computed.
#            4. RoutineControl (SID 0x31) -- a real routine Start request,
#               dispatched by EDS's real service_0x31.c handler.
#
#          BOTH ENDPOINTS ARE REAL, AND THE SINGLE HOST PREREQUISITE IS
#          DOCKER:
#            - EDS:  examples/basic_ecu_cda_demo (a disclosed fork of
#              examples/basic_ecu -- see that example's own README.md for
#              exactly what differs and why), built for Zephyr's native_sim
#              board against a REAL host SocketCAN device (EDS#231's
#              boards/native_sim/native_sim_realcan.{conf,overlay}), inside a
#              container this script builds -- no pre-existing west
#              workspace, Zephyr SDK, or host cmake/ninja/gcc install
#              required.
#            - CDA: built from its own public source, pointed at its own
#              shipped FLXC1000.mdd ODX fixture (testcontainer/odx/,
#              Apache-2.0, no ISO schema purchase needed -- see
#              xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md).
#            - A `vxcan` virtual CAN pair joining the two, created inside a
#              Docker container via --cap-add=NET_ADMIN/NET_RAW -- this is
#              the answer to "but that needs sudo": it doesn't, it needs
#              CAP_NET_ADMIN in a network namespace, and `docker run` grants
#              that without any host privilege escalation. Re-verified for
#              this revision of the script, not assumed from the original
#              spike.
#
#          HONESTY, SAID PLAINLY, NOT BURIED:
#            - This is a CAN-only demo. DoIP does NOT work against the real
#              CDA: it discovers DoIP gateways exclusively by UDP Vehicle
#              Identification, and EDS has never implemented that side
#              (tracked as EDS#392). Nothing below exercises DoIP.
#            - EDS is a test target that happens to be real -- not "the
#              SoDeV diagnostics layer." This script proves interop, not an
#              architecture recommendation.
#            - The pytest/CAPL test-generation half of EDS needs the
#              commercial toolchain (tools/testgen.py). This script never
#              invokes it and does not need it -- see LICENSING below.
#            - examples/basic_ecu_cda_demo carries two entries (DID 0xF100,
#              routine 0x1001) whose specific IDs exist to match the CDA's
#              own shipped fixture, not because EDS needed them otherwise.
#              Disclosed in that example's README.md, not carried quietly.
#              A custom ODX fixture describing EDS's own real identity would
#              be the cleaner alternative; it isn't available -- the CDA's
#              own PDX-to-MDD converter is not publicly distributable (same
#              shape as this project's ISO 22901-1 decision, see
#              xaloqi-knowledge lessons/run-043). See that README for the
#              full reasoning.
#
#          LICENSING: every step below runs with no commercial EDS licence.
#          `west build` is invoked with -DDIAG_SKIP_CODEGEN=ON, which skips
#          tools/codegen.py entirely and uses the generated/ sources already
#          committed to this public, GPL-2.0/Apache-2.0 repo. Nothing in this
#          script reaches for tools/codegen.py, tools/testgen.py, or any
#          other commercial-tier tool.
#
# PREREQUISITES:
#   - Docker, with the invoking user able to run it without sudo (i.e.
#     already in the `docker` group) -- the only requirement `docker run`
#     itself has.
#   - Network access on first run only: to build this script's own EDS
#     builder image (Ubuntu + build tools, no Zephyr SDK needed -- native_sim
#     links against the host libc via its own toolchain-independent host
#     variant, nothing here cross-compiles), to clone the Zephyr RTOS source
#     tree (pinned by this repo's west.yml, ~v3.7.0), and to clone + build
#     the Eclipse CDA. All three are cached under XALOQI_DEMO_CACHE_DIR
#     (Zephyr workspace + CDA source/build) and in Docker's own image layer
#     cache (the builder image) -- repeat runs rebuild nothing that hasn't
#     changed.
#
# USAGE:
#   bash scripts/demo_can_sovd_cda_interop.sh
#
# ENV OVERRIDES:
#   XALOQI_DEMO_CACHE_DIR   base cache directory (default: ~/.cache/xaloqi-eds-demo)
#   CDA_REF                 git ref of the CDA to build (default: the exact
#                            commit this demo was last verified against)
#
# EXIT CODES:
#   0  Demo completed: real EDS answered every step above through the real
#      CDA, over real CAN.
#   1  A prerequisite is missing, or a build/run/verification step failed --
#      message says which, and where to find the failing step's log.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CDA_REF="${CDA_REF:-b470ab059a216803a58bbae8a14baef039466f45}"
CACHE_DIR="${XALOQI_DEMO_CACHE_DIR:-$HOME/.cache/xaloqi-eds-demo}"
CDA_CACHE_DIR="$CACHE_DIR/classic-diagnostic-adapter"
EDS_WEST_DIR="$CACHE_DIR/eds-west-workspace"
BUILDER_IMAGE="xaloqi-eds-demo-builder:native-sim"
RUN_IMAGE="ubuntu:24.04"

echo "============================================================"
echo "  EDS <-> real Eclipse OpenSOVD CDA, over real CAN"
echo "============================================================"
echo

# --- Prerequisite: Docker only ----------------------------------------------
if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker not found on PATH. This demo needs only Docker --" >&2
  echo "  see this script's header comment for exactly why." >&2
  exit 1
fi
if ! docker version >/dev/null 2>&1; then
  echo "ERROR: 'docker version' failed. Docker must be running and usable" >&2
  echo "  without sudo (the invoking user in the 'docker' group)." >&2
  exit 1
fi
echo "Docker         : OK"
echo "Cache dir      : $CACHE_DIR"
echo

mkdir -p "$CACHE_DIR"

# --- Step 1: build this script's own EDS builder image, cached by Docker ---
echo "--- [1/5] Building the EDS builder image (cached by Docker after first run) ---"
# Deliberately NOT the Zephyr SDK: native_sim is Zephyr's host-native board --
# with ZEPHYR_TOOLCHAIN_VARIANT=host (set below), it builds with the
# container's own gcc-multilib (producing a 32-bit binary, hence libc6:i386
# in the run image below) instead of looking for the SDK's cross-compiler,
# the same way this repo's own CI builds every native_sim example (see
# .github/workflows/ci.yml's zephyr-examples-native job, which installs no
# Zephyr SDK either). Installing the ~1GB SDK here would buy nothing for
# this specific board.
docker build -t "$BUILDER_IMAGE" -f - "$REPO_ROOT" > "$CACHE_DIR/builder-image.log" 2>&1 <<'DOCKERFILE_EOF' \
  || { echo "ERROR: builder image build failed -- see $CACHE_DIR/builder-image.log" >&2; tail -40 "$CACHE_DIR/builder-image.log" >&2; exit 1; }
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y --no-install-recommends -qq \
      build-essential gcc-multilib g++-multilib cmake ninja-build \
      device-tree-compiler python3 python3-pip python3-venv \
      git wget xz-utils ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --break-system-packages --no-cache-dir \
      west pyyaml jinja2 pyelftools
DOCKERFILE_EOF
echo "  image ready: $BUILDER_IMAGE"
echo

# --- Step 2: build examples/basic_ecu_cda_demo for native_sim, containerized
echo "--- [2/5] Building examples/basic_ecu_cda_demo for native_sim (containerized) ---"
mkdir -p "$EDS_WEST_DIR"
docker run --rm \
  -e ZEPHYR_TOOLCHAIN_VARIANT=host \
  -v "$EDS_WEST_DIR":/west \
  -v "$REPO_ROOT":/west/EDS:ro \
  "$BUILDER_IMAGE" bash -c '
    set -e
    cd /west
    if [ ! -d .west ]; then
      west init -l EDS
      west update --narrow --fetch-opt=--depth=1
    fi
    west build -b native_sim EDS/examples/basic_ecu_cda_demo -d build_demo --pristine -- \
      -DDIAG_SKIP_CODEGEN=ON \
      -DEXTRA_CONF_FILE=/west/EDS/boards/native_sim/native_sim_realcan.conf \
      -DDTC_OVERLAY_FILE=/west/EDS/boards/native_sim/native_sim_realcan.overlay
  ' > "$CACHE_DIR/eds-build.log" 2>&1 \
  || { echo "ERROR: EDS build failed -- see $CACHE_DIR/eds-build.log" >&2; tail -40 "$CACHE_DIR/eds-build.log" >&2; exit 1; }
EDS_BINARY="$EDS_WEST_DIR/build_demo/zephyr/zephyr.exe"
if [ ! -x "$EDS_BINARY" ]; then
  echo "ERROR: build reported success but $EDS_BINARY is missing." >&2
  exit 1
fi
echo "  built: $EDS_BINARY"
echo

# --- Step 3: clone + build the real CDA, cached across runs ----------------
echo "--- [3/5] Building the real Eclipse CDA (cached after first run) ---"
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

# --- Step 4: run both, over a vxcan pair, inside one container -------------
echo "--- [4/5] Standing up the vxcan link and starting both sides ---"

CONTAINER_NAME="eds-cda-demo-$$"
cleanup() {
  docker stop "$CONTAINER_NAME" >/dev/null 2>&1 || true
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# RUN_IMAGE is pinned to the SAME base (ubuntu:24.04) the builder image uses
# above, rather than sniffed from the host -- the native_sim binary links
# against whichever container built it, and now that it's always built in a
# container (step 2), the binary's glibc and this run image's glibc are the
# same by construction, not by matching /etc/os-release at run time.
docker run -d --name "$CONTAINER_NAME" --cap-add=NET_ADMIN --cap-add=NET_RAW \
  -v "$CDA_CACHE_DIR":/work:ro \
  -v "$CDA_CACHE_DIR/testcontainer/odx/FLXC1000.mdd":/fixture/FLXC1000.mdd:ro \
  "$RUN_IMAGE" sleep infinity > /dev/null

docker exec "$CONTAINER_NAME" bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  dpkg --add-architecture i386
  apt-get update -qq
  apt-get install -y -qq iproute2 can-utils curl libc6:i386 python3 python3-pip > /dev/null
  pip install --break-system-packages --quiet --no-cache-dir cryptography
' > "$CACHE_DIR/container-setup.log" 2>&1 \
  || { echo "ERROR: container package setup failed -- see $CACHE_DIR/container-setup.log" >&2; exit 1; }

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
VARIANT="$(docker exec "$CONTAINER_NAME" curl -s http://127.0.0.1:20002/Vehicle/v15/components/flxc1000 \
  | sed -n 's/.*"variant":{"name":"\([^"]*\)".*/\1/p')"
echo "  real EDS discovered and held Online by the real CDA (variant: $VARIANT)."
echo

auth_token() {
  docker exec "$CONTAINER_NAME" curl -s -X POST \
    http://127.0.0.1:20002/vehicle/v15/authorize \
    -H 'Content-Type: application/json' \
    -d '{"client_id":"demo","client_secret":"demo"}' \
    | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p'
}

# --- Step 5: exercise what a fictional MDD cannot ---------------------------
echo "--- [5/5] Session control, SecurityAccess (real crypto), RoutineControl, multi-frame read ---"

TOKEN="$(auth_token)"
if [ -z "$TOKEN" ]; then
  echo "ERROR: did not get an access token from the CDA's dev-mode /authorize endpoint." >&2
  exit 1
fi

echo "  [a] multi-frame ISO-TP read: ReadDataByIdentifier(VINDataIdentifier, 0xF190)"
RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/data/VINDataIdentifier)"
echo "$RESULT" | sed 's/^/      /'
echo "$RESULT" | grep -q "HTTP_STATUS:200" \
  || { echo "ERROR: VIN read did not return 200." >&2; exit 1; }

echo "  [b] acquiring component lock (required before session/security changes)"
LOCK_RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/locks -d '{"lock_expiration": 100000}')"
echo "$LOCK_RESULT" | sed 's/^/      /'
echo "$LOCK_RESULT" | grep -q "HTTP_STATUS:201" \
  || { echo "ERROR: component lock was not granted." >&2; exit 1; }

echo "  [c] session control: switching to Extended session (UDS SID 0x10)"
SESSION_RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/modes/session -d '{"value":"extended"}')"
echo "$SESSION_RESULT" | sed 's/^/      /'
echo "$SESSION_RESULT" | grep -q "HTTP_STATUS:200" \
  || { echo "ERROR: session switch to Extended failed." >&2; exit 1; }

echo "  [d] SecurityAccess (SID 0x27): RequestSeed, sub-function 0x03"
SEED_RESULT="$(docker exec "$CONTAINER_NAME" curl -s -X PUT \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/modes/security -d '{"value":"3_RequestSeed"}')"
echo "      $SEED_RESULT"
SEED_HEX="$(echo "$SEED_RESULT" | sed -n 's/.*"Request_Seed":"\([^"]*\)".*/\1/p')"
if [ -z "$SEED_HEX" ]; then
  echo "ERROR: did not receive a seed from RequestSeed." >&2
  exit 1
fi

# The computation below is real, independent AES-128-CMAC -- not a lookup
# table and not pre-computed -- against the real seed bytes real EDS just
# sent over the wire. basic_ecu_cda_demo's Level-2 key is EDS's own
# documented placeholder (core/uds_security_algo.c, s_level_keys[1]):
# 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f.
echo "  [e] computing AES-128-CMAC(seed, basic_ecu's documented Level-2 key), truncated to 4 bytes"
KEY_HEX="$(docker exec "$CONTAINER_NAME" python3 -c "
from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import algorithms

level2_key = bytes.fromhex('101112131415161718191a1b1c1d1e1f')
seed_tokens = '$SEED_HEX'.split()
seed = bytes(int(t, 16) for t in seed_tokens)
c = cmac.CMAC(algorithms.AES(level2_key))
c.update(seed)
key = c.finalize()[:4]
print(' '.join(f'0x{b:02x}' for b in key))
")"
echo "      seed: $SEED_HEX"
echo "      key : $KEY_HEX"

echo "  [f] SecurityAccess (SID 0x27): SendKey, sub-function 0x04"
SENDKEY_RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/modes/security \
  -d "{\"value\":\"Level_3\",\"Key\":{\"Send_Key\":\"$KEY_HEX\"}}")"
echo "$SENDKEY_RESULT" | sed 's/^/      /'
echo "$SENDKEY_RESULT" | grep -q "HTTP_STATUS:200" \
  || { echo "ERROR: SendKey was rejected -- real EDS did not accept the computed key." >&2; exit 1; }

SEC_STATUS="$(docker exec "$CONTAINER_NAME" curl -s -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/modes/security)"
echo "      security status: $SEC_STATUS"
echo "$SEC_STATUS" | grep -q '"value":"Level_3"' \
  || { echo "ERROR: security status did not confirm Level_3 unlocked." >&2; exit 1; }

echo "  [g] RoutineControl (SID 0x31): starting routine 'SelfTest' (ID 0x1001)"
ROUTINE_RESULT="$(docker exec "$CONTAINER_NAME" curl -s -w '\nHTTP_STATUS:%{http_code}' \
  -X POST -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  http://127.0.0.1:20002/Vehicle/v15/components/flxc1000/operations/SelfTest/executions -d '{}')"
echo "$ROUTINE_RESULT" | sed 's/^/      /'
echo "$ROUTINE_RESULT" | grep -q "HTTP_STATUS:200" \
  || { echo "ERROR: RoutineControl Start did not return 200." >&2; exit 1; }
echo "$ROUTINE_RESULT" | grep -q '"RoutineId":4097' \
  || { echo "ERROR: RoutineControl response did not echo RoutineId 4097 (0x1001)." >&2; exit 1; }

echo
echo "============================================================"
echo "  Real EDS answered every step above through the real Eclipse"
echo "  OpenSOVD CDA, over real CAN. Nothing above was mocked:"
echo "    - multi-frame ISO-TP read               : PASS"
echo "    - session control (SID 0x10)             : PASS"
echo "    - SecurityAccess, real AES-128-CMAC (0x27): PASS"
echo "    - RoutineControl (SID 0x31)               : PASS"
echo "============================================================"
echo
echo "The VIN above reads as all zero bytes -- basic_ecu_cda_demo's example"
echo "DID handler returns a placeholder, which is correct for an"
echo "unconfigured example, not a defect."
echo
echo "What this does NOT prove: DoIP (the real CDA discovers DoIP gateways"
echo "only by UDP Vehicle Identification, which EDS has never implemented --"
echo "EDS#392), or that the pytest/CAPL test-generation half of EDS works"
echo "without a commercial licence (it does not -- see README.md)."
echo "See examples/basic_ecu_cda_demo/README.md for why two of this ECU's"
echo "diagnostic IDs were chosen to match the CDA's own shipped fixture, and"
echo "xaloqi-knowledge campaigns/2026-10-12-cda-can-spike.md for the full"
echo "decoded wire traces this was verified against."
