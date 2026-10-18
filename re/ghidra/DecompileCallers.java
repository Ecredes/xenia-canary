// Decompiles every function that references the given addresses (for
// example an import thunk). Headless args: <out-file> <addr> [addr...]
// @category Analysis

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompileCallers extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    DecompInterface decomp = new DecompInterface();
    decomp.openProgram(currentProgram);
    try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
      for (int i = 1; i < args.length; i++) {
        Address target = toAddr(Long.parseUnsignedLong(args[i], 16));
        Set<Function> callers = new LinkedHashSet<>();
        for (Reference ref : getReferencesTo(target)) {
          Function f = getFunctionContaining(ref.getFromAddress());
          out.println("// ref to " + args[i] + " from " + ref.getFromAddress()
              + " in " + (f == null ? "?" : f.getName()));
          if (f != null) {
            callers.add(f);
          }
        }
        for (Function f : callers) {
          DecompileResults res = decomp.decompileFunction(f, 120, monitor);
          out.println("// ===== caller " + f.getName() + " @ "
              + f.getEntryPoint());
          out.println(res.decompileCompleted()
              ? res.getDecompiledFunction().getC()
              : "// decompile failed: " + res.getErrorMessage());
        }
      }
    }
    decomp.dispose();
  }
}
