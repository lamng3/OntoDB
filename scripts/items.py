"""Plan items shared by scripts/check and scripts/status."""

ITEMS = [
    ("0.1", "ShellSmoke.*:QueryHarness.*", False, False),
    ("0.2", "P0_2_*", True, False),
    ("0.3", "ExplainSmoke.*", False, False),
    ("1.1", "P1_1_*", True, False),
    ("1.2", "P1_2_*", True, False),
    ("1.3", "P1_3_*", True, False),
    ("1.4", "P1_4_*", True, False),
    ("1.5", "P1_5_*", True, False),
    ("1.6", "P1_6_*", True, False),
    ("2.1", "P2_1_*", True, False),
    ("2.2", "P2_2_*", True, False),
    ("2.3", "P2_3_*", True, False),
    ("2.4", "P2_4_*", True, False),
    ("2.5", "P2_5_*", True, False),
    ("2.6", "P2_6_*", True, False),
    ("2.7", "P2_7_*", True, False),
    ("3.1", "P3_1_*", True, False),
    ("3.2", "P3_2_*", True, False),
    ("3.3", "P3_3_*", True, False),
    ("3.4", "P3_4_*", True, False),
    ("3.5", "P3_5_*", True, False),
    ("3.6", "P3_6_*", True, False),
    ("4.1", "P4_1_*", True, False),
    ("4.2", "P4_2_*", True, False),
    ("4.3", "P4_3_*", True, False),
    ("4.4", "P4_4_*", True, False),
    ("4.5", "P4_5_*", True, False),
    ("4.6", "P4_6_*", True, False),
    ("5.1", "P5_1_*", True, False),
    ("5.2", "P5_2_*", True, False),
    ("5.3", "P5_3_*", True, False),
    ("5.4", "P5_4_*", True, False),
    ("5.5", "P5_5_*", True, False),
    ("6.1", "P6_1_*", True, False),
    ("6.2", "P6_2_*", True, False),
    ("7.1", "P7_1_*", True, True),
    ("7.2", "P7_2_*", True, True),
    ("7.3", "P7_3_*", True, True),
    ("7.4", "P7_4_*", True, True),
    ("7.5", "P7_5_*", True, True),
    ("7.6", "P7_6_*", True, True),
    ("8.1", "P8_1_*", True, False),
    ("8.2", "P8_2_*", True, False),
    ("8.3", "P8_3_*", True, False),
    ("8.4", "P8_4_*", True, False),
    ("8.5", "P8_5_*", True, False),
    ("8.6", "P8_6_*", True, False),
    ("8.7", "P8_7_*", True, False),
]

PROGRESS = [item for item in ITEMS if item[0] not in ("0.1", "0.3")]


def select(token):
    if token in {item[0] for item in ITEMS}:
        return [item for item in ITEMS if item[0] == token]
    phase = token.split(".", 1)[0]
    chosen = [item for item in ITEMS if item[0].split(".", 1)[0] == phase]
    if not chosen:
        raise SystemExit(f"unknown item or phase: {token}")
    return chosen


def next_item(current):
    ids = [item[0] for item in ITEMS]
    index = ids.index(current)
    if index + 1 < len(ids):
        return ids[index + 1]
    return "done"
