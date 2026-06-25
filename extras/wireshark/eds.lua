-- eds.lua — Xaloqi EDS Wireshark dissector
-- Version: 1.8.4
-- Protocols: UDS (ISO 14229-1), ISO-TP (ISO 15765-2), DoIP (ISO 13400-2)
--
-- Usage:
--   wireshark -Xlua,script:extras/wireshark/eds.lua
-- Or copy to your Wireshark plugins directory and reload via Analyze → Reload Lua Plugins.
-- See extras/wireshark/README.md for full installation steps.

-- ─── Lookup tables ───────────────────────────────────────────────────────────

local UDS_SERVICES = {
    [0x10] = "DiagnosticSessionControl",
    [0x11] = "ECUReset",
    [0x14] = "ClearDiagnosticInformation",
    [0x19] = "ReadDTCInformation",
    [0x22] = "ReadDataByIdentifier",
    [0x27] = "SecurityAccess",
    [0x28] = "CommunicationControl",
    [0x2E] = "WriteDataByIdentifier",
    [0x31] = "RoutineControl",
    [0x34] = "RequestDownload",
    [0x36] = "TransferData",
    [0x37] = "RequestTransferExit",
    [0x3E] = "TesterPresent",
    [0x85] = "ControlDTCSetting",
}

local UDS_NRC = {
    [0x10] = "generalReject",
    [0x11] = "serviceNotSupported",
    [0x12] = "subFunctionNotSupported",
    [0x13] = "incorrectMessageLengthOrInvalidFormat",
    [0x14] = "responseTooLong",
    [0x21] = "busyRepeatRequest",
    [0x22] = "conditionsNotCorrect",
    [0x24] = "requestSequenceError",
    [0x31] = "requestOutOfRange",
    [0x33] = "securityAccessDenied",
    [0x34] = "authenticationRequired",
    [0x35] = "invalidKey",
    [0x36] = "exceededNumberOfAttempts",
    [0x37] = "requiredTimeDelayNotExpired",
    [0x70] = "uploadDownloadNotAccepted",
    [0x71] = "transferDataSuspended",
    [0x72] = "generalProgrammingFailure",
    [0x73] = "wrongBlockSequenceCounter",
    [0x78] = "requestCorrectlyReceivedResponsePending",
    [0x7E] = "subFunctionNotSupportedInActiveSession",
    [0x7F] = "serviceNotSupportedInActiveSession",
}

local ISOTP_FRAME_TYPES = {
    [0x0] = "Single Frame (SF)",
    [0x1] = "First Frame (FF)",
    [0x2] = "Consecutive Frame (CF)",
    [0x3] = "Flow Control (FC)",
}

local ISOTP_FC_STATUS = {
    [0x0] = "ContinueToSend (CTS)",
    [0x1] = "Wait",
    [0x2] = "Overflow",
}

local DOIP_PAYLOAD_TYPES = {
    [0x0005] = "RoutingActivationRequest",
    [0x0006] = "RoutingActivationResponse",
    [0x0007] = "AliveCheckRequest",
    [0x0008] = "AliveCheckResponse",
    [0x8001] = "DiagnosticMessage",
    [0x8002] = "DiagnosticMessagePositiveAck",
    [0x8003] = "DiagnosticMessageNegativeAck",
}

-- ─── UDS dissector ────────────────────────────────────────────────────────────

local uds_proto  = Proto("eds_uds",  "EDS UDS (ISO 14229-1)")
local f_uds_sid  = ProtoField.uint8 ("eds_uds.sid",     "Service ID",           base.HEX)
local f_uds_name = ProtoField.string("eds_uds.name",    "Service")
local f_uds_rsid = ProtoField.uint8 ("eds_uds.rsid",    "Request SID (echoed)", base.HEX)
local f_uds_nrc  = ProtoField.uint8 ("eds_uds.nrc",     "NRC",                  base.HEX)
local f_uds_nrcn = ProtoField.string("eds_uds.nrc_name","NRC Name")
local f_uds_data = ProtoField.bytes ("eds_uds.data",    "Payload")
uds_proto.fields = { f_uds_sid, f_uds_name, f_uds_rsid, f_uds_nrc, f_uds_nrcn, f_uds_data }

