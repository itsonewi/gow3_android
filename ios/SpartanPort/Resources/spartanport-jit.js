// SpartanPort JIT script for StikDebug.
//
// StikDebug attaches to the app, loads this script, and lets it service the
// debugger protocol. The app's JIT layer (Native/JITAllocator.c) raises
// BRK #0xf00d and expects this script to answer:
//
//   x16 = 0                       detach
//   x16 = 1, x0 = address, x1 = length
//        - x0 = 0: allocate `length` RX bytes with StikDebug's _M command;
//        - authorize the range with prepare_memory_region();
//        - return the address in x0.
//
// Everything else is left alone: BRKs that are not this protocol get x0 = 0 and
// the app's SIGTRAP fallback decides what to do, and non-BRK stops simply
// continue so the app's own signal handlers see them.
//
// The protocol is the one StikDebug/StikJIT define (the same one Madeira's
// script implements); this file is a small, independent implementation of it.

const BRK_OPCODE_MASK = 0xffe0001f;
const BRK_OPCODE = 0xd4200000;
const PROTOCOL_IMM = 0xf00d;

function leHexToBigInt(hex) {
    let value = 0n;
    for (let i = 7; i >= 0; i--) {
        const byte = parseInt(hex.substr(i * 2, 2), 16);
        value = (value << 8n) | BigInt(isNaN(byte) ? 0 : byte);
    }
    return value;
}

function bigIntToLeHex(value) {
    let out = "";
    for (let i = 0; i < 8; i++) {
        out += Number(value & 0xffn).toString(16).padStart(2, "0");
        value >>= 8n;
    }
    return out;
}

function register(stop, index) {
    const match = new RegExp(index.toString(16).padStart(2, "0") + ":([0-9a-f]{16});").exec(stop);
    return match ? leHexToBigInt(match[1]) : null;
}

function threadOf(stop) {
    const match = /thread:([0-9a-f]+);/.exec(stop);
    return match ? match[1] : null;
}

const pid = get_pid();
log(`SpartanPort JIT: attaching to pid ${pid}`);
const attach = send_command(`vAttach;${pid.toString(16)}`);
log(`SpartanPort JIT: attach -> ${attach}`);

let detached = false;
let logLines = 20;
function note(message) {
    if (logLines > 0) {
        logLines--;
        log(message);
    }
}

while (!detached) {
    const stop = send_command("c");
    if (typeof stop !== "string" || /^[WX]/.test(stop)) {
        log("SpartanPort JIT: inferior exited");
        break;
    }

    const tid = threadOf(stop);
    const pcText = /20:([0-9a-f]{16});/.exec(stop);
    if (!tid || !pcText) continue;

    const pc = leHexToBigInt(pcText[1]);

    // Read the instruction at the stop. Only BRK #0xf00d is ours.
    const raw = send_command(`m${pc.toString(16)},4`);
    if (typeof raw !== "string" || !/^[0-9a-fA-F]{8}$/.test(raw)) continue;
    const instruction = parseInt(raw.match(/../g).reverse().join(""), 16);
    const isBrk = ((instruction & BRK_OPCODE_MASK) >>> 0) === BRK_OPCODE;
    const immediate = (instruction >>> 5) & 0xffff;

    if (!isBrk || immediate !== PROTOCOL_IMM) {
        // Not the pool protocol. Continue; the app handles its own traps.
        continue;
    }

    const command = register(stop, 0x10); // x16
    const x0 = register(stop, 0x00);
    const x1 = register(stop, 0x01);

    // Step over the BRK before answering, so it cannot fire twice.
    send_command(`P20=${bigIntToLeHex(pc + 4n)};thread:${tid};`);

    if (command === 0n) {
        note("SpartanPort JIT: detach requested");
        send_command("D");
        detached = true;
        continue;
    }

    if (command !== 1n || x1 === null || x1 === 0n) {
        send_command(`P0=${bigIntToLeHex(0n)};thread:${tid};`);
        continue;
    }

    let address = x0 === null ? 0n : x0;
    if (address === 0n) {
        // Let the debugger allocate the RX run.
        const allocated = send_command(`_M${x1.toString(16)},rx`);
        if (typeof allocated === "string" && allocated.length > 0) {
            address = BigInt(`0x${allocated}`);
            note(`SpartanPort JIT: _M ${x1.toString(16)} -> 0x${address.toString(16)}`);
        }
    }

    if (address !== 0n) {
        const prepared = prepare_memory_region(address, x1);
        note(`SpartanPort JIT: prepare 0x${address.toString(16)}+0x${x1.toString(16)} -> ${prepared}`);
    }

    send_command(`P0=${bigIntToLeHex(address)};thread:${tid};`);
}
