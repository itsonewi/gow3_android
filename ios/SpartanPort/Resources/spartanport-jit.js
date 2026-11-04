// SpartanPort JIT script for StikDebug / StikJIT.
// Ported from Madeira (https://github.com/willfaust/Madeira,
// app/Madeira/madeira-jit.js, GPL-3.0-or-later). Protocol and behavior are
// unchanged; only the log prefix differs.
//
// Handles BRK #0xf00d (universal protocol) with x16-based command dispatch:
//   x16 = 0  -> detach
//   x16 = 1  -> prepare region (x0 = address or 0, x1 = length)
//   x16 = 3  -> map page zero (legacy Wine path, unused here)
// and the legacy BRK #0x69.

function littleEndianHexStringToNumber(hexStr) {
    const bytes = [];
    for (let i = 0; i < hexStr.length; i += 2) {
        bytes.push(parseInt(hexStr.substr(i, 2), 16));
    }
    let num = 0n;
    for (let i = 7; i >= 0; i--) {
        num = (num << 8n) | BigInt(bytes[i] || 0);
    }
    return num;
}

function numberToLittleEndianHexString(num) {
    const bytes = [];
    for (let i = 0; i < 8; i++) {
        bytes.push(Number(num & 0xFFn));
        num >>= 8n;
    }
    return bytes.map(b => b.toString(16).padStart(2, '0')).join('');
}

function littleEndianHexToU32(hexStr) {
    return parseInt(hexStr.match(/../g).reverse().join(''), 16);
}

function extractBrkImmediate(u32) {
    return (u32 >> 5) & 0xFFFF;
}

let pid = get_pid();
log(`SpartanPort JIT: pid = ${pid}`);
let attachResponse = send_command(`vAttach;${pid.toString(16)}`);
log(`SpartanPort JIT: attached = ${attachResponse}`);

// Only service BRK. Everything else either goes to the app's own Mach handles
// or is delivered back as a normal unix signal. On StikDebug every stop costs
// synchronous round-trips; Wine-style workloads can otherwise trip the
// scene-update watchdog. Harmless if the stub does not support these packets.
{
    let ign = send_command(`QSetIgnoredExceptions:EXC_BAD_ACCESS;EXC_BAD_INSTRUCTION`);
    log(`SpartanPort JIT: QSetIgnoredExceptions -> ${ign || '(unsupported)'}`);
    let sigs = [];
    for (let s = 1; s <= 31; s++) if (s !== 5) sigs.push(s.toString(16));
    let pass = send_command(`QPassSignals:${sigs.join(';')}`);
    log(`SpartanPort JIT: QPassSignals -> ${pass || '(unsupported)'}`);
}

let detached = false;
let pending = null;        // stop packet returned by a continue we already sent
let lastFaultKey = null;   // "tid:pc" of the last non-BRK stop
let faultRepeats = 0;
let faultLogs = 0;
let sigLogs = 0;
// Each log() drives a UI update in StikDebug; keep the budget small.
let logBudget = 40;
function ulog(msg) { if (logBudget > 0) { logBudget--; log(msg); } }

function looksLikeStop(resp) {
    return typeof resp === 'string' && /^[TSWX]/.test(resp);
}

// Forward a unix signal to the stopped thread and remember the next stop.
function forwardSignal(sig, tid) {
    let sigHex = sig.toString(16).padStart(2, '0');
    let resp = send_command(`vCont;C${sigHex}:${tid};c`);
    if (!looksLikeStop(resp)) {
        resp = send_command(`C${sigHex}`);
    }
    if (looksLikeStop(resp)) {
        pending = resp;
        return true;
    }
    return false;
}