local function dissect_uds(tvb, pinfo, tree)
    if tvb:len() < 1 then return end
    local sid = tvb:range(0, 1):uint()
    local subtree = tree:add(uds_proto, tvb(), "UDS")

    if sid == 0x7F then
        -- Negative response: 0x7F <request_SID> <NRC>
        subtree:add(f_uds_sid,  tvb:range(0, 1)):append_text(" (NegativeResponse)")
        pinfo.cols.protocol:set("EDS-UDS")
        if tvb:len() >= 2 then
            local rsid  = tvb:range(1, 1):uint()
            local rname = UDS_SERVICES[rsid] or "unknown service"
            subtree:add(f_uds_rsid, tvb:range(1, 1)):append_text(" [" .. rname .. "]")
        end
        if tvb:len() >= 3 then
            local nrc   = tvb:range(2, 1):uint()
            local nname = UDS_NRC[nrc] or "reserved"
            subtree:add(f_uds_nrc,  tvb:range(2, 1))
            subtree:add(f_uds_nrcn, tvb:range(2, 1), nname)
            pinfo.cols.info:set(string.format("NegativeResponse NRC=0x%02X (%s)", nrc, nname))
        end
    else
        local is_response = (sid >= 0x40) and (UDS_SERVICES[sid - 0x40] ~= nil)
        local base_sid    = is_response and (sid - 0x40) or sid
        local sname       = UDS_SERVICES[base_sid] or "unknown service"
        local direction   = is_response and "Response" or "Request"

        subtree:add(f_uds_sid,  tvb:range(0, 1)):append_text(
            string.format(" [%s %s]", sname, direction))
        subtree:add(f_uds_name, tvb:range(0, 1), sname)
        pinfo.cols.protocol:set("EDS-UDS")
        pinfo.cols.info:set(string.format("0x%02X %s %s", sid, sname, direction))

        if tvb:len() > 1 then
            subtree:add(f_uds_data, tvb:range(1))
        end
    end
end

-- ─── ISO-TP dissector ─────────────────────────────────────────────────────────

local isotp_proto   = Proto("eds_isotp",  "EDS ISO-TP (ISO 15765-2)")
local f_pci_type    = ProtoField.uint8 ("eds_isotp.pci_type",  "PCI Frame Type",    base.HEX)
local f_pci_name    = ProtoField.string("eds_isotp.pci_name",  "Frame Type Name")
local f_sf_dl       = ProtoField.uint8 ("eds_isotp.sf_dl",     "SF Data Length",    base.DEC)
local f_ff_dl       = ProtoField.uint16("eds_isotp.ff_dl",     "FF Data Length",    base.DEC)
local f_cf_sn       = ProtoField.uint8 ("eds_isotp.cf_sn",     "Sequence Number",   base.DEC)
local f_fc_fs       = ProtoField.uint8 ("eds_isotp.fc_fs",     "Flow Status",       base.HEX)
local f_fc_fsn      = ProtoField.string("eds_isotp.fc_fs_name","Flow Status Name")
local f_fc_bs       = ProtoField.uint8 ("eds_isotp.fc_bs",     "Block Size",        base.DEC)
local f_fc_stmin    = ProtoField.uint8 ("eds_isotp.fc_stmin",  "STmin",             base.HEX)
local f_isotp_data  = ProtoField.bytes ("eds_isotp.data",      "Data")
isotp_proto.fields  = {
    f_pci_type, f_pci_name, f_sf_dl, f_ff_dl, f_cf_sn,
    f_fc_fs, f_fc_fsn, f_fc_bs, f_fc_stmin, f_isotp_data,
}

