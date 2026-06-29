from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class MrnMode(Enum):
    vals = ["forward_value", "producer_reg_alias"]


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
    predictIntLoadsOnly = Param.Bool(
        True,
        "Mode B forwards a scalar RegVal, which is only valid for "
        "integer-destination loads. When True (default), non-integer loads "
        "are simply not forwarded. Setting it False relaxes the restriction "
        "but is unimplemented for wide values, so a forwarded non-integer "
        "load will panic.",
    )
    mrnMode = Param.MrnMode(
        "forward_value", "forward_value (B) | producer_reg_alias (C)"
    )
