// Repairs analysis around the __savegprlr_N / __restgprlr_N helpers: makes
// them returning, inline functions, disassembles the code after every call
// to them (cut off by a wrong non-returning guess), and fixes the containing
// function bodies. Headless args: <save_base_hex> <rest_base_hex>
// (addresses of __savegprlr_14 and __restgprlr_14).
// @category Analysis

import java.util.HashSet;
import java.util.Set;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

public class FixSaveRestGpr extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    long saveBase;
    long restBase;
    if (args.length >= 2) {
      saveBase = Long.parseUnsignedLong(args[0], 16);
      restBase = Long.parseUnsignedLong(args[1], 16);
    } else {
      // std r14,-0x98(r1); std r15,-0x90(r1) starts __savegprlr_14, and
      // ld r14,-0x98(r1); ld r15,-0x90(r1) starts __restgprlr_14.
      Address save = findBytes(currentProgram.getMinAddress(),
          "\\xf9\\xc1\\xff\\x68\\xf9\\xe1\\xff\\x70");
      Address rest = findBytes(currentProgram.getMinAddress(),
          "\\xe9\\xc1\\xff\\x68\\xe9\\xe1\\xff\\x70");
      if (save == null || rest == null) {
        println("save/restore helpers not found");
        return;
      }
      saveBase = save.getOffset();
      restBase = rest.getOffset();
      println("found __savegprlr_14 at " + save + ", __restgprlr_14 at "
          + rest);
    }
    Set<Address> callSites = new HashSet<>();
    for (int i = 0; i < 18; i++) {
      fixHelper(toAddr(saveBase + 4 * i), "__savegprlr_" + (14 + i),
          callSites);
      fixHelper(toAddr(restBase + 4 * i), "__restgprlr_" + (14 + i),
          callSites);
    }
    int disassembled = 0;
    Set<Function> touched = new HashSet<>();
    for (Address site : callSites) {
      Instruction insn = getInstructionAt(site);
      if (insn == null) {
        continue;
      }
      // The non-returning guess left CALL_TERMINATOR overrides behind.
      if (insn.getFlowOverride() != FlowOverride.NONE) {
        insn.setFlowOverride(FlowOverride.NONE);
      }
      if (insn.isFallThroughOverridden()) {
        insn.clearFallThroughOverride();
      }
      if (!insn.getFlowType().isCall()) {
        continue;
      }
      Address next = site.add(4);
      if (getInstructionAt(next) == null && disassemble(next)) {
        disassembled++;
      }
      Function f = getFunctionContaining(site);
      if (f != null) {
        touched.add(f);
      }
    }
    for (Function f : touched) {
      CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
    }
    println("call sites " + callSites.size() + ", disassembled "
        + disassembled + ", functions fixed " + touched.size());
  }

  private void fixHelper(Address entry, String name, Set<Address> callSites)
      throws Exception {
    for (Reference ref : getReferencesTo(entry)) {
      callSites.add(ref.getFromAddress());
    }
    // The helpers are entry points into one run of code and Ghidra bodies
    // can't overlap, so each gets a one-instruction body.
    Function containing = getFunctionContaining(entry);
    if (containing != null) {
      removeFunction(containing);
    }
    if (getInstructionAt(entry) == null) {
      disassemble(entry);
    }
    Function f = currentProgram.getFunctionManager().createFunction(name,
        entry, new AddressSet(entry, entry.add(3)), SourceType.USER_DEFINED);
    f.setNoReturn(false);
  }
}
