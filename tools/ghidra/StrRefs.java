// StrRefs.java - fonctions qui référencent une chaîne (recherche exacte, ASCII) ; décompile ces fonctions.
// Usage : -postScript StrRefs.java <sortie> <chaine> [<chaine>...]
// @category SenjinAleste
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class StrRefs extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs(); File out = new File(a[0]); out.mkdirs();
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        for (int i = 1; i < a.length; i++) {
            byte[] b = (a[i] + "\0").getBytes("ASCII");
            Address s = currentProgram.getMinAddress();
            while ((s = find(s, b)) != null) {
                println("chaine " + a[i] + " @ " + s);
                for (Reference r : getReferencesTo(s)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) continue;
                    println("  ref " + r.getFromAddress() + " dans " + f.getName());
                    File fo = new File(out, f.getEntryPoint() + ".c");
                    if (fo.exists()) continue;
                    DecompileResults res = d.decompileFunction(f, 120, monitor);
                    try (PrintWriter p = new PrintWriter(fo, "UTF-8")) { p.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// echec"); }
                }
                s = s.add(1);
            }
        }
    }
}