local function dissect_isotp(tvb, pinfo, tree)
    if tvb:len() < 1 then return end
    local b0      = tvb:range(0, 1):uint()
    local pci     = bit.rshift(bit.band(b0, 0xF0), 4)
    local subtree = tree:add(isotp_proto, tvb(), "ISO-TP")
    subtree:add(f_pci_type, tvb:range(0, 1), pci)
    subtree:add(f_pci_name, tvb:range(0, 1), ISOTP_FRAME_TYPES[pci] or "Unknown")
    pinfo.cols.protocol:set("EDS-ISOTP")

    if pci == 0x0 then
        -- Single Frame
        local dl
        if b0 == 0x00 and tvb:len() >= 2 then
            -- CAN FD escape: 0x00 <SF_DL>
            dl = tvb:range(1, 1):uint()
            subtree:add(f_sf_dl, tvb:range(1, 1), dl):append_text(" (CAN FD escape)")
            if tvb:len() > 2 then
                local payload = tvb:range(2)
                subtree:add(f_isotp_data, payload)
                dissect_uds(payload:tvb(), pinfo, subtree)
            end
        else
            dl = bit.band(b0, 0x0F)
            subtree:add(f_sf_dl, tvb:range(0, 1), dl)
            if tvb:len() > 1 then
                local payload = tvb:range(1)
                subtree:add(f_isotp_data, payload)
                dissect_uds(payload:tvb(), pinfo, subtree)
            end
        end
        pinfo.cols.info:set(string.format("SF len=%d", dl))

    elseif pci == 0x1 then
        -- First Frame: upper nibble of b0 is 0x1, lower nibble + b1 = total length
        if tvb:len() < 2 then return end
        local ff_dl = bit.lshift(bit.band(b0, 0x0F), 8) + tvb:range(1, 1):uint()
        subtree:add(f_ff_dl, tvb:range(0, 2), ff_dl)
        if tvb:len() > 2 then
            subtree:add(f_isotp_data, tvb:range(2))
        end
        pinfo.cols.info:set(string.format("FF total_len=%d", ff_dl))

    elseif pci == 0x2 then
        -- Consecutive Frame
        local sn = bit.band(b0, 0x0F)
        subtree:add(f_cf_sn, tvb:range(0, 1), sn)
        if tvb:len() > 1 then
            subtree:add(f_isotp_data, tvb:range(1))
        end
        pinfo.cols.info:set(string.format("CF sn=%d", sn))

    elseif pci == 0x3 then
        -- Flow Control
        local fs   = bit.band(b0, 0x0F)
        subtree:add(f_fc_fs,  tvb:range(0, 1), fs)
        subtree:add(f_fc_fsn, tvb:range(0, 1), ISOTP_FC_STATUS[fs] or "reserved")
        if tvb:len() >= 2 then subtree:add(f_fc_bs,    tvb:range(1, 1)) end
        if tvb:len() >= 3 then subtree:add(f_fc_stmin, tvb:range(2, 1)) end
        pinfo.cols.info:set(string.format("FC fs=%s", ISOTP_FC_STATUS[fs] or "?"))
    end
end

-- ─── DoIP dissector ───────────────────────────────────────────────────────────

local doip_proto     = Proto("eds_doip",    "EDS DoIP (ISO 13400-2)")
local f_doip_ver     = ProtoField.uint8 ("eds_doip.version",    "Protocol Version",  base.HEX)
local f_doip_iver    = ProtoField.uint8 ("eds_doip.inv_version","Inverse Version",   base.HEX)
local f_doip_ptype   = ProtoField.uint16("eds_doip.payload_type","Payload Type",     base.HEX)
local f_doip_pname   = ProtoField.string("eds_doip.payload_name","Payload Type Name")
local f_doip_plen    = ProtoField.uint32("eds_doip.payload_len", "Payload Length",   base.DEC)
local f_doip_src     = ProtoField.uint16("eds_doip.src_addr",    "Source Address",   base.HEX)
local f_doip_tgt     = ProtoField.uint16("eds_doip.tgt_addr",    "Target Address",   base.HEX)
local f_doip_ack     = ProtoField.uint8 ("eds_doip.ack_code",    "Ack Code",         base.HEX)
local f_doip_nack    = ProtoField.uint8 ("eds_doip.nack_code",   "Nack Code",        base.HEX)
local f_doip_acttype = ProtoField.uint8 ("eds_doip.act_type",    "Activation Type",  base.HEX)
local f_doip_data    = ProtoField.bytes ("eds_doip.data",        "Payload")
doip_proto.fields    = {
    f_doip_ver, f_doip_iver, f_doip_ptype, f_doip_pname, f_doip_plen,
    f_doip_src, f_doip_tgt, f_doip_ack, f_doip_nack, f_doip_acttype, f_doip_data,
}

