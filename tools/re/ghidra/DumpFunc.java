// DumpFunc.java - disassembly (address, raw bytes, text) + decompilation for the function containing each
// given CODE address ("f:<hex>" forces a function entry at exactly that address, "p:<hex>" = prologue scan-back).
// usage: -postScript DumpFunc.java <outdir> <addr>...   -> <outdir>/fn_<addr>.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DumpFunc extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    File out = new File(a[0]); out.mkdirs();
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      String arg = a[i];
      boolean exact = arg.startsWith("f:");
      String hex = exact ? arg.substring(2) : arg;
      Address ad = toAddr(Long.parseLong(hex, 16));
      Function f = getFunctionContaining(ad);
      if (exact && (f == null || !f.getEntryPoint().equals(ad))) {
        if (f != null) removeFunction(f);
        disassemble(ad); f = createFunction(ad, null);
      } else if (f == null) {
        Address s = ad;
        for (int k = 0; k < 0x1000; k++, s = s.subtract(1))
          if ((getByte(s) & 0xff) == 0x55 && (getByte(s.add(1)) & 0xff) == 0x8B && (getByte(s.add(2)) & 0xff) == 0xEC) break;
        disassemble(s); f = createFunction(s, null);
      }
      try (PrintWriter w = new PrintWriter(new File(out, "fn_" + hex + ".txt"))) {
        if (f == null) { w.println("no function"); continue; }
        w.println("// " + f.getName() + " entry " + f.getEntryPoint() + " body " + f.getBody().getMinAddress() + ".." + f.getBody().getMaxAddress());
        w.println("// callers:");
        int nc = 0; for (Reference r : getReferencesTo(f.getEntryPoint())) { if (nc++ < 6) w.println("//   " + r.getFromAddress() + " " + r.getReferenceType()); } w.println("//   total " + nc);
        w.println("// ---- disassembly");
        for (Instruction in : currentProgram.getListing().getInstructions(f.getBody(), true)) {
          StringBuilder b = new StringBuilder();
          for (byte x : in.getBytes()) b.append(String.format("%02x", x & 0xff));
          w.println(in.getAddress() + "  " + String.format("%-20s", b) + in);
        }
        w.println("// ---- decompiled");
        DecompileResults dr = di.decompileFunction(f, 180, monitor);
        w.println(dr.decompileCompleted() ? dr.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
  }
}
