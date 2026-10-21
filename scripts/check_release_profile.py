#!/usr/bin/env python3
"""Source-only checks for ESP32-S3 production-vs-lab configuration.

No build, flash, network, or hardware interaction.  The production gate is
intentionally stricter than the compatible lab profile.  Verify the merged
ESP-IDF sdkconfig after local compilation separately.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import sys

REQUIRED_COMMON = {
    'CONFIG_IDF_TARGET': '"esp32s3"',
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y',
    'CONFIG_LWIP_IP4_REASSEMBLY': 'y',
    'CONFIG_MICHI_SKU_EXPECTS_AUDIO': 'y',
}
REQUIRED_PRODUCTION = {
    'CONFIG_MICHI_WIFI_REQUIRE_WPA2': 'y',
    'CONFIG_MICHI_WIFI_ALLOW_BLE_PROVISIONING_LAB': 'n',
}


def parse_config(path: Path) -> dict[str,str]:
    if not path.is_file():
        raise ValueError(f'missing configuration: {path}')
    values = {}
    for number, raw in enumerate(path.read_text(encoding='utf-8').splitlines(),1):
        line = raw.strip()
        if not line or line.startswith('#'):
            # IDF saves disabled booleans as '# CONFIG_X is not set'.
            disabled = re.fullmatch(r'# (CONFIG_[A-Z0-9_]+) is not set',line)
            if disabled: values[disabled.group(1)] = 'n'
            continue
        if '=' not in line:
            raise ValueError(f'bad configuration line {path}:{number}')
        key,value=line.split('=',1)
        if not re.fullmatch(r'CONFIG_[A-Z0-9_]+',key):
            raise ValueError(f'invalid symbol {key!r} at {path}:{number}')
        if key in values and values[key] != value:
            raise ValueError(f'conflicting value for {key} in {path}')
        values[key]=value
    return values


def check(root: Path, *, production: bool) -> dict:
    root = root.resolve(strict=True)
    base = parse_config(root/'firmware'/'sdkconfig.defaults')
    overrides = parse_config(root/'firmware'/'sdkconfig.production.defaults') if production else {}
    effective = {**base,**overrides}
    expected = dict(REQUIRED_COMMON)
    if production: expected.update(REQUIRED_PRODUCTION)
    violations = [f'{key}: need {value}, got {effective.get(key,"unset")}'
                  for key,value in expected.items() if effective.get(key) != value]
    kconfig = root/'firmware'/'components'/'michi_wifi'/'Kconfig'
    if not kconfig.is_file() or 'config MICHI_WIFI_REQUIRE_WPA2' not in kconfig.read_text(encoding='utf-8'):
        violations.append('Wi-Fi Kconfig lacks MICHI_WIFI_REQUIRE_WPA2')
    return {'profile':'production' if production else 'lab','passed_static_gate':not violations,
            'tests_executed':False,'violations':violations}


def main(argv=None)->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--profile',choices=('lab','production'),default='production')
    args=ap.parse_args(argv)
    try: report=check(args.root,production=args.profile=='production')
    except (ValueError,OSError) as exc:
        print(f'RELEASE PROFILE REFUSED: {exc}',file=sys.stderr);return 2
    print(json.dumps(report,indent=2,sort_keys=True))
    return 0 if report['passed_static_gate'] else 1

if __name__=='__main__':
    raise SystemExit(main())