while (!detached) {
    let brkResponse = pending !== null ? pending : send_command(`c`);
    pending = null;

    // W/X = inferior exited; nothing left to debug.
    if (typeof brkResponse === 'string' && /^[WX]/.test(brkResponse)) {
        ulog(`SpartanPort JIT: inferior exited (${brkResponse})`);
        detached = true;
        continue;
    }

    let tidMatch = /T[0-9a-f]+thread:(?<tid>[0-9a-f]+);/.exec(brkResponse);
    let tid = tidMatch ? tidMatch.groups['tid'] : null;
    let pcMatch = /20:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    let pc = pcMatch ? pcMatch.groups['reg'] : null;

    if (!tid || !pc) {
        ulog(`SpartanPort JIT: failed to parse stop, continuing`);
        continue;
    }

    let pcNum = littleEndianHexStringToNumber(pc);

    let metypeMatch = /metype:([0-9a-f]+);/.exec(brkResponse);
    let metype = metypeMatch ? parseInt(metypeMatch[1], 16) : 0;
    let medata = [];
    let mre = /medata:([0-9a-fx]+);/g, mm;
    while ((mm = mre.exec(brkResponse)) !== null) medata.push(parseInt(mm[1], 16));

    // EXC_SOFTWARE / EXC_SOFT_SIGNAL: the kernel is routing a unix signal
    // through the debugger. Forward the ORIGINAL signo untouched.
    if (metype === 5) {
        let signo = (medata.length > 1 && medata[1] >= 1 && medata[1] <= 31) ? medata[1] : 0;
        if (sigLogs < 8 || (sigLogs % 500) === 0) {
            ulog(`SpartanPort JIT: soft-signal tid=${tid} pc=0x${pcNum.toString(16)} ` +
                `signo=${signo || '?'} (#${sigLogs})`);
        }
        sigLogs++;
        if (signo === 0 || !forwardSignal(signo, tid)) {
            let resp = send_command(`c`);
            if (looksLikeStop(resp)) pending = resp;
        }
        continue;
    }

    let instrHex = send_command(`m${pcNum.toString(16)},4`);
    let insnOk = typeof instrHex === 'string' && /^[0-9a-fA-F]{8}$/.test(instrHex);
    let instrU32 = insnOk ? littleEndianHexToU32(instrHex) : 0;
    // BRK #imm16 = 1101 0100 001 imm16 00000
    let isBrk = insnOk && ((instrU32 & 0xFFE0001F) >>> 0) === 0xD4200000;

    if (!isBrk) {
        // A raw fault stop escalated past the app's Mach handler. Never skip
        // it; deliver it back to the process as a unix signal.
        let key = `${tid}:${pc}`;
        faultRepeats = (key === lastFaultKey) ? faultRepeats + 1 : 1;
        lastFaultKey = key;

        let kcode = medata.length > 0 ? medata[0] : 0;

        // EXC_RESOURCE is a task-level advisory, not a thread fault.
        if (metype === 11) {
            ulog(`SpartanPort JIT: EXC_RESOURCE tid=${tid} kcode=${kcode.toString(16)} -- continuing, no signal`);
            let resp = send_command(`c`);
            if (looksLikeStop(resp)) pending = resp;
            continue;
        }

        let sig = 11;                                  // SIGSEGV default
        if (metype === 1) sig = (kcode === 1) ? 11 : 10; // BAD_ACCESS: INVALID -> SEGV, PROT -> BUS
        else if (metype === 2) sig = 4;                // BAD_INSTRUCTION -> SIGILL
        else if (metype === 3) sig = 8;                // ARITHMETIC -> SIGFPE
        else if (metype === 6) sig = 5;                // BREAKPOINT (non-BRK) -> SIGTRAP

        if (faultLogs < 16) {
            faultLogs++;
            ulog(`SpartanPort JIT: fault (not BRK) tid=${tid} pc=0x${pcNum.toString(16)} ` +
                `insn=${insnOk ? instrU32.toString(16).padStart(8, '0') : `<${instrHex}>`} ` +
                `metype=${metype} kcode=${kcode.toString(16)} -> sig ${sig} (repeat ${faultRepeats})`);
        }

        // Never detach here: an unserviced task exception port parks threads.
        if (faultRepeats >= 8) {
            ulog(`SpartanPort JIT: fault at pc=0x${pcNum.toString(16)} undeliverable -- killing inferior`);
            send_command(`k`);
            detached = true;
            continue;
        }

        if (!forwardSignal(sig, tid)) {
            let resp = send_command(`c`);
            if (looksLikeStop(resp)) pending = resp;
        }
        continue;
    }

    // Genuine BRK from here on: the protocol path.
    lastFaultKey = null;
    faultRepeats = 0;

    let brkImm = extractBrkImmediate(instrU32);

    // Advance PC past the BRK so it cannot re-fire.
    let pcPlus4 = numberToLittleEndianHexString(pcNum + 4n);
    send_command(`P20=${pcPlus4};thread:${tid};`);

    let x16Match = /10:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    let x16 = x16Match ? x16Match.groups['reg'] : null;

    if ((brkImm !== 0xf00d && brkImm !== 0x69) || !x16) {
        // Unknown BRK: x0 = 0 so the app's SIGTRAP fallback sees a skip.
        send_command(`P0=${numberToLittleEndianHexString(0n)};thread:${tid};`);
        continue;
    }

    ulog(`SpartanPort JIT: BRK #0x${brkImm.toString(16)}`);

    let x0Match = /00:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    let x1Match = /01:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    let x0 = x0Match ? littleEndianHexStringToNumber(x0Match.groups['reg']) : 0n;
    let x1 = x1Match ? littleEndianHexStringToNumber(x1Match.groups['reg']) : 0n;
    let x16Num = littleEndianHexStringToNumber(x16);

    if (brkImm === 0xf00d) {
        ulog(`SpartanPort JIT: x16 = ${x16Num}`);

        if (x16Num === 0n) {
            // CMD_DETACH
            ulog(`SpartanPort JIT: detach`);
            send_command(`D`);
            detached = true;

        } else if (x16Num === 1n) {
            // CMD_PREPARE_REGION
            ulog(`SpartanPort JIT: prepare addr=0x${x0.toString(16)} size=0x${x1.toString(16)}`);

            let addr = x0;
            if (x0 === 0n && x1 !== 0n) {
                let allocResp = send_command(`_M${x1.toString(16)},rx`);
                if (allocResp && allocResp.length > 0) {
                    addr = BigInt(`0x${allocResp}`);
                    ulog(`SpartanPort JIT: allocated at 0x${addr.toString(16)}`);
                }
            }

            if (addr !== 0n && x1 !== 0n) {
                let prepResp = prepare_memory_region(addr, x1);
                ulog(`SpartanPort JIT: prepared = ${prepResp}`);
            }

            send_command(`P0=${numberToLittleEndianHexString(addr)};thread:${tid};`);

        } else if (x16Num === 3n) {
            // CMD_MAP_PAGE_ZERO (legacy; kept for protocol parity).
            ulog(`SpartanPort JIT: map page zero, TEB=0x${x0.toString(16)} size=0x${x1.toString(16)}`);

            let success = 0n;
            if (x0 !== 0n && x1 !== 0n) {
                let tebPage = x0 & ~0x3FFFn;
                let tebOff = x0 - tebPage;
                let tebData = send_command(`m${x0.toString(16)},100`);
                if (tebData && tebData.length > 0) {
                    let writeResp = send_command(`M${tebOff.toString(16)},${(tebData.length/2).toString(16)}:${tebData}`);
                    ulog(`SpartanPort JIT: write TEB to page0 offset 0x${tebOff.toString(16)}: ${writeResp}`);
                    if (writeResp === 'OK') {
                        success = 1n;
                    }
                }
            }
            send_command(`P0=${numberToLittleEndianHexString(success)};thread:${tid};`);
        }

    } else if (brkImm === 0x69) {
        // Legacy protocol.
        ulog(`SpartanPort JIT: legacy BRK 0x69, x0=0x${x0.toString(16)}`);
        if (x0 !== 0n) {
            prepare_memory_region(x0, x0);
        }
        send_command(`P0=${numberToLittleEndianHexString(x0)};thread:${tid};`);
    }
}
