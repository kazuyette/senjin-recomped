// ClassApiMap.java - Senjin Aleste: C++ classes (RTTI) -> virtual methods -> Windows APIs used.
// Outputs in <output>:
//   classes.csv:  class;vftable;n_methods
//   methods.csv:  class;index;function;size;direct_apis;depth2_apis
//   imports.csv:  dll;api;n_functions;classes (calling functions attached to a class when possible)
// Usage: -postScript ClassApiMap.java <output>
// @category SenjinAleste
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ClassApiMap extends GhidraScript {
    Map<Function, Set<String>> directCache = new HashMap<>();

    Set<String> directApis(Function f) {
        Set<String> s = directCache.get(f);
        if (s != null) return s;
        s = new TreeSet<>();
        for (Function c : f.getCalledFunctions(monitor)) {
            Function t = c.isThunk() ? c.getThunkedFunction(true) : c;
            if (t != null && t.isExternal()) {
                ExternalLocation el = t.getExternalLocation();
                s.add((el != null ? el.getLibraryName() : "?") + "!" + t.getName());
            }
        }
        // indirect calls via IAT: CALL [addr] where addr is an external reference
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            if (!ins.getFlowType().isCall()) continue;
            for (Reference r : ins.getReferencesFrom()) {
                Symbol sym = getSymbolAt(r.getToAddress());
                if (sym != null && sym.isExternal()) s.add(sym.getParentNamespace().getName() + "!" + sym.getName());
                else if (sym != null) {
                    for (Reference r2 : getReferencesFrom(r.getToAddress())) {
                        Symbol s2 = getSymbolAt(r2.getToAddress());
                        if (s2 != null && s2.isExternal()) s.add(s2.getParentNamespace().getName() + "!" + s2.getName());
                    }
                }
            }
        }
        directCache.put(f, s);
        return s;
    }

    public void run() throws Exception {
        File out = new File(getScriptArgs()[0]); out.mkdirs();
        Listing lst = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        PrintWriter cp = new PrintWriter(new File(out, "classes.csv"), "UTF-8");
        PrintWriter mp = new PrintWriter(new File(out, "methods.csv"), "UTF-8");
        cp.println("class;vftable;n_methods");
        mp.println("class;index;function;size;direct_apis;depth2_apis");
        Map<Function, String> owner = new HashMap<>();
        SymbolIterator it = currentProgram.getSymbolTable().getSymbols("vftable");
        List<Symbol> vts = new ArrayList<>();
        while (it.hasNext()) vts.add(it.next());
        vts.sort(Comparator.comparing(s -> s.getParentNamespace().getName(true)));
        for (Symbol vt : vts) {
            String cls = vt.getParentNamespace().getName(true);
            Address a = vt.getAddress(); int n = 0;
            List<Function> ms = new ArrayList<>();
            while (n < 400) {
                Address p;
                try { p = toAddr(mem.getInt(a.add(n * 4L)) & 0xffffffffL); } catch (Exception e) { break; }
                if (n > 0 && getSymbolAt(a.add(n * 4L)) != null && getSymbolAt(a.add(n * 4L)).getName().equals("vftable")) break;
                Function f = getFunctionAt(p);
                if (f == null) break;
                ms.add(f); n++;
            }
            cp.println(cls + ";" + a + ";" + ms.size());
            for (int i = 0; i < ms.size(); i++) {
                Function f = ms.get(i);
                owner.putIfAbsent(f, cls);
                Set<String> d = directApis(f);
                Set<String> d2 = new TreeSet<>();
                for (Function c : f.getCalledFunctions(monitor)) if (!c.isExternal() && !c.isThunk()) d2.addAll(directApis(c));
                d2.removeAll(d);
                mp.println(cls + ";" + i + ";" + f.getEntryPoint() + ";" + f.getBody().getNumAddresses() + ";" + String.join(" ", d) + ";" + String.join(" ", d2));
            }
        }
        cp.close(); mp.close();
        // imports -> calling functions -> class
        Map<String, Set<String>> imp = new TreeMap<>(); Map<String, Integer> cnt = new TreeMap<>();
        for (Function f : lst.getFunctions(true)) {
            for (String api : directApis(f)) {
                cnt.merge(api, 1, Integer::sum);
                String o = owner.get(f);
                if (o == null) for (Function caller : f.getCallingFunctions(monitor)) { o = owner.get(caller); if (o != null) { o = o + "(via)"; break; } }
                imp.computeIfAbsent(api, k -> new TreeSet<>()).add(o != null ? o : "-");
            }
        }
        PrintWriter ip = new PrintWriter(new File(out, "imports.csv"), "UTF-8");
        ip.println("dll;api;n_functions;classes");
        for (String api : imp.keySet()) {
            String[] dl = api.split("!", 2);
            ip.println(dl[0] + ";" + dl[1] + ";" + cnt.get(api) + ";" + String.join(" ", imp.get(api)));
        }
        ip.close();
        println("ClassApiMap: " + vts.size() + " vftables, " + imp.size() + " API -> " + out);
    }
}
