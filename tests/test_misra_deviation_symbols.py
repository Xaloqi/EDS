"""EDS#583: Rule 8.7 deviations are per function, not per file.

DEV-ACCS-02 (documented API) names its functions, so a new externally-linked
function in the same files is an OPEN violation until someone classifies it.
The test-only accessors had a record of their own (DEV-ACCS-03); since EDS#599
they are compiled only under EDS_TEST_HOOKS and the record is gone: a new
accessor that only tests call is an open violation, or gets the macro.
Needs no cppcheck.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import misra_analysis as m  # noqa: E402

BY_ID = {d["id"]: d for d in m.MISRA_DEVIATIONS}
API = BY_ID["DEV-ACCS-02"]


def test_deviation_ids_are_unique():
    ids = [d["id"] for d in m.MISRA_DEVIATIONS]
    assert len(ids) == len(set(ids))


def test_api_list_is_unique():
    assert len(API["symbols"]) == len(set(API["symbols"]))


def test_test_only_record_is_gone():
    """EDS#599: the accessors are hidden by EDS_TEST_HOOKS, not deviated."""
    assert "DEV-ACCS-03" not in BY_ID


def test_every_symbol_is_defined_in_one_of_its_files():
    text = "".join((ROOT / f).read_text() for f in API["files"])
    for sym in API["symbols"]:
        assert re.search(rf"^[A-Za-z_][\w \*]*\b{sym}\s*\(", text, re.M), \
            f"{API['id']}: {sym} is not defined in {API['files']}"


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
    # isotp_reset is test-only (EDS_TEST_HOOKS): no record covers it any more
    (f / "isotp.c").write_text("uds_status_t isotp_reset(isotp_ctx_t *c)\n{ return 0; }\n")
    assert m._deviation_for("8.7", str(f / "isotp.c"), 1) is None
