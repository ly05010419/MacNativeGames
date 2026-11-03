// Dump Ghidra's function list for the lifter: one row per function with
// entry, body extent, chunk count and name. Body ranges with more than one
// chunk mean Ghidra saw non-contiguous code (shared tails, outlined blocks).
//
// Headless:
//   bash /usr/share/ghidra/support/analyzeHeadless <projdir> <project> \
//     -process <game.exe> -noanalysis -readOnly \
//     -scriptPath tools/ghidra -postScript ExportFunctions.java <out.tsv>
//@category Recomp

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportFunctions extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    String out = args.length > 0 ? args[0] : "functions.tsv";
    int total = 0, multiChunk = 0, thunks = 0;
    try (PrintWriter w = new PrintWriter(out)) {
      w.println("entry\tmin\tmax\tchunks\tthunk\tnoreturn\tcallconv\tname");
      FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
      while (it.hasNext() && !monitor.isCancelled()) {
        Function f = it.next();
        AddressSetView body = f.getBody();
        int chunks = 0;
        for (AddressRange r : body.getAddressRanges()) chunks++;
        total++;
        if (chunks > 1) multiChunk++;
        if (f.isThunk()) thunks++;
        w.printf("%08X\t%08X\t%08X\t%d\t%d\t%d\t%s\t%s%n",
            f.getEntryPoint().getOffset(), body.getMinAddress().getOffset(),
            body.getMaxAddress().getOffset(), chunks, f.isThunk() ? 1 : 0,
            f.hasNoReturn() ? 1 : 0, f.getCallingConventionName(),
            f.getName(true));
      }
    }
    println("ExportFunctions: " + total + " functions, " + multiChunk +
        " multi-chunk, " + thunks + " thunks -> " + out);
  }
}
