"""Shared fixtures for the MS-09 E2E interoperability suite."""

import pytest

from harness import BundleSchemas, SimRuntime


@pytest.fixture(scope="module")
def bundle():
    return BundleSchemas()


@pytest.fixture(scope="module")
def sim():
    runtime = SimRuntime()
    yield runtime
    runtime.stop()
