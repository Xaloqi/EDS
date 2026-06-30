# =============================================================================
# cmake/eds_service_sources.cmake — canonical UDS service handler source list
#
# USAGE (from any example CMakeLists.txt):
#
#   include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)
#   target_sources(app PRIVATE ${EDS_SERVICE_SRCS})
#
# WHY THIS FILE EXISTS:
#   service_registration.c references all 19 UDS service handler symbols by
#   name. Every build target must link against all 19 .c files or the linker
#   fails with "undefined reference to uds_service_0xNN_handler".
#
#   Previously each example enumerated the list independently. That caused
#   real bugs: when a new SID was added, examples were missed, and the only
#   symptom was a linker error at customer build time (not in CI).
#
# ADDING A NEW SID:
#   Add exactly one line here. All 20 example targets pick it up automatically.
#   No other CMakeLists.txt changes required.
#
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

# CMAKE_CURRENT_LIST_DIR is the cmake/ directory; one level up is the EDS root.
set(_EDS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

set(EDS_SERVICE_SRCS
    # Dispatch table — must always be first: provides g_uds_service_table[].
    ${_EDS_ROOT}/core/uds_services/service_registration.c

    # Core diagnostic services (SIDs 0x10 – 0x3D)
    ${_EDS_ROOT}/core/uds_services/service_0x10.c   # DiagnosticSessionControl
    ${_EDS_ROOT}/core/uds_services/service_0x11.c   # ECUReset
    ${_EDS_ROOT}/core/uds_services/service_0x14.c   # ClearDiagnosticInformation
    ${_EDS_ROOT}/core/uds_services/service_0x19.c   # ReadDTCInformation
    ${_EDS_ROOT}/core/uds_services/service_0x22.c   # ReadDataByIdentifier
    ${_EDS_ROOT}/core/uds_services/service_0x23.c   # ReadMemoryByAddress
    ${_EDS_ROOT}/core/uds_services/service_0x27.c   # SecurityAccess
    ${_EDS_ROOT}/core/uds_services/service_0x28.c   # CommunicationControl
    ${_EDS_ROOT}/core/uds_periodic.c                # Periodic scheduler (SID 0x2A)
    ${_EDS_ROOT}/core/uds_services/service_0x2A.c   # ReadDataByPeriodicIdentifier
    ${_EDS_ROOT}/core/uds_services/service_0x2E.c   # WriteDataByIdentifier
    ${_EDS_ROOT}/core/uds_services/service_0x2F.c   # InputOutputControlByIdentifier

    # Extended services
    ${_EDS_ROOT}/core/uds_services/service_0x31.c   # RoutineControl
    ${_EDS_ROOT}/core/uds_services/service_0x3E.c   # TesterPresent
    ${_EDS_ROOT}/core/uds_services/service_0x85.c   # ControlDTCSetting

    # Firmware download / memory services — all six must be present together.
    # service_registration.c registers all six unconditionally; omitting any
    # one causes a linker failure: "undefined reference to uds_service_0xNN_handler".
    ${_EDS_ROOT}/core/uds_services/service_0x34.c   # RequestDownload
    ${_EDS_ROOT}/core/uds_services/service_0x35.c   # RequestUpload
    ${_EDS_ROOT}/core/uds_services/service_0x36.c   # TransferData
    ${_EDS_ROOT}/core/uds_services/service_0x37.c   # RequestTransferExit
    ${_EDS_ROOT}/core/uds_services/service_0x3D.c   # WriteMemoryByAddress
)
