// Post-analysis: MacsBug symbols follow their function's RTS, but routines compiled
// without symbols sit before them in the same span. Give each name to the function
// that contains its RTS, then export the function list (entry, end, name).
// Args: pop2_meta.json  functions.json
//@category PoP2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import com.google.gson.*;
import java.io.*;
import java.util.*;

public class FixNames extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    JsonObject m = JsonParser.parseReader(new FileReader(a[0])).getAsJsonObject();
    FunctionManager fm = currentProgram.getFunctionManager();
    int moved = 0, same = 0, missing = 0;
    Map<Function, String> want = new HashMap<>();
    for (JsonElement e : m.getAsJsonArray("functions")) {
      JsonObject f = e.getAsJsonObject();
      long start = f.get("addr").getAsLong(), end = f.get("end").getAsLong();
      String name = f.get("name").getAsString();
      Function fe = fm.getFunctionContaining(toAddr(end - 2));
      if (fe == null) { missing++; continue; }
      Function fs = fm.getFunctionAt(toAddr(start));
      if (fs != null && fs != fe && fs.getName().equals(name)) {
        fs.setName(String.format("FUN_%08x", start), SourceType.DEFAULT);
      }
      want.put(fe, name);
      if (fe.getEntryPoint().getOffset() == start) same++; else moved++;
    }
    for (Map.Entry<Function, String> e : want.entrySet()) {
      try { e.getKey().setName(e.getValue(), SourceType.IMPORTED); } catch (Exception ex) { println("rename fail " + e.getValue() + ": " + ex); }
    }
    JsonArray out = new JsonArray();
    for (Function f : fm.getFunctions(toAddr(0x10000), true)) {
      long ep = f.getEntryPoint().getOffset();
      if (ep >= 0x00200000L) break;
      JsonObject o = new JsonObject();
      o.addProperty("name", f.getName());
      o.addProperty("entry", ep);
      o.addProperty("end", f.getBody().getMaxAddress().getOffset() + 1);
      o.addProperty("named", f.getSymbol().getSource() == SourceType.IMPORTED);
      out.add(o);
    }
    try (Writer w = new FileWriter(a[1])) { new GsonBuilder().setPrettyPrinting().create().toJson(out, w); }
    println("same=" + same + " moved=" + moved + " missing=" + missing + " exported=" + out.size());
  }
}
