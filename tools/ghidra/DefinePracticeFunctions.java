// Define the current build's live-verified Practice-controller virtual methods.
// Run after Auto Analyze finishes. Changes the Ghidra database only.
// @category OpenDojo

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DefinePracticeFunctions extends GhidraScript {
    private static final long VTABLE = 0x1485651F8L;
    private static final long[] ENTRIES = {
        0x145C9BE40L, 0x145C561A0L, 0x141213CC0L, 0x145C5FA10L,
        0x145CA4280L, 0x145CAB220L, 0x145CA42D0L, 0x145CAB120L,
        0x145CA42A0L, 0x145C9D550L
    };

    @Override
    public void run() throws Exception {
        if (currentProgram == null || currentProgram.getImageBase().getOffset() != 0x140000000L) {
            printerr("Open the current Polaris executable at image base 0x140000000 first.");
            return;
        }
        // Verify the entire live-observed table before changing any definitions.
        for (int i = 0; i < ENTRIES.length; ++i) {
            if (currentProgram.getMemory().getLong(toAddr(VTABLE + i * 8L)) != ENTRIES[i]) {
                printerr("Practice vtable mismatch at slot " + i + "; no changes made.");
                return;
            }
        }
        for (long entry : ENTRIES) {
            monitor.checkCancelled();
            Address address = toAddr(entry);
            Function existing = getFunctionAt(address);
            if (existing != null) {
                println("Already defined: " + existing.getName() + " at " + address);
                continue;
            }
            existing = getFunctionContaining(address);
            if (existing != null) {
                println("Skipping " + address + ": inside existing " + existing.getName());
                continue;
            }
            disassemble(address);
            Function created = createFunction(address, null);
            println((created == null ? "Could not define: " : "Defined: ") + address);
        }
        println("Practice entry-point pass finished. The bridge can now retry decompilation.");
    }
}
