from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class MrnCorrelation(Enum):
    # How producer aliasing finds a load's producing store. lsq_forward
    # learns loadPC->storePC bindings from the LSQ store->load forward event;
    # store_set is a stub (treated as "no producer found"); value_file is
    # the rendezvous model: a store deposits its producer information into
    # a named cell at its own rename, and the cell is learned by address at
    # the store's execution (address resolution).
    vals = ["lsq_forward", "store_set", "value_file"]


class MemRenamePredictor(SimObject):
    type = "MemRenamePredictor"
    cxx_class = "gem5::o3::MemRenamePredictor"
    cxx_header = "cpu/o3/mem_rename_predictor.hh"

    numThreads = Param.Unsigned(Parent.numThreads, "Number of threads")

    storeTableEntries = Param.Unsigned(
        1024, "Number of store-cache entries (effAddr -> value-file slot)"
    )
    storeTableAssoc = Param.Unsigned(4, "Store-cache associativity")
    loadTableEntries = Param.Unsigned(
        1024, "Number of load-cache entries (loadPC -> slot + confidence)"
    )
    loadTableAssoc = Param.Unsigned(4, "Load-cache associativity")
    valueFileEntries = Param.Unsigned(
        512, "Number of value-file slots, each holding {value, valid}"
    )
    confBits = Param.Unsigned(4, "Confidence counter width in bits")
    confThreshold = Param.Unsigned(
        8, "Minimum confidence required to issue a prediction"
    )
    confInc = Param.Unsigned(1, "Confidence increment on a correct train")
    confDec = Param.Unsigned(1, "Confidence decrement on a value mismatch")
    resetConfOnMispredict = Param.Bool(
        True, "Reset confidence on a value mismatch (else subtract confDec)"
    )
    trainAtCommit = Param.Bool(
        True, "Train the predictor at commit (value snapshot)"
    )
    trainOnRenameSnapshot = Param.Bool(
        True,
        "Train the value-forwarding confidence counter against the "
        "value the prediction "
        "actually used at rename, not the value file as of commit. The "
        "commit-time value has already been refreshed by the producing "
        "store, so a changing store->load recurrence reports a spurious "
        "match every iteration and saturates confidence on exactly the loads "
        "the forward gets wrong (measured: 43%% of gcc matches had a wrong "
        "rename snapshot). Set False to restore the commit-time comparison.",
    )
    aliasRequireCurrentProducer = Param.Bool(
        True,
        "Producer aliasing: only alias when the rename-map mapping of "
        "the store's data "
        "architectural register still equals the physreg the located store "
        "itself captured. When they disagree the arch reg was redefined in "
        "between, so the alias is a bet on register liveness rather than on "
        "memory dataflow -- measured at 29.9% correct on 721.gcc_r.2.0 and "
        "0.0% on 714.cpython_r.2.3, against 100% (n=155, zero errors) when "
        "they agree. A rejected alias falls back to the value snapshot "
        "snapshot rather than losing the prediction. Set False to restore "
        "the previous always-alias behaviour.",
    )
    predictIntLoadsOnly = Param.Bool(
        True,
        "Value forwarding uses a scalar RegVal, which is only valid for "
        "integer-destination loads. When True (default), non-integer loads "
        "are simply not forwarded. Setting it False relaxes the restriction "
        "but is unimplemented for wide values, so a forwarded non-integer "
        "load will panic.",
    )
    enableValueForwarding = Param.Bool(
        True,
        "Forward a snapshotted value into a high-confidence load's renamed "
        "destination (the value path). Independent of producer aliasing.",
    )
    enableProducerAliasing = Param.Bool(
        False,
        "Alias a load's renamed destination to an in-flight producing "
        "store's physreg when the correlator has a binding (the alias "
        "path). Independent of value forwarding; when both are enabled, "
        "aliasing takes priority.",
    )
    mrnCorrelation = Param.MrnCorrelation(
        "lsq_forward",
        "Producer-binding source for producer aliasing: lsq_forward "
        "(loadPC->storePC learned from LSQ forwarding) | store_set (stub, "
        "treated as no producer found) | value_file (rendezvous model, "
        "deposits at rename, learned by address at execution).",
    )

    vfEntries = Param.Unsigned(1024, "Value-file rendezvous cells")
    slcEntries = Param.Unsigned(4096, "Store/load cache entries (PC-indexed)")
    slcAssoc = Param.Unsigned(4, "Store/load cache associativity")
    scEntries = Param.Unsigned(4096, "Store cache entries (address-indexed)")
    scAssoc = Param.Unsigned(4, "Store cache associativity")
    scGranularityBytes = Param.Unsigned(8, "Store cache line granularity")
    vfForwardProducerValue = Param.Bool(
        True,
        "Forward the producer physreg's value when it is already ready "
        "at the load's rename (value_file correlation only)",
    )
    vfForwardLastValue = Param.Bool(
        True,
        "Forward the last value from a self-bound cell "
        "(value_file correlation only)",
    )