local function dissect_doip(tvb, pinfo, tree)
    if tvb:len() < 8 then return end
    local subtree  = tree:add(doip_proto, tvb(), "DoIP")
    local ptype    = tvb:range(2, 2):uint()
    local plen     = tvb:range(4, 4):uint()
    local ptype_nm = DOIP_PAYLOAD_TYPES[ptype] or string.format("unknown (0x%04X)", ptype)

    subtree:add(f_doip_ver,   tvb:range(0, 1))
    subtree:add(f_doip_iver,  tvb:range(1, 1))
    subtree:add(f_doip_ptype, tvb:range(2, 2)):append_text(" [" .. ptype_nm .. "]")
    subtree:add(f_doip_pname, tvb:range(2, 2), ptype_nm)
    subtree:add(f_doip_plen,  tvb:range(4, 4))
    pinfo.cols.protocol:set("EDS-DoIP")
    pinfo.cols.info:set(ptype_nm)

    if tvb:len() <= 8 then return end
    local payload = tvb:range(8)

    if ptype == 0x8001 then
        -- DiagnosticMessage: [src 2B][tgt 2B][UDS...]
        if payload:len() < 4 then return end
        subtree:add(f_doip_src, payload:range(0, 2))
        subtree:add(f_doip_tgt, payload:range(2, 2))
        if payload:len() > 4 then
            local uds_tvb = payload:range(4):tvb()
            dissect_uds(uds_tvb, pinfo, subtree)
        end

    elseif ptype == 0x8002 then
        -- DiagnosticMessagePositiveAck: [src 2B][tgt 2B][ack 1B]
        if payload:len() < 4 then return end
        subtree:add(f_doip_src, payload:range(0, 2))
        subtree:add(f_doip_tgt, payload:range(2, 2))
        if payload:len() >= 5 then subtree:add(f_doip_ack, payload:range(4, 1)) end

    elseif ptype == 0x8003 then
        -- DiagnosticMessageNegativeAck: [src 2B][tgt 2B][nack 1B]
        if payload:len() < 4 then return end
        subtree:add(f_doip_src, payload:range(0, 2))
        subtree:add(f_doip_tgt, payload:range(2, 2))
        if payload:len() >= 5 then subtree:add(f_doip_nack, payload:range(4, 1)) end

    elseif ptype == 0x0005 then
        -- RoutingActivationRequest: [src 2B][activation_type 1B][reserved 4B]
        if payload:len() < 1 then return end
        subtree:add(f_doip_src,     payload:range(0, 2))
        if payload:len() >= 3 then
            subtree:add(f_doip_acttype, payload:range(2, 1))
        end

    else
        subtree:add(f_doip_data, payload)
    end
end

function doip_proto.dissector(tvb, pinfo, tree)
    dissect_doip(tvb, pinfo, tree)
end

function isotp_proto.dissector(tvb, pinfo, tree)
    dissect_isotp(tvb, pinfo, tree)
end

-- ─── Registration ─────────────────────────────────────────────────────────────

-- DoIP: automatic on TCP/UDP port 13400
local tcp_table = DissectorTable.get("tcp.port")
tcp_table:add(13400, doip_proto)
local udp_table = DissectorTable.get("udp.port")
udp_table:add(13400, doip_proto)

-- ISO-TP: register for Decode As on SocketCAN captures
--   Analyze → Decode As → eds_isotp
local can_table = DissectorTable.get("can.subdissector")
if can_table then
    can_table:add_for_decode_as(isotp_proto)
end
