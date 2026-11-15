// TryAt.java - debugging helper: try to create a function at each address given as script args, print bytes and the result.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class TryAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String s : getScriptArgs()) {
            Address a = toAddr(Long.decode(s));
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 16; i++) sb.append(String.format("%02X ", getByte(a.add(i)) & 0xFF));
            Instruction ins = getInstructionAt(a);
            Instruction cont = getInstructionContaining(a);
            Function f = getFunctionContaining(a);
            println(s + ": bytes " + sb + "| insn@ " + ins + " | insnContaining " + cont + " | funcContaining " + (f == null ? "null" : f.getName() + "@" + f.getEntryPoint()));
            if (f == null || !f.getEntryPoint().equals(a)) {
                boolean d = disassemble(a);
                Function nf = createFunction(a, null);
                println("   disassemble=" + d + " createFunction=" + (nf == null ? "null" : nf.getName() + " body " + nf.getBody().getNumAddresses()));
            }
        }
    }
}
