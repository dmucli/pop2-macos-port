// Post-analysis: resolve selector-dispatched traps (QDExtensions, SoundDispatch,
// Pack N, ...) to specific stubs using CALL_OVERRIDE_UNCONDITIONAL references.
// Arg 0: path to pop2_meta.json
//@category PoP2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.lang.Register;
import com.google.gson.*;
import java.io.*;
import java.util.*;

public class MacPost extends GhidraScript {
    @Override
    public void run() throws Exception {
        JsonObject m = JsonParser.parseReader(new FileReader(getScriptArgs()[0])).getAsJsonObject();
        Map<Long, JsonObject> rootByAddr = new HashMap<>();
        for (JsonElement e : m.getAsJsonArray("traps")) {
            JsonObject t = e.getAsJsonObject();
            if (t.has("dispatcher") && t.get("dispatcher").getAsBoolean()) rootByAddr.put(t.get("addr").getAsLong(), t);
        }
        // variants keyed by trap word
        Map<Integer, List<JsonObject>> vars = new HashMap<>();
        for (JsonElement e : m.getAsJsonArray("variants")) {
            JsonObject v = e.getAsJsonObject();
            vars.computeIfAbsent(v.get("trap").getAsInt(), k -> new ArrayList<>()).add(v);
        }
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        int resolved = 0, unresolved = 0;
        Map<String, Integer> unres = new TreeMap<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (!ins.getMnemonicString().equals("atrap")) continue;
            Address dest = null;
            for (Reference r : ins.getReferencesFrom()) if (r.getReferenceType().isCall()) dest = r.getToAddress();
            if (dest == null) continue;
            JsonObject root = rootByAddr.get(dest.getOffset());
            if (root == null) continue;
            int word = root.get("word").getAsInt();
            String loc = root.get("loc").isJsonNull() ? "D0L" : root.get("loc").getAsString();
            Long sel = findSelector(ins, loc);
            JsonObject match = null;
            int key = word >= 0xA800 ? (word & ~0x400) : word;
            List<JsonObject> cands = vars.getOrDefault(key, Collections.emptyList());
            if (sel != null) {
                for (JsonObject v : cands) {
                    long vs = v.get("selector").getAsLong();
                    if (loc.equals("D0<0xFF>") ? ((vs & 0xFF) == (sel & 0xFF)) :
                        loc.equals("D0W") || loc.equals("StackW") ? ((vs & 0xFFFF) == (sel & 0xFFFF)) : vs == sel) {
                        match = v; break;
                    }
                }
                if (match == null && loc.equals("D0L")) {
                    for (JsonObject v : cands) if ((v.get("selector").getAsLong() & 0xFFFF) == (sel & 0xFFFF)) { match = v; break; }
                }
            }
            if (match == null) {
                unresolved++;
                unres.merge(root.get("name").getAsString() + " sel=" + (sel == null ? "?" : Long.toHexString(sel)), 1, Integer::sum);
                setEOLComment(ins.getAddress(), "dispatcher selector " + (sel == null ? "unknown" : "0x" + Long.toHexString(sel)));
                continue;
            }
            Address va = toAddr(match.get("addr").getAsLong());
            Reference ref = rm.addMemoryReference(ins.getAddress(), va, RefType.CALL_OVERRIDE_UNCONDITIONAL, SourceType.USER_DEFINED, 0);
            rm.setPrimary(ref, true);
            setEOLComment(ins.getAddress(), match.get("name").getAsString());
            resolved++;
        }
        println("dispatch resolved=" + resolved + " unresolved=" + unresolved);
        println("purges: " + reapplyPurges(m));
        println("jump-table refs: " + resolveJumpTable(m));
        for (Map.Entry<String, Integer> e : unres.entrySet()) println("  unresolved " + e.getKey() + " x" + e.getValue());
    }

    Long findSelector(Instruction ins, String loc) {
        Instruction p = ins;
        for (int i = 0; i < 4; i++) {
            p = p.getPrevious();
            if (p == null) return null;
            String mn = p.getMnemonicString();
            String op1 = p.getNumOperands() > 1 ? p.getDefaultOperandRepresentation(1) : "";
            Scalar sc = p.getScalar(0);
            if (loc.startsWith("Stack")) {
                if ((mn.startsWith("move") || mn.equals("pea")) && op1.contains("-(SP)") && sc != null) return sc.getUnsignedValue();
                if (mn.equals("pea") && sc != null) return sc.getUnsignedValue();
                if (op1.contains("-(SP)") || p.getFlowType().isCall()) return null;
            } else {
                if (op1.equals("D0") && (mn.startsWith("move") || mn.startsWith("moveq")) && sc != null) return sc.getUnsignedValue();
                if (op1.equals("D0")) return null;
            }
        }
        return null;
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

    int reapplyPurges(JsonObject m) {
        int n = 0;
        FunctionManager fm = currentProgram.getFunctionManager();
        for (String key : new String[] {"traps", "variants"}) {
            for (JsonElement e : m.getAsJsonArray(key)) {
                JsonObject t = e.getAsJsonObject();
                Function f = fm.getFunctionAt(toAddr(t.get("addr").getAsLong()));
                if (f == null || !t.has("pop")) continue;
                boolean reg = t.has("regbased") && t.get("regbased").getAsBoolean();
                int pop = t.get("pop").isJsonNull() ? -1 : t.get("pop").getAsInt();
                if (t.has("dispatcher") && t.get("dispatcher").isJsonPrimitive() && t.get("dispatcher").getAsJsonPrimitive().isBoolean() && t.get("dispatcher").getAsBoolean()) continue;
                if (pop < 0) continue;
                f.setStackPurgeSize(reg ? 0 : pop);
                n++;
            }
        }
        return n;
    }
}
