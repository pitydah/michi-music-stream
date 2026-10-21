#!/usr/bin/env python3
"""Static asset provenance and screen-guard regression (run locally)."""
from pathlib import Path
import hashlib
import re

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "firmware/components/michi_display"


def test_official_svg_matches_generated_header():
    source = (UI / "assets/brand/michi_stream_official.svg").read_bytes()
    header = (UI / "assets/icons/michi_stream_brand_data.h").read_text()
    assert hashlib.sha256(source).hexdigest() in header
    for n in (40, 72):
        assert f"michi_brand_{n}[{n}u * {n}u]" in header


def test_font_depth_and_kconfig():
    font_data = (UI / "assets/fonts/michi_ui_fonts_data.h").read_text()
    defaults = (ROOT / "firmware/sdkconfig.defaults").read_text()
    assert "MICHI_UI_SM_RASTER_BITS  2" in font_data or re.search(r"MICHI_UI_SM_RASTER_BITS\s+2", font_data)
    assert "CONFIG_MICHI_DISPLAY_RASTER_2BIT=y" in defaults


def test_field_mode_is_opt_in_and_signed_tx_not_remote_acceptance():
    kconfig = (UI / "Kconfig").read_text()
    assert "MICHI_DISPLAY_DIAGNOSTICS_ON_BOOT" in kconfig
    assert "default n" in kconfig
    screens = (UI / "michi_ui_screens.c").read_text()
    assert "TX firmados:" in screens
    assert "ctx->announces_sent_session" in screens
    assert "Sin negociar" in screens
