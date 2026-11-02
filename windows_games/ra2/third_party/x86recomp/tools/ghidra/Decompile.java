// Decompile the functions containing the given addresses into one text
// file (for reading game code without the Ghidra GUI).
//
// Headless:
//   bash /usr/share/ghidra/support/analyzeHeadless <projdir> <project> \
//     -process <game.exe> -noanalysis -readOnly \
//     -scriptPath tools/ghidra -postScript Decompile.java <out.c> <hexva>...
//@category Recomp

import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    try (PrintWriter w = new PrintWriter(args[0])) {
      for (int i = 1; i < args.length; i++) {
        Address a = toAddr(Long.parseLong(args[i], 16));
        Function f = getFunctionContaining(a);
        if (f == null) {
          w.printf("// %s: no function%n%n", args[i]);
          continue;
        }
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        w.printf("// ==== %s @ %s ====%n", f.getName(true), f.getEntryPoint());
        w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC()
                                         : "// decompile failed: " + r.getErrorMessage());
      }
    }
  }
}
