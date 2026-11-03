// Dump every computed jump Ghidra resolved (switch tables): one row per
// indirect jmp with its target list. The lifter turns these into
// `switch` dispatch inside the owning function. Unresolved computed jumps
// are listed with no targets; the lifter skips them (they are IAT thunks
// and tail jumps).
//
// Headless:
//   bash /usr/share/ghidra/support/analyzeHeadless <projdir> <project> \
//     -process <game.exe> -noanalysis -readOnly \
//     -scriptPath tools/ghidra -postScript ExportJumpTables.java <out.tsv>
//@category Recomp

import java.io.PrintWriter;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

public class ExportJumpTables extends GhidraScript {
  @Override
  protected void run() throws Exception {
    String[] args = getScriptArgs();
    String out = args.length > 0 ? args[0] : "jumptables.tsv";
    int resolved = 0, unresolved = 0;
    try (PrintWriter w = new PrintWriter(out)) {
      w.println("jmp\ttargets");
      InstructionIterator it = currentProgram.getListing().getInstructions(true);
      while (it.hasNext() && !monitor.isCancelled()) {
        Instruction insn = it.next();
        FlowType ft = insn.getFlowType();
        if (!ft.isJump() || !ft.isComputed()) continue;
        TreeSet<Long> targets = new TreeSet<>();
        for (Reference r : insn.getReferencesFrom()) {
          if (r.getReferenceType().isJump() && r.getToAddress().isMemoryAddress())
            targets.add(r.getToAddress().getOffset());
        }
        StringBuilder sb = new StringBuilder();
        for (Long t : targets) {
          if (sb.length() > 0) sb.append(',');
          sb.append(String.format("%08X", t));
        }
        if (targets.isEmpty()) unresolved++; else resolved++;
        w.printf("%08X\t%s%n", insn.getAddress().getOffset(), sb);
      }
    }
    println("ExportJumpTables: " + resolved + " resolved, " + unresolved +
        " unresolved -> " + out);
  }
}
