from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class MrnMode(Enum):
    # value_only = mode B alone (value snapshot forwarding). unified = C >= B:
    # alias to an in-flight producer's physreg when one is found, else fall
    # back to the mode-B value snapshot.
    vals = ["value_only", "unified"]


class MrnCorrelation(Enum):
    # How the unified predictor finds a load's producing store. lsq_forward
    # learns loadPC->storePC bindings from the LSQ store->load forward event;
    # store_set is a stub (treated as "no producer found").
    vals = ["lsq_forward", "store_set"]


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
        True, "Train the predictor at commit (mode B value snapshot)"
    )
    aliasRequireCurrentProducer = Param.Bool(
        True,
        "Mode C: only alias when the rename-map mapping of the store's data "
        "architectural register still equals the physreg the located store "
        "itself captured. When they disagree the arch reg was redefined in "
        "between, so the alias is a bet on register liveness rather than on "
        "memory dataflow -- measured at 29.9% correct on 721.gcc_r.2.0 and "
        "0.0% on 714.cpython_r.2.3, against 100% (n=155, zero errors) when "
        "they agree. A rejected alias falls back to the mode-B value "
        "snapshot rather than losing the prediction. Set False to restore "
        "the previous always-alias behaviour.",
    )
    predictIntLoadsOnly = Param.Bool(
        True,
        "Mode B forwards a scalar RegVal, which is only valid for "
        "integer-destination loads. When True (default), non-integer loads "
        "are simply not forwarded. Setting it False relaxes the restriction "
        "but is unimplemented for wide values, so a forwarded non-integer "
        "load will panic.",
    )
    mrnMode = Param.MrnMode(
        "value_only",
        "value_only (mode B value snapshot only) | unified (mode C, which "
        "subsumes B: alias to an in-flight producer's physreg when found, "
        "else fall back to the B value snapshot).",
    )
    mrnCorrelation = Param.MrnCorrelation(
        "lsq_forward",
        "Producer-binding source for the unified predictor: lsq_forward "
        "(loadPC->storePC learned from LSQ forwarding) | store_set (stub, "
        "treated as no producer found).",
    )
