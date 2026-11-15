// DecompAt.java - decompile the function containing each given CODE address; if Ghidra has no function
// there (raw dump, little auto-analysis) scan back for a "push ebp; mov ebp,esp" prologue and create one.
// usage: -postScript DecompAt.java <outdir> <hexaddr>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    File out = new File(a[0]); out.mkdirs();
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      boolean exact = a[i].startsWith("f:");   // "f:<hex>" = known function entry, create exactly there
      Address ad = toAddr(Long.parseLong(exact ? a[i].substring(2) : a[i], 16));
      Function f = getFunctionContaining(ad);
      String how = "existing";
      if (exact && (f == null || !f.getEntryPoint().equals(ad))) {
        if (f != null) removeFunction(f);
        disassemble(ad);
        f = createFunction(ad, null);
        how = "created at exact entry";
      } else if (f == null) {
        Address s = ad;
        for (int k = 0; k < 0x1000; k++, s = s.subtract(1)) {
          if ((getByte(s) & 0xff) == 0x55 && (getByte(s.add(1)) & 0xff) == 0x8B && (getByte(s.add(2)) & 0xff) == 0xEC) break;
        }
        disassemble(s);
        f = createFunction(s, null);
        how = "created at prologue scan-back from " + ad;
      }
      try (PrintWriter w = new PrintWriter(new File(out, "at_" + a[i].replace(":", "_") + ".c"))) {
        if (f == null) { w.println("// no function for " + ad); continue; }
        w.println("// " + f.getName() + " @ " + f.getEntryPoint() + " (" + how + ") callers:");
        for (Reference r : getReferencesTo(f.getEntryPoint())) w.println("//   from " + r.getFromAddress());
        DecompileResults dr = di.decompileFunction(f, 180, monitor);
        w.println(dr.decompileCompleted() ? dr.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
  }
}
