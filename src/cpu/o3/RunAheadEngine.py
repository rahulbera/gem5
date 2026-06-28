# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.BaseCPU import BaseCPU
from m5.params import *


class RunAheadEngine(BaseCPU):
    """Execute-at-fetch run-ahead engine (v1 substrate).

    Structurally a stripped-down CheckerCPU: a BaseCPU side-car (built with
    is_checker=True so it does not register as a schedulable CPU) that runs the
    true instruction path ahead of the O3 pipeline and records each
    instruction's ground truth (OracleInfo). The pipeline attaches that truth
    at fetch and validates it at commit. v1 is metadata-only.
    """

    type = "RunAheadEngine"
    cxx_class = "gem5::o3::RunAheadEngine"
    cxx_header = "cpu/o3/run_ahead_engine.hh"

    @classmethod
    def memory_mode(cls):
        return "atomic"

    @classmethod
    def require_caches(cls):
        return False

    @classmethod
    def support_take_over(cls):
        return True

    enable = Param.Bool(True, "Produce and attach OracleInfo at fetch")
    validateOracle = Param.Bool(
        True, "Assert OracleInfo == actual at commit (panic on mismatch)"
    )
    injectOracleFault = Param.Int(
        0, "Debug: corrupt one OracleInfo field to exercise the assert (0=off)"
    )
    capacity = Param.Int(256, "Max in-flight OracleInfo records retained")
