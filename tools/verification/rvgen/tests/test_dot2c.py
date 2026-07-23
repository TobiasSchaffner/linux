# SPDX-License-Identifier: GPL-2.0-only
"""Tests for dot2c output (classic format + edgestat table header).

Verifies that:
  - Classic dots produce correct struct output (unchanged behavior).
  - format_table_header() produces state/event/edge enums, transition
    and edge_id matrices and label arrays.
  - The on= attribute decouples event names from edge labels.
"""
import tempfile
import unittest
from pathlib import Path

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rvgen.dot2c import Dot2c


# Real wwnr format (matches kernel's tools/verification/models/wwnr.dot)
CLASSIC_DOT = """\
digraph state_automaton {
\t{node [shape = plaintext, style=invis, label=""] "__init_not_running"};
\t{node [shape = ellipse] "not_running"};
\t{node [shape = plaintext] "not_running"};
\t{node [shape = plaintext] "running"};
\t"__init_not_running" -> "not_running";
\t"not_running" [label = "not_running", color = green3];
\t"not_running" -> "not_running" [ label = "wakeup" ];
\t"not_running" -> "running" [ label = "switch_in" ];
\t"running" [label = "running"];
\t"running" -> "not_running" [ label = "switch_out" ];
\t{ rank = min ;
\t\t"__init_not_running";
\t\t"not_running";
\t}
}
"""

# Edgestat dot with on=.  Includes a name= attribute as
# format_table_header() builds enums named after it.
EDGESTAT_DOT = """\
digraph state_automaton {
\tname="demo";
\t{node [shape = plaintext, style=invis, label=""] "__init_IDLE"};
\t{node [shape = doublecircle] "IDLE"};
\t{node [shape = circle] "BUSY"};
\t{node [shape = circle] "WAITING"};
\t"__init_IDLE" -> "IDLE";
\t"IDLE" -> "BUSY" [label="start_work", on="begin"];
\t"BUSY" -> "WAITING" [label="work_done", on="finish"];
\t"WAITING" -> "IDLE" [label="ack_from_busy", on="ack"];
\t"IDLE" -> "WAITING" [label="start_wait", on="wait"];
}
"""


class TestClassicStructUnchanged(unittest.TestCase):
    """Classic dots produce the same struct-based output as before."""

    def _model(self, content):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "test.dot"
            p.write_text(content)
            d = Dot2c(str(p))
            return "\n".join(d.format_model())

    def test_struct_definition(self):
        text = self._model(CLASSIC_DOT)
        self.assertIn("struct automaton {", text)

    def test_state_enum(self):
        text = self._model(CLASSIC_DOT)
        self.assertIn("not_running,", text)
        self.assertIn("running,", text)
        self.assertIn("state_max,", text)

    def test_event_enum(self):
        text = self._model(CLASSIC_DOT)
        self.assertIn("switch_in,", text)
        self.assertIn("switch_out,", text)
        self.assertIn("wakeup,", text)

    def test_initial_state(self):
        text = self._model(CLASSIC_DOT)
        self.assertIn(".initial_state = not_running,", text)


class TestFormatTableHeader(unittest.TestCase):
    """format_table_header() emits the composed edge-stat header."""

    def _hdr(self, content):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "test.dot"
            p.write_text(content)
            return Dot2c(str(p)).format_table_header()

    def test_state_enum(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("enum demo_state {", text)
        self.assertIn("DEMO_S_IDLE", text)
        self.assertIn("DEMO_S_BUSY", text)
        self.assertIn("DEMO_S__MAX", text)

    def test_event_enum(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("enum demo_event {", text)
        # Events come from on=, sorted.
        self.assertIn("DEMO_EV_BEGIN", text)
        self.assertIn("DEMO_EV_FINISH", text)
        self.assertIn("DEMO_EV_ACK", text)
        self.assertIn("DEMO_EV_WAIT", text)

    def test_edge_enum(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("enum demo_edge {", text)
        self.assertIn("DEMO_EDGE_WORK_DONE", text)
        self.assertIn("DEMO_EDGE_START_WORK", text)

    def test_label_arrays(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("demo_state_labels", text)
        self.assertIn("demo_event_labels", text)
        self.assertIn("demo_edge_labels", text)

    def test_transition_and_edge_id_tables(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("demo_transition", text)
        self.assertIn("demo_edge_id", text)


    def test_static_asserts(self):
        text = self._hdr(EDGESTAT_DOT)
        self.assertIn("static_assert(DEMO_S__MAX", text)
        self.assertIn("static_assert(DEMO_EV__MAX", text)
        self.assertIn("static_assert(DEMO_EDGE__MAX", text)


if __name__ == "__main__":
    unittest.main()
