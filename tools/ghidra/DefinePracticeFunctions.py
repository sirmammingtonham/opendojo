# Define the current build's live-verified Practice virtual methods.
# Run after Auto Analyze finishes. Changes the Ghidra database only.
# @category OpenDojo
# @runtime Jython

VTABLE = 0x1485651F8
ENTRIES = [
    0x145C9BE40, 0x145C561A0, 0x141213CC0, 0x145C5FA10,
    0x145CA4280, 0x145CAB220, 0x145CA42D0, 0x145CAB120,
    0x145CA42A0, 0x145C9D550,
    0x145C9D920, 0x145CAB5F0, 0x145CA2B40, 0x145C9F030,
    0x145CAB620, 0x145CA2A80, 0x145CAAC70, 0x145CAB600,
    0x14127B010, 0x145CAB760, 0x145CA2A90, 0x145CAB630,
    0x140A5E480, 0x140A5E480, 0x140A5E480, 0x140A60010,
    0x145CAA2B0, 0x145CA4E60, 0x145CA2B10, 0x145CA28D0,
    0x145CA2360, 0x140F2A990, 0x140A5E480, 0x140A5E480,
]


def define_practice_functions():
    if currentProgram is None or currentProgram.getImageBase().getOffset() != 0x140000000:
        printerr("Open the current Polaris executable at image base 0x140000000 first.")
        return

    memory = currentProgram.getMemory()
    for index, entry in enumerate(ENTRIES):
        if memory.getLong(toAddr(VTABLE + index * 8)) != entry:
            printerr("Practice vtable mismatch at slot %d; no changes made." % index)
            return

    for entry in ENTRIES:
        monitor.checkCancelled()
        address = toAddr(entry)
        existing = getFunctionAt(address)
        if existing is not None:
            println("Already defined: %s at %s" % (existing.getName(), address))
            continue
        existing = getFunctionContaining(address)
        if existing is not None:
            println("Skipping %s: inside existing %s" % (address, existing.getName()))
            continue
        disassemble(address)
        created = createFunction(address, None)
        println(("Could not define: " if created is None else "Defined: ") + str(address))

    println("Practice entry-point pass finished. The bridge can now retry decompilation.")


define_practice_functions()
