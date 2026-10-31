// ExportRecompInfo.java - kit recomp_x86 : exporte ce dont le générateur x86 -> C a besoin.
// Sortie <fichier.json> :
//   functions : [{ "entry": "0x401000", "name": "...", "ranges": [["0x401000","0x40107f"], ...] }]
//   jumps     : { "0x401234": ["0x401300", ...] }   cibles connues des sauts indirects (tables de switch)
//   imports   : { "0x5b83d8": "XINPUT1_3.DLL!Ordinal_2", ... }  emplacements d'IAT
// Usage : analyzeHeadless <projet> <nom> -process <exe> -noanalysis -readOnly
//         -scriptPath _kit/recomp_x86/ghidra -postScript ExportRecompInfo.java <sortie.json>
// @category Kit
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportRecompInfo extends GhidraScript {
    String h(Address a) { return "\"0x" + Long.toHexString(a.getOffset()) + "\""; }
    public void run() throws Exception {
        PrintWriter p = new PrintWriter(new File(getScriptArgs()[0]), "UTF-8");
        Listing lst = currentProgram.getListing();
        p.println("{\"functions\": [");
        boolean first = true;
        for (Function f : lst.getFunctions(true)) {
            if (f.isThunk() && f.getThunkedFunction(true) != null && f.getThunkedFunction(true).isExternal()) continue;
            StringBuilder sb = new StringBuilder();
            sb.append(first ? "" : ",\n").append("{\"entry\": ").append(h(f.getEntryPoint()))
              .append(", \"name\": \"").append(f.getName().replace("\"", "'")).append("\", \"ranges\": [");
            boolean fr = true;
            for (AddressRange r : f.getBody()) { sb.append(fr ? "" : ", ").append("[").append(h(r.getMinAddress())).append(", ").append(h(r.getMaxAddress())).append("]"); fr = false; }
            sb.append("]}");
            p.print(sb); first = false;
        }
        p.println("],\n\"jumps\": {");
        first = true;
        InstructionIterator ii = lst.getInstructions(true);
        while (ii.hasNext()) {
            Instruction ins = ii.next();
            if (!ins.getFlowType().isJump() || !ins.getFlowType().isComputed()) continue;
            List<String> t = new ArrayList<>();
            for (Reference r : ins.getReferencesFrom()) if (r.getReferenceType().isFlow()) t.add(h(r.getToAddress()));
            if (t.isEmpty()) continue;
            p.print((first ? "" : ",\n") + h(ins.getAddress()) + ": [" + String.join(", ", t) + "]"); first = false;
        }
        p.println("},\n\"imports\": {");
        first = true;
        for (Symbol s : currentProgram.getSymbolTable().getExternalSymbols()) {
            for (Reference r : getReferencesTo(s.getAddress())) {
                if (r.getReferenceType() != RefType.DATA) continue;
                String lib = s.getParentNamespace().getName();
                p.print((first ? "" : ",\n") + h(r.getFromAddress()) + ": \"" + lib + "!" + s.getName() + "\""); first = false;
            }
        }
        p.println("}}");
        p.close();
        println("ExportRecompInfo : écrit " + getScriptArgs()[0]);
    }
}
