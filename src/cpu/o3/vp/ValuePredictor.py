from m5.params import *
from m5.SimObject import SimObject


class BaseValuePredictor(SimObject):
    type = "BaseValuePredictor"
    abstract = True
    cxx_class = "gem5::o3::BaseValuePredictor"
    cxx_header = "cpu/o3/vp/base.hh"

    onlyLoads = Param.Bool(
        True,
        "Predict loads only; when false, every eligible (single scalar "
        "integer destination) instruction is in scope.",
    )
    scalarOnly = Param.Bool(
        True,
        "Restrict prediction to scalar destinations. Currently subsumed "
        "by the integer-only eligibility rule; the knob exists so the "
        "interface is stable when FP/vector support lands.",
    )


class LastValueVP(BaseValuePredictor):
    type = "LastValueVP"
    cxx_class = "gem5::o3::LastValueVP"
    cxx_header = "cpu/o3/vp/last_value.hh"

    entries = Param.Unsigned(4096, "Value prediction table entries")
    assoc = Param.Unsigned(4, "Value prediction table associativity")
    confBits = Param.Unsigned(4, "Confidence counter width in bits")
    confThreshold = Param.Unsigned(
        15, "Minimum confidence required to predict"
    )
    confDecrementOnWrong = Param.Bool(
        False,
        "Decrement (rather than reset to zero) confidence on a value "
        "mismatch",
    )
