// Apply Pascal calling convention info (from detect_pascal.py): stack purge,
// parameters covering the popped bytes, and the result as a stack-stored return.
// Arg 0: pascal_fns.json
//@category PoP2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.data.*;
import ghidra.program.model.symbol.*;
import com.google.gson.*;
import java.io.*;
import java.util.*;

public class ApplyPascal extends GhidraScript {
  public void run() throws Exception {
    JsonArray a = JsonParser.parseReader(new FileReader(getScriptArgs()[0])).getAsJsonArray();
    int ok = 0, fail = 0;
    for (JsonElement e : a) {
      JsonObject o = e.getAsJsonObject();
      Function f = getFunctionAt(toAddr(o.get("entry").getAsLong()));
      if (f == null) { fail++; continue; }
      int purge = o.get("purge").getAsInt(), ret = o.get("ret").getAsInt();
      try {
        TreeMap<Integer, Integer> have = new TreeMap<>();
        for (Parameter p : f.getParameters())
          if (p.isStackVariable() && p.getStackOffset() >= 4 && p.getStackOffset() + p.getLength() <= 4 + purge)
            have.put(p.getStackOffset(), Math.max(p.getLength(), 2));
        List<Variable> ps = new ArrayList<>();
        int off = 4;
        while (off < 4 + purge) {
          int sz = have.containsKey(off) ? have.get(off) : 2;
          DataType dt = sz == 4 ? Undefined4DataType.dataType : Undefined2DataType.dataType;
          ps.add(new ParameterImpl(String.format("p%d", off), dt, off, currentProgram));
          off += sz;
        }
        ReturnParameterImpl rp = ret > 0
          ? new ReturnParameterImpl(ret == 4 ? Undefined4DataType.dataType : Undefined2DataType.dataType,
                                    new VariableStorage(currentProgram, 4 + purge, ret == 4 ? 4 : 2), currentProgram)
          : new ReturnParameterImpl(VoidDataType.dataType, currentProgram);
        f.updateFunction(null, rp, ps, FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
        f.setStackPurgeSize(purge);
        String c = "pascal convention: callee pops " + purge + " bytes" + (ret > 0 ? ", " + ret + "-byte result above the arguments" : "");
        if (f.getComment() == null || !f.getComment().contains("pascal convention")) f.setComment(c);
        ok++;
      } catch (Exception ex) { println("fail " + f.getName() + ": " + ex); fail++; }
    }
    println("pascal applied " + ok + " failed " + fail);
  }
}
