// D3D9Map.java - Senjin Aleste : cartographie des appels COM Direct3D 9 dans l'exe.
// Repère les appels indirects "CALL [reg+off]" dont l'offset correspond à une méthode d'IDirect3DDevice9
// (ou IDirect3D9 / IDirect3DTexture9 pour quelques-unes), les regroupe par fonction, et exporte :
//   d3d9_calls.csv   (fonction, adresse d'appel, méthode)
//   d3d9_funcs.csv   (fonction, nb d'appels D3D par méthode) trié par nombre d'appels
//   functions.csv    (toutes les fonctions : adresse, taille, nom)
// Usage headless : analyzeHeadless <projet> <nom> -process senxin_aleste_apm3.exe -noanalysis
//                  -scriptPath tools/ghidra -postScript D3D9Map.java <dossier_sortie>
// @category SenjinAleste
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;

public class D3D9Map extends GhidraScript {
    static final String[] DEV = {"QueryInterface","AddRef","Release","TestCooperativeLevel","GetAvailableTextureMem","EvictManagedResources","GetDirect3D","GetDeviceCaps","GetDisplayMode","GetCreationParameters","SetCursorProperties","SetCursorPosition","ShowCursor","CreateAdditionalSwapChain","GetSwapChain","GetNumberOfSwapChains","Reset","Present","GetBackBuffer","GetRasterStatus","SetDialogBoxMode","SetGammaRamp","GetGammaRamp","CreateTexture","CreateVolumeTexture","CreateCubeTexture","CreateVertexBuffer","CreateIndexBuffer","CreateRenderTarget","CreateDepthStencilSurface","UpdateSurface","UpdateTexture","GetRenderTargetData","GetFrontBufferData","StretchRect","ColorFill","CreateOffscreenPlainSurface","SetRenderTarget","GetRenderTarget","SetDepthStencilSurface","GetDepthStencilSurface","BeginScene","EndScene","Clear","SetTransform","GetTransform","MultiplyTransform","SetViewport","GetViewport","SetMaterial","GetMaterial","SetLight","GetLight","LightEnable","GetLightEnable","SetClipPlane","GetClipPlane","SetRenderState","GetRenderState","CreateStateBlock","BeginStateBlock","EndStateBlock","SetClipStatus","GetClipStatus","GetTexture","SetTexture","GetTextureStageState","SetTextureStageState","GetSamplerState","SetSamplerState","ValidateDevice","SetPaletteEntries","GetPaletteEntries","SetCurrentTexturePalette","GetCurrentTexturePalette","SetScissorRect","GetScissorRect","SetSoftwareVertexProcessing","GetSoftwareVertexProcessing","SetNPatchMode","GetNPatchMode","DrawPrimitive","DrawIndexedPrimitive","DrawPrimitiveUP","DrawIndexedPrimitiveUP","ProcessVertices","CreateVertexDeclaration","SetVertexDeclaration","GetVertexDeclaration","SetFVF","GetFVF","CreateVertexShader","SetVertexShader","GetVertexShader","SetVertexShaderConstantF","GetVertexShaderConstantF","SetVertexShaderConstantI","GetVertexShaderConstantI","SetVertexShaderConstantB","GetVertexShaderConstantB","SetStreamSource","GetStreamSource","SetStreamSourceFreq","GetStreamSourceFreq","SetIndices","GetIndices","CreatePixelShader","SetPixelShader","GetPixelShader","SetPixelShaderConstantF","GetPixelShaderConstantF","SetPixelShaderConstantI","GetPixelShaderConstantI","SetPixelShaderConstantB","GetPixelShaderConstantB","DrawRectPatch","DrawTriPatch","DeletePatch","CreateQuery"};

    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : ".");
        out.mkdirs();
        Listing lst = currentProgram.getListing();
        PrintWriter calls = new PrintWriter(new File(out, "d3d9_calls.csv"), "UTF-8");
        PrintWriter funcs = new PrintWriter(new File(out, "functions.csv"), "UTF-8");
        calls.println("fonction;nom;appel;methode");
        funcs.println("adresse;taille;nom");
        Map<Function, Map<String,Integer>> per = new HashMap<>();
        int total = 0;
        for (Function f : lst.getFunctions(true)) {
            funcs.println(f.getEntryPoint() + ";" + f.getBody().getNumAddresses() + ";" + f.getName());
            for (Instruction ins : lst.getInstructions(f.getBody(), true)) {
                if (!ins.getFlowType().isCall() || !ins.getFlowType().isComputed()) continue;
                Object[] objs = ins.getOpObjects(0);
                boolean hasReg = false; long off = -1;
                for (Object o : objs) {
                    if (o instanceof Register) hasReg = true;
                    else if (o instanceof Scalar) off = ((Scalar) o).getUnsignedValue();
                }
                // "call dword ptr [reg]" = offset 0 (QueryInterface) : ignoré, trop ambigu
                if (!hasReg || off <= 0 || off % 4 != 0) continue;
                int idx = (int) (off / 4);
                if (idx < 3 || idx >= DEV.length) continue;
                String m = DEV[idx];
                calls.println(f.getEntryPoint() + ";" + f.getName() + ";" + ins.getAddress() + ";" + m);
                per.computeIfAbsent(f, k -> new TreeMap<>()).merge(m, 1, Integer::sum);
                total++;
            }
        }
        calls.close(); funcs.close();
        List<Map.Entry<Function, Map<String,Integer>>> l = new ArrayList<>(per.entrySet());
        l.sort((a, b) -> b.getValue().values().stream().mapToInt(i -> i).sum() - a.getValue().values().stream().mapToInt(i -> i).sum());
        PrintWriter fp = new PrintWriter(new File(out, "d3d9_funcs.csv"), "UTF-8");
        fp.println("fonction;nom;total;methodes");
        for (Map.Entry<Function, Map<String,Integer>> e : l) {
            int s = e.getValue().values().stream().mapToInt(i -> i).sum();
            fp.println(e.getKey().getEntryPoint() + ";" + e.getKey().getName() + ";" + s + ";" + e.getValue());
        }
        fp.close();
        println("D3D9Map : " + total + " appels candidats dans " + per.size() + " fonctions -> " + out);
    }
}
