// Pre-analysis setup for a flat classic-Mac 68k image built by build_image.py.
// Arg 0: path to pop2_meta.json
//@category PoP2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.symbol.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import com.google.gson.*;
import java.io.*;
import java.math.BigInteger;
import java.util.*;

public class MacSetup extends GhidraScript {
    Address a(long v) { return toAddr(v); }

    DataType dtForSize(int n) {
        switch (n) {
            case 1: return Undefined1DataType.dataType;
            case 2: return Undefined2DataType.dataType;
            default: return Undefined4DataType.dataType;
        }
    }

    void splitAt(long addr, String name) throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock b = mem.getBlock(a(addr));
        if (b == null) return;
        if (!b.getStart().equals(a(addr))) {
            mem.split(b, a(addr));
            b = mem.getBlock(a(addr));
        }
        b.setName(name);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        JsonObject m = JsonParser.parseReader(new FileReader(args[0])).getAsJsonObject();
        long A5 = m.get("A5").getAsLong();
        long below = m.get("below").getAsLong();
        long above = m.get("above").getAsLong();
        SymbolTable st = currentProgram.getSymbolTable();
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "true");
        Listing listing = currentProgram.getListing();

        // ---- memory blocks ----
        splitAt(0x0, "LOWMEM");
        splitAt(0x3000, "unused0");
        JsonObject segs = m.getAsJsonObject("segments");
        long lastEnd = 0;
        for (String k : segs.keySet()) {
            JsonObject s = segs.getAsJsonObject(k);
            String nm = s.get("name").isJsonNull() ? "CODE_" + k : ("CODE_" + k + "_" + s.get("name").getAsString().replaceAll("[^A-Za-z0-9]", ""));
            splitAt(s.get("base").getAsLong(), nm);
            lastEnd = Math.max(lastEnd, s.get("base").getAsLong() + s.get("size").getAsLong());
        }
        splitAt((lastEnd + 0xF) & ~0xF, "unused1");
        splitAt(A5 - below, "A5_GLOBALS");
        splitAt(A5, "A5_JUMPTABLE");
        splitAt(A5 + above, "unused2");
        splitAt(0x00200000L, "TRAPS");
        splitAt(0x00208000L, "TRAP_VARIANTS");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            boolean code = b.getName().startsWith("CODE_") || b.getName().startsWith("TRAP") || b.getName().equals("A5_JUMPTABLE");
            b.setExecute(code);
            b.setWrite(!b.getName().startsWith("CODE_"));
        }

        // ---- A5 is constant throughout the application ----
        Register a5 = currentProgram.getRegister("A5");
        currentProgram.getProgramContext().setValue(a5, a(0x10000), a(0x0020FFFF), BigInteger.valueOf(A5));
        st.createLabel(a(A5), "A5_WORLD", SourceType.USER_DEFINED);

        // ---- low memory globals ----
        for (JsonElement e : m.getAsJsonArray("lowmem")) {
            JsonObject l = e.getAsJsonObject();
            long ad = l.get("address").getAsLong();
            if (ad >= 0x3000) continue;
            try {
                st.createLabel(a(ad), "LM_" + l.get("name").getAsString(), SourceType.USER_DEFINED);
                int sz = l.get("size").getAsInt();
                if (sz == 1 || sz == 2 || sz == 4) {
                    listing.clearCodeUnits(a(ad), a(ad + sz - 1), false);
                    listing.createData(a(ad), dtForSize(sz));
                }
            } catch (Exception ex) { /* overlapping */ }
        }

        // ---- trap stubs ----
        FunctionManager fm = currentProgram.getFunctionManager();
        int nt = 0;
        for (JsonElement e : m.getAsJsonArray("traps")) {
            JsonObject t = e.getAsJsonObject();
            Address ad = a(t.get("addr").getAsLong());
            String name = t.get("name").getAsString() + String.format("_%04X", t.get("word").getAsInt());
            if (t.get("known").getAsBoolean()) name = t.get("name").getAsString();
            makeStub(ad, name, t);
            nt++;
        }
        for (JsonElement e : m.getAsJsonArray("variants")) {
            JsonObject t = e.getAsJsonObject();
            makeStub(a(t.get("addr").getAsLong()), t.get("name").getAsString(), t);
        }
        println("trap stubs: " + nt);

        // ---- jump table ----
        for (JsonElement e : m.getAsJsonArray("jumptable")) {
            JsonObject j = e.getAsJsonObject();
            Address ent = a(j.get("entry").getAsLong());
            disassemble(ent);
        }

        // ---- named functions (MacsBug symbols) ----
        int nf = 0;
        Set<Long> named = new HashSet<>();
        for (JsonElement e : m.getAsJsonArray("functions")) {
            JsonObject f = e.getAsJsonObject();
            Address ad = a(f.get("addr").getAsLong());
            Address end = a(f.get("end").getAsLong() - 1);
            named.add(f.get("addr").getAsLong());
            String name = f.get("name").getAsString();
            DisassembleCommand dc = new DisassembleCommand(ad, new AddressSet(ad, end), true);
            dc.applyTo(currentProgram, monitor);
            Function fn = fm.getFunctionAt(ad);
            try {
                if (fn == null) fn = createFunction(ad, name);
                if (fn != null) { fn.setName(name, SourceType.IMPORTED); nf++; }
                else st.createLabel(ad, name, SourceType.IMPORTED);
            } catch (Exception ex) {
                try { st.createLabel(ad, name, SourceType.IMPORTED); } catch (Exception ex2) {}
            }
        }
        // jump-table targets without symbols
        for (JsonElement e : m.getAsJsonArray("jumptable")) {
            JsonObject j = e.getAsJsonObject();
            long t = j.get("target").getAsLong();
            if (named.contains(t)) continue;
            Address ad = a(t);
            disassemble(ad);
            if (fm.getFunctionAt(ad) == null) {
                Function fn = createFunction(ad, String.format("seg%d_%06x", j.get("seg").getAsInt(), t));
            }
        }
        // entry point: first jump table entry
        JsonObject first = m.getAsJsonArray("jumptable").get(0).getAsJsonObject();
        currentProgram.getSymbolTable().addExternalEntryPoint(a(first.get("entry").getAsLong()));
        st.createLabel(a(first.get("entry").getAsLong()), "JT_Entry", SourceType.USER_DEFINED);
        println("named functions: " + nf);
        println("jump-table refs: " + resolveJumpTable(m));
    }

    void makeStub(Address ad, String name, JsonObject t) throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        disassemble(ad);
        Function fn = fm.getFunctionAt(ad);
        if (fn == null) fn = createFunction(ad, name);
        if (fn == null) return;
        fn.setName(name, SourceType.IMPORTED);
        if (!t.has("args")) return;
        JsonArray args = t.getAsJsonArray("args");
        List<Variable> params = new ArrayList<>();
        boolean regbased = t.has("regbased") && t.get("regbased").getAsBoolean();
        int rs = t.has("retsize") ? t.get("retsize").getAsInt() : 0;
        try {
            if (regbased) {
                Set<String> used = new HashSet<>();
                for (JsonElement ae : args) {
                    JsonObject arg = ae.getAsJsonObject();
                    if (arg.get("register") == null || arg.get("register").isJsonNull()) continue;
                    String r = arg.get("register").getAsString();
                    if (!r.matches("[AD][0-7].*")) continue;
                    String rn = r.substring(0, 2);
                    if (!used.add(rn)) continue;
                    Register reg = currentProgram.getRegister(rn);
                    params.add(new ParameterImpl(arg.get("name").getAsString(), Undefined4DataType.dataType, reg, currentProgram));
                }
                String rr = t.has("returnreg") && !t.get("returnreg").isJsonNull() ? t.get("returnreg").getAsString() : "D0";
                ReturnParameterImpl ret;
                if (rs == 0 && !t.has("returnreg")) ret = new ReturnParameterImpl(VoidDataType.dataType, currentProgram);
                else if (rs == 2 && rr.equals("D0")) ret = new ReturnParameterImpl(Undefined2DataType.dataType, currentProgram.getRegister("D0w"), currentProgram);
                else ret = new ReturnParameterImpl(Undefined4DataType.dataType, currentProgram.getRegister(rr.substring(0, 2)), currentProgram);
                fn.updateFunction(null, ret, params, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.IMPORTED);
                fn.setStackPurgeSize(0);
            } else {
                // Pascal: args pushed left to right; the last arg sits nearest the return address.
                int off = 4;
                List<JsonObject> al = new ArrayList<>();
                for (JsonElement ae : args) al.add(ae.getAsJsonObject());
                List<Variable> rev = new ArrayList<>();
                if (t.has("loc") && !t.get("loc").isJsonNull() && t.get("loc").getAsString().startsWith("Stack")) {
                    int sz = t.get("loc").getAsString().equals("StackW") ? 2 : 4;
                    rev.add(new ParameterImpl("selector", dtForSize(sz), off, currentProgram));
                    off += sz;
                }
                for (int i = al.size() - 1; i >= 0; i--) {
                    JsonObject arg = al.get(i);
                    if (arg.get("stack") == null || arg.get("stack").isJsonNull()) continue;
                    int sz = arg.get("stack").getAsInt();
                    String pn = arg.get("name").getAsString();
                    if (pn.equals("selector")) pn = "selector_arg";
                    rev.add(new ParameterImpl(pn, dtForSize(sz), off, currentProgram));
                    off += sz;
                }
                Collections.reverse(rev);
                ReturnParameterImpl ret = rs > 0
                    ? new ReturnParameterImpl(dtForSize(rs), new VariableStorage(currentProgram, off, rs), currentProgram)
                    : new ReturnParameterImpl(VoidDataType.dataType, currentProgram);
                fn.updateFunction(null, ret, rev, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.IMPORTED);
                // Pascal callee pops its arguments (and a stack selector); the result slot stays for the caller.
                fn.setStackPurgeSize(off - 4);
            }
            String sig = t.has("return") && !t.get("return").isJsonNull() ? t.get("return").getAsString() : "void";
            StringBuilder sb = new StringBuilder();
            for (JsonElement ae : args) { JsonObject a = ae.getAsJsonObject(); sb.append(sb.length() > 0 ? ", " : "").append(a.get("type").getAsString()).append(" ").append(a.get("name").getAsString()); }
            setPlateComment(ad, "Mac Toolbox: " + sig + " " + name.substring(1) + "(" + sb + ")");
        } catch (Exception ex) {
            println("stub sig failed " + name + ": " + ex);
        }
    }

    /** Redirect JSR/JMP d16(A5) through jump-table entries straight to their targets. */
    int resolveJumpTable(JsonObject m) throws Exception {
        long A5 = m.get("A5").getAsLong();
        java.util.Map<Long, Long> jt = new java.util.HashMap<>();
        for (JsonElement e : m.getAsJsonArray("jumptable")) {
            JsonObject j = e.getAsJsonObject();
            jt.put(j.get("entry").getAsLong(), j.get("target").getAsLong());
        }
        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (java.util.Map.Entry<Long, Long> e : jt.entrySet()) {
            Function f = fm.getFunctionAt(toAddr(e.getValue()));
            String n = f != null ? f.getName() : String.format("x%06x", e.getValue());
            if (st.getSymbols(toAddr(e.getKey())).length == 0)
                try { st.createLabel(toAddr(e.getKey()), "jt_" + n, SourceType.USER_DEFINED); } catch (Exception ex) {}
        }
        ghidra.program.model.mem.Memory mem = currentProgram.getMemory();
        int n = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getAddress().getOffset() >= 0x00200000L) break;
            int op = mem.getShort(ins.getAddress()) & 0xFFFF;
            boolean isJsr = op == 0x4EAD, isJmp = op == 0x4EED, isPea = op == 0x486D, isLea = (op & 0xF1FF) == 0x41ED;
            if (!(isJsr || isJmp || isPea || isLea)) continue;
            Long tgt = jt.get(A5 + mem.getShort(ins.getAddress().add(2)));
            if (tgt == null) continue;
            Address ta = toAddr(tgt);
            boolean done = false;
            for (Reference r : ins.getReferencesFrom()) {
                if (r.getToAddress().equals(ta)) { done = true; continue; }
                if ((isJsr || isJmp) && r.getReferenceType().isFlow()) rm.delete(r);
            }
            if (done) continue;
            if (isJsr || isJmp) {
                RefType rt = isJsr ? RefType.CALL_OVERRIDE_UNCONDITIONAL : RefType.JUMP_OVERRIDE_UNCONDITIONAL;
                Reference r = rm.addMemoryReference(ins.getAddress(), ta, rt, SourceType.USER_DEFINED, 0);
                rm.setPrimary(r, true);
            } else {
                rm.addMemoryReference(ins.getAddress(), ta, RefType.DATA, SourceType.USER_DEFINED, 0);
            }
            n++;
        }
        return n;
    }
}
