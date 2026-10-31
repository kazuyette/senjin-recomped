// CallersOf.java - functions that call/reference a symbol (import or function); decompiles them.
// Usage: -postScript CallersOf.java <output> <symbol> [<symbol>...]
// @category SenjinAleste
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
public class CallersOf extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); File out = new File(a[0]); out.mkdirs();
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            for (Symbol s : currentProgram.getSymbolTable().getSymbols(a[i])) {
                println("symbol " + a[i] + " @ " + s.getAddress());
                java.util.List<Reference> refs = new java.util.ArrayList<>();
                for (Reference r : getReferencesTo(s.getAddress())) refs.add(r);
                // references to the IAT entry (pointer): follow them too
                for (Reference r : refs) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) { for (Reference r2 : getReferencesTo(r.getFromAddress())) { Function f2 = getFunctionContaining(r2.getFromAddress()); if (f2 != null) dump(d, out, f2, r2.getFromAddress().toString()); } continue; }
                    dump(d, out, f, r.getFromAddress().toString());
                }
            }
        }
    }
    void dump(DecompInterface d, File out, Function f, String from) throws Exception {
        println("  ref " + from + " in " + f.getName() + " @ " + f.getEntryPoint());
        File fo = new File(out, f.getEntryPoint() + ".c"); if (fo.exists()) return;
        DecompileResults res = d.decompileFunction(f, 120, monitor);
        try (PrintWriter p = new PrintWriter(fo, "UTF-8")) { p.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed"); }
    }
}
