// DecompAll.java - headless: find functions by following call targets in the code, then decompile EVERY function once.
//
// Usage (after the raw image is imported; script args: <outdir>):
//   analyzeHeadless <projdir> <proj> -process <file> -noanalysis -scriptPath <this dir> -postScript DecompAll.java <outdir>
//
// Output (kept OUTSIDE any repository - it is decompiled game code):
//   <outdir>/functions.csv   addr,size,name,callers
//   <outdir>/decomp_all.c    every function: "// ==== FUN_xxxxxxxx @ 0xADDR size N" + the decompiler's C
//   <outdir>/progress.txt    running counters (watch it while it works)
//
// Notes: save without a BOM; the first lines are plain ASCII. The image is one block (code and data), so the call-target pass only
// accepts E8 rel32 whose target lies inside the image and starts with a plausible prologue byte.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.*;
import java.util.*;

public class DecompAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = (args.length > 0) ? args[0] : "decomp_out";
        new File(out).mkdirs();
        Memory mem = currentProgram.getMemory();
        MemoryBlock[] blocks = mem.getBlocks();
        long lo = blocks[0].getStart().getOffset(), hi = blocks[0].getEnd().getOffset();
        for (MemoryBlock mb : blocks) { lo = Math.min(lo, mb.getStart().getOffset()); hi = Math.max(hi, mb.getEnd().getOffset()); }
        println("image " + Long.toHexString(lo) + " .. " + Long.toHexString(hi));

        // pass 1: call targets
        Set<Long> targets = new TreeSet<Long>();
        byte[] buf = new byte[(int) (hi - lo + 1)];
        for (MemoryBlock mb : blocks) {
            byte[] part = new byte[(int) mb.getSize()];
            mem.getBytes(mb.getStart(), part);
            System.arraycopy(part, 0, buf, (int) (mb.getStart().getOffset() - lo), part.length);
        }
        // the code section in this image: VA 0x1000 .. about 0x6FFFFF is code; the rest is data. Scan the first 0x700000 bytes.
        int scan = Math.min(buf.length - 5, 0x700000);
        for (int i = 0x1000; i < scan; i++) {
            if ((buf[i] & 0xFF) != 0xE8) continue;
            int rel = (buf[i + 1] & 0xFF) | ((buf[i + 2] & 0xFF) << 8) | ((buf[i + 3] & 0xFF) << 16) | ((buf[i + 4] & 0xFF) << 24);
            long t = lo + i + 5 + rel;
            if (t < lo + 0x1000 || t >= lo + 0x700000) continue;
            int b0 = buf[(int) (t - lo)] & 0xFF;
            // plausible starts: push ebp/esi/edi/ebx, sub esp, mov, xor, cmp, test, push imm, mov eax/ecx/edx, lea, inc, dec, ret-less thunks (jmp)
            if (b0 == 0x55 || b0 == 0x56 || b0 == 0x57 || b0 == 0x53 || b0 == 0x83 || b0 == 0x81 || b0 == 0x8B || b0 == 0x33 || b0 == 0x6A
                || b0 == 0x68 || b0 == 0xA1 || b0 == 0x8A || b0 == 0x0F || b0 == 0xE9 || b0 == 0x51 || b0 == 0x52 || b0 == 0x8D || b0 == 0xB8
                || b0 == 0x3B || b0 == 0x39 || b0 == 0x85 || b0 == 0x84 || b0 == 0x80 || b0 == 0x31 || b0 == 0x2B || b0 == 0xD9 || b0 == 0xDD)
                targets.add(t);
        }
        println("call targets: " + targets.size());
        // pass 1b: pointers stored in data tables (script built-in tables, vtables, callbacks): any dword in the data part that points
        // into the code and at a plausible prologue
        int ptrs = 0;
        for (int i = 0x540000; i < Math.min(buf.length - 4, 0x1400000); i += 4) {
            long v = (buf[i] & 0xFFL) | ((buf[i + 1] & 0xFFL) << 8) | ((buf[i + 2] & 0xFFL) << 16) | ((buf[i + 3] & 0xFFL) << 24);
            if (v < lo + 0x1000 || v >= lo + 0x540000) continue;
            int b0 = buf[(int) (v - lo)] & 0xFF;
            if (b0 == 0x55 || b0 == 0x56 || b0 == 0x57 || b0 == 0x53 || b0 == 0x83 || b0 == 0x81 || b0 == 0x8B || b0 == 0x33 || b0 == 0x6A
                || b0 == 0x68 || b0 == 0xA1 || b0 == 0x8A || b0 == 0x51 || b0 == 0x52 || b0 == 0x8D || b0 == 0xB8 || b0 == 0x3B || b0 == 0x85
                || b0 == 0x80 || b0 == 0x31 || b0 == 0xD9 || b0 == 0xDD || b0 == 0xE9 || b0 == 0x64 || b0 == 0xC7 || b0 == 0xC3) {
                if (targets.add(v)) ptrs++;
            }
        }
        println("pointer-table targets added: " + ptrs + " (total " + targets.size() + ")");
        int made = 0;
        List<long[]> splits = new ArrayList<long[]>();
        for (long t : targets) {
            if (monitor.isCancelled()) break;
            Address a = toAddr(t);
            if (getFunctionAt(a) != null) continue;
            Function c = getFunctionContaining(a);
            if (c != null) { splits.add(new long[] { t, c.getEntryPoint().getOffset() }); continue; }
            try {
                disassemble(a);
                if (createFunction(a, null) != null) made++;
            } catch (Exception e) { /* skip */ }
        }
        println("functions created: " + made + ", swallowed by another function: " + splits.size());
        int fixed = 0;
        for (long[] sp : splits) {
            try {
                Address a = toAddr(sp[0]);
                if (getFunctionAt(a) != null) continue;
                Function c = getFunctionContaining(a);
                if (c == null) { disassemble(a); if (createFunction(a, null) != null) fixed++; continue; }
                Address ce = c.getEntryPoint();
                removeFunction(c);
                disassemble(a);
                if (createFunction(a, null) != null) fixed++;
                if (getFunctionAt(ce) == null) createFunction(ce, null);
            } catch (Exception e) { /* skip */ }
        }
        println("swallowed functions split out: " + fixed);

        // pass 3: fill the holes between functions (code only: below 0x56A000). A hole is skipped over when it is padding; an address with a
        // plausible prologue gets a function. Functions reached only through indirect calls end up here.
        int gapMade = 0;
        long codeLo = lo + 0x1000, codeHi = lo + 0x16A000;
        long a3 = codeLo;
        while (a3 < codeHi && !monitor.isCancelled()) {
            Address ad = toAddr(a3);
            Function cf = getFunctionContaining(ad);
            if (cf != null) { a3 = cf.getBody().getMaxAddress().getOffset() + 1; continue; }
            int b0 = buf[(int) (a3 - lo)] & 0xFF;
            int b1 = buf[(int) (a3 - lo + 1)] & 0xFF;
            boolean pad = (b0 == 0xCC || b0 == 0x90 || b0 == 0x00);
            boolean start = !pad && (b0 == 0x55 || b0 == 0x56 || b0 == 0x57 || b0 == 0x53 || b0 == 0x51 || b0 == 0x52 || (b0 == 0x83 && b1 == 0xEC)
                || (b0 == 0x8B && b1 == 0x44) || b0 == 0x6A || b0 == 0x68 || (b0 == 0x81 && b1 == 0xEC) || b0 == 0xA1 || b0 == 0x33 || b0 == 0x8A
                || b0 == 0x64 || (b0 == 0x8B && (b1 == 0x4C || b1 == 0x54 || b1 == 0x5C || b1 == 0x74 || b1 == 0xFF || b1 == 0xEC)));
            if (start) {
                try {
                    if (disassemble(ad) && createFunction(ad, null) != null) {
                        gapMade++;
                        Function nf = getFunctionAt(ad);
                        a3 = (nf != null) ? nf.getBody().getMaxAddress().getOffset() + 1 : a3 + 1;
                        continue;
                    }
                } catch (Exception e) { /* skip */ }
            }
            a3++;
        }
        println("functions created by the gap fill: " + gapMade);

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        List<Function> fns = new ArrayList<Function>();
        while (it.hasNext()) fns.add(it.next());
        println("functions total: " + fns.size());

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter csv = new PrintWriter(new BufferedWriter(new FileWriter(out + "/functions.csv")));
        PrintWriter src = new PrintWriter(new BufferedWriter(new FileWriter(out + "/decomp_all.c")));
        csv.println("addr,size,name,callers");
        int n = 0, ok = 0, fail = 0;
        long t0 = System.currentTimeMillis();
        for (Function f : fns) {
            if (monitor.isCancelled()) break;
            n++;
            long size = f.getBody().getNumAddresses();
            int callers = f.getSymbol().getReferenceCount();
            csv.println("0x" + f.getEntryPoint().toString() + "," + size + "," + f.getName() + "," + callers);
            src.println("// ==== " + f.getName() + " @ 0x" + f.getEntryPoint().toString() + " size " + size);
            try {
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    src.println(r.getDecompiledFunction().getC());
                    ok++;
                } else {
                    src.println("// (decompile failed: " + (r == null ? "null" : r.getErrorMessage()) + ")");
                    fail++;
                }
            } catch (Exception e) {
                src.println("// (exception: " + e + ")");
                fail++;
            }
            if (n % 200 == 0) {
                PrintWriter p = new PrintWriter(new FileWriter(out + "/progress.txt"));
                p.println(n + " / " + fns.size() + " ok=" + ok + " fail=" + fail + " elapsed_s=" + (System.currentTimeMillis() - t0) / 1000);
                p.close();
                src.flush(); csv.flush();
            }
        }
        csv.close(); src.close();
        PrintWriter p = new PrintWriter(new FileWriter(out + "/progress.txt"));
        p.println("DONE " + n + " / " + fns.size() + " ok=" + ok + " fail=" + fail + " elapsed_s=" + (System.currentTimeMillis() - t0) / 1000);
        p.close();
        println("done: ok=" + ok + " fail=" + fail);
    }
}
