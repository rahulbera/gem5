from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class MemRenamePredictor(SimObject):
    type = "MemRenamePredictor"
    cxx_class = "gem5::o3::MemRenamePredictor"
    cxx_header = "cpu/o3/mem_rename_predictor.hh"

    numThreads = Param.Unsigned(Parent.numThreads, "Number of threads")

    # Value-file rendezvous tables (see mem_rename_valuefile.hh): a store
    # deposits its producer information into a named cell at its own rename;
    # a load reads the cell at its rename; the store cache learns
    # loadPC->cell bindings by address at execution.
    vfEntries = Param.Unsigned(1024, "Value-file rendezvous cells")
    slcEntries = Param.Unsigned(4096, "Store/load cache entries (PC-indexed)")
    slcAssoc = Param.Unsigned(4, "Store/load cache associativity")
    scEntries = Param.Unsigned(4096, "Store cache entries (address-indexed)")
    scAssoc = Param.Unsigned(4, "Store cache associativity")
    scGranularityBytes = Param.Unsigned(8, "Store cache line granularity")
    lmEntries = Param.Unsigned(
        4096, "Load-address monitor entries (store-write observation)"
    )
    lmAssoc = Param.Unsigned(4, "Load-address monitor associativity")
    lvStabilityTarget = Param.Unsigned(
        0,
        "Address-instability hysteresis for last-value forwarding: two "
        "wrong forwards with changed addresses disable last-value "
        "consumption for that PC until this many consecutive same-line "
        "executes are observed (0 = gate disabled).",
    )

    # Confidence (per-load, in the store/load cache entry).
    confBits = Param.Unsigned(4, "Confidence counter width in bits")
    confThreshold = Param.Unsigned(
        8, "Minimum confidence required to consume a prediction"
    )
    confInc = Param.Unsigned(1, "Confidence increment on a correct train")
    confDec = Param.Unsigned(1, "Confidence decrement on a value mismatch")
    resetConfOnMispredict = Param.Bool(
        True, "Reset (rather than decrement) confidence on a mispredict"
    )

    # Consumption modes.
    enableProducerAliasing = Param.Bool(
        False,
        "Alias a load's renamed destination to the in-flight producing "
        "store's physreg deposited in its bound value-file cell (the "
        "alias path).",
    )
    vfForwardProducerValue = Param.Bool(
        True,
        "Forward the producer physreg's value when it is already ready "
        "at the load's rename",
    )
    vfForwardLastValue = Param.Bool(
        True, "Forward the last value from a self-bound cell"
    )
