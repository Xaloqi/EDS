"""EDS#583: Rule 8.7 deviations are per function, not per file.

DEV-ACCS-02 (documented API) and DEV-ACCS-03 (test-only accessors) name their
functions, so a new externally-linked function in the same files is an OPEN
violation until someone classifies it. Needs no cppcheck.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import misra_analysis as m  # noqa: E402

BY_ID = {d["id"]: d for d in m.MISRA_DEVIATIONS}
API, TEST_ONLY = BY_ID["DEV-ACCS-02"], BY_ID["DEV-ACCS-03"]


def test_deviation_ids_are_unique():
    ids = [d["id"] for d in m.MISRA_DEVIATIONS]
    assert len(ids) == len(set(ids))


def test_api_and_test_only_lists_are_disjoint_and_unique():
    assert len(API["symbols"]) == len(set(API["symbols"]))
    assert not set(API["symbols"]) & set(TEST_ONLY["symbols"])


def test_every_symbol_is_defined_in_one_of_its_files():
    for dev in (API, TEST_ONLY):
        text = "".join((ROOT / f).read_text() for f in dev["files"])
        for sym in dev["symbols"]:
            assert re.search(rf"^[A-Za-z_][\w \*]*\b{sym}\s*\(", text, re.M), \
                f"{dev['id']}: {sym} is not defined in {dev['files']}"


def test_a_symbol_outside_the_lists_is_not_covered(tmp_path):
    src = tmp_path / "isotp.c"
    src.write_text("uds_status_t isotp_brand_new(void)\n{ return 0; }\n")
    # path ends in a covered file name, symbol is not listed
    f = tmp_path / "transport"
    f.mkdir()
    (f / "isotp.c").write_text(src.read_text())
    assert m._deviation_for("8.7", str(f / "isotp.c"), 1) is None


def test_a_listed_symbol_is_covered_by_the_right_record(tmp_path):
    f = tmp_path / "transport"
    f.mkdir()
    (f / "isotp.c").write_text("uds_status_t isotp_reset_rx(isotp_ctx_t *c)\n{ return 0; }\n"
                               "uds_status_t isotp_get_state(isotp_ctx_t *c)\n{ return 0; }\n")
    assert m._deviation_for("8.7", str(f / "isotp.c"), 1)["id"] == "DEV-ACCS-02"
    # isotp_reset is the test-only one
    (f / "isotp.c").write_text("uds_status_t isotp_reset(isotp_ctx_t *c)\n{ return 0; }\n")
    assert m._deviation_for("8.7", str(f / "isotp.c"), 1)["id"] == "DEV-ACCS-03"
