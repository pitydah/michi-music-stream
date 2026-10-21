"""Tests for source-only release-policy checker; for local execution later."""
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
import check_release_profile as checker


def tree(tmp_path:Path)->Path:
    root=tmp_path/'michi-music-stream'
    (root/'firmware'/'components'/'michi_wifi').mkdir(parents=True)
    (root/'firmware'/'sdkconfig.defaults').write_text('''CONFIG_IDF_TARGET="esp32s3"\nCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\nCONFIG_LWIP_IP4_REASSEMBLY=y\nCONFIG_MICHI_SKU_EXPECTS_AUDIO=y\n''')
    (root/'firmware'/'sdkconfig.production.defaults').write_text('''CONFIG_MICHI_WIFI_REQUIRE_WPA2=y\nCONFIG_MICHI_WIFI_ALLOW_BLE_PROVISIONING_LAB=n\n''')
    (root/'firmware'/'components'/'michi_wifi'/'Kconfig').write_text('config MICHI_WIFI_REQUIRE_WPA2\n')
    return root


def test_production_policy(tmp_path):
    assert checker.check(tree(tmp_path),production=True)['passed_static_gate']


def test_fail_closed_when_wpa_disabled(tmp_path):
    root=tree(tmp_path)
    (root/'firmware'/'sdkconfig.production.defaults').write_text('CONFIG_MICHI_WIFI_REQUIRE_WPA2=n\n')
    report=checker.check(root,production=True)
    assert not report['passed_static_gate'] and any('WPA2' in x for x in report['violations'])


def test_lab_compatible_with_open_ap(tmp_path):
    assert checker.check(tree(tmp_path),production=False)['passed_static_gate']


def test_duplicate_conflicting_config_rejected(tmp_path):
    root=tree(tmp_path)
    p=root/'firmware'/'sdkconfig.defaults'
    with p.open('a') as handle:handle.write('CONFIG_MICHI_SKU_EXPECTS_AUDIO=n\n')
    try:checker.check(root,production=True)
    except ValueError as err: assert 'conflicting' in str(err)
    else:raise AssertionError('conflicting configuration was accepted')
