// SplitBlock.java - preScript for headless: split the raw one-block image into executable code and non-executable data,
// so Ghidra's auto-analysis (function start search, call targets, tables) treats only the code as code.
// Usage: -preScript SplitBlock.java 0x56A000   (the address where the code ends; the image base is 0x400000)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class SplitBlock extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long codeEnd = (args.length > 0) ? Long.decode(args[0]) : 0x56A000L;
        Memory mem = currentProgram.getMemory();
        MemoryBlock b = mem.getBlocks()[0];
        Address at = toAddr(codeEnd);
        mem.split(b, at);
        MemoryBlock code = mem.getBlock(toAddr(0x401000));
        MemoryBlock data = mem.getBlock(at);
        code.setName(".text");
        code.setExecute(true); code.setWrite(false); code.setRead(true);
        data.setName(".data");
        data.setExecute(false); data.setWrite(true); data.setRead(true);
        println("split at 0x" + Long.toHexString(codeEnd) + ": .text exec, .data non-exec");
    }
}
