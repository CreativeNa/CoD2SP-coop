// DecompRefs.java - for each address in the script args, decompile every function that references it
// (or contains it, if it is code) into <outdir>/<addr>.c.  usage: -postScript DecompRefs.java <outdir> <hexaddr>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DecompRefs extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    File out = new File(a[0]); out.mkdirs();
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      Address ad = toAddr(Long.parseLong(a[i], 16));
      Set<Function> fs = new LinkedHashSet<>();
      Function self = getFunctionContaining(ad);
      if (self != null) fs.add(self);
      for (Reference r : getReferencesTo(ad)) {
        Function f = getFunctionContaining(r.getFromAddress());
        if (f != null) fs.add(f);
      }
      try (PrintWriter w = new PrintWriter(new File(out, a[i] + ".c"))) {
        w.println("// target " + ad + " functions: " + fs.size());
        for (Function f : fs) {
          w.println("\n// ===== " + f.getName() + " @ " + f.getEntryPoint() + " callers:");
          for (Reference r : getReferencesTo(f.getEntryPoint())) w.println("//   from " + r.getFromAddress());
          DecompileResults dr = di.decompileFunction(f, 120, monitor);
          w.println(dr.decompileCompleted() ? dr.getDecompiledFunction().getC() : "// decompile failed");
        }
      }
    }
  }
}
