// DecompileList.java - decompiles a list of functions (hex addresses) into <output>/<address>.c
// Usage: -postScript DecompileList.java <output> 00521410 004dfc20 ...
// @category SenjinAleste
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DecompileList extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); File out = new File(a[0]); out.mkdirs();
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            Function f = getFunctionAt(toAddr(a[i])); if (f == null) { println("no function " + a[i]); continue; }
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            try (PrintWriter p = new PrintWriter(new File(out, a[i] + ".c"), "UTF-8")) {
                p.println("// " + f.getName() + " @ " + f.getEntryPoint());
                p.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
