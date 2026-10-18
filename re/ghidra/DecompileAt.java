// Decompiles the functions containing the given addresses and writes the C
// output to a file. Headless args: <out-file> <addr> [addr...]
// @category Analysis

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    DecompInterface decomp = new DecompInterface();
    decomp.openProgram(currentProgram);
    try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
      for (int i = 1; i < args.length; i++) {
        Address addr = toAddr(Long.parseUnsignedLong(args[i], 16));
        Function f = getFunctionContaining(addr);
        if (f == null) {
          // Walk back to the nearest "mflr r12" (7D8802A6) prologue.
          Address start = addr;
          for (int back = 0; back < 0x4000; back += 4) {
            Address a = addr.subtract(back);
            if (getInt(a) == 0x7D8802A6) {
              start = a;
              break;
            }
          }
          if (getInstructionAt(start) == null) {
            disassemble(start);
          }
          f = createFunction(start, null);
        }
        if (f == null) {
          out.println("// no function at " + args[i]);
          continue;
        }
        DecompileResults res = decomp.decompileFunction(f, 120, monitor);
        out.println("// ===== " + args[i] + " in " + f.getName() + " @ "
            + f.getEntryPoint());
        if (res.decompileCompleted()) {
          out.println(res.getDecompiledFunction().getC());
        } else {
          out.println("// decompile failed: " + res.getErrorMessage());
        }
      }
    }
    decomp.dispose();
  }
}
