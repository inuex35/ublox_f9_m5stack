"""Read (and optionally set) ZED-F9R configuration over USB with UBX CFG-VALGET / CFG-VALSET.

usage:
  python f9r_cfg.py COM3 backup OUT.json      read UART1, rate and all CFG-MSGOUT keys from the flash and RAM layers
  python f9r_cfg.py COM3 apply                apply the logging configuration below to RAM + BBR + flash
"""
import json
import sys
import time

from serial import Serial
from pyubx2 import POLL_LAYER_FLASH, POLL_LAYER_RAM, SET_LAYER_BBR, SET_LAYER_FLASH, SET_LAYER_RAM, TXN_NONE, UBXMessage, UBXReader

PORT, CMD = sys.argv[1], sys.argv[2]

# Everything the logger needs on UART1 (to the M5Stack). Rates are per navigation epoch / per sensor sample.
LOG_CONFIG = {
    "CFG_UART1_BAUDRATE": 921600,
    "CFG_UART1INPROT_UBX": 1, "CFG_UART1INPROT_RTCM3X": 1, "CFG_UART1INPROT_NMEA": 0,
    "CFG_UART1OUTPROT_UBX": 1, "CFG_UART1OUTPROT_NMEA": 1,
    # raw GNSS for post-processing (RINEX / PPK)
    "CFG_MSGOUT_UBX_RXM_RAWX_UART1": 1,
    "CFG_MSGOUT_UBX_RXM_SFRBX_UART1": 1,
    # built-in IMU / sensor fusion
    "CFG_MSGOUT_UBX_ESF_RAW_UART1": 1,
    "CFG_MSGOUT_UBX_ESF_MEAS_UART1": 1,
    "CFG_MSGOUT_UBX_ESF_STATUS_UART1": 1,
    "CFG_MSGOUT_UBX_ESF_INS_UART1": 1,
    # receiver's own solution
    "CFG_MSGOUT_UBX_NAV_PVT_UART1": 1,
    "CFG_MSGOUT_UBX_NAV_PVAT_UART1": 1,       # position + velocity + vehicle attitude (fused), was already on
    # GGA for the NTRIP caster; the other NMEA sentences off on UART1
    "CFG_MSGOUT_NMEA_ID_GGA_UART1": 1,
    "CFG_MSGOUT_NMEA_ID_GLL_UART1": 0, "CFG_MSGOUT_NMEA_ID_GSA_UART1": 0, "CFG_MSGOUT_NMEA_ID_GSV_UART1": 0,
    "CFG_MSGOUT_NMEA_ID_RMC_UART1": 0, "CFG_MSGOUT_NMEA_ID_VTG_UART1": 0, "CFG_MSGOUT_NMEA_ID_THS_UART1": 0,
}


def transact(ser, msg, want, timeout=3.0):
    """Send msg, return the first parsed reply whose identity is in want (or None)."""
    ser.reset_input_buffer()
    ser.write(msg.serialize())
    rdr = UBXReader(ser, protfilter=2)
    t = time.time()
    while time.time() - t < timeout:
        try:
            _, parsed = rdr.read()
        except Exception:
            continue
        if parsed is not None and parsed.identity in want:
            return parsed
    return None


def valget(ser, layer, keys):
    out = {}
    for i in range(0, len(keys), 30):          # VALGET takes up to 64 keys; keep replies small
        chunk = keys[i:i + 30]
        if len(chunk) > 1:                     # a key unknown to this firmware NAKs the whole chunk: retry singly
            rep = transact(ser, UBXMessage.config_poll(layer, 0, chunk), {"CFG-VALGET", "ACK-NAK"})
            if rep is None or rep.identity != "CFG-VALGET":
                for k in chunk:
                    out.update(valget(ser, layer, [k]))
                continue
            for k in chunk:
                out[k] = getattr(rep, k, None)
            continue
        rep = transact(ser, UBXMessage.config_poll(layer, 0, chunk), {"CFG-VALGET", "ACK-NAK"})
        if rep is None or rep.identity != "CFG-VALGET":
            out.update({k: None for k in chunk})
            continue
        for k in chunk:
            out[k] = getattr(rep, k, None)
    return out


with Serial(PORT, 38400, timeout=0.2) as ser:
    if CMD == "backup":
        from pyubx2.ubxtypes_configdb import UBX_CONFIG_DATABASE
        keys = [k for k in UBX_CONFIG_DATABASE if k.startswith(("CFG_UART1", "CFG_I2C", "CFG_RATE", "CFG_MSGOUT"))]
        result = {"RAM": valget(ser, POLL_LAYER_RAM, keys), "FLASH": valget(ser, POLL_LAYER_FLASH, keys)}
        json.dump(result, open(sys.argv[3], "w"), indent=1)
        ram = result["RAM"]
        print("RAM: UART1 baud", ram.get("CFG_UART1_BAUDRATE"), "| meas rate ms", ram.get("CFG_RATE_MEAS"),
              "| nav rate", ram.get("CFG_RATE_NAV"))
        for port in ("_UART1", "_I2C"):
            on = sorted(k.replace("CFG_MSGOUT_", "") for k, v in ram.items() if k.startswith("CFG_MSGOUT") and k.endswith(port) and v)
            print(port[1:], "messages on:", on)
        print("flash layer keys stored:", sum(v is not None for v in result["FLASH"].values()))
    elif CMD in ("apply", "apply-i2c"):
        if CMD == "apply-i2c":                 # same message set on I2C (Qwiic), the port the M5Stack reads
            LOG_CONFIG = {k.replace("_UART1", "_I2C"): v for k, v in LOG_CONFIG.items() if k.startswith("CFG_MSGOUT")}
            LOG_CONFIG.update({"CFG_I2CINPROT_UBX": 1, "CFG_I2CINPROT_RTCM3X": 1, "CFG_I2CINPROT_NMEA": 0,
                               "CFG_I2COUTPROT_UBX": 1, "CFG_I2COUTPROT_NMEA": 1})
        items = list(LOG_CONFIG.items())
        layers = SET_LAYER_RAM | SET_LAYER_BBR | SET_LAYER_FLASH
        for i in range(0, len(items), 20):
            rep = transact(ser, UBXMessage.config_set(layers, TXN_NONE, items[i:i + 20]), {"ACK-ACK", "ACK-NAK"})
            print("VALSET", i, "->", rep.identity if rep else "no reply")
        check = valget(ser, POLL_LAYER_RAM, list(LOG_CONFIG))
        bad = {k: (check[k], v) for k, v in LOG_CONFIG.items() if check.get(k) != v}
        print("verified" if not bad else f"mismatch {bad}")
